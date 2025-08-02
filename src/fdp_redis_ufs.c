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
 * | manifest ----- aof_base ----- aof_incr -----  rdb | 
 * ---------------------------------------------------- */
static void fdpUfsInitManifest(fdpUfs *ufs){
    fdpNvme *fn = ufs->fdp_nvme;
    uint32_t lba_shift = ufs->lba_shift;

    /* solesie: devide lba space */ 
    uint64_t unit = (ufs->device_size / 100) >> lba_shift;
    uint64_t chunk1 = unit * 22;
    uint64_t chunk2 = unit * 56;
    // uint64_t chunk3 = unit * 22;
    ufs->manifest.rio.manifest_rio_start_lba = 0;
    ufs->manifest.bio.manifest_bio_start_lba = ufs->manifest.rio.manifest_rio_start_lba + 1;
    ufs->manifest.rio.aof_base_start_lba = ufs->manifest.bio.manifest_bio_start_lba + 1;
    ufs->manifest.bio.aof_incr_start_lba = ufs->manifest.rio.aof_base_start_lba + chunk1;
    ufs->manifest.rio.rdb_start_lba = ufs->manifest.bio.aof_incr_start_lba + chunk2;

    /* solesie: allocate FDP placement handle */
    int manifest_phd = fdpNvmeAllocateFdpHandle(fn);
    int aof_phd = fdpNvmeAllocateFdpHandle(fn);
    int rdb_phd = fdpNvmeAllocateFdpHandle(fn);
    if(server.pid_enabled){
        ufs->manifest.rio.manifest_rio_phd = manifest_phd;
        ufs->manifest.rio.aof_base_phd = aof_phd;
        ufs->manifest.rio.rdb_phd = rdb_phd;
        ufs->manifest.bio.manifest_bio_phd = manifest_phd;
        ufs->manifest.bio.aof_incr_phd = aof_phd;
    } else{
        ufs->manifest.rio.manifest_rio_phd = 1;
        ufs->manifest.rio.aof_base_phd = 1;
        ufs->manifest.rio.rdb_phd = 1;
        ufs->manifest.bio.manifest_bio_phd = 1;
        ufs->manifest.bio.aof_incr_phd = 1;
    }

    /* solesie: init offset */
    ufs->manifest.rio.aof_base_cur_offt = ufs->manifest.rio.aof_base_cur_offt_aligned
        = ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
        = ufs->manifest.rio.aof_base_start_lba << lba_shift;
    ufs->manifest.bio.aof_incr_cur_offt = ufs->manifest.bio.aof_incr_cur_offt_aligned
        = ufs->aof_incr_rofft = ufs->aof_incr_rofft_aligned
        =ufs->manifest.bio.aof_incr_start_lba << lba_shift;
    ufs->manifest.rio.rdb_cur_offt = ufs->manifest.rio.rdb_cur_offt_aligned
        = ufs->rdb_rofft = ufs->rdb_rofft_aligned
        = ufs->manifest.rio.rdb_start_lba << lba_shift;

    return;
}

#define QDEPTH_RIO 8
#define QDEPTH_BIO 4
void fdpUfsInit(void){
    server.fdp_ufs = (fdpUfs*)zcalloc(sizeof(*server.fdp_ufs));
    
    fdpNvme *fn = fdpNvmeCreate(server.fdp_device_file);
    fdpModule *fd_bio = fdpModuleCreate(fn, 3, QDEPTH_BIO, fdpUfsSuccessCb);
    server.fdp_ufs->fdp_nvme = fn;
    server.fdp_ufs->fdp_module_bio = fd_bio;
    server.fdp_ufs->device_size = fdpNvmeGetDeviceSize(fn);
    server.fdp_ufs->lba_shift = fdpNvmeGetLbaShift(fn);

    uint32_t ios = fdpNvmeGetMaxIOSize(fn);
    server.fdp_ufs->device_default_io_size = ios;
    // server.fdp_ufs->aof_base_wbuf = zcalloc(ios * QDEPTH_RIO);
    // server.fdp_ufs->aof_incr_wbuf = zcalloc(ios * QDEPTH_BIO);
    // server.fdp_ufs->rdb_wbuf = zcalloc(ios * QDEPTH_RIO);
    // server.fdp_ufs->aof_base_wbuf_size = ios * QDEPTH_RIO;
    // server.fdp_ufs->aof_incr_wbuf_size = ios * QDEPTH_BIO;
    // server.fdp_ufs->rdb_wbuf_size = ios * QDEPTH_RIO;
    server.fdp_ufs->aof_base_wbuf = zcalloc(ios);
    server.fdp_ufs->aof_incr_wbuf = zcalloc(ios);
    server.fdp_ufs->rdb_wbuf = zcalloc(ios);
    server.fdp_ufs->aof_base_wbuf_size = ios;
    server.fdp_ufs->aof_incr_wbuf_size = ios;
    server.fdp_ufs->rdb_wbuf_size = ios;

    /* solesie: 테스트용 */
    fdpNvmeDeallocateLba(fn, 0, server.fdp_ufs->device_size >> server.fdp_ufs->lba_shift);

    fdpUfsInitManifest(server.fdp_ufs);
}

