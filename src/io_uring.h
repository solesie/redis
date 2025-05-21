#ifndef __REDIS_IO_URING_H
#define __REDIS_IO_URING_H

#ifndef REDIS_IOURING_DISABLE
#include "liburing.h"

/* solesie: TOTALLY THREAD UNSAFE */
typedef struct _IoUring IOUring;

typedef struct _IOUringOp IOUringOp;

IOUring *ioUringCreate(int is_fdp, uint32_t qDepth);
void ioUringRelease(IOUring **ioUring);
int ioUringSubmitOp(IOUring *ioUring, IOUringOp *op);
size_t ioUringPollCQ(IOUring *ioUring, IOUringOp ***outCompleted);

IOUringOp *ioUringOpCreate(IOUring *ioUring);
void ioUringOpRelease(IOUringOp **op);
struct io_uring_sqe *ioUringOpGetSqe(IOUringOp *op);
ssize_t ioUringOpGetResult(IOUringOp *op);
uint32_t ioUringOpGetResubmitted(IOUringOp *op);
void ioUringOpIncreaseResubmitted(IOUringOp *op);

#endif
#endif