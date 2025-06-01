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
 * ----------------------------------------------------
 * | file system | manifest ----- aof_base -----  rdb | 
 * ---------------------------------------------------- */
static void fdpUfsInitManifest(fdpUfs *ufs){
    fdpNvme *fn = ufs->fdp_nvme;

    uint32_t lba_shift = ufs->lba_shift;
    uint64_t chunk = (ufs->device_size / 2) >> lba_shift;
    ufs->manifest.aof_base_start_lba = 1; /* solesie: 1 for manifest */
    ufs->manifest.rdb_start_lba = ufs->manifest.aof_base_start_lba + chunk;

    /* solesie: allocate FDP placement handle */
    ufs->manifest.aof_phd = 0;
    ufs->manifest.rdb_phd = fdpNvmeAllocateFdpHandle(fn);
    ufs->manifest.manifest_phd = fdpNvmeAllocateFdpHandle(fn);

    /* solesie: init offset */
    ufs->manifest.aof_base_cur_offt = ufs->manifest.aof_base_cur_offt_aligned
        = ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
        = ufs->manifest.aof_base_start_lba << lba_shift;
    ufs->manifest.rdb_cur_offt = ufs->manifest.rdb_cur_offt_aligned
        = ufs->rdb_rofft = ufs->rdb_rofft_aligned
        = ufs->manifest.rdb_start_lba << lba_shift;

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
    server.fdp_ufs->device_size = fdpNvmeGetDeviceSize(fn);
    server.fdp_ufs->lba_shift = fdpNvmeGetLbaShift(fn);

    uint32_t ios = fdpNvmeGetMaxIOSize(fn);
    server.fdp_ufs->device_default_io_size = ios;
    server.fdp_ufs->aof_base_wbuf = zcalloc(ios);
    server.fdp_ufs->rdb_wbuf = zcalloc(ios);
    server.fdp_ufs->aof_base_wbuf_size = ios;
    server.fdp_ufs->rdb_wbuf_size = ios;

    fdpUfsInitManifest(server.fdp_ufs);
}

void fdpUfsResetData(fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lba_shift = ufs->lba_shift;
    uint32_t lb_size = fdpNvmeGetLbSize(ufs->fdp_nvme);
    switch (type){
        case FDP_UFS_AOF_BASE: {
            uint64_t start_offt = ufs->manifest.aof_base_start_lba << lba_shift;
            ufs->manifest.aof_base_cur_offt = ufs->manifest.aof_base_cur_offt_aligned
                = ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
                = start_offt;
            
            /* solesie: init cache */
            memset(ufs->aof_base_wbuf, 0, ufs->aof_base_wbuf_size);
            ufs->aof_base_wbuf_len = 0;

            /* solesie: deallocate */
            uint64_t end_offt_ceiled 
                = ((ufs->manifest.aof_base_cur_offt + lb_size - 1) / lb_size) * lb_size;
            uint32_t nlb = (end_offt_ceiled - start_offt) >> lba_shift;
            fdpNvmeDeallocateLba(ufs->fdp_nvme, ufs->manifest.aof_base_start_lba, nlb);
            break;
        }
        case FDP_UFS_RDB: {
            uint64_t start_offt = ufs->manifest.rdb_start_lba << lba_shift;
            ufs->manifest.rdb_cur_offt = ufs->manifest.rdb_cur_offt_aligned
                = ufs->rdb_rofft = ufs->rdb_rofft_aligned
                = start_offt;
            
            /* solesie: init cache */
            memset(ufs->rdb_wbuf, 0, ufs->rdb_wbuf_size);
            ufs->rdb_wbuf_len = 0;

            /* solesie: deallocate */
            uint64_t end_offt_ceiled 
                = ((ufs->manifest.rdb_cur_offt + lb_size - 1) / lb_size) * lb_size;
            uint32_t nlb = (end_offt_ceiled - start_offt) >> lba_shift;
            fdpNvmeDeallocateLba(ufs->fdp_nvme, ufs->manifest.rdb_start_lba, nlb);
            break;
        }
        default:{
            exit(1);
        }
    }
}

