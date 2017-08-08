/*
 * Input Covering Schedules
 * Author: Tom Bergan
 *
 * Instrumentation for constrained execution
 *
 */

#include "ics/Instrumentor.h"

#include "klee/Context.h"
#include "klee/Executor.h"
#include "klee/ExecutionState.h"
#include "klee/SymbolicObjectTable.h"
#include "klee/Init.h"
#include "klee/Internal/ADT/ImmutableList.h"
#include "klee/Internal/Module/InstructionInfoTable.h"
#include "klee/Internal/Module/KModule.h"
#include "klee/Internal/Support/ModuleUtil.h"
#include "klee/Internal/Support/Timer.h"
#include "klee/util/ExprHashMap.h"
#include "klee/util/ExprUtil.h"
#include "klee/util/ExprVisitor.h"
#include "klee/util/HashUtil.h"
#include "klee/util/RefCounted.h"
#include "klee/util/StringUtil.h"
#include "cloud9/worker/KleeCommon.h"
#include "../Core/Common.h"
#include "../Core/TimingSolver.h"
#include "../Module/Passes.h"

#include "llvm/Argument.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/IntrinsicInst.h"
#include "llvm/LLVMContext.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/ValueSymbolTable.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Analysis/DebugInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bitcode/ReaderWriter.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/raw_os_ostream.h"
#include "llvm/Target/TargetData.h"

#include "dsa/DataStructureAA.h"

#include <memory>
#include <sstream>
#include <tr1/unordered_set>

// Importing a bunch of things explicitly to minimize namespace
// pollution: e.g., klee and llvm both have a ConstantExpr
using klee::ref;
using klee::RefCounted;
using klee::ExprNaryIterator;
using klee::Array;
using klee::MemoryObject;
using klee::CallingContext;
using klee::CallingContextPrinter;
using klee::SymbolicObject;
using klee::SymbolicObjectTable;
using klee::SymbolicPtrResolution;
using klee::SymbolicPtrResolutionTable;
using klee::Solver;
using klee::ImmutableList;
using klee::ImmutableSet;
using klee::ImmutableMap;
using klee::ImmutableExprSet;
using klee::ImmutableExprMap;
using klee::StringMatcher;
using klee::KInstruction;
using klee::KFunction;
using klee::ThreadUid;
using klee::thread_id_t;
using klee::process_id_t;

using klee::Expr;
using klee::ConstantExpr;
using klee::NotExpr;
using klee::CastExpr;
using klee::BinaryExpr;
using klee::SimplifiableBinaryExpr;
using klee::CmpExpr;
using klee::AndExpr;
using klee::OrExpr;
using klee::AddExpr;
using klee::SubExpr;
using klee::UleExpr;
using klee::EqExpr;
using klee::SelectExpr;
using klee::ConcatExpr;
using klee::ExtractExpr;
using klee::ReadExpr;
using klee::UpdateList;
using klee::UpdateNode;

using llvm::Value;
using llvm::Argument;
using llvm::Instruction;
using llvm::PHINode;
using llvm::CallInst;
using llvm::InvokeInst;
using llvm::AllocaInst;
using llvm::BranchInst;
using llvm::ReturnInst;
using llvm::UnreachableInst;
using llvm::TerminatorInst;
using llvm::BasicBlock;
using llvm::Function;
using llvm::ConstantInt;
using llvm::ConstantArray;
using llvm::Constant;
using llvm::Type;
using llvm::IntegerType;
using llvm::PointerType;
using llvm::ArrayType;
using llvm::StructType;
using llvm::StringRef;

// XXX
extern "C" void GdbDumpValue(const llvm::Value *v) {
  v->dump();
}
extern "C" void GdbDumpExpr(const klee::Expr *expr) {
  expr->dump();
}
// XXX

namespace klee {
extern void Optimize(llvm::Module*);
extern void OptimizeFull(llvm::Module*);
}

typedef ics::DepthFirstICS::RegionsInfoTy RegionsInfoTy;
typedef ics::DepthFirstICS::ParallelRegionInfo ParallelRegionInfo;

// Ugly
typedef ics::DepthFirstICS::RegionsInfoTy::const_iterator RegionsIterator;
typedef ics::PathManager::ScheduleListTy::const_iterator SchedulesIterator;

namespace ics {

llvm::cl::opt<bool> DoCountInstructions("ics-instrum-do-count-instructions", llvm::cl::init(false));
llvm::cl::opt<bool> OptMinimizeCallLogging("ics-instrum-opt-minimize-call-logging", llvm::cl::init(false));
llvm::cl::opt<bool> OptMinimizeUpdatesOnRead("ics-instrum-opt-minimize-updates-on-read", llvm::cl::init(false));
llvm::cl::opt<bool> DbgInstrumentor("ics-debug-instrumentor", llvm::cl::init(false));

// Defined in SearchStrategy.cpp
void BottomUpCallGraphClosure(const llvm::DSAA::CallerMapTy &BottomUpCG,
                              const llvm::DenseSet<const llvm::Function*> &seeds,
                              const llvm::DenseSet<const llvm::Function*> &ignore,
                              llvm::DenseSet<const llvm::Function*> *closure);

// Misc
bool BasicBlockNameLess(BasicBlock *a, BasicBlock *b) {
  return a->getName() < b->getName();
}

//-----------------------------------------------------------------------------
// The current environment for evaluating a klee expr
// The cache includes only those values that MUST dominate the current inst
//-----------------------------------------------------------------------------

class EvalEnv {
private:
  friend class ref<EvalEnv>;
  unsigned refCount;

  // This get forked on clone()
  // It is used to enumerate the current must-be-true constraints
  klee::ConstraintManager _constraints;

  // These get forked on clone()
  // See comments in BuildBooleanEvaluator()
  typedef ImmutableMap<ref<Expr>, Value*> ReadCacheTy;
  ReadCacheTy _cachedReads;

  typedef ImmutableMap<std::pair<SymbolicObject::Kind,unsigned>, Value*> SymbolicBasePtrCacheTy;
  SymbolicBasePtrCacheTy _cachedBasePtrs;

  typedef ImmutableMap<const SymbolicObject*, Value*> InputPtrCacheTy;
  InputPtrCacheTy _inputPtrs;

  ImmutableList<bool> _enableOrderedReadsStack;

  // These are global for each function
  typedef std::set<std::string> StringSetTy;
  ref< RefCounted<StringSetTy> > _usedNames;
  ref< RefCounted<std::string> > _blockLabelPrefix;
  ImmutableExprMap<unsigned> _availableSnapshottedExprs;

  klee::Executor *_executor;
  klee::ExecutionState *_dummyKState;
  Function *_f;

private:
  void addConstraint(ref<Expr> cond) {
    ref<Expr> old = _constraints.getSummaryExpr();
    _constraints.addConstraint(cond);
    if (klee::clvars::DebugConstraintValidity) {
      // FIXME: code duplication with Executor::addConstraint()
      // Check if the entire constraint set is satisfiable.  We don't simply
      // check mayBeTrue(state, condition), since optimizations such as the
      // IndependentSolver may remove invalid constraints from "state".
      ref<Expr> expr = _constraints.getSummaryExpr();
      _dummyKState->globalConstraints.clear();
      std::cerr << "TEST.MayBeTrue:\n" << expr << "\n";
      bool mayBeTrue;
      bool success =
        _executor->getTimingSolver()->mayBeTrue(*_dummyKState, expr, mayBeTrue);
      if (!success || !mayBeTrue) {
        std::cerr << "ERROR: Invalid Constraint!\n"
                  << "SolverResult: success=" << success << "mayBeTrue=" << mayBeTrue << "\n"
                  << "New Constraint:\n"
                  << cond << "\n\n"
                  << "Prior Constraints:\n"
                  << old << "\n\n"
                  << "Full (simplified) Constraints:\n"
                  << expr << "\n\n";
        assert(0 && "attempt to add invalid constant symbolic constraint");
      }
      Solver::Validity res;
      _dummyKState->globalConstraints = _constraints;
      success = _executor->getTimingSolver()->evaluate(*_dummyKState, Expr::True, res);
      if (!success || res != Solver::True) {
        std::cerr << "ERROR: Invalid Constraint! (via #2)\n"
                  << "SolverResult: success=" << success << "res=" << res << "\n"
                  << "New Constraint:\n"
                  << cond << "\n\n"
                  << "Prior Constraints:\n"
                  << old << "\n\n"
                  << "Full (simplified) Constraints:\n"
                  << expr << "\n\n";
        assert(0 && "attempt to add invalid constant symbolic constraint");
      }
    }
  }

public:
  EvalEnv(Function *f, klee::Executor *executor, ImmutableExprMap<unsigned> snapshots)
    : refCount(0),
      _usedNames(new RefCounted<StringSetTy>),
      _blockLabelPrefix(new RefCounted<std::string>),
      _availableSnapshottedExprs(snapshots),
      _executor(executor),
      _dummyKState(new klee::ExecutionState(_executor, std::vector<ref<Expr> >())),
      _f(f)
  {}

  ref<Expr> constraints() const {
    return _constraints.getSummaryExpr();
  }

  // Cloning

  ref<EvalEnv> clone() const {
    ref<EvalEnv> e = new EvalEnv(_f, _executor, _availableSnapshottedExprs);
    *e = *this;
    return e;
  }

  ref<EvalEnv> cloneTrue(ref<Expr> cond) const {
    ref<EvalEnv> e = clone();
    e->addConstraint(cond);
    return e;
  }

  ref<EvalEnv> cloneFalse(ref<Expr> cond) const {
    ref<EvalEnv> e = clone();
    e->addConstraint(Expr::createIsZero(cond));
    return e;
  }

  ref<EvalEnv> cloneFalseCanFail(ref<Expr> cond) const {
    bool truth;
    _dummyKState->globalConstraints = _constraints;
    cond = Expr::createIsZero(cond);
    bool ok = _executor->getTimingSolver()->mayBeTrue(*_dummyKState, cond, truth);
    if (ok && truth) {
      ref<EvalEnv> e = clone();
      e->addConstraint(cond);
      return e;
    }
    return NULL;
  }

  // Constraint checking

  bool mayBeTrue(ref<Expr> cond) const {
    bool truth;
    _dummyKState->globalConstraints = _constraints;
    bool ok = _executor->getTimingSolver()->mayBeTrue(*_dummyKState, cond, truth);
    return !ok || truth;
  }

  Solver::Validity checkValidity(ref<Expr> cond) const {
    Solver::Validity res;
    _dummyKState->globalConstraints = _constraints;
    bool ok = _executor->getTimingSolver()->evaluate(*_dummyKState, cond, res);
    return ok ? res : Solver::Unknown;
  }

  ref<Expr> simplifyExpr(ref<Expr> expr) const {
    return _constraints.simplifyExpr(expr);
  }

  // Enabling/disabling the hasOrderedReads check
  bool orderedReadsEnabled() const {
    return _enableOrderedReadsStack.empty() || _enableOrderedReadsStack.front();
  }

  void pushDisableOrderedReads() {
    _enableOrderedReadsStack = _enableOrderedReadsStack.push_front(false);
  }

  void popDisableOrderedReads() {
    _enableOrderedReadsStack = _enableOrderedReadsStack.pop_front();
  }

  // Snapshot cache

  bool lookupSnapshotOffset(ref<Expr> expr, unsigned *offset, bool *childIsSnapshotted) {
    const ImmutableExprMap<unsigned>::value_type *v;
    // Simple case: entire expr has a snapshot
    v = _availableSnapshottedExprs.lookup(expr);
    if (v) {
      if (offset) *offset = v->second;
      return true;
    }
    // Complex case: each byte of the expr has a snapshot
    // We check to see if the per-byte snapshots are contiguous in the snapshot buffer
    ref<ReadExpr> base = Expr::hasOrderedReads(expr);
    if (base.get() && isa<ConcatExpr>(expr)) {
      // map each byte of 'expr' to an offset in the snapshot buffer
      std::map<unsigned, unsigned> offsetsMap;
      for (ExprNaryIterator it(expr, Expr::Concat); it != ExprNaryIterator(); ++it) {
        v = _availableSnapshottedExprs.lookup(*it);
        ref<ReadExpr> re = dyn_cast<ReadExpr>(*it);
        if (!v || !re.get())
          continue;
        if (childIsSnapshotted)
          *childIsSnapshotted = true;
        if (ConstantExpr *cex = dyn_cast<ConstantExpr>(SubExpr::create(re->index, base->index)))
          offsetsMap[cex->getZExtValue()] = v->second;
      }
      // ensure that the offsets mapping covers 'expr' and is contiguous
      bool foundAll = (offsetsMap.size() == Expr::getMinBytesForWidth(expr->getWidth()));
      for (size_t k = 0; foundAll && k < offsetsMap.size(); ++k) {
        if (!offsetsMap.count(k) ||
            (k > 0 &&
             (( klee::Context::get().isLittleEndian() && offsetsMap[k] != offsetsMap[k-1]+1) ||
              (!klee::Context::get().isLittleEndian() && offsetsMap[k] != offsetsMap[k-1]-1)))) {
          foundAll = false;
          break;
        }
      }
      if (foundAll) {
        v = _availableSnapshottedExprs.lookup(base);
        assert(v);
        if (offset) *offset = v->second;
        return true;
      }
    }
    return false;
  }

  // Read cache

  void addCachedRead(ref<Expr> e, Value *v) {
    _cachedReads = _cachedReads.replace(std::make_pair(e,v));
  }

  Value* lookupCachedRead(ref<Expr> e) const {
    const ReadCacheTy::value_type *cached = _cachedReads.lookup(e);
    return cached ? cached->second : NULL;
  }

  // LookupBasePtr cache

  void addCachedBasePtr(SymbolicObject::Kind kind, unsigned id, Value *v) {
    const SymbolicBasePtrCacheTy::key_type key(kind, id);
    _cachedBasePtrs = _cachedBasePtrs.replace(std::make_pair(key,v));
  }

  Value* lookupCachedBasePtr(SymbolicObject::Kind kind, unsigned id) const {
    const SymbolicBasePtrCacheTy::key_type key(kind, id);
    const SymbolicBasePtrCacheTy::value_type *cached = _cachedBasePtrs.lookup(key);
    return cached ? cached->second : NULL;
  }

  // InputPtr cache

  void addInputPtr(const SymbolicObject *inputObj, Value *inputPtr) {
    _inputPtrs = _inputPtrs.insert(inputObj, inputPtr);
  }

  Value* lookupInputPtr(const SymbolicObject *inputObj) {
    const InputPtrCacheTy::value_type *res = _inputPtrs.lookup(inputObj);
    return res ? res->second : NULL;
  }

  // Naming

  std::string uniquifyName(std::string name, const char *sep = "_") {
    unsigned id = 0;
    std::string unique = name;
    while (!_usedNames->insert(unique).second) {
      unique = name + sep + llvm::utostr(++id);
    }
    return unique;
  }

  std::string uniquifyNameWithId(std::string name, unsigned baseId, const char *sep = "_") {
    unsigned id = baseId;
    std::string unique = name + sep + llvm::utostr(id);
    while (!_usedNames->insert(unique).second) {
      unique = name + sep + llvm::utostr(++id);
    }
    return unique;
  }

  void resetBlockLabelPrefix(const std::string &prefix) {
    _blockLabelPrefix->assign(prefix);
    _usedNames->insert(prefix + "_branch");
    _usedNames->insert(prefix + "_branch1");
  }

  BasicBlock* newBasicBlock(llvm::LLVMContext &llvmContext, std::string label = "") {
    if (label.empty() && !_blockLabelPrefix->empty()) {
      label = uniquifyName(*_blockLabelPrefix + "_branch", "");
    } else if (!label.empty()) {
      label = uniquifyName(label, "_");
    }
    return BasicBlock::Create(llvmContext, label, _f);
  }
};

//-----------------------------------------------------------------------------
// A wrapper for instrumentation code
// Gives easy access to shared state
//-----------------------------------------------------------------------------

namespace {

// Stats: maps statistics names to counts
typedef std::map<std::string, double> StatsMapTy;
StatsMapTy STATS;

class ScheduleFragment;
class ScheduleTreeNode;

class Instrumentor
{
private:
  // The regions being instrumented
  const RegionsInfoTy& _regions;

  // Symbolic objects
  const SymbolicObjectTable& _symbolicObjects;
  const SymbolicPtrResolutionTable& _resolvedSymbolicPtrs;

  // Shortcuts
  klee::Executor*      _kleeExecutor;
  const klee::KModule& _kleeModule;
  llvm::Module&        _llvmModule;
  llvm::LLVMContext&   _llvmContext;

  const Type*const        _llvmVoidTy;
  const IntegerType*const _llvmInt8Ty;
  const IntegerType*const _llvmInt16Ty;
  const IntegerType*const _llvmInt32Ty;
  const IntegerType*const _llvmInt64Ty;
  const IntegerType*const _llvmIntPtrTy;
  const PointerType*const _llvmPtrCharTy;

  Constant*const _constZero;
  Constant*const _constZero64;

  // ICS runtime functions
  typedef std::map<const Type*, Function*> TypeBasedLoggingFnMapTy;
  TypeBasedLoggingFnMapTy _icsLookupStackVarFns;
  TypeBasedLoggingFnMapTy _icsLogStackVarFns;

  TypeBasedLoggingFnMapTy _icsSnapshotLogFns;
  TypeBasedLoggingFnMapTy _icsSnapshotLookupFns;

  Function *_icsLookupThreadLocalAddr;
  Function *_icsLookupThreadId;
  Function *_icsLookupWaitSlot;
  Function *_icsLogThreadLocalAddr;

  Function *_icsPanicNoSched;
  Function *_icsCallStackPush;
  Function *_icsCallStackPop;
  Function *_icsDoInitialSyncOp;
  Function *_icsInitialize;

  const StructType* _icsScheduleTy;
  const PointerType* _icsSchedulePtrTy;

  // ICS runtime tables
  llvm::GlobalVariable* _icsEmptyThreadScheduleComplete;
  llvm::GlobalVariable* _icsEmptyThreadSchedulePartial;

  //
  // Instrumentor state
  //

  // Arrays used by selector functions
  // Each array is given a unique ID >= 1 (so "0" represents an "invalid id")
  typedef llvm::DenseSet<const Array*> UsedArraySetTy;
  typedef llvm::DenseMap<const Array*, unsigned> UsedArrayMapTy;
  UsedArraySetTy _usedArrays;
  UsedArrayMapTy _usedInputsArrays;
  UsedArrayMapTy _usedThreadLocalsArrays;
  // N.B.: We cannot partition this by thread-id, because, for the logical-thread
  // optimization, we will need to remap the thread ids.  This mean each thread has
  // to log each local variable.  FIXME: could do better: if two SymbolicObjects
  // have the same "programValue" and "callctx", then they can be merged in this
  // Array (the only difference is the thread-id, which affects lookup, not logging)
  std::map<unsigned, UsedArrayMapTy> _usedLocalArraysBySize;

  // Calling contexts are given a unique id using the hashing scheme described
  // in the PCC paper (OOPSLA 2007).  The hash is updated at each callsite as
  // using the function:
  //    f(V,cs) = 3*V + cs
  // where:
  //    V is the prior value (the hash of the older stack frames), and
  //    f(V,cs) is applied at the callsite for "cs", and
  //    cs is a random value unique for each callsite
  //
  // The initial value of V is "0".
  //
  // callsite <--> unique (randomly selected) id
  llvm::DenseMap<const Instruction*, uint64_t> _usedCallsToId;
  llvm::DenseMap<uint64_t, const Instruction*> _usedCallsFromId;

  // calling context <--> unique (hash-computed) id
  llvm::DenseMap<const CallingContext*, uint64_t> _usedCallCtxsToId;
  llvm::DenseMap<uint64_t, const CallingContext*> _usedCallCtxsFromId;

  // calling context --> sort unique id (>=1, assigned monotonically)
  // these short ids are used in the HbSchedule thread schedule arrays
  llvm::DenseMap<const CallingContext*, unsigned> _usedCallCtxsShortId;

  // lists of used calling contexts
  typedef std::vector<const CallingContext*> UsedCallCtxListTy;
  typedef llvm::DenseSet<const Instruction*> InstSetTy;
  typedef llvm::DenseSet<const Function*> FuncSetTy;
  UsedCallCtxListTy _usedCallCtxList;
  InstSetTy _instsUsedInCallCtxs;
  FuncSetTy _fnsUsedInCallCtxs;

  // maps region.numericId => schedules
  typedef std::map<size_t, ref<ScheduleTreeNode> > ScheduleTreesMapTy;
  ScheduleTreesMapTy _scheduleTrees;

  // maps id => function
  // we guarantee that the initial selectorFn & snapshotFn for region R have id = R.numericId
  typedef std::map<size_t, Function*> FunMapByIdTy;
  FunMapByIdTy _scheduleSelectors;
  FunMapByIdTy _snapshotFns;

  // maps constarray => const global table
  // this is used to read from arrays of type Array::ConstantInitialValues
  typedef std::map<const Array*, llvm::GlobalVariable*> ConstArrayTablesMapTy;
  ConstArrayTablesMapTy _constArrayTables;

