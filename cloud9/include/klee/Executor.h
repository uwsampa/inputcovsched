//===-- Executor.h ----------------------------------------------*- C++ -*-===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Class to perform actual execution, hides implementation details from external
// interpreter.
//
//===----------------------------------------------------------------------===//

#ifndef KLEE_EXECUTOR_H
#define KLEE_EXECUTOR_H

#include "klee/ExecutionState.h"
#include "klee/Interpreter.h"
#include "klee/Expr.h"
#include "klee/ForkTag.h"
#include "klee/Solver.h"
#include "klee/util/Ref.h"
#include "klee/Internal/Module/Cell.h"
#include "klee/Internal/Module/KInstruction.h"
#include "klee/Internal/Module/KModule.h"
#include "llvm/Support/CallSite.h"
#include "cloud9/worker/SymbolicEngine.h"
#include "cloud9/Logger.h"
#include <vector>
#include <string>
#include <map>
#include <set>

struct KTest;

namespace llvm {
  class BasicBlock;
  class BranchInst;
  class CallInst;
  class Constant;
  class ConstantExpr;
  class Function;
  class GlobalValue;
  class Instruction;
  class SCEV;
  class TargetData;
  class Twine;
  class Value;
}

namespace ics {
class DepthFirstICS;
}

namespace klee {  
  class Array;
  struct Cell;
  class DataflowState;
  class ExecutionState;
  class ExternalDispatcher;
  class Expr;
  class InstructionInfoTable;
  struct KFunction;
  struct KInstruction;
  class KInstIterator;
  class KModule;
  class MemoryManager;
  class MemoryObject;
  class ObjectState;
  class PTree;
  class Searcher;
  class SpecialFunctionHandler;
  struct StackFrame;
  class StatsTracker;
  class TimingSolver;
  class TreeStreamWriter;

  bool IsConstantThreadDependent(const llvm::Constant *C);

  /// \todo Add a context object to keep track of data only live
  /// during an instruction step. Should contain addedStates,
  /// removedStates, and haltExecution, among others.

class Executor : public Interpreter, public ::cloud9::worker::SymbolicEngine {
  friend class BumpMergingSearcher;
  friend class MergingSearcher;
  friend class RandomPathSearcher;
  friend class OwningSearcher;
  friend class WeightedRandomSearcher;
  friend class SpecialFunctionHandler;
  friend class StatsTracker;
  friend class ObjectState;

public:
  class Timer {
  public:
    Timer();
    virtual ~Timer();

    /// The event callback.
    virtual void run() = 0;
  };

  typedef std::pair<ExecutionState*,ExecutionState*> StatePair;

private:
  class TimerInfo;

  KModule *kmodule;
  InterpreterHandler *interpreterHandler;
  Searcher *searcher;

  ExternalDispatcher *externalDispatcher;
  TimingSolver *solver;
  MemoryManager *memory;
  std::set<ExecutionState*> states;
  StatsTracker *statsTracker;
  TreeStreamWriter *pathWriter, *symPathWriter;
  SpecialFunctionHandler *specialFunctionHandler;
  std::vector<TimerInfo*> timers;
  PTree *processTree;

  /// Used to track states that have been added during the current
  /// instructions step. 
  /// \invariant \ref addedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::set<ExecutionState*> addedStates;
  /// Used to track states that have been removed during the current
  /// instructions step. 
  /// \invariant \ref removedStates is a subset of \ref states. 
  /// \invariant \ref addedStates and \ref removedStates are disjoint.
  std::set<ExecutionState*> removedStates;

  /// Map of globals to their bound address. This also includes
  /// globals that have no representative object (i.e. functions).
  /// CAVEAT: This does NOT include thread-local variables since their
  /// addresses are not constant across threads.
  std::map<const llvm::GlobalValue*, ref<ConstantExpr> > globalAddresses;

  /// The set of legal function addresses, used to validate function
  /// pointers. We use the actual Function* address as the function address.
  std::set<uint64_t> legalFunctions;

  /// When non-null the bindings that will be used for calls to
  /// klee_make_symbolic in order replay.
  const struct KTest *replayOut;
  /// When non-null a list of branch decisions to be used for replay.
  const std::vector<bool> *replayPath;
  /// The index into the current \ref replayOut or \ref replayPath
  /// object.
  unsigned replayPosition;

  /// Disables forking, instead a random path is chosen. Enabled as
  /// needed to control memory usage. \see fork()
  bool atMemoryLimit;

  /// Disables forking, set by client. \see setInhibitForking()
  bool inhibitForking;

