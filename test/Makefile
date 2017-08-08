#####################################################################
# Makefile for test cases
#
# use `make foo.test` to run test case foo
#####################################################################

ALLDIRS = $(shell find . -maxdepth 1 -mindepth 1 -type d | sed -e 's/^.\///' | sort)
PTRDIRS = $(shell find . -maxdepth 1 -mindepth 1 -type d -name '*ptr*' | sed -e 's/^.\///' | sort)
NONPTRDIRS = $(shell find . -maxdepth 1 -mindepth 1 -type d -not -name '*ptr*' | sed -e 's/^.\///' | sort)

ALLTESTS      = $(patsubst %,%.test,$(ALLDIRS))
ALLTESTS_LOUD = $(patsubst %,%.test.loud,$(ALLDIRS))
ALLTESTS_GDB  = $(patsubst %,%.test.gdb,$(ALLDIRS))

PTRTESTS      = $(patsubst %,%.test,$(PTRDIRS))
PTRTESTS_LOUD = $(patsubst %,%.test.loud,$(PTRDIRS))
PTRTESTS_GDB  = $(patsubst %,%.test.gdb,$(PTRDIRS))

NONPTREXE          = $(patsubst %,%.exe,$(NONPTRDIRS))
NONPTREXE_RUNLOUD  = $(patsubst %,%.exe.run.timed,$(NONPTRDIRS))
NONPTREXE_RUNQUIET = $(patsubst %,%.exe.run.timedquiet,$(NONPTRDIRS))

# Boilerplate to use klee headers
Klee.Runtime.Inc := -D__UCLIBC_HAS_TLS__=1
Klee.Runtime.Inc += -I../cloud9/runtime/POSIX/wrappers
Klee.Runtime.Inc += -I../cloud9/include
Klee.Runtime.Inc += -include ../uclibc/include/libc-symbols.h
Klee.Runtime.Inc += -isystem ../uclibc/include
Klee.Runtime.Inc += -isystem ../uclibc/include/libc/sysdeps/linux
Klee.Runtime.Inc += -isystem ../uclibc/include/libc/sysdeps/linux/x86_64

#
# Build source files
#

CFLAGS := $(Klee.Runtime.Inc) --emit-llvm -S -g -O3

.PRECIOUS: %.ll
.PRECIOUS: %.bc

%.ll: %.cpp
	llvm-gcc $(CFLAGS) $< -o $@

%.bc: %.ll
	llvm-as $< -o $@

#
# Execute tests
# NOTE: tests can specify extra args with "//cl:" (see runtest.py)
#

OUTPUT_BASE_DIR := /nvtmp/build-tbergan/testing/scratch/tests-output

C9 := c9-worker --stand-alone --posix-runtime --libc=uclibc --no-truncate-source-lines

# Testing-only options
C9 += --use-stable-array-hashing
C9 += --ics-sort-path-constraint-terms

# Klee options
# NOTE: We do NOT specify --optimize for testing

C9 += --symbolic-ptr-aliasing=ByAnalysis
C9 += --opt-symbolic-thread-ids

C9 += --partial-eval-threadargs=generic
C9 += --partial-eval-max-loop-unroll=32
#C9 += --partial-eval-use-solver-for-branches
#C9 += --partial-eval-use-main-thread-only

C9 += --write-test-cases=false
C9 += --cloud9-instrumentation=false
C9 += --dump-states-on-halt=false
C9 += --cov-update-interval=0
C9 += --output-istats=false
#C9 += --output-stats=false

#C9 += --debug-call-history
#C9 += --debug-allocation-history
#C9 += --debug-forking
C9 += --debug-thread-notify
#C9 += --debug-partial-eval
#C9 += --debug-partial-eval-detail
#C9 += --debug-partial-eval-sccs
#C9 += --debug-print-array-addrs
#C9 += --debug-symbolic-ptr-resolution
#C9 += --debug-symbolic-ptr-memaccess
#C9 += --warn-raise-asm
#C9 += --verbose-linking

C9 += --enable-stp-badalloc-catching
#C9 += --enable-stp-timeouts
#C9 += --max-stp-time=30

C9 += --debug-constraint-validity
C9 += --debug-validate-solver
C9 += --debug-cex-cache-check-binding
C9 += --debug-stp-check-counterexamples
#C9 += --debug-stp-enable-verbose-debugging
#C9 += --use-independent-solver=false
#C9 += --use-cex-cache=false
#C9 += --use-cache=false
#C9 += --use-fast-cex-solver

# ICS options
# NOTE: The "--ics-opt" flags are carefully chosen here for tests
# NOTE: Specific tests add more "--ics-opt" flags

C9 += --do-input-covering-schedules
#C9 += --ics-max-paths-per-region=0

C9 += --ics-opt-skip-early-exit-branches
C9 += --ics-opt-ignore-uclibc-sync
C9 += --ics-instrum-opt-minimize-call-logging
C9 += --ics-instrum-opt-minimize-updates-on-read

