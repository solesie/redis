#ifndef REDIS_IOURING_DISABLE
#include <pthread.h>
#include <error.h>
#include "atomicvar.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "fdp_device.h"
#include "fdp_nvme.h"

struct _fdpDevice{
    ioUring *uring;

    /* solesie: As we change File System to Direct I/O, 
     * all written data should be aligned with NVMe LBA size(e.g., 4KiB).

     * Chainging all memory alignment to LBA may result in the need to change 
     * all application logic like ZNS SSD. (For redis, rio.c, rio.h)
     * 
     * So apply the same logic as copy_from/to_user.
     * 
     * Unlike Cachelib + Folly (baton_, C++),
     * memory release should be performed explicitly when handling is completed.
     * 
     * There is also a way to use fdpDeviceIOWait(), but you can't use the sliding window 
     * in situations where write bulk of data without waiting. */

    void (*success_cb)(void *arg);

    fdpNvme *fdp_nvme;

    /* Some devices have this transfer size limit due to DMA size limitations.
     * This limit is applicable for both writes and reads. */
    size_t max_io_size;

    uint32_t resubmit_limit;

    size_t cur_submitted_cnt;
    size_t cur_completed_cnt;
    size_t cur_success_cnt;
    atomic_int wait_called;
    atomic_int wait_completed;
    int cur_io_res;

    pthread_t tid;
};

/* solesie: check whether user change buf member of not */
static inline int isAligned(fdpNvme *fdp_nvme, void *buf, size_t len, off_t offt){
    size_t lbs = fdpNvmeGetLbSize(fdp_nvme);
    if(offt % lbs != 0 || len % lbs != 0 || (uintptr_t)buf % lbs != 0 ){
        return 0;
    }
    return 1;
}

static void *handling(void *arg){
    fdpDevice *fdp_device = (fdpDevice *)arg;
    ioUringOp **out_completed;
    /* solesie: CQ handling start */
    while(1){
        size_t completed_len = ioUringPollCQ(fdp_device->uring, &out_completed);
        for(size_t i = 0; i < completed_len; ++i){
            ioUringOp *op = out_completed[i];
            ssize_t res = ioUringOpGetResult(op);
            if(res != 0){
                if(ioUringOpGetResubmitted(op) < fdp_device->resubmit_limit){
                    while(!ioUringSubmitOp(fdp_device->uring, op)){};
                    ioUringOpIncreaseResubmitted(op);
                } else{
                    ioUringOpRelease(&op);
                    errno = res;
                    fdp_device->cur_io_res = res;
                    fdp_device->cur_completed_cnt++;
                }
            } else{
                void *arg = ioUringOpGetUserDefinedData(op);
                fdp_device->success_cb(arg);
                ioUringOpRelease(&op);
                fdp_device->cur_completed_cnt++;
                fdp_device->cur_success_cnt++;
            }
        }

        int wait_called = atomic_load_explicit(&fdp_device->wait_called, memory_order_acquire);
        if(wait_called && fdp_device->cur_completed_cnt == fdp_device->cur_submitted_cnt){
            if(fdp_device->cur_success_cnt == fdp_device->cur_submitted_cnt){
                fdp_device->cur_io_res = 1;
            } else{
                assert(fdp_device->cur_io_res < 0);
            }
            atomic_store_explicit(&fdp_device->wait_called, 0, memory_order_release);

            /* solesie: IO thread(i.e., main thread) will be waked up. */
            atomic_store_explicit(&fdp_device->wait_completed, 1, memory_order_release);
        }
    }

    return NULL;
}

/*
 * solesie: Creates FDP(Flexible Data Placement) device object.
 * 
 * @note For multithreaded applications, it is necessary to have 1 fdpDevice per thread.
 */
fdpDevice *fdpDeviceCreate(
    fdpNvme *fdp_nvme, 
    uint32_t resubmit_limit, 
    uint32_t qdepth, 
    void (*success_cb)(void *arg)){
    
    fdpDevice *fdp_device = zcalloc(sizeof(*fdp_device));
    
    fdp_device->uring = ioUringCreate(1, qdepth);
    fdp_device->success_cb = success_cb;
    fdp_device->resubmit_limit = resubmit_limit;

    fdp_device->fdp_nvme = fdp_nvme;

    fdp_device->max_io_size = fdpNvmeGetMaxIOSize(fdp_nvme);

    int res = pthread_create(&fdp_device->tid, NULL, handling, fdp_device);
    assert(res == 0);
    pthread_detach(fdp_device->tid);

    return fdp_device;
}

void fdpDeviceRelease(fdpDevice *fdp_device){
    ioUringRelease(&fdp_device->uring);
    fdpNvmeRelease(fdp_device->fdp_nvme);
    zfree(fdp_device);
}

void fdpDeviceIORead(
    fdpDevice *fdp_device, 
    void *aligned_buf, 
    size_t aligned_len,
    off_t aligned_offt,
    void *success_cb_arg){
    
    assert(isAligned(fdp_device->fdp_nvme, aligned_buf, aligned_len, aligned_offt));
    assert(aligned_len <= fdp_device->max_io_size);

    fdp_device->cur_submitted_cnt++;
        
    ioUringOp *op = ioUringOpCreate(fdp_device->uring);
    ioUringOpSetUserDefinedData(op, success_cb_arg);
    struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

    fdpNvmePrepReadUringCmdSqe(fdp_device->fdp_nvme, sqe, aligned_buf, aligned_len, aligned_offt);
    while(!ioUringSubmitOp(fdp_device->uring, op)){};

    return;
}

void fdpDeviceIOWrite(
    fdpDevice *fdp_device, 
    void *aligned_buf, 
    size_t aligned_len, 
    off_t aligned_offt,
    int placement_handle,
    void *success_cb_arg){
    
    assert(isAligned(fdp_device->fdp_nvme, aligned_buf, aligned_len, aligned_offt));
    assert(aligned_len <= fdp_device->max_io_size);

    int pid = placement_handle;
    if (pid < 0 || pid >= fdpNvmeGetMaxPIDLength(fdp_device->fdp_nvme)) {
        pid = -1;
    }

    fdp_device->cur_submitted_cnt++;

    ioUringOp *op = ioUringOpCreate(fdp_device->uring);
    ioUringOpSetUserDefinedData(op, success_cb_arg);
    struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

    fdpNvmePrepWriteUringCmdSqe(fdp_device->fdp_nvme, sqe, aligned_buf, aligned_len, aligned_offt, pid);
    while(!ioUringSubmitOp(fdp_device->uring, op)){};

    return;
}

int fdpDeviceIOWait(fdpDevice *fdp_device){
    atomic_store_explicit(&fdp_device->wait_called, 1, memory_order_release);

    /* solesie: spin lock */
    int wait_completed = 0;
    while(1){
        wait_completed = atomic_load_explicit(&fdp_device->wait_completed, memory_order_acquire);
        if(wait_completed == 1){
            break;
        }
    }
    atomic_store_explicit(&fdp_device->wait_completed, 0, memory_order_release);
    
    int ret = fdp_device->cur_io_res;

    fdp_device->cur_submitted_cnt = 0;
    fdp_device->cur_completed_cnt = 0;
    fdp_device->cur_success_cnt = 0;
    fdp_device->cur_io_res = 0;

    return ret;
}

#endif