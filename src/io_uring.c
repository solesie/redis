#ifndef REDIS_IOURING_DISABLE

#include <stdlib.h>
#include <string.h>

#include "io_uring.h"
#include "zmalloc.h"
#include "server.h"
#include "fdp_nvme.h"
#include "atomicvar.h"

struct _IoUring{
    struct io_uring ioRing;
	struct io_uring_params params;

	uint32_t qDepth;

	IOUringOpOptions options;

	size_t pending;
	/* solesie: If IOUring handles CQ as ASYNC, poolFd means CQ fd.
	 * Otherwise(i.e., CQ on SYNC_BASED), pollFd is -1. */
	int pollFd;

	CompletedOpsPool *completedOpsPool;
};


// http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
static uint32_t roundUpToNextPowerOfTwo(uint32_t num) {
    if (num == 0) {
        return 0;
    }
    num--;
    num |= num >> 1;
    num |= num >> 2;
    num |= num >> 4;
    num |= num >> 8;
    num |= num >> 16;
    return num + 1;
}

static inline int areOptionsEqual(IOUringOpOptions *opt1, IOUringOpOptions *opt2){
	if(opt1->isSqe128 == opt2->isSqe128 && opt1->isCqe32 == opt2->isCqe32){
		return 1;
	}
	return 0;
}

static inline size_t getSqeSize(IOUringOpOptions *opt){
	return opt->isSqe128 ? 128 :  sizeof(struct io_uring_sqe);
}

static inline size_t getCqeSize(IOUringOpOptions *opt) {
	return opt->isCqe32 ? 32 : sizeof(struct io_uring_cqe);
}

static inline void cOpsPoolReserve(CompletedOpsPool *pool, size_t capacity){
	if(capacity > pool->_capacity){
		IOUringOp **newArr = (IOUringOp**)zrealloc(pool->_arr, sizeof(IOUringOp*) * capacity);
		pool->_arr = newArr;
		pool->_capacity = capacity;
	}
}

static inline void cOpsPushBack(CompletedOpsPool *pool, const IOUringOp *op) {
    if (pool->_length >= pool->_capacity) {
        size_t newCapacity = pool->_capacity ? pool->_capacity * 2 : 1;
        cOpsPoolReserve(pool, newCapacity);
    }
	pool->_arr[pool->_length++] = op;
}

static inline void cOpsClear(CompletedOpsPool *pool) {
    for(int i = 0; i < pool->_length; ++i){
        ioUringOpFree(&pool->_arr[i]);
    }
    pool->_length = 0;
}

static void doWait(
	IOUring *ioUring,
    size_t minRequests,
    size_t maxRequests) {

	cOpsClear(ioUring->completedOpsPool);

	size_t count = 0;
	while (count < maxRequests) {
		struct io_uring_cqe* cqe = NULL;
		if (!io_uring_peek_cqe(&ioUring->ioRing, &cqe) && cqe) {
			count++;
			IOUringOp *op = (IOUringOp*)io_uring_cqe_get_data(cqe);
			if(unlikely(op == NULL)){
				serverLog(LOG_ERR, "solesie: doWait() failed");
				exit(1);
			}
			
			memcpy(&op->cqe_, cqe, getCqeSize(&op->options));
			io_uring_cqe_seen(&ioUring->ioRing, cqe);
			--ioUring->pending;
			
			op->result = cqe->res;
			cOpsPushBack(ioUring->completedOpsPool, op);
		} else {
			if (count < minRequests) {
				io_uring_wait_cqe(&ioUring->ioRing, &cqe);
			} else {
				break;
			}
		}
	}
}

/* solesie: Create a Linux io_uring wrapper object that can be used for general purposes.
 * @param is_fdp Indicates whether the io_uring will be created for NVMe Flexible Data Placement.
 * @param cqHandlingMode Specifies how the user will handle the io_uring completion queue.
 * @param qDepth Represents the size of the io_uring submission/completion queue. 
 * @param pool Must not be NULL. */
