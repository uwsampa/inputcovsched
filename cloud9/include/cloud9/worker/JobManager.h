/*
 * Cloud9 Parallel Symbolic Execution Engine
 *
 * Copyright (c) 2011, Dependable Systems Laboratory, EPFL
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of the Dependable Systems Laboratory, EPFL nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE DEPENDABLE SYSTEMS LABORATORY, EPFL BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * All contributors are listed in CLOUD9-AUTHORS file.
 *
*/

#ifndef JOBMANAGER_H_
#define JOBMANAGER_H_

#include "cloud9/Logger.h"
#include "cloud9/worker/TreeNodeInfo.h"
#include "cloud9/worker/SymbolicEngine.h"
#include "cloud9/worker/CoreStrategies.h"

#include "klee/KleeHandler.h"

#include <boost/thread.hpp>
#include <list>
#include <set>
#include <string>

namespace llvm {
class Module;
class Function;
}

namespace klee {
class Interpreter;
class Executor;
class ExecutionState;
class Searcher;
class KModule;
class KleeHandler;
}

namespace cloud9 {

namespace worker {

class SymbolicState;
class ExecutionJob;
class OracleStrategy;

class JobManager: public StateEventHandler {
private:
  /***************************************************************************
   * Initialization
   **************************************************************************/
  void initialize(llvm::Module *module, llvm::Function *mainFn, int argc,
      char **argv, char **envp);

  void initKlee();
  void initBreakpoints();
  void initStatistics();
  void initStrategy();


  StateSelectionStrategy *createCoverageOptimizedStrat(StateSelectionStrategy *base);
  OracleStrategy *createOracleStrategy();

  void initRootState(llvm::Function *f, int argc, char **argv, char **envp);

  /***************************************************************************
   * KLEE integration
   **************************************************************************/
  klee::Executor *executor;
  klee::Interpreter *interpreter;
  SymbolicEngine *symbEngine;

  klee::KleeHandler *kleeHandler;
  klee::KModule *kleeModule;

  llvm::Function *mainFn;

  /*
   * Symbolic tree
   */
  WorkerTree* tree;

  // recursive mutex so we can destroy states from onStateDeactivated callbacks
  boost::condition_variable_any jobsAvailabe;
  boost::recursive_mutex jobsMutex;

  typedef boost::unique_lock<boost::recursive_mutex> unique_jobs_lock_ty;

  bool terminationRequest;
  unsigned int jobCount;

  JobSelectionStrategy *selStrategy;

  /*
   * Job execution state
   */
  ExecutionJob *currentJob;
  bool replaying;
  bool batching;

  /*
   * Statistics
   */

  std::set<WorkerTree::NodePin> stats;
  bool statChanged;
  bool refineStats;

  /*
   * Breakpoint management data structures
   *
   */
  std::set<WorkerTree::NodePin> pathBreaks;
  std::set<unsigned int> codeBreaks;

  /*
   * Debugging and instrumentation
   */
  std::map<const char*, int> traceCounters;

  bool collectTraces;

  void dumpStateTrace(WorkerTree::Node *node);
  void dumpInstructionTrace(WorkerTree::Node *node);

  void serializeInstructionTrace(std::ostream &s, WorkerTree::Node *node);
  void parseInstructionTrace(std::istream &s, std::vector<unsigned int> &dest);

  void serializeExecutionTrace(std::ostream &os, WorkerTree::Node *node);
  void serializeExecutionTrace(std::ostream &os, SymbolicState *state);

  void processTestCase(SymbolicState *state);

  void fireActivateState(SymbolicState *state);
  void fireDeactivateState(SymbolicState *state);
  void fireUpdateState(SymbolicState *state, WorkerTree::Node *oldNode);
  void fireAddJob(ExecutionJob *job);
  void fireRemovingJob(ExecutionJob *job);

  void submitJob(ExecutionJob* job, bool activateStates);
  void finalizeJob(ExecutionJob *job, bool deactivateStates, bool invalid);

  template<typename JobIterator>
  void submitJobs(JobIterator begin, JobIterator end, bool activateStates) {
    int count = 0;
    for (JobIterator it = begin; it != end; it++) {
      submitJob(*it, activateStates);
      count++;
    }

    jobsAvailabe.notify_all();

    //CLOUD9_DEBUG("Submitted " << count << " jobs to the local queue");
  }

  ExecutionJob* selectNextJob(boost::unique_lock<boost::recursive_mutex> &lock,
      unsigned int timeOut);
  ExecutionJob* selectNextJob();

  static bool isJob(WorkerTree::Node *node);
  bool isExportableJob(WorkerTree::Node *node);
  bool isValidJob(WorkerTree::Node *node);

  void executeJob(boost::unique_lock<boost::recursive_mutex> &lock, ExecutionJob *job,
      bool spawnNew);
  void executeJobsBatch(boost::unique_lock<boost::recursive_mutex> &lock,
      ExecutionJob *origJob, bool spawnNew);

  long stepInNode(boost::unique_lock<boost::recursive_mutex> &lock,
      WorkerTree::Node *node, long count);
  void replayPath(boost::unique_lock<boost::recursive_mutex> &lock,
      WorkerTree::Node *pathEnd, WorkerTree::Node *&brokenEnd);
  void cleanInvalidJobs(WorkerTree::Node *rootNode);

  void processLoop(bool allowGrowth, bool blocking, unsigned int timeOut);

  void refineStatistics();
  void cleanupStatistics();

  void selectJobs(WorkerTree::Node *root, std::vector<ExecutionJob*> &jobSet,
      int maxCount);

