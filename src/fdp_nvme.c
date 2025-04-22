#ifndef REDIS_IOURING_DISABLE
#include <regex.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include "zmalloc.h"
#include "fdp_nvme.h"

/* Reference: https://github.com/axboe/fio/blob/master/engines/nvme.h
 * If the uapi headers installed on the system lacks nvme uring command
 * support, use the local version to prevent compilation issues. */
#ifndef CONFIG_NVME_URING_CMD
struct nvme_uring_cmd {
    __u8 opcode;
    __u8 flags;
    __u16 rsvd1;
    __u32 nsid;
    __u32 cdw2;
    __u32 cdw3;
    __u64 metadata;
    __u64 addr;
    __u32 metadata_len;
    __u32 data_len;
    __u32 cdw10;
    __u32 cdw11;
    __u32 cdw12;
    __u32 cdw13;
    __u32 cdw14;
    __u32 cdw15;
    __u32 timeout_ms;
    __u32 rsvd2;
};
#define NVME_URING_CMD_IO _IOWR('N', 0x80, struct nvme_uring_cmd)
#define NVME_URING_CMD_IO_VEC _IOWR('N', 0x81, struct nvme_uring_cmd)
#endif /* CONFIG_NVME_URING_CMD */

#define NVME_DEFAULT_IOCTL_TIMEOUT 0

#define DEFAULT_PLACEMENT_ID_IDX 0

enum nvme_io_mgmt_recv_mo {
    NVME_IO_MGMT_RECV_RUH_STATUS = 0x1,
};

enum nvme_io_opcode {
    nvme_cmd_write = 0x01,
    nvme_cmd_read = 0x02,
    nvme_cmd_io_mgmt_recv = 0x12,
    nvme_cmd_io_mgmt_send = 0x1d,
};

struct _FdpNvme{
    int fd;
    uint16_t maxPIDLength;
    /* solesie: length of placementIDs == maxPIDLength */
    uint16_t *placementIDs;
    uint16_t nextPIDIdx;

    /* NVMe Data */
    int nsid;
    uint32_t maxTfrSize;
    uint32_t lbaShift;
    uint64_t startLba;
};

/* solesie: Validates NVMe block-device names using a POSIX regular expression.
 * On success, return 1
 * On failure, return 0 */
static int isValidNvmeDevice(const char* bdevName) {
    regex_t regex;
    int ret;
    const char* pattern = "^/dev/nvme\\d+n\\d+(p\\d+)?$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return 0;
    }

    /* Execute regex match */
    ret = regexec(&regex, bdevName, 0, NULL, 0);
    regfree(&regex);
    if(ret == 0){
        return 1;
    }
    return 0;
}

/* solesie:  Extracts the NVMe char device path from a block-device name.
 * On failure, return NULL.
 * Refer the paper "NVMe IO Passthru" */