IOUring *ioUringCreate(int is_fdp, CQHandlingMode cqHandlingMode, uint32_t qDepth, CompletedOpsPool *pool){
    IOUring *ioUring = (IOUring*)zcalloc(sizeof(*ioUring));

	ioUring->qDepth = qDepth;

    if(is_fdp){
		/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
        ioUring->params.flags |= IORING_SETUP_SQE128;
        ioUring->params.flags |= IORING_SETUP_CQE32;
		ioUring->options.isSqe128 = 1;
		ioUring->options.isCqe32 = 1;
    }

    ioUring->params.flags |= IORING_SETUP_CQSIZE;
    ioUring->params.cq_entries = roundUpToNextPowerOfTwo(qDepth);

    int rc = io_uring_queue_init_params(
        roundUpToNextPowerOfTwo(qDepth), &ioUring->ioRing, &ioUring->params);
    if(rc < 0){
        serverLog(LOG_ERR, "solesie: io_uring_queue_init error");
        exit(1);
    }

	ioUring->pollFd = -1;
	if(cqHandlingMode == CQ_ASYNC){
		ioUring->pollFd = ioUring->ioRing.ring_fd;
	}
	
	ioUring->completedOpsPool = pool;
	cOpsPoolReserve(ioUring->completedOpsPool, qDepth);

    return ioUring;
}

void ioUringRelease(IOUring **ioUring){
	if(*ioUring == NULL){
		return;
	}
	io_uring_queue_exit(&(*ioUring)->ioRing);
	cOpsClear((*ioUring)->completedOpsPool);

    zfree(*ioUring);
	*ioUring = NULL;
}

/* solesie: The op is submitted to io_uring. 
 * On success, 1 is returned; 
 * on failure, -1 is returned. */
int ioUringSubmitOp(IOUring *ioUring, IOUringOp *op) {
	if(unlikely(!areOptionsEqual(&ioUring->options, &op->options))){
		serverLog(LOG_ERR, "solesie: IOUring options != IOUringOp options ");
		return -1;
	}

	io_uring_sqe_set_data(&op->sqe_.sqe, op);
	struct io_uring_sqe* sqe = io_uring_get_sqe(&ioUring->ioRing);
	if (unlikely(!sqe)) {
		return -1;
	}
	memcpy(sqe, &op->sqe_.sqe, getSqeSize(&op->options));

	if(unlikely(ioUring->pending >= ioUring->qDepth)){
		serverLog(LOG_WARNING, "solesie: ioUringSubmitOp() too many pending requests");
		return -1;
	}
	++ioUring->pending;

	/* solesie: rc will be 1 */
	int rc = io_uring_submit(&ioUring->ioRing);
	if (rc <= 0) {
		--ioUring->pending;
		if (unlikely(rc < 0)) {
			serverLog(LOG_ERR, "solesie: ioUringSubmitOp() failed");
			exit(1);
		}
	}
	return 1;
}

/* solesie: Waits synchronously until at least minRequests operations are completed.
 * Only to be used in the SYNC Completion Queue mode.
 * 
 * The user can handle completion by using CompletedOpsPool. */
void ioUringClearCOpsPoolAndWaitOp(IOUring *ioUring, size_t minRequests){
	if(unlikely(ioUring->pollFd != -1)){
		serverLog(LOG_ERR, "solesie: ioUringClearCOpsPoolAndWaitOp() only allowed on SYNC object");
		exit(1);
	}
	doWait(ioUring, minRequests, ioUring->pending);
	return;
}

/* solesie: Only to be used in the ASYNC Completion Queue mode.
 * 
 * The user can handle completion by using CompletedOpsPool. */
void ioUringClearCOpsPoolAndPollCompleted(IOUring *ioUring){
	if(unlikely(ioUring->pollFd == -1)){
		serverLog(LOG_ERR, "solesie: ioUringClearCOpsPoolAndPollCompleted() only allowed on ASYNC object");
		exit(1);
	}
	if(io_uring_cq_ready(&ioUring->ioRing) <= 0){
		return; // nothing completed
	}
	doWait(ioUring, 0, ioUring->pending);
}

#endif