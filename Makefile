CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE \
          -I. -Iconfig -Icore -Itransport -Istate
LDFLAGS = -lpthread

# ===== Optional Feature Flags =====
# Enable by passing on the command line:
#   make DOIP_IPV6=1                            — IPv6 dual-stack
#   make DOIP_TLS=1                             — TLS on TCP / DTLS on UDP
#   make DOIP_ISOTP=1                           — ISO-TP segmentation
#   make DOIP_TLS=1 DOIP_IPV6=1 DOIP_ISOTP=1  — all features

#   make DOIP_2019=1                            — ISO 13400-2:2019 header version (0x03)
ifeq ($(DOIP_2019),1)
  VERSION_CFLAGS = -DDOIP_PROTOCOL_VERSION_SELECTED=1
endif
CFLAGS += $(VERSION_CFLAGS)

ifeq ($(DOIP_IPV6),1)
  CFLAGS  += -DDOIP_ENABLE_IPV6=true
endif

ifeq ($(DOIP_TLS),1)
  CFLAGS  += -DDOIP_ENABLE_TLS=true
  LDFLAGS += -lssl -lcrypto
  FEAT_TLS_SRCS = transport/doip_tls.c transport/doip_io.c
else
  FEAT_TLS_SRCS = transport/doip_io.c
endif

ifeq ($(DOIP_ISOTP),1)
  CFLAGS       += -DDOIP_ENABLE_ISO_TP=true
  FEAT_ISOTP_SRCS = transport/doip_isotp.c
else
  FEAT_ISOTP_SRCS =
endif

# ===== Library Versioning =====
VERSION     = 1.0.0
SONAME      = libdoip.so.1
PREFIX     ?= /usr/local
DESTDIR    ?=

# ===== Server (ECU Simulator) =====
SRC_CORE = core/doip_frame.c core/doip_log.c
SRC_API  = doip_api.c
SRC_UDP  = transport/doip_udp.c
SRC_TCP  = transport/doip_tcp.c
SRC_UDS  = transport/doip_uds.c
SRC_FSM  = state/doip_fsm.c
SRC_MAIN = main.c

OBJS   = $(SRC_CORE:.c=.o) $(SRC_API:.c=.o) \
         $(SRC_UDP:.c=.o) $(SRC_TCP:.c=.o) $(SRC_UDS:.c=.o) \
         $(SRC_FSM:.c=.o) \
         $(FEAT_TLS_SRCS:.c=.o) $(FEAT_ISOTP_SRCS:.c=.o) \
         $(SRC_MAIN:.c=.o)
TARGET = doip_ecu_sim

# ===== Client (DoIP Tester) =====
CLIENT_OBJS   = core/doip_frame.o core/doip_log.o \
                $(FEAT_ISOTP_SRCS:.c=.o) \
                client/doip_client.o client/main_client.o
CLIENT_TARGET = doip_client_demo

# ===== Shared / Static Library =====
LIB_SRC = $(SRC_CORE) $(SRC_API) $(SRC_UDP) $(SRC_TCP) $(SRC_UDS) $(SRC_FSM) \
          $(FEAT_TLS_SRCS) $(FEAT_ISOTP_SRCS) \
          client/doip_client.c
LIB_OBJS      = $(LIB_SRC:.c=.lo)
LIB_CFLAGS    = $(CFLAGS) -fPIC -fvisibility=hidden -DDOIP_BUILDING_LIB
SHARED_LIB    = libdoip.so.$(VERSION)
STATIC_LIB    = libdoip.a

# ===== Public Headers (installed to $(PREFIX)/include/doip/) =====
PUBLIC_HDRS = include/doip.h include/doip_version.h \
              doip_api.h client/doip_client.h \
              core/doip_types.h core/doip_log.h \
              config/doip_config.h config/doip_log_config.h config/doip_uds_config.h

# =====================================================================
# Top-level Targets
# =====================================================================
all: $(TARGET) $(CLIENT_TARGET)

