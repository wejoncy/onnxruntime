// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#include "core/platform/env_var_utils.h"
#include "contrib_ops/cuda/bert/attention.h"
#include "contrib_ops/cuda/bert/attention_impl.h"
#include "core/providers/cuda/cuda_common.h"
#include "core/providers/cuda/shared_inc/fpgeneric.h"
#include "contrib_ops/cuda/bert/tensorrt_fused_multihead_attention/mha_runner.h"

using namespace onnxruntime::cuda;
using namespace ::onnxruntime::common;
using namespace ONNX_NAMESPACE;

namespace onnxruntime {
namespace contrib {
namespace cuda {

#define REGISTER_KERNEL_TYPED(T)                                  \
  ONNX_OPERATOR_TYPED_KERNEL_EX(                                  \
      Attention,                                                  \
      kMSDomain,                                                  \
      1,                                                          \
      T,                                                          \
      kCudaExecutionProvider,                                     \
      (*KernelDefBuilder::Create())                               \
          .TypeConstraint("T", DataTypeImpl::GetTensorType<T>()), \
      Attention<T>);

REGISTER_KERNEL_TYPED(float)
REGISTER_KERNEL_TYPED(MLFloat16)

static inline bool HasFusedFp16Kernel(int sm, int head_size, int sequence_length) {
  if (!(sm == kSM_70 || sm == kSM_75 || sm == kSM_80 || sm == kSM_86)) {
    return false;
  }

  if (head_size == 32) {
    return (sm == kSM_75 || sm == kSM_80) &&
           (sequence_length == 128 || sequence_length == 256 || sequence_length == 512);
  }

  if (head_size != 64) {
    return false;
  }

  if (!(sequence_length == 64 || sequence_length == 128 || sequence_length == 192 ||
        sequence_length == 256 || sequence_length == 384 || sequence_length == 512)) {
    return false;
  }

  if (sm == kSM_70 || sm == kSM_86) {
    return sequence_length != 512;
  }

  return true;
}

template <typename T>
Attention<T>::Attention(const OpKernelInfo& info) : CudaKernel(info), AttentionBase(info) {
}

template <typename T>
Status Attention<T>::ComputeInternal(OpKernelContext* context) const {
  const Tensor* input = context->Input<Tensor>(0);
  const Tensor* weights = context->Input<Tensor>(1);
  const Tensor* bias = context->Input<Tensor>(2);
  const Tensor* mask_index = context->Input<Tensor>(3);
  const Tensor* past = context->Input<Tensor>(4);
  const Tensor* extra_add_qk = context->Input<Tensor>(5);

  auto& device_prop = GetDeviceProp();
  ORT_RETURN_IF_ERROR(CheckInputs(input->Shape(),
                                  weights->Shape(),
                                  bias->Shape(),
                                  mask_index,
                                  past,
                                  extra_add_qk,
                                  device_prop.maxThreadsPerBlock));

  // input shape (batch_size, sequence_length, input_hidden_size)
  const auto& shape = input->Shape();
  int batch_size = static_cast<int>(shape[0]);
  int sequence_length = static_cast<int>(shape[1]);
  int input_hidden_size = static_cast<int>(shape[2]);

  // bias shape (3 * hidden_size)
  const auto& bias_shape = bias->Shape();
  int hidden_size = static_cast<int>(bias_shape[0]) / 3;

  int head_size = hidden_size / num_heads_;

  TensorShapeVector output_shape(3);
  output_shape[0] = shape[0];
  output_shape[1] = shape[1];
  output_shape[2] = static_cast<int64_t>(hidden_size);
  Tensor* output = context->Output(0, output_shape);

  int past_sequence_length = 0;
  Tensor* present = GetPresent(context, past, batch_size, head_size, sequence_length, past_sequence_length);

  // Check whether we can use fused kernel
  int sm = device_prop.major * 10 + device_prop.minor;
  bool use_mha_runner = (sizeof(T) == 2 &&
                         nullptr != mask_index && mask_index->Shape().NumDimensions() == 1 &&
                         nullptr == past &&
                         nullptr == present &&
                         nullptr == extra_add_qk &&
                         !is_unidirectional_ &&
                        HasFusedFp16Kernel(sm, head_size, sequence_length));

  MHARunner* mha_runner = nullptr;
  if (use_mha_runner){
    if (nullptr == dispatcher_fp16_.get()) {
      dispatcher_fp16_.reset(new FusedMHARunnerFP16v2(num_heads_, head_size, sm));
    }
    mha_runner = dispatcher_fp16_.get();
  }

  cublasHandle_t cublas = CublasHandle();
  constexpr size_t element_size = sizeof(T);

  // Use GEMM for fully connection.
  int m = batch_size * sequence_length;
  int n = 3 * hidden_size;
  int k = input_hidden_size;
  auto gemm_buffer = GetScratchBuffer<T>(batch_size * sequence_length * 3 * hidden_size * element_size);

  typedef typename ToCudaType<T>::MappedType CudaT;
  CudaT one = ToCudaType<T>::FromFloat(1.0f);
  CudaT zero = ToCudaType<T>::FromFloat(0.0f);

  // Gemm, note that CUDA assumes col-major, so result(N, M) = 1 * weights x input + 1 x B.
  CUBLAS_RETURN_IF_ERROR(cublasGemmHelper(
      cublas, CUBLAS_OP_N, CUBLAS_OP_N, n, m, k, &one,
      reinterpret_cast<const CudaT*>(weights->Data<T>()), n,
      reinterpret_cast<const CudaT*>(input->Data<T>()), k,
      &zero, reinterpret_cast<CudaT*>(gemm_buffer.get()), n, device_prop));

  size_t workSpaceSize = GetAttentionWorkspaceSize(element_size,
                                                   batch_size,
                                                   num_heads_,
                                                   head_size,
                                                   sequence_length,
                                                   past_sequence_length,
                                                   mha_runner);

  auto work_space = GetScratchBuffer<void>(workSpaceSize);
  if (!LaunchAttentionKernel(
          device_prop,
          Stream(),
          cublas,
          element_size,
          batch_size,
          sequence_length,
          num_heads_,
          head_size,
          past_sequence_length,
          is_unidirectional_,
          reinterpret_cast<const void*>(gemm_buffer.get()),
          bias->Data<T>(),
          nullptr == mask_index ? nullptr : mask_index->Data<int>(),
          nullptr == mask_index ? gsl::span<const int64_t>() : mask_index->Shape().GetDims(),
          nullptr == past ? nullptr : past->Data<T>(),
          nullptr == extra_add_qk ? nullptr : extra_add_qk->Data<T>(),
          work_space.get(),
          output->MutableData<T>(),
          nullptr == present ? nullptr : present->MutableData<T>(),
          mha_runner)) {
    // Get last error to reset it to cudaSuccess.
    CUDA_CALL(cudaGetLastError());
    return Status(common::ONNXRUNTIME, common::FAIL);
  }

  return Status::OK();
}

}  // namespace cuda
}  // namespace contrib
}  // namespace onnxruntime
