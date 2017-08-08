//===- DataStructureAA.cpp - Data Structure Based Alias Analysis ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass uses the top-down data structure graphs to implement a simple
// context sensitive alias analysis.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "dsaa"

#include "dsa/DataStructureAA.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/Support/CommandLine.h"
#include <stack>

using namespace llvm;

static const Function* getFunctionForValue(const Value *V);

namespace {
// Register the pass
RegisterPass<DSAA> X("ds-aa", "Data Structure Alias Analysis");

// Register as an implementation of AliasAnalysis
RegisterAnalysisGroup<AliasAnalysis> Y(X);

// Command line opts
cl::opt<bool> ThreadSafeDSAA("ds-aa-thread-safe",
         cl::desc("Enable thread-safe analysis for DSAA."),
         cl::Hidden, cl::init(true));

// debugging
cl::opt<bool> DumpEscapingNodes("ds-aa-dump-escaping-nodes", cl::Hidden, cl::init(false));
cl::opt<bool> DumpMemoryBarriers("ds-aa-dump-memory-barriers", cl::Hidden, cl::init(false));
cl::opt<bool> DumpCallGraph("ds-aa-dump-callgraph", cl::Hidden, cl::init(false));
cl::opt<bool> DumpInverseCallGraph("ds-aa-dump-inverse-callgraph", cl::Hidden, cl::init(false));
cl::opt<bool> DumpCallGraphSCCs("ds-aa-dump-callgraph-sccs", cl::Hidden, cl::init(false));
cl::opt<bool> DumpFinalDSGraphs("ds-aa-dump-final-dsgraphs", cl::Hidden, cl::init(false));
cl::opt<bool> DumpFinalSteensGraph("ds-aa-dump-final-steensgraph", cl::Hidden, cl::init(false));
cl::opt<std::string> DumpFinalDSGraphFor("ds-aa-dump-final-dsgraph-for", cl::Hidden, cl::init(""));
}

char DSAA::ID;

ModulePass *llvm::createDSAAPass() { return new DSAA(); }

DSAA::DSAA()
  : ModulePass(ID), TD(NULL), BU(NULL), Steens(NULL), valid(false)
{}

DSAA::~DSAA() {
  releaseMemory();
}

void DSAA::releaseMemory() {
  valid = false;
  StdLib = NULL;
  TD = NULL;
  BU = NULL;
  Steens = NULL;
  invalidateCache();
}

void DSAA::invalidateCache() {
  MapCS = CallSite();
  CallerCalleeMap.clear();
}

void DSAA::getAnalysisUsage(AnalysisUsage &AU) const {
  AliasAnalysis::getAnalysisUsage(AU);
  // Does not transform code
  AU.setPreservesAll();
  AU.addRequiredTransitive<SteensgaardDataStructures>();  // for alias
  AU.addRequiredTransitive<TDDataStructures>();           // for alias, getModRefInfo
  AU.addRequiredTransitive<BUDataStructures>();           // for getModRefInfo
  AU.addRequiredTransitive<StdLibDataStructures>();       // for getModRefInfo
}

bool DSAA::runOnModule(Module &M) {
  InitializeAliasAnalysis(this);

  assert(!valid && "DSAA executed twice without being invalidated?");

  StdLib = &getAnalysis<StdLibDataStructures>();
  TD = &getAnalysis<TDDataStructures>();
  BU = &getAnalysis<BUDataStructures>();
  Steens = &getAnalysis<SteensgaardDataStructures>();
  buildBottomUpCallGraph();

  if (ThreadSafeDSAA) {
    enumerateMemoryBarriers();
    enumerateEscapingNodes(M);
  }

  valid = true;

  // DEBUG
  if (DumpEscapingNodes)
    dumpEscapingNodes(errs(), M);

  if (DumpMemoryBarriers)
    dumpMemoryBarriers(errs());

  if (DumpCallGraph)
    const_cast<DSCallGraph&>(getCallGraph()).dump();

  if (DumpInverseCallGraph)
    dumpBottomUpCallGraph(errs());

  if (DumpCallGraphSCCs) {
    dumpCallGraphSCCs(errs());
  }

  if (DumpFinalDSGraphs) {
    errs() << "======== TD Graph =========\n";
    TD->print(errs(), &M);
  }

  if (DumpFinalSteensGraph) {
    errs() << "======== Steens Graph =========\n";
    Steens->print(errs(), &M);
  }

  if (!DumpFinalDSGraphFor.empty()) {
    const Function *F = M.getFunction(DumpFinalDSGraphFor);
    if (!F) {
      errs() << "ERROR: Unknown function " << DumpFinalDSGraphFor << "\n";
    } else if (!TD->hasDSGraph(*F)) {
      errs() << "ERROR: No DSGrpah for function " << DumpFinalDSGraphFor << "\n";
    } else {
      errs() << "======== TD Graph for " << F->getNameStr() <<  "=========\n";
      TD->getDSGraph(*F)->writeGraphToFile(errs(), F->getNameStr());
    }
  }

  return false;
}

