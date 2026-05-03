CC      = gcc
CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_DEFAULT_SOURCE \
          -I. -Iconfig -Icore -Itransport -Istate
LDFLAGS = -lpthread

# ===== Library Versioning =====
VERSION     = 1.0.0
SONAME      = libdoip.so.1
PREFIX     ?= /usr/local
DESTDIR    ?=

# ===== Server (ECU Simulator) =====
SRC_CORE = core/doip_frame.c core/doip_log.c core/doip_det.c
SRC_API  = doip_api.c
SRC_UDP  = transport/doip_udp.c
SRC_TCP  = transport/doip_tcp.c
SRC_UDS  = transport/doip_uds.c
SRC_FSM  = state/doip_fsm.c
SRC_MAIN = main.c

OBJS   = $(SRC_CORE:.c=.o) $(SRC_API:.c=.o) \
         $(SRC_UDP:.c=.o) $(SRC_TCP:.c=.o) $(SRC_UDS:.c=.o) \
         $(SRC_FSM:.c=.o) $(SRC_MAIN:.c=.o)
TARGET = doip_ecu_sim

# ===== Client (DoIP Tester) =====
CLIENT_OBJS   = core/doip_frame.o core/doip_log.o \
                client/doip_client.o client/main_client.o
CLIENT_TARGET = doip_client_demo

# ===== Shared / Static Library =====
LIB_SRC = $(SRC_CORE) $(SRC_API) $(SRC_UDP) $(SRC_TCP) $(SRC_UDS) $(SRC_FSM) \
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
	@for h in $(PUBLIC_HDRS); do \
	    sed 's|#include "include/|#include "|g; \
	         s|#include "config/|#include "|g; \
	         s|#include "core/|#include "|g; \
	         s|#include "client/|#include "|g; \
	         s|#include "\.\./config/|#include "|g; \
	         s|#include "\.\./core/|#include "|g; \
	         s|#include "\.\./client/|#include "|g; \
	         s|#include "\.\./|#include "|g' \
	         $$h > $(DESTDIR)$(PREFIX)/include/doip/$$(basename $$h); \
	    chmod 644 $(DESTDIR)$(PREFIX)/include/doip/$$(basename $$h); \
	done
	@# --- pkg-config ---
	sed 's|@PREFIX@|$(PREFIX)|g; s|@VERSION@|$(VERSION)|g' doip.pc.in \
	    > $(DESTDIR)$(PREFIX)/lib/pkgconfig/doip.pc

uninstall:
	rm -rf $(DESTDIR)$(PREFIX)/include/doip
	rm -f  $(DESTDIR)$(PREFIX)/lib/libdoip.so*
	rm -f  $(DESTDIR)$(PREFIX)/lib/$(STATIC_LIB)
	rm -f  $(DESTDIR)$(PREFIX)/lib/pkgconfig/doip.pc

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

.PHONY: all lib install uninstall docs clean