/* solesie: Rio uses multi-process, unlike Bio uses multi-thread.
 * But, io_uring handling may use the thread to handling CQ.
 * So, fdpModule for Rio should be created in child process. */
void fdpUfsActivateRio(void){
    fdpModule *fd_rio = fdpModuleCreate(server.fdp_ufs->fdp_nvme, 3, QDEPTH_RIO, fdpUfsSuccessCb);
    server.fdp_ufs->fdp_module_rio = fd_rio;
}
void fdpUfsDeactivateRio(void){
    fdpModuleRelease(server.fdp_ufs->fdp_module_rio);
    server.fdp_ufs->fdp_module_rio = NULL;
}

void fdpUfsResetAofBase(void){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lba_shift = ufs->lba_shift;
    uint32_t lb_size = 1 << lba_shift;
    uint64_t start_offt = ufs->manifest.rio.aof_base_start_lba << lba_shift;

    /* solesie: deallocate */
    uint64_t end_offt_ceiled 
        = ((ufs->manifest.rio.aof_base_cur_offt + lb_size - 1) / lb_size) * lb_size;
    uint32_t nlb = (end_offt_ceiled - start_offt) >> lba_shift;
    if(nlb != 0){
        fdpNvmeDeallocateLba(ufs->fdp_nvme, ufs->manifest.rio.aof_base_start_lba, nlb);
    }
    
    ufs->manifest.rio.aof_base_cur_offt = ufs->manifest.rio.aof_base_cur_offt_aligned
        = ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
        = start_offt;
    
    /* solesie: init cache */
    memset(ufs->aof_base_wbuf, 0, ufs->aof_base_wbuf_size);
    ufs->aof_base_wbuf_len = 0;
}

void fdpUfsResetRdb(void){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lba_shift = ufs->lba_shift;
    uint32_t lb_size = 1 << lba_shift;
    uint64_t start_offt = ufs->manifest.rio.rdb_start_lba << lba_shift;

    /* solesie: deallocate */
    uint64_t end_offt_ceiled 
        = ((ufs->manifest.rio.rdb_cur_offt + lb_size - 1) / lb_size) * lb_size;
    uint32_t nlb = (end_offt_ceiled - start_offt) >> lba_shift;
    if(nlb != 0){
        fdpNvmeDeallocateLba(ufs->fdp_nvme, ufs->manifest.rio.rdb_start_lba, nlb);
    }

    ufs->manifest.rio.rdb_cur_offt = ufs->manifest.rio.rdb_cur_offt_aligned
        = ufs->rdb_rofft = ufs->rdb_rofft_aligned
        = start_offt;
    
    /* solesie: init cache */
    memset(ufs->rdb_wbuf, 0, ufs->rdb_wbuf_size);
    ufs->rdb_wbuf_len = 0;
}

void fdpUfsResetAofIncr(void){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lba_shift = ufs->lba_shift;
    uint64_t start_offt = ufs->manifest.bio.aof_incr_start_lba << lba_shift;
    
    serverLog(LL_NOTICE, "solesie: incr 초기화, 남은 lb: %ld", ufs->manifest.rio.rdb_start_lba - (ufs->manifest.bio.aof_incr_cur_offt >> lba_shift));

    /* solesie: deallocate */
    uint32_t nlb = ufs->manifest.rio.rdb_start_lba - ufs->manifest.bio.aof_incr_start_lba;
    if(nlb != 0){
        fdpNvmeDeallocateLba(ufs->fdp_nvme, ufs->manifest.bio.aof_incr_start_lba, nlb);
    }
    
    ufs->manifest.bio.aof_incr_cur_offt = ufs->manifest.bio.aof_incr_cur_offt_aligned
        = ufs->aof_incr_rofft = ufs->aof_incr_rofft_aligned
        = start_offt;

    /* solesie: init cache */
    memset(ufs->aof_incr_wbuf, 0, ufs->aof_incr_wbuf_size);
    ufs->aof_incr_wbuf_len = 0;
}