  // list of all ScheduleFragments
  typedef std::vector< ref<ScheduleFragment> > ScheduleFragmentListTy;
  ScheduleFragmentListTy _scheduleFragments;

public:
  Instrumentor(klee::Executor *kexecutor,
               const klee::KModule *kmodule,
               const RegionsInfoTy &r,
               const SymbolicObjectTable *so,
               const SymbolicPtrResolutionTable *sp)
    : _regions(r),
      _symbolicObjects(*so),
      _resolvedSymbolicPtrs(*sp),
      _kleeExecutor(kexecutor),
      _kleeModule(*kmodule),
      _llvmModule(*kmodule->module),
      _llvmContext(kmodule->module->getContext()),
      _llvmVoidTy(Type::getVoidTy(_llvmContext)),
      _llvmInt8Ty(Type::getInt8Ty(_llvmContext)),
      _llvmInt16Ty(Type::getInt16Ty(_llvmContext)),
      _llvmInt32Ty(Type::getInt32Ty(_llvmContext)),
      _llvmInt64Ty(Type::getInt64Ty(_llvmContext)),
      _llvmIntPtrTy(IntegerType::get(_llvmContext, _kleeModule.targetData->getPointerSizeInBits())),
      _llvmPtrCharTy(PointerType::getUnqual(_llvmInt8Ty)),
      _constZero(ConstantInt::get(_llvmInt16Ty, 0)),
      _constZero64(ConstantInt::get(_llvmInt64Ty, 0)),
      _icsEmptyThreadScheduleComplete(NULL),
      _icsEmptyThreadSchedulePartial(NULL)
  {
    // Load some functions we will need
    char namebuf[64];

    // intX klee_ics_lookup_stackvar_intX(int32 tid, int32 objid)
    // void klee_ics_log_stackvar_intX(int32 objid, inst64 callctxid, intX value)
    int bits[] = { 8, 16, 32, 64, 0 };
    for (int *b = bits; *b; ++b) {
      const Type *ty = IntegerType::get(_llvmContext, *b);
      // Lookup
      snprintf(namebuf, sizeof namebuf, "klee_ics_lookup_stackvar_int%d", *b);
      _icsLookupStackVarFns[ty] =
        cast<Function>(_llvmModule.getOrInsertFunction(namebuf, ty,
                                                       _llvmInt32Ty, _llvmInt32Ty, NULL));
      // Log
      snprintf(namebuf, sizeof namebuf, "klee_ics_log_stackvar_int%d", *b);
      _icsLogStackVarFns[ty] =
        cast<Function>(_llvmModule.getOrInsertFunction(namebuf, _llvmVoidTy,
                                                       _llvmInt32Ty, _llvmInt64Ty, ty, NULL));
    }

    // intX klee_ics_snapshot_lookup_intX(int32 offset)
    // void klee_ics_snapshot_log_intX(int32 offset, intX val)
    for (int *b = bits; *b; ++b) {
      const Type *ty = IntegerType::get(_llvmContext, *b);
      // Lookup
      snprintf(namebuf, sizeof namebuf, "klee_ics_snapshot_lookup_int%d", *b);
      _icsSnapshotLookupFns[ty] =
        cast<Function>(_llvmModule.getOrInsertFunction(namebuf, ty, _llvmInt32Ty, NULL));
      // Log
      snprintf(namebuf, sizeof namebuf, "klee_ics_snapshot_log_int%d", *b);
      _icsSnapshotLogFns[ty] =
        cast<Function>(_llvmModule.getOrInsertFunction(namebuf, _llvmVoidTy, _llvmInt32Ty, ty, NULL));
    }

    // int8* klee_ics_lookup_threadlocaladdr(int32 objid)
    _icsLookupThreadLocalAddr =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_lookup_threadlocaladdr",
                                                     _llvmPtrCharTy, _llvmInt32Ty, NULL));

    // int64 klee_ics_lookupthreadId(int32 logical_threadid)
    _icsLookupThreadId =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_lookupthreadid",
                                                     _llvmInt64Ty, _llvmInt32Ty, NULL));

    // int32 klee_ics_lookupwaitslot(int32 threadid)
    _icsLookupWaitSlot =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_lookupwaitslot",
                                                     _llvmInt32Ty, _llvmInt32Ty, NULL));

    // void klee_ics_log_threadlocaladdr(int32 objid, int8* addr)
    _icsLogThreadLocalAddr =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_log_threadlocaladdr",
                                                     _llvmVoidTy, _llvmInt32Ty, _llvmPtrCharTy, NULL));

    // void klee_ics_panic_nosched(int32 sid)
    _icsPanicNoSched =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_panic_nosched",
                                                     _llvmVoidTy, _llvmInt32Ty, NULL));
    _icsPanicNoSched->setDoesNotReturn();

    // void klee_ics_callstack_push(int64 cs)
    // void klee_ics_callstack_pop()
    _icsCallStackPush =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_callstack_push",
                                                     _llvmVoidTy, _llvmInt64Ty, NULL));
    _icsCallStackPop =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_callstack_pop",
                                                     _llvmVoidTy, NULL));

    // void klee_ics_do_initial_syncop()
    _icsDoInitialSyncOp =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_do_initial_syncop",
                                                     _llvmVoidTy, NULL));

    // void klee_ics_initialize()
    _icsInitialize =
      cast<Function>(_llvmModule.getOrInsertFunction("klee_ics_initialize",
                                                     _llvmVoidTy, NULL));

    // See comments near DumpScheduleFragments()
    // struct { int16, int16, int16, int16, int16* [0] }
    _icsScheduleTy =
      StructType::get(_llvmContext, _llvmInt16Ty, _llvmInt16Ty, _llvmInt16Ty, _llvmInt16Ty,
                      ArrayType::get(PointerType::getUnqual(_llvmInt16Ty), 0), NULL);
    _icsSchedulePtrTy =
      PointerType::getUnqual(_icsScheduleTy);
  }

  // Misc
  const UsedArraySetTy& getUsedArrays() const { return _usedArrays; }
  template<class IntT> ConstantInt* GetConstantInt16(IntT v) {
    assert(v <= (IntT)UINT16_MAX);
    return ConstantInt::get(_llvmInt16Ty, v);
  }

  // Enumerating objects
  unsigned LookupObjectId(const SymbolicObject *obj);
  void AddUsedObject(const SymbolicObject *obj);
  void EnumerateUsedObjects(ref<Expr> expr);
  void EnumerateUsedInputsInHbGraph(HBGraphRef hbGraph,
                                    std::deque<hbnode_id_t> *hbNodeIdQueue);

  // Building schedule trees
  ref<ScheduleTreeNode> BuildScheduleTree(const ParallelRegionInfo &r);
  void BuildScheduleTrees();
  void DumpScheduleTrees();
  void ValidateScheduleTrees();

  // Building schedule selectors
  const IntegerType* KleeWidthToLlvmType(Expr::Width width);
  ref<Expr> TransformSymbolicReadRemovingUpdates(ref<Expr> fullExpr, ref<EvalEnv> env);

  Value* BuildSymbolicReadForMem(const SymbolicObject *obj,
                                 ref<Expr> fullExprNoUpdates,
                                 ref<Expr> offsetExpr,
                                 ref<EvalEnv> env,
                                 BasicBlock **thisBlock);

  Value* BuildSymbolicReadForReg(const SymbolicObject *obj,
                                 ref<Expr> readExprNoUpdates,
                                 ref<EvalEnv> env,
                                 BasicBlock **thisBlock);

  Value* BuildSymbolicRead(ref<Expr> fullExpr,
                           ref<ReadExpr> baseExpr,
                           ref<EvalEnv> env,
                           BasicBlock **thisBlock);

  Value* BuildExprEvaluator(ref<Expr> expr,
                            ref<EvalEnv> env,
                            BasicBlock **thisBlock);

  void BuildBooleanEvaluator(ref<Expr> expr,
                             ref<EvalEnv> env,
                             BasicBlock **thisBlock,
                             BasicBlock *successBlock,
                             BasicBlock *failBlock);

  void BuildScheduleSelector(ref<ScheduleTreeNode> node,
                             ref<EvalEnv> env,
                             BasicBlock *thisBlock,
                             BasicBlock *failBlock);
  void BuildScheduleSelector(ref<ScheduleTreeNode> node, const size_t selectorId);
  void BuildScheduleSelectors();

  struct SnapshotInfo;
  void BuildSnapshotFunction(ref<ScheduleTreeNode> node, const SnapshotInfo &info);
  void BuildSnapshotFunctions();

  // Logging callstacks
  void AddUsedCallCtx(const CallingContext *callctx);
  uint64_t LookupCallCtxLongId(const CallingContext *callctx);
  unsigned LookupCallCtxShortId(const CallingContext *callctx);
  uint64_t LookupCallSiteId(const Instruction *i);
  void EnumerateUsedCallCtxs();
  bool MustLogCallStackAtInstruction(Instruction *i);
  void InstrumentCallStackAt(Instruction *i, const uint64_t id);
  void InstrumentCallStacks();

  // Logging variables
  bool HasUniqueCallingContext(const Function *f, const CallingContext *expectedCtx);
  void InstrumentLocalVars();
  void InstrumentThreadLocals();

  // Dumping global tables
  llvm::GlobalVariable* AddGlobalMustNotExist(const Type *ty, Constant *init,
                                              llvm::StringRef name,
                                              const bool readOnly = true);
  llvm::GlobalVariable* ReplaceGlobalMustExist(const Type *ty, Constant *init,
                                                 llvm::StringRef name,
                                                 const bool readOnly = true);
  llvm::GlobalVariable* AddGlobalFunctionTable(const FunMapByIdTy &map,
                                               const char *tabledesc,
                                               const char *name);
  std::string GetNameForCallingContext(const size_t ctxId);
  void DumpCallingContexts();
  std::string GetNameForRegionContext(const size_t regionId);
  void DumpRegionContexts();
  std::string GetNameForSchedule(const size_t fragId);
  std::string GetNameForThreadSchedule(const size_t fragId, const ThreadUid tuid);
  llvm::GlobalVariable* DumpThreadSchedule(const ScheduleFragment &schedfrag, const ThreadUid tuid);
  void DumpScheduleFragments();

  void DeclareGlobalLoggingTable(const UsedArrayMapTy &map,
                                 const std::string &name,
                                 const Type *elemTy);
  void DeclareStackLoggingTable(const UsedArrayMapTy &map,
                                const std::string &name,
                                const Type *elemTy);
  void DeclareLoggingTables();

  // Function replacement
  void MakeFunctionNop(const char *fnName);
  void ReplaceLibFunctions();
  void PromoteLibcFunctionsToIntrinsics();
};

//-----------------------------------------------------------------------------
// Misc
//-----------------------------------------------------------------------------

template<class T>
std::pair<unsigned, bool> AddUniqueId(llvm::DenseMap<T,unsigned> *usedMap, T elem) {
  // Returns: (id, true/false)
  // id    => allocated from the sequence 1,2,3,...
  // true  => element was inserted into usedMap
  // false => element already existed in the usedMap
  typedef llvm::DenseMap<T,unsigned> MapTy;
  std::pair<T, unsigned> entry(elem, usedMap->size()+1);
  typename std::pair<typename MapTy::iterator, bool> res = usedMap->insert(entry);
  return std::make_pair(res.first->second, res.second);
}

template<class T>
unsigned LookupUniqueId(const llvm::DenseMap<T,unsigned> &usedMap, T elem) {
  // REQUIRES: elem exists in the usedMap
  typename llvm::DenseMap<T,unsigned>::const_iterator it = usedMap.find(elem);
  assert(it != usedMap.end());
  return it->second;
}

template<class KeyT, class ValueT>
ValueT* LookupInMap(const std::map<KeyT, ValueT*> &map, const KeyT key) {
  typename std::map<KeyT, ValueT*>::const_iterator it = map.find(key);
  assert(it != map.end());
  return it->second;
}

//-----------------------------------------------------------------------------
// Enumerating objects that are used by schedules
//-----------------------------------------------------------------------------

unsigned Instrumentor::LookupObjectId(const SymbolicObject *obj) {
  // Lookup the unique id for this object
  // REQUIRES: object was added by AddUsedObject()
  switch (obj->kind) {
  case SymbolicObject::VirtualRegister:
  case SymbolicObject::FunctionArgument: {
    const Value *val = obj->getProgramValue();
    const unsigned sizeInBits = _kleeModule.targetData->getTypeSizeInBits(val->getType());
    return LookupUniqueId(_usedLocalArraysBySize[sizeInBits], obj->array);
  }

  case SymbolicObject::SymbolicInput:
    return LookupUniqueId(_usedInputsArrays, obj->array);

  case SymbolicObject::ThreadLocalVariable:
    return LookupUniqueId(_usedThreadLocalsArrays, obj->array);

  case SymbolicObject::ThreadId:
  case SymbolicObject::WaitSlot:
  case SymbolicObject::GlobalVariable:
  case SymbolicObject::SymbolicPtr:
    // Shouldn't be called for these
    assert(0 && "Unexpected type for LookupObjectId()");
    break;

  default:
    assert(0);
    break;
  }
}

void Instrumentor::AddUsedObject(const SymbolicObject *obj) {
  std::pair<unsigned, bool> res;

  // Add to the appropriate sets
  // N.B.: We separate these by type to reduce the size of the tables,
  // since at runtime we have to keep one table per type anyway.
  switch (obj->kind) {
  case SymbolicObject::VirtualRegister:
  case SymbolicObject::FunctionArgument: {
    const Value *val = obj->getProgramValue();
    const unsigned sizeInBits = _kleeModule.targetData->getTypeSizeInBits(val->getType());
    res = AddUniqueId(&_usedLocalArraysBySize[sizeInBits], obj->array);
    _usedArrays.insert(obj->array);
    break;
  }

  case SymbolicObject::SymbolicInput:
    res = AddUniqueId(&_usedInputsArrays, obj->array);
    _usedArrays.insert(obj->array);
    break;

  case SymbolicObject::ThreadLocalVariable:
    res = AddUniqueId(&_usedThreadLocalsArrays, obj->array);
    _usedArrays.insert(obj->array);
    break;

  case SymbolicObject::ThreadId:
  case SymbolicObject::WaitSlot:
  case SymbolicObject::GlobalVariable:
  case SymbolicObject::SymbolicPtr:
    // We dont need a unique id for these
    res.second = _usedArrays.insert(obj->array).second;
    res.first = 0;
    break;

  default:
    assert(0);
    break;
  }

  if (DbgInstrumentor && res.second) {
    llvm::errs() << "USING " << SymbolicObject::KindToString(obj->kind)
                 << " (id=" << res.first << "):\n";
    SymbolicObject::Dump("  ", obj);
  }
  // STATS
  if (res.second) {
    switch (obj->kind) {
#define IncStat(kind) \
    case SymbolicObject::kind: \
      STATS["UsedInputVia" #kind]++; \
      break;

    IncStat(VirtualRegister)
    IncStat(FunctionArgument)
    IncStat(GlobalVariable)
    IncStat(ThreadLocalVariable)
    IncStat(ThreadId)
    IncStat(SymbolicInput)
    IncStat(SymbolicPtr)
    IncStat(WaitSlot)
#undef IncStat
    }
  }
}

void Instrumentor::EnumerateUsedObjects(ref<Expr> expr) {
  typedef std::set<const Array*> ArraySetTy;
  ArraySetTy objects;
  klee::findSymbolicObjects(expr, objects);

  // Extract the set of symbolic arrays being read
  for (ArraySetTy::iterator a = objects.begin(); a != objects.end(); ++a) {
    const SymbolicObject *obj = _symbolicObjects.find(*a);
    if (!obj) {
      llvm::errs() << "ERROR: Object not found in the SymbolicObjectTable:\n"
                   << (*a)->name << "\n";
      assert(0);
    }

    AddUsedObject(obj);

    // For symbolic ptrs, transitively enumerate all source arrays
    if (obj->kind == SymbolicObject::SymbolicPtr) {
      ref<SymbolicPtrResolution> res = _resolvedSymbolicPtrs.lookupPrimaryByTargetObj(obj);
      EnumerateUsedObjects(res->targetAddress);
    }
  }
}

void Instrumentor::EnumerateUsedInputsInHbGraph(HBGraphRef hbGraph,
                                                std::deque<hbnode_id_t> *hbNodeIdQueue) {
  // Enumerate all HBNodes that include a used input
  for (hbnode_id_t i = 1; i <= hbGraph->getMaxNodeId(); ++i) {
    const HBNode &node = hbGraph->getNode(i);
    if (node.inputObj() && _usedArrays.count(node.inputObj()->array))
      hbNodeIdQueue->push_back(i);
  }
}

//-----------------------------------------------------------------------------
// Partitioning schedules into fragments
//-----------------------------------------------------------------------------

ref<Expr> ReverseConjunction(ref<Expr> expr) {
  // Reverse (A&B&C) to (C&B&C)
  // This is important for summaries produced by ConstraintManagers, in which
  // the oldest branches are represented by the deepest AndExprs in the summary.
  // ConstraintManager does this to maximize sharing across paths.  However, we
  // want to evaluate each schedule constraint in program order, just in case
  // an earlier constraint is necessary to avoid an error in a later constraint
  // (e.g., a div-by-0 or out-of-bounds).  Here, we invert that chain so the
  // oldest AndExprs are on the top.
  ref<Expr> res = Expr::True;
  for (ExprNaryIterator it(expr, Expr::And); !it.done(); ++it) {
    res = AndExpr::create(*it, res);
  }
  return res;
}

// A fragment of a HBGraph schedule
// This includes all HBNodes in the range [firstNodeId, lastNodeId].  For all
// fragments for which "lastNodeId < hbGraph.getMaxNodeId()", "threadLeader"
// specifies the thread that will evaluate the next schedule selector once the
// next dynamic input arrives.
//
// Corner case: in rare cases, we need to specify an "empty" schedule fragment.
// We do this by setting firstNodeId > lastNodeId so that the obvious for loop
// "for (i = first; i <= last; ++i)" will "just work".
//
// INVARIANTS: The runtime assumes that nextSelectorId == 0 iff this fragment
// ends the current region's schedule (i.e., if the current fragment does not
// read dynamic input that can affect the schedule).
struct ScheduleFragment {
public:
  HBGraphRef hbGraph;
  hbnode_id_t firstNodeId;
  hbnode_id_t lastNodeId;
  ThreadUid nextThreadLeader;
  bool exitsProgram;

  size_t schedId;          // id of the parent schedule (for debugging)
  size_t fragId;           // id of this schedule fragment
  size_t nextSelectorId;   // id of the selector function to select the next fragment

  // Unique ids for each input read by this fragment (unique ids start at 1).
  // N.B.: These ids differ from LookupObjectId(inputObj)!  The later ids are
  // not used.  The ids below are used for snapshotting inputs.
  typedef std::map<const SymbolicObject*, size_t> InputIdsMapTy;
  InputIdsMapTy  inputIds;

public:
  ScheduleFragment() : schedId(0), fragId(0), nextSelectorId(0), refCount(0) {}

  void dumpTxt(std::ostream &os) const {
    os << "Id: schedId=" << schedId << " fragId=" << fragId << "\n"
       << "Range: [" << firstNodeId << "," << lastNodeId << "]\n"
       << "Graph:\n";
    hbGraph->dumpTxt(os);
  }

private:
  friend class ref<ScheduleFragment>;
  unsigned refCount;
};

bool operator==(const ScheduleFragment &left, const ScheduleFragment &right) {
  if (left.exitsProgram != right.exitsProgram)
    return false;
  if (left.nextThreadLeader != right.nextThreadLeader)
    return false;
  hbnode_id_t ldiff = left.lastNodeId - left.firstNodeId;
  hbnode_id_t rdiff = right.lastNodeId - right.firstNodeId;
  if (ldiff != rdiff)
    return false;
  // Empty range?
  if (left.lastNodeId < left.firstNodeId)
    return true;
  return HBGraph::equalRange(*left.hbGraph, left.firstNodeId,
                             *right.hbGraph, right.firstNodeId, ldiff+1);
}

bool operator!=(const ScheduleFragment &left, const ScheduleFragment &right) {
  return !(left == right);
}

// Each region has a "schedule tree" (or "trie") which defines the set of
// input-covering schedules for that region.  The root node is evaluated at
// region entry, and other nodes may be required to evaluate dynamic inputs.
// Node evaluation (at runtime) works as follows:
//
//   1. Select an edge \in node s.t. the current input matches edge->getSummary()
//   2. If edge->schedfrag!=NULL, then use that schedule, and advance the "current"
//      node to edge->next
//   3. Otherwise, edge contains a partial constraint, so we evaluate edge->next
//
// Partial constraints are used to improve sharing in the schedule selectors.
// For example, given two input constraints (A&B&C) and (A&B&D), the terms (A&B)
// are shared and should be evaluated just once.  We do this by creating an edge
// that has terms (A&B) and points at a next node which contains edges for C and D.
//
struct ScheduleTreeNode {
public:
  struct Edge {
    std::vector< ref<Expr> > terms;
    ref<ScheduleFragment>    schedfrag;
    ref<ScheduleTreeNode>    next;

    // This edge's summary constraint is the conjunction of "terms".
    // N.B.: The returned conjunction maintains the same order as "terms".
    ref<Expr> getSummary() const {
      ref<Expr> cond = Expr::True;
      for (size_t k = terms.size(); k != 0; --k)
        cond = AndExpr::create(terms[k-1], cond);
      return cond;
    }
  };

  typedef std::vector<Edge> EdgeListTy;
  EdgeListTy edges;

  // For snapshots, this maps each offset of the snapshot buffer to the
  // single-byte ReadExpr that it snapsnots (N.B.: index is ref<ReadExpr>)
  ImmutableExprMap<unsigned> snapshotOffsets;

public:
  ScheduleTreeNode() : refCount(0) {}

  ref<ScheduleTreeNode> addEdge(std::vector< ref<Expr> > &terms, ScheduleFragment schedfrag) {
    // Look for an existing edge that matches "cond"
    Edge *found = NULL;
    size_t shared = 0;
    for (EdgeListTy::iterator edge = edges.begin(); edge != edges.end(); ++edge) {
      if (edge->terms[0] != terms[0])
        continue;
      found = &*edge;
      for (shared = 1; shared < edge->terms.size(); ++shared) {
        if (edge->terms[shared] != terms[shared])
          break;
      }
      break;
    }

    // XXX: Can we do this better?
    // More expensive match in case "terms" is ordered differently than edge.terms
    if (!found && DbgInstrumentor) {
      for (EdgeListTy::iterator edge = edges.begin(); edge != edges.end(); ++edge) {
        std::vector< ref<Expr> >::iterator it;
        shared = 0;
        while (shared < edge->terms.size()) {
          it = std::find(terms.begin()+shared, terms.end(), edge->terms[shared]);
          if (it == terms.end())
            break;
          found = &*edge;
          std::copy_backward(terms.begin()+shared, it, it+1);
          terms[shared] = edge->terms[shared];
          shared++;
        }
        if (found) {
          std::cerr << "SORTING TERMS WOULD ADD A REDUNDANCY!\n";
          found = NULL;
          break;
        }
      }
    }

    if (found) {
      assert(found->next.get());

      // Same size: should have the same schedule fragment
      if (shared == terms.size() && shared == found->terms.size() && found->schedfrag.get()) {
        STATS["ScheduleFragmentsRedundant"]++;
        if (schedfrag != *found->schedfrag) {
          std::cerr << "==== schedfrag ===\n";
          schedfrag.dumpTxt(std::cerr);
          std::cerr << "==== foundfrag ===\n";
          found->schedfrag->dumpTxt(std::cerr);
          assert(0);
        }
        return found->next;
      }

      // Remove leftovers from "terms" (empty => true)
      terms.erase(terms.begin(), terms.begin()+shared);
      if (terms.empty())
        terms.push_back(Expr::True);

      // If it matches "*found" exactly and "*found" does not have a fragment,
      // recurse on found->next with the remaining terms
      if (shared == found->terms.size() && !found->schedfrag.get())
        return found->next->addEdge(terms, schedfrag);

      STATS["ScheduleFragmentsPartiallySharedTerms"]++;

      // Put leftovers from "*found" into "foundExtra" (empty => true)
      std::vector< ref<Expr> > foundExtra(found->terms.begin()+shared, found->terms.end());
      if (foundExtra.empty())
        foundExtra.push_back(Expr::True);

      // Create a new node for the leftovers
      ref<ScheduleTreeNode> next = new ScheduleTreeNode;
      next->edges.resize(2);
      next->edges[0].terms = terms;
      next->edges[0].schedfrag = new ScheduleFragment(schedfrag);
      next->edges[0].next = new ScheduleTreeNode;
      next->edges[1].terms = foundExtra;
      next->edges[1].schedfrag = found->schedfrag;
      next->edges[1].next = found->next;

      // Reset "found" to the "shared" prefix
      found->terms.erase(found->terms.begin()+shared, found->terms.end());
      found->schedfrag = NULL;
      found->next = next;

      // Result is the "next" node for "terms"
      return next->edges[0].next;
    }

    if (terms.size() == 1 && isa<ConstantExpr>(terms[0])) {
      STATS["ScheduleFragmentsConstantTerms"]++;
    } else {
      STATS["ScheduleFragmentsUnsharedSharedTerms"]++;
    }

    // No match: make a new edge
    edges.resize(edges.size() + 1);
    Edge &edge = edges.back();
    edge.terms = terms;
    edge.schedfrag = new ScheduleFragment(schedfrag);
    edge.next = new ScheduleTreeNode;
    return edge.next;
  }

  ref<ScheduleTreeNode> addEdge(ref<Expr> cond, ScheduleFragment schedfrag) {
    // Expand to terms
    std::vector< ref<Expr> > terms;
    for (ExprNaryIterator it(cond, Expr::And); it != ExprNaryIterator(); ++it)
      terms.push_back(*it);

    // Ensure this is setup
    // ASSUMES: the lastNode contains a used input (see algorithm in BuildScheduleTree)
    // HOWEVER: the schedule can be empty!
    if (schedfrag.hbGraph->getMaxNodeId() != 0) {
      schedfrag.nextThreadLeader = schedfrag.hbGraph->getNode(schedfrag.lastNodeId).tuid();
    }

    // Must be set by caller
    assert(schedfrag.schedId);

    return addEdge(terms, schedfrag);
  }

private:
  friend class ref<ScheduleTreeNode>;
  unsigned refCount;
};

//
// Visitor framework
// This performs a DFS (edges at the root node are depth=0)
//

struct ScheduleTreeNodeVisitor {
  virtual void visitNode(ScheduleTreeNode &node, size_t depth) {}
  virtual void visitNodePost(ScheduleTreeNode &node, size_t depth) {}
  virtual void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {}
  virtual void visitEdgePost(ScheduleTreeNode::Edge &edge, size_t depth) {}
};

void TraverseScheduleTree(ref<ScheduleTreeNode> node, ScheduleTreeNodeVisitor &v, size_t depth) {
  v.visitNode(*node, depth);
  for (size_t k = 0; k < node->edges.size(); ++k) {
    ScheduleTreeNode::Edge &edge = node->edges[k];
    v.visitEdge(edge, depth);
    if (edge.next.get())
      TraverseScheduleTree(edge.next, v, depth+1);
    v.visitEdgePost(edge, depth);
  }
  v.visitNodePost(*node, depth);
}

void TraverseScheduleTree(ref<ScheduleTreeNode> root, ScheduleTreeNodeVisitor &v) {
  TraverseScheduleTree(root.get(), v, 0);
}

//
// Building schedule trees
//

ref<ScheduleTreeNode> Instrumentor::BuildScheduleTree(const ParallelRegionInfo &r) {
  ref<ScheduleTreeNode> root = new ScheduleTreeNode;

  const PathManager::ScheduleListTy &schedules = r.paths.getSchedules();
  for (SchedulesIterator sched = schedules.begin(); sched != schedules.end(); ++sched) {
    // We will partition the HBGraph into fragments
    ScheduleFragment schedfrag;
    schedfrag.hbGraph = (*sched)->hbGraph;
    schedfrag.firstNodeId = 1;
    schedfrag.lastNodeId = 0;
    schedfrag.schedId = (*sched)->id;
    schedfrag.exitsProgram = false;  // only the final fragment might exit

    // We start from the root of the ScheduleTree
    ref<ScheduleTreeNode> node = root;
    klee::ConstraintManager cm;

    // We partition the path constraints via the the input HBNodes.
    // Each partition becomes its own ScheduleFragment.
    std::deque<hbnode_id_t> inputHbNodeIds;
    EnumerateUsedInputsInHbGraph((*sched)->hbGraph, &inputHbNodeIds);

    // Simple algorithm
    // TODO: Here's where we could do better and allow other threads
    // (other than the one reading the input) to run ahead for as long
    // as the conditions don't depend on this input.
    for (PathConstraint::ListTy::const_iterator it = (*sched)->inputConstraint.list.begin();
         it != (*sched)->inputConstraint.list.end(); ++it) {
      // Include input constraints only
      if (it->fc != klee::KLEE_FORK_INVARIANT_INPUT)
        continue;
      // If there is a sequence of inputs read by the same thread that all
      // happen-before the current (*it), then merge all those inputs into
      // the same schedfrag
      hbnode_id_t lastInputNodeId = 0;
      ThreadUid lastInputTuid;
      while (!inputHbNodeIds.empty() && inputHbNodeIds.front() <= it->currHbNodeId) {
        const HBNode &node = (*sched)->hbGraph->getNode(inputHbNodeIds.front());
        if (lastInputNodeId && node.tuid() != lastInputTuid)
          break;
        lastInputNodeId = inputHbNodeIds.front();
        lastInputTuid = node.tuid();
        inputHbNodeIds.pop_front();
      }
      if (lastInputNodeId != 0) {
        schedfrag.firstNodeId = schedfrag.lastNodeId + 1;
        schedfrag.lastNodeId = lastInputNodeId;
        node = node->addEdge(cm.getSummaryExpr(), schedfrag);
        cm.clear();
      }
      // Add this input constraint to the current schedfrag
      cm.addConstraint(it->expr);
    }

    // Final fragment
    schedfrag.firstNodeId = schedfrag.lastNodeId + 1;
    schedfrag.lastNodeId = schedfrag.hbGraph->getMaxNodeId();
    schedfrag.exitsProgram = (*sched)->exitsProgram;
    node->addEdge(ReverseConjunction(cm.getSummaryExpr()), schedfrag);
  }

  return root;
}

void Instrumentor::BuildScheduleTrees() {
  assert(!_regions.empty());
  assert(_scheduleTrees.empty());

  // Build a schedule tree for each region
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    _scheduleTrees[r->second->numericId] = BuildScheduleTree(*r->second);
  }

#if 0
  // Fixup Pass
  // XXX: Move "true" to the back so we don't block-out other constraints
  // XXX: I think this has something to do with inputs (\eg, an input arrives
  // XXX: as the FIRST thing and we try to do something with it?)
  struct FixupVisitor : public ScheduleTreeNodeVisitor {
    void visitNode(ScheduleTreeNode &node, size_t depth) {
      if (node.edges.empty())
        return;
      for (size_t k = 0; k < node.edges.size() - 1; ++k) {
        if (node.edges[k].terms.size() == 1 &&
            node.edges[k].terms[0]->isTrue()) {
          std::swap(node.edges.back(), node.edges[k]);
          std::cerr << "MOVED 'TRUE' TO THE BACK OF TERMS\n";
          break;
        }
      }
    }
  };

  {
    FixupVisitor v;
    for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
      TraverseScheduleTree(_scheduleTrees[r->second->numericId].get(), v);
    }
  }
