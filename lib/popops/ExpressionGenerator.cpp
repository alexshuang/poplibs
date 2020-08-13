// Copyright (c) 2019 Graphcore Ltd. All rights reserved.
#include "ExpressionGenerator.hpp"
#include "ExprOpUtil.hpp"
#include "poplibs_support/Compiler.hpp"
#include "poplibs_support/gcd.hpp"
#include "popops/ElementWise.hpp"
#include "popops/ElementWiseUtil.hpp"
#include "poputil/Broadcast.hpp"
#include "poputil/TileMapping.hpp"
#include "poputil/Util.hpp"
#include "poputil/VertexTemplates.hpp"
#include "poputil/exceptions.hpp"

#include <boost/optional.hpp>

#include "ExprOpUtil.hpp"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <fstream>
#include <math.h>
#include <poplibs_support/logging.hpp>
#include <queue>
#include <sstream>
#include <stack>

using namespace poputil;
using namespace poplar;
using namespace poplar::program;

using popops::expr::BinaryOpType;
using popops::expr::BroadcastOpType;
using popops::expr::TernaryOpType;
using popops::expr::UnaryOpType;
using namespace poplibs_support;

namespace popops {

static bool isSupportedType(poplar::Type t) {
  return t == poplar::FLOAT || t == poplar::HALF || t == poplar::INT ||
         t == poplar::UNSIGNED_INT || t == poplar::BOOL;
}

static bool traverseAndCheck(const expr::Expr &expr,
                             const std::vector<poplar::Tensor> &inputs,
                             uint32_t &numberOfOperations) {

  if (const expr::Const *c = expr.getAs<expr::Const>()) {

    poplar::Type type = c->getType();

    // Check if the type is floating point it is not NAN.
    if (type == poplar::FLOAT) {
      float val = *reinterpret_cast<float *>(c->getData());
      if (std::isinf(val) || std::isnan(val)) {
        return false;
      }
    } else if (type == poplar::HALF) {
      assert(c->getTypeTraits().isFloat == true &&
             c->getTypeTraits().size == sizeof(float));

      float val = *reinterpret_cast<float *>(c->getData());
      if (std::isinf(val) || std::isnan(val)) {
        return false;
      }
    }

    return isSupportedType(c->getType());
  } else if (const expr::PlaceHolder *p = expr.getAs<expr::PlaceHolder>()) {

    // Check the placeholder type is supported.
    const size_t index = p->getIndex();

    if (index > inputs.size())
      return false;

    poplar::Type type = inputs[index - 1].elementType();
    return isSupportedType(type);
  } else if (const expr::Cast *c = expr.getAs<expr::Cast>()) {

    poplar::Type typeCastingTo = c->getRHSType();
    numberOfOperations++;

    // Check the type being casted to is supported and also the type being
    // casted.
    return isSupportedType(typeCastingTo) &&
           traverseAndCheck(c->getLHS(), inputs, numberOfOperations);

  } else if (const expr::UnaryOp *u = expr.getAs<expr::UnaryOp>()) {
    numberOfOperations++;

    return traverseAndCheck(u->getArg(), inputs, numberOfOperations);
  } else if (const expr::BinaryOp *b = expr.getAs<expr::BinaryOp>()) {

    BinaryOpType opType = b->getOpType();
    if (opType == BinaryOpType::VARIANCE_TO_INV_STD_DEV ||

        opType == BinaryOpType::INV_STD_DEV_TO_VARIANCE) {
      return false;
    }
    numberOfOperations++;
    if (!traverseAndCheck(b->getRHS(), inputs, numberOfOperations) ||
        !traverseAndCheck(b->getLHS(), inputs, numberOfOperations)) {
      return false;
    }

  } else if (const expr::TernaryOp *t = expr.getAs<expr::TernaryOp>()) {

    numberOfOperations++;
    if (!traverseAndCheck(t->getArg2(), inputs, numberOfOperations) ||
        !traverseAndCheck(t->getArg1(), inputs, numberOfOperations) ||
        !traverseAndCheck(t->getArg0(), inputs, numberOfOperations)) {
      return false;
    }
  }

  return true;
}

ExprInfo analyseExpr(const expr::Expr &expr,
                     const std::vector<poplar::Tensor> &inputs,
                     bool isForcedOn) {
  if (inputs.size() == 0)
    return {false, false};

  // All tensors should be the same shape or scalar
  unsigned size = 1;
  auto shape = inputs[0].shape();
  for (const Tensor &t : inputs) {
    if (size == 1) {
      size = t.numElements();
      shape = (size == 1) ? t.flatten().shape() : t.shape();
    }
    if ((t.shape() != shape && t.numElements() != 1) || t.containsAliases()) {
      return {false, false};
    }
  }
  uint32_t numberOfOperations = 0;
  bool isOk = traverseAndCheck(expr, inputs, numberOfOperations);

  // Check that this is not just a single operation.
  isOk &= isForcedOn || numberOfOperations > 1;
  return {isOk, (size == 1)};
}

namespace {

std::string getTypeAlias(const std::string &typeAsStr) {
  std::string str = typeAsStr + "_ty";
  size_t pos = str.find("unsigned");
  if (pos != std::string::npos) {
    str.replace(0, 9, "u", 1);
  }

  return str;
}

void executeCodelet(Graph &graph, const std::string &codeletName,
                    std::vector<Tensor> inputs, const Tensor &out,
                    const std::vector<std::vector<Interval>> &intervals,
                    unsigned tile, const ComputeSet &cs, size_t numFusedOps,
                    bool vectorizationIsSupported, bool inPlace) {
  const auto dType = inputs[0].elementType();
  const auto &target = graph.getTarget();
  const auto vectorWidth = target.getVectorWidth(dType);
  const auto grainSize =
      std::max<unsigned>(vectorWidth, target.getAtomicStoreGranularity());
  auto vertexRegions =
      splitRegionsBetweenWorkers(target, intervals, grainSize, 2 * grainSize);
  for (const auto &regions : vertexRegions) {
    auto v = graph.addVertex(cs, codeletName);

    poplar::Tensor outRegions = poplar::concat(out.flatten().slices(regions));

    std::vector<poplar::Tensor> inRegions(inputs.size());

    std::transform(inputs.begin(), inputs.end(), inRegions.begin(),
                   [&](poplar::Tensor &t) {
                     return poplar::concat(t.flatten().slices(regions));
                   });

    std::uint64_t estimate = 13;
    for (unsigned i = 0; i < inRegions.size(); ++i) {
      if (inputs[i].numElements() == 1) {

        graph.connect(v["in" + std::to_string(i + 1)], inputs[i].reshape({}));
      } else {
        graph.connect(v["in" + std::to_string(i + 1)], inRegions[i]);
      }

      estimate += inRegions[i].numElements() / vectorWidth * numFusedOps;
      estimate += inRegions[i].numElements() % vectorWidth * numFusedOps;
    }

    graph.setCycleEstimate(v, estimate);

    if (!inPlace) {
      graph.connect(v["out"],
                    out.numElements() == 1 ? out.reshape({}) : outRegions);
    }
    graph.setTileMapping(v, tile);
  }
}

} // end anonymous namespace

// Top level function which calls functions to generate and execute the
// generated codelet.
poplar::Tensor generateAndExecuteMappedOperations(
    Graph &graph, const expr::Expr &expr, const std::vector<Tensor> &inputs,
    std::unordered_map<const expr::Expr *, Type> &constTypes, Sequence &prog,
    bool inPlace, bool allInputsScalar, const std::string &debugPrefix) {

  GenerateCodeletFromMapExpr generate{inPlace, inputs};

  // Traverse the expression tree and based on each node in the tree build up
  // the body of the map operation in a string format representing the end code.
  generate.traverseExpressionTree(expr, constTypes);

  poplar::Type returnType = generate.deduceReturnType();

  // Generate the actual codelet which will be run, compile it, add it to the
  // graph, and store the name of the generated codelet in codeletName.
  std::string codeletName =
      generate.generateCodelet(graph, allInputsScalar, expr);

  size_t numFusedOp = generate.getNumFusedOps();

  bool isVectorizationSupported = generate.isVectorized();

  std::vector<Tensor> flattenedIns;
  std::vector<Tensor *> asPtr;
  std::vector<Tensor> vectorIns;

  flattenedIns.reserve(inputs.size());
  vectorIns.reserve(inputs.size());

  // Flatten the input and also record the address of the flattened tensor if it
  // has > 1 element.
  for (unsigned i = 0; i < inputs.size(); ++i) {
    flattenedIns.push_back(inputs[i].flatten());
    if (inputs[i].numElements() != 1) {
      asPtr.push_back(&flattenedIns[i]);
      vectorIns.push_back(inputs[i]);
    }
  }

  poplar::Tensor out;

  if (inPlace) {
    out = inputs[0];
  } else {
    out = createOutputForElementWiseOp(
        graph, vectorIns.size() == 0 ? inputs : vectorIns, returnType,
        codeletName + "/Out");
  }
  auto outFlat = out.flatten();
  const auto &target = graph.getTarget();
  const auto numTiles = target.getNumTiles();
  const auto cs = graph.addComputeSet(debugPrefix);
  graph.reorderToSimplify(&outFlat, asPtr);
  const auto mapping = graph.getTileMapping(outFlat);
  for (auto tile = 0U; tile != numTiles; ++tile) {
    const auto thisTileMap = mapping[tile];
    const auto tileContiguousRegions =
        graph.getSortedContiguousRegions(outFlat, thisTileMap);
    executeCodelet(graph, codeletName, flattenedIns, outFlat,
                   tileContiguousRegions, tile, cs, numFusedOp,
                   isVectorizationSupported, inPlace);
  }
  prog.add(Execute(cs));

  return out;
}

// Convert a constant expression into a string representing that constant in
// C/C++.
static std::string handleConstant(const expr::Const *c) {
  return c->printValue();
}

static bool typeSupportsVectorization(poplar::Type type) {
  return type == poplar::HALF || type == poplar::FLOAT || type == poplar::BOOL;
}

void GenerateCodeletFromMapExpr::traverseExpressionTree(
    const expr::Expr &expr,
    std::unordered_map<const expr::Expr *, Type> &constTypes) {

  if (const expr::Const *c = expr.getAs<expr::Const>()) {

    const poplar::Type type = constTypes.at(c);
    TypesNeedingAlias.insert(type);

    std::string typeAsStr = getTypeAlias(type.toString());
    const std::string constantAsString = handleConstant(c);
    const std::string variableName =
        "C" + std::to_string(constantInitalizers.size() + 1);
    const std::string initalizer =
        "const " + typeAsStr + " " + variableName + " = ";

    vectorizationIsSupported &= typeSupportsVectorization(type);

    constantInitalizers.push({initalizer, constantAsString});
    data.push({variableName, type});

  } else if (const expr::Cast *c = expr.getAs<expr::Cast>()) {
    traverseExpressionTree(c->getLHS(), constTypes);

    poplar::Type typeCastingTo = c->getRHSType();
    auto pair = data.top();
    data.pop();

    TypesNeedingAlias.insert(typeCastingTo);

    // Propagate the fact that the operand is a constant.
    std::string variable_name = "my_var_" + std::to_string(initalizers.size());

    if (pair.first[0] == 'C') {
      variable_name.insert(variable_name.begin(), 'C');
    }
    std::string asStr = getTypeAlias(typeCastingTo.toString());
    std::string result = "const " + asStr + " " + variable_name + " = (" +
                         asStr + ")" + pair.first + ";";

    vectorizationIsSupported = false;

    // The initializer to be printed in the function.
    data.push({variable_name, typeCastingTo});
    // The variable name to be used in subsequent iterations.
    initalizers.push(result);
  } else if (const expr::PlaceHolder *p = expr.getAs<expr::PlaceHolder>()) {
    const size_t index = p->getIndex();

    const std::string placeholder = "load" + std::to_string(index);

    poplar::Type type = inputs[index - 1].elementType();

    vectorizationIsSupported &= typeSupportsVectorization(type);

    data.push({placeholder, type});
    TypesNeedingAlias.insert(type);
    usedPlaceholders.insert(index);
  } else if (const expr::UnaryOp *u = expr.getAs<expr::UnaryOp>()) {
    numFusedOps++;
    traverseExpressionTree(u->getArg(), constTypes);

    assert(!data.empty() &&
           "Expression traversal failed in unary op, data is empty");

    auto opType = u->getOpType();
    auto pair = data.top();
    data.pop();

    const std::string &param = pair.first;

    std::string variable_name = "my_var_" + std::to_string(initalizers.size());

    // Propagate the fact that the operand is a constant.
    if (pair.first[0] == 'C') {
      variable_name.insert(variable_name.begin(), 'C');
    }
    poplar::Type type = getReturnType(opType, pair.second);

    TypesNeedingAlias.insert(type);

    std::string asStr = getTypeAlias(type.toString());

    std::string result = "const " + asStr + " " + variable_name + " = ";
    if (isSpecialCase(opType)) {
      result += handleSpecialCase(opType, param);
    } else {
      result += getUnaryOpAsString(opType, type);
      result += "(" + param + ")";
    }
    result += ";\n";

    vectorizationIsSupported &= supportsVectorization(opType);
    // The initializer to be printed in the function.
    data.push({variable_name, type});
    // The variable name to be used in subsequent iterations.
    initalizers.push(result);
  } else if (const expr::BinaryOp *b = expr.getAs<expr::BinaryOp>()) {
    numFusedOps++;
    auto opType = b->getOpType();

    traverseExpressionTree(b->getRHS(), constTypes);
    traverseExpressionTree(b->getLHS(), constTypes);

    assert(data.size() >= 2 &&
           "Expression traversal failed in binary op, data is less than 2");

    auto pair1 = data.top();
    data.pop();
    auto pair2 = data.top();
    data.pop();

    const std::string &param1 = pair1.first;
    const std::string &param2 = pair2.first;

    std::string variable_name = "my_var_" + std::to_string(initalizers.size());
    // Propagate the fact that the operand is a constant.
    if (pair1.first[0] == 'C' && pair2.first[0] == 'C') {
      variable_name.insert(variable_name.begin(), 'C');
    }
    poplar::Type type = getReturnType(opType, pair1, pair2);

    TypesNeedingAlias.insert(type);

    std::string result =
        "const " + getTypeAlias(type.toString()) + " " + variable_name + " = ";

    if (hasFunctionSemantics(opType)) {
      // Call it like a function.
      result += std::string(getBinaryOpAsString(opType, type)) + "(" + param1 +
                "," + param2 + ")";
    } else if (isSpecialCase(opType)) {
      result += handleSpecialCase(opType, param1, param2);
    } else {
      result += param1;
      result += getBinaryOpAsString(opType, type);
      result += param2;
    }

    result += ";\n";
    // The initializer to be printed in the function.
    data.push({variable_name, type});
    // The variable name to be used in subsequent iterations.
    initalizers.push(result);
  } else if (const expr::TernaryOp *t = expr.getAs<expr::TernaryOp>()) {
    numFusedOps++;
    auto opType = t->getOpType();

    traverseExpressionTree(t->getArg2(), constTypes);
    traverseExpressionTree(t->getArg1(), constTypes);
    traverseExpressionTree(t->getArg0(), constTypes);

    assert(data.size() >= 3 &&
           "Expression traversal failed in ternary op, data is less than 2");

    // Pop the three arguments from the stack.
    auto pair1 = data.top();
    data.pop();
    auto pair2 = data.top();
    data.pop();
    auto pair3 = data.top();
    data.pop();

    bool isFloatingPoint =
        pair2.second == poplar::HALF || pair2.second == poplar::FLOAT;

    const std::string variable_name =
        "my_var_" + std::to_string(initalizers.size());
    std::string result;

    // Select is implemented as if (C) A; else B;
    if (opType == TernaryOpType::SELECT) {
      TypesNeedingAlias.insert(pair1.second);
      poplar::Type returnType = pair1.second;

      bool lhsIsConst = pair1.first[0] == 'C';
      if (lhsIsConst) {
        returnType = pair2.second;
      }

      const std::string type = getTypeAlias(returnType.toString());
      result = type + " " + variable_name + ";";

      result += "if (" + pair3.first + ") { " + variable_name + " = " +
                pair1.first + ";} else {" + variable_name + " = " +
                pair2.first + ";}";

    } else {
      assert(opType == TernaryOpType::CLAMP &&
             "TernaryOpType is not supported by expression generator.");
      const std::string maxFunc = isFloatingPoint ? "NAMESPACE::fmax" : "max";
      const std::string minFunc = isFloatingPoint ? "NAMESPACE::fmin" : "min";

      TypesNeedingAlias.insert(pair2.second);
      const std::string type = getTypeAlias(pair2.second.toString());

      // Clamp is 'const Type VAR = max(low, min(val, high));'
      result = "const " + type + " " + variable_name + " = " + maxFunc + "(" +
               pair2.first + "," + minFunc + "(" + pair1.first + "," +
               pair3.first + "));";
    }

    // The initializer to be printed in the function.
    data.push({variable_name, pair2.second});
    // The variable name to be used in subsequent iterations.
    initalizers.push(result);
  }
}

// Generate the actual codelet.

void GenerateCodeletFromMapExpr::addHeader(std::stringstream &stream) {
  stream << R"l(
#include <poplar/HalfFloat.hpp>
  #ifdef __IPU__

  // Use the IPU intrinsics
  #include <ipu_memory_intrinsics>
  #include <ipu_vector_math>
  #define NAMESPACE ipu
  #else
  // Use the std functions
  #include <cmath>
  #define NAMESPACE std
  #endif
template <typename T>
const T &max(const T &x, const T &y) {
  return x < y ? y : x;
}

template <typename T>
const T &min(const T &x, const T &y) {
  return x < y ? x : y;
}

  template<typename T>
  struct Traits {
  typename std::remove_reference<T>::type ONE() { return 1; }
  };

  template<>
  struct Traits<double> { static double ONE() { return 1.0;} };

  template<>
  struct Traits<float> { static float ONE(){ return 1.0f;} };

  template<>
  struct Traits<double&> { static double ONE() { return 1.0;} };

  template<>
  struct Traits<float&> { static float ONE() {return 1.0f;} };


  template<>
  struct Traits<half> { static half ONE() {return 1;} };

  template<>
  struct Traits<half&> { static half ONE() {return 1;} };

#ifdef __IPU__
  template<>
  struct Traits<float2> { static float2 ONE() { return {1.0f, 1.0f};} };
  template<>
  struct Traits<float2&> { static float2 ONE() { return {1.0f, 1.0f};}  };

  template<>
  struct Traits<half2> { static half2 ONE() { return {1.0, 1.0};} };
  template<>
  struct Traits<half2&> { static half2 ONE() {return {1.0, 1.0};}  };

  template<>
  struct Traits<half4> { static half4 ONE(){return {1.0, 1.0,1.0, 1.0};}  };
  template<>
  struct Traits<half4&> { static half4 ONE(){return {1.0, 1.0, 1.0, 1.0};}  };
#endif

  template<typename T>
  inline T internal_rsqrt(T x) {
  #ifdef __IPU__
      return ipu::rsqrt(x);
  #else
     return Traits<T>::ONE() / std::sqrt(x);
  #endif
  }

  template <typename T>
  inline T internal_remainder(T x, T y) {
    if (std::is_integral<T>::value) {
        T tmp = x / y;
        return x - tmp*y;
    } else {
        return NAMESPACE::fmod(float(x), float(y));
    }
  }


 template <typename T>
  inline T internal_sigmoid(T x) {
    #ifdef __IPU__
      return ipu::sigmoid(x);
    #else
      T one = Traits<T>::ONE();
      return one / (one + NAMESPACE::exp(-x));
    #endif
  }

  #include <poplar/Vertex.hpp>
  using namespace poplar;
  )l";
}

