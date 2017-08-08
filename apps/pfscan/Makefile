SOURCES := $(shell ls *.c)
HEADERS := $(shell ls *.h)

CFLAGS += -Wall -g
PROG := pfscan

all: $(PROG).ll

clean::
	rm -rf obj
	rm -f $(PROG).ll $(PROG).bc

obj/%.bc: %.c $(HEADERS)
	@mkdir -p obj
	llvm-gcc $(CFLAGS) -c $< -o $@

$(PROG).bc: $(patsubst %.c,obj/%.bc,$(SOURCES))
	llvm-link $^ -o $@

$(PROG).ll: $(PROG).bc
	llvm-dis $<
