############################################################
# FastRG coverage machinery (included by the top-level Makefile)
#
# Everything coverage-related lives here: the unit-test `coverage`
# target, the e2e instrumentation flow (`coverage-e2e-build` /
# `coverage-e2e-report`), the per-feature view (`coverage-features`)
# and their shared variables. All of it is on-demand measurement
# tooling — none of these targets take part in the commit gates.
#
# This file is included after CFLAGS/LDFLAGS and the build variables
# (TARGET, TESTDIR, GRPCDIR, CONTROLLERDIR, ...) are defined; it only
# appends to them. The COVERAGE / COVERAGE_E2E flag blocks inside the
# grpc and controller sub-Makefiles stay there, because they must take
# effect in those sub-builds.
############################################################

# E2E-instrumented production build, driven by `make coverage-e2e-build`
# (which re-invokes make with COVERAGE_E2E=1). Unlike the unit-test
# `coverage` target this keeps -O3 and TEST_MODE-only production semantics
# (no UNIT_TEST): the dp busy-poll behaviour would be distorted at -O0,
# so line numbers are fuzzier under -O3 but function-level coverage is
# exact. The flag propagates to the grpc / controller sub-makes through
# MAKEFLAGS so their objects are instrumented the same way.
#
# -fprofile-update=atomic is required, not optional: several data-plane
# lcores execute the same rx/tx loop code, so with plain (racy) counter
# increments the per-packet counters wrap negative (observed -40M on the
# dp.c rx loop after one e2e run), and geninfo then zeroes the line —
# flipping a hot, obviously-executed line to "not covered". Atomic updates
# keep covered/not-covered semantics correct on shared hot paths.
ifeq ($(COVERAGE_E2E),1)
CFLAGS += --coverage -fprofile-update=atomic
endif

COVERAGE_DIR = coverage
LCOV ?= lcov
GENHTML ?= genhtml
COVERAGE_OBJ_DIRS = -d src -d northbound -d unit_test
COVERAGE_FILTERS = '*/unit_test/*' '*/northbound/controller/test/*' '*/lib/*' '/usr/*' '*.pb.cc' '*.pb.h'

# Per-module summary computed straight from the tracefile LF/LH/FNF/FNH
# records (shared by the unit `coverage` and `coverage-e2e-report` targets).
# `lcov --list` is not used here: the lcov 2.0-1 shipped on Ubuntu 24.04
# prints broken Rate/Num columns (e.g. rates above 100%), while the records
# themselves are correct and match genhtml/--summary.
MODULE_SUMMARY_AWK = '/^SF:/ { f = $$2; m = "other"; \
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
      tlf ? 100 * tlh / tlf : 0, tlh, tlf, tff ? 100 * tfh / tff : 0, tfh, tff }'

######################################
# Unit-test coverage (on-demand measurement, not part of the commit gates)
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
		--output-file $(COVERAGE_DIR)/unit-merged.info
	$(LCOV) --ignore-errors unused --remove $(COVERAGE_DIR)/unit-merged.info \
		$(COVERAGE_FILTERS) \
		--output-file $(COVERAGE_DIR)/coverage.info
	$(LCOV) --summary $(COVERAGE_DIR)/coverage.info
	# Per-file summary straight from the tracefile records (see the
	# MODULE_SUMMARY_AWK comment for why `lcov --list` is avoided).
	@awk -F: '/^SF:/  { f = $$2; if (match(f, /\/(src|northbound)\/.*/)) f = substr(f, RSTART + 1) } \
	  /^LF:/  { lf = $$2 }  /^LH:/  { lh = $$2 } \
	  /^FNF:/ { fnf = $$2 } /^FNH:/ { fnh = $$2 } \
	  /^end_of_record/ { printf "%-48s lines: %5.1f%% (%4d/%4d)  functions: %5.1f%% (%3d/%3d)\n", f, \
	      lf ? 100 * lh / lf : 0, lh, lf, fnf ? 100 * fnh / fnf : 0, fnh, fnf; \
	      lf = lh = fnf = fnh = 0 }' \
	  $(COVERAGE_DIR)/coverage.info | sort | tee $(COVERAGE_DIR)/file-summary.txt
	@awk -F: $(MODULE_SUMMARY_AWK) \
	  $(COVERAGE_DIR)/coverage.info | tee $(COVERAGE_DIR)/module-summary.txt
	$(GENHTML) $(COVERAGE_DIR)/coverage.info --output-directory $(COVERAGE_DIR)/html
	find src northbound unit_test \( -name '*.gcda' -o -name '*.gcno' \) -delete
	$(MAKE) clean
	@echo "Coverage report: $(COVERAGE_DIR)/html/index.html"
	@echo "Per-module summary: $(COVERAGE_DIR)/module-summary.txt"

