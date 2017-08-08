#####################################################################
# Makefile for test cases
#
# use `make foo` to run test case foo
#####################################################################

ALLDIRS := $(shell find . -maxdepth 1 -mindepth 1 -type d | sed -e 's/^.\///')
ALLAPPS := $(patsubst %,%.run,$(ALLDIRS))

SYNCOPS := "\
__klee_pthread_model_pthread_create,\
__klee_pthread_model_pthread_join,\
__klee_pthread_model_pthread_detach,\
__klee_pthread_model_pthread_once,\
__klee_pthread_model_pthread_mutex_lock,\
__klee_pthread_model_pthread_mutex_trylock,\
__klee_pthread_model_pthread_mutex_unlock,\
__klee_pthread_model_pthread_cond_wait,\
__klee_pthread_model_pthread_cond_broadcast,\
__klee_pthread_model_pthread_cond_signal,\
__klee_pthread_model_pthread_rwlock_rdlock,\
__klee_pthread_model_pthread_rwlock_tryrdlock,\
__klee_pthread_model_pthread_rwlock_wrlock,\
__klee_pthread_model_pthread_rwlock_trywrlock,\
__klee_pthread_model_pthread_rwlock_unlock,\
__klee_pthread_model_pthread_barrier_wait,\
klee_lockset_add,\
klee_lockset_remove"
SYNCOPS := $(shell echo "$(SYNCOPS)" | sed -e 's/ //g')

################################################################################
# Build apps
################################################################################

# Boilerplate to use klee headers
Klee.Runtime.Inc := -D__UCLIBC_HAS_TLS__=1
Klee.Runtime.Inc += -I../../cloud9/include
Klee.Runtime.Inc += -I../../cloud9/runtime/POSIX/wrappers
Klee.Runtime.Inc += -include ../../uclibc/include/libc-symbols.h
Klee.Runtime.Inc += -isystem ../../uclibc/include
Klee.Runtime.Inc += -isystem ../../uclibc/include/libc/sysdeps/linux
Klee.Runtime.Inc += -isystem ../../uclibc/include/libc/sysdeps/linux/x86_64

# Boilerplate for enabling klee in these apps
Klee.Runtime.Def += -DENABLE_KLEE
Klee.Runtime.Def += -DENABLE_KLEE_PTHREAD_JOINALL           # optimization
Klee.Runtime.Def += -DENABLE_KLEE_MANUAL_LOOP_ABSTRACTIONS  # optimization (Sec 4.2 from OOPSLA'13 paper)

# Stupid hack that should be enabled only when building *.nondet targets
#Klee.Runtime.Def += -DENABLE_KLEE_PTHREAD_PADDING

# Macros to mark code that should execute only when klee is NOT running
Klee.Runtime.Def += -D"IF_NOT_RUNNING_KLEE_START=if (!klee_is_running()) {"
Klee.Runtime.Def += -D"IF_NOT_RUNNING_KLEE_ELSE=} else {"
Klee.Runtime.Def += -D"IF_NOT_RUNNING_KLEE_END=}"
Klee.Runtime.Def += -D"IF_RUNNING_KLEE_START=if (klee_is_running()) {"
Klee.Runtime.Def += -D"IF_RUNNING_KLEE_ELSE=} else {"
Klee.Runtime.Def += -D"IF_RUNNING_KLEE_END=}"

# For enabling ICS experiments
Klee.Runtime.Inc += -DENABLE_KLEE_EXPERIMENT=1
#Klee.Runtime.Inc += -DENABLE_KLEE_EXPERIMENT=2

export PARSECDIR := /dev/null
export version := pthreads

export CFLAGS := $(Klee.Runtime.Inc) $(Klee.Runtime.Def) --emit-llvm -S -g -O0
export CXXFLAGS := $(CFLAGS)
export CC := llvm-gcc
export CXX := llvm-g++
export CPP := llvm-cpp
export M4 := m4

.PRECIOUS: %.ll
.PRECIOUS: %.bc

%.build:
	@cd $* && $(MAKE)

%.clean:
	@cd $* && $(MAKE) clean

################################################################################
# Execute apps
################################################################################

OUTPUT_BASE_DIR := /nvtmp/build-tbergan/testing/scratch/apps-output

C9 := c9-worker --stand-alone --posix-runtime --libc=uclibc --no-truncate-source-lines

#####################################
# Klee options

C9 += --optimize
C9 += --disable-dead-arg-elim
C9 += --allow-external-sym-calls
#C9 += --check-div-zero=false
#C9 += --div-zero-checks-for-fp
C9 += --enable-address-bounds-checks=true
C9 += --object-cache-size-default=256
C9 += --object-cache-size-max=4096
#C9 += --opt-conservative-symbolic-sync
C9 += --opt-symbolic-thread-ids

