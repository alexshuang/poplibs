// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
/** \file ElementWise.hpp
 *
 *  These functions perform the same operation on each element of one or more
 *  tensors.
 *
 *  Every function has an in-place overload that writes the result of
 *  the function to the first tensor argument of the function.
 *
 *  The functions that perform operations on two tensors also have overloads for
 *  one of the tensors being a constant scalar. These functions perform the same
 *  operation on each element in the remaining tensor using the scalar as the
 *  other side of the operation for all elements.
 */

#ifndef popops_ElementWise_hpp
#define popops_ElementWise_hpp

#include <poplar/Graph.hpp>
#include <poplar/OptionFlags.hpp>
#include <poplar/Program.hpp>
#include <popops/Expr.hpp>
#include <poputil/DebugInfo.hpp>
#include <string>

namespace poputil {
template <>
poplar::ProfileValue toProfileValue(const popops::expr::UnaryOpType &op);
template <>
poplar::ProfileValue toProfileValue(const popops::expr::BinaryOpType &op);
template <>
poplar::ProfileValue toProfileValue(const popops::expr::TernaryOpType &op);
} // namespace poputil

namespace popops {

/**
 *  Writes the generated codelet for the given \p expr and \p ts to \p os.
 *
 *  \param target  The target the graph is being constructed to work with.
 *  \param expr    The expression to map across the tensors. The placeholders
 *                 in the expressions will be substituted with corresponding
 *                 elements from the tensors in \p ts.
 *  \param ts      The list of tensors to map the expression across.
 *                 If elements from these tensors are used in binary/ternary
 *                 operations in the expression the numpy-style broadcast rules
 *                 are used to match the shapes of the tensors (see
 *                 poputil::broadcastToMatch()).
 *  \param options A list of flags to pass to the expression evaluator. See
 *                 map() function for details.
 *
 *  \param os      The stream to output generated codelet.
 */
void outputGeneratedCodelet(const poplar::Target &target,
                            const expr::Expr &expr,
                            const std::vector<poplar::Tensor> &ts,
                            const poplar::OptionFlags &options,
                            std::ostream &os);

// Elementwise operations.

/** \name map
 * Map an expression across tensors.
 *
 * **Elementwise Options**
 *
 *    * `enableGenerateCodelet` (true, false) [=true]
 *
 *      When true and the following conditions are met, poplar will generate a
 *      codelet to execute the map operation. Otherwise, it will sequence
 *      poplibs codelets to create the expression.
 *      - All of the inputs are of the same size
 *      - Inputs do not alias
 *      - Multiple operations are being performed
 */
/*[INTERNAL]
 *    * `enableVectorBroadcastOptimisations` (true, false) [=true]
 *
 *      This option is only applicable if `expr` is a binary operation. If true,
 *      vector broadcasts are optimised by attempting to find the most efficient
 *      way to perform the binary operation on each tile.
 *
 *    * `forceGenerateCodelet` (true, false) [=false]
 *
 *      Always generates codelets regardless of the expression and input
 *      tensors. Intended for testing only.
 *
 *    * `enableExpressionOptimizations' (true, false_ [=true])
 *
 *      Optimize expressions to simplify them where possible.
 */
/**
 *  \param graph   The graph to update.
 *  \param expr    The expression to map across the tensors. The placeholders
 *                 in the expressions will be substituted with corresponding
 *                 elements from the tensors in \p ts.
 *  \param ts      The list of tensors to map the expression across.
 *                 If elements from these tensors are used in binary/ternary
 *                 operations in the expression the numpy-style broadcast rules
 *                 are used to match the shapes of the tensors (see
 *                 poputil::broadcastToMatch()).
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options A list of flags to pass to the expression evaluator.
 *
 *  \returns A tensor containing the elements resulting from the application of
 *           the expression across the tensors.
 *
 * @{
 */
poplar::Tensor map(poplar::Graph &graph, const expr::Expr &expr,
                   const std::vector<poplar::Tensor> &ts,
                   poplar::program::Sequence &prog,
                   const poplar::DebugContext &debugContext = {},
                   const poplar::OptionFlags &options = {});

inline poplar::Tensor map(poplar::Graph &graph, expr::UnaryOpType op,
                          const poplar::Tensor &t,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {

  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, t, options));
  auto output =
      map(graph, expr::UnaryOp(op, expr::_1), {t}, prog, {di}, options);
  di.addOutput(output);
  return output;
}

inline poplar::Tensor map(poplar::Graph &graph, expr::BinaryOpType op,
                          const poplar::Tensor &a, const poplar::Tensor &b,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, a, b, options));
  auto output = map(graph, expr::BinaryOp(op, expr::_1, expr::_2), {a, b}, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

inline poplar::Tensor map(poplar::Graph &graph, expr::TernaryOpType op,
                          const poplar::Tensor &a, const poplar::Tensor &b,
                          const poplar::Tensor &c,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, a, b, c, options));
  auto output = map(graph, expr::TernaryOp(op, expr::_1, expr::_2, expr::_3),
                    {a, b, c}, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name mapInPlace
 * Update the input tensors with the result of map().
 *
 * @{
 */
void mapInPlace(poplar::Graph &graph, const expr::Expr &expr,
                const std::vector<poplar::Tensor> &ts,
                poplar::program::Sequence &prog,
                const poplar::DebugContext &debugContext = {},
                const poplar::OptionFlags &options = {});

inline void mapInPlace(poplar::Graph &graph, expr::UnaryOpType op,
                       const poplar::Tensor &t, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, t, options));

  mapInPlace(graph, expr::UnaryOp(op, expr::_1), {t}, prog, {di}, options);
}

inline void mapInPlace(poplar::Graph &graph, expr::BinaryOpType op,
                       const poplar::Tensor &a, const poplar::Tensor &b,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, a, b, options));

  mapInPlace(graph, expr::BinaryOp(op, expr::_1, expr::_2), {a, b}, prog, {di},
             options);
}

inline void mapInPlace(poplar::Graph &graph, expr::TernaryOpType op,
                       const poplar::Tensor &a, const poplar::Tensor &b,
                       const poplar::Tensor &c, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, a, b, c, options));

  mapInPlace(graph, expr::TernaryOp(op, expr::_1, expr::_2, expr::_3),
             {a, b, c}, prog, {di}, options);
}
/** @} */

/** \name mapWithOutput
 * Write the result of map() to the given output tensor.
 *
 * @{
 */
void mapWithOutput(poplar::Graph &graph, const expr::Expr &expr,
                   const std::vector<poplar::Tensor> &ts,
                   const poplar::Tensor &out, poplar::program::Sequence &prog,
                   const poplar::DebugContext &debugContext = {},
                   const poplar::OptionFlags &options = {});

inline void mapWithOutput(poplar::Graph &graph, expr::UnaryOpType op,
                          const poplar::Tensor &in, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, in, out, options));

  mapWithOutput(graph, expr::UnaryOp(op, expr::_1), {in}, out, prog, {di},
                options);
}

inline void mapWithOutput(poplar::Graph &graph, expr::BinaryOpType op,
                          const poplar::Tensor &a, const poplar::Tensor &b,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(op, a, b, out, options));

  mapWithOutput(graph, expr::BinaryOp(op, expr::_1, expr::_2), {a, b}, out,
                prog, {di}, options);
}

