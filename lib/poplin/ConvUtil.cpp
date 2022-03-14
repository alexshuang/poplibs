// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
#include "poplin/ConvUtil.hpp"
#include "ConvUtilInternal.hpp"

#include "CanonicalConvParams.hpp"
#include "poplibs_support/gcd.hpp"
#include <boost/optional.hpp>
#include <boost/range/irange.hpp>
#include <cassert>
#include <map>
#include <poplibs_support/logging.hpp>
#include <popops/Rearrange.hpp>
#include <poputil/Util.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>

#include <gccs/Algorithm.hpp>

using namespace popops;
using namespace poputil;
using namespace poplibs_support;

namespace poplin {

namespace {

class ConvRange {
  unsigned begin_;
  unsigned end_;
  /// Whether the range is exact or a conservative approximation.
  bool isExact_;
  /// Whether the range may contains gaps / zeros.
  bool isDilated_;

  void canonicalize() {
    if (begin_ >= end_) {
      begin_ = end_ = 0;
      isExact_ = true;
      isDilated_ = false;
    } else if (size() < 3) {
      isDilated_ = false;
    }
  }

public:
  ConvRange(unsigned begin, unsigned end, bool isExact = true,
            bool isDilated = false)
      : begin_(begin), end_(end), isExact_(isExact), isDilated_(isDilated) {
    canonicalize();
  }
  explicit ConvRange(std::pair<unsigned, unsigned> range, bool isExact = true,
                     bool isDilated = false)
      : ConvRange(range.first, range.second, isExact, isDilated) {}
  static ConvRange getEmpty() { return ConvRange(0, 0); }
  bool empty() const { return begin_ == end_; }
  unsigned size() const { return end_ - begin_; }
  unsigned begin() const { return begin_; }
  unsigned end() const { return end_; }
  bool isExact() const { return isExact_; }
  bool isDilated() const { return isDilated_; }
  unsigned last() const {
    assert(!empty());
    return end_ - 1;
  }
  // Set lower bound of the range to the max of the current lower bound and
  // the specified value.
  void refineBegin(unsigned bound) {
    if (bound > begin_) {
      begin_ = bound;
      if (isDilated_)
        isExact_ = false;
      canonicalize();
    }
  }
  // Set upper bound of the range to the max of the current upper bound and
  // the specified value.
  void refineEnd(unsigned bound) {
    if (bound < end_) {
      end_ = bound;
      if (isDilated_)
        isExact_ = false;
      canonicalize();
    }
  }
  operator std::pair<unsigned, unsigned>() const { return {begin_, end_}; }
};

} // End anonymous namespace

unsigned getDilatedSize(unsigned size, unsigned dilation) {
  if (size == 0)
    return 0;
  return 1 + (size - 1) * dilation;
}

/// Given an index in a volume return the corresponding index after applying the
/// specified truncation, dilation, padding and flipping. Return ~0U if
/// truncation means the element is ignored.
static unsigned applyTruncateDilatePadAndFlip(
    unsigned index, unsigned inputSize, unsigned truncationLower,
    unsigned truncationUpper, unsigned dilation, unsigned paddingLower,
    unsigned paddingUpper, bool flip) {
  assert(index < inputSize);
  if (index < truncationLower || index >= inputSize - truncationUpper)
    return ~0U;
  const auto truncatedIndex = index - truncationLower;
  const auto truncatedSize = inputSize - (truncationLower + truncationUpper);
  const auto truncatedDilatedIndex = truncatedIndex * dilation;
  const auto truncatedDilatedSize = getDilatedSize(truncatedSize, dilation);
  const auto truncatedDilatedPaddedIndex = truncatedDilatedIndex + paddingLower;
  const auto truncatedDilatedPaddedSize =
      paddingLower + truncatedDilatedSize + paddingUpper;
  return flip ? truncatedDilatedPaddedSize - 1 - truncatedDilatedPaddedIndex
              : truncatedDilatedPaddedIndex;
}

/// Given a range in a volume return the corresponding range after applying the
/// specified truncation, dilation, padding and flipping.
static ConvRange applyTruncateDilatePadAndFlip(
    ConvRange range, unsigned inputSize, unsigned truncationLower,
    unsigned truncationUpper, unsigned dilation, unsigned paddingLower,
    unsigned paddingUpper, bool flip) {
  assert(range.begin() <= range.end());
  assert(range.end() <= inputSize);
  range.refineBegin(truncationLower);
  range.refineEnd(inputSize - truncationUpper);
  if (range.empty())
    return range;
  auto transformedBegin = applyTruncateDilatePadAndFlip(
      range.begin(), inputSize, truncationLower, truncationUpper, dilation,
      paddingLower, paddingUpper, flip);
  assert(transformedBegin != ~0U);
  if (range.size() == 1) {
    return {transformedBegin, transformedBegin + 1, range.isExact(), false};
  }
  auto transformedLast = applyTruncateDilatePadAndFlip(
      range.last(), inputSize, truncationLower, truncationUpper, dilation,
      paddingLower, paddingUpper, flip);
  assert(transformedLast != ~0U);
  if (flip) {
    std::swap(transformedBegin, transformedLast);
  }
  return {transformedBegin, transformedLast + 1, range.isExact(),
          range.isDilated() || dilation > 1};
}

static ConvRange applyTransform(unsigned dim, ConvRange range,
                                const std::vector<std::size_t> &inputSize,
                                const ConvParams::InputTransform &transform) {
  return applyTruncateDilatePadAndFlip(
      range, inputSize[dim], transform.truncationLower[dim],
      transform.truncationUpper[dim], transform.dilation[dim],
      transform.paddingLower[dim], transform.paddingUpper[dim],
      transform.flip[dim]);
}

/// Given a index in a dilated and padded volume return the index in the
/// original volume. Return ~0U if the index doesn't correspond to any
/// index in the original volume.
static unsigned reverseTruncateDilatePadAndFlip(
    unsigned truncatedDilatedPaddedFlippedIndex, unsigned inputSize,
    unsigned truncationLower, unsigned truncationUpper, unsigned dilation,
    unsigned paddingLower, unsigned paddingUpper, bool flip) {
  const auto truncatedSize = inputSize - (truncationLower + truncationUpper);
  const auto truncatedDilatedSize = getDilatedSize(truncatedSize, dilation);
  const auto truncatedDilatedPaddedSize =
      paddingLower + truncatedDilatedSize + paddingUpper;
  assert(truncatedDilatedPaddedFlippedIndex < truncatedDilatedPaddedSize);
  const auto truncatedDilatedPaddedIndex =
      flip ? truncatedDilatedPaddedSize - 1 - truncatedDilatedPaddedFlippedIndex
           : truncatedDilatedPaddedFlippedIndex;
  if (truncatedDilatedPaddedIndex < paddingLower ||
      truncatedDilatedPaddedIndex >= truncatedDilatedPaddedSize - paddingUpper)
    return ~0U;
  const auto truncatedDilatedIndex = truncatedDilatedPaddedIndex - paddingLower;
  if (truncatedDilatedIndex % dilation != 0)
    return ~0U;
  const auto truncatedIndex = truncatedDilatedIndex / dilation;
  return truncatedIndex + truncationLower;
}

/// Given a range in a dilated and padded volume return the range in the
/// original volume.
static ConvRange reverseTruncateDilatePadAndFlip(
    ConvRange range, unsigned inputSize, unsigned truncationLower,
    unsigned truncationUpper, unsigned dilation, unsigned paddingLower,
    unsigned paddingUpper, bool flip) {
  auto codomain = applyTruncateDilatePadAndFlip(
      {0, inputSize}, inputSize, truncationLower, truncationUpper, dilation,
      paddingLower, paddingUpper, flip);
  range.refineBegin(codomain.begin());
  range.refineEnd(codomain.end());
  if (range.empty())
    return range;
  if (dilation > 1) {
    range.refineBegin(
        codomain.begin() +
        gccs::alignNext(range.begin() - codomain.begin(), dilation));
    if (range.empty())
      return range;
    range.refineEnd(codomain.begin() +
                    gccs::alignPrev(range.last() - codomain.begin(), dilation) +
                    1);
    if (range.empty())
      return range;
  }
  auto transformedBegin = reverseTruncateDilatePadAndFlip(
      range.begin(), inputSize, truncationLower, truncationUpper, dilation,
      paddingLower, paddingUpper, flip);
  assert(transformedBegin != ~0U);
  if (range.size() == 1) {
    return {transformedBegin, transformedBegin + 1, range.isExact(), false};
  }
  auto transformedLast = reverseTruncateDilatePadAndFlip(
      range.last(), inputSize, truncationLower, truncationUpper, dilation,
      paddingLower, paddingUpper, flip);
  assert(transformedLast != ~0U);
  if (flip) {
    std::swap(transformedBegin, transformedLast);
  }
  return {transformedBegin, transformedLast + 1, range.isExact(),
          range.isDilated()};
}

static ConvRange reverseTransform(unsigned dim, ConvRange range,
                                  const std::vector<std::size_t> &inputSize,
                                  const ConvParams::InputTransform &transform) {
  return reverseTruncateDilatePadAndFlip(
      range, inputSize[dim], transform.truncationLower[dim],
      transform.truncationUpper[dim], transform.dilation[dim],
      transform.paddingLower[dim], transform.paddingUpper[dim],
      transform.flip[dim]);
}

unsigned getInputIndex(unsigned dim, unsigned truncatedStridedPaddedOutputIndex,
                       unsigned kernelIndex, const ConvParams &params) {
  const auto outputSize = params.getOutputSize(dim);
  assert(truncatedStridedPaddedOutputIndex < outputSize);
  const auto paddedKernelIndex =
      applyTruncateDilatePadAndFlip(kernelIndex, params.kernelShape[dim],
                                    params.kernelTransform.truncationLower[dim],
                                    params.kernelTransform.truncationUpper[dim],
                                    params.kernelTransform.dilation[dim],
                                    params.kernelTransform.paddingLower[dim],
                                    params.kernelTransform.paddingUpper[dim],
                                    params.kernelTransform.flip[dim]);
  if (paddedKernelIndex == ~0U)
    return ~0U;
  if (truncatedStridedPaddedOutputIndex <
          params.outputTransform.paddingLower[dim] ||
      truncatedStridedPaddedOutputIndex >=
          outputSize - params.outputTransform.paddingUpper[dim])
    return ~0U;
  const auto truncatedStridedOutputIndex =
      truncatedStridedPaddedOutputIndex -
      params.outputTransform.paddingLower[dim];
  const auto truncatedOutputIndex =
      truncatedStridedOutputIndex * params.outputTransform.stride[dim];
  const auto outputIndex =
      truncatedOutputIndex + params.outputTransform.truncationLower[dim];
  int paddedInputIndex = paddedKernelIndex + outputIndex;
  return reverseTruncateDilatePadAndFlip(
      paddedInputIndex, params.inputFieldShape[dim],
      params.inputTransform.truncationLower[dim],
      params.inputTransform.truncationUpper[dim],
      params.inputTransform.dilation[dim],
      params.inputTransform.paddingLower[dim],
      params.inputTransform.paddingUpper[dim], params.inputTransform.flip[dim]);
}

unsigned getKernelIndex(unsigned dim,
                        unsigned truncatedStridedPaddedOutputIndex,
                        unsigned inputIndex, const ConvParams &params) {
  const auto outputSize = params.getOutputSize(dim);
  assert(truncatedStridedPaddedOutputIndex < outputSize);
  const auto paddedInputIndex = applyTruncateDilatePadAndFlip(
      inputIndex, params.inputFieldShape[dim],
      params.inputTransform.truncationLower[dim],
      params.inputTransform.truncationUpper[dim],
      params.inputTransform.dilation[dim],
      params.inputTransform.paddingLower[dim],
      params.inputTransform.paddingUpper[dim], params.inputTransform.flip[dim]);
  if (paddedInputIndex == ~0U)
    return ~0U;
  if (truncatedStridedPaddedOutputIndex <
          params.outputTransform.paddingLower[dim] ||
      truncatedStridedPaddedOutputIndex >=
          outputSize - params.outputTransform.paddingUpper[dim])
    return ~0U;
  const auto truncatedStridedOutputIndex =
      truncatedStridedPaddedOutputIndex -
      params.outputTransform.paddingLower[dim];
  const auto truncatedOutputIndex =
      truncatedStridedOutputIndex * params.outputTransform.stride[dim];
  const auto outputIndex =
      truncatedOutputIndex + params.outputTransform.truncationLower[dim];
  if (outputIndex > paddedInputIndex)
    return ~0U;
  const auto paddedKernelIndex = paddedInputIndex - outputIndex;
  if (paddedKernelIndex >= params.getTransformedKernelSize(dim))
    return ~0U;
  return reverseTruncateDilatePadAndFlip(
      paddedKernelIndex, params.kernelShape[dim],
      params.kernelTransform.truncationLower[dim],
      params.kernelTransform.truncationUpper[dim],
      params.kernelTransform.dilation[dim],
      params.kernelTransform.paddingLower[dim],
      params.kernelTransform.paddingUpper[dim],
      params.kernelTransform.flip[dim]);
}

// Quickly compute the output range that corresponds to the specified input and
// kernel ranges. The range returned may be larger than the true range, in which
// case the isExact field of the range will be false.
static ConvRange
getOutputRangeQuick(unsigned dim, std::pair<unsigned, unsigned> inputRange,
                    std::pair<unsigned, unsigned> kernelRange,
                    std::pair<unsigned, unsigned> outputRangeBounds,
                    const ConvParams &params) {
  auto transformedInputRange =
      applyTransform(dim, ConvRange(inputRange), params.inputFieldShape,
                     params.inputTransform);
  if (transformedInputRange.empty()) {
    return ConvRange::getEmpty();
  }
  auto transformedKernelRange = applyTransform(
      dim, ConvRange(kernelRange), params.kernelShape, params.kernelTransform);
  if (transformedKernelRange.empty()) {
    return ConvRange::getEmpty();
  }
  const auto transformedInputBegin = transformedInputRange.begin();
  const auto transformedInputLast = transformedInputRange.last();
  const auto transformedKernelBegin = transformedKernelRange.begin();
  const auto transformedKernelLast = transformedKernelRange.last();
  const auto untransformedOutputSize = params.getUntransformedOutputSize(dim);
  if (transformedKernelBegin > transformedInputLast ||
      transformedKernelLast + untransformedOutputSize <=
          transformedInputBegin) {
    return ConvRange::getEmpty();
  }
  bool isExact =
      transformedInputRange.isExact() && transformedKernelRange.isExact();
  bool isDilated =
      transformedInputRange.isDilated() || transformedKernelRange.isDilated();
  unsigned untransformedOutputBegin;
  if (transformedInputBegin >= transformedKernelLast) {
    untransformedOutputBegin = transformedInputBegin - transformedKernelLast;
  } else {
    if (isDilated)
      isExact = false;
    untransformedOutputBegin = 0;
  }
  auto untransformedOutputLast = transformedInputLast - transformedKernelBegin;
  const auto untransformedOutputRange =
      ConvRange(untransformedOutputBegin, untransformedOutputLast + 1, isExact,
                isDilated);
  auto outputRange = reverseTruncateDilatePadAndFlip(
      untransformedOutputRange, params.getOutputSize(dim),
      params.outputTransform.paddingLower[dim],
      params.outputTransform.paddingUpper[dim],
      params.outputTransform.stride[dim],
      params.outputTransform.truncationLower[dim],
      params.outputTransform.truncationUpper[dim], false);
  outputRange.refineBegin(outputRangeBounds.first);
  outputRange.refineEnd(outputRangeBounds.second);
  return outputRange;
}

// Quickly compute the input range that corresponds to the specified kernel and
// output ranges. The range returned may be larger than the true range, in which
// case the isExact field of the range will be false.
static ConvRange
getInputRangeQuick(unsigned dim, std::pair<unsigned, unsigned> inputRangeBounds,
                   std::pair<unsigned, unsigned> kernelRange,
                   std::pair<unsigned, unsigned> outputRange,
                   const ConvParams &params) {
  auto untransformedOutputRange = applyTruncateDilatePadAndFlip(
      ConvRange(outputRange), params.getOutputSize(dim),
      params.outputTransform.paddingLower[dim],
      params.outputTransform.paddingUpper[dim],
      params.outputTransform.stride[dim],
      params.outputTransform.truncationLower[dim],
      params.outputTransform.truncationUpper[dim], false);
  if (untransformedOutputRange.empty()) {
    return ConvRange::getEmpty();
  }
  auto transformedKernelRange = applyTransform(
      dim, ConvRange(kernelRange), params.kernelShape, params.kernelTransform);
  if (transformedKernelRange.empty()) {
    return ConvRange::getEmpty();
  }
  const auto untransformedOutputBegin = untransformedOutputRange.begin();
  const auto untransformedOutputLast = untransformedOutputRange.last();
  const auto transformedKernelBegin = transformedKernelRange.begin();
  const auto transformedKernelLast = transformedKernelRange.last();
  const auto transformedInputBegin =
      untransformedOutputBegin + transformedKernelBegin;
  const auto transformedInputLast =
      untransformedOutputLast + transformedKernelLast;
  bool isExact =
      untransformedOutputRange.isExact() && transformedKernelRange.isExact();
  bool isDilated = untransformedOutputRange.isDilated() ||
                   transformedKernelRange.isDilated();
  const auto transformedInputRange = ConvRange(
      transformedInputBegin, transformedInputLast + 1, isExact, isDilated);
  auto inputRange =
      reverseTransform(dim, transformedInputRange, params.inputFieldShape,
                       params.inputTransform);
  inputRange.refineBegin(inputRangeBounds.first);
  inputRange.refineEnd(inputRangeBounds.second);
  return inputRange;
}

// Quickly compute the kernel range that corresponds to the specified input and
// output ranges. The range returned may be larger than the true range, in which
// case the isExact field of the range will be false.
static ConvRange
getKernelRangeQuick(unsigned dim, std::pair<unsigned, unsigned> inputRange,
                    std::pair<unsigned, unsigned> kernelRangeBounds,
                    std::pair<unsigned, unsigned> outputRange,
                    const ConvParams &params) {
  auto untransformedOutputRange = applyTruncateDilatePadAndFlip(
      ConvRange(outputRange), params.getOutputSize(dim),
      params.outputTransform.paddingLower[dim],
      params.outputTransform.paddingUpper[dim],
      params.outputTransform.stride[dim],
      params.outputTransform.truncationLower[dim],
      params.outputTransform.truncationUpper[dim], false);
  if (untransformedOutputRange.empty()) {
    return ConvRange::getEmpty();
  }
  auto transformedInputRange =
      applyTransform(dim, ConvRange(inputRange), params.inputFieldShape,
                     params.inputTransform);
  if (transformedInputRange.empty()) {
    return ConvRange::getEmpty();
  }
  const auto untransformedOutputBegin = untransformedOutputRange.begin();
  const auto untransformedOutputLast = untransformedOutputRange.last();
  const auto transformedInputBegin = transformedInputRange.begin();
  const auto transformedInputLast = transformedInputRange.last();
  const auto transformedKernelSize = params.getTransformedKernelSize(dim);
  if (transformedInputLast < untransformedOutputBegin ||
      transformedInputBegin >=
          untransformedOutputLast + transformedKernelSize) {
    return ConvRange::getEmpty();
  }
  bool isExact =
      untransformedOutputRange.isExact() && transformedInputRange.isExact();
  bool isDilated =
      untransformedOutputRange.isDilated() || transformedInputRange.isDilated();
  unsigned transformedKernelBegin, transformedKernelLast;
  if (transformedInputBegin >= untransformedOutputLast) {
    transformedKernelBegin = transformedInputBegin - untransformedOutputLast;
  } else {
    if (isDilated)
      isExact = false;
    transformedKernelBegin = 0;
  }
  if (transformedInputLast - untransformedOutputBegin <=
      transformedKernelSize - 1) {
    transformedKernelLast = transformedInputLast - untransformedOutputBegin;
  } else {
    if (isDilated)
      isExact = true;
    transformedKernelLast = transformedKernelSize - 1;
  }
  const auto transformedKernelRange = ConvRange(
      transformedKernelBegin, transformedKernelLast + 1, isExact, isDilated);
  auto kernelRange = reverseTransform(
      dim, transformedKernelRange, params.kernelShape, params.kernelTransform);
  kernelRange.refineBegin(kernelRangeBounds.first);
  kernelRange.refineEnd(kernelRangeBounds.second);
  return kernelRange;
}

std::pair<unsigned, unsigned>
getOutputRangeForKernelIndex(unsigned dim,
                             std::pair<unsigned, unsigned> outputRangeBounds,
                             unsigned kernelIndex, const ConvParams &params) {
  assert(outputRangeBounds.first <= outputRangeBounds.second);
  auto refinedOutputRangeBounds = getOutputRangeQuick(
      dim, {0, params.inputFieldShape[dim]}, {kernelIndex, kernelIndex + 1},
      outputRangeBounds, params);
  if (refinedOutputRangeBounds.isExact()) {
    return refinedOutputRangeBounds;
  }
  unsigned outputBegin = 0, outputEnd = 0;
  for (unsigned i = refinedOutputRangeBounds.begin();
       i != refinedOutputRangeBounds.end(); ++i) {
    if (getInputIndex(dim, i, kernelIndex, params) == ~0U) {
      continue;
    }
    outputBegin = i;
    break;
  }
  for (unsigned i = refinedOutputRangeBounds.end();
       i != refinedOutputRangeBounds.begin(); --i) {
    if (getInputIndex(dim, i - 1, kernelIndex, params) == ~0U) {
      continue;
    }
    outputEnd = i;
    break;
  }
  return {outputBegin, outputEnd};
}

std::pair<unsigned, unsigned>
getOutputRangeForInputIndex(unsigned dim,
                            std::pair<unsigned, unsigned> outputRangeBounds,
                            unsigned inputIndex, const ConvParams &params) {
  assert(outputRangeBounds.first <= outputRangeBounds.second);
  auto refinedOutputRangeBounds = getOutputRangeQuick(
      dim, {inputIndex, inputIndex + 1}, {0, params.kernelShape[dim]},
      outputRangeBounds, params);
  if (refinedOutputRangeBounds.isExact())
    return refinedOutputRangeBounds;
  unsigned outputBegin = 0, outputEnd = 0;
  for (unsigned i = refinedOutputRangeBounds.begin();
       i != refinedOutputRangeBounds.end(); ++i) {
    if (getKernelIndex(dim, i, inputIndex, params) == ~0U) {
      continue;
    }
    outputBegin = i;
    break;
  }
  for (unsigned i = refinedOutputRangeBounds.end();
       i != refinedOutputRangeBounds.begin(); --i) {
    if (getKernelIndex(dim, i - 1, inputIndex, params) == ~0U) {
      continue;
    }
    outputEnd = i;
    break;
  }
  return {outputBegin, outputEnd};
}

std::pair<unsigned, unsigned> getOutputRangeForKernelRange(
    unsigned dim, std::pair<unsigned, unsigned> outputRangeBounds,
    std::pair<unsigned, unsigned> kernelIndexRange, const ConvParams &params) {
  assert(kernelIndexRange.second >= kernelIndexRange.first);
  auto refinedOutputRangeBounds =
      getOutputRangeQuick(dim, {0, params.inputFieldShape[dim]},
                          kernelIndexRange, outputRangeBounds, params);
  if (refinedOutputRangeBounds.isExact())
    return refinedOutputRangeBounds;
  unsigned outputBegin = 0, outputEnd = 0;
  bool first = true;
  for (unsigned kernelIndex = kernelIndexRange.first;
       kernelIndex != kernelIndexRange.second; ++kernelIndex) {
    const auto trimmedOutputRange = getOutputRangeForKernelIndex(
        dim, refinedOutputRangeBounds, kernelIndex, params);
    if (trimmedOutputRange.first != trimmedOutputRange.second) {
      if (first) {
        outputBegin = trimmedOutputRange.first;
        outputEnd = trimmedOutputRange.second;
        first = false;
      } else {
        outputBegin = std::min(outputBegin, trimmedOutputRange.first);
        outputEnd = std::max(outputEnd, trimmedOutputRange.second);
      }
    }
  }
  return {outputBegin, outputEnd};
}

std::pair<unsigned, unsigned> getOutputRangeForInputRange(
    unsigned dim, std::pair<unsigned, unsigned> outputRangeBounds,
    std::pair<unsigned, unsigned> inputRange, const ConvParams &params) {
  assert(inputRange.second >= inputRange.first);
  auto refinedOutputRangeBounds = getOutputRangeQuick(
      dim, inputRange, {0, params.kernelShape[dim]}, outputRangeBounds, params);
  if (refinedOutputRangeBounds.isExact())
    return refinedOutputRangeBounds;
  unsigned outputBegin = 0, outputEnd = 0;
  bool first = true;
  for (unsigned inputIndex = inputRange.first; inputIndex != inputRange.second;
       ++inputIndex) {
    const auto trimmedOutputRange = getOutputRangeForInputIndex(
        dim, refinedOutputRangeBounds, inputIndex, params);
    if (trimmedOutputRange.first != trimmedOutputRange.second) {
      if (first) {
        outputBegin = trimmedOutputRange.first;
        outputEnd = trimmedOutputRange.second;
        first = false;
      } else {
        outputBegin = std::min(outputBegin, trimmedOutputRange.first);
        outputEnd = std::max(outputEnd, trimmedOutputRange.second);
      }
    }
  }
  return {outputBegin, outputEnd};
}

std::pair<unsigned, unsigned>
getInputRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
              unsigned kernelIndex, const ConvParams &params) {
  unsigned inputBegin = 0, inputEnd = 0;
  auto trimmedOutputRange =
      getOutputRangeForKernelIndex(dim, outputRange, kernelIndex, params);
  if (trimmedOutputRange.first != trimmedOutputRange.second) {
    inputBegin =
        getInputIndex(dim, trimmedOutputRange.first, kernelIndex, params);
    assert(inputBegin != ~0U);
    auto inputLast =
        getInputIndex(dim, trimmedOutputRange.second - 1, kernelIndex, params);
    assert(inputLast != ~0U);
    if (params.inputTransform.flip[dim]) {
      std::swap(inputBegin, inputLast);
    }
    inputEnd = inputLast + 1;
  }
  assert(inputBegin <= inputEnd);
  return {inputBegin, inputEnd};
}

