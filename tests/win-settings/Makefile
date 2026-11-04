CC = cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2
SOURCES = test_settings.c ../../tools/win/resolution.c

test: test_settings
	./test_settings

test_settings: $(SOURCES) ../../tools/win/resolution.h ../../tools/hostfsd/windows-path.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@

.PHONY: test