#endif

  // DEBUG
  DumpScheduleTrees();

  // Number all of the schedule fragments
  // There are two special numbering constraints:
  //   * The initial fragment for region #1 must have fragId=1 (others do not matter)
  //   * The initial selector function for region "r" has a selectorId equal to "r.numericId"
  struct NumberingVisitor : public ScheduleTreeNodeVisitor {
    const UsedArraySetTy *usedArrays;
    ScheduleFragmentListTy *scheduleFragmentsList;
    size_t nextFragId;
    size_t nextSelectorId;
    size_t nextInputId;

    virtual void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
      // Clear next if empty
      if (edge.next->edges.empty()) {
        edge.next = NULL;
      }
      // Assign id
      ref<ScheduleFragment> schedfrag = edge.schedfrag;
      if (schedfrag.get()) {
        assert(!schedfrag->fragId);
        assert(!schedfrag->nextSelectorId);
        schedfrag->fragId = nextFragId++;
        scheduleFragmentsList->push_back(schedfrag);
        if (edge.next.get()) {
          schedfrag->nextSelectorId = nextSelectorId++;
        }
        for (hbnode_id_t k = schedfrag->firstNodeId; k <= schedfrag->lastNodeId; ++k) {
          const SymbolicObject *inputObj = schedfrag->hbGraph->getNode(k).inputObj();
          if (inputObj && usedArrays->count(inputObj->array))
            schedfrag->inputIds[inputObj] = nextInputId++;
        }
      }
    }
  };

  {
    NumberingVisitor v;
    v.usedArrays = &_usedArrays;
    v.scheduleFragmentsList = &_scheduleFragments;
    v.nextFragId     = 1;
    v.nextSelectorId = _regions.size()+1;   // "1..LastRegionId" are reserved
    v.nextInputId    = _regions.size()+1;   // "1..LastRegionId" are reserved
    // Do the first region first
    // This ensures the initial schedule fragment gets fragId=1
    assert(_scheduleTrees[1].get());
    TraverseScheduleTree(_scheduleTrees[1].get(), v);
    assert(_scheduleTrees[1]->edges.size() == 1);
    assert(_scheduleTrees[1]->edges[0].schedfrag.get());
    assert(_scheduleTrees[1]->edges[0].schedfrag->fragId == 1);
    // Now do other fragments
    for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
      if (r->second->numericId != 1)
        TraverseScheduleTree(_scheduleTrees[r->second->numericId].get(), v);
    }
  }

  // Compute some stats
  struct StatsVisitor : public ScheduleTreeNodeVisitor {
    struct ScopedStat {
      size_t maxValue;
      size_t currValue;
      std::stack<size_t> saved;
      double totalAccum;
      double totalCount;

      ScopedStat() : maxValue(0), currValue(0), totalAccum(0), totalCount(0) {}

      void pushAndInc(size_t incBy) {
        saved.push(currValue);
        currValue += incBy;
        maxValue = std::max(currValue, maxValue);
      }
      void pop() {
        currValue = saved.top();
        saved.pop();
      }
      void finalizeForAvg() {
        totalAccum += currValue;
        totalCount++;
        currValue = 0;
      }
    };

    ScopedStat fragmentListSize;
    ScopedStat decisionTreeSize;
    size_t maxDepth;
    double totalDepth;  // for computing avgDepth
    double numEdges;    // for computing avgDepth

    virtual void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
      maxDepth = std::max(depth, maxDepth);
      totalDepth += depth;
      numEdges++;
      decisionTreeSize.pushAndInc(sizeOfTerms(edge));
      if (edge.schedfrag.get()) {
        fragmentListSize.pushAndInc(1);
        decisionTreeSize.finalizeForAvg();
        if (edge.schedfrag->firstNodeId > edge.schedfrag->lastNodeId) {
          STATS["ScheduleFragmentsEmpty"]++;
          if (edge.schedfrag->lastNodeId != edge.schedfrag->hbGraph->getMaxNodeId())
            STATS["ScheduleFragmentsEmptyInMiddle"]++;
        }
      } else {
        fragmentListSize.pushAndInc(0);
      }
    }

    virtual void visitEdgePost(ScheduleTreeNode::Edge &edge, size_t depth) {
      fragmentListSize.pop();
      decisionTreeSize.pop();
    }

    static size_t sizeOfTerm(ref<Expr> e) {
      if (BinaryExpr *b = dyn_cast<BinaryExpr>(e)) {
        return 1 + sizeOfTerm(b->left) + sizeOfTerm(b->right);
      } else if (NotExpr *n = dyn_cast<NotExpr>(e)) {
        return 1 + sizeOfTerm(n->expr);
      } else {
        return 0;  // just count the operators
      }
    }

    static size_t sizeOfTerms(ScheduleTreeNode::Edge &edge) {
      size_t sz = edge.terms.size()-1;  // N-1 implicit && operators
      for (size_t k = 0; k < edge.terms.size(); ++k)
        sz += sizeOfTerm(edge.terms[k]);
      return sz;
    }
  };

  {
    StatsVisitor v;
    v.maxDepth = 0;
    v.totalDepth = 0;
    v.numEdges = 0;
    for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
      TraverseScheduleTree(_scheduleTrees[r->second->numericId].get(), v);
    }

    STATS["ScheduleTreeMaxFragmentListSize"] = v.fragmentListSize.maxValue;
    STATS["ScheduleTreeMaxDepth"] = v.maxDepth;
    STATS["ScheduleTreeAvgDepth"] = v.totalDepth / v.numEdges;

    STATS["ScheduleTreeConditionsMaxSize"] = v.decisionTreeSize.maxValue;
    STATS["ScheduleTreeConditionsAvgSize"] =
      v.decisionTreeSize.totalAccum / v.decisionTreeSize.totalCount;

    // FIXME: Emit the __ics_curr_fragment_list[] table via the Instrumentor
    // so this constant is not hardcoded (I tried to emit that table, but had
    // trouble getting it to work ... might have run into an LLVM bug?)
    // TODO: I think the problem is that I declared the variable as
    //   extern const HBSchedule** __ics_curr_fragment_list
    // rather than
    //   extern const HBSchedule* __ics_curr_fragment_list[]
    assert(v.fragmentListSize.maxValue < 32);
  }
}

void Instrumentor::DumpScheduleTrees() {
  std::auto_ptr<std::ostream>
    os(_kleeExecutor->getHandler().openOutputFile("finalScheduleTrees.txt"));

  // === Region #x ===
  // cond:#x
  // cond:#x schedId:#x fragId:#x selectorId:#x range:full inputs:{..}
  // cond:#x schedId:#x fragId:#x selectorId:#x range:[first,last] inputs:{..}

  // === Cond #x ===
  // <summary>

  // Sort regions by id
  typedef std::map<size_t, ParallelRegionId> SortedRegionIds;
  SortedRegionIds sorted;
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    sorted[r->second->numericId] = r->first;
  }

  // Edge printer
  struct PrintingVisitor : public ScheduleTreeNodeVisitor {
    std::vector<const ScheduleTreeNode::Edge*> edgesWithConds;
    std::ostream *os;

    virtual void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
      const std::string prefix = std::string("   ") * depth;
      (*os) << prefix << &edge;
      if (edge.terms.size() == 1 && isa<ConstantExpr>(edge.terms[0])) {
        (*os) << " cond:" << edge.terms[0];
      } else {
        edgesWithConds.push_back(&edge);
        (*os) << " cond:#" << edgesWithConds.size();
      }
      if (edge.schedfrag.get()) {
        (*os) << " schedId:" << edge.schedfrag->schedId;
        (*os) << " fragId:" << edge.schedfrag->fragId;
        (*os) << " nextSelectorId:" << edge.schedfrag->nextSelectorId;
        if (edge.schedfrag->firstNodeId == 1 &&
            edge.schedfrag->lastNodeId == edge.schedfrag->hbGraph->getMaxNodeId()) {
          (*os) << " range:full";
        } else {
          (*os) << " range:[" << edge.schedfrag->firstNodeId
                << "," << edge.schedfrag->lastNodeId << "]";
        }
        (*os) << " inputs:{";
        const char *sep = "";
        for (ScheduleFragment::InputIdsMapTy::const_iterator
             it = edge.schedfrag->inputIds.begin(); it != edge.schedfrag->inputIds.end(); ++it) {
          (*os) << sep << it->second << "(" << it->first->array->name << ")";
          sep = ",";
        }
        (*os) << "}";
      }
      (*os) << "\n";
      if (edge.next.get() && edge.next->edges.empty()) {
        (*os) << prefix << "   <noedges>\n";
      }
    }
  };

  // Print the tree for each region
  PrintingVisitor v;
  v.os = os.get();
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    (*os) << "======= Parallel Region #" << r->second->numericId << " =======\n";
    TraverseScheduleTree(_scheduleTrees[r->second->numericId], v);
    (*os) << "\n";
  }

  // Print each condition
  for (size_t k = 0; k < v.edgesWithConds.size(); ++k) {
    (*os) << "======= Cond #" << (k+1) << " =======\n";
    (*os) << v.edgesWithConds[k]->getSummary() << "\n\n";
  }
}

void Instrumentor::ValidateScheduleTrees() {
  // Some sanity checks
  struct CheckerVisitor : public ScheduleTreeNodeVisitor {
    virtual void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
      // Do the child edges come after an input?
      if (!edge.schedfrag.isNull()) {
        // There should be a next schedule selector iff this fragment has a next node
        assert(edge.next.isNull() == (edge.schedfrag->nextSelectorId == 0));
        // If this fragment exits the program, there should be no child nodes
        if (edge.schedfrag->exitsProgram)
          assert(!edge.next.get());
        // If this fragment completes the HBGraph, there should be no child nodes
        // DumpThreadSchedule() expects this invariant
        if (edge.schedfrag->lastNodeId == edge.schedfrag->hbGraph->getMaxNodeId())
          assert(!edge.next.get());
      }
    }
  };

  CheckerVisitor v;
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    TraverseScheduleTree(_scheduleTrees[r->second->numericId].get(), v);
  }
}

//-----------------------------------------------------------------------------
// Building schedule-selection functions
//-----------------------------------------------------------------------------

const IntegerType* Instrumentor::KleeWidthToLlvmType(Expr::Width width) {
  return IntegerType::get(_llvmContext, width);
}

Instruction::CastOps KleeToLlvmCastOp(Expr::Kind kind) {
#define KLEE_TO_LLVM(k) case Expr::k: return Instruction::k
  switch (kind) {
  KLEE_TO_LLVM(ZExt);
  KLEE_TO_LLVM(SExt);
  default: assert(0);
  }
#undef KLEE_TO_LLVM
}

Instruction::BinaryOps KleeToLlvmBinaryOp(Expr::Kind kind) {
#define KLEE_TO_LLVM(k) case Expr::k: return Instruction::k
  switch (kind) {
  KLEE_TO_LLVM(Add);
  KLEE_TO_LLVM(Sub);
  KLEE_TO_LLVM(Mul);
  KLEE_TO_LLVM(UDiv);
  KLEE_TO_LLVM(SDiv);
  KLEE_TO_LLVM(URem);
  KLEE_TO_LLVM(SRem);
  KLEE_TO_LLVM(Shl);
  KLEE_TO_LLVM(LShr);
  KLEE_TO_LLVM(AShr);
  KLEE_TO_LLVM(And);
  KLEE_TO_LLVM(Or);
  KLEE_TO_LLVM(Xor);
  default: assert(0);
  }
#undef KLEE_TO_LLVM
}

llvm::CmpInst::Predicate KleeToLlvmCmpOp(Expr::Kind kind) {
#define KLEE_TO_LLVM(k,c) case Expr::k: return llvm::CmpInst::ICMP_##c
  switch (kind) {
  KLEE_TO_LLVM(Eq,  EQ);
  KLEE_TO_LLVM(Ne,  NE);
  KLEE_TO_LLVM(Ult, ULT);
  KLEE_TO_LLVM(Ule, ULE);
  KLEE_TO_LLVM(Ugt, UGT);
  KLEE_TO_LLVM(Uge, UGE);
  KLEE_TO_LLVM(Slt, SLT);
  KLEE_TO_LLVM(Sle, SLE);
  KLEE_TO_LLVM(Sgt, SGT);
  KLEE_TO_LLVM(Sge, SGE);
  default: assert(0);
  }
#undef KLEE_TO_LLVM
}