  unsigned int countJobs(WorkerTree::Node *root);

  void updateTreeOnBranch(klee::ExecutionState *state,
      klee::ExecutionState *parent, int index, klee::ForkTag forkTag);
  void updateTreeOnDestroy(klee::ExecutionState *state);

  void updateCompressedTreeOnBranch(SymbolicState *state, SymbolicState *parent);
  void updateCompressedTreeOnDestroy(SymbolicState *state);

  void fireBreakpointHit(WorkerTree::Node *node);

  /*
   * Breakpoint management
   */

  void setCodeBreakpoint(int assemblyLine);
  void setPathBreakpoint(ExecutionPathPin path);
public:
  JobManager(llvm::Module *module, std::string mainFnName, int argc,
      char **argv, char **envp);
  virtual ~JobManager();

  klee::Executor *getKleeExecutor() {
    return executor;
  }

  SymbolicEngine *getSymbEngine() {
    return symbEngine;
  }

  WorkerTree *getTree() {
    return tree;
  }

  WorkerTree::Node *getCurrentNode();

  JobSelectionStrategy *getStrategy() {
    return selStrategy;
  }

  StateSelectionStrategy *createBaseStrategy();

  void lockJobs() {
    jobsMutex.lock();
  }
  void unlockJobs() {
    jobsMutex.unlock();
  }

  unsigned getModuleCRC() const;

  void processJobs(bool standAlone, unsigned int timeOut = 0);
  void replayJobs(ExecutionPathSetPin paths, unsigned int timeOut = 0);

  void finalize();

  virtual bool onStateBranching(klee::ExecutionState *state, klee::ForkTag forkTag);
  virtual void onStateBranched(klee::ExecutionState *state,
			klee::ExecutionState *parent, int index, klee::ForkTag forkTag);
	virtual void onUpdateSearcherAfterBranch(const std::vector<klee::ExecutionState*> &states);
  virtual void onStateDestroy(klee::ExecutionState *state, bool silenced);
	virtual void onControlFlowEvent(klee::ExecutionState *state, ControlFlowEvent event,
      klee::ThreadUid targetTuid, klee::KInstruction *targetKinst,
      llvm::Function *externalCall);
  virtual void onDebugInfo(klee::ExecutionState *state,
      const std::string &message);
  virtual void onOutOfResources(klee::ExecutionState *destroyedState);
  virtual void onEvent(klee::ExecutionState *state, klee::EventClass type, uint64_t value);
  virtual void onIcsBegin(klee::ExecutionState *state);
  virtual void onIcsMaybeEndRegion(klee::ExecutionState *state, klee::ThreadUid tuid, bool force);

  // nops
	virtual void onConstraintAdded(klee::ExecutionState *state,
      klee::ref<klee::Expr> condition, klee::ForkClass forkclass)
  {}
	virtual void onMemoryAccess(klee::ExecutionState *state, const klee::MemoryObject *mo,
      klee::ref<klee::Expr> offset, klee::ref<klee::Expr> nbytes, bool isStore, bool isAlloc)
  {}
  virtual void onResolvedSymbolicPtr(klee::ExecutionState *kstate,
      klee::ref<klee::SymbolicPtrResolution> res)
  {}

  /*
   * Statistics methods
   */

  void getStatisticsData(std::vector<int> &data, ExecutionPathSetPin &paths,
      bool onlyChanged);

  unsigned int getJobCount() const { return jobCount; }

  void setRefineStatistics() {
    refineStats = true;
  }

  void requestTermination() {
    terminationRequest = true;
    // Wake up the manager
    jobsAvailabe.notify_all();
  }

  /*
   * Coverage related functionality
   */

  void getUpdatedLocalCoverage(cov_update_t &data);
  void setUpdatedGlobalCoverage(const cov_update_t &data);
  uint32_t getCoverageIDCount() const;

  /*
   * Job import/export methods
   */
  void importJobs(ExecutionPathSetPin paths, std::vector<long> &replayInstrs);
  ExecutionPathSetPin exportJobs(ExecutionPathSetPin seeds,
      std::vector<int> &counts, std::vector<long> &replayInstrs);

  /*
   * Debugging
   */

  // Must be inlined to workaround a stupid lib-order linking bug.
  inline __attribute__((always_inline))
  int getTraceCounter(const char *prefix) {
    return traceCounters[prefix];
  }

  // Must be inlined to workaround a stupid lib-order linking bug.
  inline __attribute__((always_inline))
  std::ostream* openTraceOutputFile(const char *prefix, const char *suffix,
                                    const char* comment) {
    char filename[256];
    int& counter = traceCounters[prefix];
    counter++;
    snprintf(filename, sizeof(filename), "%s%05d.%s", prefix, counter, suffix);
    CLOUD9_INFO("Dumping " << comment << " in file '" << filename << "'");
    return kleeHandler->openOutputFile(filename);
  }

  // Must be inlined to workaround a stupid lib-order linking bug.
  inline __attribute__((always_inline))
  std::ostream* openSummaryOutputFile(const char *filename, const char* comment) {
    CLOUD9_INFO("Dumping " << comment << " in file '" << filename << "'");
    return kleeHandler->openOutputFile(filename);
  }

  template<class Decorator>
  void dumpSymbolicTree(WorkerTree::Node *node,
                        Decorator decorator,
                        const char *fileprefix = "workertree") {
    std::ostream *os = openTraceOutputFile(fileprefix, "dot", "symbolic tree");
    assert(os != NULL);

    tree->dumpDotGraph(node, *os, decorator);

    delete os;
  }
};

}
}

#endif /* JOBMANAGER_H_ */