static char* getNvmeCharDevice(const char *bdevName) {
    const char* p = bdevName;
    while (*p && !isdigit((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return NULL;
    }
    const char* devPos = p;

    /* Find the 'p' that marks the partition, if any */
    const char* pPos = strchr(devPos, 'p');
    size_t idLen = pPos ? (size_t)(pPos - devPos) : strlen(devPos);

    const char* prefix = "/dev/ng";
    size_t prefixLen = strlen(prefix);
    /* +1 for '\0' */
    size_t totalLen = prefixLen + idLen + 1;
    char* cdev = (char*)zmalloc(totalLen);
    if (!cdev) {
        return NULL;
    }

    /* Build the char device path */
    memcpy(cdev, prefix, prefixLen);
    memcpy(cdev + prefixLen, devPos, idLen);
    cdev[prefixLen + idLen] = '\0';

    return cdev;
}

/* solesie: Opens the NVMe character device corresponding to the given block-device.
 * On success, returns a non-negative file descriptor.
 * On failure, returns 0. */
static int openNvmeCharFile(const char* bdevName) {
    if (!isValidNvmeDevice(bdevName)) {
        return 0;
    }

    char* cdevName = getNvmeCharDevice(bdevName);
    if (!cdevName) {
        return 0;
    }

    int fd = open(cdevName, O_RDONLY);
    zfree(cdevName);
    return fd;
}

/* On success, returns 1.
 * On failure, returns 0. */
static int readFull(int fd, void *buf, size_t count, size_t *readSize) {
    *readSize = 0;
    char *p = buf;
    while (count > 0) {
        ssize_t n = read(fd, p + *readSize, count);
        if (n < 0) {
            return 0;
        }
        if (n == 0) {
            break;  /* EOF */
        }
        *readSize += n;
        count -= n;
    }
    return 1;
}

/* On success, return 1.
 * On failure, return 0. */
static int readFile(int fd, char **out, size_t *outSize,
               size_t numBytes /* = SIZE_MAX */) 
{
    const size_t initialAlloc = 1024 * 4;
    struct stat st;
    size_t cap;
    char *buf = NULL;
    size_t used = 0;

    if (fstat(fd, &st) < 0) {
        return 0;
    }

    if (st.st_size > 0) {
        cap = (size_t)st.st_size + 1;
        if (cap > numBytes) cap = numBytes;
    } else {
        cap = initialAlloc;
    }

    buf = (char*)zmalloc(cap);

    while (used < cap) {
        size_t got = 0;
        int flag = readFull(fd, buf + used, cap - used, &got);
        if (flag == 0) {
            zfree(buf);
            return 0;
        }
        used += got;
        if (got < cap - used) {
            break; /* EOF */
        }
        
        size_t next = cap * 3 / 2;
        if (next > numBytes) next = numBytes;
        if (next <= cap) {
            break;
        }
        char *tmp = zrealloc(buf, next);
        buf = tmp;
        cap = next;
    }

    if (used + 1 > cap) {
        char *tmp = zrealloc(buf, used + 1);
        buf = tmp;
        cap = used + 1;
    }
    buf[used] = '\0';

    *out = buf;
    *outSize = used;
    return 1;
}

/* solesie: Read the /sys/block/xx entry for any block device.
 * On success, return 1.
 * On failure, return 0. */
static int readDevAttr(const char *bname, const char *attr, char **out) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/sys/block/%s/%s", bname, attr);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    *out = NULL;
    size_t len = 0;
    if (!readFile(fd, out, &len, SIZE_MAX)) {
        close(fd);
        return 0;
    }
    close(fd);
    return 1;
}

/* solesie: Initialize Namespace ID, Max Transfer Size, LBA shift, and Start LBA of NVMe Device.
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeData(FdpNvme *fdpNvme, const char *nsName, const char *partName){
    char *nsidStr = NULL, *maxTfrSizeStr = NULL, *lbsStr = NULL, *partStartStr = NULL;
    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s%s%s",
         nsName, (partName && partName[0]) ? "/" : "",
         (partName && partName[0]) ? partName : "");


    int nsidFlag = readDevAttr(nsName, "nsid", &nsidStr);
    int maxTfrSizeFlag = readDevAttr(nsName, "queue/max_hw_sectors_kb", &maxTfrSizeStr);
    int lbsFlag = readDevAttr(nsName, "queue/logical_block_size", &lbsStr);
    int partStartFlag = readDevAttr(full, "start", &partStartStr);
    if(!nsidFlag || !maxTfrSizeFlag || !lbsFlag || !partStartFlag){
        zfree(nsidStr);
        zfree(maxTfrSizeStr);
        zfree(lbsStr);
        zfree(partStartStr);
        return 0;
    }

    fdpNvme->nsid = atoi(nsidStr);
    fdpNvme->maxTfrSize = strtoul(maxTfrSizeStr, NULL, 10) * 1024u;
    uint32_t lbs = strtoul(lbsStr, NULL, 10);
    uint32_t shift = 0;
    while ((1U << shift) < lbs) ++shift;
    fdpNvme->lbaShift = shift;
    uint64_t partStartBytes = strtoull(partStartStr, NULL, 10) * 512u;
    fdpNvme->startLba = partStartBytes >> shift;

    zfree(nsidStr);
    zfree(maxTfrSizeStr);
    zfree(lbsStr);
    zfree(partStartStr);
    return 1;
}

/* It returns nsName = "nvme0n1" for both "/dev/nvme0n1" and "/dev/nvme0n1p1".
 * Also partName = "nvme0n1p1" for partition, and "" otherwise.
 * 
 * On success, return 1.
 * On failure, return 0 */
static int getNsAndPartition(
    const char* bdevName,
    char* nsName,
    char* partName) {

    const char* base = strrchr(bdevName, '/');
    if (!base) {
        return 0;
    }
    base++;
    const char* p = strrchr(base, 'p');
    if (!p) {
        strcpy(nsName, base);
        partName[0] = '\0';
    } else {
        size_t len = p - base;
        strncpy(nsName, base, len);
        nsName[len] = '\0';
        strcpy(partName, base);
    }
    return 1;
}

/* solesie: Initialize the NVMe related info from a valid NVMe device path.
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeInfo(FdpNvme *fdpNvme, const char *bdevName) {
    char *nsName = zcalloc(PATH_MAX);
    char *partName = zcalloc(PATH_MAX);
    int flag = getNsAndPartition(bdevName, nsName, partName);
    if(!flag){
        goto error;
    }

    flag = initNvmeData(fdpNvme, nsName, partName);
    if(!flag){
        goto error;
    }

    zfree(nsName);
    zfree(partName);
    return 1;

error:
    zfree(nsName);
    zfree(partName);
    return 0;
}

/* NVMe IO Mnagement Receive fn for specific config reading */
static int nvmeIOMgmtRecv(
    int fd,
    uint32_t nsid,
    void *data,
    uint32_t data_len,
    uint8_t op,
    uint16_t op_specific) {
    
    /* Build the I/O management receive command
     * For further details on the CDB format, consult the specification
     * available as "TP4146 Flexible Data Placement 2022.11.30 Ratified"
     * in the following link:
     * https://nvmexpress.org/wp-content/uploads/NVM-Express-2.0-Ratified-TPs_20230111.zip */
    uint32_t cdw10 = (op & 0xf) | (op_specific & 0xff << 16);
    uint32_t cdw11 = (data_len >> 2) - 1; /* cdw11 is 0 based */

    struct nvme_passthru_cmd cmd = {
        .opcode = nvme_cmd_io_mgmt_recv,
        .nsid = nsid,
        .addr = (uint64_t)(uintptr_t)data,
        .data_len = data_len,
        .cdw10 = cdw10,
        .cdw11 = cdw11,
        .timeout_ms = NVME_DEFAULT_IOCTL_TIMEOUT,
    };

    return ioctl(fd, NVME_IOCTL_IO_CMD, &cmd);
}

/* solesie: Initialize the FDP specific information (i.e., Placment ID).
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeFdpStatus(FdpNvme *fdpNvme){
    struct RuhStatusDesc {
        uint16_t pid;
        uint16_t ruhid;
        uint32_t earutr;
        uint64_t ruamw;
        uint8_t rsvd16[16];
    };
      
    struct {
        struct {
            uint8_t  rsvd0[14];
            uint16_t nruhsd;
        } header;
        struct RuhStatusDesc *ruhsds;
    } ruhStatus;
    
    int err;

    /* solesie: First, read header. */
    err = nvmeIOMgmtRecv(
        fdpNvme->fd,
        fdpNvme->nsid, 
        &ruhStatus.header, 
        sizeof(ruhStatus.header), 
        NVME_IO_MGMT_RECV_RUH_STATUS, 
        0);
    if (err) {
        return 0;
    }

    ruhStatus.ruhsds = (struct RuhStatusDesc *)zmalloc(ruhStatus.header.nruhsd * sizeof(*ruhStatus.ruhsds));

    /* solesie: Second, read descriptor. */
    err = nvmeIOMgmtRecv(
        fdpNvme->fd,
        fdpNvme->nsid,
        ruhStatus.ruhsds,
        ruhStatus.header.nruhsd * sizeof(*ruhStatus.ruhsds),
        NVME_IO_MGMT_RECV_RUH_STATUS,
        0);
    if (err) {
        zfree(ruhStatus.ruhsds);
        return 0;
    }

    fdpNvme->maxPIDLength = ruhStatus.header.nruhsd;
    fdpNvme->placementIDs = (uint16_t*)zmalloc(fdpNvme->maxPIDLength * sizeof(*fdpNvme->placementIDs));
    for (int i = 0; i < fdpNvme->maxPIDLength; ++i) {
        fdpNvme->placementIDs[i] = ruhStatus.ruhsds[i].pid;
    }

    fdpNvme->nextPIDIdx = DEFAULT_PLACEMENT_ID_IDX + 1;

    zfree(ruhStatus.ruhsds);
    return 1;
}