void fdpUfsResetReadPointer(fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            ufs->aof_base_rofft = ufs->aof_base_rofft_aligned
                = ufs->manifest.rio.aof_base_start_lba << ufs->lba_shift;

            break;
        }
        case FDP_UFS_AOF_INCR: {
            ufs->aof_incr_rofft = ufs->aof_incr_rofft_aligned
                = ufs->manifest.bio.aof_incr_start_lba << ufs->lba_shift;

            break;
        }
        case FDP_UFS_RDB: {
            ufs->rdb_rofft = ufs->rdb_rofft_aligned
                = ufs->manifest.rio.rdb_start_lba << ufs->lba_shift;
            
            break;
        }
        default:{
            exit(1);
        }
    }
}

static void writeInternal(
    fdpModule *fm,
    const void *buf, 
    uint64_t len, 
    uint64_t aligned_offt, 
    int phd,
    int rg){
    
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lb_size = 1 << ufs->lba_shift;
    uint32_t io_size = ufs->device_default_io_size;

    uint8_t *data = (uint8_t *)buf;
    uint64_t aligned_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */
    while(aligned_len > 0){
        size_t user_write_len = min(io_size, len);
        size_t write_len = min(io_size, aligned_len);
        void *aligned_buf = zcalloc_aligned(lb_size, write_len);
        memcpy(aligned_buf, data, user_write_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = WRITE;
        
        fdpModuleIOWrite(fm, aligned_buf, write_len, aligned_offt, phd, rg, arg);

        data += write_len;
        aligned_len -= write_len;
        len -= write_len;
        aligned_offt += write_len;
    }
}

static void readInternal(
    fdpModule *fm,
    void *buf,
    uint64_t len, 
    uint64_t aligned_offt){
        
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lb_size = 1 << ufs->lba_shift;
    uint32_t io_size = ufs->device_default_io_size;

    uint8_t *data = (uint8_t *)buf;
    uint64_t aligned_len = ((len + lb_size - 1) / lb_size) * lb_size; /* ceil */

    while(aligned_len > 0){
        size_t user_read_len = min(io_size, len);
        size_t read_len = min(io_size, aligned_len);
        void *aligned_buf = zcalloc_aligned(lb_size, read_len);
        cbArg *arg = (cbArg*)zcalloc(sizeof(*arg));

        arg->aligned_buf = aligned_buf;
        arg->type = READ;
        arg->user_buf = data;
        arg->user_buf_len = user_read_len;
        
        fdpModuleIORead(fm, aligned_buf, read_len, aligned_offt, arg);

        data += read_len;
        aligned_len -= read_len;
        len -= read_len;
        aligned_offt += read_len;
    }
}

int fdpUfsIOWrite(const void *buf, uint64_t len, fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;

    /* solesie: Not apply buffer cache */
    if(type == FDP_UFS_MANIFEST_RIO){
        writeInternal(
            ufs->fdp_module_rio,
            buf, 
            len,
            ufs->manifest.rio.manifest_rio_start_lba << ufs->lba_shift,
            ufs->manifest.rio.manifest_rio_phd,
            0);
        return 1;
    }
    if(type == FDP_UFS_MANIFEST_BIO){
        int rg = 0;
        if(server.rg_enabled){
            rg = 1;
        }
        writeInternal(
            ufs->fdp_module_bio, 
            buf, 
            len, 
            ufs->manifest.bio.manifest_bio_start_lba << ufs->lba_shift,
            ufs->manifest.bio.manifest_bio_phd,
            rg);
        return 1;
    }

    fdpModule *fm = NULL;
    uint8_t *cache_buf = NULL;
    uint64_t *cur_offt = NULL;
    uint64_t *cur_offt_aligned = NULL;
    size_t *cache_buf_size = NULL;
    size_t *cache_buf_len = NULL;
    int phd = 0;
    int rg = 0;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            fm = ufs->fdp_module_rio;
            cache_buf = (uint8_t*)ufs->aof_base_wbuf;
            cur_offt = &(ufs->manifest.rio.aof_base_cur_offt);
            cur_offt_aligned = &(ufs->manifest.rio.aof_base_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_base_wbuf_size);
            cache_buf_len = &(ufs->aof_base_wbuf_len);
            phd = ufs->manifest.rio.aof_base_phd;
            if(server.rg_enabled){
                rg = 0;
            }

            serverAssert(*cur_offt + len < (ufs->manifest.bio.aof_incr_start_lba << ufs->lba_shift));
            break;
        }
        case FDP_UFS_AOF_INCR: {
            fm = ufs->fdp_module_bio;
            cache_buf = (uint8_t*)ufs->aof_incr_wbuf;
            cur_offt = &(ufs->manifest.bio.aof_incr_cur_offt);
            cur_offt_aligned = &(ufs->manifest.bio.aof_incr_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_incr_wbuf_size);
            cache_buf_len = &(ufs->aof_incr_wbuf_len);
            phd = ufs->manifest.bio.aof_incr_phd;
            if(server.rg_enabled){
                rg = 1;
            }

            if(*cur_offt + len >= (ufs->manifest.rio.rdb_start_lba << ufs->lba_shift)){
                serverLog(LL_WARNING, "solesie: %lld < %lld", *cur_offt + len, (ufs->manifest.rio.rdb_start_lba << ufs->lba_shift));
                serverAssert(*cur_offt + len < (ufs->manifest.rio.rdb_start_lba << ufs->lba_shift));
                /* For the fragmentation issue, real-world Redis deployments may need to adopt region management techniques */
            }
            break;
        }
        case FDP_UFS_RDB: {
            fm = ufs->fdp_module_rio;
            cache_buf = (uint8_t*)ufs->rdb_wbuf;
            cur_offt = &(ufs->manifest.rio.rdb_cur_offt);
            cur_offt_aligned = &(ufs->manifest.rio.rdb_cur_offt_aligned);
            cache_buf_size = &(ufs->rdb_wbuf_size);
            cache_buf_len = &(ufs->rdb_wbuf_len);
            phd = ufs->manifest.rio.rdb_phd;
            if(server.rg_enabled){
                rg = 0;
            }

            serverAssert(*cur_offt + len < ufs->device_size);
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
        return 1;
    }

    uint64_t remaining_len = len;
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
        writeInternal(fm, cache_buf, *cache_buf_size, *cur_offt_aligned, phd, rg);
        *cur_offt_aligned += *cache_buf_size;
        
        /* solesie: init cache */
        *cache_buf_len = 0;
        memset(cache_buf, 0, *cache_buf_size);
    }

    /* solesie: body of write */
    if(remaining_len / (*cache_buf_size) > 0){
        uint64_t write_len = (remaining_len / (*cache_buf_size)) * (*cache_buf_size); /* floor */

        writeInternal(fm, user_buf, write_len, *cur_offt_aligned, phd, rg);
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
    return 1;
}