inline void mapWithOutput(poplar::Graph &graph, expr::TernaryOpType op,
                          const poplar::Tensor &a, const poplar::Tensor &b,
                          const poplar::Tensor &c, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext,
                                 DI_ARGS(op, a, b, c, out, options));

  mapWithOutput(graph, expr::TernaryOp(op, expr::_1, expr::_2, expr::_3),
                {a, b, c}, out, prog, {di}, options);
}

/** @} */

// Unary operations

//   Common.

/** Compute the absolute value of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::abs(a)`, where \c a is an element of \p A.
 *
 */
inline poplar::Tensor abs(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::ABSOLUTE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of abs().
 */
inline void absInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::ABSOLUTE, A, prog, {di}, options);
}

/** Write the result of abs() to the given output tensor.
 */
inline void absWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::ABSOLUTE, A, out, prog, {di},
                options);
}

/** Compute the inverse of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `1 / a`, where \c a is an element of \p A.
 */
inline poplar::Tensor inv(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output = map(graph, expr::UnaryOpType::INVERSE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of inv().
 */
inline void invInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::INVERSE, A, prog, {di}, options);
}

/** Write the result of inv() to the given output tensor.
 */
inline void invWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::INVERSE, A, out, prog, {di}, options);
}

/** Compute the logical NOT of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `!a`, where \c a is an element of \p A.
 */
inline poplar::Tensor logicalNot(poplar::Graph &graph, const poplar::Tensor &A,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output =
      map(graph, expr::UnaryOpType::LOGICAL_NOT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of logicalNot().
 */
inline void logicalNotInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::LOGICAL_NOT, A, prog, {di}, options);
}

/** Write the result of logicalNot() to the given output tensor.
 */
inline void logicalNotWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::LOGICAL_NOT, A, out, prog, {di},
                options);
}

/** Compute the negation of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `-1 * a`, where \c a is an element of \p A.
 */
inline poplar::Tensor neg(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::NEGATE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of neg().
 */
inline void negInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::NEGATE, A, prog, {di}, options);
}

/** Write the result of neg() to the given output tensor.
 */
inline void negWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::NEGATE, A, out, prog, {di}, options);
}

/** Compute the signum of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is one of -1, 0 or +1 if the
 * corresponding element in \p A was less than, equal to or greater than 0
 * respectively.
 */
inline poplar::Tensor signum(poplar::Graph &graph, const poplar::Tensor &A,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::SIGNUM, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of signum().
 */
inline void signumInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::SIGNUM, A, prog, {di}, options);
}

/** Write the result of signum() to the given output tensor.
 */
inline void signumWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &out,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::SIGNUM, A, out, prog, {di}, options);
}

/** Check if each element in \p A is finite.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::isfinite(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor isFinite(poplar::Graph &graph, const poplar::Tensor &A,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output =
      map(graph, expr::UnaryOpType::IS_FINITE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Write the result of isFinite() to the given output tensor.
 */
inline void isFiniteWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                               const poplar::Tensor &out,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::IS_FINITE, A, out, prog, {di},
                options);
}

/** \name checkTypes
 * Check that the host compile-time type \p constType
 *  is compatible with the run-time IPU type \p elementType.
 *
 *  \param  elementType The run-time IPU type.
 *  \param  constant    Unused.
 *  \tparam constType   The host compile-time type.
 *
 *  \throw std::runtime_error If the types are not compatible.
 *
 * @{
 */
template <typename constType>
inline void checkTypes(poplar::Type elementType, constType) {
  if (elementType != poplar::equivalent_device_type<constType>().value) {
    throw std::runtime_error("Type mismatch between Binary op Tensor "
                             "and constant");
  }
}

template <> inline void checkTypes<float>(poplar::Type elementType, float) {
  if (elementType != poplar::FLOAT && elementType != poplar::HALF) {
    throw std::runtime_error("Type mismatch between Binary op Tensor "
                             "and constant");
  }
  return;
}

template <> inline void checkTypes<double>(poplar::Type elementType, double) {
  if (elementType != poplar::FLOAT && elementType != poplar::HALF) {
    throw std::runtime_error("Type mismatch between Binary op Tensor "
                             "and constant");
  }
  return;
}
/** @} */

//   Bitwise.

/** Compute the bitwise NOT operation for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `~a`, where \c a is an element of \p A.
 */
inline poplar::Tensor bitwiseNot(poplar::Graph &graph, const poplar::Tensor &A,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output =
      map(graph, expr::UnaryOpType::BITWISE_NOT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of bitwiseNot().
 */
inline void bitwiseNotInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::BITWISE_NOT, A, prog, {di}, options);
}

/** Write the result of bitwiseNot() to the given output tensor.
 */
inline void bitwiseNotWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::BITWISE_NOT, A, out, prog, {di},
                options);
}

/** Compute the number of binary leading zeros of each element in \p A.
 *
 *  \note If the element is zero then it is treated as 32 leading zeros.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `a ? __builtin_clz(a) : 32`, where \c a is an element of \p A.
 */
inline poplar::Tensor
countLeadingZeros(poplar::Graph &graph, const poplar::Tensor &A,
                  poplar::program::Sequence &prog,
                  const poplar::DebugContext &debugContext = {},
                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::COUNT_LEADING_ZEROS, A, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of countLeadingZeros().
 */
inline void
countLeadingZerosInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::COUNT_LEADING_ZEROS, A, prog, {di},
             options);
}

/** Write the result of countLeadingZeros() to the given output tensor.
 */
inline void
countLeadingZerosWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::COUNT_LEADING_ZEROS, A, out, prog,
                {di}, options);
}

/** Compute the number of 1 bits in each element of \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::popcount(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor popcount(poplar::Graph &graph, const poplar::Tensor &A,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::POPCOUNT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of popcount().
 */
inline void popcountInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::POPCOUNT, A, prog, {di}, options);
}

/** Write the result of popcount() to the given output tensor.
 */
inline void popcountWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                               const poplar::Tensor &out,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::POPCOUNT, A, out, prog, {di},
                options);
}

//   Rounding.

/** Compute the ceiling of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::ceil(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor ceil(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::CEIL, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of ceil().
 */
inline void ceilInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::CEIL, A, prog, {di}, options);
}

/** Write the result of ceil() to the given output tensor.
 */
inline void ceilWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::CEIL, A, out, prog, {di}, options);
}

/** Compute the floor of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::floor(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor floor(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output = map(graph, expr::UnaryOpType::FLOOR, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of floor().
 */
inline void floorInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::FLOOR, A, prog, {di}, options);
}

/** Write the result of floor() to the given output tensor.
 */
inline void floorWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::FLOOR, A, out, prog, {di}, options);
}

/** Round each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::round(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor round(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::ROUND, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of round().
 */
inline void roundInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::ROUND, A, prog, {di}, options);
}

/** Write the result of round() to the given output tensor.
 */
inline void roundWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::ROUND, A, out, prog, {di}, options);
}

//   Power functions.

/** Compute the cube-root for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::cbrt(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor cbrt(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::CBRT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of cbrt().
 */
inline void cbrtInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::CBRT, A, prog, {di}, options);
}

