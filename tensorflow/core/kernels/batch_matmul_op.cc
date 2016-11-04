/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

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

// See docs in ../ops/math_ops.cc.

#define EIGEN_USE_THREADS

#include <vector>
#include "third_party/eigen3/unsupported/Eigen/CXX11/Tensor"
#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/register_types.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/type_traits.h"
#include "tensorflow/core/framework/types.h"
#include "tensorflow/core/kernels/fill_functor.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/util/work_sharder.h"

#if GOOGLE_CUDA
#include "tensorflow/core/platform/stream_executor.h"
#endif  // GOOGLE_CUDA

namespace tensorflow {

typedef Eigen::ThreadPoolDevice CPUDevice;
typedef Eigen::GpuDevice GPUDevice;

namespace {

// Inner kernel for multiplying a batch of matrices.
template <typename In, typename Out, bool IsComplex = true>
struct InnerBatchMatMulKernel {
  template <typename Device, typename Tx, typename Ty, typename Tz>
  static void Contract(const Device& d, Tx x, Ty y, Tz z,
                       const Eigen::array<Eigen::IndexPair<Eigen::DenseIndex>,
                                          1>& contract_pairs) {
    z.device(d) = x.contract(y, contract_pairs);
  }

  template <typename Device, typename Tz>
  static void Conjugate(const Device& d, Tz z) {
    z.device(d) = z.conjugate();
  }

  static void Run(const OpKernelContext* context, bool parallelize_inner, In Tx,
                  In Ty, bool adj_x, bool adj_y, Out Tz, int start, int limit) {
    static_assert(IsComplex, "Complex type expected.");
    Eigen::DefaultDevice default_device;
    const Eigen::ThreadPoolDevice thread_pool_device =
        context->eigen_cpu_device();

    // We use the identities
    //   conj(a) * conj(b) = conj(a * b)
    //   conj(a) * b = conj(a * conj(b))
    // to halve the number of cases. The final conjugation of the result is
    // done at the end of LaunchBatchMatMul<CPUDevice, Scalar>::Launch().
    Eigen::array<Eigen::IndexPair<Eigen::DenseIndex>, 1> contract_pairs;
    if (!adj_x && !adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(1, 0);
    } else if (adj_x && adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(0, 1);
    } else if (!adj_x && adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(1, 1);
    } else {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(0, 0);
    }

    for (int i = start; i < limit; ++i) {
      auto x = Tx.template chip<0>(i);
      auto z = Tz.template chip<0>(i);
      if (adj_x != adj_y) {
        auto y = Ty.template chip<0>(i).conjugate();
        if (parallelize_inner) {
          Contract(thread_pool_device, x, y, z, contract_pairs);
        } else {
          Contract(default_device, x, y, z, contract_pairs);
        }
      } else {
        auto y = Ty.template chip<0>(i);
        if (parallelize_inner) {
          Contract(thread_pool_device, x, y, z, contract_pairs);
        } else {
          Contract(default_device, x, y, z, contract_pairs);
        }
      }
    }
  }
};

// The Eigen contraction kernel used here is very large and slow to compile,
// so we specialize InnerBatchMatMulKernel for real types to avoid all but
// one of the instantiations.
template <typename In, typename Out>
struct InnerBatchMatMulKernel<In, Out, false> {
  template <typename Device, typename Txy, typename Tz>
  static void Contract(const Device& d, Txy x, Txy y, Tz z,
                       const Eigen::array<Eigen::IndexPair<Eigen::DenseIndex>,
                                          1>& contract_pairs) {
    z.device(d) = x.contract(y, contract_pairs);
  }

  template <typename Device, typename Tz>
  static void Conjugate(const Device& d, Tz z) {}

