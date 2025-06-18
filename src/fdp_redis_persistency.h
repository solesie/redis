#ifndef __REDIS_FDP_REDIS_PERSISTENCY_H
#define __REDIS_FDP_REDIS_PERSISTENCY_H

#ifndef REDIS_IOURING_DISABLE
#include "server.h"

void fdpPersistencyInit(void);
void fdpPersistencyLoadManifestFromDisk(void);
int fdpPersistencyLoadAof(void);
int fdpPersistencyBackgroundRewriteAof(void);
int fdpPersistencySaveRdb(int req, rdbSaveInfo *rsi, int rdbflags);
void fdpPersistencySaveAofIncr(void *data, uint64_t len);
void fdpPersistencyOpenNewAofIncr(void);
void fdpPersistencyAofIncrFsync(void);
int fdpPersistencyAofIncrFsyncInProgress(void);
void fdpPersistencyBackgroundRewriteDoneHandler(int exitcode, int bysignal);

#endif

#endif