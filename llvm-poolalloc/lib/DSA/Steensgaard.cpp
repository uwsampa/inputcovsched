//===- Steensgaard.cpp - Context Insensitive Data Structure Analysis ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass computes a context-insensitive data analysis graph.  It does this
// by computing the local analysis graphs for all of the functions, then merging
// them together into a single big graph without cloning.
//
//===----------------------------------------------------------------------===//

#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include <iostream>
#include <ostream>

using namespace llvm;

// Ugly
// Since this analysis is equivalence-based and context-insensitive, we lose a ton
// of precision due to wrapper functions for malloc (e.g.) whose usage essentially
// causes all heap objects to be merged into a single node.  The following is an
// ugly hack to get back a little bit of precision for this case.  Essentially,
// we completely ignore these wrapper functions when creating the SteensGraph, and
// when a user queries a value defined in these functions, we conservatively assume
// that pointer may-alias any object in the program.  As a rule, these wrapper
// functions should *not* make any indirect calls.

#define ARRAY_SIZE(X) (sizeof(X)/sizeof(X[0]))
#define DEFINE_STRING_SET(SetName, ...) \
  const char* const SetName##List[] = { __VA_ARGS__ }; \
  static std::set<StringRef> SetName(SetName##List, SetName##List + ARRAY_SIZE(SetName##List));

DEFINE_STRING_SET(IgnoredFunctions,
  "__klee_model_malloc",
  "__klee_model_calloc",
  "__klee_model_realloc",
  "__klee_model_memalign",
  "__klee_model_posix_memalign",
  "__klee_model_valloc",
  "__klee_model__Znaj",
  "__klee_model__Znwj",
  "__klee_model__ZnamRKSt9nothrow_t",
  "__klee_model__Znam",
  "__klee_model__Znwm",
  "__klee_model__ZnwmRKSt9nothrow_t",
  "__klee_model_free",
  "__klee_model__ZdlPv",
  "__klee_model__ZdlPvRKnothrow_t",
  "__klee_model__ZdaPv",
  "__klee_model__ZdaPvRKnothrow_t",
  "__klee_model_memset",
  "__klee_model_memcpy",
  "__klee_model_mempcpy",
  "__klee_model_memmove",
  "__klee_model_bzero",
  "__klee_model_bcopy",
  "__klee_pthread_model_pthread_mutex_init",
  "__klee_pthread_model_pthread_mutex_destroy",
  "__klee_pthread_model_pthread_mutex_lock",
  "__klee_pthread_model_pthread_mutex_trylock",
  "__klee_pthread_model_pthread_mutex_unlock",
  "__klee_pthread_model_pthread_cond_init",
  "__klee_pthread_model_pthread_cond_destroy",
  "__klee_pthread_model_pthread_cond_wait",
  "__klee_pthread_model_pthread_cond_broadcast",
  "__klee_pthread_model_pthread_cond_signal",
  "__klee_pthread_model_pthread_barrier_init",
  "__klee_pthread_model_pthread_barrier_destroy",
  "__klee_pthread_model_pthread_barrier_wait",
  "__klee_pthread_model_pthread_rwlock_init",
  "__klee_pthread_model_pthread_rwlock_destroy",
  "__klee_pthread_model_pthread_rwlock_rdlock",
  "__klee_pthread_model_pthread_rwlock_tryrdlock",
  "__klee_pthread_model_pthread_rwlock_wrlock",
  "__klee_pthread_model_pthread_rwlock_trywrlock",
  "__klee_pthread_model_pthread_rwlock_unlock",
  "__klee_pthread_model_sem_init",
  "__klee_pthread_model_sem_destroy",
  "__klee_pthread_model_sem_wait",
  "__klee_pthread_model_sem_trywait",
  "__klee_pthread_model_sem_post",
  "__uClibc_wrap_pthread_mutex_lock",
  "__uClibc_wrap_pthread_mutex_trylock",
  "__uClibc_wrap_pthread_mutex_unlock",
)

bool SteensgaardDataStructures::isIgnoredValue(const Value *ptr) {
  if (const Argument *a = dyn_cast<Argument>(ptr)) {
    return isIgnoredFunction(a->getParent());
  }
  if (const Instruction *i = dyn_cast<Instruction>(ptr)) {
    return isIgnoredFunction(i->getParent()->getParent());
  }
  return false;
}

bool SteensgaardDataStructures::isIgnoredFunction(const Function *F) {
  return IgnoredFunctions.count(F->getName())
      || F->getName().startswith("__icsruntime")
      || F->getName().startswith("__ics_");
}

// The pass

static RegisterPass<SteensgaardDataStructures>
X("dsa-steens", "Steensgaard Data Structure Analysis");

char SteensgaardDataStructures::ID;

SteensgaardDataStructures::~SteensgaardDataStructures() {
  releaseMemory();
}

void
SteensgaardDataStructures::releaseMemory() {
  delete ResultGraph; 
  ResultGraph = 0;
  DataStructures::releaseMemory();
}

void
SteensgaardDataStructures::print(llvm::raw_ostream &O, const Module *M) const {
  assert(ResultGraph && "Result graph has not yet been computed!");
  ResultGraph->writeGraphToFile(O, "steensgaards");
}


/// run - Build up the result graph, representing the pointer graph for the
/// program.
///
bool
SteensgaardDataStructures::runOnModule(Module &M) {
  DS = &getAnalysis<TDDataStructures>();
  init(DS, true, true, true, false);
  return runOnModuleInternal(M);
}

bool
SteensgaardDataStructures::runOnModuleInternal(Module &M) {
  assert(ResultGraph == NULL && "Result graph already allocated!");
  
  // Create a new, empty, graph.
  // The GlobalsGraph was created for us by init()
  ResultGraph = new DSGraph(GlobalECs, getTargetData(), *TypeSS, GlobalsGraph);

  // Merge graphs for non-external functions into this graph.
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
    if (I->isDeclaration())
      continue;
    // Ugly: For ignored functions, don't clone the graph, but do remember the
    // list of function calls so we can build a complete call graph.
    if (isIgnoredFunction(I)) {
      DSGraph *G = DS->getDSGraph(*I);
      for (DSGraph::fc_iterator fc = G->fc_begin(); fc != G->fc_end(); ++fc) {
        std::vector<DSNodeHandle> empty;
        // XXX FIXME
        if (fc->isIndirectCall()) {
          llvm::errs() << "Steens: WARNING: Ignoring indirect call in " << I->getName() << ": "
                       << *fc->getCallSite().getInstruction() << "\n";
          continue;
        }
        ResultGraph->getFunctionCalls().push_back(
            DSCallSite(fc->getCallSite(), DSNodeHandle(), DSNodeHandle(), fc->getCalleeFunc(),
                       empty));
      }
      for (DSGraph::fc_iterator fc = G->getRemovedFunctionCalls().begin();
                               fc != G->getRemovedFunctionCalls().end(); ++fc) {
        std::vector<DSNodeHandle> empty;
        // XXX FIXME
        if (fc->isIndirectCall()) {
          llvm::errs() << "Steens: WARNING: Ignoring indirect call in " << I->getName() << ": "
                       << *fc->getCallSite().getInstruction() << "\n";
          continue;
        }
        ResultGraph->getRemovedFunctionCalls().push_back(
            DSCallSite(fc->getCallSite(), DSNodeHandle(), DSNodeHandle(), fc->getCalleeFunc(),
                       empty));
      }
    } else {
      ResultGraph->cloneInto(DS->getDSGraph(*I));  // ugh, should be named "cloneFrom"
    }
  }

  // Merge in the globals graph
  ResultGraph->cloneInto(GlobalsGraph);

  // Now that all graphs are inlined, eliminate call nodes.  We eliminate all
  // actual calls and also the dummy thread calls (these get copied into the
  // FunctionCalls list).  After we're done, the calls remaining in `FunctionCalls`
  // are ones which could not be fully resolved.
  std::list<DSCallSite> Calls(ResultGraph->getFunctionCalls());
  Calls.insert(Calls.end(), ResultGraph->getCreateThreadCalls().begin(),
                            ResultGraph->getCreateThreadCalls().end());

  for (std::list<DSCallSite>::iterator CI = Calls.begin(); CI != Calls.end();) {
    DSCallSite &CurCall = *CI++;

    if (isIgnoredFunction(&CurCall.getCaller()))
      continue;

    // Get possible call targets at this call site.
    std::vector<const Function*> CallTargets;
    if (CurCall.isDirectCall())
      CallTargets.push_back(CurCall.getCalleeFunc());
    else
      CurCall.getCalleeNode()->addFullFunctionList(CallTargets);

    // Loop over the possible targets, eliminating as many as possible.
    // To eliminate, we merge the retval and args at the callsite with
    // those at the definition (thus we can't eliminate external calls).
    for (unsigned c = 0; c < CallTargets.size(); ++c) {
      const Function *F = CallTargets[c];
      if (!F->isDeclaration() && !isIgnoredFunction(F))
        ResolveFunctionCall(F, CurCall, ResultGraph->getReturnNodes()[F]);
    }
  }

  // Remove our knowledge of what the return values of the functions are, except
  // for functions that are externally visible from this module (e.g. main).  We
  // keep these functions so that their arguments are marked incomplete.
  for (DSGraph::ReturnNodesTy::iterator
         I = ResultGraph->getReturnNodes().begin(),
         E = ResultGraph->getReturnNodes().end(); I != E; ) {
    if (I->first->hasInternalLinkage() || I->first->hasPrivateLinkage())
      ResultGraph->getReturnNodes().erase(I++);
    else
      ++I;
  }

  // Clone the global nodes into this graph.
  cloneGlobalsInto(ResultGraph, DSGraph::DontCloneCallNodes |
                                DSGraph::DontCloneAuxCallNodes);

  // TDDataStructures has labeled incomplete/external nodes.  Now
  // we propagate external labels transitively in case of node merges.
  // TODO: need to propagate incomplete labels in the same way?
  ResultGraph->computeExternalFlags(DSGraph::DontMarkFormalsExternal |
                                    DSGraph::IgnoreCallSites);
  ResultGraph->computeIntPtrFlags();

  // Build a final callgraph.  This is much more accurate than the callgraph
  // inherited from TD, especially for indirect function calls, since BU and
  // TD both approximate indirect calls with the set of functions whose
  // address is taken (yuck).  Also, it includes calls which were removed by
  // DSGraph optimizations (e.g., StdLibPass), so it is a true callgraph.
  ResultGraph->getFunctionCalls().splice(ResultGraph->getFunctionCalls().end(),
                                         ResultGraph->getRemovedFunctionCalls());
  callgraph = DSCallGraph();
  std::vector<const Function*> unused;
  ResultGraph->buildCallGraph(callgraph, unused, true);
  callgraph.buildSCCs();
  callgraph.buildRoots();

  DEBUG(print(errs(), &M));
  return false;
}

/// ResolveFunctionCall - Resolve the actual arguments of a call to function F
/// with the specified call site descriptor.  This function links the arguments
/// and the return value for the call site context-insensitively.
///
void
SteensgaardDataStructures::ResolveFunctionCall(const Function *F, 
                                               const DSCallSite &Call,
                                               DSNodeHandle &RetVal) {
  assert(ResultGraph != 0 && "Result graph not allocated!");
  DSGraph::ScalarMapTy &ValMap = ResultGraph->getScalarMap();

  // Handle the return value of the function...
  if (Call.getRetVal().getNode() && RetVal.getNode())
    RetVal.mergeWith(Call.getRetVal());

  // Loop over all pointer arguments, resolving them to their provided pointers
  unsigned PtrArgIdx = 0;
  for (Function::const_arg_iterator AI = F->arg_begin(), AE = F->arg_end();
       AI != AE && PtrArgIdx < Call.getNumPtrArgs(); ++AI) {
    DSGraph::ScalarMapTy::iterator I = ValMap.find(AI);
    if (I != ValMap.end())    // If its a pointer argument...
      I->second.mergeWith(Call.getPtrArg(PtrArgIdx++));
  }
}
