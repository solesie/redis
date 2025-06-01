#ifndef __REDIS_FDP_REDIS_UFS_H
#define __REDIS_FDP_REDIS_UFS_H

#ifndef REDIS_IOURING_DISABLE
#include "fdp_device.h"
#include <stdint.h>

// #define GET_OFFT(addr)  ( (uint64_t)(addr) & ((1ULL << (server.fdp_ufs->manifest.lba_shift)) - 1) )
// #define GET_NLBA(addr)  ( (uint64_t)(addr) >> (server.fdp_ufs->manifest.lba_shift)  ) 
// static inline uint64_t calcDataSize(uint64_t addr){
//     return GET_OFFT(addr) 
//         + (GET_NLBA(addr) - 1) * (1 << server.fdp_ufs->manifest.lba_shift);
// }

typedef enum{
    FDP_UFS_MANIFEST,
    FDP_UFS_AOF_BASE,
    FDP_UFS_RDB
} fdpUfsDataType;

/*-----------------------------------------------------------------------------
 * solesie: FDP io_uring Direct IO manifest definition
 *----------------------------------------------------------------------------*/
typedef struct {
    int         dirty;                /* 1 Indicates that the ufs manifest in the memory is inconsistent with
                                         disk, we need to persist it immediately. */
    int         manifest_phd;
    uint64_t    aof_base_start_lba;
    uint64_t    aof_base_cur_offt;
    uint64_t    aof_base_cur_offt_aligned;
    int         aof_phd;               /* solesie: == 0 because incr uses file system */
    uint64_t    rdb_start_lba;
    uint64_t    rdb_cur_offt;
    uint64_t    rdb_cur_offt_aligned;
    int         rdb_phd;

    
} fdpUfsManifest;

typedef struct _fdpUfs{
    /* solesie: metadata */
    fdpUfsManifest manifest;

    fdpDevice *fdp_device;
    fdpNvme *fdp_nvme;
    /* solesie: should be smaller than max io size supported by device */
    uint32_t device_default_io_size;
    uint64_t device_size;
    uint32_t lba_shift;

    /* solesie: aof_base write buffer */
    void *aof_base_wbuf;
    /* solesie: size should be aligned with lba size */
    size_t aof_base_wbuf_size;
    size_t aof_base_wbuf_len;
    /* solesie: rdb write buffer */
    void *rdb_wbuf;
    /* solesie: size should be aligned with lba size */
    size_t rdb_wbuf_size;
    size_t rdb_wbuf_len;

    /* solesie: no need to save read pointer in manifest */
    uint64_t aof_base_rofft;
    uint64_t aof_base_rofft_aligned;
    /* solesie: no need to save read pointer in manifest */
    uint64_t rdb_rofft;
    uint64_t rdb_rofft_aligned;
} fdpUfs;

void fdpUfsSuccessCb(void *arg);
void fdpUfsInit(void);
void fdpUfsIOWrite(const void *buf, size_t len, fdpUfsDataType type);
void fdpUfsIOFlush(fdpUfsDataType type);
void fdpUfsIORead(void *buf, size_t len, fdpUfsDataType type);
void fdpUfsResetData(fdpUfsDataType type);
int fdpUfsIOWait(void);

#endif
#endif