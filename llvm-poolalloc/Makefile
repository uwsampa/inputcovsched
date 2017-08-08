#
# This is a sample Makefile for a project that uses LLVM.
#

#
# Indicates our relative path to the top of the project's root directory.
#
LEVEL = .

#
# Directories that needs to be built.
#
#DIRS = lib tools
DIRS = lib

#
# Include the Master Makefile that knows how to build all.
#
-include $(LEVEL)/Makefile.common

notconfigured:
	@echo "ERROR: You must configure this project before you can use it!"
	@exit 1

distclean:: clean
	${RM} -f Makefile.common Makefile.config

TAGS::
	@etags --extra=+f					\
		`find include/dsa -name *.h`			\
		`find lib/DSA -name *.h`			\
		`find lib/DSA -name *.cpp`			\
		`find $(LLVM_SRC_ROOT)/include -name *.h`	\
		`find $(LLVM_SRC_ROOT)/lib -name *.h`		\
		`find $(LLVM_SRC_ROOT)/lib -name *.cpp`