# ----- Server link -----
$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ----- Client link -----
$(CLIENT_TARGET): $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# ----- Compile rules (non-PIC, for executables) -----
client/%.o: client/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# =====================================================================
# Library Build
# =====================================================================
lib: $(SHARED_LIB) $(STATIC_LIB)

# PIC compile rules (.lo = position-independent object)
%.lo: %.c
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

client/%.lo: client/%.c
	$(CC) $(LIB_CFLAGS) -c -o $@ $<

# Shared library
$(SHARED_LIB): $(LIB_OBJS)
	$(CC) -shared -Wl,-soname,$(SONAME) -o $@ $^ $(LDFLAGS)
	ln -sf $(SHARED_LIB) $(SONAME)
	ln -sf $(SONAME) libdoip.so

# Static library
$(STATIC_LIB): $(LIB_OBJS)
	$(AR) rcs $@ $^

# =====================================================================
# Install / Uninstall
# =====================================================================
install: lib
	install -d $(DESTDIR)$(PREFIX)/lib
	install -d $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -d $(DESTDIR)$(PREFIX)/include/doip
	@# --- Libraries ---
	install -m 755 $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/
	cp -P $(SONAME) libdoip.so $(DESTDIR)$(PREFIX)/lib/
	install -m 644 $(STATIC_LIB)  $(DESTDIR)$(PREFIX)/lib/
	-ldconfig -n $(DESTDIR)$(PREFIX)/lib 2>/dev/null || true
	@# --- Headers (flatten into include/doip/, rewrite include paths) ---
	@# One -e per rule: a backslash-newline inside a quoted sed script is not a
	@# line continuation, and sed then rejects the whole script.
	@for h in $(PUBLIC_HDRS); do \
	    sed -e 's|#include "include/|#include "|g' \
	        -e 's|#include "config/|#include "|g' \
	        -e 's|#include "core/|#include "|g' \
	        -e 's|#include "client/|#include "|g' \
	        -e 's|#include "\.\./config/|#include "|g' \
	        -e 's|#include "\.\./core/|#include "|g' \
	        -e 's|#include "\.\./client/|#include "|g' \
	        -e 's|#include "\.\./|#include "|g' \
	        $$h > $(DESTDIR)$(PREFIX)/include/doip/$$(basename $$h) || exit 1; \
	    chmod 644 $(DESTDIR)$(PREFIX)/include/doip/$$(basename $$h); \
	done
	@# --- pkg-config ---
	sed 's|@PREFIX@|$(PREFIX)|g; s|@VERSION@|$(VERSION)|g' doip.pc.in \
	    > $(DESTDIR)$(PREFIX)/lib/pkgconfig/doip.pc

# Install into a scratch prefix (build/install-check), then build and run a
# program that uses only <doip/doip.h> and libdoip from there.
test-install:
	rm -rf build/install-check
	$(MAKE) install PREFIX=/usr DESTDIR=$(CURDIR)/build/install-check
	$(CC) -Wall -Wextra -Werror -std=c11 -D_DEFAULT_SOURCE -Ibuild/install-check/usr/include \
	    tests/install/consumer.c -Lbuild/install-check/usr/lib -ldoip $(LDFLAGS) \
	    -o build/install-check/consumer
	LD_LIBRARY_PATH=build/install-check/usr/lib build/install-check/consumer

uninstall:
	rm -rf $(DESTDIR)$(PREFIX)/include/doip
	rm -f  $(DESTDIR)$(PREFIX)/lib/libdoip.so*
	rm -f  $(DESTDIR)$(PREFIX)/lib/$(STATIC_LIB)
	rm -f  $(DESTDIR)$(PREFIX)/lib/pkgconfig/doip.pc

# =====================================================================
# Library Integration Test
# Build server + client main files linked against libdoip.so (not .o files)
# =====================================================================
LIB_TEST_SERVER = test_lib_server
LIB_TEST_CLIENT = test_lib_client
LIB_TEST_EXAMPLE = test_lib_example

