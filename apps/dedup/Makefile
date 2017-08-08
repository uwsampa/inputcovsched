SOURCES := $(shell ls *.c)
SOURCES += $(shell ls openssl/*.c)
SOURCES += $(shell ls zlib/*.c)

HEADERS := $(shell ls *.h)
HEADERS += $(shell ls openssl/*.h)
HEADERS += $(shell ls zlib/*.h)

CFLAGS += -Wall -g -std=c99 -fno-strict-aliasing -D_XOPEN_SOURCE=600 -DSTDC
CFLAGS += -I .
PROG := dedup

all: $(PROG).ll

clean::
	rm -rf obj
	rm -f $(PROG).ll $(PROG).bc

obj/%.bc: %.c $(HEADERS)
	@mkdir -p obj/openssl
	@mkdir -p obj/zlib
	llvm-gcc $(CFLAGS) -c $< -o $@

$(PROG).bc: $(patsubst %.c,obj/%.bc,$(SOURCES))
	llvm-link $^ -o $@

$(PROG).ll: $(PROG).bc
	llvm-dis $<
