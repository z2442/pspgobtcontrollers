.DEFAULT_GOAL := all
PSPGOBT_DEBUG ?= 0
PSPGOBT_SESSION_TRACE ?= 0
ifneq ($(PSPGOBT_SESSION_TRACE),0)
ifneq ($(PSPGOBT_SESSION_TRACE),1)
$(error PSPGOBT_SESSION_TRACE must be 0 or 1)
endif
endif
ifneq ($(PSPGOBT_DEBUG),0)
ifneq ($(PSPGOBT_DEBUG),1)
$(error PSPGOBT_DEBUG must be 0 or 1)
endif
endif

# Host-side protocol and patch checks do not require PSPSDK.
HOST_CC ?= cc
HOST_CFLAGS := -std=c99 -O2 -Wall -Wextra -Werror -pedantic
.PHONY: test test-mips test-bridge-mips package protocol
test:
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -Iinclude tests/test_core.c src/dualsense.c src/input_state.c src/xmb_patch.c src/probe_policy.c src/hid_route.c src/pairing_route.c src/report_bridge.c src/target_cache.c -o build/test_core
	./build/test_core
	python3 tests/test_inspect.py

test-mips:
	mkdir -p build
	$(HOST_CC) $(HOST_CFLAGS) -shared -fPIC -Iinclude src/hid_route.c src/pairing_route.c src/report_bridge.c src/dualsense.c -o build/libhidroute_host.so
	python3 tests/test_route_emulation.py
	python3 tests/test_hid_mode_execution.py

ifeq ($(strip $(filter test test-mips,$(MAKECMDGOALS))),)
TARGET = pspgobt_probe
OBJS = src/plugin.o src/xmb_patch.o src/dualsense.o src/input_state.o src/native_sink.o src/probe_policy.o src/bsman_trace.o src/hid_route.o src/xmb_hid_route.o src/pairing_route.o src/xmb_pair_route.o src/pair_command_trace.o src/report_bridge.o src/bt_input_bridge.o src/target_cache.o src/session_trace.o
INCDIR = include
DEPFLAGS = -MMD -MP
CFLAGS = -std=gnu99 -Os -G0 -Wall -Wextra -Werror -fno-pic
CFLAGS += $(DEPFLAGS) -DPSPGOBT_DEBUG=$(PSPGOBT_DEBUG) -DPSPGOBT_SESSION_TRACE=$(PSPGOBT_SESSION_TRACE)
ASFLAGS = $(CFLAGS)
BUILD_PRX = 1
PRX_EXPORTS = exports.exp
USE_KERNEL_LIBS = 1
USE_KERNEL_LIBC = 1
LDFLAGS = -nostartfiles
LIBS = -lpspsystemctrl_kernel
PSPSDK := $(shell psp-config --pspsdk-path)
include $(PSPSDK)/lib/build.mak

all: protocol

# Switching modes must rebuild objects even if no source file has changed.
PROFILE_STAMP := build/.pspgobt-profile-$(PSPGOBT_DEBUG)-$(PSPGOBT_SESSION_TRACE)
$(PROFILE_STAMP):
	mkdir -p build
	rm -f build/.pspgobt-profile-*
	touch $@

$(OBJS): $(PROFILE_STAMP)

test-bridge-mips: all
	python3 tests/test_bridge_execution.py
	python3 tests/test_sink_execution.py
	python3 tests/test_reconnect_execution.py
	python3 tests/test_cached_record_execution.py
	python3 tests/test_install_execution.py
	python3 tests/test_transport_rebind_execution.py
	python3 tests/test_controller_execution.py
	python3 tests/test_shutdown_execution.py
	python3 tests/test_session_execution.py
	python3 tests/test_hid_mode_kernel_execution.py
	python3 tests/test_manager_session_execution.py
	python3 tests/test_connect_epoch_execution.py
	python3 tests/test_input_scheduling_execution.py
	python3 tests/test_session_trace_execution.py
	python3 tests/test_build_profile.py --debug $(PSPGOBT_DEBUG) --session-trace $(PSPGOBT_SESSION_TRACE)

protocol: build/libpspgobt_protocol.a

build/libpspgobt_protocol.a: src/dualsense.o src/input_state.o
	mkdir -p build
	psp-ar rcs $@ $^

EXTRA_CLEAN += build/libpspgobt_protocol.a src/dualsense.o build/.pspgobt-debug-0 build/.pspgobt-debug-1 build/.pspgobt-profile-*
-include $(OBJS:.o=.d)

package: all
	mkdir -p dist/seplugins/pspgobt
	cp pspgobt_probe.prx dist/seplugins/pspgobt/
	cp packaging/PLUGINS.TXT.example dist/seplugins/pspgobt/
	cp README.md dist/
	cp CHANGELOG.md dist/
	cp LICENSE THIRD_PARTY_NOTICES.md dist/
	mkdir -p dist/docs
	cp docs/research.md dist/docs/
	cp docs/capture-analysis.md dist/docs/
	cp docs/capture-v02-analysis.md dist/docs/
	cp docs/capture-v03-analysis.md dist/docs/
	cp docs/capture-v04-analysis.md dist/docs/
	cp docs/capture-v05-analysis.md dist/docs/
	cp docs/capture-v08-analysis.md dist/docs/
	cp docs/capture-v09-analysis.md dist/docs/
	cp docs/capture-v10-analysis.md dist/docs/
	cp docs/capture-v11-analysis.md dist/docs/
	cp docs/capture-v12-analysis.md dist/docs/
	cp docs/reconnect-v015.md dist/docs/
	cp docs/reconnect-debug-v016.md dist/docs/
	cp docs/reconnect-v017.md dist/docs/
	cp docs/reconnect-v018.md dist/docs/
	cp docs/reconnect-v019.md dist/docs/
	cp docs/handoff-v020.md dist/docs/
	cp docs/session-v021.md dist/docs/
	cp docs/manager-session-v022.md dist/docs/
	cp docs/session-v023.md dist/docs/
	cp docs/generic-hid-v024.md dist/docs/
	cp docs/hid-mode-v025.md dist/docs/
	cp docs/transport-v026.md dist/docs/
	cp docs/connect-v027.md dist/docs/
	cp docs/performance-v028.md dist/docs/
	cp docs/input-latency.md dist/docs/
	cp docs/reconnect-input.md dist/docs/
	python3 tools/package.py --debug $(PSPGOBT_DEBUG) --session-trace $(PSPGOBT_SESSION_TRACE)
endif
