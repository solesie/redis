#ifndef REDIS_IOURING_DISABLE

#include "bio.h"
#include "fdp_module.h"
#include "fdp_redis_ufs.h"
#include "fdp_redis_persistency.h"
#include "server.h"

/* solesie: should be smaller than lba size */
#define MANIFEST_BUF_LEN 4096

static int checkCrc(char *buf, size_t size){
    uint64_t real;

    memcpy(&real, buf + size - sizeof(uint64_t), sizeof(uint64_t));
    uint64_t expected = crc64(0, (unsigned char*)buf, size - sizeof(uint64_t));
    memrev64ifbe(&real);

    if(real != expected || real == 0){
        return 0;
    }

    return 1;
}
static void appendCrc(char *buf, size_t size){
    uint64_t crc = crc64(0, (unsigned char*)buf, size - sizeof(uint64_t));

    memcpy(buf + size - sizeof(uint64_t), &crc, sizeof(uint64_t));
}

static void persistManifestRio(void){
    fdpUfs *ufs = server.fdp_ufs;
    char buf[MANIFEST_BUF_LEN];

    memset(buf, 0, MANIFEST_BUF_LEN);
    memcpy(buf, &ufs->manifest.rio, sizeof(ufs->manifest.rio));
    appendCrc(buf, MANIFEST_BUF_LEN);
    fdpUfsIOWrite(buf, MANIFEST_BUF_LEN, FDP_UFS_MANIFEST_RIO);
    if(fdpUfsIOWait(FDP_UFS_MANIFEST_RIO) != 1){
        exit(1);
    }
    return;
}
static void persistManifestBio(void){
    fdpUfs *ufs = server.fdp_ufs;
    char buf[MANIFEST_BUF_LEN];

    memset(buf, 0, MANIFEST_BUF_LEN);
    memcpy(buf, &ufs->manifest.bio, sizeof(ufs->manifest.bio));
    appendCrc(buf, MANIFEST_BUF_LEN);
    fdpUfsIOWrite(buf, MANIFEST_BUF_LEN, FDP_UFS_MANIFEST_BIO);
    if(fdpUfsIOWait(FDP_UFS_MANIFEST_BIO) != 1){
        exit(1);
    }
    return;
}

static inline int existAofBase(void){
    fdpUfs *ufs = server.fdp_ufs;
    return ufs->manifest.rio.aof_base_cur_offt 
        != (ufs->manifest.rio.aof_base_start_lba << ufs->lba_shift);
}

static inline int existAofIncr(void){
    fdpUfs *ufs = server.fdp_ufs;
    return ufs->manifest.bio.aof_incr_cur_offt 
        != (ufs->manifest.bio.aof_incr_start_lba << ufs->lba_shift);
}

static inline off_t getAofBaseSize(void){
    fdpUfs *ufs = server.fdp_ufs;
    return ufs->manifest.rio.aof_base_cur_offt 
        - (ufs->manifest.rio.aof_base_start_lba << ufs->lba_shift);
}

static inline off_t getAofIncrSize(void){
    fdpUfs *ufs = server.fdp_ufs;
    return ufs->manifest.bio.aof_incr_cur_offt 
        - (ufs->manifest.bio.aof_incr_start_lba << ufs->lba_shift);
}

void fdpPersistencyInit(void){
    /* solesie: validation */
    serverAssert(server.fdp_enabled);
    serverAssert(server.fdp_device_file != NULL);
    // serverAssert(server.aof_use_rdb_preamble);
    // serverAssert(server.aof_fsync == AOF_FSYNC_ALWAYS_FDP_DIRECT_IO);

    fdpUfsInit();
}