ref<Expr>
Instrumentor::TransformSymbolicReadRemovingUpdates(ref<Expr> fullExpr, ref<EvalEnv> env) {
  // Recurse on concats
  if (ConcatExpr *e = dyn_cast<ConcatExpr>(fullExpr)) {
    ref<Expr> left = TransformSymbolicReadRemovingUpdates(e->left, env);
    ref<Expr> right = TransformSymbolicReadRemovingUpdates(e->right, env);
    return ConcatExpr::create(left, right);
  }

  // Now we should have a one-byte read with non-empty updates
  ReadExpr *readExpr = cast<ReadExpr>(fullExpr);
  assert(readExpr->updates.root);
  assert(readExpr->updates.head);

  // Go through the update list one-by-one and build a chain of SelectExprs
  // which select the update that matches the index.  We buildup the chain
  // as a stack on which we push a sequence:
  //   cond0 ifTrue0 cond1 ifTrue1 ...
  // This sequence translates to the expression:
  //   (SelectExpr c0 it0 (SelectExpr c1 it1 (... (SelectExpr cN itN expr))))
  std::stack<ref<Expr> > stack;
  for (const UpdateNode *n = readExpr->updates.head; n; /* nop */) {
    ref<Expr> cond = n->getGuard();
    ref<Expr> value = n->getValueAtIndex(readExpr->index);

    // Coalescing optimization
    if (n->isMultiByte()) {
      assert(cond == n->getGuardForIndex(readExpr->index));
      n = n->next;
    } else {
      assert(value == n->getByteValue());

      // Single-byte
      const UpdateNode *first = n;
      ref<Expr> baseIndex = n->getByteIndex();
      ref<Expr> currIndex = baseIndex;
      ref<Expr> priorIndex = baseIndex;
      int currOffset = 0;
      int minOffset = 0;
      int maxOffset = 0;

      // Optimization: collapse ranges of indices into a single condition.
      // We can do this if all indices are adjacent and if all updates have
      // exactly the same value and guard.
      for (n = n->next; n; priorIndex = currIndex, n = n->next) {
        if (n->getByteValue() != value || n->getGuard() != cond)
          break;
        currIndex = n->getByteIndex();
        if (currIndex == AddExpr::create(priorIndex, Expr::One32)) {
          currOffset++;
        } else if (currIndex == SubExpr::create(priorIndex, Expr::One32)) {
          currOffset--;
        } else {
          break;
        }
        minOffset = std::min(currOffset, minOffset);
        maxOffset = std::max(currOffset, maxOffset);
      }

      if (minOffset == maxOffset) {
        STATS["UpdatesNotCoalesced"]++;
        cond = AndExpr::create(cond,
                               EqExpr::create(baseIndex, readExpr->index));
        assert(minOffset == 0);
        assert(cond == first->getGuardForIndex(readExpr->index));
      } else {
        STATS["UpdatesCoalesced"] += maxOffset - minOffset;
        ref<Expr> minIndex = AddExpr::create(baseIndex, ConstantExpr::alloc(minOffset, Expr::Int32));
        ref<Expr> maxIndex = AddExpr::create(baseIndex, ConstantExpr::alloc(maxOffset, Expr::Int32));
        cond = AndExpr::create(cond,
               AndExpr::create(UleExpr::create(minIndex, readExpr->index),
                               UleExpr::create(readExpr->index, maxIndex)));
        if (DbgInstrumentor) {
          llvm::errs() << "Symbolic Updates: Condensed index range to "
                       << "[" << minIndex << " , " << maxIndex << "]\n";
        }
      }
    }

    // Feasible?
    Solver::Validity cv = Solver::Unknown;
    if (OptMinimizeUpdatesOnRead) {
      cv = env->checkValidity(cond);
      if (cv == Solver::False) {
        STATS["UpdatesInfeasible"]++;
        if (DbgInstrumentor) {
          llvm::errs() << "Symbolic Updates: Skipping infeasible update, cond is:\n"
                       << cond << "\n";
        }
        continue;
      }
      STATS["UpdatesFeasible"]++;
    }

    stack.push(cond);
    stack.push(value);

    // Stop early if the read's index must-mach the current update
    if (cv == Solver::True || cond->isTrue())
      break;
  }

  // Convert the above stack to a chain of SelectExprs
  // The base case reads from the root array directly
  ref<Expr> final = readExpr->updates.root->getValueAtIndex(readExpr->index);
  while (!stack.empty()) {
    ref<Expr> ifTrue = stack.top();
    stack.pop();
    assert(!stack.empty());
    ref<Expr> cond = stack.top();
    stack.pop();

    final = SelectExpr::create(cond, ifTrue, final);
  }

  return final;
}

Value* Instrumentor::BuildSymbolicReadForMem(const SymbolicObject *obj,
                                             ref<Expr> fullExprNoUpdates,
                                             ref<Expr> offsetExpr,
                                             ref<EvalEnv> env,
                                             BasicBlock **thisBlock) {
  // Get the base pointer of the memory access
  Value *basePtr;
  const Type *const basePtrTy = _llvmPtrCharTy;

  switch (obj->kind) {
  case SymbolicObject::GlobalVariable:
    basePtr = const_cast<Value*>(obj->ptr);
    break;

  case SymbolicObject::SymbolicInput:
    basePtr = env->lookupInputPtr(obj);
    if (!basePtr) {
      llvm::errs() << "ERROR: Couldn't find basePtr for SymbolicInput:\n"
                   << fullExprNoUpdates << "\n";
      SymbolicObject::Dump("  ", obj);
      assert(0);
    }
    break;

  case SymbolicObject::ThreadLocalVariable:
    {
      unsigned id = LookupObjectId(obj);
      basePtr = env->lookupCachedBasePtr(obj->kind, id);
      if (!basePtr) {
        std::string vname = env->uniquifyName(obj->array->name + ".ptr");
        Value *args[] = { ConstantInt::get(_llvmInt32Ty, id) };
        basePtr = CallInst::Create(_icsLookupThreadLocalAddr, args, args+1, vname, *thisBlock);
        env->addCachedBasePtr(obj->kind, id, basePtr);
      }
    }
    break;

  case SymbolicObject::SymbolicPtr:
    {
      ref<SymbolicPtrResolution> res = _resolvedSymbolicPtrs.lookupPrimaryByTargetObj(obj);
      assert(res.get());
      assert(res->targetAddress.get());
      basePtr = BuildExprEvaluator(res->targetAddress, env, thisBlock);
      basePtr = new llvm::IntToPtrInst(basePtr, basePtrTy, "", *thisBlock);
    }
    break;

  default:
    assert(0);
    break;
  }

  // Ensure the basePtr has type int8*
  // This should be true for pointers accessed via _icsLookupBasePtr
  if (basePtr->getType() != basePtrTy) {
    assert(obj->kind != SymbolicObject::SymbolicInput);
    assert(obj->kind != SymbolicObject::ThreadLocalVariable);
    basePtr = new llvm::BitCastInst(basePtr, basePtrTy, "", *thisBlock);
  }

  // ptr = (intX*) (base + offset)
  Value *offset = BuildExprEvaluator(offsetExpr, env, thisBlock);
  Value *ptr = llvm::GetElementPtrInst::Create(basePtr, offset, "", *thisBlock);
  const Type *finalTy =
    PointerType::getUnqual(IntegerType::get(_llvmContext, fullExprNoUpdates->getWidth()));
  ptr = new llvm::BitCastInst(ptr, finalTy, "", *thisBlock);

  // *ptr
  std::string vname;
  if (offsetExpr->isZero()) {
    vname = env->uniquifyName(obj->array->name);
  } else {
    vname = env->uniquifyName(obj->array->name + ".unknown_field");
  }
  return new llvm::LoadInst(ptr, vname, *thisBlock);
}

Value* Instrumentor::BuildSymbolicReadForReg(const SymbolicObject *obj,
                                             ref<Expr> readExprNoUpdates,
                                             ref<EvalEnv> env,
                                             BasicBlock **thisBlock) {
  assert(obj->kind == SymbolicObject::VirtualRegister ||
         obj->kind == SymbolicObject::FunctionArgument ||
         obj->kind == SymbolicObject::WaitSlot ||
         obj->kind == SymbolicObject::ThreadId);

  // Lookup the untruncated (aka "raw") value for the reg
  // N.B.: "Raw" value can be larger than the requested value if there is a truncation
  Value *raw = NULL;
  ref<Expr> rawReadExprNoUpdates = _symbolicObjects.lookupExprForObj(obj);

  // We convert the object type to an integer since the lookup functions
  // always return integer types
  const Type *rawTy = obj->getObjectType();
  const unsigned rawSizeInBits = _kleeModule.targetData->getTypeSizeInBits(rawTy);
  if (!rawTy->isIntegerTy()) {
    assert(rawTy->isFloatTy() || rawTy->isDoubleTy() || rawTy->isPointerTy());
    rawTy = IntegerType::get(_llvmContext, rawSizeInBits);
  }

  // If we're doing a truncation, the raw value might already be cached
  if (rawSizeInBits > readExprNoUpdates->getWidth()) {
    raw = env->lookupCachedRead(rawReadExprNoUpdates);
  }
  // Otherwise: call klee_ics_lookup*(id)
  if (!raw) {
    switch (obj->kind) {
    case SymbolicObject::ThreadId: {
      Value *arg = ConstantInt::get(_llvmInt32Ty, obj->tuid.tid);
      raw = CallInst::Create(_icsLookupThreadId, arg, obj->array->name, *thisBlock);
      break;
    }
    case SymbolicObject::WaitSlot: {
      Value *arg = ConstantInt::get(_llvmInt32Ty, obj->tuid.tid);
      raw = CallInst::Create(_icsLookupWaitSlot, arg, obj->array->name, *thisBlock);
      break;
    }
    default: {
      Function *lookupFn = LookupInMap(_icsLookupStackVarFns, rawTy);
      Value *args[] = { ConstantInt::get(_llvmInt32Ty, obj->tuid.tid),
                        ConstantInt::get(_llvmInt32Ty, LookupObjectId(obj)) };
      raw = CallInst::Create(lookupFn, args, args+2, obj->array->name, *thisBlock);
      break;
    }
    }
    env->addCachedRead(rawReadExprNoUpdates, raw);
  }

  // Got the "raw" value for the reg
  // Now truncate if needed
  if (rawSizeInBits > readExprNoUpdates->getWidth()) {
    const Type *finalTy = KleeWidthToLlvmType(readExprNoUpdates->getWidth());
    std::string vname = env->uniquifyName(obj->array->name) + "_trunc";
    return new llvm::TruncInst(raw, finalTy, vname, *thisBlock);
  } else {
    return raw;
  }
}

Value* Instrumentor::BuildSymbolicRead(ref<Expr> fullExpr,
                                       ref<ReadExpr> baseExpr,
                                       ref<EvalEnv> env,
                                       BasicBlock **thisBlock) {
  // Cached?
  Value *final = env->lookupCachedRead(fullExpr);
  if (final)
    return final;

  // Expr snapshotted in-order?
  bool childIsSnapshotted = false;
  unsigned snapshotOffset = 0;
  if (env->lookupSnapshotOffset(fullExpr, &snapshotOffset, &childIsSnapshotted)) {
    const Type *ty = IntegerType::get(_llvmContext, fullExpr->getWidth());
    Function *lookupFn = LookupInMap(_icsSnapshotLookupFns, ty);
    Value* args[] = { ConstantInt::get(_llvmInt32Ty, snapshotOffset) };
    final = CallInst::Create(lookupFn, args, args+1, baseExpr->updates.root->name, *thisBlock);
    env->addCachedRead(fullExpr, final);
    return final;
  }

  // Expr snapshotted out-of-order?
  // In this case, we need to eval the ConcatExpr
  if (childIsSnapshotted) {
    env->pushDisableOrderedReads();
    final = BuildExprEvaluator(fullExpr, env, thisBlock);
    env->popDisableOrderedReads();
    env->addCachedRead(fullExpr, final);
    return final;
  }

  // Has updates?
  if (baseExpr->updates.head) {
    if (DbgInstrumentor) {
      llvm::errs() << "Build symbolic read with updates:\n" << fullExpr << "\n";
    }
    final =
      BuildExprEvaluator(TransformSymbolicReadRemovingUpdates(fullExpr, env), env, thisBlock);
    env->addCachedRead(fullExpr, final);
    return final;
  }

  // Read from a non-symbolic array?
  const Array *root = baseExpr->updates.root;
  if (root->type != Array::Symbolic) {
    // Need to build this value one byte at a time
    if (fullExpr->getWidth() != Expr::Int8) {
      env->pushDisableOrderedReads();
      final = BuildExprEvaluator(fullExpr, env, thisBlock);
      env->popDisableOrderedReads();
      env->addCachedRead(fullExpr, final);
      return final;
    }

    // Now read this byte
    ref<Expr> val;
    if ((root->type == Array::ConstrainedInitialValues) ||
        (root->type == Array::ConstantInitialValues && isa<klee::ConstantExpr>(baseExpr->index))) {
      // In this case, construction of root[x] is simple
      final = BuildExprEvaluator(root->getValueAtIndex(baseExpr->index), env, thisBlock);
    } else {
      assert(root->type == Array::ConstantInitialValues);
      // In this case, to construct root[x], we read from a constant global table that
      // contains all of the initial values for "root".  This table is created once
      // per "root" array.
      llvm::GlobalVariable *gvTable = _constArrayTables[root];
      if (!gvTable) {
        const size_t elemSize = root->getConstantInitialValues().size();
        const ArrayType *tableTy = ArrayType::get(_llvmInt8Ty, elemSize);
        std::vector<Constant*> tmp(elemSize);
        for (size_t k = 0; k < elemSize; ++k) {
          uint64_t v = root->getConstantInitialValues()[k]->getZExtValue();
          tmp[k] = ConstantInt::get(_llvmInt8Ty, v);
        }
        std::string tableName = "__ics_consttable_" + root->name;
        gvTable = AddGlobalMustNotExist(tableTy, ConstantArray::get(tableTy, tmp), tableName);
        _constArrayTables[root] = gvTable;
      }
      // Read from gvTable[baseExpr->index]
      llvm::Value *index = BuildExprEvaluator(baseExpr->index, env, thisBlock);
      Value *indices[] = { _constZero, index };
      llvm::Value *ptr = llvm::GetElementPtrInst::Create(gvTable, indices, indices+2, "", *thisBlock);
      final = new llvm::LoadInst(ptr, "", *thisBlock);
    }

    env->addCachedRead(fullExpr, final);
    return final;
  }

  // No updates on a symbolic array: read the object directly
  const SymbolicObject *obj = _symbolicObjects.find(root);
  if (!obj) {
    llvm::errs() << "ERROR: Read from unknown object (not in table?) " << root->name << "\n";
    assert(0);
  }

  switch (obj->kind) {
  case SymbolicObject::VirtualRegister:
  case SymbolicObject::FunctionArgument:
  case SymbolicObject::ThreadId:
  case SymbolicObject::WaitSlot:
    final = BuildSymbolicReadForReg(obj, fullExpr, env, thisBlock);
    break;

  case SymbolicObject::GlobalVariable:
  case SymbolicObject::ThreadLocalVariable:
  case SymbolicObject::SymbolicInput:
  case SymbolicObject::SymbolicPtr:
    final = BuildSymbolicReadForMem(obj, fullExpr, baseExpr->index, env, thisBlock);
    break;

  default:
    assert(0);
  }

  env->addCachedRead(fullExpr, final);
  return final;
}

Value* Instrumentor::BuildExprEvaluator(ref<Expr> expr,
                                        ref<EvalEnv> env,
                                        BasicBlock **thisBlock) {
  if (ConstantExpr *e = dyn_cast<ConstantExpr>(expr)) {
    return ConstantInt::get(KleeWidthToLlvmType(e->getWidth()), e->getZExtValue());
  }

  if (NotExpr *e = dyn_cast<NotExpr>(expr)) {
    Value *right = BuildExprEvaluator(e->expr, env, thisBlock);
    return llvm::BinaryOperator::CreateNot(right, "", *thisBlock);
  }

  if (CastExpr *e = dyn_cast<CastExpr>(expr)) {
    Value *right = BuildExprEvaluator(e->src, env, thisBlock);
    Instruction::CastOps op = KleeToLlvmCastOp(e->getKind());
    return llvm::CastInst::Create(op, right, KleeWidthToLlvmType(e->width), "", *thisBlock);
  }

  if (CmpExpr *e = dyn_cast<CmpExpr>(expr)) {
    Value *left = BuildExprEvaluator(e->left, env, thisBlock);
    Value *right = BuildExprEvaluator(e->right, env, thisBlock);
    llvm::CmpInst::Predicate pred = KleeToLlvmCmpOp(e->getKind());
    return llvm::CmpInst::Create(Instruction::ICmp, pred, left, right, "", *thisBlock);
  }

  if (SimplifiableBinaryExpr *e = dyn_cast<SimplifiableBinaryExpr>(expr)) {
    return BuildExprEvaluator(e->simplify(), env, thisBlock);
  }

  if (BinaryExpr *e = dyn_cast<BinaryExpr>(expr)) {
    Value *left = BuildExprEvaluator(e->left, env, thisBlock);
    Value *right = BuildExprEvaluator(e->right, env, thisBlock);
    Instruction::BinaryOps op = KleeToLlvmBinaryOp(e->getKind());
    return llvm::BinaryOperator::Create(op, left, right, "", *thisBlock);
  }

  if (SelectExpr *e = dyn_cast<SelectExpr>(expr)) {
    // Simple recursive descent if both branches are constants
    // Otherwise, use a short-circuited "select"
    if (isa<ConstantExpr>(e->trueExpr) && isa<ConstantExpr>(e->falseExpr)) {
      Value *cond = BuildExprEvaluator(e->cond, env, thisBlock);
      Value *ifTrue = BuildExprEvaluator(e->trueExpr, env, thisBlock);
      Value *ifFalse = BuildExprEvaluator(e->falseExpr, env, thisBlock);
      return llvm::SelectInst::Create(cond, ifTrue, ifFalse, "", *thisBlock);
    } else {
      // Check validity of cond to see if we can simplify
      const Solver::Validity v = env->checkValidity(e->cond);
      if (v == Solver::True) {
        return BuildExprEvaluator(e->trueExpr, env, thisBlock);
      }
      if (v == Solver::False) {
        return BuildExprEvaluator(e->falseExpr, env, thisBlock);
      }
      // if (cond) trueExpr else falseExpr
      BasicBlock *trueBlock = env->newBasicBlock(_llvmContext);
      BasicBlock *falseBlock = env->newBasicBlock(_llvmContext);
      BuildBooleanEvaluator(e->cond, env, thisBlock, trueBlock, falseBlock);
      Value *ifTrue  = BuildExprEvaluator(e->trueExpr, env->cloneTrue(e->cond), &trueBlock);
      Value *ifFalse = BuildExprEvaluator(e->falseExpr, env->cloneFalse(e->cond), &falseBlock);
      // phi = [(trueBlock=>ifTrue), (falseBlock=>ifFalse)]
      BasicBlock *joinBlock = env->newBasicBlock(_llvmContext);
      BranchInst::Create(joinBlock, trueBlock);
      BranchInst::Create(joinBlock, falseBlock);
      PHINode *phi = PHINode::Create(KleeWidthToLlvmType(e->getWidth()), "", joinBlock);
      phi->addIncoming(ifTrue, trueBlock);
      phi->addIncoming(ifFalse, falseBlock);
      *thisBlock = joinBlock;
      return phi;
    }
  }

  if (ExtractExpr *e = dyn_cast<ExtractExpr>(expr)) {
    // result = (e >> offset) & (2^width-1)
    Value *right = BuildExprEvaluator(e->expr, env, thisBlock);
    if (e->offset > 0) {
      Value *off = ConstantInt::get(KleeWidthToLlvmType(e->expr->getWidth()), e->offset);
      right = llvm::BinaryOperator::Create(Instruction::LShr, right, off, "", *thisBlock);
    }
    return new llvm::TruncInst(right, KleeWidthToLlvmType(e->width), "", *thisBlock);
  }

  // This will catch all ReadExprs and some ConcatExprs
  if (env->orderedReadsEnabled()) {
    if (ReadExpr *e = Expr::hasOrderedReads(expr).get())
      return BuildSymbolicRead(expr, e, env, thisBlock);
  }

  // This catches all simple Reads when !env->orderedReadsEnabled()
  if (ReadExpr *e = dyn_cast<ReadExpr>(expr))
    return BuildSymbolicRead(expr, e, env, thisBlock);

  // This catches the remaining Concats
  if (ConcatExpr *e = dyn_cast<ConcatExpr>(expr)) {
    Value *left = BuildExprEvaluator(e->left, env, thisBlock);
    Value *right = BuildExprEvaluator(e->right, env, thisBlock);
    // convert to typeof output
    const Type *ty = KleeWidthToLlvmType(e->getWidth());
    left = new llvm::ZExtInst(left, ty, "", *thisBlock);
    right = new llvm::ZExtInst(right, ty, "", *thisBlock);
    // result = (left << right.width) | right
    Value *off = ConstantInt::get(ty, e->right->getWidth());
    left = llvm::BinaryOperator::Create(Instruction::Shl, left, off, "", *thisBlock);
    return llvm::BinaryOperator::Create(Instruction::Or, left, right, "", *thisBlock);
  }

  // We should have covered all cases
  std::cerr << "???: " << *expr << "\n";
  assert(0);
}

void Instrumentor::BuildBooleanEvaluator(ref<Expr> expr,
                                         ref<EvalEnv> env,
                                         BasicBlock **thisBlock,
                                         BasicBlock *successBlock,
                                         BasicBlock *failBlock) {
  // Short-circuited "and"/"or"
  //
  // Env passing: we modify "env" on paths that MUST execute before both
  // "successBlock" and "failBlock", and we clone "env" on all other paths.
  // For example, caller can do:
  //   BuildBooleanEvaluator(cond, env, thisBlock, successBlock, failBlock)
  //   Eval(.., env, successBlock)  // our modifications to "env" are valid on both branches
  //   Eval(.., env, failBlock)
  //
  // After this returns, the value of *thisBlock is indeterminate.  Caller is
  // expected to create successBlock and failBlock and continue from there.  Our
  // job here is to link *thisBlock to successBlock and failBlock, appropriately.
  //
  if (expr->getWidth() == Expr::Bool) {
    if (AndExpr *e = dyn_cast<AndExpr>(expr)) {
      // Check validity of the left expr to see if we can simplify
      const Solver::Validity v = env->checkValidity(e->left);
      if (v == Solver::True) {          // true && right ==> right
        BuildBooleanEvaluator(e->right, env, thisBlock, successBlock, failBlock);
      } else if (v == Solver::False) {  // false && right ==> false
        BuildBooleanEvaluator(Expr::False, env, thisBlock, successBlock, failBlock);
      } else {                          // false && right
        BasicBlock *rightBlock = env->newBasicBlock(_llvmContext);
        BuildBooleanEvaluator(e->left, env, thisBlock, rightBlock, failBlock);
        BuildBooleanEvaluator(e->right, env->cloneTrue(e->left), &rightBlock, successBlock, failBlock);
      }
      return;
    }
    if (OrExpr *e = dyn_cast<OrExpr>(expr)) {
      // Check validity of the left expr to see if we can simplify
      const Solver::Validity v = env->checkValidity(e->left);
      if (v == Solver::True) {         // true  || right ==> true
        BuildBooleanEvaluator(Expr::True, env, thisBlock, successBlock, failBlock);
      } else if (v == Solver::False) { // false || right ==> right
        BuildBooleanEvaluator(e->right, env, thisBlock, successBlock, failBlock);
      } else {                         // left  || right
        BasicBlock *rightBlock = env->newBasicBlock(_llvmContext);
        ref<EvalEnv> oldenv = env->cloneFalse(e->left);
        BuildBooleanEvaluator(e->left, env, thisBlock, successBlock, rightBlock);
        BuildBooleanEvaluator(e->right, oldenv, &rightBlock, successBlock, failBlock);
      }
      return;
    }
    // Simplify booleans
    expr = env->simplifyExpr(expr);
    const Solver::Validity v = env->checkValidity(expr);
    if (v == Solver::True) {
      expr = Expr::True;
    } else if (v == Solver::False) {
      expr = Expr::False;
    }
  }

  // Direct evaluation
  // N.B.: BuildExprEvaluator() will change "thisBlock" when evaluating nested
  // short-circuited conditionals (see case for SelectExpr).  If this happens,
  // we will put the branch in the most-recent block.
  Value *truth = BuildExprEvaluator(expr, env, thisBlock);
  BranchInst::Create(successBlock, failBlock, truth, *thisBlock);
}