// Build a mapping from Function -> [Callers].
void DSAA::buildBottomUpCallGraph() {
  const DSCallGraph &CG = getCallGraph();
  DenseSet<const Function*> visited;

  BottomUpCallGraph.clear();
  TopDownCallGraph.clear();

  for (DSCallGraph::root_iterator r = CG.root_begin(); r != CG.root_end(); ++r) {
    buildBottomUpCallGraph(CG, *r, &visited);
  }
}

void DSAA::buildBottomUpCallGraph(const DSCallGraph &CG,
                                  const Function *LeaderF,
                                  DenseSet<const Function*> *visited) {
  if (LeaderF->isDeclaration())
    return;

  // Mark SCC visited
  if (visited->count(LeaderF))
    return;
  visited->insert(LeaderF);

  // Process all call instructions for all functions in this SCC.
  // Note thate we can't just invert CG.SimpleCallees because we want to
  // include functions from StdLibInfo which are not included in DSCallGraph.
  for (DSCallGraph::scc_iterator F = CG.scc_begin(LeaderF),
                                FE = CG.scc_end(LeaderF); F != FE; ++F) {
    for (Function::const_iterator BB = F->begin(); BB != F->end(); ++BB) {
      for (BasicBlock::const_iterator I = BB->begin(); I != BB->end(); ++I) {
        if (!isa<CallInst>(I) && !isa<InvokeInst>(I))
          continue;
        CallSite CS(const_cast<Instruction*>(cast<Instruction>(I)));
        // Direct call?
        const Function* Callee = dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
        if (Callee) {
          BottomUpCallGraph[Callee].insert(*F);
          TopDownCallGraph[*F].insert(Callee);
          if (!Callee->getIntrinsicID())
            buildBottomUpCallGraph(CG, CG.sccLeader(Callee), visited);
          continue;
        }
        // Process all potential call targets
        for (DSCallGraph::callee_iterator SCC = CG.callee_begin(CS),
                                         SCCE = CG.callee_end(CS); SCC != SCCE; ++SCC) {
          for (DSCallGraph::scc_iterator T = CG.scc_begin(*SCC),
                                        TE = CG.scc_end(*SCC); T != TE; ++T) {
            BottomUpCallGraph[*T].insert(*F);
            TopDownCallGraph[*F].insert(*T);
          }
          buildBottomUpCallGraph(CG, *SCC, visited);
        }
      }
    }
  }
}

void DSAA::dumpBottomUpCallGraph(raw_ostream &os) {
  for (CallerMapTy::iterator
       FI = BottomUpCallGraph.begin(); FI != BottomUpCallGraph.end(); ++FI) {
    const Function *F = FI->first;

    os << "BottomUpCallGraph[" << F->getNameStr() << "]";

    for (CallerMapTy::mapped_type::iterator
         Caller = FI->second.begin(); Caller != FI->second.end(); ++Caller) {
      os << " " << (*Caller)->getNameStr();
    }
    os << "\n";
  }
}

void DSAA::dumpCallGraphSCCs(raw_ostream &os) {
  const DSCallGraph &CG = getCallGraph();
  for (DSCallGraph::flat_key_iterator F = CG.flat_key_begin(),
                                     FE = CG.flat_key_end(); F != FE; ++F) {
    DSCallGraph::scc_iterator SCC = CG.scc_begin(*F);
    DSCallGraph::scc_iterator SCCE = CG.scc_end(*F);
    if (++SCC == SCCE)
      continue;
    os << "SCC:";
    for (SCC = CG.scc_begin(*F); SCC != SCCE; ++SCC) {
      if (*SCC == *F)
        os << " [" << (*SCC)->getNameStr() << "]";
      else
        os << " " << (*SCC)->getNameStr();
    }
    os << "\n";
  }
}