  static void Run(const OpKernelContext* context, bool parallelize_inner, In Tx,
                  In Ty, bool adj_x, bool adj_y, Out Tz, int start, int limit) {
    Eigen::DefaultDevice default_device;
    const Eigen::ThreadPoolDevice thread_pool_device =
        context->eigen_cpu_device();
    Eigen::array<Eigen::IndexPair<Eigen::DenseIndex>, 1> contract_pairs;
    if (!adj_x && !adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(1, 0);
    } else if (adj_x && adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(0, 1);
    } else if (!adj_x && adj_y) {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(1, 1);
    } else {
      contract_pairs[0] = Eigen::IndexPair<Eigen::DenseIndex>(0, 0);
    }

    for (int i = start; i < limit; ++i) {
      auto x = Tx.template chip<0>(i);
      auto y = Ty.template chip<0>(i);
      auto z = Tz.template chip<0>(i);
      if (parallelize_inner) {
        Contract(thread_pool_device, x, y, z, contract_pairs);
      } else {
        Contract(default_device, x, y, z, contract_pairs);
      }
    }
  }
};

}  // namespace

template <typename Device, typename Scalar>
struct LaunchBatchMatMul;

template <typename Scalar>
struct LaunchBatchMatMul<CPUDevice, Scalar> {
  static void Launch(OpKernelContext* context, const Tensor& in_x,
                     const Tensor& in_y, bool adj_x, bool adj_y, Tensor* out) {
    typedef typename TTypes<Scalar, 3>::ConstTensor In;
    typedef typename TTypes<Scalar, 3>::Tensor Out;
    typedef InnerBatchMatMulKernel<In, Out, Eigen::NumTraits<Scalar>::IsComplex>
        Kernel;

    In Tx = in_x.tensor<Scalar, 3>();
    In Ty = in_y.tensor<Scalar, 3>();
    Out Tz = out->tensor<Scalar, 3>();

    // Number of matrix multiplies i.e. size of the batch.
    const int64 num_units = in_x.dim_size(0);
    const int64 cost_per_unit =
        in_x.dim_size(1) * in_x.dim_size(2) * out->dim_size(2);

    // For large matrix products it is counter-productive to parallelize
    // over the batch dimension.
    const int64 kMaxCostOuterParallelism = 128 * 256 * 256;  // heuristic.
    bool parallelize_inner = true;
    if (num_units == 1 ||
        (cost_per_unit > kMaxCostOuterParallelism && out->dim_size(2) > 1)) {
      Kernel::Run(context, parallelize_inner, Tx, Ty, adj_x, adj_y, Tz, 0,
                  num_units);
    } else {
      auto worker_threads =
          *(context->device()->tensorflow_cpu_worker_threads());
      int num_threads = worker_threads.num_threads;
      // For small matrices and large batches, it is counter-productive
      // to parallelize the inner matrix multiplies.
      parallelize_inner =
          num_threads > num_units && out->dim_size(2) > 1;  // heuristic.

      // TODO(rmlarsen): The parallelized contraction in Eigen can deadlock when
      // running num_threads or more contractions in parallel. Launch on all
      // worker_threads.num_threads threads here once that is fixed.
      const int num_outer_threads =
          parallelize_inner ? std::max(1, num_threads - 1) : num_threads;

      Shard(num_outer_threads, worker_threads.workers, num_units, cost_per_unit,
            [context, parallelize_inner, &Tx, &Ty, adj_x, adj_y, &Tz](
                int start, int limit) {
              Kernel::Run(context, parallelize_inner, Tx, Ty, adj_x, adj_y, Tz,
                          start, limit);
            });
    }

    // We used the identities
    //   conj(a) * conj(b) = conj(a * b)
    //   conj(a) * b = conj(a * conj(b))
    // to reduce code size of InnerBatchMatMulKernel, so for some cases
    // we need to conjugate the final output. This is a no-op for non-complex
    // types.
    if (adj_x) {
      Kernel::Conjugate(context->eigen_cpu_device(), Tz);
    }
  }
};

#if GOOGLE_CUDA

namespace {
template <typename T>
perftools::gputools::DeviceMemory<T> AsDeviceMemory(const T* cuda_memory) {
  perftools::gputools::DeviceMemoryBase wrapped(const_cast<T*>(cuda_memory));
  perftools::gputools::DeviceMemory<T> typed(wrapped);
  return typed;
}

class CublasScratchAllocator : public perftools::gputools::ScratchAllocator {
 public:
  using Stream = ::perftools::gputools::Stream;
  using DeviceMemoryBytes = ::perftools::gputools::DeviceMemory<uint8>;

  CublasScratchAllocator(OpKernelContext* context) : context_(context) {}

  int64 GetMemoryLimitInBytes(Stream* stream) override { return -1; }

