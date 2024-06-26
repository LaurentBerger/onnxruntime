// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/mlas/inc/mlas.h"
#include "core/platform/threadpool.h"
#include "core/common/narrow.h"
#include "core/framework/element_type_lists.h"
#include "core/framework/float8.h"
#include "core/framework/int4.h"
#include <cmath>

namespace onnxruntime {

inline float RoundHalfToEven(float input) {
  if (!std::isfinite(input)) {
    return input;
  }
  // std::remainder returns x - n, where n is the integral value nearest to x. When |x - n| = 0.5, n is chosen to be even
  return input - std::remainderf(input, 1.f);
}

template <typename T>
struct is_quant_type : std::false_type {};

template <>
struct is_quant_type<int8_t> : std::true_type {};

template <>
struct is_quant_type<uint8_t> : std::true_type {};

// Define max number of parallel threads for computing min max values
// This is hacky. Ideally we should let the thread pool handle work partition.
// Unfortunately I can't find an elegant way for aggregation at this point.
#define MAX_DEGREE_OF_PAR_FOR_MINMAX 32

struct FloatMinMax {
  float min;
  float max;
};

// ReduceRange and Symmetric is for test only
template <typename QType,
          bool ReduceRange = false,
          bool Symmetric = false,
          typename std::enable_if<is_quant_type<QType>::value, int>::type = 0>
void GetQuantizationParameter(const float* data, int64_t num_of_elements, float& scale, QType& zp, concurrency::ThreadPool* thread_pool) {
  FloatMinMax aggregate[MAX_DEGREE_OF_PAR_FOR_MINMAX];

  // Min max operation granularity: AVX512 can potentially handle 64 ~ 128 floats
  // per iteration.
  constexpr int granularity = 128;
  std::ptrdiff_t block_size;
  std::ptrdiff_t num_blocks;
  if (concurrency::ThreadPool::ShouldParallelize(thread_pool) && num_of_elements > granularity) {
    block_size = onnxruntime::narrow<std::ptrdiff_t>((num_of_elements + MAX_DEGREE_OF_PAR_FOR_MINMAX - 1) / MAX_DEGREE_OF_PAR_FOR_MINMAX);
    block_size = (block_size + granularity - 1) / granularity * granularity;
    num_blocks = onnxruntime::narrow<std::ptrdiff_t>((num_of_elements + block_size - 1) / block_size);
  } else {
    num_blocks = 1;
    block_size = onnxruntime::narrow<std::ptrdiff_t>(num_of_elements);
  }

  for (int i = 0; i < num_blocks; i++) {
    aggregate[i].min = std::numeric_limits<float>::max();
    aggregate[i].max = std::numeric_limits<float>::lowest();
  }

  const TensorOpCost unit_cost{static_cast<double>(block_size) * sizeof(float), 2.0, static_cast<double>(block_size)};
  concurrency::ThreadPool::TryParallelFor(thread_pool, num_blocks, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
    auto begin_idx = begin * block_size;
    auto end_idx = std::min(std::ptrdiff_t(num_of_elements), end * block_size);
    auto agg_idx = begin % num_blocks;
    MlasFindMinMaxElement(&(data[begin_idx]), &aggregate[agg_idx].min, &aggregate[agg_idx].max, end_idx - begin_idx);
  });

  float& min = aggregate[0].min;
  float& max = aggregate[0].max;
  for (int i = 1; i < num_blocks; i++) {
    min = std::min(min, aggregate[i].min);
    max = std::max(max, aggregate[i].max);
  }
  // ensure the input range includes zero
  min = std::min(min, 0.0f);
  max = std::max(max, 0.0f);

  // find scale and zero point
  QType qmin = std::numeric_limits<QType>::min();
  QType qmax = std::numeric_limits<QType>::max();
  if (std::is_same<QType, int8_t>::value) {
    if (ReduceRange) {
      qmin = static_cast<QType>(-64);
      qmax = static_cast<QType>(64);
    }

    if (Symmetric) {
      zp = 0;
      float max_value = std::max(max, -min);
      scale = max_value > 0 ? max_value / qmax : 1.f;
      return;
    }
  }
  scale = max == min ? 1.0f : (max - min) / float(qmax - qmin);