void fdpUfsIORead(void *buf, uint64_t len, fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    uint32_t lb_size = 1 << server.fdp_ufs->lba_shift;

    /* solesie: Apply direct io to manifest. */
    if(type == FDP_UFS_MANIFEST_RIO){
        readInternal(
            ufs->fdp_module_rio, 
            buf, 
            len, 
            ufs->manifest.rio.manifest_rio_start_lba << ufs->lba_shift);
        return;
    }
    if(type == FDP_UFS_MANIFEST_BIO){
        readInternal(
            ufs->fdp_module_bio,
            buf, 
            len, 
            ufs->manifest.bio.manifest_bio_start_lba << ufs->lba_shift);
        return;
    }

    fdpModule *fm = NULL;
    uint64_t *cur_offt = NULL;
    uint64_t *cur_offt_aligned = NULL;
    switch (type){
        case FDP_UFS_AOF_BASE: {
            fm = ufs->fdp_module_rio;
            cur_offt = &(ufs->aof_base_rofft);
            cur_offt_aligned = &(ufs->aof_base_rofft_aligned);
            break;
        }
        case FDP_UFS_AOF_INCR: {
            fm = ufs->fdp_module_bio;
            cur_offt = &(ufs->aof_incr_rofft);
            cur_offt_aligned = &(ufs->aof_incr_rofft_aligned);
            break;
        }
        case FDP_UFS_RDB: {
            fm = ufs->fdp_module_rio;
            cur_offt = &(ufs->rdb_rofft);
            cur_offt_aligned = &(ufs->rdb_rofft_aligned);
            break;
        }
        default: {
            exit(1);
        }
    }

    readInternal(fm, buf, len, *cur_offt_aligned);
    *cur_offt += len;
    *cur_offt_aligned = ((*cur_offt) / lb_size) * lb_size;
}