/** Write the result of cbrt() to the given output tensor.
 */
inline void cbrtWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::CBRT, A, out, prog, {di}, options);
}

/** Compute the exponential of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::exp(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor exp(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output = map(graph, expr::UnaryOpType::EXPONENT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of exp().
 */
inline void expInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::EXPONENT, A, prog, {di}, options);
}

/** Write the result of exp() to the given output tensor.
 */
inline void expWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::EXPONENT, A, out, prog, {di},
                options);
}

/** Compute the exponential of each element in \p A minus one.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::expm1(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor expm1(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output =
      map(graph, expr::UnaryOpType::EXPONENT_MINUS_ONE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of expm1().
 */
inline void expm1InPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::EXPONENT_MINUS_ONE, A, prog, {di},
             options);
}

/** Write the result of expm1() to the given output tensor.
 */
inline void expm1WithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::EXPONENT_MINUS_ONE, A, out, prog,
                {di}, options);
}

/** Compute the log base-e of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::log(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor log(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output =
      map(graph, expr::UnaryOpType::LOGARITHM, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of log().
 */
inline void logInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::LOGARITHM, A, prog, {di}, options);
}

/** Write the result of log() to the given output tensor.
 */
inline void logWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::LOGARITHM, A, out, prog, {di},
                options);
}

/** Compute the log base-e of each element in \p A plus one.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::log1p(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor log1p(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output =
      map(graph, expr::UnaryOpType::LOGARITHM_ONE_PLUS, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of log1p().
 */
inline void log1pInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::LOGARITHM_ONE_PLUS, A, prog, {di},
             options);
}

/** Write the result of log1p() to the given output tensor.
 */
inline void log1pWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::LOGARITHM_ONE_PLUS, A, out, prog,
                {di}, options);
}

/** Compute the square-root for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::sqrt(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor sqrt(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::SQRT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of sqrt().
 */
inline void sqrtInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::SQRT, A, prog, {di}, options);
}

/** Write the result of sqrt() to the given output tensor.
 */
inline void sqrtWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::SQRT, A, out, prog, {di}, options);
}

/** Compute the square for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `x * x`, where \c a is an element of \p A.
 */
inline poplar::Tensor square(poplar::Graph &graph, const poplar::Tensor &A,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::SQUARE, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of square().
 */
inline void squareInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::SQUARE, A, prog, {di}, options);
}

/** Write the result of square() to the given output tensor.
 */
inline void squareWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &out,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::SQUARE, A, out, prog, {di}, options);
}

/** Compute the reciprocal square root for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `1 / sqrt(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor rsqrt(poplar::Graph &graph, const poplar::Tensor &A,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::RSQRT, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of rsqrt().
 */
inline void rsqrtInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::RSQRT, A, prog, {di}, options);
}

/** Write the result of rsqrt() to the given output tensor
 */
inline void rsqrtWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::RSQRT, A, out, prog, {di}, options);
}

/** Compute the sigmoid (logistic) function for each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `1 / (1 + exp(-x))`, where \c a is an element of \p A.
 */
inline poplar::Tensor sigmoid(poplar::Graph &graph, const poplar::Tensor &A,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::SIGMOID, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of sigmoid().
 */
inline void sigmoidInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::SIGMOID, A, prog, {di}, options);
}

/** Write the result of sigmoid() to the given output tensor.
 */
inline void sigmoidWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &out,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::SIGMOID, A, out, prog, {di}, options);
}

//   Trigonometric.

/** Compute the arc-sine of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::asin(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor asin(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::ASIN, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of asin().
 */
inline void asinInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::ASIN, A, prog, {di}, options);
}

/** Write the result of asin() to the given output tensor.
 */
inline void asinWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::ASIN, A, out, prog, {di}, options);
}

/** Compute the cosine of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::cos(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor cos(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output = map(graph, expr::UnaryOpType::COS, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of cos().
 */
inline void cosInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::COS, A, prog, {di}, options);
}

/** Write the result of cos() to the given output tensor.
 */
inline void cosWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::COS, A, out, prog, {di}, options);
}

/** Compute the sine of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::sin(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor sin(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::SIN, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of sin().
 */
inline void sinInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::SIN, A, prog, {di}, options);
}

/** Write the result of sin() to the given output tensor.
 */
inline void sinWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::SIN, A, out, prog, {di}, options);
}

/** Compute the tangent of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::tan(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor tan(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::TAN, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of tan().
 */
inline void tanInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::TAN, A, prog, {di}, options);
}

/** Write the result of tan() to the given output tensor.
 */
inline void tanWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::TAN, A, out, prog, {di}, options);
}

/** Compute the hyperbolic tangent of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::tanh(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor tanh(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::TANH, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of tanh().
 */
inline void tanhInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::TANH, A, prog, {di}, options);
}

/** Write the result of tanh() to the given output tensor.
 */
inline void tanhWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::TANH, A, out, prog, {di}, options);
}

//   Statistical.

/* Variance conversion operations can be created using the map functions,
 *  but that requires the input and output to be of the same type.
 *  It can be an advantage to maintain variance in full precision and
 *  inverse standard deviation in half precision. These supplementary functions
 *  make that possible.
 */
/** \name varianceToInvStdDev
 * Convert variance to inverse standard deviation.
 *
 *  \param graph   The graph to update.
 *  \param src     The source tensor.
 *  \param epsilon A tensor initialised with the epsilon parameter used in
 *                 conversion.  Must have a single element and have the same
 *                 type as the input type.  Alternatively a float value can be
 *                 used and the appropriate tensor will be created.
 *  \param prog    The sequence to extend with the execution of conversion.
 *  \param dstType The type of the tensor to be output. Must be \c HALF
 *                 or equal to the input type.
 *  \param debugContext Optional debug information
 *
 *  \returns       A tensor where each element is the inverse of standard
 *  deviation. Each element is the result of `b = sqrt(1 / a)`, where \c a and
 *  \c b are the corresponding elements of \p src and the result tensor
 *  respectively.
 *
 * @{
 */
poplar::Tensor
varianceToInvStdDev(poplar::Graph &graph, const poplar::Tensor &src,
                    const poplar::Tensor &epsilon,
                    poplar::program::Sequence &prog,
                    const poplar::Type dstType = poplar::HALF,
                    const poplar::DebugContext &debugContext = {});

poplar::Tensor
varianceToInvStdDev(poplar::Graph &graph, const poplar::Tensor &src,
                    const float epsilon, poplar::program::Sequence &prog,
                    const poplar::Type dstType = poplar::HALF,
                    const poplar::DebugContext &debugContext = {});
/** @} */

/** \name invStdDevToVariance
 * Convert inverse standard deviation to variance.
 *
 *  \param graph   The graph to update.
 *  \param src     The source tensor.
 *  \param epsilon A tensor initialised with the epsilon parameter used in
 *                 conversion. Must have a single element and have the same
 *                 type as the input type. Alternatively, a float value can be
 *                 used and the appropriate tensor will be created.
 *  \param prog    The sequence to extend with the execution of conversion.
 *  \param dstType The type of the tensor to be output. Must be \c FLOAT
 *                 or equal to the input type.
 *  \param debugContext Optional debug information
 *
 *  \returns       A tensor where each element is the variance. Each element is
 *  the result of `b = (1 / a) ^ 2`, where \c a and \c b are the corresponding
 *  elements of \p src and the result tensor respectively.
 *
 * @{
 */