/* solesie: Since the kernel does not support Flexible Data Placement (FDP), the host must perform data I/O 
 * using io_uring in accordance with the NVMe Flexible Data Placement protocol. 
 * Additionally, the host is responsible for directly retrieving information from the FDP SSD.
 * 
 * Creates an FdpNvme object that contains helper functions for constructing NVMe commands for data I/O, 
 * and automatically retrieves the FDP SSD's information upon creation.
 * 
 * @param bdevNvme Must follow the format (e.g., /dev/nvme0n1, /dev/nvme0n1p1). */
FdpNvme *fdpNvmeCreate(const char *bdevName){
    FdpNvme *fdpNvme = zcalloc(sizeof(*fdpNvme));

    int fd = openNvmeCharFile(bdevName);
    assert(fd >= 0);
    fdpNvme->fd = fd;

    int flag = initNvmeInfo(fdpNvme, bdevName);
    assert(flag);

    flag = initNvmeFdpStatus(fdpNvme);
    assert(flag);

    return fdpNvme;
}

void fdpNvmeRelease(FdpNvme *fdpNvme){
    close(fdpNvme->fd);
    zfree(fdpNvme->placementIDs);
    zfree(fdpNvme);
}

/* Allocates an FDP specific placement handle. 
 * This handle will be interpreted by the device for data placement.
 
 * @return The allocated FDP specific Placement Handle. */