C9 += --ics-dump-path-constraints
#C9 += --ics-dump-symbolic-trees
#C9 += --ics-dump-slice-traces
C9 += --ics-dump-slice-constraints
#C9 += --ics-dump-execution-traces
C9 += --ics-dump-hbgraphs
#C9 += --ics-dump-execution-trace-stats
#C9 += --ics-dump-precond-slicing-stats
#C9 += --ics-dump-region-marker-stats
#C9 += --ics-dump-region-marker-cfgs
#C9 += --ics-dump-region-symbolic-objects
C9 += --ics-dump-symbolic-ptr-graphs
#C9 += --ics-dump-optimization-stats

C9 += --ics-debug-symbolic-paths
#C9 += --ics-debug-symbolic-paths-verbose
#C9 += --ics-debug-sync-op-callers
#C9 += --ics-debug-may-happen-ba-thread-create
C9 += --ics-debug-input-constraints
#C9 += --ics-debug-input-constraints-verbose
C9 += --ics-debug-path-constraints
#C9 += --ics-debug-path-constraints-verbose
#C9 += --ics-debug-constraints
#C9 += --ics-debug-ctxswitch-events
#C9 += --ics-debug-precond-slicing
#C9 += --ics-debug-precond-slicing-verbose
#C9 += --ics-debug-symbolic-ptr-slicing
#C9 += --ics-debug-region-markers
#C9 += --ics-debug-scheduling-optimizations
#C9 += --ics-debug-hb-graphs
C9 += --ics-debug-instrumentor
#C9 += --ics-debug-ignore-uncovered-loops

# LLVM options

#C9 += --debug-only=dsaa
#C9 += --ds-aa-dump-escaping-nodes
#C9 += --ds-aa-dump-memory-barriers
#C9 += --ds-aa-dump-callgraph
#C9 += --ds-aa-dump-inverse-callgraph
#C9 += --ds-aa-dump-callgraph-sccs
#C9 += --ds-aa-dump-final-dsgraphs
#C9 += --ds-aa-dump-final-steensgraph
#C9 += --ds-aa-dump-final-dsgraph-for=_Z3runPv
#C9 += --ds-aa-dump-final-dsgraph-for=_ZL8traverseP4LISTi

%.test: %/test.bc
	@VERBOSE=0 ./runtest.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.test.loud: %/test.bc
	@VERBOSE=1 ./runtest.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.test.gdb: %/test.bc
	@VERBOSE=1 USEGDB=1 ./runtest.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.test.prof: %/test.bc
	@VERBOSE=0 USEPROF=cpu ./runtest.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

%.test.heapprof: %/test.bc
	@VERBOSE=0 USEPROF=heap ./runtest.py $* "$(C9)" "$(OUTPUT_BASE_DIR)/$*"

#
# Build the instrumented executable
#

FinalOptimizedBc := instrumentedLibReplacedOptimized.bc

.PRECIOUS: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
$(OUTPUT_BASE_DIR)/%/klee-last/test.exe: $(OUTPUT_BASE_DIR)/%/klee-last/$(FinalOptimizedBc)
	llvm-ld -lpthread -native -o $@ $<

%.exe: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	@echo Built $<

%.exe.run: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	$<

%.exe.run.timed: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	@VERBOSE=1 TIMEOUT=30 ./runtimed.py $<

%.exe.run.timedquiet: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	@VERBOSE=0 TIMEOUT=30 ./runtimed.py $<

%.exe.gdb: $(OUTPUT_BASE_DIR)/%/klee-last/test.exe
	gdb $<

#
# Build the uninstrumented executable
#

NONDET_RUNTIME_LIB := ../cloud9/Release+Asserts/lib/libicsRuntimeNondetExec.bca

.PRECIOUS: %/test.nondet.exe
%/test.nondet.exe: %/test.bc $(NONDET_RUNTIME_LIB)
	llvm-ld -lpthread -native -o $@ $^

%.nondet.exe: %/test.nondet.exe
	@echo Built $<

%.nondet.exe.run: %/test.nondet.exe
	KLEE_ENABLE_DEBUG_PRINTS=1 $<

%.nondet.exe.gdb: %/test.nondet.exe
	KLEE_ENABLE_DEBUG_PRINTS=1 gdb $<

#
# Misc
#

.PHONY: all.test
all.test:: $(ALLTESTS)

.PHONY: all.test.loud
all.test.loud:: $(ALLTESTS_LOUD)

.PHONY: allptr.test
allptr.test:: $(PTRTESTS)

.PHONY: allptr.test.loud
allptr.test.loud:: $(PTRTESTS_LOUD)

# Can't exec the ptr tests (they would deref random ptr vals)
.PHONY: all.exe.run
all.exe.run:: $(NONPTREXE_RUNQUIET)

.PHONY: all.exe.loud
all.exe.run.loud:: $(NONPTREXE_RUNLOUD)

.PHONY: clean
clean::
	rm -f $(patsubst %,%/test.bc,$(ALLDIRS))
	rm -f $(patsubst %,%/test.ll,$(ALLDIRS))
	rm -f $(patsubst %,%/test.nondet.exe,$(ALLDIRS))
	rm -f $(patsubst %,%/test.nondet.exe.bc,$(ALLDIRS))

.PHONY: clean.tests.outdir
clean.tests.outdir::
	rm -rf $(OUTPUT_BASE_DIR)
