// Copyright (c) 2017 Graphcore Ltd. All rights reserved.
#ifndef _popsolver_Scheduler_hpp_
#define _popsolver_Scheduler_hpp_

#include <popsolver/Model.hpp>

#include <cassert>
#include <queue>
#include <vector>

namespace popsolver {

/// The Scheduler class schedules propagation of constraints. All modifications
/// to domains of variables are made via method on this class to ensure that
/// the relevent constraints are propagated whenever a variable's domain
/// changes.
class Scheduler {
  Domains domains;
  std::vector<Constraint *> constraints;
  /// Map from each variable to the constraint number to propagate when the
  /// domain of the variable changes.
  std::vector<std::vector<Variable::IndexType>> variableConstraints;
  std::queue<Variable::IndexType> worklist;
  std::vector<bool> queued;
  void queueConstraints(Variable v) {
    if (v.id < variableConstraints.size()) {
      for (auto c : variableConstraints[v.id]) {
        if (!queued[c]) {
          worklist.push(c);
          queued[c] = true;
        }
      }
    }
  }

public:
  Scheduler(Domains domains, std::vector<Constraint *> constraints);
  const Domains &getDomains() { return domains; }
  void setDomains(Domains value) { domains = value; }
  void set(Variable v, DataType value) {
    assert(value >= domains[v].min_);
    assert(value <= domains[v].max_);
    domains[v].min_ = domains[v].max_ = value;
    queueConstraints(v);
  }
  void setMin(Variable v, DataType value) {
    assert(value >= domains[v].min_);
    assert(value <= domains[v].max_);
    domains[v].min_ = value;
    queueConstraints(v);
  }
  void setMax(Variable v, DataType value) {
    assert(value >= domains[v].min_);
    assert(value <= domains[v].max_);
    domains[v].max_ = value;
    queueConstraints(v);
  }
  std::pair<bool, ConstraintEvaluationSummary> propagate();
  std::pair<bool, ConstraintEvaluationSummary> initialPropagate();
};

} // End namespace popsolver.

#endif // _popsolver_Scheduler_hpp_
