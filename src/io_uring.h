#ifndef __REDIS_IO_URING_H
#define __REDIS_IO_URING_H

#ifndef REDIS_IOURING_DISABLE
#include "liburing.h"

/* solesie: TOTALLY THREAD UNSAFE */
typedef struct _ioUring ioUring;

typedef struct _ioUringOp ioUringOp;

ioUring *ioUringCreate(int is_fdp, uint32_t qdepth);
void ioUringRelease(ioUring **uring);
int ioUringSubmitOp(ioUring *uring, ioUringOp *op);
size_t ioUringPollCQ(ioUring *uring, ioUringOp ***out_completed);

ioUringOp *ioUringOpCreate(ioUring *uring);
void ioUringOpRelease(ioUringOp **op);
struct io_uring_sqe *ioUringOpGetSqe(ioUringOp *op);
ssize_t ioUringOpGetResult(ioUringOp *op);
void *ioUringOpGetUserDefinedData(ioUringOp *op);
void ioUringOpSetUserDefinedData(ioUringOp *op, void *user_defined_data);
uint32_t ioUringOpGetResubmitted(ioUringOp *op);
void ioUringOpIncreaseResubmitted(ioUringOp *op);

#endif
#endif