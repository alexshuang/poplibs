// Copyright (c) 2018, Graphcore Ltd, All rights reserved.

#ifndef popnn_Loss_hpp
#define popnn_Loss_hpp

namespace popnn {

enum LossType {
  SUM_SQUARED_LOSS,
  CROSS_ENTROPY_LOSS
};

} // end namespace popnn

#ifndef __POPC__
#include "popops/EncodingConstants.hpp"
#include <poplar/Graph.hpp>
#include <poplar/Program.hpp>
#include <poplar/Tensor.hpp>

namespace popnn {

/** Calculate loss and gradient for a set of activations and
 *  expected labels.
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param modelOutputs   2D tensor of model outputs per-batch to calculate
 *                        loss for.
 *  \param expected       Labels per-batch. Elements of the expected labels
 *                        may be masked by using MASKED_LABEL_CODE. Such
 *                        labels will not contribute to loss calculation.
 *  \param loss           Tensor to store the loss per-batch
 *  \param deltas         Tensor to store deltas for each activation from
 *                        the expected per-batch.
 *  \param lossType       Method for calculating loss measurement.
 *  \param debugPrefix    Optional debug prefix for operations and tensors
 *                        for this operation.
 */
poplar::program::Program
calcLoss(poplar::Graph &graph,
         const poplar::Tensor& modelOutputs,
         const poplar::Tensor& expected,
         const poplar::Tensor& loss,
         const poplar::Tensor& deltas,
         LossType lossType,
         const std::string &debugPrefix = "");

/** Calculate loss, gradient, and number of correct classifications
 *  per-batch for a set of activations and expected labels.
 *  Elements of the expected labels may be masked by using
 *  MASKED_LABEL_CODE. Such labels will not contribute to the accuracy
 *  and loss calculation.
 *
 *  \see calcLoss, and \see calcAccuracy which this function is simply
 *  a combination of.
 */
poplar::program::Program
calcLoss(poplar::Graph &graph,
         const poplar::Tensor &modelOutputs,
         const poplar::Tensor &expected,
         const poplar::Tensor &loss,
         const poplar::Tensor &deltas,
         const poplar::Tensor &numCorrect,
         LossType lossType,
         const std::string &debugPrefix = "");

/** Calculate the number of correct classifications for a set of
 *  activations and expected labels.
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param modelOutputs   2D tensor of model outputs per-batch to calculate
 *                        loss for.
 *  \param expected       Labels per-batch. Elements of the expected labels
 *                        may be masked by using MASKED_LABEL_CODE. Such
 *                        labels will not contribute to the accuracy
 *                        calculation.

 *  \param numCorrect     Tensor to store the number of correct
 *                        classifications. Must be scalar, or single-element
 *                        Tensor.
 *  \param activationType Device type used for activations.
 *  \param expectedType   Device type used for expected labels.
 *  \param debugPrefix    Optional debug prefix for operations and tensors
 *                        for this operation.
 */
poplar::program::Program
calcAccuracy(poplar::Graph &graph,
             const poplar::Tensor &modelOutputs,
             const poplar::Tensor &expected,
             const poplar::Tensor &numCorrect,
             const std::string &debugPrefix = "");

/** compute argmax for each of the outer dimensions of \a input tensor. i.e.
 *  If \a input is a tensor of dim [y][x] then argmax is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugPrefix    Optional debug prefix for operations and tensors
 *                        for this operation.
 */
poplar::Tensor
argMax(poplar::Graph &graph,
       const poplar::Tensor &input,
       poplar::program::Sequence &prog,
       const std::string &debugPrefix = "");

/** compute argmin for each of the outer dimensions of \a input tensor. i.e.
 *  If \a input is a tensor of dim [y][x] then argmin is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugPrefix    Optional debug prefix for operations and tensors
 *                        for this operation.
 */
poplar::Tensor argMin(poplar::Graph &graph, const poplar::Tensor &input,
                      poplar::program::Sequence &prog,
                      const std::string &debugPrefix = "");

} // end namespace popnn

#endif // !__POPC__


#endif // popnn_Loss_hpp
