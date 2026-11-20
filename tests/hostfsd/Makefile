# Exercises the hostfs protocol with no emulator in sight -- which is
# the whole reason the host side was built first: a cycle here is
# milliseconds instead of a boot.
#
# The test spawns the helper itself, so `make test` needs nothing set up.

CFLAGS = -Wall -Wextra -O1 -I../../tools/hostfsd

HELPER = ../../tools/hostfsd/shinogi-hostfsd

test: test_hostfsd $(HELPER) slowsync.so
	./test_hostfsd

# Linux: makes fsync and syncfs slow, for the test of a posted SYNC.
slowsync.so: slowsync.c
	$(CC) -Wall -Wextra -O1 -shared -fPIC -o $@ slowsync.c -ldl

test_hostfsd: test_hostfsd.c ../../tools/hostfsd/shinogi-hostfs-proto.h
	$(CC) $(CFLAGS) -o $@ test_hostfsd.c

$(HELPER): ../../tools/hostfsd/shinogi-hostfsd.c \
           ../../tools/hostfsd/shinogi-hostfs-proto.h
	$(MAKE) -C ../../tools/hostfsd

clean:
	python3 -c "import os; [os.remove(f) for f in ('test_hostfsd', 'slowsync.so') if os.path.exists(f)]"

.PHONY: test clean
