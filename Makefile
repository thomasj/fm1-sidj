# SPDX-License-Identifier: GPL-3.0-only
# Host-side targets of SiDJ: the SID engine tests and the host player (the firmware builds
# with ./build.sh, the simulator and the rest of the tests with tests/run_tests.sh).
CC ?= cc
CXX ?= c++
CFLAGS ?= -O2 -Wall -Wextra -std=c11
CXXFLAGS ?= -O2 -w -DVERSION='"0.16"'
RESID := third_party/resid-0.16
B := build/host
C64 := firmware/src/c64

RESID_SRC := $(addprefix $(RESID)/, envelope.cc extfilt.cc filter.cc pot.cc sid.cc voice.cc wave.cc \
             wave6581_PST.cc wave6581_PS_.cc wave6581_P_T.cc wave6581__ST.cc \
             wave8580_PST.cc wave8580_PS_.cc wave8580_P_T.cc wave8580__ST.cc)

.PHONY: all test sidplay clean tables
all: $(B)/sid_ref_test $(B)/sidplay

$(B):
	mkdir -p $(B)

$(B)/sid.o: sid/sid.c sid/sid.h sid/sid_tables.h | $(B)
	$(CC) $(CFLAGS) -Isid -c $< -o $@

$(B)/sid_tables.o: sid/sid_tables.c sid/sid_tables.h | $(B)
	$(CC) $(CFLAGS) -Isid -c $< -o $@

$(B)/libresid016.a: $(RESID_SRC) | $(B)
	mkdir -p $(B)/resid
	for f in $(RESID_SRC); do $(CXX) $(CXXFLAGS) -I$(RESID) -c $$f -o $(B)/resid/$$(basename $$f .cc).o || exit 1; done
	ar rcs $@ $(B)/resid/*.o

$(B)/sid_ref_test: tests/sid_ref_test.cc $(B)/sid.o $(B)/sid_tables.o $(B)/libresid016.a
	$(CXX) $(CXXFLAGS) -Isid -I$(RESID) $< $(B)/sid.o $(B)/sid_tables.o $(B)/libresid016.a -o $@

$(B)/sidplay: host/sidplay.c $(C64)/c64.c $(C64)/cpu6502.c $(C64)/sidfile.c $(B)/sid.o $(B)/sid_tables.o
	$(CC) $(CFLAGS) -Isid -I$(C64) host/sidplay.c $(C64)/c64.c $(C64)/cpu6502.c $(C64)/sidfile.c $(B)/sid.o $(B)/sid_tables.o -o $@

test: $(B)/sid_ref_test
	$(B)/sid_ref_test

sidplay: $(B)/sidplay

tables:
	tools/gen_sid_tables.sh

clean:
	rm -rf build/host