  perftools::gputools::port::StatusOr<DeviceMemoryBytes> AllocateBytes(
      Stream* stream, int64 byte_size) override {
    Tensor temporary_memory;

    Status allocation_status(context_->allocate_temp(
        DT_UINT8, TensorShape({byte_size}), &temporary_memory));
    if (!allocation_status.ok()) {
      return perftools::gputools::port::StatusOr<DeviceMemoryBytes>(
          DeviceMemoryBytes::MakeFromByteSize(nullptr, 0));
    }
    // Hold the reference of the allocated tensors until the end of the
    // allocator.
    allocated_tensors_.push_back(temporary_memory);
    return perftools::gputools::port::StatusOr<DeviceMemoryBytes>(
        DeviceMemoryBytes::MakeFromByteSize(
            temporary_memory.flat<uint8>().data(),
            temporary_memory.flat<uint8>().size()));
  }

 private:
  OpKernelContext* context_;
  std::vector<Tensor> allocated_tensors_;
};
}  // namespace

template <typename Scalar>
struct LaunchBatchMatMul<GPUDevice, Scalar> {
  static void Launch(OpKernelContext* context, const Tensor& in_x,
                     const Tensor& in_y, bool adj_x, bool adj_y, Tensor* out) {
    constexpr perftools::gputools::blas::Transpose kTranspose =
        is_complex<Scalar>::value
            ? perftools::gputools::blas::Transpose::kConjugateTranspose
            : perftools::gputools::blas::Transpose::kTranspose;
    perftools::gputools::blas::Transpose trans[] = {
        perftools::gputools::blas::Transpose::kNoTranspose, kTranspose};
    const uint64 m = in_x.dim_size(adj_x ? 2 : 1);
    const uint64 k = in_x.dim_size(adj_x ? 1 : 2);
    const uint64 n = in_y.dim_size(adj_y ? 1 : 2);
    const uint64 batch_size = in_x.dim_size(0);
    auto blas_transpose_a = trans[adj_x];
    auto blas_transpose_b = trans[adj_y];

    auto* stream = context->op_device_context()->stream();
    OP_REQUIRES(context, stream, errors::Internal("No GPU stream available."));

    typedef perftools::gputools::DeviceMemory<Scalar> DeviceMemoryType;
    std::vector<DeviceMemoryType> a_device_memory;
    std::vector<DeviceMemoryType> b_device_memory;
    std::vector<DeviceMemoryType> c_device_memory;
    std::vector<DeviceMemoryType*> a_ptrs;
    std::vector<DeviceMemoryType*> b_ptrs;
    std::vector<DeviceMemoryType*> c_ptrs;
    a_device_memory.reserve(batch_size);
    b_device_memory.reserve(batch_size);
    c_device_memory.reserve(batch_size);
    a_ptrs.reserve(batch_size);
    b_ptrs.reserve(batch_size);
    c_ptrs.reserve(batch_size);
    auto* a_base_ptr = in_x.template flat<Scalar>().data();
    auto* b_base_ptr = in_y.template flat<Scalar>().data();
    auto* c_base_ptr = out->template flat<Scalar>().data();
    for (int64 i = 0; i < batch_size; ++i) {
      a_device_memory.push_back(AsDeviceMemory(a_base_ptr + i * m * k));
      b_device_memory.push_back(AsDeviceMemory(b_base_ptr + i * k * n));
      c_device_memory.push_back(AsDeviceMemory(c_base_ptr + i * m * n));
      a_ptrs.push_back(&a_device_memory.back());
      b_ptrs.push_back(&b_device_memory.back());
      c_ptrs.push_back(&c_device_memory.back());
    }

    // Cublas does
    // C = A x B
    // where A, B and C are assumed to be in column major.
    // We want the output to be in row-major, so we can compute
    // C' = B' x A' (' stands for transpose)
    CublasScratchAllocator scratch_allocator(context);
    bool blas_launch_status =
        stream
            ->ThenBlasGemmBatchedWithScratch(
                blas_transpose_b, blas_transpose_a, n, m, k,
                static_cast<Scalar>(1.0), b_ptrs, adj_y ? k : n, a_ptrs,
                adj_x ? m : k, static_cast<Scalar>(0.0), c_ptrs, n, batch_size,
                &scratch_allocator)
            .ok();
    if (!blas_launch_status) {
      context->SetStatus(errors::Internal(
          "Blas SGEMMBatched launch failed : a.shape=",
          in_x.shape().DebugString(), ", b.shape=", in_y.shape().DebugString(),
          ", m=", m, ", n=", n, ", k=", k, ", batch_size=", batch_size));
    }
  }
};

#endif  // GOOGLE_CUDA

template <typename Device, typename Scalar>
class BatchMatMul : public OpKernel {
 public:
  explicit BatchMatMul(OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("adj_x", &adj_x_));
    OP_REQUIRES_OK(context, context->GetAttr("adj_y", &adj_y_));
  }

