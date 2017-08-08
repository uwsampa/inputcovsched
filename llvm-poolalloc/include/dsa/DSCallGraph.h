//===- DSCallGaph.h - Build call graphs -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implement a detailed call graph for DSA.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DSCALLGRAPH_H
#define	LLVM_DSCALLGRAPH_H

#include "dsa/svset.h"
#include "dsa/keyiterator.h"

#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/Support/CallSite.h"

#include <cassert>
#include <map>

class DSCallGraph {
public:
  typedef svset<const llvm::Function*> FuncSet;
  typedef std::map<llvm::CallSite, FuncSet> ActualCalleesTy;
  typedef std::map<const llvm::Function*, FuncSet> SimpleCalleesTy;

private:
  // ActualCallees contains CallSite -> set of Function mappings
  ActualCalleesTy ActualCallees;

  // SimpleCallees contains Function -> set of Functions mappings
  SimpleCalleesTy SimpleCallees;

  // These are used for returning empty sets when the caller has no callees
  FuncSet EmptyActual;
  FuncSet EmptySimple;

  // An equivalence class is exactly an SCC
  llvm::EquivalenceClasses<const llvm::Function*> SCCs;

  // Functions we know about that aren't called
  svset<const llvm::Function*> knownRoots;
  
  // Functions that might be called from an incomplete
  // unresolved call site.
  svset<const llvm::Function*> IncompleteCalleeSet;

  svset<llvm::CallSite> completeCS;

  // Types for SCC construction
  typedef std::map<const llvm::Function*, unsigned> TFMap;
  typedef std::vector<const llvm::Function*> TFStack;

  // Tarjan's SCC algorithm
  unsigned tarjan_rec(const llvm::Function* F, TFStack& Stack, unsigned &NextID,
                      TFMap& ValMap);

  void removeECFunctions();

public:
  DSCallGraph() {}

  typedef ActualCalleesTy::mapped_type::const_iterator callee_iterator;
  typedef KeyIterator<ActualCalleesTy::const_iterator> callee_key_iterator;
  typedef SimpleCalleesTy::mapped_type::const_iterator flat_iterator;
  typedef KeyIterator<SimpleCalleesTy::const_iterator> flat_key_iterator;
  typedef FuncSet::const_iterator                      root_iterator;
  typedef llvm::EquivalenceClasses<const llvm::Function*>::member_iterator scc_iterator;

  void insert(llvm::CallSite CS, const llvm::Function* F);
  void erase(llvm::CallSite CS, const llvm::Function* F);

  void insureEntry(const llvm::Function* F);

  template<class Iterator>
  void insert(llvm::CallSite CS, Iterator _begin, Iterator _end) {
    for (; _begin != _end; ++_begin)
      insert(CS, *_begin);
  }

  callee_iterator callee_begin(llvm::CallSite CS) const {
    ActualCalleesTy::const_iterator ii = ActualCallees.find(CS);
    if (ii == ActualCallees.end())
      return EmptyActual.end();
    return ii->second.begin();
  }

  callee_iterator callee_end(llvm::CallSite CS) const {
    ActualCalleesTy::const_iterator ii = ActualCallees.find(CS);
    if (ii == ActualCallees.end())
      return EmptyActual.end();
    return ii->second.end();
  }

  callee_key_iterator key_begin() const {
    return callee_key_iterator(ActualCallees.begin());
  }

  callee_key_iterator key_end() const {
    return callee_key_iterator(ActualCallees.end());
  }

  flat_iterator flat_callee_begin(const llvm::Function* F) const {
    SimpleCalleesTy::const_iterator ii = SimpleCallees.find(F);
    if (ii == SimpleCallees.end())
      return EmptySimple.end();
    return ii->second.begin();
  }

  flat_iterator flat_callee_end(const llvm::Function* F) const {
    SimpleCalleesTy::const_iterator ii = SimpleCallees.find(F);
    if (ii == SimpleCallees.end())
      return EmptySimple.end();
    return ii->second.end();
  }

  flat_key_iterator flat_key_begin() const {
    return flat_key_iterator(SimpleCallees.begin());
  }

  flat_key_iterator flat_key_end() const {
    return flat_key_iterator(SimpleCallees.end());
  }

  root_iterator root_begin() const {
    return knownRoots.begin();
  }

  root_iterator root_end() const {
    return knownRoots.end();
  }

  bool isKnownRoot(const llvm::Function *F) const {
    return knownRoots.count(F);
  }

  scc_iterator scc_begin(const llvm::Function* F) const {
    assert(F == SCCs.getLeaderValue(F) && "Requested non-leader");
    return SCCs.member_begin(SCCs.findValue(F));
  }

  scc_iterator scc_end(const llvm::Function* F) const {
    assert(F == SCCs.getLeaderValue(F) && "Requested non-leader");
    return SCCs.member_end();
  }
  
  const llvm::Function* sccLeader(const llvm::Function *F) const {
    return SCCs.getLeaderValue(F);
  }
  