  /// Signals the executor to halt execution at the next instruction
  /// step.
  bool haltExecution;  

  /// Whether implied-value concretization is enabled. Currently
  /// false, it is buggy (it needs to validate its writes).
  bool ivcEnabled;

  /// The maximum time to allow for a single stp query.
  double stpTimeout; 

  llvm::Function* getCalledFunction(llvm::CallSite &cs, ExecutionState &state);
  
  void executeInstruction(ExecutionState &state, KInstruction *ki);

  void printFileLine(ExecutionState &state, KInstruction *ki);

  void run(ExecutionState &initialState);

  // Given a concrete object in our [klee's] address space, add it to 
  // objects checked code can reference.
  MemoryObject *addExternalObject(ExecutionState &state, void *addr, 
                                  unsigned size, bool isReadOnly);

  void initializeGlobalObject(ExecutionState &state, ObjectState *os, 
			      llvm::Constant *c,
			      unsigned offset);
  void initializeGlobals(ExecutionState &state);

  void stepInstruction(ExecutionState &state);
  void updateStates(ExecutionState *current);
  void transferToBasicBlock(llvm::BasicBlock *dst, 
			    llvm::BasicBlock *src,
			    ExecutionState &state);

  void callExternalFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector< ref<Expr> > &arguments);

  void callUnmodelledFunction(ExecutionState &state,
                            KInstruction *target,
                            llvm::Function *function,
                            std::vector<ref<Expr> > &arguments);

public:
  /// Allocate and bind a new object in a particular state. NOTE: This
  /// function may fork.
  ///
  /// \param target Value at which to bind the base address of the new
  /// object.
  ///
  /// \param isLocal Flag to indicate if the object should be
  /// automatically deallocated on function return (this also makes it
  /// illegal to free directly).
  ///
  /// \param zeroMemory Flag to indicate if the object should be
  /// automatically zero-initialized.
  ///
  /// \param copyFrom If non-zero and the allocation succeeds,
  /// initialize the new object from the given one. The initialized
  /// bytes will be the minimum of the size of the old and new objects,
  /// with remaining bytes initialized as specified by zeroMemory.
  /// N.B.: This can be used to implement realloc case, but in that
  /// case, the CALLER is responsible for freeing the old object.
  void executeAlloc(ExecutionState &state,
                    KInstruction *target,
                    ref<Expr> size,
                    ref<Expr> alignment,
                    const bool isLocal,
                    const bool zeroMemory = false,
                    const ObjectState *copyFrom = NULL,
                    ref<Expr> defaultSymbolicBaseExpr = NULL);

  /// Free the given address with checking for errors. If target is
  /// given it will be bound to 0 in the resulting states (this is a
  /// convenience for realloc). Note that this function can cause the
  /// state to fork and that \ref state cannot be safely accessed
  /// afterwards.
  void executeFree(ExecutionState &state,
                   ref<Expr> address,
                   const llvm::Value *ptr);
 
  void executeCall(ExecutionState &state,
                   KInstruction *ki,
                   llvm::Function *f,
                   std::vector< ref<Expr> > &arguments);
 
  void executeIndirectCall(ExecutionState &state,
                           KInstruction *ki,
                           ref<Expr> address,
                           std::vector< ref<Expr> > &arguments);

  bool setupArgsForCall(ExecutionState &state,
                        llvm::Function *f,
                        std::vector< ref<Expr> > &arguments,
                        StackFrame &sf);
                   
  // do address resolution / object binding / out of bounds checking
  // and perform the operation
  void executeMemoryOperation(ExecutionState &state,
                              bool isWrite,
                              const llvm::Value *ptr,
                              ref<Expr> address,
                              ref<Expr> value /* undef if read */,
                              KInstruction *target /* undef if write */);

  void executeMakeSymbolic(ExecutionState &state, const MemoryObject *mo,
      const std::string &name, bool shared, KInstruction *call);

  void executeEvent(ExecutionState &state, EventClass type, long int value);

  // Fork current and return states in which condition holds / does
  // not hold, respectively. One of the states is necessarily the
  // current state, and one of the states may be null.
  StatePair fork(ExecutionState &current, ref<Expr> condition, bool isInternal, ForkClass reason);
  StatePair fork(ExecutionState &current, ForkClass reason);
  ForkTag getForkTag(ExecutionState &current, ForkClass reason);

