#ifndef REDIS_IOURING_DISABLE
#include "fdp_nvme.h"
#include "server.h"

// Reference: https://github.com/axboe/fio/blob/master/engines/nvme.h
// If the uapi headers installed on the system lacks nvme uring command
// support, use the local version to prevent compilation issues.
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

enum nvme_io_opcode {
    nvme_cmd_write = 0x01,
    nvme_cmd_read = 0x02,
    nvme_cmd_io_mgmt_recv = 0x12,
    nvme_cmd_io_mgmt_send = 0x1d,
};

struct _FdpNvme{
    int fd;
    uint16_t maxPIDIdx;
    /* solesie: length of placementIDs == maxPIDIdx + 1 */
    uint16_t *placementIDs
};



FdpNvme *createFdpNvme(){
    FdpNvme *fdpNvme = zcalloc(sizeof(*fdpNvme));

    // solesie: TODO

    return fdpNvme;
}

void releaseFdpNvme(FdpNvme *fdpNvme){
    zfree(fdpNvme->placementIDs);
    zfree(fdpNvme);
}


// typedef struct _IOReq{
//     IOUringOp *op;
//     int fd;                 /* file descripters */
//     OpType opType;
//     uint64_t offset;
//     size_t size;
//     void *data;
//     uint16_t *placementHandle;

//     int is_req_successful;  /* 1 on success, 0 on failure */
// }IOReq;




// void prepFdpUringCmdSqe(
//     struct io_uring_sqe* sqe,
//     void* buf,
//     size_t size,
//     off_t start,
//     uint8_t opcode,
//     uint8_t dtype,
//     uint16_t dspec) {
//     uint32_t maxTfrSize = nvmeData_.getMaxTfrSize();
//     if ((maxTfrSize != 0) && (size > maxTfrSize)) {
//         throw std::invalid_argument("Exceeds max Transfer size");
//     }
//     // Clear the SQE entry to avoid some arbitrary flags being set.
//     memset(&sqe, 0, sizeof(struct io_uring_sqe));

//     sqe.fd = file_.fd();
//     sqe.opcode = IORING_OP_URING_CMD;
//     sqe.cmd_op = NVME_URING_CMD_IO;

//     struct nvme_uring_cmd* cmd = (struct nvme_uring_cmd*)&sqe.cmd;
//     if (cmd == nullptr) {
//     throw std::invalid_argument("Uring cmd is NULL!");
//     }
//     memset(cmd, 0, sizeof(struct nvme_uring_cmd));
//     cmd->opcode = opcode;

//     // start LBA of the IO = Req_start (offset in partition) + Partition_start
//     uint64_t sLba = (start >> nvmeData_.lbaShift()) + nvmeData_.partStartLba();
//     uint32_t nLb = (size >> nvmeData_.lbaShift()) - 1; // nLb is 0 based

//     /* cdw10 and cdw11 represent starting lba */
//     cmd->cdw10 = sLba & 0xffffffff;
//     cmd->cdw11 = sLba >> 32;
//     /* cdw12 represent number of lba's for read/write */
//     cmd->cdw12 = (dtype & 0xFF) << 20 | nLb;
//     cmd->cdw13 = (dspec << 16);
//     cmd->addr = (uint64_t)buf;
//     cmd->data_len = size;

// cmd->nsid = nvmeData_.nsId();
// }

// void prepFdpNvmeIo(FdpNvme *fdpNvme, IOReq *req) {
//     // std::unique_ptr<folly::IoUringOp> iouringCmdOp;
//     // IOReq& req = op.parent_;

//     // auto& options = static_cast<folly::IOUring*>(asyncBase_.get())->getOptions();
//     // iouringCmdOp = std::make_unique<folly::IoUringOp>(
//     //     folly::AsyncBaseOp::NotificationCallback(), options);

//     // iouringCmdOp->initBase();
//     // struct io_uring_sqe& sqe = iouringCmdOp->getSqe();

//     uint16_t pid;
//     if (req->opType == READ) {
//         prepFdpUringCmdSqe(&req->sqe, req->data, req->size, req->offset, nvme_cmd_read, 0, 0);
//     } else {
//         if(req->placementHandle == NULL) {
//             pid = fdpNvme->placementIDs[0];
//         } else if(0 <= req->placementHandle && req->placementHandle <= fdpNvme->maxPIDIdx) {
//             pid = fdpNvme->placementIDs[*req->placementHandle];
//         } else{
//             serverLog(LOG_WARNING, "solesie: invalid placement identifier");
//             exit(1);
//         }
//         /* solesie: As Flexible Data Placement Specification, DTYPE should be 2. */
//         prepFdpUringCmdSqe(&req->sqe, req->data, req->size, req->offset, nvme_cmd_write, 2, pid);
//     }

//     /* solesie: TODO: how to set user-defined data and user-defined CQ Handling? */
//     io_uring_sqe_set_data(&req->sqe, NULL);
//     // return std::move(iouringCmdOp);
//     return ;
// }

#endif