  virtual ~BatchMatMul() {}

  void Compute(OpKernelContext* ctx) override {
    const Tensor& in0 = ctx->input(0);
    const Tensor& in1 = ctx->input(1);
    OP_REQUIRES(ctx, in0.dims() == in1.dims(),
                errors::InvalidArgument("In[0] and In[1] has different ndims: ",
                                        in0.shape().DebugString(), " vs. ",
                                        in1.shape().DebugString()));
    const int ndims = in0.dims();
    OP_REQUIRES(
        ctx, ndims >= 2,
        errors::InvalidArgument("In[0] and In[1] ndims must be >= 2: ", ndims));
    TensorShape out_shape;
    for (int i = 0; i < ndims - 2; ++i) {
      OP_REQUIRES(ctx, in0.dim_size(i) == in1.dim_size(i),
                  errors::InvalidArgument("In[0].dim(", i, ") and In[1].dim(",
                                          i, ") must be the same: ",
                                          in0.shape().DebugString(), " vs ",
                                          in1.shape().DebugString()));
      out_shape.AddDim(in0.dim_size(i));
    }
    auto n = (ndims == 2) ? 1 : out_shape.num_elements();
    auto d0 = in0.dim_size(ndims - 2);
    auto d1 = in0.dim_size(ndims - 1);
    Tensor in0_reshaped;
    CHECK(in0_reshaped.CopyFrom(in0, TensorShape({n, d0, d1})));
    auto d2 = in1.dim_size(ndims - 2);
    auto d3 = in1.dim_size(ndims - 1);
    Tensor in1_reshaped;
    CHECK(in1_reshaped.CopyFrom(in1, TensorShape({n, d2, d3})));
    if (adj_x_) std::swap(d0, d1);
    if (adj_y_) std::swap(d2, d3);
    OP_REQUIRES(ctx, d1 == d2,
                errors::InvalidArgument(
                    "In[0] mismatch In[1] shape: ", d1, " vs. ", d2, ": ",
                    in0.shape().DebugString(), " ", in1.shape().DebugString(),
                    " ", adj_x_, " ", adj_y_));
    out_shape.AddDim(d0);
    out_shape.AddDim(d3);
    Tensor* out = nullptr;
    OP_REQUIRES_OK(ctx, ctx->allocate_output(0, out_shape, &out));
    if (out->NumElements() == 0) {
      return;
    }
    if (in0.NumElements() == 0 || in1.NumElements() == 0) {
      functor::SetZeroFunctor<Device, Scalar> f;
      f(ctx->eigen_device<Device>(), out->flat<Scalar>());
      return;
    }
    Tensor out_reshaped;
    CHECK(out_reshaped.CopyFrom(*out, TensorShape({n, d0, d3})));
    LaunchBatchMatMul<Device, Scalar>::Launch(ctx, in0_reshaped, in1_reshaped,
                                              adj_x_, adj_y_, &out_reshaped);
  }

 private:
  bool adj_x_;
  bool adj_y_;
};

#define REGISTER_CPU(TYPE)                                              \
  REGISTER_KERNEL_BUILDER(                                              \
      Name("BatchMatMul").Device(DEVICE_CPU).TypeConstraint<TYPE>("T"), \
      BatchMatMul<CPUDevice, TYPE>)

#define REGISTER_GPU(TYPE)                                              \
  REGISTER_KERNEL_BUILDER(                                              \
      Name("BatchMatMul").Device(DEVICE_GPU).TypeConstraint<TYPE>("T"), \
      BatchMatMul<GPUDevice, TYPE>)

TF_CALL_float(REGISTER_CPU);
TF_CALL_double(REGISTER_CPU);
TF_CALL_half(REGISTER_CPU);
TF_CALL_int32(REGISTER_CPU);
TF_CALL_complex64(REGISTER_CPU);
TF_CALL_complex128(REGISTER_CPU);

#if GOOGLE_CUDA
TF_CALL_float(REGISTER_GPU);
TF_CALL_double(REGISTER_GPU);
TF_CALL_complex64(REGISTER_GPU);
TF_CALL_complex128(REGISTER_GPU);
#if CUDA_VERSION >= 7050
TF_CALL_half(REGISTER_GPU);
#endif
#endif  // GOOGLE_CUDA

#undef REGISTER_CPU
#undef REGISTER_GPU
}  // end namespace tensorflow