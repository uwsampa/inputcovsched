//===-- Assignment.cpp ---------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/util/Assignment.h"

namespace klee {

void Assignment::addBinding(const Array *array, unsigned index, unsigned char value) {
  Binding b;
  if (const BindingsTy::value_type *it = _bindings.lookup(array)) {
    b = it->second;
  }
  b.add(index, value);
  _bindings = _bindings.replace(BindingsTy::value_type(array, b));
}

bool Assignment::hasBinding(const Array *array, unsigned index) const {
  // Look for an explicit mapping
  if (const BindingsTy::value_type *it = _bindings.lookup(array)) {
    bool found;
    it->second.lookup(index, &found);
    return found;
  }
  return false;
}

void Assignment::clear() {
  _bindings = BindingsTy();
}

ref<Expr> Assignment::evaluate(const Array *array, unsigned index, bool defaultToZero) const {
  // Look for an explicit mapping
  if (const BindingsTy::value_type *it = _bindings.lookup(array)) {
    bool found;
    unsigned char value = it->second.lookup(index, &found);
    if (found)
      return ConstantExpr::alloc(value, Expr::Int8);
  }
  // No explicit mapping
  if (defaultToZero) {
    return Expr::Zero8;
  } else {
    return Expr::One8;
  }
}

ref<Expr> Assignment::evaluate(ref<Expr> e) const {
  AssignmentEvaluator v(*this);
  return v.visit(e);
}

ref<Expr> Assignment::buildConstraints() const {
  ref<Expr> res = Expr::True;
  for (BindingsTy::iterator it = _bindings.begin(); it != _bindings.end(); ++it) {
    const Array *array = it->first;
    const Binding &b = it->second;
    for (Binding::SparseMapTy::iterator sp = b._sparse.begin(); sp != b._sparse.end(); ++sp) {
      ref<Expr> eq =
        EqExpr::create(ReadExpr::alloc(UpdateList(array, NULL),
                                       ConstantExpr::alloc(sp->first, array->getDomain()),
                                       false),
                       ConstantExpr::alloc(sp->second, array->getRange()));
      res = AndExpr::create(eq, res);
    }
  }
  return res;
}

void Assignment::buildSolutionList(SolutionListTy *out) const {
  for (BindingsTy::iterator it = _bindings.begin(); it != _bindings.end(); ++it) {
    const Array *array = it->first;
    const Binding &b = it->second;
    std::vector<unsigned char> data;
    if (!b.empty()) {
      const unsigned int maxIndex = b.maxIndex();
      data.resize(maxIndex+1);
      for (unsigned int k = 0; k <= maxIndex; ++k)
        data[k] = b.lookup(k);
    }
    out->push_back(SolutionListTy::value_type(array->name, data));
  }
}

void Assignment::dump(std::ostream &out, const char *prefix) const {
  for (BindingsTy::iterator it = _bindings.begin(); it != _bindings.end(); ++it) {
    out << prefix << it->first->name << " = { ";
    const Binding &b = it->second;
    for (Binding::SparseMapTy::iterator sp = b._sparse.begin(); sp != b._sparse.end(); ++sp) {
      out << "[" << sp->first << "]=" << (int)sp->second << ",";
    }
    out << "}\n";
  }
}

}  // namespace klee
