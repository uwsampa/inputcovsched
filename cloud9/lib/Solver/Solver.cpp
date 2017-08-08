//===-- Solver.cpp --------------------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Solver.h"
#include "klee/SolverImpl.h"

#include "SolverStats.h"
#include "STPBuilder.h"

#include "klee/Constraints.h"
#include "klee/Expr.h"
#include "klee/TimerStatIncrementer.h"
#include "klee/util/Assignment.h"
#include "klee/util/ExprPPrinter.h"
#include "klee/util/ExprUtil.h"
#include "klee/Internal/Support/Timer.h"

#include "llvm/Support/Process.h"
#include "llvm/Support/CommandLine.h"

#include "cloud9/instrum/Timing.h"
#include "cloud9/instrum/InstrumentationManager.h"
#include "cloud9/Logger.h"


#define vc_bvBoolExtract IAMTHESPAWNOFSATAN

#include <cassert>
#include <cstdio>
#include <map>
#include <vector>

#include <errno.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <iostream>
#include <fstream>

using namespace klee;
using namespace llvm;

using cloud9::instrum::Timer;

namespace {
  cl::opt<bool>
  DebugSTPCheckCounterexamples("debug-stp-check-counterexamples", cl::init(false));

  cl::opt<bool, true>
  DebugSTPEnableVerboseDebugging("debug-stp-enable-verbose-debugging",
                                 cl::location(STPSolver::EnableVerboseDebugging), cl::init(false));

  cl::opt<bool>
  EnableSTPTimeouts("enable-stp-timeouts", cl::init(false));

  cl::opt<bool>
  EnableSTPBadAllocs("enable-stp-badalloc-catching", cl::init(false));
}

/***/

const char *Solver::validity_to_str(Validity v) {
  switch (v) {
  default:    return "Unknown";
  case True:  return "True";
  case False: return "False";
  }
}

Solver::~Solver() {
  delete impl;
}

SolverImpl::~SolverImpl() {
}

bool Solver::evaluate(const Query& query, Validity &result) {
  assert(query.expr->getWidth() == Expr::Bool && "Invalid expression type!");

  // Maintain invariants implementations expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE->isTrue() ? True : False;
    return true;
  }

  return impl->computeValidity(query, result);
}

bool SolverImpl::computeValidity(const Query& query, Solver::Validity &result) {
  bool isTrue, isFalse;
  if (!computeTruth(query, isTrue))
    return false;
  if (isTrue) {
    result = Solver::True;
  } else {
    if (!computeTruth(query.negateExpr(), isFalse))
      return false;
    result = isFalse ? Solver::False : Solver::Unknown;
  }
  return true;
}

bool Solver::mustBeTrue(const Query& query, bool &result) {
  assert(query.expr->getWidth() == Expr::Bool && "Invalid expression type!");

  // Maintain invariants implementations expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE->isTrue() ? true : false;
    return true;
  }

  return impl->computeTruth(query, result);
}

bool Solver::mustBeFalse(const Query& query, bool &result) {
  return mustBeTrue(query.negateExpr(), result);
}

bool Solver::mayBeTrue(const Query& query, bool &result) {
  bool res;
  if (!mustBeFalse(query, res))
    return false;
  result = !res;
  return true;
}

bool Solver::mayBeFalse(const Query& query, bool &result) {
  bool res;
  if (!mustBeTrue(query, res))
    return false;
  result = !res;
  return true;
}

bool Solver::getValue(const Query& query, ref<ConstantExpr> &result) {
  // Maintain invariants implementation expect.
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(query.expr)) {
    result = CE;
    return true;
  }

  // FIXME: Push ConstantExpr requirement down.
  ref<Expr> tmp;
  if (!impl->computeValue(query, tmp))
    return false;

  result = cast<ConstantExpr>(tmp);
  return true;
}

bool Solver::getInitialValues(const Query& query,
                              const std::vector<const Array*> &objects,
                              Assignment &result) {
  bool hasSolution;
  bool success =
    impl->computeInitialValues(query, objects, result, hasSolution);
  // FIXME: Propogate this out.
  if (!hasSolution)
    return false;

  return success;
}

