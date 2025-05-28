#ifndef REDIS_IOURING_DISABLE
#include "server.h"
#include "redisassert.h"
#include "crc64.h"
#include "fdp_redis_ufs.h"

#ifndef min
#define min(a, b) (a) < (b) ? a : b
#endif

typedef enum{
    READ,
    WRITE
} ioType;
typedef struct _cbArg{
    ioType type;
    void *user_buf;
    size_t user_buf_len;
    void *aligned_buf;
} cbArg;
void fdpUfsSuccessCb(void *arg){
    cbArg *cb_arg = (cbArg*)arg;
    switch (cb_arg->type){
        case READ:{
            memcpy(cb_arg->user_buf, cb_arg->aligned_buf, cb_arg->user_buf_len);
            zfree(cb_arg->aligned_buf);
            break;
        }
        case WRITE:{
            zfree(cb_arg->aligned_buf);
            break;
        }
        default:{
            exit(1);
        }
    }
    zfree(cb_arg);
}

/* solesie: devide LBA space like below.
 * --------------------------------------
 * | aof_base ----- aof_incr -----  rdb | 
 * -------------------------------------- */
void fdpUfsInitManifest(fdpUfs *fdp_ufs){
    fdpNvme *fn = fdp_ufs->fdp_nvme;
    fdp_ufs->manifest.end_lba = fdpNvmeGetEndLba(fn);

    uint64_t chunk = (fdp_ufs->manifest.end_lba - fdpNvmeGetStartLba(fn) + 1) / 3;
    fdp_ufs->manifest.aof_base_start_lba = 1;
    fdp_ufs->manifest.aof_incr_start_lba = fdp_ufs->manifest.aof_base_start_lba + chunk;
    fdp_ufs->manifest.rdb_start_lba = fdp_ufs->manifest.aof_incr_start_lba + chunk;

    /* solesie: allocate FDP placement handle */
    fdp_ufs->manifest.aof_phd = fdpNvmeAllocateFdpHandle(fn);
    fdp_ufs->manifest.rdb_phd = fdpNvmeAllocateFdpHandle(fn);

    fdp_ufs->manifest.lba_shift = fdpNvmeGetLbaShift(fn);

    return;
}

void fdpUfsInit(void){
    if(!server.fdp_enabled){
        return;
    }
    server.fdp_ufs = (fdpUfs*)zcalloc(sizeof(*server.fdp_ufs));

    fdpNvme *fn = fdpNvmeCreate(server.fdp_device_file);
    fdpDevice *fd = fdpDeviceCreate(fn, 3, 8, fdpUfsSuccessCb);
    server.fdp_ufs->fdp_nvme = fn;
    server.fdp_ufs->fdp_device = fd;

    size_t lb_size = fdpNvmeGetLbSize(fn);
    /* solesie: Is lb_size sufficient? */
    server.fdp_ufs->aof_incr_buf = zcalloc_aligned(lb_size, lb_size);
}

void fdpUfsIOWrite(const void *buf, size_t len, int pld, fdpUfsDataType type){
    fdpDevice *fd = server.fdp_ufs->fdp_device;
    fdpNvme *fn = server.fdp_ufs->fdp_nvme;

    size_t lb_size = fdpNvmeGetLbSize(fn);
    uint32_t max_io_size = fdpNvmeGetMaxIOSize(fn);

    size_t remaining_len = len;
    uint8_t *user_buf = (uint8_t *)buf;

    size_t aligned_remaining_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */
    off_t offt = 0; /* solesie: In redis, append-only and no-random-update */
    switch (type){
        case FDP_UFS_MANIFEST: offt = 0; break;
        case FDP_UFS_RDB: offt = server.fdp_ufs->manifest.rdb_start_lba * lb_size; break;
        default: exit(1); break;
    }

    while(aligned_remaining_len > 0){
        size_t user_buf_len = min(max_io_size, remaining_len);
        size_t write_len = min(max_io_size, aligned_remaining_len);
        void *aligned_buf = zcalloc_aligned(lb_size, write_len);
        memcpy(aligned_buf, user_buf, user_buf_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = WRITE;
        arg->user_buf = user_buf;
        arg->user_buf_len = user_buf_len;
        
        fdpDeviceIOWrite(fd, aligned_buf, write_len, offt, pld, arg);

        remaining_len -= arg->user_buf_len;
        user_buf += write_len;

        aligned_remaining_len -= write_len;
        offt += write_len;
    }
}

void fdpUfsIORead(const void *buf, size_t len, fdpUfsDataType type){
    fdpDevice *fd = server.fdp_ufs->fdp_device;
    fdpNvme *fn = server.fdp_ufs->fdp_nvme;

    size_t lb_size = fdpNvmeGetLbSize(fn);
    uint32_t max_io_size = fdpNvmeGetMaxIOSize(fn);

    size_t remaining_len = len;
    uint8_t *user_buf = (uint8_t *)buf;

    size_t aligned_remaining_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */
    off_t offt = 0; /* append only */
    switch (type){
        case FDP_UFS_MANIFEST: offt = 0; break;
        case FDP_UFS_RDB: offt = server.fdp_ufs->manifest.rdb_start_lba * lb_size; break;
        default: exit(1); break;
    }

    while(aligned_remaining_len > 0){
        size_t user_buf_len = min(max_io_size, remaining_len);
        size_t read_len = min(max_io_size, aligned_remaining_len);
        void *aligned_buf = zcalloc_aligned(lb_size, read_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = READ;
        arg->user_buf = user_buf;
        arg->user_buf_len = user_buf_len;
        
        fdpDeviceIORead(fd, aligned_buf, read_len, offt, arg);

        remaining_len -= arg->user_buf_len;
        user_buf += read_len;

        aligned_remaining_len -= read_len;
        offt += read_len;
    }
}

int fdpUfsIOWait(fdpUfs *fdp_ufs){
    return fdpDeviceIOWait(fdp_ufs->fdp_device);
}

#endif