void fdpUfsIOFlush(fdpUfsDataType type){
    fdpUfs *ufs = server.fdp_ufs;
    fdpModule *fm = NULL;
    uint8_t *cache_buf = NULL;
    uint64_t *cur_offt_aligned = NULL;
    size_t *cache_buf_size = NULL;
    size_t *cache_buf_len = NULL;
    uint32_t lb_size = 1 << ufs->lba_shift;
    int phd = 0;
    int rg = 0;
    switch (type){
        case FDP_UFS_MANIFEST_RIO: {
            return;
        }
        case FDP_UFS_MANIFEST_BIO: {
            return;
        }
        case FDP_UFS_AOF_BASE: {
            fm = ufs->fdp_module_rio;
            cache_buf = (uint8_t*)ufs->aof_base_wbuf;
            cur_offt_aligned = &(ufs->manifest.rio.aof_base_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_base_wbuf_size);
            cache_buf_len = &(ufs->aof_base_wbuf_len);
            phd = ufs->manifest.rio.aof_base_phd;
            if(server.rg_enabled){
                rg = 0;
            }
            break;
        }
        case FDP_UFS_AOF_INCR: {
            fm = ufs->fdp_module_bio;
            cache_buf = (uint8_t*)ufs->aof_incr_wbuf;
            cur_offt_aligned = &(ufs->manifest.bio.aof_incr_cur_offt_aligned);
            cache_buf_size = &(ufs->aof_incr_wbuf_size);
            cache_buf_len = &(ufs->aof_incr_wbuf_len);
            phd = ufs->manifest.bio.aof_incr_phd;
            if(server.rg_enabled){
                rg = 1;
            }
            break;
        }
        case FDP_UFS_RDB: {
            fm = ufs->fdp_module_rio;
            cache_buf = (uint8_t*)ufs->rdb_wbuf;
            cur_offt_aligned = &(ufs->manifest.rio.rdb_cur_offt_aligned);
            cache_buf_size = &(ufs->rdb_wbuf_size);
            cache_buf_len = &(ufs->rdb_wbuf_size);
            phd = ufs->manifest.rio.rdb_phd;
            if(server.rg_enabled){
                rg = 0;
            }
            break;
        }
        default: {
            exit(1);
        }
    }

    /* solesie: flush cache */
    if(*cache_buf_len > 0){
        /* solesie: deal flush with lb_size */
        
        size_t write_len_floored = ((*cache_buf_len) / lb_size) * lb_size; /* floor */
        size_t write_len_ceiled = (((*cache_buf_len) + lb_size - 1) / lb_size) * lb_size;
        writeInternal(fm, cache_buf, write_len_ceiled, *cur_offt_aligned, phd, rg);
        *cur_offt_aligned += write_len_floored;
        
        size_t rem = *cache_buf_len - write_len_floored;
        if (rem > 0) {
            memmove(cache_buf, cache_buf + write_len_floored, rem);
        }
        *cache_buf_len = rem;
        memset(cache_buf + rem, 0, *cache_buf_size - rem);
    }
}

int fdpUfsIOWait(fdpUfsDataType type){
    switch (type){
        case FDP_UFS_MANIFEST_RIO:{
            return fdpModuleIOWait(server.fdp_ufs->fdp_module_rio);
        }
        case FDP_UFS_MANIFEST_BIO:{
            return fdpModuleIOWait(server.fdp_ufs->fdp_module_bio);
        }
        case FDP_UFS_AOF_BASE:{
            return fdpModuleIOWait(server.fdp_ufs->fdp_module_rio);
        }
        case FDP_UFS_AOF_INCR:{
            return fdpModuleIOWait(server.fdp_ufs->fdp_module_bio);
        }
        case FDP_UFS_RDB:{
            return fdpModuleIOWait(server.fdp_ufs->fdp_module_rio);
        }
        default:{
            exit(1);
        }
    }
}

#endif