void fdpUfsResetReadPointer(fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
                = ufs->manifest.aof_base_start_lba << ufs->lba_shift;

            break;
        }
        case FDP_UFS_RDB: {
            ufs->rdb_rofft = ufs->rdb_rofft_aligned
                = ufs->manifest.rdb_start_lba << ufs->lba_shift;
            
            break;
        }
        default:{
            exit(1);
        }
    }
}

static void writeInternal(
    const void *buf, 
    size_t len, 
    uint64_t aligned_offt, 
    int phd){
    
    fdpUfs *ufs = server.fdp_ufs;
    fdpDevice *fd = ufs->fdp_device;
    fdpNvme *fn = ufs->fdp_nvme;
    size_t lb_size = fdpNvmeGetLbSize(fn);
    uint32_t io_size = ufs->device_default_io_size;

    uint8_t *data = (uint8_t *)buf;
    size_t aligned_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */
    while(aligned_len > 0){
        size_t user_write_len = min(io_size, len);
        size_t write_len = min(io_size, aligned_len);
        void *aligned_buf = zcalloc_aligned(lb_size, write_len);
        memcpy(aligned_buf, data, user_write_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = WRITE;
        
        fdpDeviceIOWrite(fd, aligned_buf, write_len, aligned_offt, phd, arg);

        data += write_len;
        aligned_len -= write_len;
        len -= write_len;
        aligned_offt += write_len;
    }
}

static void readInternal(
    void *buf,
    size_t len, 
    uint64_t aligned_offt){
        
    fdpUfs *ufs = server.fdp_ufs;
    fdpDevice *fd = ufs->fdp_device;
    fdpNvme *fn = ufs->fdp_nvme;
    size_t lb_size = fdpNvmeGetLbSize(fn);
    uint32_t io_size = ufs->device_default_io_size;

    uint8_t *data = (uint8_t *)buf;
    size_t aligned_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */

    while(aligned_len > 0){
        size_t user_read_len = min(io_size, len);
        size_t read_len = min(io_size, aligned_len);
        void *aligned_buf = zcalloc_aligned(lb_size, read_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = READ;
        arg->user_buf = data;
        arg->user_buf_len = user_read_len;
        
        fdpDeviceIORead(fd, aligned_buf, read_len, aligned_offt, arg);

        data += read_len;
        aligned_len -= read_len;
        len -= read_len;
        aligned_offt += read_len;
    }
}

void fdpUfsIOWrite(const void *buf, size_t len, fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;

    /* solesie: Apply direct io to manifest. */
    if(type == FDP_UFS_MANIFEST){
        writeInternal(buf, len, 0, ufs->manifest.manifest_phd);
        return;
    }

    uint8_t *cache_buf = NULL;
    uint64_t *cur_offt = NULL;
    uint64_t *cur_offt_aligned = NULL;
    size_t *cache_buf_size = NULL;
    size_t *cache_buf_len = NULL;
    int phd = 0;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            cache_buf = (uint8_t*)ufs->aof_base_wbuf;
            cur_offt = &(ufs->manifest.aof_base_cur_offt);
            cur_offt_aligned = &(ufs->manifest.aof_base_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_base_wbuf_size);
            cache_buf_len = &(ufs->aof_base_wbuf_len);
            phd = ufs->manifest.aof_phd;
            break;
        }
        case FDP_UFS_RDB: {
            cache_buf = (uint8_t*)ufs->rdb_wbuf;
            cur_offt = &(ufs->manifest.rdb_cur_offt);
            cur_offt_aligned = &(ufs->manifest.rdb_cur_offt_aligned);
            cache_buf_size = &(ufs->rdb_wbuf_size);
            cache_buf_len = &(ufs->rdb_wbuf_len);
            phd = ufs->manifest.rdb_phd;
            break;
        }
        default: {
            exit(1);
        }
    }

    size_t cache_buf_remaining = (*cache_buf_size) - (*cache_buf_len);
    
    /* solesie: caching strategy */
    if(len < cache_buf_remaining){
        memcpy(cache_buf + (*cache_buf_len), buf, len);
        *cur_offt += len;
        *cache_buf_len += len;
        return;
    }

    size_t remaining_len = len;
    uint8_t *user_buf = (uint8_t *)buf;

    /* solesie: head of write */
    if(*cache_buf_len > 0){
        /* solesie: fill cache */
        size_t write_len = cache_buf_remaining;
        memcpy(cache_buf + (*cache_buf_len), user_buf, write_len);
        *cur_offt += write_len;
        user_buf += write_len;
        remaining_len -= write_len;

        /* solesie: flush cache */
        assert(*cache_buf_len + write_len == *cache_buf_size);
        writeInternal(cache_buf, *cache_buf_size, *cur_offt_aligned, phd);
        *cur_offt_aligned += *cache_buf_size;
        
        /* solesie: init cache */
        *cache_buf_len = 0;
        memset(cache_buf, 0, *cache_buf_size);
    }

    /* solesie: body of write */
    if(remaining_len / (*cache_buf_size) > 0){
        size_t write_len = (remaining_len / (*cache_buf_size)) * (*cache_buf_size); /* floor */
        writeInternal(user_buf, write_len, *cur_offt_aligned, phd);
        *cur_offt += write_len;
        user_buf += write_len;
        remaining_len -= write_len;
        *cur_offt_aligned += write_len;
    }

    /* solesie: tail of write */
    if(remaining_len){
        size_t write_len = remaining_len;
        memcpy(cache_buf, user_buf, write_len);
        *cur_offt += write_len;
        user_buf += write_len;
        remaining_len -= write_len;
    }

    assert(remaining_len == 0);
}