int fdpNvmeAllocateFdpHandle(FdpNvme *fdpNvme) {
    uint16_t phndl;

    /* Get NS specific Fdp Placement Handle(PHNDL) */
    if (fdpNvme->nextPIDIdx < fdpNvme->maxPIDLength) {
        phndl = fdpNvme->nextPIDIdx++;
    } else {
        phndl = DEFAULT_PLACEMENT_ID_IDX;
    }

    return (int)phndl;
}

static void prepFdpUringCmdSqe(
    FdpNvme *fdpNvme,
    struct io_uring_sqe *sqe,
    void *buf,
    size_t size,
    off_t start,
    uint8_t opcode,
    uint8_t dtype,
    uint16_t dspec) {
    
    assert((fdpNvme->maxTfrSize == 0) || (size <= fdpNvme->maxTfrSize));
    /* Clear the SQE entry to avoid some arbitrary flags being set. */
    memset(sqe, 0, sizeof(*sqe));

    sqe.fd = fdpNvme->fd;
    sqe.opcode = IORING_OP_URING_CMD;
    sqe.cmd_op = NVME_URING_CMD_IO;

    struct nvme_uring_cmd *cmd = (struct nvme_uring_cmd*)&sqe.cmd;
    assert(cmd != NULL);
    memset(cmd, 0, sizeof(struct nvme_uring_cmd));
    cmd->opcode = opcode;

    /* start LBA of the IO = Req_start (offset in partition) + Partition_start */
    uint64_t sLba = (start >> fdpNvme->lbaShift) + fdpNvme->startLba;
    uint32_t nLb = (size >> fdpNvme->lbaShift) - 1; /* nLb is 0 based */

    /* cdw10 and cdw11 represent starting lba */
    cmd->cdw10 = sLba & 0xffffffff;
    cmd->cdw11 = sLba >> 32;
    /* cdw12 represent number of lba's for read/write */
    cmd->cdw12 = (dtype & 0xFF) << 20 | nLb;
    cmd->cdw13 = (dspec << 16);
    cmd->addr = (uint64_t)buf;
    cmd->data_len = size;

    cmd->nsid = fdpNvme->nsid;
}

void fdpNvmePrepReadUringCmdSqe(
    FdpNvme *fdpNvme,
    struct io_uring_sqe *sqe,
    void *buf,
    size_t size,
    off_t start) {
    
    prepFdpUringCmdSqe(fdpNvme, sqe, buf, size, start, nvme_cmd_read, 0, 0);
}

void fdpNvmePrepWriteUringCmdSqe(
    FdpNvme *fdpNvme,
    struct io_uring_sqe *sqe,
    void *buf, 
    size_t size, 
    off_t start, 
    int handle) {
    
    uint16_t pid;

    if (handle == -1) {
        pid = fdpNvme->placementIDs[DEFAULT_PLACEMENT_ID_IDX]; /* Use the default stream */
    } else if (handle >= 0 && handle < fdpNvme->maxPIDLength) {
        pid = fdpNvme->placementIDs[handle];
    } else {
        assert(false);
    }
    /* solesie: As Flexible Data Placement Specification, DTYPE should be 2. */
    prepFdpUringCmdSqe(fdpNvme, sqe, buf, size, start, nvme_cmd_write, 2, pid);
}

#endif