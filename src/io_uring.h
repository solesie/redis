#ifndef __REDIS_IO_URING_H
#define __REDIS_IO_URING_H

#ifndef REDIS_IOURING_DISABLE
#include "liburing.h"

typedef struct _IoUring IOUring;

// IO Operation type supported by IOReq
// typedef enum { 
//     INVALID = 0, 
//     READ, 
//     WRITE 
// } OpType;

typedef enum {
    UNINITIALIZED,
    INITIALIZED,
    PENDING,
    COMPLETED,
    CANCELED,
} IOUringOpState;

/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
typedef struct _IOUringOpOptions{
    int isSqe128;
    int isCqe32;
} IOUringOpOptions;

typedef struct _IOUringOp{
    IOUringOpState state;
    ssize_t result;
    void *userData;

    /* we use unions with the largest size to avoid
     * indidual allocations for the sqe/cqe */
    union {
        struct io_uring_sqe sqe;
        uint8_t data[128];
    } sqe_;
    
    /* we have to use a union here because of -Wgnu-variable-sized-type-not-at-end
     * __u64 big_cqe[]; */
    union {
        __u64 user_data; // first member from from io_uring_cqe
        uint8_t data[32];
    } cqe_;

    IOUringOpOptions options;
}IOUringOp;

// typedef struct _IOReq{
//     int fd;                 /* file descripters */
//     OpType opType;
//     uint64_t offset;
//     size_t size;
//     void *data;
//     uint16_t *placementHandle;

//     int is_req_successful;  /* 1 on success, 0 on failure */
// }IOReq;

IOUring *ioUringCreate(int is_fdp, uint32_t qDepth);
void ioUringRelease(IOUring *ioUring);
void submitOne(IOUring* ioUring, IOUringOp *op);

#endif
#endif