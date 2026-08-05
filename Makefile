############################################################
# FastRG makefile
############################################################

######################################
# Set variable
######################################	
CC = gcc
INCLUDE = -Inorthbound/grpc -Inorthbound/controller

BUILD_TIME := $(shell date '+%Y/%b/%d %H:%M:%S %Z')
GIT_COMMIT := $(shell git describe --always --dirty --tags)

DPDK_CFLAGS := $(shell pkg-config --cflags libdpdk)
CFLAGS = $(INCLUDE) -Wall -Werror -g $(DPDK_CFLAGS) -O3 -DALLOW_EXPERIMENTAL_API -DTEST_MODE #-Wextra -fsanitize=address

# GCC 12/13 emits a false-positive -Warray-bounds inside DPDK's inlined
# rte_memcpy (SSE/emmintrin.h path) at -O3: the 128-255 byte branch issues fixed
# 16-byte loads at offsets the compiler can't prove are unreached, so it wrongly
# flags copies into 128-byte buffers. This only happens on the non-AVX
# (generic/SSE4.2) DPDK baseline used for portable CI builds; a native build uses
# the AVX path and never trips it. Suppress only when DPDK is not built -march=native.
ifeq (,$(findstring -march=native,$(DPDK_CFLAGS)))
CFLAGS += -Wno-array-bounds
endif

LDFLAGS = $(shell pkg-config --static --libs libdpdk) -lutils -lconfig -luuid -Wl,--start-group -lstdc++ $(shell pkg-config --libs grpc++ protobuf jsoncpp) -letcd-cpp-api $(shell pkg-config --libs rdkafka) -laddress_sorting -lpthread -lpcap -larchive -Wl,--end-group

