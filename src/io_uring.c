#ifndef REDIS_IOURING_DISABLE

#include <string.h>

#include "atomicvar.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "io_uring.h"
#include "fdp_nvme.h"

/* solesie: The vector is defined for completed IOUringOp*. */
typedef struct _completedIOUringOpsVector{
	ioUringOp **arr;
	size_t len;
    size_t cap;
} completedIOUringOpsVector;

/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
typedef struct _ioUringOpOptions{
    int is_sqe_128;
    int is_cqe_32;
} ioUringOpOptions;

struct _ioUring{
    struct io_uring io_ring;
	struct io_uring_params params;

	uint32_t qdepth;

	ioUringOpOptions options;

	atomic_size_t pending;

	/* solesie: If ioUring handles CQ as ASYNC, vecFd means CQ fd.
	 * Otherwise(i.e., CQ on SYNC_BASED), poll_fd is -1. */
	int poll_fd;

	completedIOUringOpsVector *vec;
};

/* solesie: The user submits IOUringOp* commands to Linux io_uring, 
 * and receives the same IOUringOp* upon completion.
 *
 * The user must allocate and free IOUringOp memory 
 * using ioUringOpCreate() and ioUringOpRelease(). */
struct _ioUringOp{
    /* solesie: the number of bytes on Complete, and < 0 on failure. */
    ssize_t result;

    void *user_defined_data;

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