// Compute set of functions which may invoke a memory barrier.
// Recursive traversal of the call graph, bottom-up, starting at memory barriers.
void DSAA::enumerateMemoryBarriers() {
  std::stack<const Function*> stack;
  MemoryBarriers.clear();

  // For each function
  for (CallerMapTy::iterator
       FI = BottomUpCallGraph.begin(); FI != BottomUpCallGraph.end(); ++FI) {
    const Function *F = FI->first;

    // Memory barrier?
    if (!isMemoryBarrierPrimitive(F))
      continue;

    MemoryBarriers.insert(F);

    // Go bottom-up
    stack.push(F);
    while (!stack.empty()) {
      F = stack.top();
      stack.pop();

      CallerMapTy::iterator CI = BottomUpCallGraph.find(F);
      if (CI == BottomUpCallGraph.end())
        continue;
      for (CallerMapTy::mapped_type::iterator
           Caller = CI->second.begin(); Caller != CI->second.end(); ++Caller) {
        if (MemoryBarriers.count(*Caller))
          continue;
        MemoryBarriers.insert(*Caller);
        stack.push(*Caller);
      }
    }
  }
}

bool DSAA::isMemoryBarrierPrimitive(const Function *F) {
  // Functions with definitions are not primitives
  if (!F->isDeclaration())
    return false;

  // Known memory barrier?
  const StdLibInfo::LibAction* action =
    StdLib->getStdLibInfo().getLibActionForFunction(F);
  if (action)
    return action->memoryBarrier;

  // Incomplete call site?
  if (TD->getCallGraph().called_from_incomplete_site(F))
    return true;

  return false;
}

void DSAA::dumpMemoryBarriers(raw_ostream &os) {
  os << "MemoryBarriers: [ ";
  for (std::set<const Function*>::iterator
       it = MemoryBarriers.begin(); it != MemoryBarriers.end(); ++it)
    os << (*it)->getNameStr() << " ";
  os << "]\n";
}

// Compute thread-escaping nodes.
void DSAA::enumerateEscapingNodes(Module &M) {
  EscapingNodes.clear();
  DSGraph *G = Steens->getResultGraph();

  //
  // -- 1 --
  // All nodes reachable from non-thread-local globals are escaping
  //

  std::vector<const GlobalValue*> Globals;

  for (DSGraph::node_const_iterator N = G->node_begin(); N != G->node_end(); ++N) {
    if (N->isUnknownNode()) {
      N->markReachableNodes(EscapingNodes);
      continue;
    }
    if (N->isGlobalNode()) {
      Globals.clear();
      N->addFullGlobalsList(Globals);

      bool threadlocal = true;
      for (size_t i = 0; i < Globals.size(); ++i) {
        const GlobalVariable *V = dyn_cast<GlobalVariable>(Globals[i]);
        if (!V || !V->isThreadLocal()) {
          threadlocal = false;
          break;
        }
      }

      if (!threadlocal)
        N->markReachableNodes(EscapingNodes);
    }
  }

  //
  // -- 2 --
  // All nodes reachable from a thread creation function are escaping
  //

  for (std::list<DSCallSite>::const_iterator
       CS = G->getCreateThreadCalls().begin(); CS != G->getCreateThreadCalls().end(); ++CS) {
    if (CS->getNumPtrArgs() < 1)
      continue;
    CS->getPtrArg(0).getNode()->markReachableNodes(EscapingNodes);
  }

  //
  // -- 3 --
  // All nodes reachable from the arguments or return values of external
  // functions are escaping.  Note that this is already computed by the
  // `External` flag of a node, so we do not need to do anything here.
  //
}

void DSAA::dumpEscapingNodes(raw_ostream &os, Module &M) {
  std::vector<const Value*> Values;

  os << "EscapingNodes: [\n";
  for (DenseSet<const DSNode*>::iterator
       N = EscapingNodes.begin(); N != EscapingNodes.end(); ++N) {
    Values.clear();
    (*N)->addValueList(Values);
    for (size_t i = 0; i < Values.size(); ++i) {
      const Value *V = Values[i];
      os << "  ";
      const Function *F = getFunctionForValue(V);
      if (F) {
        os << "{" << F->getNameStr() << ": ";
        WriteAsOperand(os, V, false, &M);
        os << "}";
      } else {
        WriteAsOperand(os, V, false, &M);
      }
    }
    os << "\n";
  }
  os << "]\n";
}

