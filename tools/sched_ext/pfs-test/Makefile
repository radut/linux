# SPDX-License-Identifier: GPL-2.0
# Static binaries so the same files work on the host and inside a FROM-scratch
# container image. Set STATIC= for dynamic linking.
CC ?= gcc
CFLAGS ?= -O2 -Wall
STATIC ?= -static

all: bin/spin bin/noisy bin/reqlat

bin/spin: spin.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(STATIC) -pthread -o $@ $<

# same program under another name, so the trace tells the neighbour apart
bin/noisy: bin/spin
	cp $< $@

bin/reqlat: reqlat.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(STATIC) -pthread -o $@ $<

clean:
	rm -f bin/spin bin/noisy bin/reqlat

.PHONY: all clean
