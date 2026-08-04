# Exercises the hostfs protocol with no emulator in sight -- which is
# the whole reason the host side was built first: a cycle here is
# milliseconds instead of a boot.
#
# The test spawns the helper itself, so `make test` needs nothing set up.

CFLAGS = -Wall -Wextra -O1 -I../../tools/hostfsd

HELPER = ../../tools/hostfsd/shinogi-hostfsd

test: test_hostfsd $(HELPER)
	./test_hostfsd

test_hostfsd: test_hostfsd.c ../../tools/hostfsd/shinogi-hostfs-proto.h
	$(CC) $(CFLAGS) -o $@ test_hostfsd.c

$(HELPER): ../../tools/hostfsd/shinogi-hostfsd.c \
           ../../tools/hostfsd/shinogi-hostfs-proto.h
	$(MAKE) -C ../../tools/hostfsd

clean:
	python3 -c "import os; os.path.exists('test_hostfsd') and os.remove('test_hostfsd')"

.PHONY: test clean