  float initial_zero_point = qmin - min / scale;
  zp = static_cast<QType>(RoundHalfToEven(std::max(float(qmin), std::min(float(qmax), initial_zero_point))));
}

/**
 * @brief Run MlasQuantizeLinear in parallel, with provided thread pool
 */

template <typename OutputType>
#if !defined(DISABLE_FLOAT8_TYPES)
typename std::enable_if<!boost::mp11::mp_contains<element_type_lists::AllFloat8, OutputType>::value, void>::type
#else
void
#endif
ParQuantizeLinearStd(const float* Input,
                     OutputType* Output,
                     size_t N,
                     float Scale,
                     OutputType ZeroPoint,
                     concurrency::ThreadPool* thread_pool) {
  constexpr std::ptrdiff_t block_size = 128;
  const std::ptrdiff_t num_blocks = (N + block_size - 1) / block_size;
  const TensorOpCost unit_cost{static_cast<double>(block_size * sizeof(float)), static_cast<double>(block_size * sizeof(uint8_t)), static_cast<double>(block_size) * 2.0};
  concurrency::ThreadPool::TryParallelFor(thread_pool, num_blocks, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
    auto begin_idx = begin * block_size;
    auto end_idx = std::min(static_cast<std::ptrdiff_t>(N), end * block_size);
    MlasQuantizeLinear(&(Input[begin_idx]), &(Output[begin_idx]), end_idx - begin_idx, Scale, ZeroPoint);
  });
}

/**
 * Defines a function for int4 quantization. Calls MLAS kernel in parallel with the provided thread pool.
 *
 * \param FUNC_NAME The name of the generated function.
 * \param INT4_TYPE The int4 type (i.e., either Int4x2 or UInt4x2)
 * \param MLAS_FUNC The MLAS quantization kernel to call.
 * \param Input The input float values to quantize. Must contain `out_end - out_start` elements.
 * \param Output The output buffer that will contain the quantized values.
 * \param out_start The int4 element index at which to start writing to the output buffer.
 *                  Divide by 2 to get index into Output buffer.
 * \param out_end The int4 element index at which to stop writing to the output buffer.
 *                Divide by 2 to get index into Output buffer.
 * \param Scale The quantization scale value.
 * \param ZeroPoint The quantization zero-point value.
 * \param thread_pool The thread pool to use.
 */
