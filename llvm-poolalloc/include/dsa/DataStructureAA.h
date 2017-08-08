//===- DataStructureAA.h - Data Structure Based Alias Analysis ------------===//
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

#ifndef LLVM_ANALYSIS_DATASTRUCTUREAA_H
#define LLVM_ANALYSIS_DATASTRUCTUREAA_H

#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "dsa/DataStructure.h"
#include "dsa/DSGraph.h"

#include <tr1/unordered_map>

namespace llvm {

class DSAA : public ModulePass, public AliasAnalysis {
public:
  typedef std::tr1::unordered_map<const llvm::Function*, DSCallGraph::FuncSet> CallerMapTy;

private:
  // We do *NOT* want the EQ/Equiv analyses (less precise, also broken)
  StdLibDataStructures *StdLib;
  TDDataStructures *TD;
  BUDataStructures *BU;
  SteensgaardDataStructures *Steens;

  // The callgraph, bottom-up
  // This includes functions from StdLibInfo not normally included in DSCallGraph
  CallerMapTy BottomUpCallGraph;

  // The callgraph, top-down
  // This includes functions from StdLibInfo not normally included in DSCallGraph
  CallerMapTy TopDownCallGraph;

  // The set of functions which may invoke a memory barrier, including via a
  // transitive call
  std::set<const Function*> MemoryBarriers;

  // The set of nodes which may escape from a single thread to multiple threads
  // These are nodes in the Steensgaard DSGraph
  DenseSet<const DSNode*> EscapingNodes;

  // These members are used to cache mod/ref information to make us return
  // results faster, particularly for aa-eval.  On the first request of
  // mod/ref information for a particular call site, we compute and store the
  // calculated nodemap for the call site.  Any time DSA info is updated we
  // free this information, and when we move onto a new call site, this
  // information is also freed.
  CallSite MapCS;
  std::multimap<DSNode*, const DSNode*> CallerCalleeMap;
  bool valid;

private:
  void invalidateCache();

  void buildBottomUpCallGraph();
  void buildBottomUpCallGraph(const DSCallGraph &CG,
                              const Function *F,
                              DenseSet<const Function*> *visited);
  void dumpBottomUpCallGraph(raw_ostream &os);
  void dumpCallGraphSCCs(raw_ostream &os);

  void enumerateMemoryBarriers();
  bool isMemoryBarrierPrimitive(const Function *F);
  void dumpMemoryBarriers(raw_ostream &os);

  void enumerateEscapingNodes(Module &M);
  void dumpEscapingNodes(raw_ostream &os, Module &M);

  DSGraph *getFunctionGraphForValue(const Value *V);

  AliasAnalysis::AliasResult
  checkAliasAndPointsTo(const Value *V1, const Value *V2,
                        uint64_t V1Size, uint64_t V2Size,
                        const bool checkPointsTo,
                        const bool ignoreOffsets,
                        const bool ignoreNullPointers);

  std::pair<bool, ModRefResult>
  getModRefInfoForCallee(ImmutableCallSite CS,
                         const Location &Loc,
                         const Function *Callee);

  ModRefResult
  mergeChainedModRefInfo(ImmutableCallSite CS,
                         const Location &Loc,
                         ModRefResult mayinfo);

public:
  static char ID;
  DSAA();
  virtual ~DSAA();

  //------------------------------------------------
  // Implement the Pass API
  //

  virtual bool runOnModule(Module &M);
  virtual void releaseMemory();
  virtual void getAnalysisUsage(AnalysisUsage &AU) const;

  /// getAdjustedAnalysisPointer - This method is used when a pass implements
  /// an analysis interface through multiple inheritance.  If needed, it
  /// should override this to adjust the this pointer as needed for the
  /// specified pass info.
  virtual void *getAdjustedAnalysisPointer(const void *ID) {
    if (ID == &AliasAnalysis::ID)
      return (AliasAnalysis*)this;
    return this;
  }

  //------------------------------------------------
  // Expose some internals
  //