TARGET = fastrg
VERSION_H = src/version.h
SRC = $(wildcard src/*.c) $(wildcard src/pppd/*.c) $(wildcard src/dhcpd/*.c) $(wildcard src/dnsd/*.c)
OBJ = $(SRC:.c=.o)

GRPCDIR = northbound/grpc
GRPC_SRC = $(filter-out $(GRPCDIR)/%client.cpp, $(wildcard $(GRPCDIR)/*.cpp))
GRPC_OBJ = $(GRPC_SRC:.cpp=.o) ${GRPCDIR}/*pb.o

CONTROLLERDIR = northbound/controller
CONTROLLER_SRC = $(wildcard $(CONTROLLERDIR)/*.cpp)
CONTROLLER_OBJ = $(CONTROLLER_SRC:.cpp=.o) ${CONTROLLERDIR}/proto/*pb.o
TESTDIR = unit_test
TESTBIN = unit-tester

ifneq ($(shell pkg-config --exists libdpdk && echo 0),0)
$(error "no installation of DPDK found")
endif
	
.PHONY: $(TARGET)
all: clean $(TARGET)
######################################
# Compile & Link
# 	Must use \tab key after new line
######################################
$(VERSION_H):
	@echo "Generating $@"
	@echo "#ifndef VERSION_H"              >  $@
	@echo "#define VERSION_H"              >> $@
	@echo ""                               >> $@
	@echo "#define GIT_COMMIT_ID \"$(GIT_COMMIT)\"" >> $@
	@echo "#define BUILD_TIME   \"$(BUILD_TIME)\""  >> $@
	@echo ""                               >> $@
	@echo "#endif"                         >> $@

$(TARGET): $(VERSION_H) $(OBJ)
	${MAKE} -C $(GRPCDIR)
	${MAKE} -C $(CONTROLLERDIR)
	$(CC) $(CFLAGS) $(OBJ) $(GRPC_OBJ) $(CONTROLLER_OBJ) -o $(TARGET) $(LDFLAGS)

install:
	cp $(TARGET) /usr/local/bin/$(TARGET)

test: CFLAGS += -DUNIT_TEST
test: clean $(TARGET)
	${MAKE} -C $(TESTDIR) $(TESTBIN)
	./$(TESTDIR)/$(TESTBIN)
	${MAKE} -C $(CONTROLLERDIR) run-tests

######################################
# Coverage
######################################
# `make coverage` measures real line/function coverage of the unit test run:
# it rebuilds the same UNIT_TEST tree as `make test`, but at -O0 (replacing
# -O3, so gcov line data maps 1:1 to source lines) with gcov instrumentation
# (--coverage), runs the C unit suite and the controller (C++) test suite,
# then produces an lcov report under $(COVERAGE_DIR)/.
#
# COVERAGE=1 is exported so the grpc / controller / unit_test sub-makes add
# the same instrumentation to their own flags (see their Makefiles).
#
# An --initial baseline capture is merged in so files that are compiled but
# never executed by the tests still show up as 0% instead of being silently
# omitted. The report filters out test harness code (unit_test/), vendored
# libraries (lib/), system headers and generated protobuf sources.
#
# The target ends by deleting every .gcda/.gcno and running a full clean, so
# a subsequent `make` / `make test` starts from a pristine, uninstrumented
# tree; the only artifacts left behind live in $(COVERAGE_DIR)/.
COVERAGE_DIR = coverage
LCOV ?= lcov
GENHTML ?= genhtml
COVERAGE_OBJ_DIRS = -d src -d northbound -d unit_test

.PHONY: coverage
coverage: export COVERAGE = 1
# -Wno-format-truncation: at -O0 GCC's value-range analysis is weaker than at
# -O3, so -Wformat-truncation fires on snprintf calls (e.g. src/dbg.c LOGGER)
# that the regular -O3 -Werror build proves safe and accepts. Suppress it only
# in this measurement build; the production build keeps full -Werror.
coverage: CFLAGS := $(filter-out -O3,$(CFLAGS)) -O0 --coverage -Wno-format-truncation -DUNIT_TEST
# -lcpprest: at -O0 the pplx/cpprest calls in etcd_client.cpp are no longer
# inlined away, so the linker needs libcpprest spelled out explicitly (the
# regular -O3 build resolves everything through -letcd-cpp-api alone).
coverage: LDFLAGS += -lcpprest
coverage: clean $(TARGET)
	${MAKE} -C $(TESTDIR) $(TESTBIN)
	./$(TESTDIR)/$(TESTBIN)
	${MAKE} -C $(CONTROLLERDIR) run-tests
	rm -rf $(COVERAGE_DIR)
	mkdir -p $(COVERAGE_DIR)
	$(LCOV) --capture --initial $(COVERAGE_OBJ_DIRS) --output-file $(COVERAGE_DIR)/baseline.info
	$(LCOV) --capture $(COVERAGE_OBJ_DIRS) --output-file $(COVERAGE_DIR)/tests.info
	$(LCOV) --add-tracefile $(COVERAGE_DIR)/baseline.info --add-tracefile $(COVERAGE_DIR)/tests.info \
		--output-file $(COVERAGE_DIR)/combined.info
	$(LCOV) --ignore-errors unused --remove $(COVERAGE_DIR)/combined.info \
		'*/unit_test/*' '*/northbound/controller/test/*' '*/lib/*' '/usr/*' '*.pb.cc' '*.pb.h' \
		--output-file $(COVERAGE_DIR)/coverage.info
	$(LCOV) --summary $(COVERAGE_DIR)/coverage.info
	# Per-file summary straight from the tracefile records. `lcov --list`
	# is not used here: the lcov 2.0-1 shipped on Ubuntu 24.04 prints
	# broken Rate/Num columns (e.g. rates above 100%), while the LF/LH/
	# FNF/FNH records themselves are correct and match genhtml/--summary.
	@awk -F: '/^SF:/  { f = $$2; if (match(f, /\/(src|northbound)\/.*/)) f = substr(f, RSTART + 1) } \
	  /^LF:/  { lf = $$2 }  /^LH:/  { lh = $$2 } \
	  /^FNF:/ { fnf = $$2 } /^FNH:/ { fnh = $$2 } \
	  /^end_of_record/ { printf "%-48s lines: %5.1f%% (%4d/%4d)  functions: %5.1f%% (%3d/%3d)\n", f, \
	      lf ? 100 * lh / lf : 0, lh, lf, fnf ? 100 * fnh / fnf : 0, fnh, fnf; \
	      lf = lh = fnf = fnh = 0 }' \
	  $(COVERAGE_DIR)/coverage.info | sort | tee $(COVERAGE_DIR)/file-summary.txt
	@awk -F: '/^SF:/ { f = $$2; m = "other"; \
	    if      (f ~ /\/src\/pppd\//)              m = "src/pppd"; \
	    else if (f ~ /\/src\/dhcpd\//)             m = "src/dhcpd"; \
	    else if (f ~ /\/src\/dnsd\//)              m = "src/dnsd"; \
	    else if (f ~ /\/src\//)                    m = "src (core)"; \
	    else if (f ~ /\/northbound\/grpc\//)       m = "northbound/grpc"; \
	    else if (f ~ /\/northbound\/controller\//) m = "northbound/controller"; \
	    else if (f ~ /\/northbound\/cmdline\//)    m = "northbound/cmdline"; \
	    cur = m; mods[m] = 1 } \
	  /^LF:/  { lf[cur]  += $$2 } /^LH:/  { lh[cur]  += $$2 } \
	  /^FNF:/ { fnf[cur] += $$2 } /^FNH:/ { fnh[cur] += $$2 } \
	  END { printf "%-24s %18s %18s\n", "Module", "Lines", "Functions"; \
	    n = split("src (core)|src/pppd|src/dhcpd|src/dnsd|northbound/grpc|northbound/controller|northbound/cmdline|other", order, "|"); \
	    for (i = 1; i <= n; i++) { m = order[i]; if (!(m in mods)) continue; \
	      lr = lf[m]  ? 100 * lh[m]  / lf[m]  : 0; \
	      fr = fnf[m] ? 100 * fnh[m] / fnf[m] : 0; \
	      printf "%-24s %6.1f%% (%d/%d) %6.1f%% (%d/%d)\n", m, lr, lh[m], lf[m], fr, fnh[m], fnf[m]; \
	      tlf += lf[m]; tlh += lh[m]; tff += fnf[m]; tfh += fnh[m] } \
	    printf "%-24s %6.1f%% (%d/%d) %6.1f%% (%d/%d)\n", "Total", \
	      tlf ? 100 * tlh / tlf : 0, tlh, tlf, tff ? 100 * tfh / tff : 0, tfh, tff }' \
	  $(COVERAGE_DIR)/coverage.info | tee $(COVERAGE_DIR)/module-summary.txt
	$(GENHTML) $(COVERAGE_DIR)/coverage.info --output-directory $(COVERAGE_DIR)/html
	find src northbound unit_test \( -name '*.gcda' -o -name '*.gcno' \) -delete
	$(MAKE) clean
	@echo "Coverage report: $(COVERAGE_DIR)/html/index.html"
	@echo "Per-module summary: $(COVERAGE_DIR)/module-summary.txt"

######################################
# Clean 
######################################
clean:
	rm -rf $(OBJ) $(TARGET) .libs $(VERSION_H)
	$(MAKE) -C $(TESTDIR) -f Makefile $@
	$(MAKE) -C $(GRPCDIR) -f Makefile $@
	$(MAKE) -C $(CONTROLLERDIR) -f Makefile $@

uninstall:
	rm -f /usr/local/bin/$(TARGET)