poplar::Tensor
invStdDevToVariance(poplar::Graph &graph, const poplar::Tensor &src,
                    const poplar::Tensor &epsilon,
                    poplar::program::Sequence &prog,
                    const poplar::Type dstType = poplar::FLOAT,
                    const poplar::DebugContext &debugContext = {});

poplar::Tensor
invStdDevToVariance(poplar::Graph &graph, const poplar::Tensor &src,
                    const float epsilon, poplar::program::Sequence &prog,
                    const poplar::Type dstType = poplar::FLOAT,
                    const poplar::DebugContext &debugContext = {});
/** @} */

/** Compute (1 + erf(A/sqrt(2))) * A / 2 where all the
 *  operations are element-wise, and erf is the error function. This is a very
 *   accurate implementation with low relative and absolute error.
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `geluErf(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor geluErf(poplar::Graph &graph, const poplar::Tensor &A,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  auto output = map(graph, expr::UnaryOpType::GELU_ERF, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of geluErf().
 */
inline void geluErfInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));

  mapInPlace(graph, expr::UnaryOpType::GELU_ERF, A, prog, {di}, options);
}

/** Write the result of geluErf() to the given output tensor.
 */
inline void geluErfWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &out,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));

  mapWithOutput(graph, expr::UnaryOpType::GELU_ERF, A, out, prog, {di},
                options);
}

/** Compute the error function of each element in \p A.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equivalent to the result of
 *           `std::erf(a)`, where \c a is an element of \p A.
 */
inline poplar::Tensor erf(poplar::Graph &graph, const poplar::Tensor &A,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  auto output = map(graph, expr::UnaryOpType::ERF, A, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of erf().
 */
inline void erfInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, options));
  mapInPlace(graph, expr::UnaryOpType::ERF, A, prog, {di}, options);
}

/** Write the result of erf() to the given output tensor.
 */
inline void erfWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, out, options));
  mapWithOutput(graph, expr::UnaryOpType::ERF, A, out, prog, {di}, options);
}

// Binary operations

//   Arithmetic.

/** \name add
 * Add each element in \p A to the corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a + b`, where \c a
 * and \c b are the corresponding elements of \p A and \p B tensors
 * respectively.
 *
 * @{
 */
inline poplar::Tensor add(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::ADD, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor add(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::ADD, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor add(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::ADD, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name addInPlace
 * Update the input tensor with the result of add().
 *
 * @{
 */
inline void addInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::ADD, A, B, prog, {di}, options);
}

template <typename constType>
inline void addInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::ADD, A, BTensor, prog, {di}, options);
}
/** @} */

/** \name addWithOutput
 * Write the result of add() to the given output tensor.
 *
 * @{
 */
inline void addWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::ADD, A, B, out, prog, {di}, options);
}

template <typename constType>
inline void addWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::ADD, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void addWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::ADD, ATensor, B, out, prog, {di},
                options);
}
/** @} */

/** \name sub
 * Subtract the elements of \p B from \p A and return the result in a new
 * tensor.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of elements which will be subtracted from.
 *  \param B       The tensor of elements to subtract from \p A.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to a - b, where \c a and \c b
 *  are the corresponding elements of \p A and \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor sub(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::SUBTRACT, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor sub(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::SUBTRACT, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor sub(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::SUBTRACT, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name subInPlace
 * Update the input tensor with the result of sub().
 *
 * @{
 */
inline void subInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::SUBTRACT, A, B, prog, {di}, options);
}

template <typename constType>
inline void subInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::SUBTRACT, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name subWithOutput
 * Write the result of sub() to the given output tensor.
 *
 * @{
 */
inline void subWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::SUBTRACT, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void subWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SUBTRACT, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void subWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SUBTRACT, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name mul
 * Multiply each element in \p A by the corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a * b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor mul(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::MULTIPLY, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor mul(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MULTIPLY, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor mul(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MULTIPLY, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name mulInPlace
 * Update the input tensor with the result of mul().
 *
 * @{
 */
inline void mulInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::MULTIPLY, A, B, prog, {di}, options);
}

template <typename constType>
inline void mulInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::MULTIPLY, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name mulWithg=Output
 * Write the result of mul() to the given output tensor.
 *
 * @{
 */
inline void mulWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::MULTIPLY, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void mulWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MULTIPLY, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void mulWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MULTIPLY, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name div
 * Divide each element in \p A by the corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of dividends.
 *  \param B       The tensor of divisors.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a / b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor div(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::DIVIDE, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor div(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::DIVIDE, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor div(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::DIVIDE, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name divInPlace
 * Update the input tensor with the result of div().
 *
 * @{
 */
inline void divInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::DIVIDE, A, B, prog, {di}, options);
}

template <typename constType>
inline void divInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::DIVIDE, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name divWithOutput
 * Write the result of div() to the given output tensor.
 *
 * @{
 */
inline void divWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::DIVIDE, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void divWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::DIVIDE, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void divWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::DIVIDE, ATensor, B, out, prog, {di},
                options);
}
/** @} */

/** \name pow
 * Compute each element in \p A to the power of the corresponding element in \p
 * B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of bases.
 *  \param B       The tensor of exponents.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to `pow(a, b)`, where \c a and
 *  \c b are the corresponding elements of \p A and \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor pow(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::POWER, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor pow(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::POWER, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor pow(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::POWER, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name powInPlace
 * Update the input tensor with the result of pow().
 *
 * @{
 */
inline void powInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::POWER, A, B, prog, {di}, options);
}

template <typename constType>
inline void powInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::POWER, A, BTensor, prog, {di}, options);
}
/** @} */

/** \name powWithOutput
 * Write the result of pow() to the given output tensor.
 *
 * @{
 */
inline void powWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::POWER, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void powWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::POWER, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void powWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::POWER, ATensor, B, out, prog, {di},
                options);
}
/** @} */

/** \name rem
 * Compute the remainder of each element in \p A divided by the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of dividends.
 *  \param B       The tensor of divisors.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to a % b, where \c a and \c b
 *  are the corresponding elements of \p A and \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor rem(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::REMAINDER, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor rem(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::REMAINDER, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor rem(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::REMAINDER, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name remInPlace
 * Update the input tensor with the result of rem().
 *
 * @{
 */
inline void remInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::REMAINDER, A, B, prog, {di}, options);
}

template <typename constType>
inline void remInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::REMAINDER, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name remWithOutput
 * Write the result of rem() to the given output tensor.
 *
 * @{
 */
inline void remWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::REMAINDER, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void remWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::REMAINDER, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void remWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::REMAINDER, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

//   Bitwise.

/** \name bitwiseAnd
 * Compute the bitwise AND of each element in \p A with the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 * \returns A tensor where each element is the result of `a & b`, where \c a
 * and \c bare the corresponding elements of \p A and \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor bitwiseAnd(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::BITWISE_AND, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseAnd(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_AND, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
template <typename constType>
inline poplar::Tensor bitwiseAnd(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_AND, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name bitwiseAndInPlace
 * Update the input tensor with the result of bitwiseAnd().
 *
 * @{
 */
inline void bitwiseAndInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::BITWISE_AND, A, B, prog, {di}, options);
}