void fdpPersistencyLoadManifestFromDisk(void){
    char buf_rio[MANIFEST_BUF_LEN];
    // char buf_bio[MANIFEST_BUF_LEN];

    fdpUfsActivateRio();

    // fdpUfsIORead(buf_bio, MANIFEST_BUF_LEN, FDP_UFS_MANIFEST_BIO);
    // if(fdpUfsIOWait(FDP_UFS_MANIFEST_BIO) != 1){
    //     exit(1);
    // }

    fdpUfsIORead(buf_rio, MANIFEST_BUF_LEN, FDP_UFS_MANIFEST_RIO);
    if(fdpUfsIOWait(FDP_UFS_MANIFEST_RIO) != 1){
        exit(1);
    }

    if(!checkCrc(buf_rio, MANIFEST_BUF_LEN)){
        persistManifestBio();
        persistManifestRio();

        fdpUfsDeactivateRio();
        return;
    }

    fdpUfsDeactivateRio();

    /* solesie: recovery test -> true */
    if(false){
        memcpy(&server.fdp_ufs->manifest.rio, buf_rio, sizeof(server.fdp_ufs->manifest.rio));
        printf("fdp ufs manifest exist!: %d %ld\n", server.fdp_ufs->manifest.rio.aof_base_start_lba, server.fdp_ufs->manifest.rio.aof_base_cur_offt);
    }

    return;
}

static int loadAofBase(void){
    struct client *fakeClient;
    int old_aof_state = server.aof_state;
    int ret = AOF_OK;

    /* Temporarily disable AOF, to prevent EXEC from feeding a MULTI
     * to the same file we're about to read. */
    server.aof_state = AOF_OFF;

    client *old_cur_client = server.current_client;
    client *old_exec_client = server.executing_client;
    fakeClient = createAOFClient();
    server.current_client = server.executing_client = fakeClient;

    fdpUfsActivateRio();

    /* RDB format. Pass loading the RDB functions. */
    rio r;
    rioInitWithFdpUfs(&r, FDP_UFS_AOF_BASE);

    if (rdbLoadRio(&r,RDBFLAGS_AOF_PREAMBLE,NULL) != C_OK) {
        serverLog(LL_WARNING, "solesie: Error reading the fdp ufs RDB preamble, AOF loading aborted");
        ret = AOF_FAILED;
        goto cleanup;
    } else {
        loadingAbsProgress(getAofBaseSize());
    }
    server.aof_state = old_aof_state;

cleanup:
    fdpUfsResetReadPointer(FDP_UFS_AOF_BASE);
    fdpUfsDeactivateRio();

    if (fakeClient) freeClient(fakeClient);
    server.current_client = old_cur_client;
    server.executing_client = old_exec_client;
    return ret;
}

static int loadAofIncr(void){

    /* solesie: TODO, but... after redis 7.0, does it need for test? */


    return AOF_OK;
}

int fdpPersistencyLoadAof(void){
    int ret = AOF_OK;
    long long start;
    off_t total_size = 0, base_size = 0;

    /* Here we calculate the total size of all BASE and INCR files in
     * advance, it will be set to `server.loading_total_bytes`. */
    total_size += getAofBaseSize();
    total_size += getAofIncrSize();
    if (total_size == 0) {
        return AOF_EMPTY;
    }

    startLoading(total_size, RDBFLAGS_AOF_PREAMBLE, 0);

    /* Load BASE AOF if needed. */
    if (existAofBase()) {
        base_size = getAofBaseSize();
        start = ustime();
        ret = loadAofBase();
        if (ret == AOF_OK) {
            serverLog(LL_NOTICE, "DB loaded from base: %.3f seconds",
                (float)(ustime()-start)/1000000);
        }

        if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
            goto cleanup;
        }
    }

    /* Load INCR AOFs if needed. */
    if (existAofIncr()) {
        start = ustime();
        ret = loadAofIncr();
        if(ret == AOF_OK){
            serverLog(LL_NOTICE, "DB loaded from incr: %.3f seconds",
                (float)(ustime()-start)/1000000);
        }
        /* We know that (at least) one of the AOF files has data (total_size > 0),
         * so empty incr AOF file doesn't count as a AOF_EMPTY result */
        if (ret == AOF_EMPTY) ret = AOF_OK;
        if (ret == AOF_OPEN_ERR || ret == AOF_FAILED) {
            goto cleanup;
        }
    }

    server.aof_current_size = total_size;
    /* Ideally, the aof_rewrite_base_size variable should hold the size of the
     * AOF when the last rewrite ended, this should include the size of the
     * incremental file that was created during the rewrite since otherwise we
     * risk the next automatic rewrite to happen too soon (or immediately if
     * auto-aof-rewrite-percentage is low). However, since we do not persist
     * aof_rewrite_base_size information anywhere, we initialize it on restart
     * to the size of BASE AOF file. This might cause the first AOFRW to be
     * executed early, but that shouldn't be a problem since everything will be
     * fine after the first AOFRW. */
    server.aof_rewrite_base_size = base_size;