bool DSAA::mightValueEscapeThread(const Value *V) {
  DSGraph *G = Steens->getResultGraph();

  // Stupid hacky case: any of the "ignored" values might escape
  if (Steens->isIgnoredValue(V))
    return true;

  // The normal case
  const DSGraph::ScalarMapTy &GSM = G->getScalarMap();
  DSGraph::ScalarMapTy::const_iterator I = GSM.find(V);

  // Stupid DSGraphs sometimes remove nodes from the graph, e.g. for globals that
  // alias with nothing.  I think I fixed that bug, but just in case, here's a quick
  // test for global values (these obviously must escape).
  if (I == GSM.end()) {
    V = V->stripPointerCasts();
    if (isa<GlobalValue>(V)) {
      if (const GlobalVariable *var = dyn_cast<GlobalVariable>(V))
        return !var->isThreadLocal();
      return true;
    }
    return false;
  }

  // Is the node escaping?
  // Need to check if this node is external (see comment in enumerateEscapingNodes)
  DSNode *N = I->second.getNode();
  if (N->isExternalNode() || N->isIncompleteNode() || EscapingNodes.count(N) > 0)
    return true;

  return false;
}

// Enumerate possible callees for a callsite
void DSAA::getCallTargets(const CallSite &CS,
                          std::vector<const Function*> *targets) const {
  assert(targets);
  if (const Function *F = CS.getCalledFunction()) {
    targets->push_back(F);
  } else {
    getCallGraph().addFullFunctionList(CS, *targets);
  }
}

// Enumerate possible functions that a value may resolve to
void DSAA::getFunctionsForValue(const Value *V,
                                std::vector<const Function*> *targets) const {
  assert(V);
  assert(targets);
  V = V->stripPointerCasts();
  if (const Function *F = dyn_cast<Function>(V)) {
    targets->push_back(F);
  } else {
    // N.B.: Don't need to check "isIgnored" here because the "ignored"
    // functions are not allowed to make indirect calls.
    const DSGraph::ScalarMapTy &GSM = Steens->getResultGraph()->getScalarMap();
    DSGraph::ScalarMapTy::const_iterator I = GSM.find(V);
    if (I != GSM.end())
      I->second.getNode()->addFullFunctionList(*targets);
  }
}

// Return true if V is a modified DSGraph node
bool DSAA::mayBeModified(const Value *V) {
  assert(V);

  // Stupid hacky case: any of the "ignored" values might be modified
  if (Steens->isIgnoredValue(V))
    return true;

  // If not in the graph, then it's not a used pointer
  DSGraph *G = Steens->getResultGraph();
  DSGraph::ScalarMapTy::const_iterator I = G->getScalarMap().find(V);
  if (I == G->getScalarMap().end())
    return false;
  else
    return I->second.getNode()->isModifiedNode();
}

// Return the DSGraph for the function the Value appears in, or NULL
// if the value was not defined in a function.
static const Function*
getFunctionForValue(const Value *V) {
  if (const Instruction *I = dyn_cast<Instruction>(V))
    return I->getParent()->getParent();
  else if (const Argument *A = dyn_cast<Argument>(V))
    return A->getParent();
  else if (const BasicBlock *BB = dyn_cast<BasicBlock>(V))
    return BB->getParent();
  return NULL;
}

DSGraph*
DSAA::getFunctionGraphForValue(const Value *V) {
  const Function *F = getFunctionForValue(V);
  if (F) return TD->getDSGraph(*F);
  return NULL;
}

//
// Generic method for alias and points-to queries
// If points-to queries are enabled, "NoAlias" means "NeverPointsTo".
//

