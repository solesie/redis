#ifndef REDIS_IOURING_DISABLE
#include "fdp_device.h"
#include "fdp_nvme.h"

#include "server.h"

#define min(a, b) (a) < (b) ? a : b

struct _FdpDevice{
    IOUring *ioUring;
    FdpNvme *fdpNvme;

    int fd;                 /* file descripters */
    size_t ioAlignmentSize;
    size_t maxIoSize;
    size_t maxWriteSize;
};

// typedef struct _IOReq{
//     int fd;                 /* file descripters */
//     OpType opType;
//     uint64_t offset;
//     size_t size;
//     uint8_t *data;
//     PlacementHandle placementHandle;

//     int is_req_successful;  /* 1 on success, 0 on failure */
// }IOReq;

FdpDevice *fdpDeviceCreate(
    CQHandlingMode cqHandlingMode,
    size_t qDepth,
    int fd, 
    size_t ioAlignmentSize, 
    size_t maxIoSize, 
    size_t maxWriteSize){
    if(ioAlignmentSize == 0 || maxWriteSize % ioAlignmentSize != 0 
        || maxIoSize % ioAlignmentSize != 0){
        serverLog(LOG_WARNING, "solesie: fdpDeviceCreate() parameter error\n");
        exit(1);
    }

    FdpDevice *fdpDevice = zcalloc(sizeof(*fdpDevice));
    
    if(cqHandlingMode == SYNC){
        fdpDevice->ioUring = ioUringCreate(SYNC, 1);
    }
    if(cqHandlingMode == EVENT){
        fdpDevice->ioUring = ioUringCreate(EVENT, qDepth);
    }

    fdpDevice->fdpNvme = createFdpNvme();

    fdpDevice->fd = fd;
    fdpDevice->ioAlignmentSize = ioAlignmentSize;
    fdpDevice->maxIoSize = maxIoSize;
    fdpDevice->maxWriteSize = maxWriteSize;
}
void fdpDeviceRelease(FdpDevice *fdpDevice){
    ioUringRelease(fdpDevice->ioUring);
    zfree(fdpDevice);
}

// /* solesie: return 1 on success */
// void submitIO(IOUring* ioUring, IOReq *req) {
// 	// Now submit IOOp

// 	// op.startTime_ = getSteadyClock();

// 	while (ioUring->numOutstanding >= ioUring->qDepth) {
// 	    // if (qDepth_ > 1) {
// 	    //     XLOG_EVERY_MS(ERR, 10000) << fmt::format(
// 	    //         "[{}] the number of outstanding requests {} exceeds the limit {}",
// 	    //         getName(), numOutstanding_, qDepth_);
// 	    // }
// 	    // Waiter waiter;
// 	    // waitList_.push_back(waiter);
// 	    // waiter.baton_.wait();
// 	}

// 	prepFdpNvmeIo(req);
// 	std::unique_ptr<folly::AsyncBaseOp> asyncOp;
// 	asyncOp = prepAsyncIo(op);
// 	asyncOp->setUserData(&op);
// 	asyncBase_->submit(asyncOp.release());

// 	// op.submitTime_ = getSteadyClock();

// 	ioUring->numOutstanding++;
// 	ioUring->numSubmitted++;

// 	if (ioUring->cqHandlingMode == SYNC) {
// 		// Wait completion synchronously if completion handler is not available.
// 		// i.e., when async io is used with non-epoll mode
// 		auto completed = asyncBase_->wait(1 /* minRequests */);
// 		handleCompletion(completed);
// 	}

// 	return true;
// }

// /* solesie: return 1 on success, -1 on failure */
// int fdpDeviceWrite(FdpDevice *fdpDevice, uint64_t offset, uint8_t* data, size_t size, uint16_t placementHandle){
//     size_t remainingSize = size;
//     size_t maxWriteSize = (fdpDevice->maxWriteSize == 0) ? remainingSize : fdpDevice->maxWriteSize;
//     int result = 1; 
//     while (remainingSize > 0) {
//         size_t writeSize = min(maxWriteSize, remainingSize);
        
//         if(offset % fdpDevice->ioAlignmentSize != 0 || writeSize % fdpDevice->ioAlignmentSize != 0){
//             serverLog(LOG_WARNING, "solesie: fdpDeviceWrite() ioAlignementSize error\n");
//             exit(1);
//         }

//         // auto timeBegin = getSteadyClock();
        
//         IOReq *req = zmalloc(sizeof(*req));
//         req->fd = fdpDevice->fd;
//         req->opType = WRITE;
//         req->offset = offset;
//         req->size = size;
//         req->data = data;
//         req->placementHandle = placementHandle;
//         req->is_req_successful = 1;
//         submitIO(fdpDevice->ioUring, req);
//         result = req->is_req_successful;

//         // writeLatencyEstimator_.trackValue(
//         //     toMicros((getSteadyClock() - timeBegin)).count());

//         // if (result) {
//         //     bytesWritten_.add(writeSize);
//         // } else {
//         //     // One part of the write failed so we abort the rest
//         //     break;
//         // }
//         offset += writeSize;
//         data += writeSize;
//         remainingSize -= writeSize;
//     }
//     if (!result) {
//         // writeIOErrors_.inc();
//     }
//     return result;
// }

// /* solesie: return 1 on success, -1 on failure */
// int fdpDeviceRead(FdpDevice *fdpDevice, uint64_t offset, uint8_t* data, size_t size){

// }

// void prepFdpUringCmdSqe(struct io_uring_sqe* sqe,
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
//         throw std::invalid_argument("Uring cmd is NULL!");
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

//     cmd->nsid = nvmeData_.nsId();
// }

// void prepWriteUringCmdSqe(
//     struct io_uring_sqe* sqe, void* buf, size_t size, off_t start, int handle) {
//     static constexpr uint8_t kPlacementMode = 2;
//     uint16_t pid;

//     if (handle == -1) {
//         pid = getFdpPID(kDefaultPIDIdx); // Use the default stream
//     } else if (handle >= 0 && handle <= maxPIDIdx_) {
//         pid = getFdpPID(static_cast<uint16_t>(handle));
//     } else {
//         throw std::invalid_argument("Invalid placement identifier");
//     }

//     prepFdpUringCmdSqe(sqe, buf, size, start, nvme_cmd_write, kPlacementMode,
//                         pid);
// }

#endif