void fdpUfsIORead(void *buf, size_t len, fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lb_size = 1 << server.fdp_ufs->lba_shift;

    /* solesie: Apply direct io to manifest. */
    if(type == FDP_UFS_MANIFEST){
        readInternal(buf, len, 0);
        return;
    }

    uint64_t *cur_offt = NULL;
    uint64_t *cur_offt_aligned = NULL;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            cur_offt = &(ufs->aof_base_rofft);
            cur_offt_aligned = &(ufs->aof_base_rofft_aligned);
            break;
        }
        case FDP_UFS_RDB: {
            cur_offt = &(ufs->rdb_rofft);
            cur_offt_aligned = &(ufs->rdb_rofft_aligned);
            break;
        }
        default: {
            exit(1);
        }
    }

    readInternal(buf, len, *cur_offt_aligned);
    *cur_offt += len;
    *cur_offt_aligned = ((*cur_offt) / lb_size) * lb_size;
}

void fdpUfsIOFlush(fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    void *cache_buf = NULL;
    uint64_t *cur_offt_aligned = NULL;
    size_t *cache_buf_size = NULL;
    size_t *cache_buf_len = NULL;
    int phd = 0;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            cache_buf = ufs->aof_base_wbuf;
            cur_offt_aligned = &(ufs->manifest.aof_base_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_base_wbuf_size);
            cache_buf_len = &(ufs->aof_base_wbuf_len);
            phd = ufs->manifest.aof_phd;
            break;
        }
        case FDP_UFS_RDB: {
            cache_buf = ufs->rdb_wbuf;
            cur_offt_aligned = &(ufs->manifest.rdb_cur_offt_aligned);
            cache_buf_size = &(ufs->rdb_wbuf_size);
            cache_buf_len = &(ufs->rdb_wbuf_size);
            phd = ufs->manifest.rdb_phd;
            break;
        }
        default: {
            exit(1);
        }
    }

    /* solesie: flush cache */
    if(*cache_buf_len > 0){
        writeInternal(cache_buf, *cache_buf_size, *cur_offt_aligned, phd);
        *cur_offt_aligned += *cache_buf_size;
        
        /* solesie: init cache */
        *cache_buf_len = 0;
        memset(cache_buf, 0, *cache_buf_size);
    }
}

int fdpUfsIOWait(void){
    return fdpDeviceIOWait(server.fdp_ufs->fdp_device);
}

#endif