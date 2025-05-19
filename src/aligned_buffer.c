#ifndef REDIS_IOURING_DISABLE

#include <memory.h>
#include "zmalloc.h"
#include "aligned_buffer.h"

AlignedBuffer *alignedBufferAllocate(FdpNvme *fdpNvme, off_t offset, size_t size){
    AlignedBuffer *ret = (AlignedBuffer*)zmalloc(sizeof(*ret));
    uint32_t lbSize  = fdpNvmeGetLbSize(fdpNvme);

    ret->offset = (offset / lbSize) * lbSize; /* floor */
    ret->size = ((size + lbSize - 1) / lbSize) * lbSize; /* ceil */

    int err = posix_memalign(&ret->ptr, lbSize, ret->size);
    if(err != 0){
        zfree(ret);
        errno = err;
        return NULL;
    }
    memset(ret->ptr, 0, ret->size);
    return ret;
}

void alignedBufferFree(AlignedBuffer *buf){
    zfree(buf->ptr);
    zfree(buf);
}

#endif