  /// Create a new state where each input condition has been added as
  /// a constraint and return the results. The input state is included
  /// as one of the results.  Note that the output map may included
  /// NULL pointers for states which correspond to infeasible conditions.
  void forkForSwitch(ExecutionState &state,
                     const std::map<llvm::BasicBlock*, ref<Expr> > &conditions,
                     std::map<llvm::BasicBlock*, ExecutionState*> &result,
                     ForkClass reason);

  /// Add the given (boolean) condition as a constraint on state. This
  /// function is a wrapper around the state's addConstraint function
  /// which also manages manages propogation of implied values and
  /// validity checks, 
  void addConstraint(ExecutionState &state, ref<Expr> condition, ForkClass reason);

  void addThreadIdConstraints(ExecutionState &state);
  void addThreadIdConstraintsFor(ExecutionState &state, const ThreadUid tuid);

  const Cell& eval(KInstruction *ki, unsigned index, 
                   ExecutionState &state);

  Cell& getArgumentCell(ExecutionState &state, KFunction *kf, unsigned index) {
    return getArgumentCell(state.stack().back(), kf, index);
  }

  Cell& getArgumentCell(StackFrame &sf, KFunction *kf, unsigned index) {
    return sf.locals[kf->getArgRegister(index)];
  }

  Cell& getDestCell(ExecutionState &state, KInstruction *target) {
    return state.stack().back().locals[target->dest];
  }

  Cell& getIndexCell(ExecutionState &state, unsigned index) {
    return state.stack().back().locals[index];
  }

  void bindLocal(KInstruction *target, 
                 ExecutionState &state, 
                 ref<Expr> value);
  void bindArgument(KFunction *kf, 
                    unsigned index,
                    ExecutionState &state,
                    ref<Expr> value);

  ObjectState *bindObjectInState(ExecutionState &state, const MemoryObject *mo,
                                 bool isLocal, const Array *array = 0);

  ref<klee::ConstantExpr>
  evalConstantExprUncached(llvm::ConstantExpr *ce,
                           const ExecutionState::ThreadLocalsTableTy *threadLocals);
  ref<klee::ConstantExpr>
  evalConstantUncached(llvm::Constant *c,
                       const ExecutionState::ThreadLocalsTableTy *threadLocals);

  struct SCEVEvaluator;
  ref<Expr> evalSCEV(ExecutionState &state, const llvm::SCEV *scev);

  /// Return a unique constant value for the given expression in the
  /// given state, if it has one (i.e. it provably only has a single
  /// value). Otherwise return the original expression.
  ref<Expr> toUnique(const ExecutionState &state, ref<Expr> e);

  /// Return a constant value for the given expression, forcing it to
  /// be constant in the given state by adding a constraint if
  /// necessary. Note that this function breaks completeness and
  /// should generally be avoided.
  ///
  /// \param purpose An identify string to printed in case of concretization.
  ref<klee::ConstantExpr> toConstant(ExecutionState &state, ref<Expr> e, 
                                     ForkClass reason, const char *reasonStr,
                                     bool canTimeout = false);

  /// Evalute the given boolean expression. Returns 'Unknown' on solver failure.
  Solver::Validity evaluateValidity(const ExecutionState &state, ref<Expr> e);

  /// Bind a constant value for e to the given target.
  void executeGetValue(ExecutionState &state, ref<Expr> e, KInstruction *target);

  /// Get textual information regarding a memory address.
  std::string getAddressInfo(ExecutionState &state, ref<Expr> address) const;

  // Called on [for now] concrete reads, replaces constant with a symbolic
  // Used for testing.
  ref<Expr> replaceReadWithSymbolic(ExecutionState &state, ref<Expr> e);

  // For initializing thread-locals
  void initializeThreadLocalsForThread(ExecutionState &state, const ThreadUid thread);

  // remove state from queue and delete
  bool terminateState(ExecutionState &state, bool silenced);
  // call exit handler and terminate state
  void terminateStateEarly(ExecutionState &state, const llvm::Twine &message);
  // call exit handler and terminate state
  void terminateStateOnExit(ExecutionState &state);
  // call error handler and terminate state
  void terminateStateOnError(ExecutionState &state, 
                             const llvm::Twine &message,
                             const char *suffix,
                             const llvm::Twine &longMessage="");

private:
  // call error handler and terminate state, for execution errors
  // (things that should not be possible, like illegal instruction or
  // unlowered instrinsic, or are unsupported, like inline assembly)
  void terminateStateOnExecError(ExecutionState &state, 
                                 const llvm::Twine &message,
                                 const llvm::Twine &info="") {
    terminateStateOnError(state, message, "exec.err", info);
  }