ExprPair Solver::getRange(const Query& query) {
  ref<Expr> e = query.expr;
  Expr::Width width = e->getWidth();
  uint64_t min, max;

  if (width==1) {
    Solver::Validity result;
    if (!evaluate(query, result))
      assert(0 && "computeValidity failed");
    switch (result) {
    case Solver::True:
      min = max = 1; break;
    case Solver::False:
      min = max = 0; break;
    default:
      min = 0, max = 1; break;
    }
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(e)) {
    min = max = CE->getZExtValue();
  } else {
    // binary search for # of useful bits
    uint64_t lo=0, hi=width, mid, bits=0;
    while (lo<hi) {
      mid = lo + (hi - lo)/2;
      bool res;
      bool success =
        mustBeTrue(query.withExpr(
                     EqExpr::create(LShrExpr::create(e,
                                                     ConstantExpr::create(mid,
                                                                          width)),
                                    ConstantExpr::create(0, width))),
                   res);

      assert(success && "FIXME: Unhandled solver failure");
      (void) success;

      if (res) {
        hi = mid;
      } else {
        lo = mid+1;
      }

      bits = lo;
    }

    // could binary search for training zeros and offset
    // min max but unlikely to be very useful

    // check common case
    bool res = false;
    bool success =
      mayBeTrue(query.withExpr(EqExpr::create(e, ConstantExpr::create(0,
                                                                      width))),
                res);

    assert(success && "FIXME: Unhandled solver failure");
    (void) success;

    if (res) {
      min = 0;
    } else {
      // binary search for min
      lo=0, hi=bits64::maxValueOfNBits(bits);
      while (lo<hi) {
        mid = lo + (hi - lo)/2;
        bool res = false;
        bool success =
          mayBeTrue(query.withExpr(UleExpr::create(e,
                                                   ConstantExpr::create(mid,
                                                                        width))),
                    res);

        assert(success && "FIXME: Unhandled solver failure");
        (void) success;

        if (res) {
          hi = mid;
        } else {
          lo = mid+1;
        }
      }

      min = lo;
    }

    // binary search for max
    lo=min, hi=bits64::maxValueOfNBits(bits);
    while (lo<hi) {
      mid = lo + (hi - lo)/2;
      bool res;
      bool success =
        mustBeTrue(query.withExpr(UleExpr::create(e,
                                                  ConstantExpr::create(mid,
                                                                       width))),
                   res);

      assert(success && "FIXME: Unhandled solver failure");
      (void) success;

      if (res) {
        hi = mid;
      } else {
        lo = mid+1;
      }
    }

    max = lo;
  }

  return std::make_pair(ConstantExpr::create(min, width),
                        ConstantExpr::create(max, width));
}

/***/

class ValidatingSolver : public SolverImpl {
private:
  Solver *solver, *oracle;

  template<class T1, class T2>
  void printError(const Query &query, const T1 &solverSaid, const T2 &oracleSaid);

public:
  ValidatingSolver(Solver *_solver, Solver *_oracle)
    : solver(_solver), oracle(_oracle) {}
  ~ValidatingSolver() { delete solver; }

  bool computeValidity(const Query&, Solver::Validity &result);
  bool computeTruth(const Query&, bool &isValid);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            Assignment &result,
                            bool &hasSolution);
};

template<class T1, class T2>
void ValidatingSolver::printError(const Query &query,
                                  const T1 &solverSaid,
                                  const T2 &oracleSaid) {
  std::cerr << "========= ValidatingSolver: FAILURE =========\n"
            << "=== Constraints ===\n"
            << query.constraints.getSummaryExpr() << "\n"
            << "=== Query ===\n"
            << query.expr << "\n"
            << "=== Result ===\n"
            << "Solver said: " << solverSaid << "\n"
            << "Oracle said: " << oracleSaid << "\n";
}

bool ValidatingSolver::computeTruth(const Query& query, bool &isValid) {
  bool answer;

  if (!solver->impl->computeTruth(query, isValid))
    return false;
  if (!oracle->impl->computeTruth(query, answer))
    return false;

  if (isValid != answer) {
    printError(query, isValid, answer);
    // Annoying: try again to see if we get the same results.
    // If not, this may be a sign of an STP bug.
    std::cerr << "========= ValidatingSolver: Checking Again =========\n";
    STPSolver::EnableVerboseDebugging = true;
    bool ok;
    ok = solver->impl->computeTruth(query, isValid);
    assert(ok);
    std::cerr << "========= ValidatingSolver: Checking Oracle ========\n";
    ok = oracle->impl->computeTruth(query, answer);
    assert(ok);
    std::cerr << "Solver said: " << isValid << "\n"
              << "Oracle said: " << answer << "\n";
    assert(0 && "invalid solver result (computeTruth)");
  }

  return true;
}

