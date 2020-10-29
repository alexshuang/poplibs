// Copyright (c) 2019 Graphcore Ltd. All rights reserved.

#ifndef popnn_InstanceNorm_hpp
#define popnn_InstanceNorm_hpp
#include "popnn/GroupNorm.hpp"

// Instance norm uses group norm with number of groups = number of channels

/* **Instance normalisation options**
 *
 * As instance norm uses group norm, options are passed through - see the group
 * norm header documentation for the option list.
 */

namespace popnn {
namespace in {

/// Estimate mean and inverse of standard deviation of activations.
inline std::pair<poplar::Tensor, poplar::Tensor>
instanceNormStatistics(poplar::Graph &graph, const poplar::Tensor acts,
                       float eps, poplar::program::Sequence &prog,
                       bool unbiasedVarEstimate, bool stableAlgo,
                       const poplar::Type &partialsType = poplar::FLOAT,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormStatistics(
      graph, acts, eps, prog, acts.dim(1), unbiasedVarEstimate, stableAlgo,
      partialsType, debugContext.getPathName(), options);
}

/// Whiten activations given mean and standard deviation.
inline poplar::Tensor
instanceNormWhiten(poplar::Graph &graph, const poplar::Tensor &acts,
                   const poplar::Tensor &mean, const poplar::Tensor &invStdDev,
                   poplar::program::Sequence &prog,
                   const poplar::DebugContext &debugContext = {},
                   const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormWhiten(graph, acts, mean, invStdDev, prog,
                                    debugContext.getPathName(), options);
}

/// Instance normalise activations given mean, standard deviation and norm
/// parameters.
///
/// The result is two tensors
///   1. normalised activations
///   2. whitened activations
inline std::pair<poplar::Tensor, poplar::Tensor>
instanceNormalise(poplar::Graph &graph, const poplar::Tensor &acts,
                  const poplar::Tensor &gamma, const poplar::Tensor &beta,
                  const poplar::Tensor &mean, const poplar::Tensor &invStdDev,
                  poplar::program::Sequence &prog,
                  const poplar::DebugContext &debugContext = {},
                  const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormalise(graph, acts, gamma, beta, mean, invStdDev,
                                   prog, debugContext.getPathName(), options);
}

/// Compute gradients w.r.t parameters for parameter update.
inline std::pair<poplar::Tensor, poplar::Tensor> instanceNormParamGradients(
    poplar::Graph &graph, const poplar::Tensor &acts,
    const poplar::Tensor &gradsIn, const poplar::Tensor &mean,
    const poplar::Tensor &iStdDev, poplar::program::Sequence &prog,
    const poplar::Type &partialsType = poplar::FLOAT,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormParamGradients(
      graph, acts, gradsIn, mean, iStdDev, prog, partialsType,
      debugContext.getPathName(), options);
}

/// Compute gradients w.r.t parameters for parameter update.
inline std::pair<poplar::Tensor, poplar::Tensor> instanceNormParamGradients(
    poplar::Graph &graph, const poplar::Tensor &actsWhitened,
    const poplar::Tensor &gradsIn, poplar::program::Sequence &prog,
    const poplar::Type &partialsType = poplar::FLOAT,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormParamGradients(
      graph, actsWhitened, gradsIn, prog, partialsType,
      debugContext.getPathName(), options);
}

/// Compute gradients w.r.t input activations for the instance norm layer.
/// Gradients are propagated through the complete layer including
/// statistics computation.
inline poplar::Tensor
instanceNormGradients(poplar::Graph &graph, const poplar::Tensor &acts,
                      const poplar::Tensor &gradsIn, const poplar::Tensor &mean,
                      const poplar::Tensor &invStdDev,
                      const poplar::Tensor &gamma,
                      poplar::program::Sequence &prog,
                      const poplar::Type &partialsType = poplar::FLOAT,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormGradients(graph, acts, gradsIn, mean, invStdDev,
                                       gamma, prog, partialsType,
                                       debugContext.getPathName(), options);
}

/// Compute gradients w.r.t input activations for the instance norm layer.
/// Gradients are propagated through the complete layer including
/// statistics computation.
inline poplar::Tensor instanceNormGradients(
    poplar::Graph &graph, const poplar::Tensor &actsWhitened,
    const poplar::Tensor &gradsIn, const poplar::Tensor &invStdDev,
    const poplar::Tensor &gamma, poplar::program::Sequence &prog,
    const poplar::Type &partialsType = poplar::FLOAT,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormGradients(graph, actsWhitened, gradsIn, invStdDev,
                                       gamma, prog, partialsType,
                                       debugContext.getPathName(), options);
}

/// Update parameters given gradients w.r.t. parameters.
inline void
instanceNormParamUpdate(poplar::Graph &graph, const poplar::Tensor &gammaDelta,
                        const poplar::Tensor &betaDelta, float scale,
                        poplar::Tensor &gamma, poplar::Tensor &beta,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormParamUpdate(graph, gammaDelta, betaDelta, scale,
                                         gamma, beta, prog,
                                         debugContext.getPathName(), options);
}

inline void
instanceNormParamUpdate(poplar::Graph &graph, const poplar::Tensor &gammaDelta,
                        const poplar::Tensor &betaDelta,
                        const poplar::Tensor &scale, poplar::Tensor &gamma,
                        poplar::Tensor &beta, poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  return popnn::gn::groupNormParamUpdate(graph, gammaDelta, betaDelta, scale,
                                         gamma, beta, prog,
                                         debugContext.getPathName(), options);
}

/// In flop computation, the following applies:
///   - Acts per channel:
///     - for fc layers: the total number of batches.
///     - for conv layers: the field size per channel * batch size.
///
///   - Number of channels:
///     - for fc layers: the total number of activations in a batch.
///     - for conv layers: the total number of channels.

uint64_t getFwdFlops(uint64_t numChannels, uint64_t actsPerChannel,
                     bool computeEstimates);
uint64_t getBwdFlops(uint64_t numChannels, uint64_t actsPerChannel);
uint64_t getWuFlops(uint64_t numChannels, uint64_t actsPerChannel);

} // namespace in
} // namespace popnn
#endif // popnn_InstanceNorm_hpp
