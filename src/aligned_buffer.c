#ifndef REDIS_IOURING_DISABLE

#include <memory.h>
#include "zmalloc.h"
#include "aligned_buffer.h"

AlignedBuffer *alignedBufferAllocate(FdpNvme *fdpNvme, off_t offset, size_t size){
    AlignedBuffer *ret = (AlignedBuffer*)zmalloc(sizeof(*ret));
    uint32_t lbSize  = fdpNvmeGetLbSize(fdpNvme);
    uint32_t prefSz  = fdpNvmeGetPreferredWriteSize(fdpNvme);

    ret->offset = (offset / lbSize) * lbSize; /* floor */
    ret->size = ((size + lbSize - 1) / lbSize) * lbSize; /* ceil */
    ret->preferredSize = ((size + prefSz - 1) / prefSz) * prefSz;

    int err = posix_memalign(&ret->ptr, lbSize, prefSz);
    if(err != 0){
        free(ret);
        errno = err;
        return NULL;
    }
    memset(ret->ptr, 0, prefSz);
    return ret;
}

void alignedBufferFree(AlignedBuffer *buf){
    free(buf->ptr);
    free(buf);
}

#endif