bool ValidatingSolver::computeValidity(const Query& query, Solver::Validity &result) {
  Solver::Validity answer;

  if (!solver->impl->computeValidity(query, result))
    return false;
  if (!oracle->impl->computeValidity(query, answer))
    return false;

  if (result != answer) {
    printError(query, result, answer);
    // Annoying: try again to see if we get the same results.
    // If not, this may be a sign of an STP bug.
    std::cerr << "========= ValidatingSolver: Checking Again =========\n";
    STPSolver::EnableVerboseDebugging = true;
    bool ok;
    ok = solver->impl->computeValidity(query, result);
    assert(ok);
    ok = oracle->impl->computeValidity(query, answer);
    assert(ok);
    std::cerr << "Solver said: " << result << "\n"
              << "Oracle said: " << answer << "\n";
    assert(0 && "invalid solver result (computeValidity)");
  }

  return true;
}

bool ValidatingSolver::computeValue(const Query& query, ref<Expr> &result) {
  bool answer;

  if (!solver->impl->computeValue(query, result))
    return false;
  // We don't want to compare, but just make sure this is a legal solution.
  Query neq = query.withExpr(NeExpr::create(query.expr, result));
  if (!oracle->impl->computeTruth(neq, answer))
    return false;

  if (answer) {
    bool ok, shouldBeFalse;
    ok = solver->impl->computeTruth(neq, shouldBeFalse);
    printError(query, shouldBeFalse, answer);
    std::cerr << "Solver returned value: " << result << "\n";
    // Annoying: try again to see if we get the same results.
    // If not, this may be a sign of an STP bug.
    std::cerr << "========= ValidatingSolver: Checking Again =========\n";
    STPSolver::EnableVerboseDebugging = true;
    ok = solver->impl->computeValue(query, result);
    assert(ok);
    ok = solver->impl->computeTruth(neq, shouldBeFalse);
    assert(ok);
    ok = oracle->impl->computeTruth(neq, answer);
    assert(ok);
    std::cerr << "Solver said: " << shouldBeFalse << "\n"
              << "Oracle said: " << answer << "\n"
              << "Solver returned value: " << result << "\n";
    assert(0 && "invalid solver result (computeValue)");
  }

  return true;
}

bool
ValidatingSolver::computeInitialValues(const Query& query,
                                       const std::vector<const Array*> &objects,
                                       Assignment &result,
                                       bool &hasSolution) {
  bool answer;

  if (!solver->impl->computeInitialValues(query, objects, result, hasSolution))
    return false;

  if (hasSolution) {
    // Assert the bindings as constraints, and verify that the
    // conjunction of the actual constraints is satisfiable.
    ConstraintManager bindings(result.buildConstraints());
    ref<Expr> queryConstraints = AndExpr::create(Expr::createIsZero(query.expr),
                                                 query.constraints.getSummaryExpr());

    if (!oracle->impl->computeTruth(Query(bindings, queryConstraints), answer))
      return false;
    if (!answer) {
      printError(query, hasSolution, answer);
      assert(0 && "invalid solver result (computeInitialValues)");
    }
  } else {
    if (!oracle->impl->computeTruth(query, answer))
      return false;
    if (!answer) {
      printError(query, hasSolution, answer);
      assert(0 && "invalid solver result (computeInitialValues)");
    }
  }

  return true;
}

Solver *klee::createValidatingSolver(Solver *s, Solver *oracle) {
  return new Solver(new ValidatingSolver(s, oracle));
}

/***/

class DummySolverImpl : public SolverImpl {
public:
  DummySolverImpl() {}

  bool computeValidity(const Query&, Solver::Validity &result) {
    ++stats::queries;
    // FIXME: We should have stats::queriesFail;
    return false;
  }
  bool computeTruth(const Query&, bool &isValid) {
    ++stats::queries;
    // FIXME: We should have stats::queriesFail;
    return false;
  }
  bool computeValue(const Query&, ref<Expr> &result) {
    ++stats::queries;
    ++stats::queryCounterexamples;
    return false;
  }
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            Assignment &result,
                            bool &hasSolution) {
    ++stats::queries;
    ++stats::queryCounterexamples;
    return false;
  }
};