test-lib: lib $(LIB_TEST_SERVER) $(LIB_TEST_CLIENT) $(LIB_TEST_EXAMPLE)
	@echo ""
	@echo "=== Library test binaries built ==="
	@echo "Run in two terminals:"
	@echo "  Terminal 1 (server): LD_LIBRARY_PATH=. ./$(LIB_TEST_SERVER)"
	@echo "  Terminal 2 (client): LD_LIBRARY_PATH=. ./$(LIB_TEST_CLIENT) 127.0.0.1"
	@echo ""
	@echo "Minimal API example:"
	@echo "  LD_LIBRARY_PATH=. ./$(LIB_TEST_EXAMPLE) 127.0.0.1"

$(LIB_TEST_SERVER): main.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $< -L. -ldoip $(LDFLAGS) -o $@

$(LIB_TEST_CLIENT): client/main_client.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $< -L. -ldoip $(LDFLAGS) -o $@

$(LIB_TEST_EXAMPLE): example/consumer.c $(SHARED_LIB)
	$(CC) $(CFLAGS) $< -L. -ldoip $(LDFLAGS) -o $@

# =====================================================================
# Unit + Integration Tests  (Unity framework — vendored in tests/vendor)
# =====================================================================

# Flags for test binaries: debug symbols, no optimisation, all include paths
TEST_CFLAGS  = -Wall -Wextra -O0 -g -std=c11 -D_DEFAULT_SOURCE \
               -I. -Iconfig -Icore -Itransport -Istate \
               -Itests/vendor/unity -Itests/mocks $(VERSION_CFLAGS) $(SANITIZE)

# Sanitizers for test binaries, e.g.: make test SANITIZE="-fsanitize=address,undefined"
SANITIZE    ?=

TEST_LDFLAGS = -lpthread

# Integration test flag: inject port-override header before doip_config.h
INT_CFLAGS   = $(TEST_CFLAGS) -include tests/integration/doip_config_test.h

# Production source files compiled into integration test binaries. Always the
# full set: doip_tls.c / doip_isotp.c compile to nothing unless their define is
# on, so each test binary picks its features with -D alone (see test-features).
PROD_SRCS    = core/doip_frame.c core/doip_log.c \
               doip_api.c \
               transport/doip_tcp.c transport/doip_udp.c transport/doip_uds.c \
               transport/doip_io.c transport/doip_tls.c transport/doip_isotp.c \
               state/doip_fsm.c \
               client/doip_client.c

UNITY_SRC    = tests/vendor/unity/unity.c
MOCK_FSM_SRC = tests/mocks/mock_fsm.c

# ----- Unit: frame serialisation / deserialisation (no sockets) -----
test-frame: tests/vendor/unity/unity.c tests/unit/test_frame.c \
            core/doip_frame.c core/doip_log.c
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ----- Unit: UDS request processing + identity + hook (no sockets) --
test-uds: $(UNITY_SRC) tests/unit/test_uds.c \
          transport/doip_uds.c $(MOCK_FSM_SRC) \
          core/doip_log.c
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ----- Unit: ISO-TP segmentation / reassembly (no sockets) ----------
test-isotp: $(UNITY_SRC) tests/unit/test_isotp.c \
            transport/doip_isotp.c core/doip_log.c
	$(CC) $(TEST_CFLAGS) -DDOIP_ENABLE_ISO_TP=true -o $@ $^ $(TEST_LDFLAGS)