AliasAnalysis::AliasResult
DSAA::checkAliasAndPointsTo(const Value *V1, const Value *V2,
                            uint64_t V1Size, uint64_t V2Size,
                            const bool checkPointsTo,
                            const bool ignoreOffsets,
                            const bool ignoreNullPointers) {
  // Decide on a common graph to use
  DSGraph *G1 = getFunctionGraphForValue(V1);
  DSGraph *G2 = getFunctionGraphForValue(V2);
  DSGraph *G = NULL;

  // If G1=null then V1 is a GlobalValue
  // In this case use G2 if it exists, otherwise fall back on the globals graph
  if (!G1 || !G2) {
    G = G1 ? G1 : (G2 ? G2 : TD->getGlobalsGraph());
  }

  // If the graphs differ, then the values come from different functions, so fall
  // back on the global Steensgaard graph
  else if (G1 != G2) {
    G = Steens->getResultGraph();
    // Stupid hacky case: any of the "ignored" may-alias anything
    if (Steens->isIgnoredValue(V1) || Steens->isIgnoredValue(V2))
      return MayAlias;
  }

  // Otherwise the graphs are the same and non-null
  else {
    G = G1;
  }

  // If the values aren't represented in the graph, then NoAlias
  const DSGraph::ScalarMapTy &GSM = G->getScalarMap();
  DSGraph::ScalarMapTy::const_iterator I = GSM.find(V1);
  if (I == GSM.end()) return NoAlias;

  DSGraph::ScalarMapTy::const_iterator J = GSM.find(V2);
  if (J == GSM.end()) return NoAlias;

  DSNode  *N1 = I->second.getNode(),  *N2 = J->second.getNode();
  unsigned O1 = I->second.getOffset(), O2 = J->second.getOffset();

  // Can't tell whether anything aliases null
  if ((N1 == NULL || N2 == NULL) && !ignoreNullPointers)
    return MayAlias;

  // We can make a further judgment only if one of the nodes is complete
  if ((N1->isCompleteNode() && !N1->isExternalNode()) ||
      (N2->isCompleteNode() && !N2->isExternalNode())) {
    // Different nodes don't alias
    if (N1 != N2)
      goto points_to_check;

    const bool isMemsetNode = G->isPointerUsedForMemsetOnly(V1)
                           || G->isPointerUsedForMemsetOnly(V2);

    // Non-overlapping fields don't alias
    if ((O1 != O2) && !ignoreOffsets && !isMemsetNode) {
      if (O2 < O1) {    // Ensure that O1 <= O2
        std::swap(V1, V2);
        std::swap(O1, O2);
        std::swap(V1Size, V2Size);
      }

      if (O1+V1Size <= O2)
        goto points_to_check;
    }
  }

  return MayAlias;

points_to_check:
  if (checkPointsTo) {
    // Can N1 point-to N2?
    DenseSet<const DSNode*> reachable;

    N1->markReachableNodes(reachable);
    if (reachable.count(N2))
      return MayAlias;
  }

  return NoAlias;
}

//
// Alias and points-to queries on the underlying objects
//

bool
DSAA::mayAliasUnderlyingObject(const Value *V1, const Value *V2) {
  assert(valid && "DSAA invalidated but then queried?!");

  if (V1 == V2)
    return MayAlias;

  if (!V1 || !V2)
    return false;

  DEBUG(errs() << "MayAliasUnderlyingObject for: " << *V1 << " " << *V2 << "\n");

  AliasResult res = checkAliasAndPointsTo(V1, V2, 0, 0, false, true, true);
  return (res != NoAlias);
}

bool
DSAA::mayPointToUnderlyingObject(const Value *V1, const Value *V2) {
  assert(valid && "DSAA invalidated but then queried?!");

  if (V1 == V2)
    return MayAlias;

  if (!V1 || !V2)
    return false;

  DEBUG(errs() << "MayPointToUnderlyingObject for: " << *V1 << " " << *V2 << "\n");

  AliasResult res = checkAliasAndPointsTo(V1, V2, 0, 0, true, true, true);
  return (res != NoAlias);
}

//
// alias()
// Do the locations alias?
//

AliasAnalysis::AliasResult
DSAA::alias(const Location &Loc1, const Location &Loc2) {
  assert(valid && "DSAA invalidated but then queried?!");

  const Value *V1 = Loc1.Ptr;
  const Value *V2 = Loc2.Ptr;
  uint64_t V1Size = Loc1.Size;
  uint64_t V2Size = Loc2.Size;

  if (V1 == V2)
    return MayAlias;

  if (!V1 || !V2)
    return AliasAnalysis::alias(Loc1, Loc2);

  DEBUG(errs() << "Alias for: " << *V1 << " " << *V2 << "\n");

  AliasResult res = checkAliasAndPointsTo(V1, V2, V1Size, V2Size, false, false, false);
  if (res != MayAlias)
    return res;

  // Other alias analyses cannot handle values from different functions.
  // Preempt assertion failures in that case (see, e.g., BasicAliasAnalysis).
  // Note that we can't do the separate-function comparison with G1 and G2,
  // because these may be the same for different functions in the same SCC.
  const Function *F1 = getFunctionForValue(V1);
  const Function *F2 = getFunctionForValue(V2);
  if (F1 && F2 && F1 != F2)
    return MayAlias;

  // Defer to chain
  return AliasAnalysis::alias(Loc1, Loc2);
}

