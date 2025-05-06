#ifndef __REDIS_ALIGNED_BUFFER_INL_H
#define __REDIS_ALIGNED_BUFFER_INL_H

#ifndef REDIS_IOURING_DISABLE

#include "fdp_nvme.h"

/* solesie: read only except for *ptr */
typedef struct _AlignedBuffer{
    void *ptr;
    
    off_t offset;
    size_t size;
    size_t preferredSize;  /* solesie: P9D3a 128KiB */
} AlignedBuffer;

AlignedBuffer *alignedBufferAllocate(FdpNvme *fdpNvme, off_t offset, size_t size);
void alignedBufferFree(AlignedBuffer *buf);

#endif
#endif