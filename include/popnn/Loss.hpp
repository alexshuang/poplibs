// Copyright (c) 2016 Graphcore Ltd. All rights reserved.
/** \file
 *  Loss and gradient calculations.
 */

#ifndef popnn_Loss_hpp
#define popnn_Loss_hpp

namespace popnn {

enum LossType { SUM_SQUARED_LOSS, CROSS_ENTROPY_LOSS };

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
 *  \param graph              Graph to add operations and tensors to.
 *  \param modelOutputs       2D tensor of model outputs per-batch to calculate
 *                            loss for.
 *  \param expected           One-hot encoded tensor (Labels per-batch) with the
 *                            same number of rows as modelOutputs. Elements of
 *                            the expected labels may be masked by using
 *                            MASKED_LABEL_CODE. Such labels will not contribute
 *                            to loss calculation.
 *  \param loss               1D Tensor to store the loss per-batch. Has
 *                            the same number of rows as modelOutputs.
 *  \param deltas             2D Tensor to store deltas for each activation from
 *                            the expected per-batch. Has the same dimensions
 *                            as modelOutputs.
 *  \param deltasScale        Optional Tensor to scale output deltas with when
 *                            the lossType is CROSS_ENTROPY_LOSS.  Scaling will
 *                            be deltasScale / modelOutputScaling. If no tensor
 *                            is specified a default will be created
 *                            initialised with 1.0.
 *  \param modelOutputScaling Optional Tensor indicating the scaling of the
 *                            modelOutputs when lossType is CROSS_ENTROPY_LOSS,
 *                            normally from a softMax layer when the
 *                            nonLinearity used is SOFTMAX_SCALED. If no tensor
 *                            is specified a default will be created
 *                            initialised with 1.0.
 *  \param lossType           Method for calculating loss measurement.
 *  \param debugContext       Optional debug information.
 */
poplar::program::Program
calcLoss(poplar::Graph &graph, const poplar::Tensor &modelOutputs,
         const poplar::Tensor &expected, const poplar::Tensor &loss,
         const poplar::Tensor &deltas, const poplar::Tensor &deltasScale,
         const poplar::Tensor &modelOutputScaling, LossType lossType,
         const poplar::DebugContext &debugContext = {});

poplar::program::Program
calcLoss(poplar::Graph &graph, const poplar::Tensor &modelOutputs,
         const poplar::Tensor &expected, const poplar::Tensor &loss,
         const poplar::Tensor &deltas, LossType lossType,
         const poplar::DebugContext &debugContext = {});

/** Calculate loss, gradient, and number of correct classifications
 *  per-batch for a set of activations and expected labels.
 *  Elements of the expected labels may be masked by using
 *  MASKED_LABEL_CODE. Such labels will not contribute to the accuracy
 *  and loss calculation.
 *
 *  \see `calcLoss`, and `calcAccuracy` which this function is simply
 *  a combination of.
 */
poplar::program::Program
calcLoss(poplar::Graph &graph, const poplar::Tensor &modelOutputs,
         const poplar::Tensor &expected, const poplar::Tensor &loss,
         const poplar::Tensor &deltas, const poplar::Tensor &deltasScale,
         const poplar::Tensor &modelOutputScaling,
         const poplar::Tensor &numCorrect, LossType lossType,
         const poplar::DebugContext &debugContext = {});

poplar::program::Program
calcLoss(poplar::Graph &graph, const poplar::Tensor &modelOutputs,
         const poplar::Tensor &expected, const poplar::Tensor &loss,
         const poplar::Tensor &deltas, const poplar::Tensor &numCorrect,
         LossType lossType, const poplar::DebugContext &debugContext = {});

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
 *  \param debugContext   Optional debug information.
 */
poplar::program::Program
calcAccuracy(poplar::Graph &graph, const poplar::Tensor &modelOutputs,
             const poplar::Tensor &expected, const poplar::Tensor &numCorrect,
             const poplar::DebugContext &debugContext = {});

/** \deprecated
 *  **deprecated** Use popops::topKKeyValue() instead.
 *
 * Compute argmax for each of the outer dimensions of \p input tensor.
 *
 *  If \p input is a tensor of dim [y][x] then argmax is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugContext   Optional debug information.
 */
poplar::Tensor argMax(poplar::Graph &graph, const poplar::Tensor &input,
                      poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {});

/** Compute max and argmax for each of the outer dimensions of \p input tensor.
 *
 *  If \p input is a tensor of dim [y][x] then max and argmax is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugContext   Optional debug information.
 */
std::pair<poplar::Tensor, poplar::Tensor>
maxAndArgMax(poplar::Graph &graph, const poplar::Tensor &input,
             poplar::program::Sequence &prog,
             const poplar::DebugContext &debugContext = {});

/** \deprecated
 *  **deprecated** Use popops::topKKeyValue() instead.
 *
 * Compute argmin for each of the outer dimensions of \p input tensor.
 *
 *  If \p input is a tensor of dim [y][x] then argmin is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugContext   Optional debug information.
 */
poplar::Tensor argMin(poplar::Graph &graph, const poplar::Tensor &input,
                      poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {});

/** Compute min and argmin for each of the outer dimensions of \p input tensor.
 *
 *  If \p input is a tensor of dim [y][x] then argmin is computed over x
 *  elements for each of the y outer dimension elements
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugContext   Optional debug information.
 */
std::pair<poplar::Tensor, poplar::Tensor>
minAndArgMin(poplar::Graph &graph, const poplar::Tensor &input,
             poplar::program::Sequence &prog,
             const poplar::DebugContext &debugContext = {});

/** \deprecated
 *  **deprecated** Use popops::topK() instead.
 *
 * Find the top K elements of |input|. Takes a 2D tensor in the form of
 * [batch][values] and will return a tensor in the shape of [batch][K] where K
 * is the max values of each batch of values.
 *
 *  \param graph          Graph to add operations and tensors to.
 *  \param input          2D tensor of inputs
 *  \param indices        The tensor to store the indices in.
 *  \param K              The number of values to return.
 *  \param sort           If true values will be sorted in descending order.
 *  \param prog           Program to which the graph for this operation is added
 *  \param debugContext   Optional debug information.
 */
poplar::Tensor topK(poplar::Graph &graph, const poplar::Tensor &input,
                    poplar::Tensor &indices, unsigned K, bool sort,
                    poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {});

} // end namespace popnn

#endif // !__POPC__

#endif // popnn_Loss_hpp
