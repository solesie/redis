#ifndef REDIS_IOURING_DISABLE
#include "io_uring.h"
#include "zmalloc.h"
#include "server.h"
#include "fdp_nvme.h"
#include "atomicvar.h"

// http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2
static uint32_t roundUpToNextPowerOfTwo(uint32_t num) {
    if (num == 0) {
        return 0;
    }
    num--;
    num |= num >> 1;
    num |= num >> 2;
    num |= num >> 4;
    num |= num >> 8;
    num |= num >> 16;
    return num + 1;
}

struct _IoUring{
    struct io_uring ioRing;
	struct io_uring_params params;

	uint32_t qDepth;

	IOUringOpOptions options;

	redisAtomic size_t pending;
	redisAtomic size_t submitted;
	/* solesie: If IOUring handles CQ on EVENT-BASED, poolFd means CQ fd.
	 * Otherwise(i.e., CQ on SYNC_BASED), poolFd is -1. */
	int poolFd;

	IOUringOp *completedOps;
	size_t completedOpsLen;

	IOUringOp *canceledOps;
	size_t canceledOpsLen;
};

static int isValidIOUringOp(IOUring *ioUring, IOUringOp *op){
	if(ioUring->options.isSqe128 == op->options.isSqe128 && ioUring->options.isCqe32 == op->options.isCqe32){
		return 1;
	}
	return 0;
}

IOUring *ioUringCreate(int is_fdp, uint32_t qDepth){
	
    IOUring *ioUring = zcalloc(sizeof(*ioUring));

	ioUring->qDepth = qDepth;

    if(is_fdp){
		/* solesie: Please refer to the paper on "NVMe I/O passthrough". */
        ioUring->params.flags |= IORING_SETUP_SQE128;
        ioUring->params.flags |= IORING_SETUP_CQE32;
		ioUring->options.isSqe128 = 1;
		ioUring->options.isCqe32 = 1;
    }

    ioUring->params.flags |= IORING_SETUP_CQSIZE;
    ioUring->params.cq_entries = roundUpToNextPowerOfTwo(qDepth);

    int rc = io_uring_queue_init_params(
        roundUpToNextPowerOfTwo(qDepth), &ioUring->ioRing, &ioUring->params);
    if(rc < 0){
        serverLog(LOG_WARNING, "solesie: io_uring_queue_init error");
        exit(1);
    }

    return ioUring;
}

void ioUringRelease(IOUring *ioUring){
    zfree(ioUring);
}

/* solesie: >= 0 return on success, < 0 return on failure */
int submitOne(IOUring *ioUring, IOUringOp *op){
	if(!isValidIOUringOp(ioUring, op)){
		return -1;
	}

	struct io_uring_sqe* sqe = io_uring_get_sqe(&ioUring->ioRing);
	size_t sqeSize;
	if (!sqe) {
		return -1;
	}
	
	if(op->options.isSqe128){
		sqeSize = 128;
	} else{
		sqeSize = sizeof(struct io_uring_sqe);
	}
	memcpy(sqe, &op->sqe_.sqe, sqeSize);

	return io_uring_submit(&ioUring->ioRing);
}

// void doWait(
//     WaitType type,
//     size_t minRequests,
//     size_t maxRequests,
//     std::vector<AsyncBase::Op*>& result) {
//   result.clear();

//   size_t count = 0;
//   while (count < maxRequests) {
//     struct io_uring_cqe* cqe = nullptr;
//     if (!io_uring_peek_cqe(&ioRing_, &cqe) && cqe) {
//       count++;
//       Op* op = reinterpret_cast<Op*>(io_uring_cqe_get_data(cqe));
//       CHECK(op);
//       auto res = cqe->res;
//       op->setCqe(cqe);
//       io_uring_cqe_seen(&ioRing_, cqe);
//       decrementPending();
//       switch (type) {
//         case WaitType::COMPLETE:
//           op->complete(res);
//           break;
//         case WaitType::CANCEL:
//           op->cancel();
//           break;
//       }
//       result.push_back(op);
//     } else {
//       if (count < minRequests) {
//         io_uring_wait_cqe(&ioRing_, &cqe);
//       } else {
//         break;
//       }
//     }
//   }

//   return range(result);
// }


#endif