template <typename constType>
inline void bitwiseAndInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::BITWISE_AND, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name bitwiseAndWithOutput
 * Write the result of bitwiseAnd() to the given output tensor.
 *
 * @{
 */
inline void bitwiseAndWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::BITWISE_AND, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void bitwiseAndWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B, const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_AND, A, BTensor, out, prog,
                {di}, options);
}
template <typename constType>
inline void bitwiseAndWithOutput(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_AND, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name bitwiseOr
 * Compute the bitwise OR of each element in \p A with the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 * \returns A tensor where each element is the result of `a | b`, where \c a
 * and \c b are the corresponding elements of \p A and \p B tensors
 * respectively.
 *
 * @{
 */
inline poplar::Tensor bitwiseOr(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::BITWISE_OR, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseOr(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_OR, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseOr(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_OR, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name bitwiseOrInPlace
 * Update the input tensor with the result of bitwiseOr().
 *
 * @{
 */
inline void bitwiseOrInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &B,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::BITWISE_OR, A, B, prog, {di}, options);
}

template <typename constType>
inline void bitwiseOrInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const constType B, poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::BITWISE_OR, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name bitwiseOrWithOutput
 * Write the result of bitwiseOr() to the given output tensor.
 *
 * @{
 */
inline void bitwiseOrWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::BITWISE_OR, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void bitwiseOrWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B, const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_OR, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void bitwiseOrWithOutput(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_OR, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name bitwiseXor
 * Compute the bitwise XOR of each element in \p A with the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a ^ b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor bitwiseXor(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::BITWISE_XOR, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseXor(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_XOR, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseXor(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_XOR, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name bitwiseXorInPlace
 * Update the input tensor with the result of bitwiseXor().
 *
 * @{
 */
inline void bitwiseXorInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::BITWISE_XOR, A, B, prog, {di}, options);
}

template <typename constType>
inline void bitwiseXorInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::BITWISE_XOR, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name bitwiseXorWithOutput
 * Write the result of bitwiseXor() to the given output tensor.
 *
 * @{
 */
inline void bitwiseXorWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XOR, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void bitwiseXorWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B, const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XOR, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void bitwiseXorWithOutput(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XOR, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name bitwiseXnor
 * Compute the bitwise XNOR of each element in \p A with the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `!(a ^ b)`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor bitwiseXnor(poplar::Graph &graph, const poplar::Tensor &A,
                                  const poplar::Tensor &B,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::BITWISE_XNOR, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseXnor(poplar::Graph &graph, const poplar::Tensor &A,
                                  const constType B,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_XNOR, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor bitwiseXnor(poplar::Graph &graph, const constType A,
                                  const poplar::Tensor &B,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::BITWISE_XNOR, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

/** @} */

/** \name bitwiseXnorInPlace
 * Update the input tensor with the result of bitwiseXnor().
 *
 * @{
 */
inline void bitwiseXnorInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                               const poplar::Tensor &B,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::BITWISE_XNOR, A, B, prog, {di},
             options);
}

template <typename constType>
inline void bitwiseXnorInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                               const constType B,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::BITWISE_XNOR, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name bitwiseXnorWithOutput
 * Write the result of bitwiseXnor() to the given output tensor.
 *
 * @{
 */
inline void bitwiseXnorWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                  const poplar::Tensor &B,
                                  const poplar::Tensor &out,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XNOR, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void bitwiseXnorWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                  const constType B, const poplar::Tensor &out,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XNOR, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void bitwiseXnorWithOutput(poplar::Graph &graph, const constType A,
                                  const poplar::Tensor &B,
                                  const poplar::Tensor &out,
                                  poplar::program::Sequence &prog,
                                  const poplar::DebugContext &debugContext = {},
                                  const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::BITWISE_XNOR, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name shiftLeft
 * Shift the elements of \p A left by the corresponding elements of \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of elements which to left-shift.
 *  \param B       The tensor of elements that describe the amount to left-shift
 *                 \p A by.
 *  \param prog    The sequence to extend with the execution of the
 *                 expression evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to a << b, where \c a and \c b
 *  are the corresponding elements of \p A and \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor shiftLeft(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::SHIFT_LEFT, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor shiftLeft(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_LEFT, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor shiftLeft(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_LEFT, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name shiftLeftInPlace
 * Update the input tensor with the result of shiftLeft().
 *
 * @{
 */
inline void shiftLeftInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &B,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::SHIFT_LEFT, A, B, prog, {di}, options);
}

template <typename constType>
inline void shiftLeftInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const constType B, poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::SHIFT_LEFT, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name shiftLeftWithOutput
 * Write the result of shiftLeft() to the given output tensor.
 *
 * @{
 */
inline void shiftLeftWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::SHIFT_LEFT, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void shiftLeftWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B, const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_LEFT, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void shiftLeftWithOutput(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_LEFT, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name shiftRight
 * Shift the elements of \p A right by the corresponding elements of \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of elements which to right-shift.
 *  \param B       The tensor of elements that describe the amount to
 *                 right-shift by. \p A.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to a >> b (without sign
 *  extension), where \c a and \c b are the corresponding elements of \p A and
 *  \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor shiftRight(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::SHIFT_RIGHT, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor shiftRight(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_RIGHT, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor shiftRight(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_RIGHT, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name shiftRightInPlace
 * Update the input tensor with the result of shiftRight().
 *
 * @{
 */
inline void shiftRightInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::SHIFT_RIGHT, A, B, prog, {di}, options);
}

template <typename constType>
inline void shiftRightInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::SHIFT_RIGHT, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name shiftRightWithOutput
 * Write the result of shiftRight() to the given output tensor.
 *
 * @{
 */
inline void shiftRightWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void shiftRightWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B, const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT, A, BTensor, out, prog,
                {di}, options);
}
/** @} */

template <typename constType>
inline void shiftRightWithOutput(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT, ATensor, B, out, prog,
                {di}, options);
}

/** \name shiftRightSignExtend
 * Shift the elements of \p A right with sign extension by the corresponding
 * elements of \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor of elements which to right-shift.
 *  \param B       The tensor of elements that describe the amount to
 *                 right-shift \p A by.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is equal to `a >> b` with sign
 *  extension, where \c a and \c b are the corresponding elements of \p A and
 *  \p B tensors respectively.
 *
 * @{
 */
inline poplar::Tensor
shiftRightSignExtend(poplar::Graph &graph, const poplar::Tensor &A,
                     const poplar::Tensor &B, poplar::program::Sequence &prog,
                     const poplar::DebugContext &debugContext = {},
                     const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A, B,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
shiftRightSignExtend(poplar::Graph &graph, const poplar::Tensor &A,
                     const constType B, poplar::program::Sequence &prog,
                     const poplar::DebugContext &debugContext = {},
                     const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A,
                    BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
shiftRightSignExtend(poplar::Graph &graph, const constType A,
                     const poplar::Tensor &B, poplar::program::Sequence &prog,
                     const poplar::DebugContext &debugContext = {},
                     const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, ATensor,
                    B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name shiftRightSignExtendInPlace
 * Update the input tensor with the result of shiftRightSignExtend().
 *
 * @{
 */
inline void
shiftRightSignExtendInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &B,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A, B, prog,
             {di}, options);
}

template <typename constType>
inline void
shiftRightSignExtendInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                            const constType B, poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A, BTensor,
             prog, {di}, options);
}
/** @} */

/** \name shiftRightSignExtendWithOutput
 * Write the result of shiftRightSignExtend() to the given output tensor.
 *
 * @{
 */
inline void shiftRightSignExtendWithOutput(
    poplar::Graph &graph, const poplar::Tensor &A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A, B, out,
                prog, {di}, options);
}

template <typename constType>
inline void
shiftRightSignExtendWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                               const constType B, const poplar::Tensor &out,
                               poplar::program::Sequence &prog,
                               const poplar::DebugContext &debugContext = {},
                               const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, A, BTensor,
                out, prog, {di}, options);
}

template <typename constType>
inline void shiftRightSignExtendWithOutput(
    poplar::Graph &graph, const constType A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::SHIFT_RIGHT_SIGN_EXTEND, ATensor, B,
                out, prog, {di}, options);
}
/** @} */

//   Logical.

/** \name logicalAnd
 * Compute the logical AND (`&&`) of each element in \p A with the
 * corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a && b`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor logicalAnd(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::LOGICAL_AND, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor logicalAnd(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::LOGICAL_AND, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor logicalAnd(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::LOGICAL_AND, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name logicalAndInPlace
 * Update the input tensor with the result of logicalAnd().
 *
 * @{
 */
inline void logicalAndInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const poplar::Tensor &B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::LOGICAL_AND, A, B, prog, {di}, options);
}

template <typename constType>
inline void logicalAndInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::LOGICAL_AND, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name logicalAndWithOutput
 * Write the result of logicalAnd() to the given output tensor.
 *
 * @{
 */
inline void logicalAndWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_AND, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void logicalAndWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                 const constType B, const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_AND, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void logicalAndWithOutput(poplar::Graph &graph, const constType A,
                                 const poplar::Tensor &B,
                                 const poplar::Tensor &out,
                                 poplar::program::Sequence &prog,
                                 const poplar::DebugContext &debugContext = {},
                                 const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_AND, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name logicalOr
 * Compute the logical OR (`||`) of each element in \p A with the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a || b`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor logicalOr(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::LOGICAL_OR, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor logicalOr(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::LOGICAL_OR, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor logicalOr(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::LOGICAL_OR, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name logicalOrInPlace
 * Update the input tensor with the result of logicalOr().
 *
 * @{
 */
inline void logicalOrInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &B,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::LOGICAL_OR, A, B, prog, {di}, options);
}

template <typename constType>
inline void logicalOrInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                             const constType B, poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::LOGICAL_OR, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name logicalOrWithOutput
 * Write the result of logicalOr() to the given output tensor.
 *
 * @{
 */
inline void logicalOrWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_OR, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void logicalOrWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                                const constType B, const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_OR, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void logicalOrWithOutput(poplar::Graph &graph, const constType A,
                                const poplar::Tensor &B,
                                const poplar::Tensor &out,
                                poplar::program::Sequence &prog,
                                const poplar::DebugContext &debugContext = {},
                                const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LOGICAL_OR, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

//   Comparisons.

/** \name eq
 * Check if each element in \p A is equal to the corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a == b`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor eq(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::EQUAL, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor eq(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::EQUAL, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor eq(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::EQUAL, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name eqInPlace
 * Update the input tensor with the result of eq().
 *
 * @{
 */
inline void eqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const poplar::Tensor &B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::EQUAL, A, B, prog, {di}, options);
}

template <typename constType>
inline void eqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const constType B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::EQUAL, A, BTensor, prog, {di}, options);
}
/** @} */

/** \name eqWithOutput
 * Write the result of eq() to the given output tensor.
 *
 * @{
 */
inline void eqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::EQUAL, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void eqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::EQUAL, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void eqWithOutput(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::EQUAL, ATensor, B, out, prog, {di},
                options);
}
/** @} */

/** \name neq
 * Check if each element in \p A is not equal to the corresponding element in
 * \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a != b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor neq(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::NOT_EQUAL, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor neq(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::NOT_EQUAL, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor neq(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::NOT_EQUAL, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name neqInPlace
 * Update the input tensor with the result of neq().
 *
 * @{
 */
inline void neqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::NOT_EQUAL, A, B, prog, {di}, options);
}

template <typename constType>
inline void neqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::NOT_EQUAL, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name neqWithOutput
 * Write the result of neq() to the given output tensor.
 *
 * @{
 */
inline void neqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::NOT_EQUAL, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void neqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::NOT_EQUAL, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void neqWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::NOT_EQUAL, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name gteq
 * Check if each element in \p A is greater than or equal to the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a >= b`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor gteq(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor gteq(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, BTensor,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor gteq(poplar::Graph &graph, const constType A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, ATensor, B,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name gteqInPlace
 * Update the input tensor with the result of gteq().
 *
 * @{
 */
inline void gteqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        const poplar::Tensor &B,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, B, prog, {di},
             options);
}

template <typename constType>
inline void gteqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        const constType B, poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, BTensor, prog,
             {di}, options);
}
/** @} */

/** \name gteqWithOutput
 * Write the result of gteq() to the given output tensor.
 *
 * @{
 */
inline void gteqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, B, out, prog,
                {di}, options);
}

template <typename constType>
inline void gteqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, A, BTensor, out,
                prog, {di}, options);
}

template <typename constType>
inline void gteqWithOutput(poplar::Graph &graph, const constType A,
                           const poplar::Tensor &B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN_EQUAL, ATensor, B, out,
                prog, {di}, options);
}
/** @} */

/** \name gt
 * Check if each element in \p A is greater than the corresponding element in
 * \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a > b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor gt(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::GREATER_THAN, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor gt(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::GREATER_THAN, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor gt(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::GREATER_THAN, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name gtInPlace
 * Update the input tensor with the result of gt().
 *
 * @{
 */
inline void gtInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const poplar::Tensor &B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::GREATER_THAN, A, B, prog, {di},
             options);
}

template <typename constType>
inline void gtInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const constType B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::GREATER_THAN, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name gtWithOutput
 * Write the result of gt() to the given output tensor.
 *
 * @{
 */
inline void gtWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void gtWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void gtWithOutput(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::GREATER_THAN, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

/** \name lteq
 * Check if each element in \p A is less than or equal to the corresponding
 * element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a <= b`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor lteq(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor lteq(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, BTensor,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor lteq(poplar::Graph &graph, const constType A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::LESS_THAN_EQUAL, ATensor, B,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name lteqInPlace
 * Update the input tensor with the result of lteq().
 *
 * @{
 */
inline void lteqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        const poplar::Tensor &B,
                        poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, B, prog, {di},
             options);
}

template <typename constType>
inline void lteqInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                        const constType B, poplar::program::Sequence &prog,
                        const poplar::DebugContext &debugContext = {},
                        const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name lteqWithOutput
 * Write the result of lteq() to the given output tensor.
 *
 * @{
 */
inline void lteqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, B, out, prog,
                {di}, options);
}

template <typename constType>
inline void lteqWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN_EQUAL, A, BTensor, out,
                prog, {di}, options);
}

template <typename constType>
inline void lteqWithOutput(poplar::Graph &graph, const constType A,
                           const poplar::Tensor &B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN_EQUAL, ATensor, B, out,
                prog, {di}, options);
}
/** @} */

/** \name lt
 * Check if each element in \p A is less than the corresponding element in \p
 * B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `a < b`, where \c a
 *  and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor lt(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::LESS_THAN, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor lt(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::LESS_THAN, A, BTensor, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor lt(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::LESS_THAN, ATensor, B, prog,
                    {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name ltInPlace
 * Update the input tensor with the result of lt().
 *
 * @{
 */
inline void ltInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const poplar::Tensor &B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::LESS_THAN, A, B, prog, {di}, options);
}

template <typename constType>
inline void ltInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                      const constType B, poplar::program::Sequence &prog,
                      const poplar::DebugContext &debugContext = {},
                      const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::LESS_THAN, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name ltWithOutput
 * Write the result of lt() to the given output tensor.
 *
 * @{
 */
inline void ltWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void ltWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN, A, BTensor, out, prog,
                {di}, options);
}

template <typename constType>
inline void ltWithOutput(poplar::Graph &graph, const constType A,
                         const poplar::Tensor &B, const poplar::Tensor &out,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::LESS_THAN, ATensor, B, out, prog,
                {di}, options);
}
/** @} */

//   Selecting elements.

/** \name max
 * Compute the maximum of each element in \p A with the corresponding element
 * in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `max(a, b)`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor max(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::MAXIMUM, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor max(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MAXIMUM, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor max(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MAXIMUM, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name maxInPlace
 * Update the input tensor with the result of max().
 *
 * @{
 */
inline void maxInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::MAXIMUM, A, B, prog, {di}, options);
}

template <typename constType>
inline void maxInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::MAXIMUM, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name maxWithOutput
 * Write the result of max() to the given output tensor.
 *
 * @{
 */
inline void maxWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::MAXIMUM, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void maxWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MAXIMUM, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void maxWithOutput(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MAXIMUM, ATensor, B, out, prog, {di},
                options);
}
/** @} */

/** \name min
 * Compute the minimum of each element in \p A with the corresponding element
 * in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `min(a, b)`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor min(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::MINIMUM, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor min(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MINIMUM, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor min(poplar::Graph &graph, const constType A,
                          const poplar::Tensor &B,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::MINIMUM, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name minInPlace
 * Update the input tensor with the result of min().
 *
 * @{
 */
inline void minInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const poplar::Tensor &B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::MINIMUM, A, B, prog, {di}, options);
}

template <typename constType>
inline void minInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                       const constType B, poplar::program::Sequence &prog,
                       const poplar::DebugContext &debugContext = {},
                       const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::MINIMUM, A, BTensor, prog, {di},
             options);
}
/** @} */

