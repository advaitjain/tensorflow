/* Copyright 2020 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef TENSORFLOW_COMPILER_XLA_SERVICE_GPU_NCCL_UTILS_H_
#define TENSORFLOW_COMPILER_XLA_SERVICE_GPU_NCCL_UTILS_H_

#include <memory>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/synchronization/mutex.h"
#if GOOGLE_CUDA
#include "third_party/nccl/nccl.h"
#elif TENSORFLOW_USE_ROCM
#include "rocm/include/rccl/rccl.h"
#endif
#include "tensorflow/compiler/xla/service/collective_ops_utils.h"
#include "tensorflow/compiler/xla/service/gpu/gpu_executable_run_options.h"
#include "tensorflow/compiler/xla/status.h"
#include "tensorflow/compiler/xla/statusor.h"
#include "tensorflow/compiler/xla/xla_data.pb.h"

#if TENSORFLOW_USE_ROCM
// Local hipify of cuda symbols
#define cudaError_t hipError_t
#define cudaStream_t hipStream_t
#define cudaGetErrorString hipGetErrorString
#define cudaGetDevice hipGetDevice
#define cudaSetDevice hipSetDevice
#define cudaSuccess hipSuccess
#endif

namespace xla {
namespace gpu {

ncclRedOp_t ToNcclReduction(ReductionKind kind);
StatusOr<ncclDataType_t> ToNcclDataType(PrimitiveType element_type);

bool IsGlobalNcclConfig();

Status ToStatus(ncclResult_t s, const char* file, int64 line, const char* expr);
Status ToStatus(cudaError_t s, const char* file, int64 line, const char* expr);

// Macros to return or warn on CUDA/NCCL errors.  (The same macro works for both
// NCCL and CUDA errors.)
//
// It's tempting to say these macros belong in an XLA header somewhere, but in
// practice we don't do much direct-to-CUDA-API stuff outside of this file.
#define XLA_CUDA_STATUS(expr) \
  ::xla::gpu::ToStatus(expr, __FILE__, __LINE__, #expr)

#define XLA_CUDA_RETURN_IF_ERROR(expr) \
  do {                                 \
    Status s = XLA_CUDA_STATUS(expr);  \
    if (!s.ok()) {                     \
      return s;                        \
    }                                  \
  } while (0)

#define XLA_CUDA_WARN_IF_ERROR(expr)  \
  do {                                \
    Status s = XLA_CUDA_STATUS(expr); \
    if (!s.ok()) {                    \
      LOG(ERROR) << s.ToString();     \
    }                                 \
  } while (0)

// RAII type for NCCL communicators.
using NcclComm = std::unique_ptr<ncclComm, void (*)(ncclComm_t)>;

// Owns a clique of NCCL comms which can be used for collective operations among
// a particular set of GPUs.
//
// Note that if you want to do a collective operation among a subset of these
// GPUs, you'll need a different clique.
class NcclClique {
 public:
  explicit NcclClique(
      absl::flat_hash_map<int, NcclComm> comms_by_device_ordinal);

  ncclComm_t GetCommForDeviceOrdinal(int device_ordinal) const;
  absl::Mutex* mu() { return &mu_; }

 private:
  absl::flat_hash_map<int, NcclComm> comms_by_device_ordinal_;
  absl::Mutex mu_;
};

struct LocalParticipant {
  int device_ordinal;
  int rank;
};

StatusOr<std::vector<LocalParticipant>> GetLocalParticipants(
    const std::vector<GlobalDeviceId>& participants,
    const std::vector<GlobalDeviceId>* local_devices);  // may be null

struct LockedNcclClique {
  std::shared_ptr<NcclClique> clique;
  // Must come after clique, so it is destroyed first.
  // This lock prevents other threads from using this clique. All of the threads
  // involved should hold onto the lock until they have finished with their
  // communicator.
  std::shared_ptr<absl::MutexLock> lock;
};

// Acquires a locked NCCL clique for use in NCCL collective operations.
StatusOr<LockedNcclClique> AcquireNcclClique(
    const RendezvousKey& rendezvous_key, int local_device_ordinal,
    se::Stream* stream, const std::vector<LocalParticipant>& local_participants,
    const NcclUniqueIdCallback* callback);  // may be null

// Gets the set of devices that have a NCCL channel open.  This is primarily
// for testing.
//
// (Indeed, because the NCCL channels are a global variable, in the real
// world, the value returned here is stale as soon as you read it, so it's not
// clear how you *could* use it for anything other than tests.)
absl::flat_hash_set<GlobalDeviceId> DevicesWithOpenNcclChannels();

}  // namespace gpu
}  // namespace xla

#endif  // TENSORFLOW_COMPILER_XLA_SERVICE_GPU_NCCL_UTILS_H_
