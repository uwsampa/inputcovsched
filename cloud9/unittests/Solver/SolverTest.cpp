//===-- SolverTest.cpp ----------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include <iostream>
#undef GTEST_USE_OWN_TR1_TUPLE
#define GTEST_USE_OWN_TR1_TUPLE 0
#include "gtest/gtest.h"

#include "klee/Constraints.h"
#include "klee/Expr.h"
#include "klee/Solver.h"
#include "llvm/ADT/StringExtras.h"

using namespace klee;

// Yuck
namespace klee {
testing::Message& operator<<(testing::Message &msg, const ref<Expr> &expr) {
  std::ostringstream buf;
  buf << expr;
  return (msg << buf.str());
}
}

namespace {

const int g_constants[] = { -1, 1, 4, 17, 0 };
const Expr::Width g_types[] = { Expr::Bool,
				Expr::Int8,
				Expr::Int16,
				Expr::Int32,
				Expr::Int64 };

ref<Expr> getConstant(int value, Expr::Width width) {
  int64_t ext = value;
  uint64_t trunc = ext & (((uint64_t) -1LL) >> (64 - width));
  return ConstantExpr::create(trunc, width);
}

template<class T>
void testOperation(Solver &solver,
                   int value,
                   Expr::Width operandWidth,
                   Expr::Width resultWidth) {
  std::vector<Expr::CreateArg> symbolicArgs;
  
  for (unsigned i = 0; i < T::MyNumKids; i++) {
    if (!T::isValidKidWidth(i, operandWidth))
      return;

    static uint64_t id = 0;
    const Array *array = Array::createSymbolic("arr" + llvm::utostr(++id));
    ref<Expr> re = Expr::createArrayReadAtIndex(array, getConstant(0,32), operandWidth, true);
    symbolicArgs.push_back(Expr::CreateArg(re));
  }
  
  if (T::needsResultType())
    symbolicArgs.push_back(Expr::CreateArg(resultWidth));
  
  ref<Expr> fullySymbolicExpr = Expr::createFromKind(T::MyKind, symbolicArgs);

  // For each kid, replace the kid with a constant value and verify
  // that the fully symbolic expression is equivalent to it when the
  // replaced value is appropriated constrained.
  for (unsigned kid = 0; kid < T::MyNumKids; kid++) {
    std::vector<Expr::CreateArg> partiallyConstantArgs(symbolicArgs);
    for (unsigned i = 0; i < T::MyNumKids; i++)
      if (i==kid)
        partiallyConstantArgs[i] = getConstant(value, operandWidth);

    ref<Expr> expr =
      NotOptimizedExpr::create(EqExpr::create(partiallyConstantArgs[kid].expr,
                                              symbolicArgs[kid].expr));
    
    ref<Expr> partiallyConstantExpr =
      Expr::createFromKind(T::MyKind, partiallyConstantArgs);
    
    ref<Expr> queryExpr = EqExpr::create(fullySymbolicExpr, 
                                         partiallyConstantExpr);
    
    ConstraintManager constraints;
    constraints.addConstraint(expr);
    bool res;
    bool success = solver.mustBeTrue(Query(constraints, queryExpr), res);
    EXPECT_EQ(true, success) << "Constraint solving failed";

    if (success) {
      using klee::operator<<;
      EXPECT_EQ(true, res) << "Evaluation failed!\n" 
                           << "query:\n" << queryExpr << "\n"
                           << "with:\n" << expr << "\n";
    }
  }
}

template<class T>
void testOpcode(Solver &solver, bool tryBool = true, bool tryZero = true, 
                unsigned maxWidth = 64) {
  for (unsigned j=0; j<sizeof(g_types)/sizeof(g_types[0]); j++) {
    Expr::Width type = g_types[j]; 

    if (type > maxWidth) continue;

    for (unsigned i=0; i<sizeof(g_constants)/sizeof(g_constants[0]); i++) {
      int value = g_constants[i];
      if (!tryZero && !value) continue;
      if (type == Expr::Bool && !tryBool) continue;

      // Shifting by a negative amount is undefined
      if (value < 0 &&
          (T::MyKind == Expr::Shl || T::MyKind == Expr::LShr || T::MyKind == Expr::AShr))
        continue;

      if (!T::needsResultType()) {
        testOperation<T>(solver, value, type, type);
        continue;
      }

      for (unsigned k=0; k<sizeof(g_types)/sizeof(g_types[0]); k++) {
        Expr::Width resultType = g_types[k];
          
        // nasty hack to give only Trunc/ZExt/SExt the right types
        if (T::MyKind == Expr::SExt || T::MyKind == Expr::ZExt) {
          if (Expr::getMinBytesForWidth(type) >= 
              Expr::getMinBytesForWidth(resultType)) 
            continue;
        }
            
        testOperation<T>(solver, value, type, resultType);
      }
    }
  }
}

TEST(SolverTest, Evaluation) {
  // Ugh: this is not initialized by the Expr library
  // Since we don't link with Core, we have to do it ourself
  Expr::True = ConstantExpr::create(1, Expr::Bool);
  Expr::False = ConstantExpr::create(0, Expr::Bool);

  STPSolver *stpSolver = new STPSolver(true); 
  Solver *solver = stpSolver;

  solver = createCexCachingSolver(solver);
  solver = createCachingSolver(solver);
  solver = createIndependentSolver(solver);

  testOpcode<SelectExpr>(*solver);
  testOpcode<ZExtExpr>(*solver);
  testOpcode<SExtExpr>(*solver);
  
  testOpcode<AddExpr>(*solver);
  testOpcode<SubExpr>(*solver);
  testOpcode<MulExpr>(*solver, false, true, 8);
  testOpcode<SDivExpr>(*solver, false, false, 8);
  testOpcode<UDivExpr>(*solver, false, false, 8);
  testOpcode<SRemExpr>(*solver, false, false, 8);
  testOpcode<URemExpr>(*solver, false, false, 8);
  testOpcode<ShlExpr>(*solver, false);
  testOpcode<LShrExpr>(*solver, false);
  testOpcode<AShrExpr>(*solver, false);
  testOpcode<AndExpr>(*solver);
  testOpcode<OrExpr>(*solver);
  testOpcode<XorExpr>(*solver);

  testOpcode<EqExpr>(*solver);
  testOpcode<NeExpr>(*solver);
  testOpcode<UltExpr>(*solver);
  testOpcode<UleExpr>(*solver);
  testOpcode<UgtExpr>(*solver);
  testOpcode<UgeExpr>(*solver);
  testOpcode<SltExpr>(*solver);
  testOpcode<SleExpr>(*solver);
  testOpcode<SgtExpr>(*solver);
  testOpcode<SgeExpr>(*solver);

  delete solver;
}

}
