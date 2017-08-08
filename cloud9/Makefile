#===-- klee/Makefile ---------------------------------------*- Makefile -*--===#
#
#                     The KLEE Symbolic Virtual Machine
#
# This file is distributed under the University of Illinois Open Source
# License. See LICENSE.TXT for details.
#
#===------------------------------------------------------------------------===#

#
# Indicates our relative path to the top of the project's root directory.
#
LEVEL = .

include $(LEVEL)/Makefile.config

DIRS = lib
ifeq ($(ENABLE_EXT_STP),0)
  DIRS += stp
endif
DIRS += tools runtime
EXTRA_DIST = include

# Only build support directories when building unittests.
ifeq ($(MAKECMDGOALS),unittests)
  DIRS := $(filter-out tools runtime, $(DIRS)) unittests
  OPTIONAL_DIRS :=
endif

#
# Include the Master Makefile that knows how to build all.
#
include $(LEVEL)/Makefile.common

.PHONY: doxygen
doxygen:
	doxygen docs/doxygen.cfg

.PHONY: cscope.files
cscope.files:
	find \
          lib include stp tools runtime examples unittests test \
          -name Makefile -or \
          -name \*.in -or \
          -name \*.c -or \
          -name \*.cpp -or \
          -name \*.exp -or \
          -name \*.inc -or \
          -name \*.h | sort > cscope.files

test::
	-(cd test/ && make)

.PHONY: klee-cov
klee-cov:
	rm -rf klee-cov
	zcov-scan --look-up-dirs=1 klee.zcov .
	zcov-genhtml --root $$(pwd) klee.zcov klee-cov

clean::
	$(MAKE) -C test clean 
	$(MAKE) -C unittests clean
	rm -rf docs/doxygen

TAGS::
	@etags --extra=+fq --langmap='c++:+.inc.def'			\
		--regex-C='/DEFINE_MODEL\([^,]*, *([^,]*)[,)]/\1/'	\
		--regex-C='/DEFINE_PTHREAD_MODEL\([^,]*, *([^,]*),/\1/'	\
		--regex-C++='/DEFINE_MODEL\([^,]*, *([^,]*)[,)]/\1/'	\
		--regex-C++='/DEFINE_ICSFUNC\([^,]*, *([^,]*),/\1/'	\
		--regex-C++='/DEFINE_HANDLER\(([a-zA-Z0-9_]*)\)/\1/'	\
		`find include -name *.h`				\
		`find include lib -name *.def`				\
		`find lib tools runtime -name *.h`			\
		`find lib tools runtime -name *.cpp`			\
		`find lib tools runtime -name *.c`			\
		`find ../llvm-poolalloc/include/dsa -name *.h`		\
		`find ../llvm-poolalloc/lib/DSA -name *.cpp`		\
		`find $(LLVM_SRC_ROOT)/include -name *.h`		\
		`find $(LLVM_SRC_ROOT)/lib -name *.h`			\
		`find $(LLVM_SRC_ROOT)/lib -name *.cpp`			\
		`find $(LLVM_SRC_ROOT)/lib/Support -name *.inc` 	\
		`find $(STP_ROOT)/include -name *.h`
