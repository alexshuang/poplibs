// Copyright (c) 2017 Graphcore Ltd. All rights reserved.

#ifndef popnn_BatchNorm_hpp
#define popnn_BatchNorm_hpp
#include "poplar/DebugContext.hpp"
#include "poplar/Program.hpp"
#include "poplar/Tensor.hpp"
#include <utility>

/* **Batch normalisation options**
 *
 * Presently there are no options that affect the operation of batch norm.
 * Options are included in the header prototypes for consistency with other
 * normalisation functions.
 */

namespace popnn {
namespace bn {

/// Estimate mean and inverse of standard deviation of batched activations.
std::pair<poplar::Tensor, poplar::Tensor>
batchNormStatistics(poplar::Graph &graph, const poplar::Tensor acts, float eps,
                    poplar::program::Sequence &prog, bool unbiasedVarEstimate,
                    bool stableAlgo = false,
                    const poplar::Type &partialsType = poplar::FLOAT,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {});

/// Whiten activations given mean and standard deviation.
poplar::Tensor batchNormWhiten(poplar::Graph &graph, const poplar::Tensor &acts,
                               const poplar::Tensor &mean,
                               const poplar::Tensor &invStdDev,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {});

/// Batch normalise activations given mean, standard deviation and batch norm
/// parameters.
/// The result is two tensors
///   1. normalised activations
///   2. whitened activations
std::pair<poplar::Tensor, poplar::Tensor>
batchNormalise(poplar::Graph &graph, const poplar::Tensor &acts,
               const poplar::Tensor &gamma, const poplar::Tensor &beta,
               const poplar::Tensor &mean, const poplar::Tensor &invStdDev,
               poplar::program::Sequence &prog,
               const poplar::DebugContext &debugContext = {},
               const poplar::OptionFlags &options = {});

/// Computes the output of batch normalisation given:
///   - combinedMultiplicand = gamma / stdDev
///   - addend = beta - gamma * mean / stdDev
poplar::Tensor batchNormalise(poplar::Graph &graph, const poplar::Tensor &acts,
                              const poplar::Tensor &combinedMultiplicand,
                              const poplar::Tensor &addend,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {});

/// Compute gradients w.r.t parameters required for parameter update.
std::pair<poplar::Tensor, poplar::Tensor> batchNormParamGradients(
    poplar::Graph &graph, const poplar::Tensor &acts,
    const poplar::Tensor &gradsIn, const poplar::Tensor &mean,
    const poplar::Tensor &iStdDev, poplar::program::Sequence &prog,
    const poplar::Type &partialsType = poplar::FLOAT,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {});

/// Compute gradients w.r.t parameters required for parameter update.
std::pair<poplar::Tensor, poplar::Tensor> batchNormParamGradients(
    poplar::Graph &graph, const poplar::Tensor &actsWhitened,
    const poplar::Tensor &gradsIn, poplar::program::Sequence &prog,
    const poplar::Type &partialsType = poplar::FLOAT,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {});

/// Compute gradients w.r.t input activations for the batch norm layer.
/// i.e. gradients are propagated through the complete layer including
/// statistics computation.
poplar::Tensor
batchNormGradients(poplar::Graph &graph, const poplar::Tensor &acts,
                   const poplar::Tensor &gradsIn, const poplar::Tensor &mean,
                   const poplar::Tensor &invStdDev, const poplar::Tensor &gamma,
                   poplar::program::Sequence &prog,
                   const poplar::Type &partialsType = poplar::FLOAT,
                   const poplar::DebugContext &debugContext = {},
                   const poplar::OptionFlags &options = {});

/// Compute gradients w.r.t input activations for the batch norm layer.
/// i.e. gradients are propagated through the complete layer including
/// statistics computation.
poplar::Tensor
batchNormGradients(poplar::Graph &graph, const poplar::Tensor &actsWhitened,
                   const poplar::Tensor &gradsIn,
                   const poplar::Tensor &invStdDev, const poplar::Tensor &gamma,
                   poplar::program::Sequence &prog,
                   const poplar::Type &partialsType = poplar::FLOAT,
                   const poplar::DebugContext &debugContext = {},
                   const poplar::OptionFlags &options = {});

void batchNormParamUpdate(poplar::Graph &graph,
                          const poplar::Tensor &gammaDelta,
                          const poplar::Tensor &betaDelta, float scale,
                          poplar::Tensor &gamma, poplar::Tensor &beta,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {});

void batchNormParamUpdate(poplar::Graph &graph,
                          const poplar::Tensor &gammaDelta,
                          const poplar::Tensor &betaDelta,
                          const poplar::Tensor &scale, poplar::Tensor &gamma,
                          poplar::Tensor &beta, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {});
} // namespace bn
} // namespace popnn
#endif // popnn_BatchNorm_hpp