void Instrumentor::BuildScheduleSelector(ref<ScheduleTreeNode> node,
                                         ref<EvalEnv> env,
                                         BasicBlock *thisBlock,
                                         BasicBlock *failBlock) {
  assert(!node.isNull());
  assert(!node->edges.empty());

  char namebuf[64];

  for (ScheduleTreeNode::EdgeListTy::const_iterator
       edge = node->edges.begin(); edge != node->edges.end(); ++edge) {
    // There are two cases:
    //
    //  1) schedfrag != NULL
    //     On success, we return a schedule ptr.  If edge->next exists,
    //     it refers to a NEW schedule selector (that is used to update
    //     the schedule after reading dynamic input).
    //
    //  2) schedfrag == NULL
    //     This edge contains a partial constraint.  The schedule is
    //     found via edge->next, which must exist.

    // Where we go on success
    BasicBlock *successBlock = env->newBasicBlock(_llvmContext);
    if (edge->schedfrag.get()) {
      const size_t fragId = edge->schedfrag->fragId;
      assert(fragId);
      snprintf(namebuf, sizeof namebuf, "frag_%lu_success", fragId);
      successBlock->setName(namebuf);
      // "return &sched"
      Constant *schedPtr = _llvmModule.getNamedGlobal(GetNameForSchedule(fragId));
      assert(schedPtr);
      schedPtr = llvm::ConstantExpr::getBitCast(schedPtr, _icsSchedulePtrTy);
      ReturnInst::Create(_llvmContext, schedPtr, successBlock);
    } else {
      assert(edge->next.get());
    }

    // Where we go on failure
    BasicBlock *nextBlock;
    if (edge+1 == node->edges.end()) {
      nextBlock = failBlock;
    } else {
      nextBlock = env->newBasicBlock(_llvmContext);
    }

    // This block: name
    thisBlock->setName(env->uniquifyNameWithId("check_node", 1));
    env->resetBlockLabelPrefix(thisBlock->getNameStr());

    // This block: if (summary) goto success else goto next
    ref<Expr> summary = edge->getSummary();
    BuildBooleanEvaluator(summary, env, &thisBlock, successBlock, nextBlock);

    // The code we generate is:
    //   if (eval(edge)) {
    //     successBlock: if (eval(edge->next)) ...
    //   }
    //   else {
    //     nextBlock: if (eval(edge+1)) ...
    //   }
    // We thread "env" through a sequence of instructions s.t. if A precedes B
    // in the sequence, then A dominates B.  This means we must setup the env
    // for edge+1 *before* evaluating edge->next, as eval(edge->next) does not
    // dominate eval(edge+1).
    ref<EvalEnv> nextEnv;
    if (edge+1 != node->edges.end()) {
      nextEnv = env->cloneFalseCanFail(summary);
    }

    // Recursively expand the "next" node
    if (edge->next.get()) {
      if (edge->schedfrag.get()) {
        assert(edge->schedfrag->nextSelectorId);
        BuildScheduleSelector(edge->next, edge->schedfrag->nextSelectorId);
      } else {
        BuildScheduleSelector(edge->next, env, successBlock, failBlock);
      }
    }

    // XXX: FIXME: Ensure this happens only when schedules are redundant
    if (edge+1 != node->edges.end() && !nextEnv.get()) {
      STATS["SkippingInfeasibleScheduleConstraint"]++;
      if (DbgInstrumentor) {
        llvm::errs() << "Skipping infeasible schedule constraint!\n";
        llvm::errs() << "  ... cond:\n" << summary << "\n";
        struct PrintNestedSelectors : public ScheduleTreeNodeVisitor {
          void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
            if (edge.schedfrag.get()) {
              llvm::errs() << "  ... skips:";
              llvm::errs() << " (schedId:" << edge.schedfrag->schedId
                           << " fragId:" << edge.schedfrag->fragId
                           << " nextSelectorId:" << edge.schedfrag->nextSelectorId << ")\n";
            }
          }
        };
        for (++edge; edge != node->edges.end(); ++edge) {
          PrintNestedSelectors v;
          v.visitEdge(const_cast<ScheduleTreeNode::Edge&>(*edge), 0);
          if (edge->next.get())
            TraverseScheduleTree(edge->next, v);
        }
      }
      if (nextBlock->empty()) {
        assert(nextBlock != failBlock);
        new llvm::UnreachableInst(_llvmContext, nextBlock);
      }
      return;
    }

    // Advance
    thisBlock = nextBlock;
    env = nextEnv;
  }
}

void Instrumentor::BuildScheduleSelector(ref<ScheduleTreeNode> node, const size_t selectorId) {
  // This builds a new selector function
  // The return value of each selector function is a pointer into the schedule table
  char namebuf[64];
  snprintf(namebuf, sizeof namebuf, "__ics_schedule_selector_%lu", selectorId);

  Function *f =
      cast<Function>(_llvmModule.getOrInsertFunction(namebuf, _icsSchedulePtrTy, NULL));
  _scheduleSelectors[selectorId] = f;

  // DEBUG
  if (DbgInstrumentor) {
    llvm::errs() << "Building Schedule "
                 << "(selectorId=" << selectorId << " node=" << node.get() << ")\n";
    for (ImmutableExprMap<unsigned>::iterator
         it = node->snapshotOffsets.begin(); it != node->snapshotOffsets.end(); ++it) {
      llvm::errs() << "Have snapshot for: " << it->first << "\n";
    }
  }

  // Create an entry block
  BasicBlock *entryBlock = BasicBlock::Create(_llvmContext, "", f);

  // Create a failure block
  BasicBlock *failBlock = BasicBlock::Create(_llvmContext, "fail", f);
  Value *arg = ConstantInt::get(_llvmInt32Ty, selectorId);
  CallInst *call = CallInst::Create(_icsPanicNoSched, arg, "", failBlock);
  call->setDoesNotReturn();
  new llvm::UnreachableInst(_llvmContext, failBlock);

  // Construct code to evaluate the tree
  // This will create success blocks as necessary
  ref<EvalEnv> env = new EvalEnv(f, _kleeExecutor, node->snapshotOffsets);
  BuildScheduleSelector(node, env, entryBlock, failBlock);

  // Sort blocks by name
  // N.B.: We assume all blocks are named, and whatever name is chosen for
  // the "entry" block MUST lexicographically precede all other block names.
  std::vector<BasicBlock*> blocks;
  for (Function::iterator bb = f->begin(); bb != f->end(); ++bb)
    blocks.push_back(bb);

  std::sort(blocks.begin(), blocks.end(), BasicBlockNameLess);

  // Ugly: need to sort in a separate array then reinsert
  for (size_t k = 0; k < blocks.size(); ++k) {
    blocks[k]->moveAfter(&f->back());
  }
}

void Instrumentor::BuildScheduleSelectors() {
  assert(!_regions.empty());
  assert(!_scheduleTrees.empty());
  assert(_scheduleSelectors.empty());

  // Build schedule selectors for each region
  // This recursively builds selectors for all nodes in the schedule tree
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    const size_t regionId = r->second->numericId;
    BuildScheduleSelector(_scheduleTrees[regionId], regionId);
  }

  // Emit the table
  assert(!_scheduleSelectors.empty());
  AddGlobalFunctionTable(_scheduleSelectors, "selector", "__ics_schedule_selectors");
}

typedef std::set< ref<ReadExpr> > ReadExprSet;
struct Instrumentor::SnapshotInfo {
  const SymbolicObject* inputObj;
  ReadExprSet exprs;   // exprs to snapshot
  size_t snapshotId;
  size_t selectorId;
  // all snapshots available *before* this input is read
  ImmutableExprMap<unsigned> snapshotOffsets;
};

void Instrumentor::BuildSnapshotFunction(ref<ScheduleTreeNode> node, const SnapshotInfo &info) {
  // This builds a new snapsnot function
  // Type is (char*) -> void
  char namebuf[64];
  if (info.inputObj) {
    snprintf(namebuf, sizeof namebuf, "__ics_snapshot_input_%lu_after_selector_%lu",
             info.snapshotId, info.selectorId);
  } else {
    assert(info.snapshotId == info.selectorId);
    snprintf(namebuf, sizeof namebuf, "__ics_snapshot_for_region_%lu", info.snapshotId);
  }

  Function *f =
    cast<Function>(_llvmModule.getOrInsertFunction(namebuf, _llvmVoidTy, _llvmPtrCharTy, NULL));
  _snapshotFns[info.snapshotId] = f;

  // DEBUG
  if (DbgInstrumentor) {
    llvm::errs() << "Building Snapshot Function "
                 << "(id=" << info.snapshotId << " node=" << node.get() << ")\n";
    for (ImmutableExprMap<unsigned>::iterator
         it = info.snapshotOffsets.begin(); it != info.snapshotOffsets.end(); ++it) {
      llvm::errs() << "Have snapshot for: " << it->first << "\n";
    }
  }

  // For input snapshots: the arg gives a ptr to the input
  ref<EvalEnv> env = new EvalEnv(f, _kleeExecutor, info.snapshotOffsets);
  if (info.inputObj) {
    Argument *arg = f->arg_begin();
    arg->setName("inputPtr");
    env->addInputPtr(info.inputObj, arg);
  }

  // Enumerate exprs to snapshot
  // N.B.: Here we iterate on node->snapshotOffsets, which includes all exprs
  // that need to be snapshot before "node" can execute.  Above, we initialize
  // "env" using info.snapshotOffsets, which includes only those exprs that
  // must be snapshot before the input "info.inputObj" is received.
  typedef std::map<ref<Expr>, std::pair<unsigned,unsigned> > WordsMapTy;
  typedef std::map<UpdateList, WordsMapTy> OffsetsMapTy;
  OffsetsMapTy offsetsMap;  // ReadExpr.updates -> (index -> (snapshotOffset,nbytes))

  for (ImmutableExprMap<unsigned>::iterator
       it = node->snapshotOffsets.begin(); it != node->snapshotOffsets.end(); ++it) {
    ref<ReadExpr> re = cast<ReadExpr>(it->first);
    const SymbolicObject *obj = _symbolicObjects.find(re->updates.root);
    assert(obj);

    // for inputs: we snapshot (at most) one input per snapshot function
    // for globals: we snapshot all the globals in one snapshot function
    if (info.inputObj) {
      if (obj != info.inputObj)
        continue;
    } else {
      if (obj->kind == SymbolicObject::SymbolicInput)
        continue;
    }

    // Just add this expr for now: we coalesce in the next pass
    offsetsMap[re->updates][re->index] = std::make_pair(it->second, 1);
  }

  // Coalesce exprs
  // Dumb algorithm: look for adjacent words, pair-by-pair
  for (OffsetsMapTy::iterator it = offsetsMap.begin(); it != offsetsMap.end(); ++it) {
    WordsMapTy &words = offsetsMap[it->first];
    const bool isLittleEndian = klee::Context::get().isLittleEndian();

    bool changed = true;
    while (changed) {
      changed = false;
      WordsMapTy::iterator w0;
      WordsMapTy::iterator w1;
      for (w0 = words.begin(); w0 != words.end(); ++w0) {
        for (w1 = words.begin(); w1 != words.end(); /* nop */) {
          // Skip if width > 64
          if (w0->second.second + w1->second.second > 64) {
            ++w1;
            continue;
          }
          // Little endian: look for w1.snapshotOffset = w0.snapshotOffset+w0.nbytes
          // Big    endian: look for w1.snapshotOffset = w0.snapshotOffset-w0.nbytes
          if (( isLittleEndian && w1->second.first != w0->second.first+w0->second.second) ||
              (!isLittleEndian && w1->second.first != w0->second.first-w0->second.second)) {
            ++w1;
            continue;
          }
          // Look for w1.index = w0.index+w0.nbytes
          ConstantExpr *cex = dyn_cast<ConstantExpr>(SubExpr::create(w1->first, w0->first));
          if (!cex || cex->getZExtValue() != w0->second.second) {
            ++w1;
            continue;
          }
          // Little endian: merge to [w0.snapshotOffset, w0.snapshotOffset+w0.nbytes+w1.nbytes]
          // Big    endian: merge to [w1.snapshotOffset, w1.snapshotOffset+w0.nbytes+w1.nbytes]
          if (isLittleEndian) {
            w0->second.second += w1->second.second;
          } else {
            w1->second.first = w1->second.first;
            w0->second.second += w1->second.second;
          }
          // Coalesce!
          changed = true;
          WordsMapTy::iterator tmp = w1;
          ++tmp;
          words.erase(w1);
          w1 = tmp;
        }
      }
    }
  }

  // Snapshot each expr
  // TODO FIXME: need to ensure that "ty" matches 8, 16, 32, or 64
  BasicBlock *block = BasicBlock::Create(_llvmContext, "", f);
  for (OffsetsMapTy::iterator it = offsetsMap.begin(); it != offsetsMap.end(); ++it) {
    WordsMapTy &words = offsetsMap[it->first];
    for (WordsMapTy::iterator w = words.begin(); w != words.end(); ++w) {
      ref<Expr> expr = Expr::createMultibyteRead(it->first, w->first, w->second.second);
      const Type *ty = IntegerType::get(_llvmContext, expr->getWidth());
      Value *args[] = { ConstantInt::get(_llvmInt32Ty, w->second.first),
                        BuildExprEvaluator(expr, env, &block) };
      CallInst::Create(LookupInMap(_icsSnapshotLogFns, ty), args, args+2, "", block);
    }
  }

  // Close with "ret void"
  ReturnInst::Create(_llvmContext, block);
}

void Instrumentor::BuildSnapshotFunctions() {
  assert(!_regions.empty());
  assert(!_scheduleTrees.empty());

  // Mapping of ScheduleTreeNodes to the things we need to snapshot at those nodes
  typedef std::map<ScheduleTreeNode*, std::list<SnapshotInfo> > SnapshotInfosTy;
  SnapshotInfosTy snapshotInfos;

  // Determine the set of exprs that need to be snapshot at each ScheduleTreeNode
  struct SnapshotVisitor : public ScheduleTreeNodeVisitor {
    const SymbolicObjectTable *symbolicObjects;
    const UsedArraySetTy *usedArrays;

    std::vector< std::vector<SnapshotInfo*> > snapshotInfosStack;
    ReadExprSet *usedMemExprs;
    SnapshotInfosTy *snapshotInfos;
    size_t regionId;

    const SymbolicObject* findObjectIfSymbolic(ref<ReadExpr> re) {
      const Array *root = re->updates.root;
      if (root->type != Array::Symbolic)
        return NULL;
      const SymbolicObject *obj = symbolicObjects->find(re->updates.root);
      if (!obj) {
        llvm::errs() << "ERROR: Cound not find symbolic object for ReadExpr:\n"
                     << re << "\n";
        assert(0);
      }
      return obj;
    }

    void visitEdge(ScheduleTreeNode::Edge &edge, size_t depth) {
      // Enumerate the used ReadExprs
      Expr::CachedReadsTy reads;
      for (size_t k = 0; k < edge.terms.size(); ++k)
        klee::findReads(edge.terms[k], true, reads);

      // Classify the used ReadExprs
      for (size_t k = 0; k < reads.size(); ++k) {
        ref<ReadExpr> re = cast<ReadExpr>(Expr::removeUpdatesFromSymbolicRead(reads[k]));
        std::set<const SymbolicObject*> inputsUsedByIndex;

        // FIXME: I think the above use of removeUpdatesFromSymbolicRead() might be
        // slighly broken.  It assumes that all ReadExprs in updates for each reads[k]
        // get included in reads[] (by klee::findReads), however, klee::findReads
        // might not do a complete search through copied arrays (which could have
        // more updates buried in the Forall exprs ... ugh this is ugly).

        // We do not currently support snapshotting of ReadExprs where the
        // ReadExpr.index uses a symbolic input.  Check if this is the case.
        // TODO: A more complete implementation would snapshot the ENTIRE memory object
        Expr::CachedReadsTy tmp;
        klee::findReads(re->index, true, tmp);
        for (size_t k = 0; k < tmp.size(); ++k) {
          const SymbolicObject *obj = findObjectIfSymbolic(tmp[k]);
          if (obj && obj->kind == SymbolicObject::SymbolicInput)
            inputsUsedByIndex.insert(obj);
        }

        // In the above case, dump a warning message and then skip to the next
        // ReadExpr, as we have not implemented snapshotting for this ReadExpr.
        if (!inputsUsedByIndex.empty()) {
          typedef std::pair< ref<Expr>, std::set<const SymbolicObject*> > WarningType;
          static std::tr1::unordered_set<WarningType> warnings;
          static bool once = false;
          WarningType wt = (DbgInstrumentor) ? WarningType(re,inputsUsedByIndex)
                                             : WarningType(Expr::Zero32,inputsUsedByIndex);
          if (warnings.insert(wt).second) {
            if (!once) {
              llvm::errs() << "========== Snapshotting Is Only Partially Implemented ==========\n"
                           << "WARNING: In memory access a[x], the index 'x' depends on SymbolicInput!\n"
                           << "We have not yet implemented snapshotting for this case.  We will assume\n"
                           << "that a[] is unmodified during the region, and if this assumption is\n"
                           << "invalid, execution may be incorrect.\n";
              once = true;
            } else {
              llvm::errs() << "WARNING: In memory access a[x], the index 'x' depends on SymbolicInput!\n";
            }
            if (DbgInstrumentor) {
              llvm::errs() << "a[x]:\n" << re << "\n";
            }
            for (std::set<const SymbolicObject*>::const_iterator
                 it = inputsUsedByIndex.begin(); it != inputsUsedByIndex.end(); ++it) {
              llvm::errs() << "input used by 'x':\n";
              SymbolicObject::Dump("  ", *it);
            }
            if (edge.schedfrag.get()) {
              llvm::errs() << "ScheduleTreeEdge:\n"
                           << "  schedId:" << edge.schedfrag->schedId << "\n"
                           << "  fragId:" << edge.schedfrag->fragId << "\n"
                           << "  nextSelectorId:" << edge.schedfrag->nextSelectorId << "\n"
                           << "\n";
            }
          }
          // Can't snapshot this re
          continue;
        }

        // Classify this ReadExpr, ignoring reads of non-symbolic arrays
        // TODO: "non-symbolic arrays" may actually refer to symbolic arrays
        // TODO: need to make sure that such references are handled
        const SymbolicObject *obj = findObjectIfSymbolic(re);
        if (!obj)
          continue;

        switch (obj->kind) {
        case SymbolicObject::GlobalVariable:
        case SymbolicObject::ThreadLocalVariable:
        case SymbolicObject::SymbolicPtr: {
          // Ignore mem objects in the initial region
          // N.B.: These should occur in test code only (for tests of symbolic ptr resolution)
          if (regionId == 1) {
            llvm::errs() << "WARNING: Ignoring in mem epxr from initial region\n"
                         << re << "\n";
          } else {
            usedMemExprs->insert(re);
          }
          break;
        }

        case SymbolicObject::SymbolicInput: {
          bool found = false;
          // Should use ONLY those inputs that have been defined!
          for (size_t k = 0; k < snapshotInfosStack.size(); ++k) {
            for (size_t n = 0; n < snapshotInfosStack[k].size(); ++n) {
              SnapshotInfo *info = snapshotInfosStack[k][n];
              if (info && info->inputObj == obj) {
                info->exprs.insert(re);
                found = true;
                break;
              }
            }
          }
          if (!found) {
            llvm::errs() << "ERROR: SymbolicInput used that was not first defined!\n"
                         << "ReadExpr:\n" << re << "\n";
            assert(0);
          }
          break;
        }

        default:
          break;
        }
      }

      // Has new inputs?
      // If so, update snapshotInfosStack for recursion on our child schedfrags
      ref<ScheduleFragment> schedfrag = edge.schedfrag;
      if (schedfrag.get() && edge.next.get()) {
        assert(schedfrag->nextSelectorId);
        std::vector<SnapshotInfo*> topush;
        // Push a new snapshot info for each used input
        for (hbnode_id_t k = schedfrag->firstNodeId; k <= schedfrag->lastNodeId; ++k) {
          const SymbolicObject *obj =  schedfrag->hbGraph->getNode(k).inputObj();
          if (!obj || !usedArrays->count(obj->array))
            continue;
          assert(obj->kind == SymbolicObject::SymbolicInput);
          assert(schedfrag->inputIds.count(obj));
          SnapshotInfosTy::mapped_type &list = (*snapshotInfos)[edge.next.get()];
          list.push_back(SnapshotInfo());
          list.back().inputObj = obj;
          list.back().snapshotId = schedfrag->inputIds[obj];
          list.back().selectorId = schedfrag->nextSelectorId;
          topush.push_back(&list.back());
        }
        snapshotInfosStack.push_back(topush);
      }
    }

    void visitEdgePost(ScheduleTreeNode::Edge &edge, size_t depth) {
      if (edge.schedfrag.get() && edge.next.get())
        snapshotInfosStack.pop_back();
    }
  };

  {
    SnapshotVisitor v;
    v.symbolicObjects = &_symbolicObjects;
    v.usedArrays = &_usedArrays;
    v.snapshotInfos = &snapshotInfos;
    for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
      const size_t id = r->second->numericId;
      ScheduleTreeNode *root = _scheduleTrees[id].get();
      // Add a new snapshot info
      SnapshotInfosTy::mapped_type &list = snapshotInfos[root];
      list.push_back(SnapshotInfo());
      list.back().inputObj = NULL;
      list.back().snapshotId = id;
      list.back().selectorId = id;
      // Traverse
      v.usedMemExprs = &list.back().exprs;
      v.regionId = id;
      TraverseScheduleTree(root, v);
      assert(v.snapshotInfosStack.empty());
    }
  }

  // Setup the snapshots offsets at each node
  // N.B.: This assumes that all exprs in SnapshotInfo.exprs have width=Int8
  struct SnapshotOffsetsVisitor : public ScheduleTreeNodeVisitor {
    std::stack<ScheduleTreeNode*> parents;
    SnapshotInfosTy *snapshotInfos;
    size_t maxOffset;

    void visitNode(ScheduleTreeNode &node, size_t depth) {
      SnapshotInfosTy::iterator nodeIt = snapshotInfos->find(&node);
      if (nodeIt == snapshotInfos->end())
        return;

      // Inherit from our immediate parent
      if (!parents.empty()) {
        node.snapshotOffsets = parents.top()->snapshotOffsets;
      }

      // Add all exprs snapshotted at this node
      for (SnapshotInfosTy::mapped_type::iterator
           it = nodeIt->second.begin(); it != nodeIt->second.end(); ++it) {
        it->snapshotOffsets = node.snapshotOffsets;  // things valid *before* this input
        // This sorting will maximize the effectiveness of coalescing
        typedef std::vector< ref<ReadExpr> > SortedExprsTy;
        SortedExprsTy sorted = klee::SortReadExprsAdjacent(it->exprs);
        for (SortedExprsTy::iterator re = sorted.begin(); re != sorted.end(); ++re) {
          const unsigned offset = node.snapshotOffsets.size();
          node.snapshotOffsets = node.snapshotOffsets.insert(std::make_pair(*re, offset));
          if (DbgInstrumentor) {
            llvm::errs() << "SNAPSHOT node:" << (&node)
                         << " offset: " << offset << " expr:" << (*re) << "\n";
          }
        }
      }

      maxOffset = std::max(maxOffset, node.snapshotOffsets.size());
      parents.push(&node);
    }

    void visitNodePost(ScheduleTreeNode &node, size_t depth) {
      if (snapshotInfos->count(&node))
        parents.pop();
    }
  };

  size_t snapshotBufferSize;
  {
    SnapshotOffsetsVisitor v;
    v.snapshotInfos = &snapshotInfos;
    v.maxOffset = 0;
    for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
      TraverseScheduleTree(_scheduleTrees[r->second->numericId].get(), v);
      assert(v.parents.empty());
    }
    snapshotBufferSize = v.maxOffset + 1;
  }

  // Emit the snapshot functions
  for (SnapshotInfosTy::iterator
       nodeIt = snapshotInfos.begin(); nodeIt != snapshotInfos.end(); ++nodeIt) {
    for (SnapshotInfosTy::mapped_type::const_iterator
         it = nodeIt->second.begin(); it != nodeIt->second.end(); ++it) {
      BuildSnapshotFunction(nodeIt->first, *it);
    }
  }

  // Emit the table
  assert(!_snapshotFns.empty());
  AddGlobalFunctionTable(_snapshotFns, "snapshotfn", "__ics_snapshot_functions");

  // Emit the buffer
  const ArrayType *ty = ArrayType::get(_llvmInt8Ty, snapshotBufferSize);
  ReplaceGlobalMustExist(ty, Constant::getNullValue(ty), "__ics_snapshot_buffer", false);
}

