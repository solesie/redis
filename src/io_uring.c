#ifndef REDIS_IOURING_DISABLE

#include <string.h>

#include "atomicvar.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "io_uring.h"
#include "fdp_nvme.h"

/* solesie: The vector is defined for completed IOUringOp*. */
typedef struct _CompletedIOUringOpsVector{
	IOUringOp **arr;
	size_t len;
    size_t cap;
} CompletedIOUringOpsVector;

/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
typedef struct _IOUringOpOptions{
    int isSqe128;
    int isCqe32;
} IOUringOpOptions;

struct _IoUring{
    struct io_uring ioRing;
	struct io_uring_params params;

	uint32_t qDepth;

	IOUringOpOptions options;

	atomic_size_t pending;

	/* solesie: If IOUring handles CQ as ASYNC, vecFd means CQ fd.
	 * Otherwise(i.e., CQ on SYNC_BASED), pollFd is -1. */
	int pollFd;

	CompletedIOUringOpsVector *vec;
};

/* solesie: The user submits IOUringOp* commands to Linux io_uring, 
 * and receives the same IOUringOp* upon completion.
 *
 * The user must allocate and free IOUringOp memory 
 * using ioUringOpCreate() and ioUringOpRelease(). */
struct _IOUringOp{
    /* solesie: the number of bytes on Complete, and < 0 on failure. */
    ssize_t result;

    void *userDefinedData;

    /* we use unions with the largest size to avoid
     * indidual allocations for the sqe/cqe */
    union {
        struct io_uring_sqe sqe;
        uint8_t data[128];
    } sqe_;
    
    /* we have to use a union here because of -Wgnu-variable-sized-type-not-at-end
     * __u64 big_cqe[]; */
    union {
        __u64 user_data; /* first member from from io_uring_cqe */
        uint8_t data[32];
    } cqe_;

    IOUringOpOptions options;

	uint32_t resubmitted;
};

/* http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2 */
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

static inline CompletedIOUringOpsVector *vectorCreate(void){
	CompletedIOUringOpsVector *vec = (CompletedIOUringOpsVector*)zcalloc(sizeof(*vec));
	return vec;
}
static inline void vectorRelease(CompletedIOUringOpsVector **vec) {
    for(size_t i = 0; i < (*vec)->len; ++i){
        ioUringOpRelease(&(*vec)->arr[i]);
    }
    zfree((*vec)->arr);
	zfree(*vec);
    *vec = NULL;
}
static inline void vectorReserve(CompletedIOUringOpsVector *vec, size_t capacity){
	if(capacity > vec->cap){
		IOUringOp **newArr = (IOUringOp**)zrealloc(vec->arr, sizeof(IOUringOp*) * capacity);
		vec->arr = newArr;
		vec->cap = capacity;
	}
}
static inline void vectorPushBack(CompletedIOUringOpsVector *vec, IOUringOp *completedOp) {
    if (vec->len >= vec->cap) {
        size_t newCapacity = vec->cap ? vec->cap * 2 : 1;
        vectorReserve(vec, newCapacity);
    }
	vec->arr[vec->len++] = completedOp;
}
static inline void vectorClear(CompletedIOUringOpsVector *vec) {
    vec->len = 0;
}

/* On success, return 1.
 * On failure, return 0. */
static int doWait(
	IOUring *ioUring,
    size_t minRequests,
    size_t maxRequests) {

	vectorClear(ioUring->vec);

	size_t count = 0;
	while (count < maxRequests) {
		struct io_uring_cqe* cqe = NULL;
		if (!io_uring_peek_cqe(&ioUring->ioRing, &cqe) && cqe) {
			count++;
			IOUringOp *op = (IOUringOp*)io_uring_cqe_get_data(cqe);
			if(op == NULL){
				return 0;
			}
			
			memcpy(&op->cqe_, cqe, getCqeSize(&op->options));
			io_uring_cqe_seen(&ioUring->ioRing, cqe);
			atomic_fetch_sub_explicit(&ioUring->pending, 1, memory_order_acq_rel);
			
			op->result = cqe->res;
			vectorPushBack(ioUring->vec, op);
		} else {
			if (count < minRequests) {
				io_uring_wait_cqe(&ioUring->ioRing, &cqe);
			} else {
				break;
			}
		}
	}
	return 1;
}

/* solesie: Create a Linux io_uring wrapper object that can be used for general purposes.
 * @param is_fdp Indicates whether the io_uring will be created for NVMe Flexible Data Placement.
 * @param qDepth Represents the size of the io_uring submission/completion queue. */
IOUring *ioUringCreate(int is_fdp, uint32_t qDepth){
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
	assert(rc >= 0);

	ioUring->pollFd = ioUring->ioRing.ring_fd;
	
	ioUring->vec = vectorCreate();
	vectorReserve(ioUring->vec, qDepth);

    return ioUring;
}