// Add a vectorized loop to the codelet.
void GenerateCodeletFromMapExpr::addVectorizedSection(
    std::stringstream &stream, size_t vectorizationWidth,
    std::string &initalizerString, std::string &constantInitalizerString) {

  stream << R"l(// Vectorized code
            #ifdef __IPU__
            {)l";

  for (poplar::Type type : TypesNeedingAlias) {

    stream << "using " << getTypeAlias(type.toString()) << " = "
           << type.toString() << std::to_string(vectorizationWidth) << ";\n";
  }

  // Add each input as a pointer cast.
  for (std::size_t index : usedPlaceholders) {
    const std::string type =
        getTypeAlias(inputs[index - 1].elementType().toString());
    const std::string id = std::to_string(index);
    // Add: "const {type} * In{id} = reinterpret_cast<{type}*>(in{id});"
    if (inputs[index - 1].numElements() != 1) {
      stream << "const " << type << " * In" << id << " = reinterpret_cast<"
             << type << "*>(&in" << id << "[0]);\n";
    }
  }

  assert(!data.empty() && "Attempting to read data stack which is empty");
  const std::string outType = getTypeAlias(data.top().second.toString());

  const std::string outString = inPlace ? "in1" : "out";

  // Add: "{outType} * In{id} = reinterpret_cast<{type}*>({in1/out});"
  stream << outType << " * Out "
         << " = reinterpret_cast<" << outType << "*>(&" << outString
         << "[0]);\n";

  stream << "remainder = " << outString << " .size() %"
         << std::to_string(vectorizationWidth)
         << ";\nstartIndex = " << outString << ".size() - remainder;\n";

  stream << R"l(
      asm volatile ("# Thwart loop rotation (start)" ::: "memory");
            for (unsigned i = 0; i <()l"
         << outString << ".size()/" << std::to_string(vectorizationWidth)
         << "u); ++i) {\n";

  // Load the data.
  for (std::size_t index : usedPlaceholders) {
    const std::string type =
        getTypeAlias(inputs[index - 1].elementType().toString());
    const std::string id = std::to_string(index);

    // Add: load{id} = ipu::load_postinc(&In{id}, 1);
    if (inputs[index - 1].numElements() != 1) {
      stream << type << " load" + id << "= ipu::load_postinc(&In" << id
             << ", 1);\n";
    }
  }

  stream << constantInitalizerString;
  // Each expression is a variable initialization.
  stream << initalizerString;

  assert(!data.empty() && "Attempting to read data stack which is empty");
  // Add: "ipu::store_postinc(&Out, {result}, 1);"
  stream << "ipu::store_postinc(&Out," << data.top().first << ",1);\n";

  stream << R"l(
        } // End loop
        asm volatile ("# Thwart loop rotation (end)" ::: "memory");
        } // End vectorized section.
        #endif)l";
}