  const llvm::Function* sccLeaderIfExists(const llvm::Function *F) const {
    scc_iterator it = SCCs.findLeader(F);
    return (it == SCCs.member_end()) ? NULL : *it;
  }

  size_t sccSize(const llvm::Function* F) const {
    size_t count = 0;
    F = sccLeader(F);
    for (scc_iterator it = scc_begin(F); it != scc_end(F); ++it)
      count++;
    return count;
  }

  unsigned callee_size(llvm::CallSite CS) const {
    ActualCalleesTy::const_iterator ii = ActualCallees.find(CS);
    if (ii == ActualCallees.end())
      return 0;
    return ii->second.size();
  }

  bool called_from_incomplete_site(const llvm::Function *F) const {
    return !(IncompleteCalleeSet.find(F) 
             == IncompleteCalleeSet.end()); 
  }
  void callee_mark_complete(llvm::CallSite CS) {
    completeCS.insert(CS);
  }

  bool callee_is_complete(llvm::CallSite CS) const {
    return completeCS.find(CS) != completeCS.end();
  }

  unsigned size() const {
    unsigned sum = 0;
    for (ActualCalleesTy::const_iterator ii = ActualCallees.begin(),
            ee = ActualCallees.end(); ii != ee; ++ii)
      sum += ii->second.size();
    return sum;
  }

  unsigned flat_size() const {
    return SimpleCallees.size();
  }

  void buildSCCs();

  void buildRoots();
  
  void buildIncompleteCalleeSet(svset<const llvm::Function*> callees);
  
  void addFullFunctionList(llvm::CallSite CS, std::vector<const llvm::Function*> &List) const;

  void dump();

  void assertSCCRoot(const llvm::Function* F) {
    assert(F == SCCs.getLeaderValue(F) && "Not Leader?");
  }

  // common helper; no good reason for it to be here rather than elsewhere
  static bool hasPointers(const llvm::Function* F);
  static bool hasPointers(llvm::CallSite& CS);

public:
  //
  // Make a nice iterator to avoid an annoying idiom.  N.B.: This is useful
  // because direct calls are not always included in the callgraph (yuck).
  // More specifically:
  //   * Callgraphs constructed via DSGraph::buildCompleteCallGraph()
  //     do NOT include direct calls
  //   * Callgraphs constructed via DSGraph::buildCallGraph()
  //     DO include direct calls
  //
  // This iterator makes it easy to deal with both in the same way.  Possibly,
  // there's a bug in DSGraph::buildCompleteCallGraph(), but I'm afraid of
  // breaking code, so I won't change it.
  //
  class call_target_iterator : public std::iterator<std::input_iterator_tag,
                                                    const llvm::Function*> {
  private:
    const DSCallGraph *_cg;
    const llvm::Function *_direct;
    callee_iterator _scc;
    callee_iterator _sccEnd;
    scc_iterator _target;
    scc_iterator _targetEnd;
    bool _end;

  public:
    call_target_iterator()
      : _cg(NULL), _direct(NULL), _end(true)
    {}
    call_target_iterator(const llvm::CallSite &cs, const DSCallGraph &cg, bool end) {
      _cg = &cg;
      _end = end;
      _direct = cs.getCalledFunction();
      if (!end && !_direct) {
        _scc = cg.callee_begin(cs);
        _sccEnd = cg.callee_end(cs);
        if (_scc == _sccEnd) {
          _end = true;
        } else {
          _target = cg.scc_begin(*_scc);
          _targetEnd = cg.scc_end(*_scc);
        }
      }
    }

    bool operator==(const call_target_iterator &rhs) const {
      if (_cg != rhs._cg || _end != rhs._end)
        return false;
      if (_end)
        return true;
      if (_direct)
        return (_direct == rhs._direct);
      if (_scc != rhs._scc)
        return false;
      if (_scc != _sccEnd && _target != rhs._target)
        return false;
      return true;
    }

    bool operator!=(const call_target_iterator &rhs) const {
      return !((*this) == rhs);
    }

    const llvm::Function* operator*() const {
      assert(_cg);
      assert(!_end);

      if (_direct)
        return _direct;
      else
        return *_target;
    }

    call_target_iterator& operator++() {
      assert(_cg);
      assert(!_end);

      if (_direct) {
        _end = true;
      } else {
        assert(_scc != _sccEnd);
        assert(_target != _targetEnd);

        if (++_target == _targetEnd) {
          if (++_scc == _sccEnd) {
            _end = true;
          } else {
            _target = _cg->scc_begin(*_scc);
            _targetEnd = _cg->scc_end(*_scc);
          }
        }
      }
      return *this;
    }

    call_target_iterator operator++(int) {
      call_target_iterator old = *this;
      ++(*this);
      return old;
    }
  };

  call_target_iterator call_target_begin(const llvm::CallSite &CS) const {
    return call_target_iterator(CS, *this, false);
  }

  call_target_iterator call_target_end(const llvm::CallSite &CS) const {
    return call_target_iterator(CS, *this, true);
  }

};

#endif	/* LLVM_DSCALLGRAPH_H */