//
// getModRefInfo()
// Does a callsite modify or reference a value?
//

AliasAnalysis::ModRefResult
DSAA::getModRefInfo(ImmutableCallSite CS, const Location &Loc) {
  assert(valid && "DSAA invalidated but then queried?!");

  if (!Loc.Ptr)
    return AliasAnalysis::getModRefInfo(CS, Loc);

  std::pair<bool, ModRefResult> res;

  //
  // Try to resolve the call target directly
  //
  const Function *F = CS.getCalledFunction();
  if (F) {
    DEBUG(errs() << "GetModRefInfo directcall: "
                 << F->getNameStr() << " :: " << *Loc.Ptr << "\n");

    res = getModRefInfoForCallee(CS, Loc, F);

    DEBUG(errs() << "                            <"
                 << res.first << " , " << res.second << ">\n");

    if (res.first)
      return res.second;
    else
      return mergeChainedModRefInfo(CS, Loc, res.second);
  }

  //
  // Otherwise, query DSA for all possible call targets
  //
  DSGraph *G = getFunctionGraphForValue(Loc.Ptr);
  if (!G)
    G = TD->getGlobalsGraph();

  const DSGraph::ScalarMapTy &GSM = G->getScalarMap();
  DSGraph::ScalarMapTy::const_iterator nodeIt = GSM.find(CS.getCalledValue());
  if (nodeIt == GSM.end())
    return mergeChainedModRefInfo(CS, Loc, ModRef);

  std::vector<const Function*> targets;
  nodeIt->second.getNode()->addFullFunctionList(targets);
  if (targets.empty())
    return mergeChainedModRefInfo(CS, Loc, ModRef);

  // Combine the ModRefInfo for all possible targets
  bool precise = true;
  unsigned preciseinfo = NoModRef;  // we cannot be more precise than this
  unsigned mayinfo     = NoModRef;  // conservative may-info (we will OR-in as we go)

  for (size_t i = 0; i < targets.size(); ++i) {
    F = targets[i];

    DEBUG(errs() << "GetModRefInfo indirectcall: "
                 << F->getNameStr() << " " << *Loc.Ptr << "\n");

    res = getModRefInfoForCallee(CS, Loc, F);

    DEBUG(errs() << "                            <"
                 << res.first << " , " << res.second << ">\n");

    if (res.first) {
      preciseinfo |= res.second;
      if (preciseinfo == ModRef)
        return ModRef;
    }
    mayinfo |= res.second;
    if (!res.first)
      precise = false;
  }

  if (precise) {
    assert(preciseinfo == mayinfo);
    return (ModRefResult)preciseinfo;
  }

  // Maybe a chained analysis can be more precise
  return mergeChainedModRefInfo(CS, Loc, (ModRefResult)mayinfo);
}

//
// At this point, the absence of bits in `mayinfo` represents proven info.
// For example, if mayinfo=Ref, then we have proven that none of the targets
// can modify Loc.  However, we were not completely precise, so we should
// check if a chained AliasAnalysis::getModRefInfo can remove more bits
// from mayinfo.
//
AliasAnalysis::ModRefResult
DSAA::mergeChainedModRefInfo(ImmutableCallSite CS, const Location &Loc,
                             ModRefResult mayinfo) {
  if (mayinfo == NoModRef)
    return NoModRef;

  // Other alias analyses cannot handle values from different functions
  // Preempt assertion failures in that case (see, e.g., BasicAliasAnalysis)
  const Function *F = getFunctionForValue(Loc.Ptr);
  if (F && F != CS.getCaller())
    return (ModRefResult)mayinfo;

  mayinfo = ModRefResult(mayinfo & AliasAnalysis::getModRefInfo(CS, Loc));
  return mayinfo;
}