  /// bindModuleConstants - Initialize the module constant table.
  void bindModuleConstants();

  template <typename TypeIt>
  void computeOffsets(KGEPInstruction *kgepi, TypeIt ib, TypeIt ie);

  /// bindInstructionConstants - Initialize any necessary per instruction
  /// constant values.
  void bindInstructionConstants(KInstruction *KI);

  void handlePointsToObj(ExecutionState &state, 
                         KInstruction *target, 
                         const std::vector<ref<Expr> > &arguments);

  void doImpliedValueConcretization(ExecutionState &state,
                                    ref<Expr> e,
                                    ref<ConstantExpr> value);

  /// Add a timer to be executed periodically.
  ///
  /// \param timer The timer object to run on firings.
  /// \param rate The approximate delay (in seconds) between firings.
  void addTimer(Timer *timer, double rate);

  void initTimers();
  void processTimers(ExecutionState *current,
                     double maxInstTime);
  void resetTimers();

  /// Pthread create needs a specific StackFrame instead of the one of the current state
  void bindArgumentToPthreadCreate(KFunction *kf, unsigned index, 
				   StackFrame &sf, ref<Expr> value);

  /// Finds the functions coresponding to an address.
  /// For now, it only support concrete values for the thread and function pointer argument.
  /// Can be extended easily to take care of symbolic function pointers.
  /// \param address address of the function pointer
  KFunction* resolveFunction(ref<Expr> address);

public:
  //pthread handlers
  void executeThreadCreate(ExecutionState &state, thread_id_t tid,
      ref<Expr> start_function, ref<Expr> arg);

  void executeThreadExit(ExecutionState &state);
  
  void executeProcessExit(ExecutionState &state);
  
  void executeProcessFork(ExecutionState &state, KInstruction *ki,
      process_id_t pid);
  
  bool schedule(ExecutionState &state, bool yield);
  
  void executeThreadNotifyOne(ExecutionState &state,
                              const WaitingListKey &wl,
                              hbnode_id_t sourceHbNode,
                              hbnode_id_t sinkHbNode,
                              ref<Expr> expectedQueueSize,
                              bool icsRegionMarker);
  void executeThreadNotifyAll(ExecutionState &state,
                              const WaitingListKey &wl,
                              hbnode_id_t sourceHbNode,
                              hbnode_id_t sinkHbNode,
                              ref<Expr> expectedQueueSize,
                              bool icsRegionMarker);

  void filterSymbolicWaitingListByKey(ExecutionState &state,
                                      const WaitingListKey &wl,
                                      std::set<ThreadUid> *mayAliasSet,
                                      std::set<ThreadUid> *mustAliasSet,
                                      bool filterMustFromMay);

  void executeFork(ExecutionState &state, KInstruction *ki, ForkClass reason);

  ref<Expr> simplifyAddressOffset(ExecutionState &state, ref<Expr> address,
      ref<Expr> offset, const MemoryObject *mo);

  void executeSingleMemoryOperation(ExecutionState &state, bool isWrite,
      ref<Expr> offset, Expr::Width width, const MemoryObject *mo, const ObjectState* os,
      ref<Expr> value /* undef if read */, KInstruction *target /* undef if write */,
      ref<Expr> *currStoredValue = NULL);

  // Dealing with symbolic pointers
  bool pointerMayAliasObject(const llvm::Value *pointer, const MemoryObject *obj);
  bool pointerMayAliasFunction(const llvm::Value *pointer, const llvm::Function *fn,
                               const std::set<const llvm::Function*> &aliases);

  void resolveSymbolicPointer(ExecutionState &state, const llvm::Value *const pointer,
      const ref<Expr> targetAddress, ObjectPair *outPrimaryObj = NULL);

  // Partial eval
  ref<Expr> partialEvalLoad(ExecutionState &state,
                            DataflowState *dstate,
                            ref<Expr> address,
                            const llvm::LoadInst *load);
  void partialEvalStore(ExecutionState &state,
                        DataflowState *dstate,
                        ref<Expr> address,
                        ref<Expr> value,
                        const llvm::StoreInst *store);
  void partialEvalStore(ExecutionState &state,
                        DataflowState *dstate,
                        ref<Expr> address,
                        ref<Expr> value,
                        const Expr::Width bits,
                        const llvm::Value *pointer);

  bool partialEvalIncomingBranch(ExecutionState &state,
                                 DataflowState *dstate,
                                 llvm::BasicBlock *prevBB,
                                 llvm::BasicBlock *nextBB,
                                 KInstruction *ki);