C9 += --partial-eval-threadargs=generic
C9 += --partial-eval-max-loop-unroll=32
#C9 += --partial-eval-use-solver-for-branches
#C9 += --partial-eval-use-main-thread-only

C9 += --pc-full-read-exprs=false
C9 += --write-test-cases=false
C9 += --write-pcs=false
C9 += --write-pcs-on-error=false
C9 += --cloud9-instrumentation=false
C9 += --dump-states-on-halt=false
C9 += --cov-update-interval=0
C9 += --output-istats=false
#C9 += --output-stats=false

#C9 += --debug-call-history
#C9 += --debug-allocation-history
#C9 += --debug-forking
#C9 += --debug-thread-notify
#C9 += --debug-partial-eval
#C9 += --debug-partial-eval-detail
#C9 += --debug-partial-eval-sccs
C9 += --debug-print-array-addrs
#C9 += --debug-symbolic-ptr-resolution
#C9 += --debug-symbolic-ptr-memaccess
#C9 += --debug-symbolic-ptr-memaccess-failures
#C9 += --warn-raise-asm
#C9 += --verbose-linking

C9 += --enable-stp-badalloc-catching
C9 += --enable-stp-timeouts
C9 += --max-stp-time=30

#C9 += --debug-constraint-validity
#C9 += --debug-validate-solver
#C9 += --debug-cex-cache-check-binding
#C9 += --debug-stp-check-counterexamples
#C9 += --debug-stp-enable-verbose-debugging
#C9 += --use-independent-solver=false
#C9 += --use-cex-cache=false
#C9 += --use-cache=false
#C9 += --use-fast-cex-solver

#####################################
# ICS options

C9 += --do-input-covering-schedules
#C9 += --ics-do-lazy-region-covering

C9 += --ics-min-time-between-region-markers=0
C9 += --ics-opt-regions-cover-loop-once
C9 += --ics-opt-regions-ignore-trivial-loops
# WARNING: this is really just a hack to get nicer output for lu/fft
# This folds each single-pathed region into its following region
#C9 += --ics-opt-run-regions-while-acyclic
# WARNING: use for barrier-synchronized programs ONLY! (use for radix)
#C9 += --ics-opt-assume-regions-end-in-barriers

C9 += --ics-opt-skip-early-exit-branches
C9 += --ics-opt-shortest-path-first
#C9 += --ics-opt-no-single-threaded-scheduling
C9 += --ics-opt-no-concretization-constraints-in-slice
C9 += --ics-opt-ignore-uclibc-sync

C9 += --ics-instrum-do-count-instructions
C9 += --ics-instrum-opt-minimize-call-logging
C9 += --ics-instrum-opt-minimize-updates-on-read

#C9 += --ics-dump-path-constraints
#C9 += --ics-dump-symbolic-trees
#C9 += --ics-dump-slice-traces
#C9 += --ics-dump-slice-constraints
#C9 += --ics-dump-execution-traces
#C9 += --ics-dump-hbgraphs
#C9 += --ics-dump-execution-trace-stats
#C9 += --ics-dump-precond-slicing-stats
C9 += --ics-dump-region-marker-stats
#C9 += --ics-dump-region-marker-cfgs
#C9 += --ics-dump-region-symbolic-objects
#C9 += --ics-dump-symbolic-ptr-graphs
#C9 += --ics-dump-optimization-stats

C9 += --ics-debug-symbolic-paths
#C9 += --ics-debug-symbolic-paths-verbose
#C9 += --ics-debug-progress
#C9 += --ics-debug-sync-op-callers
#C9 += --ics-debug-may-happen-ba-thread-create
#C9 += --ics-debug-input-constraints
#C9 += --ics-debug-input-constraints-verbose
#C9 += --ics-debug-path-constraints
#C9 += --ics-debug-path-constraints-verbose
#C9 += --ics-debug-constraints
#C9 += --ics-debug-ctxswitch-events
#C9 += --ics-debug-precond-slicing
#C9 += --ics-debug-precond-slicing-verbose
#C9 += --ics-debug-symbolic-ptr-slicing
C9 += --ics-debug-region-markers
#C9 += --ics-debug-scheduling-optimizations
#C9 += --ics-debug-hb-graphs
#C9 += --ics-debug-instrumentor
#C9 += --ics-debug-ignore-uncovered-loops

#####################################
# ICS Experimentation options

# ----- Odd Execution Modes -----

# Bounded-depth exploration of a manually selected program context
# Use -DENABLE_KLEE_EXPERIMENT to select the context (see above)

# N.B.: ensure the following are disabled for bounded-depth experiements
#  --opt-conservative-symbolic-sync
#  --ics-opt-no-single-threaded-scheduling

# Start directly from that context
C9 += --ics-exec-mode=MiddleViaDataflow
C9 += --ics-do-precond-slice=false
C9 += --ics-do-region-formation=false
C9 += --ics-max-concrete-depth-per-path=18
#C9 += --ics-max-paths-per-region=500
#C9 += --fork-on-schedule
C9 += --hack-make-oob-symbolic