Solver *klee::createDummySolver() {
  return new Solver(new DummySolverImpl());
}

/***/

class STPSolverImpl : public SolverImpl {
private:
  /// The solver we are part of, for access to public information.
  STPSolver *solver;
  VC vc;
  STPBuilder *builder;
  double timeout;

  ExprHandle pushQuery(const Query &query);
  void popQuery();

public:
  STPSolverImpl(STPSolver *_solver, bool _useForkedSTP, bool _optimizeDivides = true);
  ~STPSolverImpl();

  char *getConstraintLog(const Query&);
  void setTimeout(double _timeout) { timeout = _timeout; }

  bool computeTruth(const Query&, bool &isValid);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            Assignment &result,
                            bool &hasSolution);
};

static void stp_error_handler(const char* err_msg) {
  fprintf(stderr, "error: STP Error: %s\n", err_msg);
  abort();
}

STPSolverImpl::STPSolverImpl(STPSolver *_solver, bool _useForkedSTP, bool _optimizeDivides)
  : solver(_solver),
    vc(vc_createValidityChecker()),
    builder(new STPBuilder(vc, _optimizeDivides)),
    timeout(0.0)
{
  assert(vc && "unable to create validity checker");
  assert(builder && "unable to create STPBuilder");

#ifdef HAVE_EXT_STP
  // In newer versions of STP, a memory management mechanism has been
  // introduced that automatically invalidates certain C interface
  // pointers at vc_Destroy time.  This caused double-free errors
  // due to the ExprHandle destructor also attempting to invalidate
  // the pointers using vc_DeleteExpr.  By setting EXPRDELETE to 0
  // we restore the old behaviour.
  vc_setInterfaceFlags(vc, EXPRDELETE, 0);
  // Work around divide-by-zero bug in STP
  make_division_total(vc);

  if (DebugSTPCheckCounterexamples) {
    // See process_argument() in src/AST/ASTmisc.cpp for a list of flags.
    // The 'd' flag will make STP check each counter example for validity
    // before returning.  N.B.: this flag has been set by default in some
    // versions of STP, so this might be a nop.
    vc_setFlags(vc, 'd');
  }
#endif

  vc_registerErrorHandler(::stp_error_handler);
}

STPSolverImpl::~STPSolverImpl() {
  delete builder;
  vc_Destroy(vc);
}

/***/

bool STPSolver::EnableVerboseDebugging = false;

STPSolver::STPSolver(bool optimizeDivides)
  : Solver(new STPSolverImpl(this, optimizeDivides))
{
}

char *STPSolver::getConstraintLog(const Query &query) {
  return static_cast<STPSolverImpl*>(impl)->getConstraintLog(query);
}

void STPSolver::setTimeout(double timeout) {
  static_cast<STPSolverImpl*>(impl)->setTimeout(timeout);
}

/***/

ExprHandle STPSolverImpl::pushQuery(const Query &query) {
  vc_push(vc);
  builder->assertAssumptions(query.constraints);
  return builder->constructQuery(query.expr);
}

void STPSolverImpl::popQuery() {
  builder->clearCaches();
  vc_pop(vc);
}

char *STPSolverImpl::getConstraintLog(const Query &query) {
  assert(query.expr == Expr::False && "Unexpected expression in query!");

  pushQuery(query);

  char *buffer;
  unsigned long length;
  vc_printQueryStateToBuffer(vc, builder->getFalse(), &buffer, &length, false);
  popQuery();

  return buffer;
}

bool STPSolverImpl::computeTruth(const Query& query, bool &isValid) {
  std::vector<const Array*> objects;
  Assignment tmp;
  bool hasSolution;

  if (!computeInitialValues(query, objects, tmp, hasSolution))
    return false;

  isValid = !hasSolution;
  return true;
}

bool STPSolverImpl::computeValue(const Query& query, ref<Expr> &result) {
  std::vector<const Array*> objects;
  Assignment tmp;
  bool hasSolution;

  // Find the object used in the expression, and compute an assignment
  // for them.
  findSymbolicObjects(query.expr, objects);
  if (!computeInitialValues(query.withFalse(), objects, tmp, hasSolution))
    return false;
  assert(hasSolution && "state has invalid constraint set");

  // Evaluate the expression with the computed assignment.
  result = tmp.evaluate(query.expr);
  return true;
}