//
// Does a callsite modify or reference a value?  In this version, we are
// given a specific known (possible) callee, and we return a pair:
//
//   first: true iff we have precise ModRef info
//          false if we should query a chained analysis for more info
//
//   second: the ModRefResult
//
std::pair<bool, AliasAnalysis::ModRefResult>
DSAA::getModRefInfoForCallee(ImmutableCallSite CS, const Location &Loc,
                             const Function *F) {
  assert(valid && "DSAA invalidated but then queried?!");
  assert(F);

  const Value *P = Loc.Ptr;
  const uint64_t Size = Loc.Size;

  // Cannot optimize across memory barriers, unless the value doesn't escape
  // TODO: we can probably optimize this further for Acquire vs Release fences
  if (ThreadSafeDSAA) {
    if (MemoryBarriers.count(F) > 0 && mightValueEscapeThread(P))
      return std::make_pair(true, ModRef);
  }

  DSNode *N = NULL;
  // First step, check our cache.
  if (CS.getInstruction() == MapCS.getInstruction()) {
    DEBUG(errs() << "  ... cached\n");
    {
      const Function *Caller = CS.getCaller();
      DSGraph* CallerTDGraph = TD->getDSGraph(*Caller);

      // Figure out which node in the TD graph this pointer corresponds to.
      DSScalarMap &CallerSM = CallerTDGraph->getScalarMap();
      DSScalarMap::iterator NI = CallerSM.find(P);
      if (NI == CallerSM.end()) {
        invalidateCache();
        return DSAA::getModRefInfoForCallee(CS, Loc, F);
      }
      N = NI->second.getNode();
    }

  HaveMappingInfo:
    assert(N && "Null pointer in scalar map??");

    typedef std::multimap<DSNode*, const DSNode*>::iterator NodeMapIt;
    std::pair<NodeMapIt, NodeMapIt> Range = CallerCalleeMap.equal_range(N);

    // Loop over all of the nodes in the callee that correspond to "N", keeping
    // track of aggregate mod/ref info.
    bool NeverReads = true, NeverWrites = true;
    for (; Range.first != Range.second; ++Range.first) {
      if (Range.first->second->isModifiedNode())
        NeverWrites = false;
      if (Range.first->second->isReadNode())
        NeverReads = false;
      if (NeverReads == false && NeverWrites == false)
        return std::make_pair(false, ModRef);
    }

    ModRefResult Result = ModRef;
    if (NeverWrites)      // We proved it was not modified.
      Result = ModRefResult(Result & ~Mod);
    if (NeverReads)       // We proved it was not read.
      Result = ModRefResult(Result & ~Ref);

    if (Result == NoModRef)
      return std::make_pair(true, NoModRef);

    // Maybe a further AliasAnalysis can be more precise
    return std::make_pair(false, Result);
  }

  // Any cached info we have is for the wrong function.
  invalidateCache();

  if (F->isDeclaration()) {
    const bool escapes  = ThreadSafeDSAA ? mightValueEscapeThread(P) : false;

    //
    // First check if this is a known external function
    //
    const StdLibInfo::LibAction *action =
      StdLib->getStdLibInfo().getLibActionForFunction(F);

    if (action) {
      // Cannot optimize across memory barriers.  Note that `MemoryBarriers`
      // only includes functions with definitions, hence this check is not
      // redundant with the one above.
      if (ThreadSafeDSAA && action->memoryBarrier && escapes)
        return std::make_pair(true, ModRef);

      unsigned res = NoModRef;

      // For each arg (including the return value), check if the ptr
      // aliases that arg.  If it does, merge the mod/ref info for that
      // arg into the result.
      for (unsigned y = 0; y < CS.arg_size()+1; ++y) {
        if (!action->read[y] && !action->write[y])
          continue;
        const Value *a = (y == 0) ? CS.getInstruction() : CS.getArgument(y-1);
        if (isa<PointerType>(a->getType()) && alias(Loc, Location(a,Size)) != NoAlias) {
          if (action->read[y])
            res = Ref;
          if (action->write[y])
            res |= Mod;
          if (res == ModRef)
            break;
        }
      }

      return std::make_pair(true, (ModRefResult)res);
    }

    //
    // This external function is not known.
    // Thread safe:
    //   Worst case, it may call a sync function, i.e., a memory barrier
    // Not thread safe:
    //   If P doesn't escape to an external function, it cannot be modified.
    //
    if (ThreadSafeDSAA && escapes)
      return std::make_pair(true, ModRef);

    const Function *Caller = CS.getInstruction()->getParent()->getParent();
    DSGraph *G = TD->getDSGraph(*Caller);
    DSScalarMap::iterator NI = G->getScalarMap().find(P);
    if (NI == G->getScalarMap().end()) {
      // If it wasn't in the local function graph, check the global graph.  This
      // can occur for globals who are locally reference but hoisted out to the
      // globals graph despite that.
      G = G->getGlobalsGraph();
      NI = G->getScalarMap().find(P);
      if (NI == G->getScalarMap().end())
        return std::make_pair(false, ModRef);
    }

    DSNode *N = NI->second.getNode();
    if (N->isCompleteNode() && !N->isExternalNode())
      return std::make_pair(true, NoModRef);
    else
      return std::make_pair(false, ModRef);
  }

  // Get the graphs for the callee and caller.  Note that we want the BU graph
  // for the callee because we don't want all caller's effects incorporated!
  const Function *Caller = CS.getInstruction()->getParent()->getParent();
  DSGraph* CallerTDGraph = TD->getDSGraph(*Caller);
  DSGraph* CalleeBUGraph = BU->getDSGraph(*F);

  // Figure out which node in the TD graph this pointer corresponds to.
  DSScalarMap &CallerSM = CallerTDGraph->getScalarMap();
  DSScalarMap::iterator NI = CallerSM.find(P);
  if (NI == CallerSM.end()) {
    ModRefResult Result = ModRef;
    if (isa<ConstantPointerNull>(P) || isa<UndefValue>(P))
      return std::make_pair(true, NoModRef);  // null is never modified :)
    else {
      assert(isa<GlobalVariable>(P) &&
    cast<GlobalVariable>(P)->getType()->getElementType()->isFirstClassType() &&
             "This isn't a global that DSA inconsiderately dropped "
             "from the graph?");

      DSGraph* GG = CallerTDGraph->getGlobalsGraph();
      DSScalarMap::iterator NI = GG->getScalarMap().find(P);
      if (NI != GG->getScalarMap().end() && !NI->second.isNull()) {
        // Otherwise, if the node is only M or R, return this.  This can be
        // useful for globals that should be marked const but are not.
        DSNode *N = NI->second.getNode();
        if (!N->isModifiedNode())
          Result = (ModRefResult)(Result & ~Mod);
        if (!N->isReadNode())
          Result = (ModRefResult)(Result & ~Ref);
      }
    }

    if (Result == NoModRef)
      return std::make_pair(true, Result);

    // Maybe a further AliasAnalysis can be more precise
    return std::make_pair(false, Result);
  }

  // Compute the mapping from nodes in the callee graph to the nodes in the
  // caller graph for this call site.
  CallSite mCS = CallSite(const_cast<Instruction*>(CS.getInstruction()));
  DSGraph::NodeMapTy CalleeCallerMap;
  DSCallSite DSCS = CallerTDGraph->getDSCallSiteForCallSite(mCS);
  CallerTDGraph->computeCalleeCallerMapping(DSCS, *F, *CalleeBUGraph,
                                            CalleeCallerMap);

  // Remember the mapping and the call site for future queries.
  MapCS = mCS;

  // Invert the mapping into CalleeCallerInvMap.
  for (DSGraph::NodeMapTy::iterator I = CalleeCallerMap.begin(),
         E = CalleeCallerMap.end(); I != E; ++I)
    CallerCalleeMap.insert(std::make_pair(I->second.getNode(), I->first));

  N = NI->second.getNode();
  goto HaveMappingInfo;
}

// Does a function modify or reference any memory locations?
AliasAnalysis::ModRefBehavior
DSAA::getModRefBehavior(const Function *F) {
  //
  // Just look for known external functions (via StdLibInfo).
  // Everything else gets forwarded to the chained AA.
  //
  // Assume StdLib functions do *not* access global variables, other than
  // those either passed as pointers to the function or returned as a result
  // from the function.
  //
  const StdLibInfo::LibAction *action;
  if (F->isDeclaration() &&
      (action = StdLib->getStdLibInfo().getLibActionForFunction(F)) != NULL) {

    // Any writes?
    if (action->write[0])
      return UnknownModRefBehavior;

    for (int i = 1; i < StdLibInfo::numOps; ++i)
      if (action->write[i])
        return OnlyAccessesArgumentPointees;
 
    // Any reads?
    if (action->read[0])
      return OnlyReadsMemory;

    for (int i = 1; i < StdLibInfo::numOps; ++i)
      if (action->read[i])
        return OnlyReadsArgumentPointees;

    // No reads or writes
    return DoesNotAccessMemory;
  }

  return AliasAnalysis::getModRefBehavior(F);
}