#define DEFINE_PAR_QUANT_LINEAR_STD_4BIT(FUNC_NAME, INT4_TYPE, MLAS_FUNC)                                        \
  inline void FUNC_NAME(const float* Input,                                                                      \
                        INT4_TYPE* Output,                                                                       \
                        size_t out_start,                                                                        \
                        size_t out_end,                                                                          \
                        float Scale,                                                                             \
                        INT4_TYPE ZeroPoint,                                                                     \
                        concurrency::ThreadPool* thread_pool) {                                                  \
    size_t inp_start = 0;                                                                                        \
    size_t inp_end = out_end - out_start;                                                                        \
                                                                                                                 \
    /* If starting at an int4 element in the middle of a byte, quantize it by itself. */                         \
    if (out_start & 0x1) {                                                                                       \
      int32_t ival = static_cast<int32_t>(std::nearbyintf(Input[inp_start] / Scale)) +                           \
                     static_cast<int32_t>(ZeroPoint.GetElem(0));                                                 \
      size_t output_index = out_start >> 1;                                                                      \
                                                                                                                 \
      INT4_TYPE::UnpackedType quant_val = static_cast<INT4_TYPE::UnpackedType>(                                  \
          std::min(static_cast<int32_t>(INT4_TYPE::max_val),                                                     \
                   std::max(static_cast<int32_t>(INT4_TYPE::min_val), ival)));                                   \
      Output[output_index].SetElem(1, quant_val);                                                                \
                                                                                                                 \
      out_start += 1;                                                                                            \
      inp_start += 1;                                                                                            \
    }                                                                                                            \
                                                                                                                 \
    /* If ending at element that ends in the middle of a byte, quantize it by itself. */                         \
    if (out_end & 0x1) {                                                                                         \
      int32_t ival = static_cast<int32_t>(std::nearbyintf(Input[inp_end - 1] / Scale)) +                         \
                     static_cast<int32_t>(ZeroPoint.GetElem(0));                                                 \
      size_t output_index = (out_end - 1) >> 1;                                                                  \
                                                                                                                 \
      INT4_TYPE::UnpackedType quant_val = static_cast<INT4_TYPE::UnpackedType>(                                  \
          std::min(static_cast<int32_t>(INT4_TYPE::max_val),                                                     \
                   std::max(static_cast<int32_t>(INT4_TYPE::min_val), ival)));                                   \
      Output[output_index].SetElem(0, quant_val);                                                                \
                                                                                                                 \
      out_end -= 1;                                                                                              \
      inp_end -= 1;                                                                                              \
    }                                                                                                            \
                                                                                                                 \
    if (out_start == out_end) {                                                                                  \
      return;                                                                                                    \
    }                                                                                                            \
                                                                                                                 \
    /* At this point, should only need to quantize an *even* number of int4 elements that start and end at */    \
    /* a byte boundary. This is necessary to ensure that no two threads write to different int4 elements that */ \
    /* are stored in the same byte. */                                                                           \
    size_t N = out_end - out_start;                                                                              \
    assert(N % 2 == 0); /* Should be guaranteed by previous code that quantizes boundary elements. */            \
                                                                                                                 \
    constexpr std::ptrdiff_t block_size = 128;                                                                   \
    static_assert(block_size % 2 == 0,                                                                           \
                  "Block size must also be even to ensure no two threads write to the same byte.");              \
                                                                                                                 \
    const std::ptrdiff_t num_blocks = (N + block_size - 1) / block_size;                                         \
    const TensorOpCost unit_cost{static_cast<double>(block_size * sizeof(float)),                                \
                                 static_cast<double>(block_size * sizeof(INT4_TYPE::UnpackedType)) / 2.0,        \
                                 static_cast<double>(block_size) * 2.0};                                         \
    concurrency::ThreadPool::TryParallelFor(                                                                     \
        thread_pool, num_blocks, unit_cost,                                                                      \
        [&](std::ptrdiff_t begin, std::ptrdiff_t end) {                                                          \
          auto begin_idx = begin * block_size;                                                                   \
          auto end_idx = std::min(static_cast<std::ptrdiff_t>(N), end * block_size);                             \
          auto inp_idx = begin_idx + static_cast<std::ptrdiff_t>(inp_start);                                     \
          auto out_idx = begin_idx + static_cast<std::ptrdiff_t>(out_start);                                     \
                                                                                                                 \
          MLAS_FUNC(&(Input[inp_idx]),                                                                           \
                    reinterpret_cast<uint8_t*>(&(Output[out_idx >> 1])),                                         \
                    end_idx - begin_idx,                                                                         \
                    Scale,                                                                                       \
                    static_cast<int8_t>(ZeroPoint.GetElem(0)));                                                  \
        });                                                                                                      \
  }

DEFINE_PAR_QUANT_LINEAR_STD_4BIT(ParQuantizeLinearStdS4, Int4x2, MlasQuantizeLinearS4)
DEFINE_PAR_QUANT_LINEAR_STD_4BIT(ParQuantizeLinearStdU4, UInt4x2, MlasQuantizeLinearU4)