######################################
# E2E coverage (on-demand measurement, not part of the commit gates)
######################################
# Workflow:
#   1. make coverage-e2e-build   — production build (-O3, TEST_MODE, no
#      UNIT_TEST) with gcov instrumentation; the e2e suite then runs this
#      binary as usual. .gcda counters are flushed on every graceful exit
#      and accumulate across the restarts the suite performs.
#   2. e2e_test/run_e2e_test.sh  — run the suite normally (must be all-green
#      for the measurement to be meaningful).
#   3. make coverage-e2e-report  — capture the counters into
#      $(COVERAGE_DIR)/e2e.info; if $(COVERAGE_DIR)/coverage.info (the
#      filtered unit tracefile left by `make coverage`) exists, also merge
#      the two into $(COVERAGE_DIR)/combined.info. Per-module summaries are
#      written next to each tracefile.
#   4. make clean                — restore an uninstrumented tree (then
#      rebuild with plain `make` before any regular e2e/gate run).
#
# gcov note: the instrumented build uses -fprofile-update=atomic (see the
# COVERAGE_E2E block at the top of this file) because plain increments
# race between data-plane lcores, wrap negative on per-packet loops, and
# then get zeroed by geninfo — marking hot lines "not covered". The
# --ignore-errors negative on the capture below is kept as a safety net.
.PHONY: coverage-e2e-build
coverage-e2e-build:
	find src northbound unit_test \( -name '*.gcda' -o -name '*.gcno' \) -delete
	$(MAKE) COVERAGE_E2E=1 all
	@echo "Instrumented production binary ready: ./$(TARGET)"
	@echo "Run e2e_test/run_e2e_test.sh, then 'make coverage-e2e-report'."

.PHONY: coverage-e2e-report
coverage-e2e-report:
	mkdir -p $(COVERAGE_DIR)
	$(LCOV) --capture --initial -d src -d northbound --output-file $(COVERAGE_DIR)/e2e-baseline.info
	$(LCOV) --capture -d src -d northbound --ignore-errors negative \
		--output-file $(COVERAGE_DIR)/e2e-run.info
	$(LCOV) --add-tracefile $(COVERAGE_DIR)/e2e-baseline.info --add-tracefile $(COVERAGE_DIR)/e2e-run.info \
		--output-file $(COVERAGE_DIR)/e2e-merged.info
	$(LCOV) --ignore-errors unused --remove $(COVERAGE_DIR)/e2e-merged.info \
		$(COVERAGE_FILTERS) \
		--output-file $(COVERAGE_DIR)/e2e.info
	$(LCOV) --summary $(COVERAGE_DIR)/e2e.info
	@awk -F: $(MODULE_SUMMARY_AWK) \
	  $(COVERAGE_DIR)/e2e.info | tee $(COVERAGE_DIR)/e2e-module-summary.txt
	@if [ -f $(COVERAGE_DIR)/coverage.info ]; then \
	  $(LCOV) --add-tracefile $(COVERAGE_DIR)/coverage.info --add-tracefile $(COVERAGE_DIR)/e2e.info \
	    --output-file $(COVERAGE_DIR)/combined.info && \
	  $(LCOV) --summary $(COVERAGE_DIR)/combined.info && \
	  awk -F: $(MODULE_SUMMARY_AWK) \
	    $(COVERAGE_DIR)/combined.info | tee $(COVERAGE_DIR)/combined-module-summary.txt; \
	else \
	  echo "No $(COVERAGE_DIR)/coverage.info (unit data) — skipping combined.info; run 'make coverage' first to get it."; \
	fi
	@echo "E2E per-module summary: $(COVERAGE_DIR)/e2e-module-summary.txt"

# Per-feature coverage view: groups functions into product features by
# name/path rules (see e2e_test/feature_coverage.py). Uses combined
# (unit + e2e) data when available, otherwise falls back to the unit
# tracefile.
.PHONY: coverage-features
coverage-features:
	@src=$(COVERAGE_DIR)/combined.info; \
	if [ ! -f $$src ]; then src=$(COVERAGE_DIR)/coverage.info; fi; \
	if [ ! -f $$src ]; then \
	  echo "No tracefile found under $(COVERAGE_DIR)/ — run 'make coverage' (and optionally the e2e flow) first."; exit 1; \
	fi; \
	echo "Using tracefile: $$src"; \
	python3 e2e_test/feature_coverage.py $$src | tee $(COVERAGE_DIR)/feature-summary.txt