/** \name minWithOutput
 * Write the result of min() to the given output tensor.
 *
 * @{
 */
inline void minWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::MINIMUM, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void minWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                          const constType B, const poplar::Tensor &out,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MINIMUM, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void minWithOutoput(poplar::Graph &graph, const constType A,
                           const poplar::Tensor &B, const poplar::Tensor &out,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::MINIMUM, ATensor, B, out, prog, {di},
                options);
}
/** @} */

//   Trigonometric.

/** \name atan2
 * Compute the two argument arctangent of each element in \p A with the
 * corresponding element in \p B.
 *
 *  \param graph   The graph to update.
 *  \param A       A tensor of elements.
 *  \param B       A tensor of elements.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor where each element is the result of `atan2(a, b)`, where
 *  \c a and \c b are the corresponding elements of \p A and \p B tensors
 *  respectively.
 *
 * @{
 */
inline poplar::Tensor atan2(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &B,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output =
      map(graph, expr::BinaryOpType::ATAN2, A, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor atan2(poplar::Graph &graph, const poplar::Tensor &A,
                            const constType B, poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::ATAN2, A, BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor atan2(poplar::Graph &graph, const constType A,
                            const poplar::Tensor &B,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output =
      map(graph, expr::BinaryOpType::ATAN2, ATensor, B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name atan2InPlace
 * Update the input tensor with the result of atan2().
 *
 * @{
 */
inline void atan2InPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::ATAN2, A, B, prog, {di}, options);
}

template <typename constType>
inline void atan2InPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         const constType B, poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::ATAN2, A, BTensor, prog, {di}, options);
}
/** @} */

/** \name atan2WithOutput
 * Write the result of atan2() to the given output tensor.
 *
 * @{
 */
inline void atan2WithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &B, const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::ATAN2, A, B, out, prog, {di},
                options);
}

