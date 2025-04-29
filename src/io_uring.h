#ifndef __REDIS_IO_URING_H
#define __REDIS_IO_URING_H

#ifndef REDIS_IOURING_DISABLE
#include "liburing.h"

/* solesie: TOTALLY THREAD UNSAFE */
typedef struct _IoUring IOUring;

typedef struct _IOUringOp IOUringOp;

typedef enum {
    CQ_SYNC,
    /* solesie: ASYNC means POLLING and EVENT-DRIVEN */ 
    CQ_ASYNC
} CQHandlingMode;

IOUring *ioUringCreate(int is_fdp, CQHandlingMode cqHandlingMode, uint32_t qDepth);
void ioUringRelease(IOUring **ioUring);
void ioUringSubmitOp(IOUring* ioUring, IOUringOp *op);
size_t ioUringWaitOps(IOUring *ioUring, size_t minRequests, IOUringOp ***outCompleted);
size_t ioUringPollCQ(IOUring *ioUring, IOUringOp ***outCompleted);

IOUringOp *ioUringOpCreate(IOUring *ioUring);
IOUringOp *ioUringOpRelease(IOUringOp **op);
struct io_uring_sqe *ioUringOpGetSqe(IOUringOp *op);
ssize_t ioUringOpGetResult(IOUringOp *op);

#endif
#endif