// Adds the serial section of the codelet to the stream.
void GenerateCodeletFromMapExpr::addSerialSection(
    std::stringstream &stream, std::string &initalizerString,
    std::string &constantInitalizerString, bool allInputsScalar) {

  stream << R"l(
        // Remainder/Serial fallback.
        {)l";

  for (poplar::Type type : TypesNeedingAlias) {

    std::string asStr = getTypeAlias(type.toString());

    stream << "using " << asStr << " = " << type.toString() << ";\n";
  }

  // Loop over the remainder
  stream << R"l(
          for (unsigned i = startIndex; i < startIndex + remainder; ++i) {
    )l";

  // Add the aliases to the "load" variable names which the placeholders are
  // using.
  for (std::size_t index : usedPlaceholders) {
    std::string type = getTypeAlias(inputs[index - 1].elementType().toString());
    const std::string id = std::to_string(index);

    // Add: "{type} & load{id} = in{id}[i];"
    if (inputs[index - 1].numElements() == 1) {
      stream << type << " load" << id << " =  in" << id << ";\n";
    } else {
      stream << type << "& load" << id << " =  in" << id << "[i];\n";
    }
  }

  // Add the constants.
  stream << constantInitalizerString;

  // Add the variable initalizations that make up the expression.
  stream << initalizerString;

  // The final assignment of the aggregate of all the operations in
  // initalizers.
  if (allInputsScalar) {
    stream << (inPlace ? "*in1 = " : "*out = ");
  } else {
    stream << (inPlace ? "in1[i] = " : "out[i] = ");
  }

  assert(!data.empty() && "Attempting to read data stack which is empty");
  stream << data.top().first << ";\n";
}