  void partialEvalCall(ExecutionState &state,
                       DataflowState *dstate,
                       KInstruction *ki,
                       llvm::Function *target,
                       std::vector< ref<Expr> > &arguments,
                       ref<Expr> *returnValue);

  void partialEvalCall(ExecutionState &state,
                       DataflowState *dstate,
                       KInstruction *ki,
                       StackFrame *nextsf,
                       bool isFinalInstForThread);

  void partialEvalInstruction(ExecutionState &state,
                              DataflowState *dstate,
                              const llvm::BasicBlock *uniquePredBB);

  void partialEvalUnrollLoop(ExecutionState &state,
                             llvm::Loop *const loop,
                             llvm::BasicBlock *const backedgeBB,
                             ref<Expr> tripCount,
                             const DataflowState &initial,
                             DataflowState *final);

  void partialEvalStackFrame(ExecutionState &state,
                             KInstruction *kend,
                             ref<Expr> *returnValue,
                             const DataflowState &initial,
                             DataflowState *final);

  void partialEvalThreadStack(ExecutionState &state,
                              Thread &thread,
                              DataflowState *dstate);

  void partialEvalComputeFnSummary(ExecutionState &state, const llvm::Function *f);
  void partialEvalComputeFnSummariesPostorder(ExecutionState &state, const llvm::Function *leader);
  void partialEvalComputeFnSummaries(ExecutionState &state);
  void stepInStatePartialEval(ExecutionState &state);

public:
  Executor(const InterpreterOptions &opts, InterpreterHandler *ie);
  virtual ~Executor();

  InterpreterHandler& getHandler() const {
    return *interpreterHandler;
  }

  TimingSolver* getTimingSolver() const {
    return solver;
  }
  MemoryManager* getMemoryManager() const {
    return memory;
  }

  ref<ConstantExpr> evalConstant(llvm::Constant *c,
                                 const ExecutionState::ThreadLocalsTableTy *threadLocals);

  virtual void setPathWriter(TreeStreamWriter *tsw) {
    pathWriter = tsw;
  }
  virtual void setSymbolicPathWriter(TreeStreamWriter *tsw) {
    symPathWriter = tsw;
  }      

  virtual void setReplayOut(const struct KTest *out) {
    assert(!replayPath && "cannot replay both buffer and path");
    replayOut = out;
    replayPosition = 0;
  }

  virtual void setReplayPath(const std::vector<bool> *path) {
    assert(!replayOut && "cannot replay both buffer and path");
    replayPath = path;
    replayPosition = 0;
  }

  virtual const llvm::Module *
  setModule(llvm::Module *module, const ModuleOptions &opts);
  
  const KModule* getKModule() const {return kmodule;} 

  virtual void runFunctionAsMain(llvm::Function *f,
                                 int argc,
                                 char **argv,
                                 char **envp);

  void prepareToPartialEvalState(ExecutionState &state);

  /*** Runtime options ***/
  
  virtual void setHaltExecution(bool value) {
    haltExecution = value;
  }

  virtual void setInhibitForking(bool value) {
    inhibitForking = value;
  }

  /*** State accessor methods ***/

  virtual unsigned getPathStreamID(const ExecutionState &state);

  virtual unsigned getSymbolicPathStreamID(const ExecutionState &state);

  virtual void getConstraintLog(const ExecutionState &state,
                                std::string &res,
                                bool asCVC = false);

  virtual bool getSymbolicSolution(const ExecutionState &state, 
                                   Assignment &result);

  virtual void getCoveredLines(const ExecutionState &state,
                               std::map<const std::string*, std::set<unsigned> > &res);

  Expr::Width getWidthForLLVMType(const llvm::Type *type) const;

  /*** Cloud9 symbolic execution engine methods ***/

  virtual ExecutionState *createRootState(llvm::Function *f);
  virtual void initRootState(ExecutionState *state, int argc, char **argv, char **envp);

  virtual void stepInState(ExecutionState *state);

  virtual void destroyStates();

  virtual void destroyState(klee::ExecutionState *state);
  virtual void discardState(klee::ExecutionState *state);

  virtual klee::Searcher *initSearcher(klee::Searcher *base);

  virtual klee::KModule *getModule() {
	  return kmodule;
  }

  bool isLegalFunction(uint64_t addr) const {
    return legalFunctions.count(addr) != 0;
  }

  //Hack for dynamic cast in CoreStrategies, TODO Solve it as soon as possible
  static bool classof(const SymbolicEngine* engine){ return true; }
};
  
} // End klee namespace

#endif