void ioUringRelease(IOUring **ioUring){
	if(*ioUring == NULL){
		return;
	}
	io_uring_queue_exit(&(*ioUring)->ioRing);
	vectorClear((*ioUring)->vec);
	vectorRelease(&(*ioUring)->vec);

    zfree(*ioUring);
	*ioUring = NULL;
}

/* solesie: The op is submitted to io_uring. 
 * @return submitted request(i.e., on fail due to busy, return 0). */
int ioUringSubmitOp(IOUring *ioUring, IOUringOp *op) {
	size_t pending = atomic_load_explicit(&ioUring->pending, memory_order_acquire);
	if(pending >= ioUring->qDepth){
		return 0;
	}
	assert(areOptionsEqual(&ioUring->options, &op->options));

	io_uring_sqe_set_data(&op->sqe_.sqe, op);
	struct io_uring_sqe* sqe = io_uring_get_sqe(&ioUring->ioRing);
	assert(sqe);
	memcpy(sqe, &op->sqe_.sqe, getSqeSize(&op->options));

	/* solesie: rc will be 1 */
	int rc = io_uring_submit(&ioUring->ioRing);
	if (rc <= 0) {
		assert(rc == 0);
		return 0;
	}
	atomic_fetch_add_explicit(&ioUring->pending, 1, memory_order_acq_rel);
	return 1;
}

// /* solesie: Waits synchronously until at least minRequests operations are completed.
//  * (Only valid when running in the SYNC completion-queue mode, i.e. pollFd == -1)

//  * @param ioUring       The io_uring instance (must be in SYNC mode).
//  * @param minRequests   Minimum number of completions to wait for.
//  * @param outCompleted  Output: on return, *outCompleted points to an array
//  *                      of IOUringOp* entries that have completed.
//  * 
//  * @return The number of completed requests (length of *outCompleted)
//  * 
//  * @note While the user may be responsible for freeing the memory of an IOUringOp* created via IOUringOpCreate(), 
//  * manipulation of the internal array(i.e., IOUringOp**) is prohibited. */
// size_t ioUringWaitOps(IOUring *ioUring, size_t minRequests, IOUringOp ***outCompleted){
// 	/* ioUringPollOps() only allowed on SYNC object */
// 	assert(ioUring->pollFd == -1);

// 	size_t pending = 0;
// 	atomicGet(ioUring->pending, pending);
// 	int flag = doWait(ioUring, minRequests, pending);
// 	assert(flag);
// 	*outCompleted = ioUring->vec->arr;
// 	return ioUring->vec->len;
// }

/* solesie: Polls for completed IOUring operations in ASYNC Completion Queue mode.
 * (Only to be used in the ASYNC Completion Queue mode)
 *
 * @param ioUring       The IOUring instance (must be ASYNC mode).
 * @param outCompleted  Output: on return, *outCompleted points to an array
 *                      of IOUringOp* entries that have completed.
 * 
 * @return The number of completed requests (length of *outCompleted)
 * 
 * @note While the user may be responsible for freeing the memory of an IOUringOp* created via IOUringOpCreate(), 
 * manipulation of the internal array(i.e., IOUringOp**) is prohibited. */
size_t ioUringPollCQ(IOUring *ioUring, IOUringOp ***outCompleted){
	/* ioUringPollCQ() only allowed on ASYNC object */
	assert(ioUring->pollFd != -1);
	
	if(io_uring_cq_ready(&ioUring->ioRing) <= 0){
		/* nothing completed */
		return 0;
	}

	int flag = doWait(ioUring, 0, atomic_load_explicit(&ioUring->pending, memory_order_seq_cst));
	assert(flag);
	*outCompleted = ioUring->vec->arr;
	return ioUring->vec->len;
}

IOUringOp *ioUringOpCreate(IOUring *ioUring){
    IOUringOp *ret = zcalloc(sizeof(IOUringOp));
    ret->options = ioUring->options;
    return ret;
}

void ioUringOpRelease(IOUringOp **op){
    if(*op == NULL){
        return;
    }
    zfree(*op);
    *op = NULL;
}

struct io_uring_sqe *ioUringOpGetSqe(IOUringOp *op){
	return &op->sqe_.sqe;
}

ssize_t ioUringOpGetResult(IOUringOp *op){
	return op->result;
}

void ioUringOpSetUserDefinedData(IOUringOp *op, void *userDefinedData){
	op->userDefinedData = userDefinedData;
}

uint32_t ioUringOpGetResubmitted(IOUringOp *op){
	return op->resubmitted;
}

void ioUringOpIncreaseResubmitted(IOUringOp *op){
	op->resubmitted++;
}

#endif