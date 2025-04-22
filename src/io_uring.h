#ifndef __REDIS_IO_URING_H
#define __REDIS_IO_URING_H

#ifndef REDIS_IOURING_DISABLE
#include "liburing.h"

/* solesie: TOTALLY THREAD UNSAFE */
typedef struct _IoUring IOUring;

/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
typedef struct _IOUringOpOptions{
    int isSqe128;
    int isCqe32;
} IOUringOpOptions;

/* solesie: The user submits IOUringOp* commands to Linux io_uring, 
 * and receives the same IOUringOp* upon completion.
 *
 * The user must allocate and free IOUringOp memory 
 * using ioUringOpCalloc() and ioUringOpFree().
 * 
 * However, the user may don't need to call ioUringOpFree() directly, 
 * because ioUringClearCOpsPoolAndWaitOp() and ioUringClearCOpsPoolAndPollCompleted() free old IOUringOp. */
typedef struct _IOUringOp{
    /* solesie: the number of bytes on Complete, and < 0 on failure. */
    ssize_t result;

    /* solesie: The user can directly retrieve the submitted IOUringOp from the CompletedOpsPool. */
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
} IOUringOp;
static inline IOUringOp *ioUringOpCalloc(){
    return zcalloc(sizeof(IOUringOp));
}
static inline void ioUringOpFree(IOUringOp **ioUringOp){
    if(*ioUringOp == NULL){
        return;
    }
    zfree(*ioUringOp);
    *ioUringOp = NULL;
}

typedef enum {
    CQ_SYNC,
    /* solesie: ASYNC means POLLING and EVENT-DRIVEN */ 
    CQ_ASYNC
} CQHandlingMode;

/* solesie: The pool is defined for completed IOUringOp*.
 * All members must not be accessed directly. */
typedef struct _CompletedOpsPool{
	IOUringOp **_arr;
	size_t _length;
    size_t _capacity;
} CompletedOpsPool;
static inline CompletedOpsPool *cOpsPoolInit(){
	CompletedOpsPool *pool = (CompletedOpsPool*)zcalloc(sizeof(*pool));
	return pool;
}
static inline void cOpsPoolRelease(CompletedOpsPool **pool) {
    for(int i = 0; i < (*pool)->_length; ++i){
        ioUringOpFree(&(*pool)->_arr[i]);
    }
    zfree((*pool)->_arr);
	zfree(*pool);
    *pool = NULL;
}
static inline size_t cOpsPoolGetLength(CompletedOpsPool *pool){
    return pool->_length;
}
static inline IOUringOp *cOpsPoolGet(CompletedOpsPool *pool, size_t idx){
    if(idx >= pool->_length){
        return NULL;
    }
    return pool->_arr[idx];
}

IOUring *ioUringCreate(int is_fdp, CQHandlingMode cqHandlingMode, uint32_t qDepth, CompletedOpsPool *pool);
void ioUringRelease(IOUring **ioUring);
void ioUringSubmitOp(IOUring* ioUring, IOUringOp *op);
void ioUringClearCOpsPoolAndWaitOp(IOUring *ioUring, size_t minRequests);
void ioUringClearCOpsPoolAndPollCompleted(IOUring *ioUring);

#endif
#endif