// Copyright (c) 2020 Graphcore Ltd. All rights reserved.
/** \file Loop.hpp
 *
 * Functions to provide counted loops of programs.
 *
 */

#ifndef popops_Loop_hpp
#define popops_Loop_hpp

#include <poplar/Graph.hpp>
#include <poplar/Program.hpp>
#include <popops/ElementWise.hpp>
#include <poputil/DebugInfo.hpp>
#include <poputil/TileMapping.hpp>
#include <poputil/VertexTemplates.hpp>
#include <poputil/exceptions.hpp>
#include <string>

namespace popops {

using CountedLoopBodyType =
    std::function<poplar::program::Program(const poplar::Tensor &)>;

/** Create a loop program with constant initial count, increment and end value.
 *  The loop count is passed to the body program.
 *
 *  The program is equivalent to:
 *  \code
 *  for(unsigned i = begin; i != end; i += step){
 *    body;
 *  }
 *  \endcode
 *
 * \param graph        The graph the loop program will be added to.
 * \param begin        Initial counter value.
 * \param end          Counter end value (exclusive).
 * \param step         The increment added on each loop pass (must be greater
 *                     than zero).
 * \param body         The loop body program to run on each loop pass.
 * \param debugContext Optional debug information.
 *
 * \return             A program providing the above loop function.
 */
inline poplar::program::Sequence
countedLoop(poplar::Graph &graph, std::size_t begin, std::size_t end,
            size_t step, const CountedLoopBodyType &body,
            const poplar::DebugContext &debugContext = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(begin, end, step));

  if (step == 0) {
    throw poputil::poplibs_error(
        "countedLoop: step must be greater than zero.");
  }

  poplar::program::Sequence prog({}, {di});
  if (begin >= end) {
    throw poputil::poplibs_error("countedLoop: begin must be less than end");
  }

  poplar::Tensor tInductionVar =
      graph.addVariable(poplar::UNSIGNED_INT, {1}, {di});
  poplar::Tensor tBegin = graph.addConstant(
      poplar::UNSIGNED_INT, {1}, begin, {di, "begin-" + std::to_string(begin)});
  poplar::Tensor tStep = graph.addConstant(
      poplar::UNSIGNED_INT, {1}, step, {di, "step-" + std::to_string(step)});

  graph.setTileMapping(tInductionVar, 0);
  graph.setTileMapping(tBegin, graph.getTileMapping(tInductionVar));
  graph.setTileMapping(tStep, graph.getTileMapping(tInductionVar));

  prog.add(poplar::program::Copy(tBegin, tInductionVar, false, {di}));

  poplar::program::Sequence bodyProg = {body(tInductionVar)};
  popops::addInPlace(graph, tInductionVar, tStep, bodyProg, {di});

  std::size_t count = (end - begin + step - 1) / step;
  prog.add(poplar::program::Repeat(count, bodyProg, {di}));
  di.addOutputs(DI_ARGS(prog));
  return prog;
}

/** Create a loop program which executes \p count times.
 *  The loop count is passed to the body program.
 *
 *  The program is equivalent to:
 *  \code
 *  for(unsigned i = 0; i != count; i += 1){
 *    body;
 *  }
 *  \endcode
 *  This is equivalent to poplar::Program::Repeat but with a loop counter
 *  that is passed to the body program. (It is actually implemented using
 *  poplar::Program::RepeatWhileTrue with a test for the count variable
 *  reaching \p count.)
 *
 * \param graph        The graph the loop program will be added to.
 * \param count        Number of loop iterations to execute.
 * \param body         The loop body program to run on each loop pass.
 * \param debugContext Optional debug information.
 *
 * \return             A program providing the above loop function.
 */
inline poplar::program::Sequence
countedLoop(poplar::Graph &graph, std::size_t count,
            const CountedLoopBodyType &body,
            const poplar::DebugContext &debugContext = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(count));

  auto prog = countedLoop(graph, 0, count, 1, body, {di});
  di.addOutputs(DI_ARGS(prog));
  return prog;
}