//-----------------------------------------------------------------------------
// Callstack logging
//-----------------------------------------------------------------------------

// TODO: We're checking for hash collisions on the final ctx only, not
// the intermediate contexts.  Is this a problem?

uint64_t Rand64() {
  return (uint64_t)rand() | ((uint64_t)rand() << 32);
}

uint64_t UniqueRandInstId(const Instruction *i,
                          llvm::DenseMap<const Instruction*, uint64_t> &toNum,
                          llvm::DenseMap<uint64_t, const Instruction*> &fromNum,
                          std::map<const Instruction*, uint64_t> *currToNum,
                          std::map<uint64_t, const Instruction*> *currFromNum) {
  // Use old value if available
  if (toNum.count(i))
    return toNum[i];
  if (currToNum->count(i))
    return (*currToNum)[i];

  // Make a unique new value
  uint64_t v;
  for (;;) {
    v = Rand64();
    if (currFromNum->count(v) || fromNum.count(v)) {
      STATS["CallSiteIdCollisions"]++;
      continue;
    }
    break;
  }

  (*currToNum)[i] = v;
  (*currFromNum)[v] = i;
  return v;
}

const CallingContext* StripCallCtx(const CallingContext *callctx) {
  // We have to strip ALL klee model functions from the ctx, as these
  // will NOT exist in the final program.  These is especially important
  // for the model functions that have callbacks.
  return callctx->removeFunctionsMatchingPrefix("__klee_");
}

void Instrumentor::AddUsedCallCtx(const CallingContext *callctx) {
  callctx = StripCallCtx(callctx);

  // Already mapped?
  if (!AddUniqueId(&_usedCallCtxsShortId, callctx).second)
    return;

  STATS["UsedCallCtxs"]++;
  _usedCallCtxList.push_back(callctx);
  for (CallingContext::iterator ki = callctx->begin(); ki != callctx->end(); ++ki) {
    _instsUsedInCallCtxs.insert((*ki)->inst);
    _fnsUsedInCallCtxs.insert((*ki)->inst->getParent()->getParent());
  }

  // Map each instruction to a random value
  // We repeat until we're sure there is no hash collision
  std::map<const Instruction*, uint64_t> currToNum;
  std::map<uint64_t, const Instruction*> currFromNum;
  uint64_t final;
  for (int niter = 1; ; ++niter) {
    final = 0;
    currToNum.clear();
    currFromNum.clear();

    // Foreach inst, lookup if it exists, otherwise make a new random value
    for (CallingContext::iterator ki = callctx->begin(); ki != callctx->end(); ++ki) {
      const uint64_t v =
        UniqueRandInstId((*ki)->inst, _usedCallsToId, _usedCallsFromId, &currToNum, &currFromNum);
      final = 3*final + v;
    }

    // Not unique => try again
    if (_usedCallCtxsFromId.count(final)) {
      llvm::errs() << "AddUsedCallCtx: hash collision (" << niter << " iterations)\n";
      STATS["CallCtxIdCollisions"]++;
      continue;
    }

    // Found: update the maps
    _usedCallCtxsToId[callctx] = final;
    _usedCallCtxsFromId[final] = callctx;

    for (std::map<const Instruction*, uint64_t>::iterator
         it = currToNum.begin(); it != currToNum.end(); ++it) {
      _usedCallsToId[it->first] = it->second;
      _usedCallsFromId[it->second] = it->first;
    }

    break;
  }
}

uint64_t Instrumentor::LookupCallCtxLongId(const CallingContext *callctx) {
  callctx = StripCallCtx(callctx);
  assert(_usedCallCtxsToId.count(callctx));
  return _usedCallCtxsToId[callctx];
}

unsigned Instrumentor::LookupCallCtxShortId(const CallingContext *callctx) {
  callctx = StripCallCtx(callctx);
  return LookupUniqueId(_usedCallCtxsShortId, callctx);
}

uint64_t Instrumentor::LookupCallSiteId(const Instruction *i) {
  assert(_usedCallsToId.count(i));
  return _usedCallsToId[i];
}

void Instrumentor::EnumerateUsedCallCtxs() {
  // Used by localvars
  for (UsedArraySetTy::iterator it = _usedArrays.begin(); it != _usedArrays.end(); ++it) {
    const SymbolicObject *obj = _symbolicObjects.find(*it);
    assert(obj);
    if (obj->kind != SymbolicObject::VirtualRegister &&
        obj->kind != SymbolicObject::FunctionArgument)
      continue;
    assert(obj->callctx);
    AddUsedCallCtx(obj->callctx);
  }

  // Used by region ids
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    for (ParallelRegionId::ContextListTy::const_iterator
         ctx = r->first.contexts.begin(); ctx != r->first.contexts.end(); ++ctx) {
      assert(ctx->strippedctx);
      AddUsedCallCtx(ctx->strippedctx);
    }
  }

  // Used by schedules
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    const PathManager::ScheduleListTy &schedules = r->second->paths.getSchedules();
    for (SchedulesIterator sched = schedules.begin(); sched != schedules.end(); ++sched) {
      for (HBGraph::node_iterator
           node = (*sched)->hbGraph->nodesBegin(); node != (*sched)->hbGraph->nodesEnd(); ++node) {
        assert(node->callctx());
        AddUsedCallCtx(node->callctx());
      }
    }
  }

  // Expand _fnsUsedInCallCtxs to include all functions that
  // (transitively) call a function in  _fnsUsedInCallCtxs.
  FuncSetTy empty;
  BottomUpCallGraphClosure(_kleeModule.DSAA->getBottomUpCallGraph(),
                           _fnsUsedInCallCtxs,  // seeds
                           empty,               // ignores
                           &_fnsUsedInCallCtxs);
}

// TODO: not handling InvokeInst yet: this requires inserting a
// pop on *both* outgoing edges of the invoke, which can be tricky

bool Instrumentor::MustLogCallStackAtInstruction(Instruction *i) {
  if (isa<InvokeInst>(i))
    assert(0);

  if (!OptMinimizeCallLogging)
    return true;

  // We MUST log this call if it appears in a used callctx
  if (_instsUsedInCallCtxs.count(i))
    return true;

  // Don't need to log unless it's a call
  if (!isa<CallInst>(i))
    return false;

  // We can avoid logging this call if it never (transitively) calls
  // a function that appears in a used CallingContext.
  llvm::CallSite cs(i);
  const DSCallGraph &CG = _kleeModule.DSAA->getCallGraph();
  for (DSCallGraph::call_target_iterator f = CG.call_target_begin(cs),
                                      fEnd = CG.call_target_end(cs); f != fEnd; ++f) {
    if (_fnsUsedInCallCtxs.count(*f))
      return true;
  }

  return false;
}

void Instrumentor::InstrumentCallStackAt(Instruction *i, const uint64_t id) {
  // Push before
  CallInst::Create(_icsCallStackPush, ConstantInt::get(_llvmInt64Ty, id), "", i);

  // Pop after, but skip pop if call is followed by unreachable
  BasicBlock::iterator next = i;
  if (!isa<ReturnInst>(next)) {
    ++next;
    if (!isa<UnreachableInst>(next))
      CallInst::Create(_icsCallStackPop, "", next);
  }
}

void Instrumentor::InstrumentCallStacks() {
  std::set<const Instruction*> visited;

  // Ugly cases: the HBNodes for thread spawn and thread exit each use a context
  // with one entry that is either:
  //   (a) the first instruction of the thread's run function (for creation), or
  //   (b) the last instruction (for exit)
  // How we handle each:
  //   (a) This instruction is impossible to instrument in general, as it may
  //       exist in its function's prologue.  Our hack is to add a dummy call
  //       at the end of the prologue and instrument that.  In this case, we
  //       need to execute that HBNode by calling klee_ics_do_next_sync_op().
  //   (b) We add a dummy call just before the return and instrument that.  In
  //       this case, the implicit call to pthread_exit will execute the HBNode
  //       for us, so there's no need to call klee_ics_do_next_sync_op().
  for (UsedCallCtxListTy::iterator
       callctx = _usedCallCtxList.begin(); callctx != _usedCallCtxList.end(); ++callctx) {
    if ((*callctx)->size() != 1)
      continue;
    KInstruction *ki = (*callctx)->youngest();
    Function *f = ki->inst->getParent()->getParent();
    BasicBlock::iterator insertBefore;
    // Case (a): First instruction?
    if (&f->getEntryBlock().front() == ki->inst) {
      // Skip the prologue
      insertBefore = f->getEntryBlock().getFirstNonPHIOrDbg();
      while (isa<AllocaInst>(insertBefore))
        ++insertBefore;
      assert(insertBefore);
      assert(!isa<UnreachableInst>(insertBefore));
      if (DbgInstrumentor) {
        llvm::errs() << "InstrumentThreadEntry:\n"
                     << CallingContextPrinter(*callctx, "  ");
      }
      // Instrument to exec the HB node
      insertBefore = CallInst::Create(_icsDoInitialSyncOp, "", insertBefore);
    }
    // Case (b): Last instruction?
    else if (isa<ReturnInst>(ki->inst)) {
      insertBefore = ki->inst;
      if (DbgInstrumentor) {
        llvm::errs() << "InstrumentThreadExit:\n"
                     << CallingContextPrinter(*callctx, "  ");
      }
    }
    else {
      continue;
    }
    // Instrument to log the call ctx
    if (visited.insert(ki->inst).second) {
      InstrumentCallStackAt(insertBefore, LookupCallSiteId(ki->inst));
    }
  }

  // Instrument call instructions for push/pop
  for (std::vector<KFunction*>::const_iterator
       kf = _kleeModule.functions.begin(); kf != _kleeModule.functions.end(); ++kf) {
    for (unsigned i = 0; i < (*kf)->numInstructions; ++i) {
      KInstruction *ki = (*kf)->instructions[i];
      if (!visited.insert(ki->inst).second)
        continue;
      if (MustLogCallStackAtInstruction(ki->inst)) {
        InstrumentCallStackAt(ki->inst, LookupCallSiteId(ki->inst));
      }
    }
  }

  // Sanity check: ensure we visited each inst used in a callctx
  for (UsedCallCtxListTy::iterator
       callctx = _usedCallCtxList.begin(); callctx != _usedCallCtxList.end(); ++callctx) {
    for (size_t k = 0; k < (*callctx)->size(); ++k) {
      KInstruction *ki = (**callctx)[k];
      if (!visited.count(ki->inst)) {
        llvm::errs() << "ERROR: Uninstrumented callctx element:";
        DumpKInstruction("\n  ", ki);
        llvm::errs() << "\nWithing this context:\n"
                     << CallingContextPrinter(*callctx, "  ");
        assert(0);
      }
    }
    if (DbgInstrumentor) {
      llvm::errs() << "CallCtx[" << LookupCallCtxShortId(*callctx)
                   << "::" << LookupCallCtxLongId(*callctx) << "] =";
      for (size_t k = 0; k < (*callctx)->size(); ++k) {
        KInstruction *ki = (**callctx)[k];
        llvm::errs() << " " << LookupCallSiteId(ki->inst);
      }
      llvm::errs() << "\n";
    }
  }
}

//-----------------------------------------------------------------------------
// Variable logging
//-----------------------------------------------------------------------------

// NOTE: Log fn args are (objid, callctxid, value)
// The idea is that at runtime, we do log[objid] = value iff currctxid == callctxid

bool Instrumentor::HasUniqueCallingContext(const Function *f,
                                           const CallingContext *expectedCtx) {
  if (!OptMinimizeCallLogging)
    return false;

  typedef llvm::DSAA::CallerMapTy CallerMapTy;
  const CallerMapTy &BottomUpCG = _kleeModule.DSAA->getBottomUpCallGraph();

  // Used to assert that, if we find a unique ctx, it must match the exepectedCtx
  bool matches = true;
  CallingContext::reverse_iterator ctxIt = expectedCtx->rbegin();

  // Go up the callchain
  for (;;) {
    CallerMapTy::const_iterator CI = BottomUpCG.find(f);
    if (CI == BottomUpCG.end()) {
      assert(_kleeModule.DSAA->getCallGraph().isKnownRoot(f));
      assert(matches);
      assert(ctxIt == expectedCtx->rend());
      return true;
    }

    CallerMapTy::mapped_type::const_iterator begin = CI->second.begin();
    CallerMapTy::mapped_type::const_iterator end = CI->second.end();
    assert(begin != end);

    // Unique caller?
    f = *begin;
    if (++begin != end)
      return false;

    // If we hit the end of expectedCtx, that probably means we're a
    // thread spawn function that is also sometimes called directly, so
    // there is not a unique calling context here.
    if (ctxIt == expectedCtx->rend())
      return false;

    // Check expectedCtx
    if ((*ctxIt)->inst->getParent()->getParent() != f) {
      matches = false;
    }
    ++ctxIt;
  }
}

void Instrumentor::InstrumentLocalVars() {
  for (UsedArraySetTy::iterator it = _usedArrays.begin(); it != _usedArrays.end(); ++it) {
    const SymbolicObject *obj = _symbolicObjects.find(*it);
    assert(obj);
    if (obj->kind != SymbolicObject::VirtualRegister &&
        obj->kind != SymbolicObject::FunctionArgument)
      continue;

    STATS["LocalVarsInstrumented"]++;
    assert(obj->callctx);
    const unsigned objId = LookupObjectId(obj);
    const uint64_t callctxId = LookupCallCtxLongId(obj->callctx);
    Value *val = const_cast<Value*>(obj->getProgramValue());
    Function *f;

    // Args  => log "val" at fn entry
    // Insts => log "val" just after it is computed
    BasicBlock::iterator insertBefore;
    if (llvm::Argument *arg = dyn_cast<Argument>(val)) {
      f = arg->getParent();
      insertBefore = f->getEntryBlock().getFirstNonPHIOrDbg();
    } else if (llvm::Instruction *inst = dyn_cast<Instruction>(val)) {
      f = inst->getParent()->getParent();
      insertBefore = inst;
      ++insertBefore;
      while (isa<PHINode>(insertBefore) || isa<llvm::DbgInfoIntrinsic>(insertBefore))
        ++insertBefore;
    } else {
      assert(0);
    }

    // Skip prologue allocas
    while (isa<AllocaInst>(insertBefore))
      ++insertBefore;
    assert(insertBefore);
    assert(!isa<UnreachableInst>(insertBefore));

    // Determine the logging fn to call
    const unsigned valSizeInBits = _kleeModule.targetData->getTypeSizeInBits(val->getType());
    const Type *loggedTy = IntegerType::get(_llvmContext, valSizeInBits);
    Function *logFn = LookupInMap(_icsLogStackVarFns, loggedTy);

    // BitCast to the logged type if necessary
    if (val->getType() != loggedTy) {
      if (isa<PointerType>(val->getType())) {
        val = llvm::CastInst::CreatePointerCast(val, loggedTy, "", insertBefore);
      } else {
        val = new llvm::BitCastInst(val, loggedTy, "", insertBefore);
      }
    }

    // Optimization: we don't need to check the callctx when we log this variable
    // if the container function is always called from a unique context.
    // TODO: We could expand this optimization to check if all possible calling ctxs
    // of kinst get logged, rather than checking if there is just one ctx.
    const bool skipCtxCheck = HasUniqueCallingContext(f, obj->callctx);
    if (DbgInstrumentor && skipCtxCheck) {
      llvm::errs() << "Optimization: Don't need to check callctx for: "
                   << obj->array->name << "\n"
                   << CallingContextPrinter(obj->callctx, "  ");
    }

    // Log the value
    Value *args[] = { ConstantInt::get(_llvmInt32Ty, objId),
                      ConstantInt::get(_llvmInt64Ty, skipCtxCheck ? 0 : callctxId),
                      val };
    CallInst::Create(logFn, args, args+3, "", insertBefore);
  }
}

// We add a single function to log the addresses of used thread-local vars.  This
// function gets called by each new thread just after it spawns.  The function has
// the following form:
//
// void __ics_log_threadlocaladdrs(i32 threadid) {
//   if (threadid == 0) {
//      // log everthing used by T0
//      klee_ics_logbaseptr(obj3, 0, (i8*)@threadlocal3)
//      klee_ics_logbaseptr(obj8, 0, (i8*)@threadlocal8)
//      ...
//      return;
//   }
//   ...
// }

void Instrumentor::InstrumentThreadLocals() {
  // Partition used thread-local arrays by their owner's thread-id
  typedef std::multimap<ThreadUid, const SymbolicObject*> ObjectsByTuidTy;
  ObjectsByTuidTy varsByThread;

  for (UsedArraySetTy::iterator it = _usedArrays.begin(); it != _usedArrays.end(); ++it) {
    const SymbolicObject *obj = _symbolicObjects.find(*it);
    assert(obj);
    if (obj->kind == SymbolicObject::ThreadLocalVariable)
      varsByThread.insert(std::make_pair(obj->tuid, obj));
  }

  // Create the function
  Function *f =
      cast<Function>(_llvmModule.getOrInsertFunction("__ics_log_threadlocaladdrs",
                                                     _llvmVoidTy, _llvmInt32Ty, NULL));
  Argument *arg = f->arg_begin();
  arg->setName("threadId");

  // Create an entry basic block
  BasicBlock *bb = BasicBlock::Create(_llvmContext, "", f);
  const Type *ptrTy = _llvmPtrCharTy;

  // Create a case for each thread
  for (ObjectsByTuidTy::const_iterator
       it = varsByThread.begin(); it != varsByThread.end(); /* nop */) {
    const ThreadUid tuid = it->first;
    bb->setName("testthread_" + llvm::utostr(tuid.tid));
    std::string ifTrueName = "logthread_" + llvm::utostr(tuid.tid);

    // Test if arg==tid
    BasicBlock *ifTrue = BasicBlock::Create(_llvmContext, ifTrueName, f);
    BasicBlock *ifFalse = BasicBlock::Create(_llvmContext, "", f);
    Value *currTid = ConstantInt::get(_llvmInt32Ty, tuid.tid);
    Value *cond =
      llvm::CmpInst::Create(Instruction::ICmp, llvm::CmpInst::ICMP_EQ, arg, currTid, "", bb);
    BranchInst::Create(ifTrue, ifFalse, cond, bb);

    // Copy all objs used by this thread
    for (; it != varsByThread.end() && it->first == tuid; ++it) {
      const SymbolicObject *obj = it->second;
      Value *args[] = { ConstantInt::get(_llvmInt32Ty, LookupObjectId(obj)),
                        new llvm::BitCastInst(const_cast<Value*>(obj->ptr), ptrTy, "", ifTrue) };
      CallInst::Create(_icsLogThreadLocalAddr, args, args+2, "", ifTrue);
    }
    ReturnInst::Create(_llvmContext, ifTrue);

    // Next thread
    bb = ifFalse;
  }

  // Done
  bb->setName("none");
  ReturnInst::Create(_llvmContext, bb);
}

//-----------------------------------------------------------------------------
// Dump global tables
//-----------------------------------------------------------------------------

llvm::GlobalVariable*
Instrumentor::AddGlobalMustNotExist(const Type *ty,
                                    Constant *init,
                                    llvm::StringRef name,
                                    const bool readonly /* = true */) {
  if (_llvmModule.getNamedGlobal(name) != NULL) {
    llvm::errs() << "ERROR: Global variable already exists: " << name << "\n";
    assert(0);
  }
  return new llvm::GlobalVariable(_llvmModule, ty, readonly,
                                  llvm::GlobalVariable::ExternalLinkage, init, name);
}

