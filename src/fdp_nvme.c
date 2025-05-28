#ifndef REDIS_IOURING_DISABLE
#include <stdio.h>
#include <regex.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/nvme_ioctl.h>
#include "redisassert.h"
#include "zmalloc.h"
#include "fdp_nvme.h"

#define NVME_DEFAULT_IOCTL_TIMEOUT 0

#define DEFAULT_PLACEMENT_ID_IDX 0

enum nvme_io_mgmt_recv_mo {
    NVME_IO_MGMT_RECV_RUH_STATUS = 0x1,
};

enum nvme_io_opcode {
    nvme_cmd_write = 0x01,
    nvme_cmd_read = 0x02,
    nvme_cmd_id_ns = 0x06,
    nvme_cmd_io_mgmt_recv = 0x12,
    nvme_cmd_io_mgmt_send = 0x1d,
};

struct _fdpNvme{
    int fd;
    uint16_t max_pid_len;
    /* solesie: length of placement_id_arr == max_pid_len */
    uint16_t *placement_id_arr;
    uint16_t next_pid_idx;

    /* NVMe Data */
    int nsid;
    uint32_t max_tfr_size;
    uint32_t lba_shift;
    uint64_t start_lba;
    uint64_t end_lba;
    uint32_t preferred_write_size; /* Refer NVMe command spec Namespace Preferred Write Granularity */
    uint32_t max_segments; /* https://lpc.events/event/16/contributions/1382/attachments/1119/2151/LPC2022_uring-passthru.pdf */
};

/* solesie: Validates NVMe block-device names using a POSIX regular expression.
 * On success, return 1
 * On failure, return 0 */
static int isValidNvmeDevice(const char* bdev_name) {
    regex_t regex;
    int ret;
    /* ^/dev/nvme\\d+n\\d+(p\\d+)?$ */
    const char* pattern = "^/dev/nvme[0-9]+n[0-9]+(p[0-9]+)?$";

    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        return 0;
    }

    /* Execute regex match */
    ret = regexec(&regex, bdev_name, 0, NULL, 0);
    regfree(&regex);
    if(ret == 0){
        return 1;
    }
    return 0;
}

/* solesie:  Extracts the NVMe char device path from a block-device name.
 * On failure, return NULL.
 * Refer the paper "NVMe IO Passthru" */
static char* getNvmeCharDevice(const char *bdev_name) {
    const char* p = bdev_name;
    while (*p && !isdigit((unsigned char)*p)) {
        p++;
    }
    if (!*p) {
        return NULL;
    }
    const char* dev_pos = p;

    /* Find the 'p' that marks the partition, if any */
    const char* p_pos = strchr(dev_pos, 'p');
    size_t id_len = p_pos ? (size_t)(p_pos - dev_pos) : strlen(dev_pos);

    const char* prefix = "/dev/ng";
    size_t prefix_len = strlen(prefix);
    /* +1 for '\0' */
    size_t total_len = prefix_len + id_len + 1;
    char* cdev = (char*)zmalloc(total_len);
    if (!cdev) {
        return NULL;
    }

    /* Build the char device path */
    memcpy(cdev, prefix, prefix_len);
    memcpy(cdev + prefix_len, dev_pos, id_len);
    cdev[prefix_len + id_len] = '\0';

    return cdev;
}

/* solesie: Opens the NVMe character device corresponding to the given block-device.
 * On success, returns a non-negative file descriptor.
 * On failure, returns 0. */
static int openNvmeCharFile(const char* bdev_name) {
    if (!isValidNvmeDevice(bdev_name)) {
        return 0;
    }

    char* cdevName = getNvmeCharDevice(bdev_name);
    if (!cdevName) {
        return 0;
    }

    int fd = open(cdevName, O_RDONLY);
    zfree(cdevName);
    return fd;
}

/* On success, returns 1.
 * On failure, returns 0. */
static int readFull(int fd, void *buf, size_t count, size_t *read_size) {
    *read_size = 0;
    char *p = buf;
    while (count > 0) {
        ssize_t n = read(fd, p + *read_size, count);
        if (n < 0) {
            return 0;
        }
        if (n == 0) {
            break;  /* EOF */
        }
        *read_size += n;
        count -= n;
    }
    return 1;
}

/* On success, return 1.
 * On failure, return 0. */