template <typename constType>
inline void atan2WithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const constType B, const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::ATAN2, A, BTensor, out, prog, {di},
                options);
}

template <typename constType>
inline void atan2WithOutput(poplar::Graph &graph, const constType A,
                            const poplar::Tensor &B, const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::ATAN2, ATensor, B, out, prog, {di},
                options);
}
/** @} */

// Statistical.

/** \name invStdDevToVariance
 * Convert the inverse standard deviation to variance.
 *
 *  \param graph   The graph to update.
 *  \param A       The source tensor.
 *  \param B       The destination tensor.
 *  \param prog    The sequence to extend with the execution of conversion.
 *  \param debugContext Optional debug information.
 *  \param options A list of flags to pass to the expression evaluator.
 *
 *  \returns       A tensor where each element is the variance.
 *  Each element is the result of
 *  `b = (1 / a) ^ 2`, where \c a and \c b are the corresponding
 *  elements of \p A and \p B tensors respectively, and where \p A
 *  represents the inverse standard deviation and \p B the variance.
 *
 * @{
 */
inline poplar::Tensor
invStdDevToVariance(poplar::Graph &graph, const poplar::Tensor &A,
                    const poplar::Tensor &B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A, B,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
invStdDevToVariance(poplar::Graph &graph, const poplar::Tensor &A,
                    const constType B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A,
                    BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
invStdDevToVariance(poplar::Graph &graph, const constType A,
                    const poplar::Tensor &B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, ATensor,
                    B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name invStdDevToVarianceInPlace
 * Update the input tensor with the result of invStdDevToVariance().
 *
 * @{
 */
inline void
invStdDevToVarianceInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A, B, prog,
             {di}, options);
}

template <typename constType>
inline void
invStdDevToVarianceInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A, BTensor,
             prog, {di}, options);
}
/** @} */

/** \name invStdDevToVarianceWithOutput
 * Write the result of invStdDevToVariance() to the given output tensor.
 *
 * @{
 */
inline void invStdDevToVarianceWithOutput(
    poplar::Graph &graph, const poplar::Tensor &A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A, B, out,
                prog, {di}, options);
}

template <typename constType>
inline void
invStdDevToVarianceWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B, const poplar::Tensor &out,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, A, BTensor,
                out, prog, {di}, options);
}

template <typename constType>
inline void invStdDevToVarianceWithOutput(
    poplar::Graph &graph, const constType A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::INV_STD_DEV_TO_VARIANCE, ATensor, B,
                out, prog, {di}, options);
}
/** @} */