llvm::GlobalVariable*
Instrumentor::ReplaceGlobalMustExist(const Type *ty,
                                     Constant *init,
                                     llvm::StringRef name,
                                     const bool readonly /* = true */) {
  // Unlink old
  llvm::GlobalVariable *oldVar = _llvmModule.getNamedGlobal(name);
  if (!oldVar) {
    llvm::errs() << "ERROR: Global variable expected to exist but does not: " << name << "\n";
    assert(0);
  }
  oldVar->removeFromParent();
  oldVar->removeDeadConstantUsers();

  // Create new
  llvm::GlobalVariable *newVar =
    new llvm::GlobalVariable(_llvmModule, ty, readonly,
                             llvm::GlobalVariable::InternalLinkage, init, name);
  newVar->setAlignment(oldVar->getAlignment());
  newVar->setUnnamedAddr(oldVar->hasUnnamedAddr());
  newVar->setVisibility(oldVar->getVisibility());
  newVar->setSection(oldVar->getSection());

  // Replace old -> new
  oldVar->replaceAllUsesWith(llvm::ConstantExpr::getPointerCast(newVar, oldVar->getType()));

  // Done
  delete oldVar;
  return newVar;
}

llvm::GlobalVariable*
Instrumentor::AddGlobalFunctionTable(const FunMapByIdTy &map, const char *tabledesc, const char *name) {
  // Get the maximum fn id
  const size_t maxId = map.empty() ? 0 : map.rbegin()->first;

  // Covert "map" to a sorted vector
  std::vector<Constant*> tmp(maxId + 1, NULL);
  for (FunMapByIdTy::const_iterator it = map.begin(); it != map.end(); ++it) {
    if (it->first >= tmp.size()) {
      llvm::errs() << "ERROR: Bad schedule " << tabledesc << " ID: "
                   << it->first << " >= " << tmp.size() << "\n";
      assert(0 && "Bad function ID?");
    }
    tmp[it->first] = it->second;
  }

  // type == tmp[1]->type
  // or if empty, defaults to char*
  const Type *elemTy;
  if (tmp.size() > 1 && tmp[1] != NULL) {
    elemTy = tmp[1]->getType();
  } else {
    elemTy = _llvmPtrCharTy;
  }

  // tmp[0] = nullptr
  assert(tmp[0] == NULL);
  tmp[0] = Constant::getNullValue(elemTy);

  // Warn if there are any missing functions
  // N.B.: Don't (necessarily) need to warn for selector functions as those are elided where unused
  for (size_t k = 1; k <= maxId; ++k) {
    if (!map.count(k)) {
      if (DbgInstrumentor || strcmp(tabledesc, "selector") != 0) {
        llvm::errs() << "WARNING: No " << tabledesc << " for id=" << k << "\n";
      }
      tmp[k] = tmp[0];
    }
  }

  // Now output the global variable
  const ArrayType *tableTy = ArrayType::get(tmp.front()->getType(), tmp.size());
  return ReplaceGlobalMustExist(tableTy, ConstantArray::get(tableTy, tmp), name);
}

// The definitions MAX_THREADS and CACHELINE_SIZE are copied from
// runtime/POSIX/config.h.

static const size_t MAX_THREADS = 16;
static const size_t CACHELINE_SIZE = 64;

// Format of the calling context table table:
//    uint64_t table[]
//
// Where each table[i] maps "short" ctx ids to "long" ctx ids.
// The "long" ids are computed by a hash function.
//
// All contexts should be smaller than MAX_CONTEXT as defined in
// runtime/ConstrainedExec/lib.cpp.  That definition is copied here.

static const size_t MAX_CONTEXT = 64;

std::string Instrumentor::GetNameForCallingContext(const size_t ctxId) {
  char namebuf[64];
  snprintf(namebuf, sizeof namebuf, "__ics_callctx_%lu", ctxId);
  return std::string(namebuf);
}

void Instrumentor::DumpCallingContexts() {
  // Construct the global ctx table
  const IntegerType *ctxTy = _llvmInt64Ty;
  const ArrayType *tableTy = ArrayType::get(ctxTy, _usedCallCtxList.size() + 1);
  std::vector<Constant*> tableEntries;

  // Table[0] is the invalid ctx
  tableEntries.push_back(Constant::getNullValue(ctxTy));

  // Table[1+] are valid entries
  unsigned id = 1;
  for (UsedCallCtxListTy::iterator
       callctx = _usedCallCtxList.begin(); callctx != _usedCallCtxList.end(); ++callctx, ++id) {
    assert(id == LookupCallCtxShortId(*callctx));
    // Check the size
    if ((*callctx)->size() >= MAX_CONTEXT) {
      llvm::errs() << "ERROR: Callctx is too big:\n"
                   << CallingContextPrinter(*callctx, "  ");
      assert(0);
    }
    tableEntries.push_back(ConstantInt::get(ctxTy, LookupCallCtxLongId(*callctx)));
  }

  // Add the global table
  ReplaceGlobalMustExist(tableTy, ConstantArray::get(tableTy, tableEntries), "__ics_callctx_table");
}

// TODO: we should instead (or additionally) dump this table indexed by top-PC
// so that klee_ics_region_marker() doesn't need to search the entire region table.
// For example, if a region marker is called from PC "5" in thread "T1", we need to
// search only those regions in which T1 starts from PC "5".
// TODO: or, it can go into a hash table of some sort
// TODO: or, the runtime library can build that hash table at boot

// Format of the region context table:
//   int64* table[]
//
// Where each table[i] is:
//   { ctxid_0, tid_0, ctxid_1, tid_1, ..., 0 }
//
// This table describes, for each region, the set of calling contexts that
// define the region.  Each entry in the table is null-terminated.  Contexts
// are ordered by context id and are named by a pair (ctxId, threadId), where
// each ctxId is a "long" ctx id.  Note that ctxId comes first because "0" is
// a valid threadId (and if threadId came first, we could not terminate this
// list with a "0").

std::string Instrumentor::GetNameForRegionContext(const size_t regionId) {
  char namebuf[64];
  snprintf(namebuf, sizeof namebuf, "__ics_region_%lu_context", regionId);
  return std::string(namebuf);
}

void Instrumentor::DumpRegionContexts() {
  // Used to sort the global table by r->numericId
  std::map<size_t, Constant*> entries;

  // Dump a variable representing the ParallelRegionId for each region
  // Careful: The array-description for each region should be sorted by ctxId
  for (RegionsIterator r = _regions.begin(); r != _regions.end(); ++r) {
    const size_t regionId = r->second->numericId;
    // Sort contexts by ctxId
    std::vector< std::pair<uint64_t,uint64_t> > sorted;
    for (size_t k = 0; k < r->first.contexts.size(); ++k) {
      const uint64_t ctxId = LookupCallCtxLongId(r->first.contexts[k].strippedctx);
      const ThreadUid tuid = r->first.contexts[k].canonicalTuid;
      assert(tuid.pid == 2);  // same processId assumption as in DumpScheduleFragments()
      sorted.push_back(std::make_pair(ctxId, (uint64_t)tuid.tid));
    }
    std::stable_sort(sorted.begin(), sorted.end());

    // Build the constant array containing (ctxId,threadId) pairs
    std::vector<Constant*> tmp;
    for (size_t k = 0; k < sorted.size(); ++k) {
      tmp.push_back(ConstantInt::get(_llvmInt64Ty, sorted[k].first));
      tmp.push_back(ConstantInt::get(_llvmInt64Ty, sorted[k].second));
    }
    tmp.push_back(_constZero64);

    // Add the global array for this contex
    const ArrayType *ty = ArrayType::get(_llvmInt64Ty, tmp.size());
    std::string name = GetNameForRegionContext(regionId);
    llvm::GlobalVariable *v = AddGlobalMustNotExist(ty, ConstantArray::get(ty, tmp), name);
    Constant *indices[] = { _constZero, _constZero };  // ptr to the first element of the ctx
    entries[regionId] = llvm::ConstantExpr::getGetElementPtr(v, indices, 2);
  }

  // Add the global table
  // Careful: region 0 is invalid
  // Careful: need a terminating null
  const PointerType *entryTy = PointerType::getUnqual(_llvmInt64Ty);
  std::vector<Constant*> tmp;
  tmp.push_back(Constant::getNullValue(entryTy));
  for (std::map<size_t,Constant*>::iterator it = entries.begin(); it != entries.end(); ++it) {
    tmp.push_back(it->second);
  }
  tmp.push_back(Constant::getNullValue(entryTy));

  const ArrayType *tableTy = ArrayType::get(entryTy, tmp.size());
  ReplaceGlobalMustExist(tableTy, ConstantArray::get(tableTy, tmp), "__ics_regionctx_table");
}

// Format of the schedule table:
// There is a set of global variables, each of which describes a single
// schedule.  Pointers to these schedules are returned by the schedule
// selector functions.  Each schedule is a struct like the following:
//
//   struct {
//     int16 numThreads
//     int16 nextThreadLeader
//     int16 nextScheduleSelectorId
//     int16 exitsProgram
//     int16* threadSchedules[] = { &sched_t1, ...}
//   }
//
// There is a separate schedule per-thread.  Each of these schedules is
// a list of unstructured integers that we interpret as follows:
//
//   hbnode ::= ctxid inputObjId? numIncoming (threadId threadTimestamp)*
//   sched  ::= hbnode* 0
//
// Each hbnode describes the CallingContext at which the node applies,
// along with a partial vector-clock describing the time that preceeds
// the hbnode.  For example, the following schedule for T2 has 3 hbnodes:
//
//   { ctxid_1, 3, T1, 5, T3, 2, T4, 8,
//     ctxid_2, 0,
//     ctxid_3, 1, T3, 6,
//     0 }
//
// Some hbnodes represent points in the execution where we read input.
// For such nodes, we store the symbolic id of that input so the input
// buffer can be logged -- the id will be passed to klee_ics_logbaseptr().
// Inputs are relatively rare, so we avoid including the input id when it
// is not present.  We steal the high bit of ctxid as a flag: if that bit
// is set, then the next token is the id of the input buffer, and otherwise
// the next token is numIncoming.  Modifying the above example:
//
//   { ctxid_1 | (1<<15), 4, 2, T1, 5, T3, 2,  // input buffer id = 4
//     ctxid_2 | (1<<15), 3, 0,                // input buffer id = 3
//     ctxid_3, 1, T3, 6,                      // no symbolic input
//     0 }
//
// Not all input is important for schedule selection.  We remove all HB
// nodes that refer to unimportant input (we actually do the removal by
// replacing the node with a "skip" flag -- see uses of SkipHbNodeFlag).
//
// Some schedules are incomplete "fragments" that must be followed by
// another schedule fragment to complete the region.  For these schedule,
// we end with the sentinal "0xffff" rather than "0".

// N.B.: These constants (except MaxCtxId) must be kept in-sync with
// runtime/ConstrainedExec/lib.cpp
const unsigned InputBufferFlagBit = (1<<15);
const unsigned EndFragmentSentinal = 0xffff;
const unsigned SkipHbNodeFlag = 0xfffe;
const unsigned MaxCtxId = (SkipHbNodeFlag & ~InputBufferFlagBit) - 1;

std::string Instrumentor::GetNameForSchedule(const size_t fragId) {
  char namebuf[64];
  snprintf(namebuf, sizeof namebuf, "__ics_schedule_fragment_%lu", fragId);
  return std::string(namebuf);
}

std::string Instrumentor::GetNameForThreadSchedule(const size_t fragId, const ThreadUid tuid) {
  char namebuf[64];
  snprintf(namebuf, sizeof namebuf, "__ics_schedule_fragment_%lu_thread_%u", fragId, tuid.tid);
  return std::string(namebuf);
}

llvm::GlobalVariable* Instrumentor::DumpThreadSchedule(const ScheduleFragment &schedfrag,
                                                       const ThreadUid tuid) {
  const HBGraph &hbGraph = *schedfrag.hbGraph;
  HBGraph::thread_order_iterator itStart = hbGraph.threadOrderBegin(tuid, schedfrag.firstNodeId);
  HBGraph::thread_order_iterator itEnd = hbGraph.threadOrderEnd(tuid, schedfrag.lastNodeId);

  // Is this the last ScheduleFragment?
  // If so, we will skip all inputs in this fragment
  const bool isLast = (schedfrag.lastNodeId == schedfrag.hbGraph->getMaxNodeId());

  // Construct the schedule array
  std::vector<Constant*> tmp;
  for (HBGraph::thread_order_iterator node = itStart; node != itEnd; ++node) {
    // input?
    unsigned inputId = 0;
    if (node->inputObj()) {
      const SymbolicObject *obj = node->inputObj();
      if (_usedArrays.count(obj->array) && !isLast) {
        ScheduleFragment::InputIdsMapTy::const_iterator it = schedfrag.inputIds.find(obj);
        if (it == schedfrag.inputIds.end()) {
          llvm::errs() << "ERROR: No ID found for SymbolicInput:\n";
          SymbolicObject::Dump("  ", obj);
          schedfrag.dumpTxt(std::cerr);
          assert(0);
        }
        inputId = it->second;
        if (DbgInstrumentor) {
          llvm::errs() << "USING SymbolicInput " << obj->array->name << "." << obj->array << " at:\n"
                       << CallingContextPrinter(node->callctx(), "  ");
        }
      } else {
        if (0 && DbgInstrumentor) {
          llvm::errs() << "Ignoring SymbolicInput " << obj->array->name << "." << obj->array << " at:\n"
                       << CallingContextPrinter(node->callctx(), "  ");
        }
        tmp.push_back(GetConstantInt16(SkipHbNodeFlag));
        // Sanity check: the only incoming edges should be from the same thread
        for (HBNode::IncomingListTy::const_iterator
             pred = node->incoming().begin(); pred != node->incoming().end(); ++pred) {
          const HBNode &predNode = hbGraph.getNode(*pred);
          assert(predNode.tuid() == node->tuid());
          assert(predNode.threadOrder() < node->threadOrder());
        }
        continue;
      }
    }
    // callctx + input
    const unsigned ctxId = LookupCallCtxShortId(node->callctx());
    assert(ctxId <= MaxCtxId);
    if (inputId) {
      tmp.push_back(GetConstantInt16(ctxId | InputBufferFlagBit));
      tmp.push_back(GetConstantInt16(inputId));
      STATS["ScheduleHbNodesWithInputs"]++;
    } else {
      tmp.push_back(GetConstantInt16(ctxId));
    }
    // count incoming cross-thread edges
    size_t nedges = 0;
    for (HBNode::IncomingListTy::const_iterator
         pred = node->incoming().begin(); pred != node->incoming().end(); ++pred) {
      const HBNode &predNode = hbGraph.getNode(*pred);
      if (predNode.tuid() == node->tuid()) {
        assert(predNode.threadOrder() < node->threadOrder());
      } else {
        nedges++;
      }
    }
    // stats
    if (nedges) {
      STATS["ScheduleHbNodesWithCrossThreadEdges"]++;
      if (nedges+1 >= hbGraph.getThreadCount()) {
        STATS["ScheduleHbNodesWithMaximalCrossThreadEdges"]++;
      }
    } else {
      STATS["ScheduleHbNodesTrivial"]++;
    }
    // add incoming edges
    tmp.push_back(GetConstantInt16(nedges));
    for (HBNode::IncomingListTy::const_iterator
         pred = node->incoming().begin(); pred != node->incoming().end(); ++pred) {
      const HBNode &predNode = hbGraph.getNode(*pred);
      if (predNode.tuid() != node->tuid()) {
        tmp.push_back(GetConstantInt16(predNode.tuid().tid));
        tmp.push_back(GetConstantInt16(predNode.threadOrder()));
      }
    }
  }

  // Terminator
  if (schedfrag.lastNodeId == hbGraph.getMaxNodeId()) {
    tmp.push_back(GetConstantInt16(0));
  } else {
    tmp.push_back(GetConstantInt16(EndFragmentSentinal));
  }

  // Stats
  if (DbgInstrumentor) {
    llvm::errs() << "Thread-Schedule size: " << tmp.size() << "\n";
  }

  llvm::GlobalVariable *uncached = NULL;
  llvm::GlobalVariable **result = &uncached;
  std::string name = GetNameForThreadSchedule(schedfrag.fragId, tuid);

  // Canonicalize the empty schedule
  if (tmp.size() == 1) {
    if (schedfrag.lastNodeId == hbGraph.getMaxNodeId()) {
      name = "__ics_schedule_fragment_empty_complete";
      result = &_icsEmptyThreadScheduleComplete;
    } else {
      name = "__ics_schedule_fragment_empty_partial";
      result = &_icsEmptyThreadSchedulePartial;
    }
  }

  // Add a global array
  if (!*result) {
    const ArrayType *ty = ArrayType::get(_llvmInt16Ty, tmp.size());
    *result = AddGlobalMustNotExist(ty, ConstantArray::get(ty, tmp), name);
    (*result)->setAlignment(CACHELINE_SIZE);
  }

  return *result;
}

void Instrumentor::DumpScheduleFragments() {
  for (ScheduleFragmentListTy::iterator
       it = _scheduleFragments.begin(); it != _scheduleFragments.end(); ++it) {
    const ScheduleFragment &schedfrag = **it;
    STATS["ScheduleFragments"]++;

    // Sanity check
    assert(schedfrag.fragId);
    assert(schedfrag.nextSelectorId ||
           schedfrag.hbGraph->getNode(schedfrag.lastNodeId).inputObj() ||
           schedfrag.lastNodeId == schedfrag.hbGraph->getMaxNodeId());

    // We support single-threaded procs only, and we expect they have this proc ID
    const process_id_t pid = 2;

    // Determine the maximum threadId used by this *SCHEDULE*.
    thread_id_t maxThread = 0;
    for (hbnode_id_t i = 1; i <= schedfrag.hbGraph->getMaxNodeId(); ++i) {
      const ThreadUid tuid = schedfrag.hbGraph->getNode(i).tuid();
      maxThread = std::max(maxThread, tuid.tid);
      assert(tuid.pid == pid);
    }

    // Type of an array of ptrs to schedules
    const ArrayType *threadsArrayTy =
      ArrayType::get(PointerType::getUnqual(_llvmInt16Ty), maxThread+1);

    // Type of a schedule for this fragment
    // N.B.: This is exactly _icsScheduleTy except that the threads
    // array (last element) has been fixed to a known size.
    std::vector<const Type*> structElems(_icsScheduleTy->element_begin(),
                                         _icsScheduleTy->element_end());
    assert(isa<ArrayType>(structElems.back()));
    structElems.back() = threadsArrayTy;
    const StructType *schedTy = StructType::get(_llvmContext, structElems);

    // Dump a per-thread schedule for each thread
    std::vector<Constant*> tmp;
    for (thread_id_t tid = 0; tid <= maxThread; ++tid) {
      llvm::GlobalVariable *v = DumpThreadSchedule(schedfrag, ThreadUid(tid, pid));
      Constant *indices[] = { _constZero, _constZero };  // ptr to the first element of the sched
      tmp.push_back(llvm::ConstantExpr::getGetElementPtr(v, indices, 2));
    }

    // Add a global var for this schedule
    std::string name = GetNameForSchedule(schedfrag.fragId);
    std::vector<Constant*> vals;
    vals.push_back(ConstantInt::get(_llvmInt16Ty, maxThread+1));
    vals.push_back(ConstantInt::get(_llvmInt16Ty, schedfrag.nextThreadLeader.tid));
    vals.push_back(ConstantInt::get(_llvmInt16Ty, schedfrag.nextSelectorId));
    vals.push_back(ConstantInt::get(_llvmInt16Ty, schedfrag.exitsProgram));
    vals.push_back(ConstantArray::get(threadsArrayTy, tmp));
    if (schedfrag.fragId == 1) {
      ReplaceGlobalMustExist(schedTy, llvm::ConstantStruct::get(schedTy, vals), name);
    } else {
      AddGlobalMustNotExist(schedTy, llvm::ConstantStruct::get(schedTy, vals), name);
    }
  }
}

// Declare the following tables, each of which should be zero-initialized
// and aligned to a cacheline:
//
//   uint8_t*  stackvars8 [2][MAX_THREADS]
//   uint16_t* stackvars16[2][MAX_THREADS]
//   uint32_t* stackvars32[2][MAX_THREADS]
//   uint64_t* stackvars64[2][MAX_THREADS]
//
// Each above table is logically three-dimensional, as in stackvars8[b][tid][objId],
// where "b" is a 0/1 flag used for double-buffering.  However, we don't
// declare them three-dimensional, directly, since they must be declared
// extern in runtime/ConstrainedExec/lib.cpp, and C does not allow us to
// declare a three-dimensional array extern if the sizeof the third dimension
// is unknown.
//
// We declare a variable that contains the size of each table (with units
// "# of elements"):
//
//   uint32_t __ics_stackvars8_sizes[MAX_THREADS]
//   uint32_t __ics_stackvars16_sizes[MAX_THREADS]
//   uint32_t __ics_stackvars32_sizes[MAX_THREADS]
//   uint32_t __ics_stackvars64_sizes[MAX_THREADS]
//
// We also declare these one-dimensional (single-buffered) arrays:
//
//   void* inputptrs[]
//   void* threadlocalptrs[]
//
// CAVEAT: Object ids start at 1, so if we have 5 objects, the maximum id is 5
// and we need an array with *6* elements so that index [5] is valid!

size_t RoundupToCachelineSize(size_t size) {
  if (size % CACHELINE_SIZE != 0) {
    size += CACHELINE_SIZE - (size % CACHELINE_SIZE);
  }
  return size;
}

void Instrumentor::DeclareGlobalLoggingTable(const UsedArrayMapTy &map,
                                             const std::string &name,
                                             const Type *elemTy) {
  // Declare the table
  const ArrayType *ty = ArrayType::get(elemTy, map.size() + 1);
  Constant *init = llvm::ConstantAggregateZero::get(ty);
  ReplaceGlobalMustExist(init->getType(), init, name, false);
}