static int readFile(int fd, char **out, size_t *out_size,
               size_t num_bytes /* = SIZE_MAX */) 
{
    const size_t initial_alloc = 1024 * 4;
    struct stat st;
    size_t cap;
    char *buf = NULL;
    size_t used = 0;

    if (fstat(fd, &st) < 0) {
        return 0;
    }

    if (st.st_size > 0) {
        cap = (size_t)st.st_size + 1;
        if (cap > num_bytes) cap = num_bytes;
    } else {
        cap = initial_alloc;
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
        if (next > num_bytes) next = num_bytes;
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
    *out_size = used;
    return 1;
}

/* solesie: Read the /sys/block/xx entry for any block device.
 * On success, return 1.
 * On failure, return 0. */
static int readDevAttr(const char *bname, const char *attr, char **out) {
    char path[PATH_MAX];
    memset(path, 0, sizeof(path));
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

/* NVMe IO Management Receive for specific config reading */
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

/* NVMe Identify-ns for specific config reading */
static int nvmeIdNs(
    int fd,
    uint32_t nsid,
    void *data,
    uint32_t data_len) {

    struct nvme_passthru_cmd cmd = {
        .opcode = nvme_cmd_id_ns,
        .nsid = nsid,
        .addr = (uint64_t)(uintptr_t)data,
        .data_len = data_len,
        .cdw10 = 0,
        .timeout_ms = NVME_DEFAULT_IOCTL_TIMEOUT,
    };

    return ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd);
}

/* solesie: Initialize data of NVMe Device.
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeData(fdpNvme *fdp_nvme, const char *ns_name, const char *part_name){
    char *nsid_str = NULL, *max_tfr_size_str = NULL, *lbs_str = NULL, *part_start_str = NULL
        , *ms_str = NULL, *size_str;
    char full[PATH_MAX];
    memset(full, 0, sizeof(full));
    snprintf(full, sizeof(full), "%s%s%s",
         ns_name, (part_name && part_name[0]) ? "/" : "",
         (part_name && part_name[0]) ? part_name : "");


    int nsid_flag = readDevAttr(ns_name, "nsid", &nsid_str);
    int max_tfr_size_flag = readDevAttr(ns_name, "queue/max_hw_sectors_kb", &max_tfr_size_str);
    int lba_flag = readDevAttr(ns_name, "queue/logical_block_size", &lbs_str);
    int ms_flag = readDevAttr(ns_name, "queue/max_segments", &ms_str);
    int part_start_flag, size_flag;
    if(part_name && part_name[0]){
        part_start_flag = readDevAttr(full, "start", &part_start_str);
        size_flag = readDevAttr(full, "size", &size_str);
    } else{
        part_start_flag = 1;
        size_flag = readDevAttr(full, "size", &size_str);
    }
    if(!nsid_flag || !max_tfr_size_flag || !lba_flag || !part_start_flag || !ms_flag || !size_flag){
        zfree(nsid_str);
        zfree(max_tfr_size_str);
        zfree(lbs_str);
        zfree(part_start_str);
        zfree(ms_str);
        zfree(size_str);
        return 0;
    }

    fdp_nvme->nsid = atoi(nsid_str);
    fdp_nvme->max_tfr_size = strtoul(max_tfr_size_str, NULL, 10) * 1024u;
    uint32_t lbs = strtoul(lbs_str, NULL, 10);
    uint32_t shift = 0;
    while ((1U << shift) < lbs) ++shift;
    fdp_nvme->lba_shift = shift;
    uint64_t part_start_bytes = 0; 
    if(part_name && part_name[0]){
        part_start_bytes = strtoull(part_start_str, NULL, 10) * 512u;
    }
    fdp_nvme->start_lba = part_start_bytes >> shift;
    fdp_nvme->end_lba = (strtoull(size_str, NULL, 10) * 512u) >> shift;
    fdp_nvme->max_segments = strtoul(ms_str, NULL, 10);

    zfree(nsid_str);
    zfree(max_tfr_size_str);
    zfree(lbs_str);
    zfree(part_start_str);
    zfree(ms_str);
    zfree(size_str);

    struct {
        uint8_t rsvd23[24];
        uint8_t nsfeat;
        uint8_t rsvd64[39];
        uint16_t npwg;
    } id_ns_data;
    int err = nvmeIdNs(
        fdp_nvme->fd,
        fdp_nvme->nsid,
        &id_ns_data,
        sizeof(id_ns_data));
    if (err) {
        return 0;
    }
    fdp_nvme->preferred_write_size = lbs;
    if(id_ns_data.nsfeat & (1 << 4)){
        fdp_nvme->preferred_write_size = (id_ns_data.npwg + 1) * lbs;
    }

    return 1;
}

/* It returns ns_name = "nvme0n1" for both "/dev/nvme0n1" and "/dev/nvme0n1p1".
 * Also part_name = "nvme0n1p1" for partition, and "" otherwise.
 * 
 * On success, return 1.
 * On failure, return 0 */
static int getNsAndPartition(
    const char* bdev_name,
    char* ns_name,
    char* part_name) {

    const char* base = strrchr(bdev_name, '/');
    if (!base) {
        return 0;
    }
    base++;
    const char* p = strrchr(base, 'p');
    if (!p) {
        strcpy(ns_name, base);
        part_name[0] = '\0';
    } else {
        size_t len = p - base;
        strncpy(ns_name, base, len);
        ns_name[len] = '\0';
        strcpy(part_name, base);
    }
    return 1;
}

/* solesie: Initialize the NVMe related info from a valid NVMe device path.
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeInfo(fdpNvme *fdp_nvme, const char *bdev_name) {
    char *ns_name = zcalloc(PATH_MAX);
    char *part_name = zcalloc(PATH_MAX);
    int flag = getNsAndPartition(bdev_name, ns_name, part_name);
    if(!flag){
        goto error;
    }

    flag = initNvmeData(fdp_nvme, ns_name, part_name);
    if(!flag){
        goto error;
    }

    zfree(ns_name);
    zfree(part_name);
    return 1;

error:
    zfree(ns_name);
    zfree(part_name);
    return 0;
}

/* solesie: Initialize the FDP specific information (i.e., Placment ID).
 * On success, return 1.
 * On failure, return 0. */
static int initNvmeFdpStatus(fdpNvme *fdp_nvme){
    struct ruhStatusDesc {
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
        struct ruhStatusDesc ruhsds[1<<16];
    } ruh_status;
    
    int err;

    /* solesie: First, read header. */
    err = nvmeIOMgmtRecv(
        fdp_nvme->fd,
        fdp_nvme->nsid, 
        &ruh_status.header, 
        sizeof(ruh_status.header), 
        NVME_IO_MGMT_RECV_RUH_STATUS, 
        0);
    if (err) {
        return 0;
    }

    /* solesie: Second, read descriptor. */
    err = nvmeIOMgmtRecv(
        fdp_nvme->fd,
        fdp_nvme->nsid,
        &ruh_status,
        sizeof(ruh_status.header) + ruh_status.header.nruhsd * sizeof(*ruh_status.ruhsds),
        NVME_IO_MGMT_RECV_RUH_STATUS,
        0);
    if (err) {
        return 0;
    }

    fdp_nvme->max_pid_len = ruh_status.header.nruhsd;
    fdp_nvme->placement_id_arr = (uint16_t*)zmalloc(fdp_nvme->max_pid_len * sizeof(*fdp_nvme->placement_id_arr));
    for (int i = 0; i < fdp_nvme->max_pid_len; ++i) {
        fdp_nvme->placement_id_arr[i] = ruh_status.ruhsds[i].pid;
    }

    fdp_nvme->next_pid_idx = DEFAULT_PLACEMENT_ID_IDX + 1;

    return 1;
}

/* solesie: Since the kernel does not support Flexible Data Placement (FDP), the host must perform data I/O 
 * using io_uring in accordance with the NVMe Flexible Data Placement protocol. 
 * Additionally, the host is responsible for directly retrieving information from the FDP SSD.
 * 
 * Creates an fdpNvme object that contains helper functions for constructing NVMe commands for data I/O, 
 * and automatically retrieves the FDP SSD's information upon creation.
 * 
 * @param bdevNvme Must follow the format (e.g., /dev/nvme0n1, /dev/nvme0n1p1). */
fdpNvme *fdpNvmeCreate(const char *bdev_name){
    fdpNvme *fdp_nvme = zcalloc(sizeof(*fdp_nvme));

    int fd = openNvmeCharFile(bdev_name);
    assert(fd > 0);
    fdp_nvme->fd = fd;

    int flag = initNvmeInfo(fdp_nvme, bdev_name);
    assert(flag);

    flag = initNvmeFdpStatus(fdp_nvme);
    assert(flag);

    return fdp_nvme;
}

void fdpNvmeRelease(fdpNvme *fdp_nvme){
    close(fdp_nvme->fd);
    zfree(fdp_nvme->placement_id_arr);
    zfree(fdp_nvme);
}

/* Allocates an FDP specific placement handle. 
 * This handle will be interpreted by the device for data placement.
 
 * @return The allocated FDP specific Placement Handle. */
int fdpNvmeAllocateFdpHandle(fdpNvme *fdp_nvme) {
    uint16_t phndl;

    /* Get NS specific Fdp Placement Handle(PHNDL) */
    if (fdp_nvme->next_pid_idx < fdp_nvme->max_pid_len) {
        phndl = fdp_nvme->next_pid_idx++;
    } else {
        phndl = DEFAULT_PLACEMENT_ID_IDX;
    }

    return (int)phndl;
}

static void prepFdpUringCmdSqe(
    fdpNvme *fdp_nvme,
    struct io_uring_sqe *sqe,
    const void *buf,
    size_t size,
    off_t start,
    uint8_t opcode,
    uint8_t dtype,
    uint16_t dspec) {
    
    assert((fdp_nvme->max_tfr_size == 0) || (size <= fdp_nvme->max_tfr_size));
    /* Clear the SQE entry to avoid some arbitrary flags being set. */
    memset(sqe, 0, sizeof(*sqe));

    sqe->fd = fdp_nvme->fd;
    sqe->opcode = IORING_OP_URING_CMD;
    sqe->cmd_op = NVME_URING_CMD_IO;

    struct nvme_uring_cmd *cmd = (struct nvme_uring_cmd*)&sqe->cmd;
    assert(cmd != NULL);
    memset(cmd, 0, sizeof(struct nvme_uring_cmd));
    cmd->opcode = opcode;

    /* start LBA of the IO = Req_start (offset in partition) + Partition_start */
    uint64_t slba = (start >> fdp_nvme->lba_shift) + fdp_nvme->start_lba;
    uint32_t nlb = (size >> fdp_nvme->lba_shift) - 1; /* nlb is 0 based */

    /* cdw10 and cdw11 represent starting lba */
    cmd->cdw10 = slba & 0xffffffff;
    cmd->cdw11 = slba >> 32;
    /* cdw12 represent number of lba's for read/write */
    cmd->cdw12 = (dtype & 0xFF) << 20 | nlb;
    cmd->cdw13 = (dspec << 16);
    cmd->addr = (uint64_t)buf;
    cmd->data_len = size;

    cmd->nsid = fdp_nvme->nsid;
}

void fdpNvmePrepReadUringCmdSqe(
    fdpNvme *fdp_nvme,
    struct io_uring_sqe *sqe,
    void *buf,
    size_t size,
    off_t start) {
    
    prepFdpUringCmdSqe(fdp_nvme, sqe, buf, size, start, nvme_cmd_read, 0, 0);
}

void fdpNvmePrepWriteUringCmdSqe(
    fdpNvme *fdp_nvme,
    struct io_uring_sqe *sqe,
    const void *buf, 
    size_t size, 
    off_t start, 
    int handle) {
    
    uint16_t pid;

    if (handle == -1) {
        pid = fdp_nvme->placement_id_arr[DEFAULT_PLACEMENT_ID_IDX]; /* Use the default stream */
    } else if (handle >= 0 && handle < fdp_nvme->max_pid_len) {
        pid = fdp_nvme->placement_id_arr[handle];
    } else {
        assert(false);
    }
    /* solesie: As Flexible Data Placement Specification, DTYPE should be 2. */
    prepFdpUringCmdSqe(fdp_nvme, sqe, buf, size, start, nvme_cmd_write, 2, pid);
}

uint32_t fdpNvmeGetMaxIOSize(fdpNvme *fdp_nvme){
    uint32_t segLimit = fdp_nvme->max_segments * (1 << fdp_nvme->lba_shift);
    return segLimit < fdp_nvme->max_tfr_size ? segLimit : fdp_nvme->max_tfr_size;
}

uint16_t fdpNvmeGetMaxPIDLength(fdpNvme *fdp_nvme){
    return fdp_nvme->max_pid_len;
}

uint32_t fdpNvmeGetPreferredWriteSize(fdpNvme *fdp_nvme){
    return fdp_nvme->preferred_write_size;
}

uint32_t fdpNvmeGetLbSize(fdpNvme *fdp_nvme){
    return 1 << fdp_nvme->lba_shift;
}

uint32_t fdpNvmeGetLbaShift(fdpNvme *fdp_nvme){
    return fdp_nvme->lba_shift;
}

uint64_t fdpNvmeGetStartLba(fdpNvme *fdp_nvme){
    return fdp_nvme->start_lba;
}

uint64_t fdpNvmeGetEndLba(fdpNvme *fdp_nvme){
    return fdp_nvme->end_lba;
}

#endif