std::pair<unsigned, unsigned>
getKernelRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
               unsigned inputIndex, const ConvParams &params) {
  unsigned kernelBegin = 0, kernelEnd = 0;
  auto trimmedOutputRange =
      getOutputRangeForInputIndex(dim, outputRange, inputIndex, params);
  if (trimmedOutputRange.first != trimmedOutputRange.second) {
    kernelBegin =
        getKernelIndex(dim, trimmedOutputRange.second - 1, inputIndex, params);
    assert(kernelBegin != ~0U);
    auto kernelLast =
        getKernelIndex(dim, trimmedOutputRange.first, inputIndex, params);
    assert(kernelLast != ~0U);
    if (params.kernelTransform.flip[dim]) {
      std::swap(kernelBegin, kernelLast);
    }
    kernelEnd = kernelLast + 1;
  }
  assert(kernelBegin <= kernelEnd);
  return {kernelBegin, kernelEnd};
}

std::pair<unsigned, unsigned>
getInputRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
              std::pair<unsigned, unsigned> kernelRange,
              const ConvParams &params) {
  assert(kernelRange.second >= kernelRange.first);
  auto inputRangeBounds = getInputRangeQuick(
      dim, {0, params.inputFieldShape[dim]}, kernelRange, outputRange, params);
  if (inputRangeBounds.isExact()) {
    return inputRangeBounds;
  }
  // Try to narrow the kernel range.
  kernelRange = getKernelRangeQuick(dim, inputRangeBounds, kernelRange,
                                    outputRange, params);
  if (kernelRange.first == kernelRange.second)
    return {0, 0};
  unsigned inputEnd = 0;
  unsigned minBegin = inputRangeBounds.begin();
  unsigned maxEnd = inputRangeBounds.end();
  const auto kernelRangeFwd =
      boost::irange(static_cast<int>(kernelRange.first),
                    static_cast<int>(kernelRange.second), 1);
  const auto kernelRangeBwd =
      boost::irange(static_cast<int>(kernelRange.second) - 1,
                    static_cast<int>(kernelRange.first) - 1, -1);
  bool flip = params.inputTransform.flip[dim];
  for (unsigned k : flip ? kernelRangeFwd : kernelRangeBwd) {
    auto inputRange = getInputRange(dim, outputRange, k, params);
    if (inputRange.first == inputRange.second)
      continue;
    inputEnd = std::max(inputEnd, inputRange.second);
    if (inputEnd == maxEnd)
      break;
  }
  unsigned inputBegin = inputEnd;
  for (unsigned k : flip ? kernelRangeBwd : kernelRangeFwd) {
    auto inputRange = getInputRange(dim, outputRange, k, params);
    if (inputRange.first == inputRange.second)
      continue;
    inputBegin = std::min(inputBegin, inputRange.first);
    if (inputBegin == minBegin)
      break;
  }
  return {inputBegin, inputEnd};
}