void Instrumentor::DeclareStackLoggingTable(const UsedArrayMapTy &map,
                                            const std::string &name,
                                            const Type *elemTy) {
  // Declare sub-arrays for each thread/bufferbit
  std::vector<Constant*> tmp[2];
  std::vector<Constant*> sizes;
  for (unsigned tid = 0; tid < MAX_THREADS; ++tid) {
    unsigned size = map.size();
    if (size != 0) {
      size = RoundupToCachelineSize(size + 1);
    }
    for (unsigned b = 0; b < 2; ++b) {
      if (size == 0) {
        tmp[b].push_back(llvm::ConstantPointerNull::get(PointerType::getUnqual(elemTy)));
      } else {
        std::string tmpname = name + "_thread" + llvm::utostr(tid) + "_v" + llvm::utostr(b);
        const ArrayType *ty = ArrayType::get(elemTy, size);
        Constant *zeroinit = llvm::ConstantAggregateZero::get(ty);
        llvm::GlobalVariable *var = AddGlobalMustNotExist(ty, zeroinit, tmpname, false);
        var->setAlignment(CACHELINE_SIZE);
        Constant *indices[] = { _constZero, _constZero };  // ptr to var[0]
        tmp[b].push_back(llvm::ConstantExpr::getGetElementPtr(var, indices, 2));
      }
    }
    sizes.push_back(ConstantInt::get(_llvmInt32Ty, size));
  }

  // The inner arrays (size == [MAX_THREADS])
  const ArrayType *innerTy = ArrayType::get(PointerType::getUnqual(elemTy), MAX_THREADS);
  std::vector<Constant*> outer;
  outer.push_back(ConstantArray::get(innerTy, tmp[0]));
  outer.push_back(ConstantArray::get(innerTy, tmp[1]));

  // Declare the table
  const ArrayType *outerTy = ArrayType::get(innerTy, 2);
  ReplaceGlobalMustExist(outerTy, ConstantArray::get(outerTy, outer), name, false);

  // Declare the table of sizes
  const ArrayType *sizesTy = ArrayType::get(_llvmInt32Ty, MAX_THREADS);
  ReplaceGlobalMustExist(sizesTy, ConstantArray::get(sizesTy, sizes), name + "_sizes", true);
}

void Instrumentor::DeclareLoggingTables() {
  // For the stack vars, we want to declare all of the above tables even
  // if the given program needs just one (e.g., just StackVars64).  So, we
  // ensure that a per-size table exists for each of the above types.
  _usedLocalArraysBySize[8];
  _usedLocalArraysBySize[16];
  _usedLocalArraysBySize[32];
  _usedLocalArraysBySize[64];

  // DEBUG
  if (_usedLocalArraysBySize.size() != 4) {
    llvm::errs() << "???? sz = " << _usedLocalArraysBySize.size() << "\n";
    for (std::map<unsigned, UsedArrayMapTy>::iterator
         it = _usedLocalArraysBySize.begin(); it != _usedLocalArraysBySize.end(); ++it) {
      llvm::errs() << "???? " << it->first << "\n";
    }
    assert(0);
  }

  // Declare the stackvar tables
  for (std::map<unsigned, UsedArrayMapTy>::iterator
       it = _usedLocalArraysBySize.begin(); it != _usedLocalArraysBySize.end(); ++it) {
    DeclareStackLoggingTable(it->second,
                             "__ics_stackvars" + llvm::utostr(it->first),
                             IntegerType::get(_llvmContext, it->first));
  }

  // Declare the thread local ptrs table
  DeclareGlobalLoggingTable(_usedThreadLocalsArrays, "__ics_threadlocalptrs", _llvmPtrCharTy);
}

//-----------------------------------------------------------------------------
// Function replacement
//-----------------------------------------------------------------------------

void Instrumentor::MakeFunctionNop(const char *fnName) {
  if (DbgInstrumentor) {
    llvm::errs() << "Converting function to nop: " << fnName << "\n";
  }

  Function *f = _llvmModule.getFunction(fnName);
  if (!f)
    return;
  f->deleteBody();
  f->setLinkage(llvm::GlobalValue::InternalLinkage);
  ReturnInst::Create(_llvmContext, BasicBlock::Create(_llvmContext, "entry", f));
}

static const char*const StdModelPrefix = "__klee_model_";
static const char*const PthreadModelPrefix = "__klee_pthread_model_";
static const char*const InputModelPrefix = "__klee_input_model_";
static const char*const UclibcWrapperPrefix = "__uClibc_wrap_pthread_";
static const char*const IcsRuntimePrefix = "__ics_runtime_";
static const char*const IcsInputWrapPrefix = "__ics_inputwrap_";
static const char*const IcsLibcallPrefix = "__ics_libcall_";
static const char*const IcsCallSaverPrefix = "__ics_dontdelete_";

// TODO: When unlinking from uclibc, we should be able to delete all pthreads functions
// TODO: Might have to delete some klee intrinsics? (why?)

// Linking and unlinking summary
//
// During linkWithPOSIX
// 1. Replace originalF ("foo") with modelF ("__klee_model_foo", "__klee_pthread_model_foo")
// 2. Add all such originalF to @llvm.used to prevent deletion
//
// Overall linking steps:
// 1. __klee_model_foo          --> foo
// 2. pthread_foo               [delete]
// 3. __uClibc_wrap_pthread_foo --> __klee_pthread_model_pthread_foo  if (!OptIgnoreUclibcSync)
// 4. foo                       --> __ics_runtime_foo
// 5. __klee_pthread_model_foo  --> __ics_runtime_foo
// 6. __klee_input_model_foo    --> __ics_inputwrap_foo
// 7. __ics_libcall_foo         --> foo
// 8. Delete a bunch of unused model functions
//
// Additionally:
// * Before Step 4, we insert a call to the ICS initializer from main
// * For Steps 4 and 5, the sets of "foo" and "__klee_pthread_model_foo"
//   should be disjoint over "foo"
// * In Step 7, it should be true that either:
//     (a) foo() is an undefined a declaration if "foo" is "pthread_*"
//     (b) foo() is an actual function, otherwise
//

void Instrumentor::ReplaceLibFunctions() {
  // DEBUG
  klee::DbgDeleteGlobals = DbgInstrumentor;

  // Replace all "__klee_model" functions with the original uclibc implementations
  klee::unlinkFromPOSIX(&_llvmModule, StdModelPrefix);

  // Unlink from uclibc's pthreads library
  // N.B.: We shouldn't be deleting these entirely, e.g., since the runtime
  // library needs to call functions like pthread_create() and pthread_exit().
  // We leave these definition-less and rely on a future link with an actual
  // pthreads library, such as glibc's libpthread.
  for (llvm::Module::iterator F = _llvmModule.begin(); F != _llvmModule.end(); ++F) {
    if (F->getName().startswith("pthread_") || F->getName().startswith("_pthread_"))
      F->deleteBody();
  }

  // If not ignoring uclibc sync, replace uclibc sync calls with direct calls to our
  // pthread models (these will get replaced with runtime calls in the next linking step).
  if (!OptIgnoreUclibcSync) {
    for (llvm::Module::iterator F = _llvmModule.begin(); F != _llvmModule.end(); ++F) {
      if (!F->getName().startswith(UclibcWrapperPrefix))
        continue;
      std::string modelFName = std::string(PthreadModelPrefix) + std::string("pthread_") +
                               F->getNameStr().substr(strlen(UclibcWrapperPrefix));
      if (DbgInstrumentor) {
        llvm::errs() << "Replacing uclibc wrapper: " << F->getName()
                     << " with " << modelFName << "\n";
      }
      Function *modelF = _llvmModule.getFunction(modelFName);
      assert(modelF);
      F->replaceAllUsesWith(modelF);
    }
  }

  // Call the ICS initializer from main()
  // Must do this before linking in "__ics_runtime" functions
  // TODO: say why
  Function *main = _llvmModule.getFunction("main");
  assert(main);
  Instruction *hack = main->getEntryBlock().getFirstNonPHIOrDbg();
  CallInst::Create(_icsInitialize, "", hack);

  // Link in "__ics_runtime" functions
  // These replace __klee_pthread_model" fns and a few normal/intrinsic functions (e.g., klee_int).
  for (llvm::Module::iterator F = _llvmModule.begin(); F != _llvmModule.end(); ++F) {
    if (!F->getName().startswith(IcsRuntimePrefix))
      continue;
    // This attribute is no longer needed
    F->removeFnAttr(llvm::Attribute::NoInline);
    // Search for the function to replace
    std::string normalFName = F->getNameStr().substr(strlen(IcsRuntimePrefix));
    std::string modelFName = PthreadModelPrefix + normalFName;
    Function *normalF = _llvmModule.getFunction(normalFName);
    Function *modelF = _llvmModule.getFunction(modelFName);
    // Replace
    // Prefer the modelF if it exists (happens for pthread functions, in which
    // case we want to replace calls to our models rather than replace calls to
    // the uclibc stubs).
    Function *replaceeF = modelF ? modelF : normalF;
    if (DbgInstrumentor) {
      if (replaceeF) {
        llvm::errs() << "Replacing for runtime: " << replaceeF->getName()
                     << " with " << F->getName() << "\n";
      } else {
        llvm::errs() << "Ignoring runtime function (unused): " << F->getName() << "\n";
      }
    }
    if (replaceeF) {
      replaceeF->replaceAllUsesWith(F);
    }
  }

  // Link in "__ics_inputwrap" functions
  // These replace __klee_input_model" fns
  for (llvm::Module::iterator F = _llvmModule.begin(); F != _llvmModule.end(); ++F) {
    if (!F->getName().startswith(IcsInputWrapPrefix))
      continue;
    // This attribute is no longer needed
    F->removeFnAttr(llvm::Attribute::NoInline);
    // Search for the function to replace
    std::string normalFName = F->getNameStr().substr(strlen(IcsInputWrapPrefix));
    std::string modelFName = InputModelPrefix + normalFName;
    Function *modelF = _llvmModule.getFunction(modelFName);
    if (DbgInstrumentor) {
      if (modelF) {
        llvm::errs() << "Replacing for runtime: " << modelF->getName()
                     << " with " << F->getName() << "\n";
      } else {
        llvm::errs() << "Ignoring runtime function (unused): " << F->getName() << "\n";
      }
    }
    // Replace
    if (modelF) {
      modelF->replaceAllUsesWith(F);
    }
  }

  // Rename "__ics_libcall" functions
  for (llvm::Module::iterator F = _llvmModule.begin(); F != _llvmModule.end(); ++F) {
    if (!F->getName().startswith(IcsLibcallPrefix))
      continue;
    StringRef name = F->getName().substr(strlen(IcsLibcallPrefix));
    if (name.startswith("pthread_")) {
      if (llvm::GlobalValue *old =
          cast<llvm::GlobalValue>(_llvmModule.getValueSymbolTable().lookup(name))) {
        klee::DeleteGlobal(old);
      }
      if (DbgInstrumentor) {
        llvm::errs() << "Renaming for runtime: " << F->getName()
                     << " to " << name << "\n";
      }
      F->setName(name);
      assert(F->isDeclaration());
    } else {
      Function *originalF = _llvmModule.getFunction(name);
      if (!originalF) {
        llvm::errs() << "ERROR: originalF=" << name << " not found for " << F->getName() << "\n";
        assert(0);
      }
      if (DbgInstrumentor) {
        llvm::errs() << "Replacing for runtime: " << F->getName()
                     << " with " << originalF->getName() << "\n";
      }
      F->replaceAllUsesWith(originalF);
    }
  }

  // Some functions become nops
  // The optimizer will delete these
  MakeFunctionNop("klee_init_env");
  MakeFunctionNop("klee_process_args");

  // Delete global vars and functions that should not be referenced any longer
  // N.B.: Have to remove from llvm.used before we can delete
  StringMatcher todelete;
  todelete.prefixes.push_back(StdModelPrefix);
  todelete.prefixes.push_back(PthreadModelPrefix);
  todelete.prefixes.push_back(InputModelPrefix);
  todelete.prefixes.push_back(UclibcWrapperPrefix);
  todelete.prefixes.push_back(IcsCallSaverPrefix);
  todelete.prefixes.push_back("__klee_force_");
  todelete.exacts.push_back("klee_init_system_for_new_symbolic_region");
  todelete.exacts.push_back("__stupid_hack__");
  klee::RemoveFromUsedList(&_llvmModule, todelete);
  klee::DeleteGlobals(&_llvmModule, todelete);

  // ICS runtime fns don't need to be marked used, either
  klee::RemoveFromUsedList(&_llvmModule, StringMatcher::ForPrefix(IcsRuntimePrefix));
  klee::RemoveFromUsedList(&_llvmModule, StringMatcher::ForPrefix(IcsInputWrapPrefix));
}

void Instrumentor::PromoteLibcFunctionsToIntrinsics() {
  // The list of functions we can promote
  struct Info {
    const char*const name;
    llvm::Intrinsic::ID id;
    const bool isDouble;
  };
  static const Info infos[] = {
#define X(name) {#name, llvm::Intrinsic::name, true}, \
                {#name "f", llvm::Intrinsic::name, false}
    X(cos), X(exp), X(exp2), X(log), X(log10), X(log2), X(pow), X(sin), X(sqrt),
    {0,(llvm::Intrinsic::ID)0,0}
#undef X
  };
  const Type* doubleTys[] = { Type::getDoubleTy(_llvmContext), Type::getDoubleTy(_llvmContext) };
  const Type* floatTys[] = { Type::getFloatTy(_llvmContext), Type::getFloatTy(_llvmContext) };

  // Try to promote to each intrinsic
  for (const Info *i = infos; i->name; ++i) {
    Function *F = _llvmModule.getFunction(i->name);
    if (!F || !F->isDeclaration())
      continue;
    Function *FI =
      llvm::Intrinsic::getDeclaration(&_llvmModule, i->id,
                                      i->isDouble ? doubleTys : floatTys,
                                      (i->id == llvm::Intrinsic::pow) ? 2 : 1);
    if (!FI) {
      llvm::errs() << "WARNING: Could not get intrinsic for " << F->getName() << "\n";
      continue;
    }
    if (DbgInstrumentor) {
      llvm::errs() << "Replacing function " << F->getName()
                   << " with intrinsic " << FI->getName() << "\n";
    }
    F->replaceAllUsesWith(FI);
  }
}

//-----------------------------------------------------------------------------
// Instrument to count instructions
//-----------------------------------------------------------------------------

class CountInstructionsPass : public llvm::FunctionPass {
public:
  static char ID;
  llvm::Value *_countFn;
  std::set<const char*> _ignorePrefixes;

  explicit CountInstructionsPass(llvm::Value *countFn)
    : llvm::FunctionPass(ID), _countFn(countFn) {}

  virtual bool runOnFunction(llvm::Function &f);
};

char CountInstructionsPass::ID = 0;

bool CountInstructionsPass::runOnFunction(Function &f) {
  assert(_countFn);
  const Type *ty = Type::getInt32Ty(f.getContext());

  // Ignore?
  for (std::set<const char*>::const_iterator
       i = _ignorePrefixes.begin(); i != _ignorePrefixes.end(); ++i) {
    if (f.getName().startswith(*i))
      return false;
  }

  // Count
  for (Function::iterator bb = f.begin(); bb != f.end(); ++bb) {
    // Insert counts at:
    //   * before calls
    //   * before terminators (except unreachable)
    // This could be faster

    size_t count = 0;
    for (BasicBlock::iterator i = bb->begin(); i != bb->end(); ++i) {
      ++count;
      if (isa<UnreachableInst>(i))
        break;
      if (isa<llvm::DbgInfoIntrinsic>(i))
        continue;
      if (isa<CallInst>(i) || isa<TerminatorInst>(i)) {
        CallInst::Create(_countFn, ConstantInt::get(ty, count), "", i);
        count = 0;
      }
    }
  }

  return true;
}

} // namespace

//-----------------------------------------------------------------------------
// Main
//-----------------------------------------------------------------------------

static void DumpSource(klee::Executor *exec, llvm::Module *module, const char *filename) {
  CLOUD9_INFO("Dumping source to file '" << filename << "'");
  std::auto_ptr<std::ostream> os(exec->getHandler().openOutputFile(filename));
  std::auto_ptr<llvm::raw_os_ostream> ros(new llvm::raw_os_ostream(*os));
  *ros << *module;
}

static void DumpBitcode(klee::Executor *exec, llvm::Module *module, const char *filename) {
  CLOUD9_INFO("Dumping bitcode to file '" << filename << "'");
  std::auto_ptr<std::ostream> os(exec->getHandler().openOutputFile(filename));
  std::auto_ptr<llvm::raw_os_ostream> ros(new llvm::raw_os_ostream(*os));
  llvm::WriteBitcodeToFile(module, *ros);
}

static void InstrumToCountInstructions(llvm::Module *module) {
  llvm::Value *countFn =
    module->getOrInsertFunction("klee_ics_count_instructions",
                                Type::getVoidTy(module->getContext()),
                                Type::getInt32Ty(module->getContext()),
                                NULL);
  CountInstructionsPass *p = new CountInstructionsPass(countFn);
  p->_ignorePrefixes.insert("__ics_");
  p->_ignorePrefixes.insert("WaitForHbNode");
  p->_ignorePrefixes.insert("WaitAtRegionBarrier");
  p->_ignorePrefixes.insert("WaitForNextFragment");
  p->_ignorePrefixes.insert("AdvanceToNextFragment");
  p->_ignorePrefixes.insert("AdvanceHbNode");
  p->_ignorePrefixes.insert("DoOneSyncOp");
  p->_ignorePrefixes.insert("MyThreadCommonInit");
  p->_ignorePrefixes.insert("UpdateGlobalInstructionCountStats");
  p->_ignorePrefixes.insert("UpdateLocalInstructionCountStats");
  p->_ignorePrefixes.insert("RegionBarrier");
  llvm::PassManager passes;
  passes.add(p);
  passes.run(*module);
}

static void VerifyIR(llvm::Module *module) {
  llvm::errs() << "Verifying IR ...\n";
  llvm::PassManager passes;
  passes.add(llvm::createVerifierPass());
  passes.run(*module);
}

static void DumpStats() {
  llvm::errs() << "===== Instrumentor stats =====\n";
  for (StatsMapTy::const_iterator it = STATS.begin(); it != STATS.end(); ++it) {
    llvm::errs() << it->first << " = " << it->second << "\n";
  }
}

void InstrumentModuleForICS(klee::Executor *executor,
                            const SymbolicObjectTable *symbolicObjects,
                            const SymbolicPtrResolutionTable *resolvedSymbolicPtrs,
                            const RegionsInfoTy &regions) {
  assert(executor);
  assert(symbolicObjects);
  assert(resolvedSymbolicPtrs);

  const klee::KModule *kmodule = executor->getKModule();
  assert(kmodule);

  llvm::errs() << "=====================================================\n"
               << "Instrumentation for ICS\n"
               << "=====================================================\n";

  VerifyIR(kmodule->module);
  Instrumentor instrum(executor, kmodule, regions, symbolicObjects, resolvedSymbolicPtrs);

  //
  // -- 1 --
  // Enumerate all symbolic objects used by the constraints.
  // This represents the set of local variables, etc., that we need
  // to instrument in order to evaluate the constraints at runtime.
  //
  // For arrays allocated for symbolic ptrs, this also transitively
  // walks backwards following "source" links until a heap-root is
  // reached -- this builds a chain of pointers we can dereference
  // to reach the desired heap object.
  //

  for (RegionsIterator r = regions.begin(); r != regions.end(); ++r) {
    const PathManager::ScheduleListTy &schedules = r->second->paths.getSchedules();
    for (SchedulesIterator sched = schedules.begin(); sched != schedules.end(); ++sched) {
      instrum.EnumerateUsedObjects((*sched)->inputConstraint.conciseSummary);
    }
  }

  CLOUD9_INFO("Using " << instrum.getUsedArrays().size() << " of "
                       << symbolicObjects->size() << " symbolic objects");

  //
  // -- 2 --
  // Instrument to track the "shadow stack".
  // This includes call logging to track the calling context, and
  // localvar logging to track the most-recent values of local variables.
  //

  CLOUD9_INFO("Instrumenting for the shadow stack");
  instrum.EnumerateUsedCallCtxs();
  instrum.InstrumentCallStacks();
  instrum.InstrumentLocalVars();
  instrum.InstrumentThreadLocals();

  //
  // -- 3 --
  // Build the schedule trees
  //

  CLOUD9_INFO("Building schedule trees");
  instrum.BuildScheduleTrees();
  instrum.DumpScheduleTrees();
  instrum.ValidateScheduleTrees();
  DumpStats();

  //
  // -- 4 --
  // Dump all constant tables
  //

  CLOUD9_INFO("Dumping constant tables");
  instrum.DumpCallingContexts();
  instrum.DumpRegionContexts();
  instrum.DumpScheduleFragments();
  instrum.DeclareLoggingTables();

  //
  // -- 5 --
  // For each region, emit a function that selects the next schedule.
  // We store these functions in a global table indexed by region ids.
  //

  CLOUD9_INFO("Building snapshot functions");
  instrum.BuildSnapshotFunctions();

  CLOUD9_INFO("Building schedule selectors");
  instrum.BuildScheduleSelectors();

  DumpSource(executor, kmodule->module, "instrumented.ll");
  VerifyIR(kmodule->module);

  //
  // -- 6 --
  // Prepare for the final link stage: rewrite calls to klee intrinsics
  // and klee models so that each call targets a function implemented by
  // either libc or our constraint exec runtime.
  //
  // Careful: once we start deleting things, various ptrs we have lying
  // around (e.g., those in KInstructions) may become invalid.  This
  // includes the alias analysis (via TheAARunner).  Hence, this MUST be
  // the last step before linking.
  //
  // TODO: might not want to call stepInstruction() from Executor::run()
  // or Executor::destroyStates(), since that uses KInstructions and can
  // be called after this?
  //

  CLOUD9_INFO("Dropping analysis references");
  FinalizePrecondSlice();

  CLOUD9_INFO("Removing dormant region markers");
  RemoveDormantRegionMarkers(DbgInstrumentor);

  if (DoCountInstructions) {
    CLOUD9_INFO("Instrumenting program to count instructions");
    InstrumToCountInstructions(kmodule->module);
  }

  CLOUD9_INFO("Replacing lib functions to prepare for link");
  instrum.ReplaceLibFunctions();
  instrum.PromoteLibcFunctionsToIntrinsics();

  DumpSource(executor, kmodule->module, "instrumentedLibReplaced.ll");
  VerifyIR(kmodule->module);

  CLOUD9_INFO("Optimizing the final bitcode");
  klee::OptimizeFull(kmodule->module);
  DumpSource(executor, kmodule->module, "instrumentedLibReplacedOptimized.ll");
  DumpBitcode(executor, kmodule->module, "instrumentedLibReplacedOptimized.bc");
  VerifyIR(kmodule->module);

  DumpStats();
}

}  // namespace ics
