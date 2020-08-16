// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
/** \file
 *
 * A collection of utility functions to assist calculation of input/output
 * ranges when moving a 2-dimensional kernel over a larger 2-dimensional
 * space (for example in convolution or pooling layers)
 *
 */

#ifndef poplin_ConvUtil_hpp
#define poplin_ConvUtil_hpp
#include <poplin/Convolution.hpp>
#include <tuple>
#include <vector>

namespace poplin {

/// Return the output size when the specified dilation is applied to an
/// input of the specified size.
unsigned getDilatedSize(unsigned size, unsigned dilation);

/// Return the index of the input element that is multiplied by the specified
/// kernel index to produce the specified output.
/// Return ~0U if there is no such input element.
unsigned getInputIndex(unsigned dim, unsigned outputIndex, unsigned kernelIndex,
                       const ConvParams &params);

/// Return the index of the kernel element that is multiplied by the specified
/// input index to produce the specified output.
/// Return ~0U if there is no such kernel element.
unsigned getKernelIndex(unsigned dim, unsigned outputIndex, unsigned inputIndex,
                        const ConvParams &params);

/// Given an output range, return the subset whose calculation
/// involves the specified kernel index.
std::pair<unsigned, unsigned>
getOutputRangeForKernelIndex(unsigned dim,
                             std::pair<unsigned, unsigned> outputRange,
                             unsigned kernelIndex, const ConvParams &params);

/// Given an output range, return the subset whose calculation
/// involves the specified input.
std::pair<unsigned, unsigned>
getOutputRangeForInputIndex(unsigned dim,
                            std::pair<unsigned, unsigned> outputRange,
                            unsigned inputIndex, const ConvParams &params);

/// Given an output range, return the subset whose calculation
/// involves the specified range of kernel indicies.
std::pair<unsigned, unsigned> getOutputRangeForKernelRange(
    unsigned dim, std::pair<unsigned, unsigned> outputRange,
    std::pair<unsigned, unsigned> kernelIndexRange, const ConvParams &params);

/// Given an output range, return the subset whose calculation
/// involves the specified range of input indicies.
std::pair<unsigned, unsigned> getOutputRangeForInputRange(
    unsigned dim, std::pair<unsigned, unsigned> outputRange,
    std::pair<unsigned, unsigned> inputRange, const ConvParams &params);

/// Return the input range that is associated with
/// the specified kernel index when calculating the specified output range.
std::pair<unsigned, unsigned>
getInputRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
              unsigned kernelIndex, const ConvParams &params);

/// Return the kernel range that is associated with
/// the specified input index when calculating the specified output range.
std::pair<unsigned, unsigned>
getKernelRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
               unsigned inputIndex, const ConvParams &params);

/// Return the input range that is associated with the specified kernel index
/// range when calculating the specified output range.
std::pair<unsigned, unsigned>
getInputRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
              std::pair<unsigned, unsigned> kernelIndexRange,
              const ConvParams &params);

/// Return the kernel range that is associated with the specified input index
/// range when calculating the specified output range.
std::pair<unsigned, unsigned>
getKernelRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
               std::pair<unsigned, unsigned> inputRange,
               const ConvParams &params);

inline std::pair<unsigned, unsigned>
getInputRange(unsigned dim, unsigned outputIndex,
              std::pair<unsigned, unsigned> kernelIndexRange,
              const ConvParams &params) {
  return getInputRange(dim, {outputIndex, outputIndex + 1}, kernelIndexRange,
                       params);
}

inline std::pair<unsigned, unsigned>
getInputRange(unsigned dim, unsigned outputIndex, const ConvParams &params) {
  return getInputRange(dim, outputIndex, {0, params.kernelShape[dim]}, params);
}

inline std::pair<unsigned, unsigned>
getInputRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
              const ConvParams &params) {
  return getInputRange(dim, outputRange, {0, params.kernelShape[dim]}, params);
}

/// Given a set of parameters, return the set of params that
/// represent the convolution to be applied to the output gradients
/// to get the input gradients (provided the weights have been
/// transposed in the channel axes and flipped in the spatial axes).
ConvParams getGradientParams(const ConvParams &params);

/// Given a set of convolution parameters, return the set of params that
/// represent the convolution to be applied to the output gradients to get the
/// weight update gradients
ConvParams getWeightUpdateParams(const ConvParams &fwdParams);

} // namespace poplin
#endif // poplin_ConvUtil_hpp
