#ifndef REDIS_IOURING_DISABLE
#include <pthread.h>
#include <error.h>
#include "atomicvar.h"
#include "redisassert.h"
#include "zmalloc.h"
#include "fdp_module.h"
#include "fdp_nvme.h"

struct _fdpModule{
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
     * There is also a way to use fdpModuleIOWait(), but you can't use the sliding window 
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
    atomic_int stop_flag;
};

/* solesie: check whether user change buf member of not */
static inline int isAligned(fdpNvme *fn, void *buf, size_t len, uint64_t offt){
    size_t lbs = fdpNvmeGetLbSize(fn);
    if(offt % lbs != 0 || len % lbs != 0 || (uintptr_t)buf % lbs != 0 ){
        return 0;
    }
    return 1;
}

static void *handling(void *arg){
    fdpModule *fm = (fdpModule *)arg;
    ioUringOp **out_completed;
    /* solesie: CQ handling start */
    while(!atomic_load_explicit(&fm->stop_flag, memory_order_acquire)){
        size_t completed_len = ioUringPollCQ(fm->uring, &out_completed);
        for(size_t i = 0; i < completed_len; ++i){
            ioUringOp *op = out_completed[i];
            ssize_t res = ioUringOpGetResult(op);
            if(res != 0){
                if(ioUringOpGetResubmitted(op) < fm->resubmit_limit){
                    while(!ioUringSubmitOp(fm->uring, op)){};
                    ioUringOpIncreaseResubmitted(op);
                } else{
                    ioUringOpRelease(&op);
                    errno = res;
                    fm->cur_io_res = res;
                    fm->cur_completed_cnt++;
                }
            } else{
                void *arg = ioUringOpGetUserDefinedData(op);
                fm->success_cb(arg);
                ioUringOpRelease(&op);
                fm->cur_completed_cnt++;
                fm->cur_success_cnt++;
            }
        }

        int wait_called = atomic_load_explicit(&fm->wait_called, memory_order_acquire);
        if(wait_called && fm->cur_completed_cnt == fm->cur_submitted_cnt){
            if(fm->cur_success_cnt == fm->cur_submitted_cnt){
                fm->cur_io_res = 1;
            } else{
                assert(fm->cur_io_res < 0);
            }
            atomic_store_explicit(&fm->wait_called, 0, memory_order_release);

            /* solesie: IO thread(i.e., main thread) will be waked up. */
            atomic_store_explicit(&fm->wait_completed, 1, memory_order_release);
        }
    }

    return NULL;
}

/*
 * solesie: Creates FDP(Flexible Data Placement) module object.
 * 
 * @note For multithreaded applications, it is necessary to have 1 fdpModule per thread.
 * @note For child process, it is necessary to create module in child process.
 */
fdpModule *fdpModuleCreate(
    fdpNvme *fn, 
    uint32_t resubmit_limit, 
    uint32_t qdepth, 
    void (*success_cb)(void *arg)){
    
    fdpModule *fm = zcalloc(sizeof(*fm));
    
    fm->uring = ioUringCreate(1, qdepth);
    fm->success_cb = success_cb;
    fm->resubmit_limit = resubmit_limit;

    fm->fdp_nvme = fn;

    fm->max_io_size = fdpNvmeGetMaxIOSize(fn);

    int res = pthread_create(&fm->tid, NULL, handling, fm);
    assert(res == 0);

    return fm;
}

void fdpModuleRelease(fdpModule *fm){
    atomic_store_explicit(&fm->stop_flag, 1, memory_order_release);
    pthread_join(fm->tid, NULL);

    ioUringRelease(&fm->uring);
    zfree(fm);
}

void fdpModuleIORead(
    fdpModule *fm, 
    void *aligned_buf, 
    size_t aligned_len,
    uint64_t aligned_offt,
    void *success_cb_arg){
    
    assert(isAligned(fm->fdp_nvme, aligned_buf, aligned_len, aligned_offt));
    assert(aligned_len <= fm->max_io_size);

    fm->cur_submitted_cnt++;
        
    ioUringOp *op = ioUringOpCreate(fm->uring);
    ioUringOpSetUserDefinedData(op, success_cb_arg);
    struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

    fdpNvmePrepReadUringCmdSqe(fm->fdp_nvme, sqe, aligned_buf, aligned_len, aligned_offt);
    while(!ioUringSubmitOp(fm->uring, op)){};

    return;
}

void fdpModuleIOWrite(
    fdpModule *fm, 
    void *aligned_buf, 
    size_t aligned_len, 
    uint64_t aligned_offt,
    int placement_handle,
    int reclaim_group,
    void *success_cb_arg){
    
    assert(isAligned(fm->fdp_nvme, aligned_buf, aligned_len, aligned_offt));
    assert(aligned_len <= fm->max_io_size);

    int pid = placement_handle;
    if (pid < 0 || pid >= fdpNvmeGetMaxPIDLength(fm->fdp_nvme)) {
        pid = -1;
    }

    fm->cur_submitted_cnt++;

    ioUringOp *op = ioUringOpCreate(fm->uring);
    ioUringOpSetUserDefinedData(op, success_cb_arg);
    struct io_uring_sqe *sqe = ioUringOpGetSqe(op);

    fdpNvmePrepWriteUringCmdSqe(fm->fdp_nvme, sqe, aligned_buf, aligned_len, aligned_offt, pid, reclaim_group);
    while(!ioUringSubmitOp(fm->uring, op)){};

    return;
}

int fdpModuleIOWait(fdpModule *fm){
    atomic_store_explicit(&fm->wait_called, 1, memory_order_release);

    /* solesie: spin lock */
    int wait_completed = 0;
    while(1){
        wait_completed = atomic_load_explicit(&fm->wait_completed, memory_order_acquire);
        if(wait_completed == 1){
            break;
        }
    }
    atomic_store_explicit(&fm->wait_completed, 0, memory_order_release);
    
    int ret = fm->cur_io_res;

    fm->cur_submitted_cnt = 0;
    fm->cur_completed_cnt = 0;
    fm->cur_success_cnt = 0;
    fm->cur_io_res = 0;

    return ret;
}

#endif