cleanup:
    stopLoading(ret == AOF_OK);
    return ret;
}

/* Write a sequence of commands able to fully rebuild the dataset into
 * "filename". Used both by REWRITEAOF and BGREWRITEAOF.
 *
 * In order to minimize the number of commands needed in the rewritten
 * log Redis uses variadic commands when possible, such as RPUSH, SADD
 * and ZADD. However at max AOF_REWRITE_ITEMS_PER_CMD items per time
 * are inserted using a single command. */
int fdpPersistencyBackgroundRewriteAof(void) {
    pid_t childpid;

    if (hasActiveChildProcess()) return C_ERR;

    /* We set aof_selected_db to -1 in order to force the next call to the
     * feedAppendOnlyFile() to issue a SELECT command. */
    server.aof_selected_db = -1;
    fdpPersistencyOpenNewAofIncr();

    if (server.aof_state == AOF_WAIT_REWRITE) {
        /* Wait for all bio jobs related to AOF to drain. This prevents a race
         * between updates to `fsynced_reploff_pending` of the worker thread, belonging
         * to the previous AOF, and the new one. This concern is specific for a full
         * sync scenario where we don't wanna risk the ACKed replication offset
         * jumping backwards or forward when switching to a different master. */
        bioDrainWorker(BIO_FDP_PERSISTENCY_AOF_INCR_SAVE);

        /* Set the initial repl_offset, which will be applied to fsynced_reploff
         * when AOFRW finishes (after possibly being updated by a bio thread) */
        atomicSet(server.fsynced_reploff_pending, server.master_repl_offset);
        server.fsynced_reploff = 0;
    }

    server.stat_aof_rewrites++;

    if ((childpid = redisFork(CHILD_TYPE_AOF)) == 0) {
        /* Child */
        redisSetProcTitle("redis-aof-rewrite");
        redisSetCpuAffinity(server.aof_rewrite_cpulist);

        rio r;
        int error;
        
        /* solesie: for now, skip temp-rewriteaof.aof logic */
        fdpUfsResetAofBase();

        fdpUfsActivateRio();

        rioInitWithFdpUfs(&r, FDP_UFS_AOF_BASE);

        startSaving(RDBFLAGS_AOF_PREAMBLE);

        if (rdbSaveRio(SLAVE_REQ_NONE,&r,&error,RDBFLAGS_AOF_PREAMBLE,NULL) == C_ERR) {
            errno = error;
            goto werr;
        }

        /* solesie: ufs */
        rioFlush(&r);
        persistManifestRio();

        fdpUfsDeactivateRio();

        /* solesie: for now, skip temp-rewriteaof.aof logic */

        stopSaving(1);

        serverLog(LL_NOTICE,
            "Successfully created the temporary AOF base");
        sendChildCowInfo(CHILD_INFO_TYPE_AOF_COW_SIZE, "AOF rewrite");
        exitFromChild(0);

    werr:
        serverLog(LL_WARNING,"Fdp ufs aof base rewrite error: %s", strerror(errno));
        stopSaving(0);
        exitFromChild(1);
    } else {
        /* Parent */
        if (childpid == -1) {
            server.aof_lastbgrewrite_status = C_ERR;
            serverLog(LL_WARNING,
                "Can't rewrite append only file in background: fork: %s",
                strerror(errno));
            return C_ERR;
        }
        serverLog(LL_NOTICE,
            "Background append only file rewriting started by pid %ld",(long) childpid);
        server.aof_rewrite_scheduled = 0;
        server.aof_rewrite_time_start = time(NULL);
        return C_OK;
    }

    return C_OK; /* unreached */
}