std::string GenerateCodeletFromMapExpr::generateCodelet(
    poplar::Graph &graph, bool allInputsScalar, const expr::Expr &expr) {

  // Each stage of the operation is stored as a variable initalization.
  std::string initalizerString;
  while (!initalizers.empty()) {
    initalizerString += initalizers.front();
    initalizers.pop();
  }

  const auto &target = graph.getTarget();
  poplar::Type smallestVector = *std::min_element(
      TypesNeedingAlias.begin(), TypesNeedingAlias.end(),
      [&target](const poplar::Type &lhs, const poplar::Type &rhs) {
        return target.getVectorWidth(lhs) < target.getVectorWidth(rhs);
      });

  // Get the smallest vectorization width of all the types.
  size_t vectorizationWidth = target.getVectorWidth(smallestVector);
  // Process the constant values. We need this step as we cannot just embed
  // the constants if we are working with vectors.
  std::string constantInitalizerStringScalar;
  std::string constantInitalizerStringVector;
  while (!constantInitalizers.empty()) {

    auto &pair = constantInitalizers.front();

    // Just output the constant as "const T C1 = CONST;"
    constantInitalizerStringScalar += pair.first + pair.second + ";\n";

    // Turn the constant into a vector. I.E for vector size of 2: "const T C1
    // = {CONST, CONST};"
    constantInitalizerStringVector += pair.first + "{";
    for (unsigned i = 0; i < vectorizationWidth; ++i) {
      constantInitalizerStringVector += pair.second;
      if (i != vectorizationWidth - 1) {
        constantInitalizerStringVector += ", ";
      }
    }
    constantInitalizerStringVector += "};\n";
    constantInitalizers.pop();
  }
  // Create vectorised versions of all the scalar Tensors
  for (unsigned i = 0; i < inputs.size(); i++) {
    if (inputs[i].numElements() == 1) {
      const std::string type = getTypeAlias(inputs[i].elementType().toString());
      constantInitalizerStringVector +=
          type + " load" + std::to_string(i + 1) + "={";
      for (unsigned j = 0; j < vectorizationWidth; j++) {
        constantInitalizerStringVector +=
            "in" + std::to_string(i + 1) +
            (j != vectorizationWidth - 1 ? "," : "};\n");
      }
    }
  }
  std::string vertexName =
      createVertexName(expr, inputs, inPlace, allInputsScalar);

  if (graph.hasCodelet(vertexName)) {
    logging::debug("Codelet already in graph {}", vertexName);
    return vertexName;
  }

  std::stringstream stream;
  std::stringstream body_stream;

  addHeader(stream);

  stream << R"l(
  class )l";

  stream << vertexName << " : public Vertex {\npublic:\n";

  // Constructor.
  stream << vertexName << "();\n";

  // The output. Aligned to 8 to support vectorization.
  if (!inPlace) {
    assert(!data.empty() && "Attempting to read data stack which is empty");
    if (allInputsScalar) {
      body_stream << "Output<" << data.top().second.toString() << "> out;\n";
    } else {
      body_stream << "Output<Vector<" << data.top().second.toString()
                  << ",VectorLayout::SPAN, 8 >> out;\n";
    }
  }

  // The inputs/inplace outputs. Aligned to 8 for vectorization. We generate
  // these even if they are unused as the part that runs the code doesn't have
  // the information on which inputs are used or not.
  for (unsigned i = 0; i < inputs.size(); ++i) {
    if (i == 0 && inPlace) {
      if (allInputsScalar) {
        body_stream << "InOut<" << inputs[i].elementType().toString() << ">";
      } else {
        body_stream << "InOut<Vector<" << inputs[i].elementType().toString()
                    << ",VectorLayout::SPAN, 8 >>";
      }
    } else {
      if (inputs[i].numElements() == 1) {
        body_stream << "Input<" << inputs[i].elementType().toString() << ">";
      } else {
        body_stream << "Input<Vector<" << inputs[i].elementType().toString()
                    << ", VectorLayout::ONE_PTR, 8>>";
      }
    }
    body_stream << " in" << std::to_string(i + 1) << ";\n";
  }

  // Add the start of the actual compute function.
  body_stream << R"l(
          bool compute() {)l";

  // If we are vectorizing we will need a serial section to calculate the
  // remainder if the vectorization amount doesn't divide evenly.
  if (allInputsScalar) {
    body_stream << R"l(
          unsigned startIndex = 0;
          unsigned remainder = 1;
        )l";
  } else {
    if (inPlace) {
      body_stream << R"l(
          unsigned startIndex = 0;
          unsigned remainder = in1.size();
        )l";
    } else {
      body_stream << R"l(
              unsigned startIndex = 0;
              unsigned remainder = out.size();)l";
    }
  }
  // If we can generate a vectorized version add it to the codelet.
  if (vectorizationIsSupported && vectorizationWidth > 1 && !allInputsScalar) {
    addVectorizedSection(body_stream, vectorizationWidth, initalizerString,
                         constantInitalizerStringVector);
  }

  addSerialSection(body_stream, initalizerString,
                   constantInitalizerStringScalar, allInputsScalar);

  stream << body_stream.str();

  stream << R"l(
          }  // End loop
        }// End serial version.
      return true;
      }
    };
  )l";

  logging::debug("Adding codelet {} to graph", vertexName);
  graph.addCodelets(stream);

  return vertexName;
}

std::string GenerateCodeletFromMapExpr::createVertexName(
    const expr::Expr &expr, const std::vector<poplar::Tensor> &inputs,
    const bool inPlace, const bool allInputsScalar) {
  std::string result = "Fused_" + expr.name(inputs);
  result += std::to_string(inPlace);
  result += std::to_string(allInputsScalar);
  for (const auto &input : inputs) {
    result += std::to_string(input.numElements() == 1);
  }

  return result;
}

} // namespace popops
