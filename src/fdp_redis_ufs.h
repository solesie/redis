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

/*-----------------------------------------------------------------------------
 * solesie: FDP io_uring Direct IO manifest definition
 *----------------------------------------------------------------------------*/
typedef struct {
    int         dirty;                /* 1 Indicates that the ufs manifest in the memory is inconsistent with
                                         disk, we need to persist it immediately. */

    uint64_t    aof_base_start_lba;
    uint64_t    aof_base_addr;         /* solesie: nlba + offt */
    uint64_t    aof_incr_start_lba;
    uint64_t    aof_incr_chunk_num;
    int         aof_phd;
    uint64_t    rdb_start_lba;
    uint64_t    rdb_addr;              /* solesie: nlba + offt */
    int         rdb_phd;
    uint64_t    end_lba;
    uint32_t    lba_shift;
} fdpUfsManifest;

typedef struct _fdpUfs{
    /* solesie: metadata */
    fdpUfsManifest manifest;

    fdpDevice *fdp_device;
    fdpNvme *fdp_nvme;

    /* solesie: aof_incr buffer */
    void *aof_incr_buf;
} fdpUfs;

typedef enum{
    FDP_UFS_MANIFEST,
    FDP_UFS_AOF_BASE,
    FDP_UFS_AOF_INCR,
    FDP_UFS_RDB
} fdpUfsDataType;

void fdpUfsSuccessCb(void *arg);
void fdpUfsInitManifest(fdpUfs *fdp_ufs);
void fdpUfsInit(void);
void fdpUfsIOWrite(const void *buf, size_t len, int pld, fdpUfsDataType type);
void fdpUfsIORead(const void *buf, size_t len, fdpUfsDataType type);
int fdpUfsIOWait(fdpUfs *fdp_ufs);

#endif
#endif