    ioUringOpOptions options;

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

static inline int areOptionsEqual(ioUringOpOptions *opt1, ioUringOpOptions *opt2){
	if(opt1->is_sqe_128 == opt2->is_sqe_128 && opt1->is_cqe_32 == opt2->is_cqe_32){
		return 1;
	}
	return 0;
}

static inline size_t getSqeSize(ioUringOpOptions *opt){
	return opt->is_sqe_128 ? 128 :  sizeof(struct io_uring_sqe);
}
static inline size_t getCqeSize(ioUringOpOptions *opt) {
	return opt->is_cqe_32 ? 32 : sizeof(struct io_uring_cqe);
}

static inline completedIOUringOpsVector *vectorCreate(void){
	completedIOUringOpsVector *vec = (completedIOUringOpsVector*)zcalloc(sizeof(*vec));
	return vec;
}
static inline void vectorRelease(completedIOUringOpsVector **vec) {
    for(size_t i = 0; i < (*vec)->len; ++i){
        ioUringOpRelease(&(*vec)->arr[i]);
    }
    zfree((*vec)->arr);
	zfree(*vec);
    *vec = NULL;
}
static inline void vectorReserve(completedIOUringOpsVector *vec, size_t capacity){
	if(capacity > vec->cap){
		ioUringOp **newArr = (ioUringOp**)zrealloc(vec->arr, sizeof(ioUringOp*) * capacity);
		vec->arr = newArr;
		vec->cap = capacity;
	}
}
static inline void vectorPushBack(completedIOUringOpsVector *vec, ioUringOp *completed_op) {
    if (vec->len >= vec->cap) {
        size_t newCapacity = vec->cap ? vec->cap * 2 : 1;
        vectorReserve(vec, newCapacity);
    }
	vec->arr[vec->len++] = completed_op;
}
static inline void vectorClear(completedIOUringOpsVector *vec) {
    vec->len = 0;
}

/* On success, return 1.
 * On failure, return 0. */
static int doWait(
	ioUring *uring,
    size_t min_requests,
    size_t max_requests) {

	vectorClear(uring->vec);

	size_t count = 0;
	while (count < max_requests) {
		struct io_uring_cqe* cqe = NULL;
		if (!io_uring_peek_cqe(&uring->io_ring, &cqe) && cqe) {
			count++;
			ioUringOp *op = (ioUringOp*)io_uring_cqe_get_data(cqe);
			if(op == NULL){
				return 0;
			}
			
			memcpy(&op->cqe_, cqe, getCqeSize(&op->options));
			io_uring_cqe_seen(&uring->io_ring, cqe);
			atomic_fetch_sub_explicit(&uring->pending, 1, memory_order_acq_rel);
			
			op->result = cqe->res;
			vectorPushBack(uring->vec, op);
		} else {
			if (count < min_requests) {
				io_uring_wait_cqe(&uring->io_ring, &cqe);
			} else {
				break;
			}
		}
	}
	return 1;
}

/* solesie: Create a Linux io_uring wrapper object that can be used for general purposes.
 * @param is_fdp Indicates whether the io_uring will be created for NVMe Flexible Data Placement.
 * @param qdepth Represents the size of the io_uring submission/completion queue. */
ioUring *ioUringCreate(int is_fdp, uint32_t qdepth){
    ioUring *uring = (ioUring*)zcalloc(sizeof(*uring));

	uring->qdepth = qdepth;

    if(is_fdp){
		/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
        uring->params.flags |= IORING_SETUP_SQE128;
        uring->params.flags |= IORING_SETUP_CQE32;
		uring->options.is_sqe_128 = 1;
		uring->options.is_cqe_32 = 1;
    }

    uring->params.flags |= IORING_SETUP_CQSIZE;
    uring->params.cq_entries = roundUpToNextPowerOfTwo(qdepth);

    int rc = io_uring_queue_init_params(
        roundUpToNextPowerOfTwo(qdepth), &uring->io_ring, &uring->params);
	assert(rc >= 0);

	uring->poll_fd = uring->io_ring.ring_fd;
	
	uring->vec = vectorCreate();
	vectorReserve(uring->vec, qdepth);

    return uring;
}

void ioUringRelease(ioUring **uring){
	if(*uring == NULL){
		return;
	}
	io_uring_queue_exit(&(*uring)->io_ring);
	vectorClear((*uring)->vec);
	vectorRelease(&(*uring)->vec);

    zfree(*uring);
	*uring = NULL;
}

/* solesie: The op is submitted to io_uring. 
 * @return submitted request(i.e., on fail due to busy, return 0). */
int ioUringSubmitOp(ioUring *uring, ioUringOp *op) {
	size_t pending = atomic_load_explicit(&uring->pending, memory_order_acquire);
	if(pending >= uring->qdepth){
		return 0;
	}
	assert(areOptionsEqual(&uring->options, &op->options));

	io_uring_sqe_set_data(&op->sqe_.sqe, op);
	struct io_uring_sqe* sqe = io_uring_get_sqe(&uring->io_ring);
	assert(sqe);
	memcpy(sqe, &op->sqe_.sqe, getSqeSize(&op->options));

	/* solesie: rc will be 1 */
	int rc = io_uring_submit(&uring->io_ring);
	if (rc <= 0) {
		assert(rc == 0);
		return 0;
	}
	atomic_fetch_add_explicit(&uring->pending, 1, memory_order_acq_rel);
	return 1;
}

/* solesie: Polls for completed ioUring operations in ASYNC Completion Queue mode.
 * (Only to be used in the ASYNC Completion Queue mode)
 *
 * @param uring       	 The ioUring instance (must be ASYNC mode).
 * @param out_completed  Output: on return, *out_completed points to an array
 *                      of IOUringOp* entries that have completed.
 * 
 * @return The number of completed requests (length of *out_completed)
 * 
 * @note While the user may be responsible for freeing the memory of an IOUringOp* created via IOUringOpCreate(), 
 * manipulation of the internal array(i.e., IOUringOp**) is prohibited. */
size_t ioUringPollCQ(ioUring *uring, ioUringOp ***out_completed){
	/* ioUringPollCQ() only allowed on ASYNC object */
	assert(uring->poll_fd != -1);
	
	if(io_uring_cq_ready(&uring->io_ring) <= 0){
		/* nothing completed */
		return 0;
	}

	int flag = doWait(uring, 0, atomic_load_explicit(&uring->pending, memory_order_seq_cst));
	assert(flag);
	*out_completed = uring->vec->arr;
	return uring->vec->len;
}

ioUringOp *ioUringOpCreate(ioUring *uring){
    ioUringOp *ret = zcalloc(sizeof(ioUringOp));
    ret->options = uring->options;
    return ret;
}

void ioUringOpRelease(ioUringOp **op){
    if(*op == NULL){
        return;
    }
    zfree(*op);
    *op = NULL;
}

struct io_uring_sqe *ioUringOpGetSqe(ioUringOp *op){
	return &op->sqe_.sqe;
}

ssize_t ioUringOpGetResult(ioUringOp *op){
	return op->result;
}

void *ioUringOpGetUserDefinedData(ioUringOp *op){
	return op->user_defined_data;
}
void ioUringOpSetUserDefinedData(ioUringOp *op, void *user_defined_data){
	op->user_defined_data = user_defined_data;
}

uint32_t ioUringOpGetResubmitted(ioUringOp *op){
	return op->resubmitted;
}

void ioUringOpIncreaseResubmitted(ioUringOp *op){
	op->resubmitted++;
}

#endif