inline poplar::Tensor addForLoopCounterVertex(poplar::Graph &graph,
                                              const poplar::Tensor &count,
                                              const poplar::Tensor &countLimit,
                                              int countStep, unsigned tile,
                                              poplar::program::Sequence &prog,
                                              const poplar::DebugContext &di) {
  auto predicate = graph.addVariable(poplar::UNSIGNED_INT, {}, di);
  graph.setTileMapping(predicate, tile);

  auto cs = graph.addComputeSet(di);
  const auto vertex =
      graph.addVertex(cs, poputil::templateVertex("popops::ForLoopCounter",
                                                  count.elementType()));
  graph.setTileMapping(vertex, tile);

  graph.connect(vertex["count"], count.reshape({}));
  graph.connect(vertex["limit"], countLimit.reshape({}));
  graph.connect(vertex["comparisonResult"], predicate);
  graph.setInitialValue(vertex["increment"], countStep);

  prog.add(poplar::program::Execute(cs));

  return predicate;
}

/** Create a for-loop program with constant initial count and increment, and a
 *  tensor as the end value.
 *  The use of a tensor as the loop end value means that the number of
 *  iterations can be calculated at run time.
 *  The loop count variable \p count is provided by the program that calls the
 *  loop program so it can be passed to the body program.
 *
 *  The program is equivalent to:
 *  \code
 *  for(unsigned count = initialCount; count != countLimit; count += countStep){
 *    body;
 *  }
 *  \endcode
 *
 * \param graph        The graph the loop program will be added to.
 * \param count        The loop count tensor, available to the \p body program
 *                     with element type INT or UNSIGNED_INT. Value initialised
 *                     by this function.
 * \param initialCount Initial counter value.
 * \param countLimit   Count limit tensor.
 * \param countStep    The increment added to the \p count tensor on each loop
 *                     pass.
 * \param body         The loop body program to run on each loop pass.
 * \param debugContext Optional debug information.
 *
 * \return             A program providing the above loop function.
 */

inline poplar::program::Sequence
countedForLoop(poplar::Graph &graph, const poplar::Tensor &count,
               int initialCount, const poplar::Tensor &countLimit,
               int countStep, const poplar::program::Program &body,
               const poplar::DebugContext &debugContext = {}) {
  poputil::PoplibsOpDebugInfo di(
      debugContext, DI_ARGS(count, initialCount, countLimit, countStep));

  const auto tensorType = count.elementType();
  if (tensorType != countLimit.elementType()) {
    throw poputil::poplibs_error(
        "countedForLoop: count and countLimit tensors must have the same type");
  }
  if (tensorType != poplar::UNSIGNED_INT && tensorType != poplar::INT) {
    throw poputil::poplibs_error("countedForLoop: count must have type INT "
                                 " or UNSIGNED_INT");
  }
  poplar::program::Sequence prog;
  // An initialiser, decremented by 1 step, so that when pre-incremented in the
  // `cond` program the loop body has a count variable visible that counts:
  // initialCount, initialCount + countStep, initialCount +2 * countStep ...
  auto initialiser = graph.addConstant<unsigned>(
      count.elementType(), {}, static_cast<unsigned>(initialCount - countStep),
      di);
  graph.setTileMapping(initialiser, 0);
  prog.add(poplar::program::Copy(initialiser, count));

  poplar::program::Sequence cond;
  auto predicate =
      addForLoopCounterVertex(graph, count, countLimit, countStep, 0, cond, di);

  prog.add(poplar::program::RepeatWhileTrue(cond, predicate.reshape({}), body,
                                            {di, "countedLoop"}));
  di.addOutputs(DI_ARGS(prog));
  return prog;
}

/** Create a for loop program with constant initial count and increment and a
 *  tensor as the end value.
 *  The use of a tensor as the loop end value means that the number of
 *  iterations can be calculated at run time.
 *  The count tensor is created internally and is not available to the body
 *  program.
 *
 *  The program is equivalent to:
 *  \code
 *  for(unsigned count = initialCount; count != countLimit; count += countStep){
 *    body;
 *  }
 *  \endcode
 *
 * \param graph        The graph the loop program will be added to.
 * \param initialCount Initial counter value.
 * \param countLimit   Count limit tensor.
 * \param countStep    The increment added to the \p count tensor on each loop
 *                     pass.
 * \param body         The loop body program to run on each loop pass.
 * \param debugContext Optional debug information.
 *
 * \return             A program providing the above loop function
 */

inline poplar::program::Sequence
countedForLoop(poplar::Graph &graph, int initialCount,
               const poplar::Tensor &countLimit, int countStep,
               const poplar::program::Program &body,
               const poplar::DebugContext &debugContext = {}) {
  auto count = graph.addVariable(countLimit.elementType(), {}, debugContext);
  graph.setTileMapping(count, 0);
  return countedForLoop(graph, count, initialCount, countLimit, countStep, body,
                        debugContext);
}
} // namespace popops

#endif // popops_Loop_hpp
