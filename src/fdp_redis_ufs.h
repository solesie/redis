#ifndef __REDIS_FDP_REDIS_UFS_H
#define __REDIS_FDP_REDIS_UFS_H

#ifndef REDIS_IOURING_DISABLE
#include "fdp_module.h"
#include <stdint.h>

// #define GET_OFFT(addr)  ( (uint64_t)(addr) & ((1ULL << (server.fdp_ufs->manifest.lba_shift)) - 1) )
// #define GET_NLBA(addr)  ( (uint64_t)(addr) >> (server.fdp_ufs->manifest.lba_shift)  ) 
// static inline uint64_t calcDataSize(uint64_t addr){
//     return GET_OFFT(addr) 
//         + (GET_NLBA(addr) - 1) * (1 << server.fdp_ufs->manifest.lba_shift);
// }

typedef enum{
    FDP_UFS_MANIFEST_RIO,
    FDP_UFS_MANIFEST_BIO,
    FDP_UFS_AOF_BASE,
    FDP_UFS_AOF_INCR,
    FDP_UFS_RDB,

    /* solesie: fill dummy logic */

    FDP_UFS_RESERVE,
    FDP_UFS_AOF_INCR2
} fdpUfsDataType;

/*-----------------------------------------------------------------------------
 * solesie: FDP io_uring Direct IO manifest definition
 *----------------------------------------------------------------------------*/
typedef struct {
    struct {
        uint64_t    manifest_rio_start_lba;
        int         manifest_rio_phd;

        uint64_t    aof_base_start_lba;
        uint64_t    aof_base_cur_offt;
        uint64_t    aof_base_cur_offt_aligned;
        int         aof_base_phd;

        uint64_t    rdb_start_lba;
        uint64_t    rdb_cur_offt;
        uint64_t    rdb_cur_offt_aligned;
        int         rdb_phd;

        uint64_t    reserve_start_lba;
        uint64_t    reserve_cur_offt;
        uint64_t    reserve_cur_offt_aligned;
    } rio;

    struct {
        uint64_t    manifest_bio_start_lba;
        int         manifest_bio_phd;

        uint64_t    aof_incr_start_lba;
        uint64_t    aof_incr_cur_offt;
        uint64_t    aof_incr_cur_offt_aligned;
        int         aof_incr_phd;

        uint64_t    aof_incr2_start_lba;
        uint64_t    aof_incr2_cur_offt;
        uint64_t    aof_incr2_cur_offt_aligned;
    } bio;
} fdpUfsManifest;

typedef struct _fdpUfs{
    /* solesie: metadata */
    fdpUfsManifest manifest;

    /* solesie: redis bulk IO (rio) */
    fdpModule *fdp_module_rio;
    /* solesie: redis tiny(e.g., manifest, fsync) IO (bio) */
    fdpModule *fdp_module_bio;
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

    /* solesie: aof_incr write buffer */
    void *aof_incr_wbuf;
    /* solesie: size should be aligned with lba size */
    size_t aof_incr_wbuf_size;
    size_t aof_incr_wbuf_len;

    /* solesie: rdb write buffer */
    void *rdb_wbuf;
    /* solesie: size should be aligned with lba size */
    size_t rdb_wbuf_size;
    size_t rdb_wbuf_len;

    /* solesie: rdb read buffer */
    void *rdb_rbuf;
    /* solesie: size should be aligned with lba size */
    size_t rdb_rbuf_size;
    size_t rdb_rbuf_len;

    /* solesie: aof_base read buffer */
    void *aof_base_rbuf;
    /* solesie: size should be aligned with lba size */
    size_t aof_base_rbuf_size;
    size_t aof_base_rbuf_len;

    /* solesie: aof_incr read buffer */
    void *aof_incr_rbuf;
    /* solesie: size should be aligned with lba size */
    size_t aof_incr_rbuf_size;
    size_t aof_incr_rbuf_len;

    /* solesie: no need to save read pointer in manifest */
    uint64_t aof_base_rofft;
    uint64_t aof_base_rofft_aligned;
    uint64_t aof_incr_rofft;
    uint64_t aof_incr_rofft_aligned;
    uint64_t rdb_rofft;
    uint64_t rdb_rofft_aligned;
} fdpUfs;

void fdpUfsSuccessCb(void *arg);
void fdpUfsInit(void);
void fdpUfsActivateRio(void);
void fdpUfsDeactivateRio(void);
int fdpUfsIOWrite(const void *buf, uint64_t len, fdpUfsDataType type);
void fdpUfsIOFlush(fdpUfsDataType type);
void fdpUfsIORead(void *buf, uint64_t len, fdpUfsDataType type);
void fdpUfsResetAofIncr(void);
void fdpUfsResetRdb(void);
void fdpUfsResetAofBase(void);
void fdpUfsResetReadPointer(fdpUfsDataType type);
int fdpUfsIOWait(fdpUfsDataType type);

#endif
#endif