  const DSGraph* getSteensGraph() const { return Steens->getResultGraph(); }
  const DSCallGraph& getCallGraph() const { return Steens->getCallGraph(); }
  const CallerMapTy& getBottomUpCallGraph() const { return BottomUpCallGraph; }
  const CallerMapTy& getTopDownCallGraph() const { return TopDownCallGraph; }
  const StdLibInfo& getStdLibInfo() const { return StdLib->getStdLibInfo(); }

  void getCallTargets(const CallSite &CS,
                      std::vector<const Function*> *targets) const;

  // llvm::Value *runfn = call->getArgOperand(1)->stripPointerCasts();
  void getFunctionsForValue(const Value *B,
                            std::vector<const Function*> *targets) const;

  // Returns true iff the value is a pointer whose underlying pointed-to
  // object(s) may be modified.
  bool mayBeModified(const Value *V);

  //
  // Like alias()!=NoAlias, but field-insensitive, which means it
  // does not consider offset info from the DSGraph.  Notably, the
  // following query always returns true:
  //
  //    mayAliasUnderlyingObject("&s.f", "&s")
  //
  bool mayAliasUnderlyingObject(const Value *a, const Value *b);

  //
  // Returns true if "from" may transitively point-to the object
  // underlying "to", where, for example, the object underlying "s.f"
  // is "s".  This is a reachability query: it follows all links in a
  // points-to chain, while alias checks follow the first link only.
  //
  // This is field-insensitive.  For example, the following queries
  // will all return true, even if we know from the DSGraph that "p"
  // never points at a field of "s" other than "s.f":
  //
  //    mayPointToUnderlyingObject("p", "&s.f")
  //    mayPointToUnderlyingObject("p", "&s.g")
  //    mayPointToUnderlyingObject("p", "&s")
  //
  bool mayPointToUnderlyingObject(const Value *from, const Value *to);

  // XXX A bit of ugliness
  // See comments in Steensgaard.cpp for more info
  bool isSteensIgnoredValue(const Value *v) { return Steens->isIgnoredValue(v); }
  bool isSteensIgnoredFunction(const Function *f) { return Steens->isIgnoredFunction(f); }

  //------------------------------------------------
  // Implement an EscapeAnalysis API
  //

  //
  // Returns true if "V" points-to an object that is not provably thread-local.
  // This checks whether the pointed-to object *ever* might be shared across
  // threads.  Specifically, in the following code sample, we return true for
  // mightValueEscapeThread(p) even though the object pointed-to by p is
  // obviously private for the duration of the function:
  //
  //    void* foo() {
  //       p = malloc(...)       // (*p) is initially thread-private
  //       ...
  //       return p;             // (*p) is still thread-private
  //    }
  //    void var() {
  //       global->ptr = foo();  // (*p) escapes here
  //    }
  //
  bool mightValueEscapeThread(const Value *V);

  //------------------------------------------------
  // Implement the AliasAnalysis API
  //

  using AliasAnalysis::alias;
  virtual AliasResult alias(const Location &Loc1, const Location &Loc2);

  // getModRefInfo

  virtual ModRefResult getModRefInfo(ImmutableCallSite CS,
                                     const Location &Loc);

  virtual ModRefResult getModRefInfo(ImmutableCallSite CS1,
                                     ImmutableCallSite CS2) {
    return AliasAnalysis::getModRefInfo(CS1,CS2);
  }

  // getModRefBehavior

  virtual ModRefBehavior getModRefBehavior(const Function *F);

  virtual ModRefBehavior getModRefBehavior(ImmutableCallSite CS) {
    return AliasAnalysis::getModRefBehavior(CS);
  }

  // deleteValue
  // copyValue

  virtual void deleteValue(Value *V) {
    assert(valid && "DSAA invalidated but then queried?!");
    invalidateCache();
    BU->deleteValue(V);
    TD->deleteValue(V);
    AliasAnalysis::deleteValue(V);
  }

  virtual void copyValue(Value *From, Value *To) {
    assert(valid && "DSAA invalidated but then queried?!");
    if (From == To) return;
    invalidateCache();
    BU->copyValue(From, To);
    TD->copyValue(From, To);
    AliasAnalysis::copyValue(From, To);
  }
};

} // End llvm namespace

#endif