// This implementation could be more efficient however the cast from float16 to other types
// usually happens on GPU.
template <typename OutputType>
#if !defined(DISABLE_FLOAT8_TYPES)
typename std::enable_if<!boost::mp11::mp_contains<element_type_lists::AllFloat8, OutputType>::value, void>::type
#else
void
#endif
ParQuantizeLinearStd(const MLFloat16* Input,
                     OutputType* Output,
                     size_t N,
                     MLFloat16 Scale,
                     OutputType ZeroPoint,
                     concurrency::ThreadPool* thread_pool) {
  constexpr std::ptrdiff_t block_size = 128;
  const std::ptrdiff_t num_blocks = (N + block_size - 1) / block_size;
  const TensorOpCost unit_cost{static_cast<double>(block_size * sizeof(MLFloat16)), static_cast<double>(block_size * sizeof(uint8_t)), static_cast<double>(block_size) * 2.0};
  concurrency::ThreadPool::TryParallelFor(thread_pool, num_blocks, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
    auto begin_idx = begin * block_size;
    auto end_idx = std::min(static_cast<std::ptrdiff_t>(N), end * block_size);
    float fscale = Scale.ToFloat();
    for (; begin_idx != end_idx; ++begin_idx) {
      int32_t ival = static_cast<int32_t>(Input[begin_idx].ToFloat() / fscale) + ZeroPoint;
      Output[begin_idx] = static_cast<OutputType>(std::min(static_cast<int32_t>(std::numeric_limits<OutputType>::max()),
                                                           std::max(static_cast<int32_t>(std::numeric_limits<OutputType>::lowest()), ival)));
    }
  });
}

#if !defined(DISABLE_FLOAT8_TYPES)

template <typename OutputFloat8Type>
typename std::enable_if<boost::mp11::mp_contains<element_type_lists::AllFloat8, OutputFloat8Type>::value, void>::type
ParQuantizeLinearSat(const float* Input,
                     OutputFloat8Type* Output,
                     size_t N,
                     float Scale,
                     const OutputFloat8Type& /* ORT_UNUSED_PARAMETER(ZeroPoint) */,
                     bool saturate,
                     concurrency::ThreadPool* thread_pool) {
  constexpr std::ptrdiff_t block_size = 128;
  const std::ptrdiff_t num_blocks = (N + block_size - 1) / block_size;
  const TensorOpCost unit_cost{static_cast<double>(block_size * sizeof(float)), static_cast<double>(block_size * sizeof(uint8_t)), static_cast<double>(block_size) * 2.0};
  concurrency::ThreadPool::TryParallelFor(thread_pool, num_blocks, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
    auto begin_idx = begin * block_size;
    auto end_idx = std::min(static_cast<std::ptrdiff_t>(N), end * block_size);
    for (; begin_idx < end_idx; ++begin_idx) {
      Output[begin_idx] = OutputFloat8Type(Input[begin_idx] / Scale, saturate);
    }
  });
}

// The implementation converts float16 to float and then do a quantization.
// This is not efficient and is mostly added to enable unittest on CPU.
// This case usually happens on GPU.
template <typename OutputFloat8Type>
typename std::enable_if<boost::mp11::mp_contains<element_type_lists::AllFloat8, OutputFloat8Type>::value, void>::type
ParQuantizeLinearSat(const MLFloat16* Input,
                     OutputFloat8Type* Output,
                     size_t N,
                     MLFloat16 Scale,
                     const OutputFloat8Type& /* ORT_UNUSED_PARAMETER(ZeroPoint) */,
                     bool saturate,
                     concurrency::ThreadPool* thread_pool) {
  constexpr std::ptrdiff_t block_size = 128;
  const std::ptrdiff_t num_blocks = (N + block_size - 1) / block_size;
  const TensorOpCost unit_cost{static_cast<double>(block_size * sizeof(MLFloat16)), static_cast<double>(block_size * sizeof(uint8_t)), static_cast<double>(block_size) * 2.0};
  concurrency::ThreadPool::TryParallelFor(thread_pool, num_blocks, unit_cost, [&](std::ptrdiff_t begin, std::ptrdiff_t end) {
    auto begin_idx = begin * block_size;
    auto end_idx = std::min(static_cast<std::ptrdiff_t>(N), end * block_size);
    for (; begin_idx < end_idx; ++begin_idx) {
      Output[begin_idx] = OutputFloat8Type(Input[begin_idx].ToFloat() / Scale.ToFloat(), saturate);
    }
  });
}

#endif

}  // namespace onnxruntime