/* Save the DB on disk. Return C_ERR on error, C_OK on success. */
int fdpPersistencySaveRdb(int req, rdbSaveInfo *rsi, int rdbflags) {
    rio r;
    int error = 0;

    /* solesie: for now, skip temp.rdb logic */
    fdpUfsResetRdb();

    fdpUfsActivateRio();

    rioInitWithFdpUfs(&r, FDP_UFS_RDB);

    startSaving(rdbflags);

    if (rdbSaveRio(req,&r,&error,rdbflags,rsi) == C_ERR) {
        errno = error;
        goto werr;
    }

    rioFlush(&r);
    persistManifestRio();

    fdpUfsDeactivateRio();

    serverLog(LL_NOTICE,"DB saved on disk");
    server.dirty = 0;
    server.lastsave = time(NULL);
    server.lastbgsave_status = C_OK;

    stopSaving(1);
    return C_OK;

werr:
    serverLog(LL_WARNING,"Fdp ufs rdb error: %s", strerror(errno));
    stopSaving(0);
    return C_ERR;
}

int fdpPersistencySaveAofIncr(void *data, uint64_t len){
    return fdpUfsIOWrite(data, len, FDP_UFS_AOF_INCR);
}

void fdpPersistencyAofIncrFsync(void){
    fdpUfsIOFlush(FDP_UFS_AOF_INCR);
    persistManifestBio();
}

void fdpPersistencyAofIncrBackgroundFsync(void){
    bioCreateFdpPersistencyAofIncrSaveJob();
}

int fdpPersistencyAofIncrFsyncInProgress(void) {
    return bioPendingJobsOfType(BIO_FDP_PERSISTENCY_AOF_INCR_SAVE) != 0;
}

void fdpPersistencyOpenNewAofIncr(void){
    /* solesie: for now, skip failure logic */

    fdpUfsResetAofIncr();

    persistManifestBio();

    /* Reset the aof_last_incr_size. */
    server.aof_last_incr_size = 0;
    /* Reset the aof_last_incr_fsync_offset. */
    server.aof_last_incr_fsync_offset = 0;
}

void fdpPersistencyBackgroundRewriteDoneHandler(int exitcode, int bysignal){
    if (!bysignal && exitcode == 0) {
        long long now = ustime();

        serverLog(LL_NOTICE,
            "Background AOF rewrite terminated with success");
        
        persistManifestBio();

        if (server.aof_state != AOF_OFF) {
            /* AOF enabled. */
            server.aof_current_size = getAofBaseSize() + server.aof_last_incr_size;
            /* solesie: Regard Phase 1 as one day */
            server.aof_rewrite_base_size = getAofBaseSize();
        }

        server.aof_lastbgrewrite_status = C_OK;
        server.stat_aofrw_consecutive_failures = 0;

        serverLog(LL_NOTICE, "Background AOF rewrite finished successfully");

        /* Change state from WAIT_REWRITE to ON if needed */
        if (server.aof_state == AOF_WAIT_REWRITE) {
            server.aof_state = AOF_ON;

            /* Update the fsynced replication offset that just now become valid.
             * This could either be the one we took in startAppendOnly, or a
             * newer one set by the bio thread. */
            long long fsynced_reploff_pending;
            atomicGet(server.fsynced_reploff_pending, fsynced_reploff_pending);
            server.fsynced_reploff = fsynced_reploff_pending;
        }

        serverLog(LL_VERBOSE,
            "Background AOF rewrite signal handler took %lldus", ustime()-now);
    } else if (!bysignal && exitcode != 0) {
        server.aof_lastbgrewrite_status = C_ERR;
        server.stat_aofrw_consecutive_failures++;

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated with error");
    } else {
        /* SIGUSR1 is whitelisted, so we have a way to kill a child without
         * triggering an error condition. */
        if (bysignal != SIGUSR1) {
            server.aof_lastbgrewrite_status = C_ERR;
            server.stat_aofrw_consecutive_failures++;
        }

        serverLog(LL_WARNING,
            "Background AOF rewrite terminated by signal %d", bysignal);
    }

    server.aof_rewrite_time_last = time(NULL)-server.aof_rewrite_time_start;
    server.aof_rewrite_time_start = -1;
}

#endif