/** \name varianceToInvStdDev
 * Convert variance to inverse standard deviation.
 *
 *  \param graph   The graph to update.
 *  \param A       The source tensor.
 *  \param B       The destination tensor.
 *  \param prog    The sequence to extend with the execution of conversion.
 *  \param debugContext Optional debug information.
 *
 *  \returns       A tensor where each element is the inverse of standard
 *  deviation. Each element is the result of
 *  `b = sqrt(1 / a)`, where \c a and \c b are the corresponding
 *  elements of \p A and \p B tensors respectively, and where \p A
 *  represents the variance and \p B the inverse standard deviation.
 *
 * @{
 */
inline poplar::Tensor
varianceToInvStdDev(poplar::Graph &graph, const poplar::Tensor &A,
                    const poplar::Tensor &B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  auto output = map(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A, B,
                    prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
varianceToInvStdDev(poplar::Graph &graph, const poplar::Tensor &A,
                    const constType B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  auto output = map(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A,
                    BTensor, prog, {di}, options);
  di.addOutput(output);
  return output;
}

template <typename constType>
inline poplar::Tensor
varianceToInvStdDev(poplar::Graph &graph, const constType A,
                    const poplar::Tensor &B, poplar::program::Sequence &prog,
                    const poplar::DebugContext &debugContext = {},
                    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  auto output = map(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, ATensor,
                    B, prog, {di}, options);
  di.addOutput(output);
  return output;
}
/** @} */

/** \name varianceToInvStdDevInPlace
 * Update the input tensor with the result of varianceToInvStdDev().
 *
 * @{
 */
inline void
varianceToInvStdDevInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           const poplar::Tensor &B,
                           poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  mapInPlace(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A, B, prog,
             {di}, options);
}

template <typename constType>
inline void
varianceToInvStdDevInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                           const constType B, poplar::program::Sequence &prog,
                           const poplar::DebugContext &debugContext = {},
                           const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapInPlace(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A, BTensor,
             prog, {di}, options);
}
/** @} */

/** \name varianceToInvStdDevWithOutput
 * Write the result of varianceToInvStdDev() to the given output tensor.
 *
 * @{
 */
inline void varianceToInvStdDevWithOutput(
    poplar::Graph &graph, const poplar::Tensor &A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  mapWithOutput(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A, B, out,
                prog, {di}, options);
}

template <typename constType>
inline void
varianceToInvStdDevWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                              const constType B, const poplar::Tensor &out,
                              poplar::program::Sequence &prog,
                              const poplar::DebugContext &debugContext = {},
                              const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(A.elementType(), B);
  const auto BTensor = graph.addConstant(A.elementType(), {}, B, {di});
  graph.setTileMapping(BTensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, A, BTensor,
                out, prog, {di}, options);
}

template <typename constType>
inline void varianceToInvStdDevWithOutput(
    poplar::Graph &graph, const constType A, const poplar::Tensor &B,
    const poplar::Tensor &out, poplar::program::Sequence &prog,
    const poplar::DebugContext &debugContext = {},
    const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, out, options));

  checkTypes(B.elementType(), A);
  const auto ATensor = graph.addConstant(B.elementType(), {}, A, {di});
  graph.setTileMapping(ATensor, 0);
  mapWithOutput(graph, expr::BinaryOpType::VARIANCE_TO_INV_STD_DEV, ATensor, B,
                out, prog, {di}, options);
}
/** @} */

// Ternary operations

/** Populate the returned tensor with elements from \p A or \p B depending on
 *  the corresponding element of \p C.
 *
 *  That is, for each element in the output compute `c ? a : b`, where \c a,
 *  \c b and \c c are the corresponding elements in the tensors \p A, \p B,
 *  \p C respectively.
 *
 *  \param graph   The graph to update.
 *  \param A       One of the tensors containing the elements to select from.
 *  \param B       One of the tensors containing the elements to select from.
 *  \param C       The tensor containing the elements to use as predicates.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor containing the elements from \p A where the corresponding
 *           elements in \p C were not equal to zero and containing the elements
 *           from \p B where the corresponding elements in \p C were zero.
 */
inline poplar::Tensor select(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &B, const poplar::Tensor &C,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, options));
  auto output =
      map(graph, expr::TernaryOpType::SELECT, A, B, C, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of select().
 */
inline void selectInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                          const poplar::Tensor &B, const poplar::Tensor &C,
                          poplar::program::Sequence &prog,
                          const poplar::DebugContext &debugContext = {},
                          const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, options));
  mapInPlace(graph, expr::TernaryOpType::SELECT, A, B, C, prog, {di}, options);
}

/** Write the result of select() to the given output tensor.
 */
inline void selectWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                             const poplar::Tensor &B, const poplar::Tensor &C,
                             const poplar::Tensor &out,
                             poplar::program::Sequence &prog,
                             const poplar::DebugContext &debugContext = {},
                             const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, out, options));
  mapWithOutput(graph, expr::TernaryOpType::SELECT, A, B, C, out, prog, {di},
                options);
}

/** Populate the returned tensor with elements from \p A but clamp them such
 * that each element is greater than or equal to the corresponding element in \p
 * B and less than or equal to the corresponding element in \p C.
 *
 *  That is, for each element in the returned tensor compute: `min(max(a, b),
 * c)`, where \c a, \c and \c c are the corresponding elements in the tensors
 * \p A, \p B and \p C respectively.
 *
 *  \param graph   The graph to update.
 *  \param A       The tensor containing the elements to clamp.
 *  \param B       The tensor containing the elements to use as minimums.
 *  \param C       The tensor containing the elements to use as maximums.
 *  \param prog    The sequence to extend with the execution of the expression
 *                 evaluation.
 *  \param debugContext Optional debug information.
 *  \param options Element-wise options. See map().
 *
 *  \returns A tensor containing the elements resulting from the application of
 *           the expression across the tensors.
 */
inline poplar::Tensor clamp(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &B, const poplar::Tensor &C,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, options));

  auto output =
      map(graph, expr::TernaryOpType::CLAMP, A, B, C, prog, {di}, options);
  di.addOutput(output);
  return output;
}

/** Update the input tensor with the result of clamp().
 */
inline void clampInPlace(poplar::Graph &graph, const poplar::Tensor &A,
                         const poplar::Tensor &B, const poplar::Tensor &C,
                         poplar::program::Sequence &prog,
                         const poplar::DebugContext &debugContext = {},
                         const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, options));

  mapInPlace(graph, expr::TernaryOpType::CLAMP, A, B, C, prog, {di}, options);
}

/** Write the result of clamp() to the given output tensor.
 */
inline void clampWithOutput(poplar::Graph &graph, const poplar::Tensor &A,
                            const poplar::Tensor &B, const poplar::Tensor &C,
                            const poplar::Tensor &out,
                            poplar::program::Sequence &prog,
                            const poplar::DebugContext &debugContext = {},
                            const poplar::OptionFlags &options = {}) {
  poputil::PoplibsOpDebugInfo di(debugContext, DI_ARGS(A, B, C, out, options));

  mapWithOutput(graph, expr::TernaryOpType::CLAMP, A, B, C, out, prog, {di},
                options);
}

} // end namespace popops

#endif // popops_ElementWise_hpp
