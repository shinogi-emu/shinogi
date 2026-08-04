# Builds the host end of the hostfs link natively.
#
# Nothing but libc, so the same rule serves a cross build: point CC at a
# mingw-w64 compiler and the Windows branch of the transport is what
# gets compiled -- the program is otherwise the same on both.

CFLAGS ?= -Wall -Wextra -O2
LDFLAGS ?=

BIN = shinogi-hostfsd

all: $(BIN)

$(BIN): shinogi-hostfsd.c shinogi-hostfs-proto.h
	$(CC) $(CFLAGS) -o $@ shinogi-hostfsd.c $(LDFLAGS)

test: $(BIN)
	$(MAKE) -C ../../tests/hostfsd test

clean:
	python3 -c "import os; os.path.exists('$(BIN)') and os.remove('$(BIN)')"

.PHONY: all test clean