# ----- Integration: full server lifecycle + TCP/UDP (port 23400) -----
test-server: $(UNITY_SRC) tests/integration/test_server.c $(PROD_SRCS)
	$(CC) $(INT_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ----- Integration: two instances in one process (ports 23410 / 23411) ----
test-multi: $(UNITY_SRC) tests/integration/test_multi_instance.c $(PROD_SRCS)
	$(CC) $(INT_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# The same under ThreadSanitizer: each instance is ticked by its own thread
test-multi-tsan: $(UNITY_SRC) tests/integration/test_multi_instance.c $(PROD_SRCS)
	$(CC) $(filter-out $(SANITIZE),$(INT_CFLAGS)) -fsanitize=thread -o $@ $^ $(TEST_LDFLAGS)

# ----- Integration: server-initiated Alive Check timing (~22 s) ------
test-alive-bin: $(UNITY_SRC) tests/integration/test_alive_check.c $(PROD_SRCS)
	$(CC) $(INT_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ----- Feature paths: each binary is built with its own -D, whatever the
#       DOIP_* make flags are (the tests above always use the default config) --
test-isotp-server: $(UNITY_SRC) tests/integration/test_isotp_server.c $(PROD_SRCS)
	$(CC) $(INT_CFLAGS) -DDOIP_ENABLE_ISO_TP=true -o $@ $^ $(TEST_LDFLAGS)

# The whole integration suite against the dual-stack (AF_INET6) server, plus a
# native ::1 test. Skips itself with a message on hosts without IPv6.
test-server-ipv6: $(UNITY_SRC) tests/integration/test_server.c $(PROD_SRCS)
	$(CC) $(INT_CFLAGS) -DDOIP_ENABLE_IPV6=true -o $@ $^ $(TEST_LDFLAGS)

# Throwaway self-signed certificates for the TLS tests (needs the openssl tool)
CERT_DIR = tests/certs
$(CERT_DIR)/server.crt:
	mkdir -p $(CERT_DIR)
	openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=doip-test-server" \
	    -keyout $(CERT_DIR)/server.key -out $@ 2>/dev/null
	openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj "/CN=doip-test-client" \
	    -keyout $(CERT_DIR)/client.key -out $(CERT_DIR)/client.crt 2>/dev/null

test-tls: $(UNITY_SRC) tests/integration/test_tls.c $(PROD_SRCS) | $(CERT_DIR)/server.crt
	$(CC) $(INT_CFLAGS) -DDOIP_ENABLE_TLS=true -o $@ $^ $(TEST_LDFLAGS) -lssl -lcrypto

# ----- End-to-end: the real simulator (main.c) driven over sockets ----
# Built straight from sources with the test flags, so sanitizers apply.
test-sim: main.c $(PROD_SRCS)
	$(CC) $(TEST_CFLAGS) -o $@ $^ $(TEST_LDFLAGS)

# ----- Runners -------------------------------------------------------
test-unit: test-frame test-uds test-isotp
	@echo ""
	@echo "=== Unit Tests ==="
	./test-frame
	./test-uds
	./test-isotp
	@echo "=== Unit Tests Complete ==="

test-integration: test-server test-multi
	@echo ""
	@echo "=== Integration Tests (port 23400) ==="
	./test-server
	./test-multi
	@echo "=== Integration Tests Complete ==="

test-tsan: test-multi-tsan
	@echo ""
	@echo "=== ThreadSanitizer: two instances on two threads ==="
	./test-multi-tsan
	@echo "=== ThreadSanitizer Tests Complete ==="

test-alive: test-alive-bin
	@echo ""
	@echo "=== Alive Check Tests (~22 s) ==="
	./test-alive-bin
	@echo "=== Alive Check Tests Complete ==="

# Speaks the plain default wire format, so it is skipped for TLS / ISO-TP builds.
test-e2e: test-sim
	@echo ""
	@echo "=== End-to-End Tests (port 24400) ==="
ifneq ($(filter 1,$(DOIP_TLS) $(DOIP_ISOTP)),)
	@echo "skipped: TLS / ISO-TP build"
else
	@command -v python3 >/dev/null 2>&1 || { echo "skipped: python3 not found"; exit 0; }; \
	 DOIP_SIM=./test-sim python3 tests/e2e/test_sim.py
endif
	@echo "=== End-to-End Tests Complete ==="

test-features: test-isotp-server test-tls test-server-ipv6
	@echo ""
	@echo "=== Feature Tests (port 23400) ==="
	./test-isotp-server
	./test-tls
	./test-server-ipv6
	@echo "=== Feature Tests Complete ==="

# Fast CI target: unit + integration + end-to-end (< 30 s)
test: test-unit test-integration test-e2e

.PHONY: test-unit test-integration test-e2e test-features test-tsan test-alive test-isotp test

# =====================================================================
# Fuzzing  (tests/fuzz)
#   make fuzz                         any compiler: ASan/UBSan + built-in mutator,
#                                     FUZZ_RUNS inputs per target, deterministic
#   make fuzz FUZZ_ENGINE=libfuzzer   clang: coverage-guided libFuzzer binaries,
#                                     built only - run them as long as you like
# =====================================================================
FUZZ_RUNS  ?= 200000
FUZZ_CFLAGS = -Wall -Wextra -g -O1 -std=c11 -D_DEFAULT_SOURCE \
              -I. -Iconfig -Icore -Itransport -Istate -Itests/mocks -Itests/fuzz
ifeq ($(FUZZ_ENGINE),libfuzzer)
  FUZZ_CC     = clang
  FUZZ_SAN    = -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined
  FUZZ_DRIVER =
else
  FUZZ_CC     = $(CC)
  FUZZ_SAN    = -fsanitize=address,undefined -fno-sanitize-recover=undefined
  FUZZ_DRIVER = tests/fuzz/standalone.c
endif
FUZZ_BUILD  = $(FUZZ_CC) $(FUZZ_CFLAGS) $(FUZZ_SAN)
FUZZ_UDS    = transport/doip_uds.c $(MOCK_FSM_SRC) core/doip_log.c
FUZZ_TCP    = $(FUZZ_UDS) transport/doip_io.c core/doip_frame.c
FUZZ_BINS   = fuzz-uds fuzz-isotp fuzz-tcp fuzz-tcp-isotp

fuzz-uds: tests/fuzz/fuzz_uds.c $(FUZZ_UDS) $(FUZZ_DRIVER)
	$(FUZZ_BUILD) -o $@ $^
fuzz-isotp: tests/fuzz/fuzz_isotp.c transport/doip_isotp.c core/doip_log.c $(FUZZ_DRIVER)
	$(FUZZ_BUILD) -DDOIP_ENABLE_ISO_TP=true -o $@ $^
# doip_tcp.c is #included by the harness (to reach its statics), so it is a
# dependency but not a separate compilation unit
fuzz-tcp: tests/fuzz/fuzz_tcp_stream.c $(FUZZ_TCP) $(FUZZ_DRIVER) | transport/doip_tcp.c
	$(FUZZ_BUILD) -o $@ $^
fuzz-tcp-isotp: tests/fuzz/fuzz_tcp_stream.c $(FUZZ_TCP) transport/doip_isotp.c $(FUZZ_DRIVER) | transport/doip_tcp.c
	$(FUZZ_BUILD) -DDOIP_ENABLE_ISO_TP=true -o $@ $^

fuzz: $(FUZZ_BINS)
ifneq ($(FUZZ_ENGINE),libfuzzer)
	@for f in $(FUZZ_BINS); do FUZZ_RUNS=$(FUZZ_RUNS) ./$$f || exit 1; done
endif

.PHONY: fuzz

# =====================================================================
# Documentation
# =====================================================================
docs:
	doxygen Doxyfile

# =====================================================================
# Clean
# =====================================================================
clean:
	rm -f $(OBJS) $(TARGET)
	rm -f $(CLIENT_OBJS) $(CLIENT_TARGET)
	rm -f $(LIB_OBJS) $(SHARED_LIB) $(STATIC_LIB) $(SONAME) libdoip.so
	rm -f $(LIB_TEST_SERVER) $(LIB_TEST_CLIENT) $(LIB_TEST_EXAMPLE)
	rm -f test-frame test-uds test-isotp test-server test-alive-bin test-sim test-multi test-multi-tsan
	rm -f test-isotp-server test-tls test-server-ipv6
	rm -f $(FUZZ_BINS) crash-fuzz-*.bin
	rm -rf build/install-check
	rm -rf tests/certs
	rm -f transport/doip_io.o transport/doip_tls.o transport/doip_isotp.o
	rm -f transport/doip_io.lo transport/doip_tls.lo transport/doip_isotp.lo

.PHONY: all lib test-lib install test-install uninstall docs clean