std::pair<unsigned, unsigned>
getKernelRange(unsigned dim, std::pair<unsigned, unsigned> outputRange,
               std::pair<unsigned, unsigned> inputRange,
               const ConvParams &params) {
  assert(inputRange.second >= inputRange.first);
  auto kernelRangeBounds = getKernelRangeQuick(
      dim, inputRange, {0, params.kernelShape[dim]}, outputRange, params);
  if (kernelRangeBounds.isExact()) {
    return kernelRangeBounds;
  }
  // Try to narrow the input range.
  inputRange = getInputRangeQuick(dim, inputRange, kernelRangeBounds,
                                  outputRange, params);
  if (inputRange.first == inputRange.second)
    return {0, 0};
  unsigned kernelEnd = 0;
  unsigned minBegin = kernelRangeBounds.begin();
  unsigned maxEnd = kernelRangeBounds.end();
  const auto inputRangeFwd =
      boost::irange(static_cast<int>(inputRange.first),
                    static_cast<int>(inputRange.second), 1);
  const auto inputRangeBwd =
      boost::irange(static_cast<int>(inputRange.second) - 1,
                    static_cast<int>(inputRange.first) - 1, -1);
  bool flip = params.inputTransform.flip[dim];
  for (unsigned i : flip ? inputRangeBwd : inputRangeFwd) {
    auto kernelRange = getKernelRange(dim, outputRange, i, params);
    if (kernelRange.first == kernelRange.second)
      continue;
    kernelEnd = std::max(kernelEnd, kernelRange.second);
    if (kernelEnd == maxEnd)
      break;
  }
  unsigned kernelBegin = kernelEnd;
  for (unsigned i : flip ? inputRangeFwd : inputRangeBwd) {
    auto kernelRange = getKernelRange(dim, outputRange, i, params);
    if (kernelRange.first == kernelRange.second)
      continue;
    kernelBegin = std::min(kernelBegin, kernelRange.first);
    if (kernelBegin == minBegin)
      break;
  }
  return {kernelBegin, kernelEnd};
}