# Start from program entry ("whole program") mode (use "full" opts for this)
#C9 += --ics-exec-mode=MiddleViaConcretePath
#C9 += --ics-do-precond-slice=false
#C9 += --ics-do-region-formation=false
##C9 += --ics-max-paths-per-region=500
#C9 += --ics-max-concrete-depth-per-path=10
#C9 += --hack-make-oob-symbolic

# ----- Optimization Levels -----
# MUST uncomment one of these to run ICS
# The standard level is "full"

# Full:
#C9 += --symbolic-ptr-aliasing=ByAnalysis
#C9 += --partial-eval-enable-reaching-defs=true
#C9 += --enable-wait-queue-invariants=true

# -PTA:
C9 += --symbolic-ptr-aliasing=Any
C9 += --partial-eval-enable-reaching-defs=true
C9 += --enable-wait-queue-invariants=true

# -RD:
#C9 += --symbolic-ptr-aliasing=ByAnalysis
#C9 += --partial-eval-enable-reaching-defs=false
#C9 += --enable-wait-queue-invariants=true

# -SI:
#C9 += --symbolic-ptr-aliasing=ByAnalysis
#C9 += --enable-wait-queue-invariants=false
##define DISABLE_KLEE_SYNC_INVARIANTS in runtime/POSIX/config.h, then rebuild C9

# Naive:
#C9 += --symbolic-ptr-aliasing=Any
#C9 += --ics-do-region-state-via-dataflow=false
#C9 += --enable-wait-queue-invariants=false
##define DISABLE_KLEE_SYNC_INVARIANTS in runtime/POSIX/config.h, then rebuild C9

# This should be disabled when the instrumented output is not needed
C9 += --ics-do-instrumentor=false
# This fails with -SI and -PTA for some reason
C9 += --ics-always-replay-inputs-via-hb-graphs=false

#####################################
# LLVM options

C9 += --jump-threading-immobile=$(SYNCOPS)

#C9 += --debug-only=dsaa
#C9 += --ds-aa-dump-escaping-nodes
#C9 += --ds-aa-dump-memory-barriers
#C9 += --ds-aa-dump-callgraph
#C9 += --ds-aa-dump-inverse-callgraph
#C9 += --ds-aa-dump-callgraph-sccs
#C9 += --ds-aa-dump-final-dsgraphs
#C9 += --ds-aa-dump-final-steensgraph
#C9 += --ds-aa-dump-final-dsgraph-for=main
#C9 += --ds-aa-dump-final-dsgraph-for=_ZL8traverseP4LISTi

%.run: %.build
	@VERBOSE=1 ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.gdb: %.build
	@VERBOSE=1 USEGDB=1 ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.prof: %.build
	@VERBOSE=0 USEPROF=cpu ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.heapprof: %.build
	@VERBOSE=0 USEPROF=heap ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.heapcheck: %.build
	@VERBOSE=0 USECHECK=heap ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.valgrind: %.build
	@VERBOSE=1 USEVALGRIND=1 ./runapp.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

################################################################################
# Build the instrumented executable
################################################################################

FinalOptimizedBc := instrumentedLibReplacedOptimized.bc

.PRECIOUS: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
$(OUTPUT_BASE_DIR)/%/klee-last/test.exe: $(OUTPUT_BASE_DIR)/%/klee-last/$(FinalOptimizedBc)
	llvm-ld -lpthread -lm -native -o $@ $<

%.exe: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	@echo Built $<

################################################################################
# Build the uninstrumented executable
# N.B.: Need to enable -DENABLE_KLEE_PTHREAD_PADDING (see above)
################################################################################

NONDET_RUNTIME_LIB := ../cloud9/Release+Asserts/lib/libicsRuntimeNondetExec.bca
UCLIBC_RUNTIME_LIB := ../uclibc/lib/libc.a

.SECONDEXPANSION:
.PRECIOUS: %/nondet.exe
%/nondet.exe: %.build $(NONDET_RUNTIME_LIB)
	llvm-ld -lpthread -lm -native -o $@ $*/$*.bc $(NONDET_RUNTIME_LIB) $(UCLIBC_RUNTIME_LIB)

%.nondet.exe: %/nondet.exe
	@echo Built $<

################################################################################
# Misc
################################################################################

.PHONY: FORCE
FORCE:

.PHONY: all.run
all.run:: $(ALLAPPS)

.PHONY: clean
clean::
	@for f in $(ALLDIRS); do cd $$f && $(MAKE) clean; cd -; done
	rm -f $(patsubst %,%/nondet.exe,$(ALLDIRS))
	rm -f $(patsubst %,%/nondet.exe.bc,$(ALLDIRS))

.PHONY: clean.apps.outdir
clean.apps.outdir::
	rm -rf $(OUTPUT_BASE_DIR)