static int runSTPQuery(::VC vc, ::VCExpr q, const double timeoutSeconds) {
  if (EnableSTPTimeouts && timeoutSeconds > 0) {
    return vc_query_with_timeout(vc, q, (int)timeoutSeconds * 1000);
  } else {
    return vc_query(vc, q);
  }
}

static bool runAndGetCex(::VC vc, STPBuilder *builder, ::VCExpr q,
                         const std::vector<const Array*> &objects,
                         Assignment &result,
                         bool &hasSolution,
                         const double timeoutSeconds) {
  // Run STP
  int res;
  if (EnableSTPBadAllocs) {
    try {
      res = runSTPQuery(vc, q, timeoutSeconds);
    } catch (std::bad_alloc &ex) {
      std::cerr << "WARNING: STP threw bad_alloc: " << ex.what() << "\n";
      return false;
    }
  } else {
    res = runSTPQuery(vc, q, timeoutSeconds);
  }

  // Check for timeout
  if (res == 3) {
    std::cerr << "WARNING: STP timed out\n";
    return false;
  }

  hasSolution = !res;

  if (hasSolution) {
    if (STPSolver::EnableVerboseDebugging) {
      std::cerr << "Success: extracting assignment for " << objects.size() << " objects\n";
    }

    for (std::vector<const Array*>::const_iterator
         it = objects.begin(), ie = objects.end(); it != ie; ++it) {
      const Array *array = *it;
      ::VCExpr *indices;
      ::VCExpr *values;
      int size;
      vc_getCounterExampleArray(vc, builder->getInitialArray(array), &indices, &values, &size);

      ExprArrayHolder tmpI(indices, size);
      ExprArrayHolder tmpV(values, size);

      if (STPSolver::EnableVerboseDebugging) {
        std::cerr << "Assignment for " << array->name << " has " << size << " bytes\n";
      }

      for (int k = 0; k < size; ++k) {
        ExprHandle indexExpr = vc_getCounterExample(vc, indices[k]);
        ExprHandle valueExpr = vc_getCounterExample(vc, values[k]);
        unsigned i = getBVUnsigned(indexExpr);
        unsigned v = getBVUnsigned(valueExpr);
        result.addBinding(array, i, (unsigned char)v);
      }
    }
  }

  return true;
}

bool
STPSolverImpl::computeInitialValues(const Query &query,
                                    const std::vector<const Array*> &userObjects,
                                    Assignment &result,
                                    bool &hasSolution) {
  TimerStatIncrementer tsi(stats::queryTime);
  ++stats::queries;
  ++stats::queryCounterexamples;

  const std::vector<const Array*> *objects = &userObjects;
  ExprHandle stp_e = pushQuery(query);

  if (STPSolver::EnableVerboseDebugging) {
    char *buf;
    unsigned long len;
    vc_printQueryStateToBuffer(vc, stp_e, &buf, &len, false);
    fprintf(stderr, "========= STPQuery =========\n%.*s\n", (unsigned) len, buf);
    // Go manually extract all objects from the query
    if (userObjects.empty()) {
      std::vector<const Array*> *tmp = new std::vector<const Array*>;
      ref<Expr> e[2] = { query.constraints.getSummaryExpr(),
                         query.expr };
      findSymbolicObjects(e, e+2, *tmp);
      objects = tmp;
    }
  }

  Timer t;
  t.start();
  const bool success = runAndGetCex(vc, builder, stp_e, *objects, result, hasSolution, timeout);
  t.stop();
  cloud9::instrum::theInstrManager.recordEvent(cloud9::instrum::SMTSolve, t);

  if (STPSolver::EnableVerboseDebugging) {
    std::cerr << "Result: success=" << success << " hasSolution=" << hasSolution << "\n";
    result.dump(std::cerr);
    std::cerr << "Evaluated Query:\n";
    std::cerr << result.evaluate(query.expr) << "\n";
    vc_printCounterExample(vc);
    if (objects != &userObjects)
      delete objects;
  }

  if (success) {
    if (hasSolution)
      ++stats::queriesInvalid;
    else
      ++stats::queriesValid;
  }

  popQuery();

  return success;
}