ConvParams getGradientParams(const ConvParams &params_) {
  const CanonicalConvParams canonicalParams(params_);
  // Note: we assume the caller explicitly flips the weights in each spatial
  // axis before the convolution. TODO: T12889 It may be more efficient to fold
  // the flipping of the weights into the convolution by setting the flipKernel
  // parameter appropriately.
  auto bwdInputPaddingLower = canonicalParams->outputTransform.truncationLower;
  auto bwdInputPaddingUpper = canonicalParams->outputTransform.truncationUpper;
  const auto numFieldDims = canonicalParams->getNumFieldDims();
  // The "valid" convolution in the forward pass becomes a "full" convolution
  // in the backward pass. We can express this as a "valid" convolution with
  // (kernelSize - 1) padding.
  for (unsigned dim = 0; dim != numFieldDims; ++dim) {
    const auto kernelSize = canonicalParams->getTransformedKernelSize(dim);
    bwdInputPaddingLower[dim] += kernelSize - 1;
    bwdInputPaddingUpper[dim] += kernelSize - 1;
  }
  // Going backwards the weights are flipped in each axis so we must flip
  // the upper and lower truncation / padding.
  auto bwdFlipInput = std::vector<bool>(numFieldDims);
  auto bwdFlipKernel = canonicalParams->kernelTransform.flip;
  for (unsigned dim = 0; dim != numFieldDims; ++dim) {
    if (canonicalParams->inputTransform.flip[dim]) {
      // If the input is flipped in the forward pass we must flip the output
      // in the backward pass. This is equivalent to flipping both the
      // input and the kernel in the backward pass.
      bwdFlipKernel[dim] = !bwdFlipKernel[dim];
      bwdFlipInput[dim] = !bwdFlipInput[dim];
    }
  }

  const poplin::ConvParams::InputTransform inputTransform{
      canonicalParams->outputTransform.paddingLower, // Truncation lower
      canonicalParams->outputTransform.paddingUpper, // Truncation upper
      canonicalParams->outputTransform.stride,       // Dilation
      bwdInputPaddingLower,                          // Padding lower
      bwdInputPaddingUpper,                          // Padding upper
      bwdFlipInput                                   // Flip
  };
  const poplin::ConvParams::InputTransform kernelTransform{
      canonicalParams->kernelTransform.truncationUpper, // Truncation lower
      canonicalParams->kernelTransform.truncationLower, // Truncation upper
      canonicalParams->kernelTransform.dilation,        // Dilation
      canonicalParams->kernelTransform.paddingUpper,    // Padding lower
      canonicalParams->kernelTransform.paddingLower,    // Padding upper
      bwdFlipKernel                                     // Flip
  };
  const poplin::ConvParams::OutputTransform outputTransform{
      canonicalParams->inputTransform.paddingLower,    // Truncation lower
      canonicalParams->inputTransform.paddingUpper,    // Truncation upper
      canonicalParams->inputTransform.dilation,        // Stride
      canonicalParams->inputTransform.truncationLower, // Padding lower
      canonicalParams->inputTransform.truncationUpper  // Padding upper
  };
  const poplin::ConvParams bwdParams{
      canonicalParams->inputType,
      canonicalParams->outputType,
      canonicalParams->batchSize,
      canonicalParams->getOutputFieldShape(),
      canonicalParams->kernelShape,
      canonicalParams->getNumOutputChansPerConvGroup(),
      canonicalParams->getNumInputChansPerConvGroup(),
      canonicalParams->getNumConvGroups(),
      inputTransform,
      kernelTransform,
      outputTransform};
  return bwdParams.canonicalize();
}

} // namespace poplin
