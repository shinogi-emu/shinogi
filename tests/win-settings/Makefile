CC = cc
CFLAGS = -std=c99 -Wall -Wextra -Werror -O2
SOURCES = test_settings.c ../../tools/win/resolution.c

test: test_settings test_network
	./test_settings
	./test_network

test_settings: $(SOURCES) ../../tools/win/resolution.h ../../tools/hostfsd/windows-path.h
	$(CC) $(CFLAGS) $(SOURCES) -o $@

test_network: test_network.c ../../tools/win/network-config.c ../../tools/win/network-config.h
	$(CC) $(CFLAGS) test_network.c ../../tools/win/network-config.c -o $@

.PHONY: test
