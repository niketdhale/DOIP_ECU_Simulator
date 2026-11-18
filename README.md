# DoIP ECU Simulator

A C implementation of the **Diagnostic over Internet Protocol (DoIP)** stack for automotive ECU simulation, following **ISO 13400-2**. Provides both a full-featured ECU simulator (server) and a reusable DoIP client library, exportable as `libdoip.so` / `libdoip.a`.

---

## Features

### Core Protocol (always enabled)

- **DoIP Server (ECU Simulator)**
  - UDP Vehicle Announcement broadcasts (periodic, configurable count)
  - UDP VIN/EID request handling
  - UDP Entity Status (PT 0x4001) and Diagnostic Power Mode (PT 0x4003) responses — unicast to requester per ISO 13400-2 §7.5/7.6
  - TCP routing activation with multi-client support (up to 5 simultaneous testers)
  - Generic Header NACK (PT 0x0000) on malformed or unknown frames — ISO 13400-2 §7.2
  - Server-initiated Alive Check (PT 0x0007) with configurable interval and disconnect-on-timeout
  - UDS diagnostic message forwarding (ISO 14229) with per-client session state
  - S3 server session timeout enforcement
  - AUTOSAR-aligned finite state machine (FSM)
  - **Runtime-configurable ECU identity** — VIN, SW version, serial number, EID, GID via `DoIP_EcuIdentity_t` at init time (no recompile needed)
  - **Runtime UDS callback hook** (`on_uds_request`) — intercept any UDS request before the built-in service table

- **DoIP Client Library**
  - UDP vehicle discovery (broadcast or unicast)
  - TCP connect + routing activation
  - UDS request/response transactions (`DoIP_Client_Transact`)
  - Transparent handling of DiagACK (0x8002) and Alive Check (0x0007)
  - **Dual-stack connect** — `getaddrinfo()` resolves both IPv4 and IPv6 addresses

- **Library Export**
  - Shared library (`libdoip.so`) with SONAME versioning
  - Static library (`libdoip.a`)
  - Symbol visibility control (`-fvisibility=hidden` + `DOIP_API`)
  - pkg-config integration (`doip.pc`)
  - Doxygen API documentation

### Optional Transport Extensions (compile-time opt-in)

All four transport extensions are **disabled by default** and have zero impact on the baseline binary. Each is activated by a single Makefile variable.

| Feature | Flag | Description |
|---------|------|-------------|
| **TLS** | `DOIP_TLS=1` | OpenSSL TLS 1.2+ on TCP (UDP discovery stays unencrypted) |
| **IPv6** | `DOIP_IPV6=1` | Dual-stack `AF_INET6` sockets, `ff02::1` multicast |
| **ISO-TP** | `DOIP_ISOTP=1` | ISO 15765-2 segmentation/reassembly of UDS payloads |
| **Async API** | (always on) | `on_client_connect/disconnect/frame_received` callbacks in `DoIP_Config_t` |

---

### Supported UDS Services (ISO 14229)

- `0x10` Session Control
- `0x11` ECU Reset — reset types `0x01`–`0x03`; returns the client to the default session
- `0x14` Clear Diagnostic Information — `FFFFFF` clears all DTCs, or one specific DTC
- `0x19` Read DTC Information — `01` count by status mask, `02` list by status mask, `0A` all supported; DTC table in `UDS_DTC_TABLE`
- `0x22` Read Data By Identifier (VIN, SW Version, System Name, Serial Number, plus any DIDs registered with `DoIP_SetDid()` / `--did`) — several DIDs per request
- `0x27` Security Access level 1 — `27 01` returns a 4-byte seed, `27 02` expects `seed XOR UDS_SECURITY_KEY_MASK`; three wrong keys lock the client out for `UDS_SECURITY_LOCKOUT_MS`
- `0x2E` Write Data By Identifier — VIN (`0xF190`, exactly 17 bytes), system name (`0xF186`), serial number (`0xF18C`). A written VIN is also used in UDP vehicle announcements and ID responses. Registered DIDs marked writable (`--did-rw`) are accepted too
- `0x31` Routine Control
- `0x3E` Tester Present
- Suppress-positive-response bit (`0x80` in the sub-function byte) honoured for sub-function services: the service runs, the DoIP ACK is sent, and no positive UDS response follows (e.g. `3E 80`); negative responses are still sent
- Session gating: `0x27`, `0x28`, `0x2E`, `0x31`, `0x34`, `0x36`, `0x37`, `0x85` answer NRC `0x7F` in the default session (`UDS_NON_DEFAULT_SESSION_SIDS`)
- Security gating: `0x2E` answers NRC `0x33` until Security Access is unlocked (`UDS_SECURED_SIDS`)

Typical write sequence: `10 03` → `27 01` → `27 02 <key>` → `2E F1 90 <17-byte VIN>`.

- Auto-response mode for unsupported SIDs

---

## Architecture

### Baseline Layer Stack

```
DoIP_Tick() [doip_api.c]
  └─ DoIP_Fsm_MainFunction() [state/doip_fsm.c]
       ├─ doip_tcp_poll() / doip_tcp_tick()   [transport/doip_tcp.c]
       │    └─ doip_uds_process_request()     [transport/doip_uds.c]
       └─ doip_udp_poll() / doip_udp_tick()   [transport/doip_udp.c]
```

### Extended Layer Stack (all features enabled)

```
DoIP_Tick() [doip_api.c]
  └─ DoIP_Fsm_MainFunction()
       ├─ doip_tcp_poll() / doip_tcp_tick()
       │    ├─ doip_io_read/write()            ← I/O abstraction vtable
       │    │    └─ plain fd  OR  SSL_read/SSL_write  (via doip_tls.c)
       │    └─ doip_isotp_rx/tx()              ← ISO-TP reassembly (if enabled)
       │         └─ doip_uds_process_request()
       └─ doip_udp_poll() / doip_udp_tick()
            └─ [IPv4 or IPv6 dual-stack socket]
```

### Component Diagram

```
┌─────────────────────────────────────────────────────────────┐
│                    Application / Tests                       │
│              main.c          client/main_client.c            │
└────────────────┬────────────────────┬───────────────────────┘
                 │   Public API        │   Public API
        ┌────────▼────────┐   ┌───────▼──────────┐
        │   doip_api.h    │   │ client/doip_      │
        │   DoIP_Create   │   │ client.h          │
        │   DoIP_Init     │   │ DoIP_Client_Create│
        │   DoIP_Tick     │   │ DoIP_Client_Connect│
        │   DoIP_Destroy  │   │ DoIP_Client_Transact│
        └────────┬────────┘   └───────────────────┘
                 │
     ┌───────────┼────────────────────┐
     │           │                    │
┌────▼────┐ ┌───▼────┐  ┌────────────▼──────────┐
│transport│ │transport│  │     state/doip_fsm     │
│/doip_   │ │/doip_   │  │  UNINIT → IDLE →       │
│ udp.c   │ │ tcp.c   │  │  ROUTING_ACTIVE        │
│(UDP/    │ │(TCP +   │  └───────────────────────┘
│ mcast)  │ │ doip_io │
│         │ │ vtable) │
└────┬────┘ └───┬─────┘
     │          │
     └────┬─────┘
          │
┌─────────▼────────────────────────────┐
│              core/                    │
│  doip_frame.c   doip_log.c           │
└──────────────────────────────────────┘
```

---

## Directory Structure

```
.
├── main.c                       # ECU simulator entry point
├── doip_api.c/.h                # Public server API (DoIP_Create, DoIP_Init, DoIP_Tick…)
│
├── core/
│   ├── doip_frame.c/.h          # DoIP frame serialization / deserialization
│   ├── doip_log.c/.h            # Thread-safe logging (colors, timestamps, modules)
│   ├── doip_types.h             # Protocol structs & payload type constants
│   └── doip_context.h           # Internal: the per-instance state behind DoIP_Handle_t
│
├── transport/
│   ├── doip_udp.c/.h            # UDP: vehicle announcements, VIN/EID, IPv6 dual-stack
│   ├── doip_tcp.c/.h            # TCP: multi-client server, routing activation, I/O vtable
│   ├── doip_uds.c/.h            # UDS: diagnostic service handlers
│   ├── doip_io.h/.c             # I/O vtable abstraction (plain fd; TLS-transparent)
│   ├── doip_tls.h/.c            # TLS/DTLS via OpenSSL (compiled when DOIP_TLS=1)
│   └── doip_isotp.h/.c          # ISO-TP segmentation/reassembly (compiled when DOIP_ISOTP=1)
│
├── state/
│   └── doip_fsm.c/.h            # Finite State Machine + S3 session timeout
│
├── client/
│   ├── doip_client.c/.h         # DoIP Client library (getaddrinfo, dual-stack)
│   └── main_client.c            # Client demo (discovery → connect → UDS transactions)
│
├── include/
│   ├── doip.h                   # Umbrella header
│   └── doip_version.h           # Version constants + DOIP_API visibility macro
│
├── config/
│   ├── doip_config.h            # Protocol version, ports, addresses, timeouts, feature toggles
│   ├── doip_log_config.h        # Log levels, module filter
│   └── doip_uds_config.h        # UDS SIDs, NRCs, DIDs, timing parameters
│
├── example/
│   └── consumer.c               # Minimal example linking against libdoip.so
│
├── tests/
│   ├── unit/
│   │   ├── test_frame.c         # Unit: DoIP frame serialize/deserialize (10 tests)
│   │   ├── test_uds.c           # Unit: UDS service handlers + identity + hook (18 tests)
│   │   └── test_isotp.c         # Unit: ISO-TP SF/FF/CF/FC state machine (14 tests)
│   ├── integration/
│   │   ├── test_server.c        # Integration: full server lifecycle TCP/UDP (11 tests)
│   │   └── test_alive_check.c   # Integration: server-initiated Alive Check timing (3 tests)
│   ├── mocks/
│   │   └── mock_fsm.c           # FSM stub for unit tests that don't need sockets
│   └── vendor/unity/            # Unity test framework (vendored)
│
├── Makefile                     # Build system with optional feature flags
├── Doxyfile                     # Doxygen configuration
└── doip.pc.in                   # pkg-config template
```

---

## Requirements

| Dependency | Required | Notes |
|-----------|----------|-------|
| **GCC** ≥ 4.8 | Yes | C11 mode (`-std=c11`) |
| **POSIX sockets** | Yes | `-D_DEFAULT_SOURCE` |
| **pthreads** | Yes | `-lpthread` |
| **Linux** | Yes | Tested on Arch Linux / Ubuntu 24.04 |
| **OpenSSL** ≥ 1.1.1 | TLS only | `sudo apt install libssl-dev` |
| **doxygen** | Optional | API HTML docs |

---

## Build

### Standard Build (all features disabled)

```bash
# Build both executables (ECU simulator + client demo)
make

# Build shared + static library
make lib

# Build library and link test binaries against it
make test-lib

# Generate Doxygen HTML → docs/html/index.html
make docs

# Clean all build artifacts
make clean
```

### Optional Feature Flags

Each feature is activated by a Makefile variable. They can be combined freely.

```bash
# IPv6 dual-stack sockets (TCP + UDP)
make DOIP_IPV6=1

# TLS 1.2+ on TCP (requires OpenSSL)
make DOIP_TLS=1

# ISO-TP segmentation / reassembly (ISO 15765-2)
make DOIP_ISOTP=1

# All features combined
make DOIP_TLS=1 DOIP_IPV6=1 DOIP_ISOTP=1

# Zero-regression baseline (no features)
make clean && make
```

These flags inject the matching `-D` defines and automatically add the required source files and linker flags (`-lssl -lcrypto`).

---

## Running Tests

```bash
# Unit (frame + UDS + ISO-TP) + integration + end-to-end tests — fast, < 30 s
make test

# The same, with sanitizers
make test SANITIZE="-fsanitize=address,undefined"

# End-to-end: launches the real simulator binary and talks DoIP to it (needs python3)
make test-e2e

# Feature paths: ISO-TP, TLS and IPv6 dual-stack servers, each built with its
# own define (needs OpenSSL headers and the openssl tool; generates tests/certs/;
# the IPv6 part skips itself on hosts without ::1)
make test-features

# ISO-TP unit tests only (no sockets, instant)
make test-isotp

# Full integration: server lifecycle + TCP/UDP (port 23400)
make test-server && ./test-server

# Alive Check timing tests (~22 s — exercises idle timeout + disconnect)
make test-alive && ./test-alive-bin

# Two instances on two threads under ThreadSanitizer
make test-tsan

# Install into build/install-check and build + run a program against that tree only
make test-install
```

**Test suites:**

| Suite | Target | Tests |
|-------|--------|-------|
| Frame unit tests | `make test` | 10 |
| UDS unit tests | `make test` | 39 |
| ISO-TP unit tests | `make test` | 14 |
| Server integration tests (in-process server, port 23400) | `make test` | 27 |
| Two instances in one process (ports 23410 / 23411); also `make test-tsan` | `make test` | 6 |
| End-to-end tests of the binary (port 24400) | `make test` | 12 |
| Alive-check timing tests | `make test-alive` | 3 |
| ISO-TP server integration tests | `make test-features` | 5 |
| TLS server integration tests | `make test-features` | 6 |
| Integration suite against the IPv6 dual-stack server | `make test-features` | 27 |

`make test` always tests the default configuration (plus `DOIP_2019=1` if given); the `DOIP_TLS` / `DOIP_IPV6` / `DOIP_ISOTP` flags only affect the simulator and library build. The feature code paths are covered by `make test-features`, where each test binary is built with its own define.

### Fuzzing

```bash
# Any compiler: ASan/UBSan + a built-in mutator, 200000 inputs per target (a few seconds, deterministic)
make fuzz
make fuzz FUZZ_RUNS=5000000        # longer
FUZZ_SEED=7 ./fuzz-uds             # another deterministic sequence

# clang: coverage-guided libFuzzer binaries - run each as long as you like
make clean && make fuzz FUZZ_ENGINE=libfuzzer
./fuzz-tcp -max_total_time=300
```

| Target | What it feeds |
|--------|---------------|
| `fuzz-uds` | Sequences of UDS requests into the service layer (all handlers, session and security state) |
| `fuzz-isotp` | N-PDUs into ISO-TP reassembly, plus a segment-and-reassemble round trip |
| `fuzz-tcp` | A byte stream, split into arbitrary reads, into the TCP server's frame handling: header validation, reassembly, routing activation, diagnostic dispatch |
| `fuzz-tcp-isotp` | The same with ISO-TP enabled |

When a sanitizer aborts, the built-in mutator saves the input as `crash-<target>.bin`; replay it with `./fuzz-uds crash-fuzz-uds.bin`.

---

## Running the Simulator

### Terminal 1 — ECU Simulator

```bash
# Default build (all features off):
./doip_ecu_sim

# Override identity / addressing without recompiling:
./doip_ecu_sim --vin WAUZZZ8V0JA000001 --address 0x2002 --functional-address 0xE401 \
               --port 14400 --log-level 2
# Extra data identifiers: text, or hex bytes with 0x; --did-rw makes one writable
./doip_ecu_sim --did F187=PN-1234 --did-rw F1A0=0x0102AB
./doip_ecu_sim --help

# With TLS:
make clean && make DOIP_TLS=1
./doip_ecu_sim --tls-cert server.crt --tls-key server.key   # both are required in a TLS build

# With IPv6:
make clean && make DOIP_IPV6=1
./doip_ecu_sim   # now listens on AF_INET6 (accepts IPv4-mapped + native IPv6)
```

### Terminal 2 — C Client Demo

```bash
# Connect by IPv4:
./doip_client_demo 127.0.0.1

# Connect by IPv6 (when server built with DOIP_IPV6=1):
./doip_client_demo ::1
```

### End-to-end tests of the binary

```bash
make test-e2e    # builds ./test-sim, launches it with CLI flags, drives it over sockets (Python 3 stdlib)
```

---

## Public API Reference

### Server Lifecycle (`doip_api.h`)

```c
DoIP_Handle_t* DoIP_Create(void);
int            DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config);
void           DoIP_Tick(DoIP_Handle_t *handle);    // call every ~1 ms
void           DoIP_DeInit(DoIP_Handle_t *handle);
void           DoIP_Destroy(DoIP_Handle_t *handle);
void           DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level);
void           DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable);
int            DoIP_SetFaults(DoIP_Handle_t *handle, const DoIP_FaultConfig_t *faults);  // NULL clears
int            DoIP_SetDid(DoIP_Handle_t *handle, uint16_t did,
                           const void *data, uint16_t len, bool writable);
```

`DoIP_SetDid()` registers an extra data identifier for ReadDataByIdentifier without writing a hook: up to `UDS_DID_TABLE_SIZE` (16) DIDs of up to `UDS_DID_DATA_MAX` (64) bytes per instance, optionally writable with `0x2E`. For anything dynamic (live values, larger data) use the `on_uds_request` hook.

### Several ECUs in one process

A handle is a complete, independent simulator: its own sockets, testers, identity, sessions, security state, DTC memory and fault settings. Nothing is shared except the logger.

```c
DoIP_Handle_t *engine = DoIP_Create(), *gearbox = DoIP_Create();

DoIP_Config_t cfg = {0};
cfg.ecu_identity = &engine_identity;   cfg.logical_address = 0x1001;  cfg.port = 13400;
DoIP_Init(engine, &cfg);
cfg.ecu_identity = &gearbox_identity;  cfg.logical_address = 0x1002;  cfg.port = 13401;
DoIP_Init(gearbox, &cfg);

for (;;) { DoIP_Tick(engine); DoIP_Tick(gearbox); }   // or one thread per instance
```

- Every instance listens on all local addresses, so each needs its own port. A second instance on a port that is taken fails in `DoIP_Init()` and leaves nothing bound.
- Calls on different handles may run on different threads at the same time; calls on one handle are serialised by a per-handle lock. Do not call the API of a handle from inside its own callbacks.
- Logging is process-wide: the first initialised instance sets the level and the log file, later ones share them, and `DoIP_SetLogLevel()` affects all.

### `DoIP_Config_t` — Full Field Reference

```c
typedef struct {
    /* ── Logging ───────────────────────────────────────────────── */
    uint8_t     log_level;           // 0=ERROR … 4=VERBOSE
    const char *log_file_path;       // NULL = console only

    /* ── Timing ────────────────────────────────────────────────── */
    uint32_t    s3_server_timeout_ms; // UDS S3 timeout (default 5000)

    /* ── State / message callbacks ─────────────────────────────── */
    void (*on_state_change)(uint8_t old, uint8_t new, void *ctx);
                          // FSM follows the UDS session: -> DIAG_SESSION on a
                          // non-default session, -> ROUTING_ACTIVE back in the
                          // default session, -> IDLE when S3 expires
    void (*on_rx_message)(uint16_t pt, const uint8_t *data,
                          uint32_t len, void *ctx);
                          // every DoIP frame received over TCP or UDP

    /* ── ECU identity (NULL = compile-time defaults) ────────────── */
    const DoIP_EcuIdentity_t *ecu_identity;

    /* ── UDS request hook ───────────────────────────────────────── */
    int (*on_uds_request)(uint8_t sid,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_size,
                          uint16_t *resp_len_out,
                          void *ctx);
                          // return 0 = handled; -1 = use built-in

    /* ── Async TCP-client event callbacks (all optional / NULL) ─── */
    void (*on_client_connect)   (int client_fd, void *ctx);
    void (*on_client_disconnect)(int client_fd, void *ctx);
    int  (*on_frame_received)   (int client_fd, uint16_t pt,
                                  const uint8_t *payload, uint32_t plen,
                                  void *ctx);
                                  // return 0 = handled; -1 = use built-in dispatch

    void *user_context;           // forwarded to all callbacks above

    /* ── Addressing (0 = compiled-in default) ───────────────────── */
    uint16_t logical_address;     // ECU address            (DOIP_ECU_LOGICAL_ADDRESS, 0x1003)
    uint16_t functional_address;  // functional group       (DOIP_FUNCTIONAL_LOGICAL_ADDRESS, 0xE400)
    uint16_t port;                // UDP + TCP port         (DOIP_TCP_PORT, 13400)

    /* ── Fault injection (all-zero = none) ──────────────────────── */
    DoIP_FaultConfig_t faults;    // see "Fault injection"; change later with DoIP_SetFaults()

#if DOIP_ENABLE_TLS
    /* ── TLS configuration (DOIP_TLS=1 only) ───────────────────── */
    struct {
        const char *cert_file;   // PEM server certificate path
        const char *key_file;    // PEM private key path
        const char *ca_file;     // CA bundle for peer verification (NULL = skip)
        bool        verify_peer; // require client certificate
    } tls;
#endif
} DoIP_Config_t;
```

### Client API (`client/doip_client.h`)

```c
// Lifecycle
DoIP_Client_t*      DoIP_Client_Create(void);
DoIP_ClientStatus_t DoIP_Client_Init(DoIP_Client_t *c, const DoIP_ClientConfig_t *cfg);
void                DoIP_Client_Destroy(DoIP_Client_t *c);

// Discovery (stateless)
DoIP_ClientStatus_t DoIP_Client_Discover(const char *ip, uint16_t port,
                                          uint32_t timeout_ms,
                                          DoIP_DiscoveryResult_t *out);

// Connection — accepts IPv4 address, IPv6 address, or hostname
DoIP_ClientStatus_t DoIP_Client_Connect(DoIP_Client_t *c,
                                         const char *server_ip, uint16_t port);
DoIP_ClientStatus_t DoIP_Client_Disconnect(DoIP_Client_t *c);

// Routing Activation
DoIP_ClientStatus_t DoIP_Client_ActivateRouting(DoIP_Client_t *c,
                                                  uint8_t activation_type,
                                                  uint8_t *out_code);

// UDS Messaging
DoIP_ClientStatus_t DoIP_Client_Transact(DoIP_Client_t *c, uint16_t target_addr,
                                          const uint8_t *req, uint16_t req_len,
                                          uint8_t *resp, uint16_t resp_size,
                                          uint16_t *resp_len_out);

// State queries
bool DoIP_Client_IsConnected(const DoIP_Client_t *c);
bool DoIP_Client_IsRoutingActive(const DoIP_Client_t *c);
```

`DoIP_Client_GetLastNackCode(c)` returns the code behind an `ERR_NACK` (for example `0x03` = unknown target address).

Return codes: `DOIP_CLIENT_OK (0)`, `ERR_PARAM (-1)`, `ERR_SOCKET (-2)`, `ERR_TIMEOUT (-3)`, `ERR_NACK (-4)`, `ERR_REJECTED (-5)`, `ERR_NOT_READY (-6)`, `ERR_IO (-7)`

---

## Configuration

All compile-time configuration lives in the `config/` headers.

| Header | Controls |
|--------|----------|
| `config/doip_config.h` | Protocol version, addresses, ports, timeouts, feature toggles |
| `config/doip_log_config.h` | Log level, ANSI colours, per-module enable bitmask |
| `config/doip_uds_config.h` | UDS SIDs, NRCs, DIDs, timing, session flags |

### Key tunables (`doip_config.h`)

```c
/* Protocol version */
#define DOIP_PROTOCOL_VERSION_SELECTED  DOIP_VERSION_2012   // or DOIP_VERSION_2019 (make DOIP_2019=1)

/* Addressing */
#define DOIP_ECU_LOGICAL_ADDRESS        0x1003U
#define DOIP_TESTER_LOGICAL_ADDRESS     0x0E00U

/* Network */
#define DOIP_UDP_PORT                   13400U
#define DOIP_TCP_PORT                   13400U
#define DOIP_MULTICAST_ADDR             "224.0.0.1"       // IPv4 multicast

/* Timing */
#define DOIP_ANNOUNCE_INTERVAL_MS       2000U
#define DOIP_ANNOUNCE_COUNT_MAX         5
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS  5000U
#define DOIP_ALIVE_CHECK_INTERVAL_MS    5000U   // idle before probe
#define DOIP_ALIVE_CHECK_TIMEOUT_MS     2000U   // disconnect if no reply

/* ── Protocol toggles (each overridable with -D, e.g. CFLAGS+=-DDOIP_SUPPORT_EID_REQUEST=false) ── */
#define DOIP_SUPPORT_VIN_REQUEST        true    // answer Vehicle ID requests (PT 0x0001, 0x0003); false = ignored
#define DOIP_SUPPORT_EID_REQUEST        true    // answer Vehicle ID request with EID (PT 0x0002); false = ignored
#define DOIP_SUPPORT_ALIVE_CHECK        true    // server probes idle testers; false = no probe/timeout
                                                // (a tester's own Alive Check request is always answered)

/* ── Optional feature toggles (also settable via Makefile) ─────── */
/* IPv6 — dual-stack AF_INET6; build with: make DOIP_IPV6=1        */
#define DOIP_ENABLE_IPV6                false
#define DOIP_IPV6_MULTICAST_ADDR        "ff02::1"

/* TLS/DTLS — OpenSSL >= 1.1.1; build with: make DOIP_TLS=1        */
#define DOIP_ENABLE_TLS                 false

/* ISO-TP — ISO 15765-2; build with: make DOIP_ISOTP=1             */
#define DOIP_ENABLE_ISO_TP              false
#define DOIP_ISOTP_SF_MAX               7U       // max SF data bytes
#define DOIP_ISOTP_N_BS_MS              1000U    // wait for FC after FF
#define DOIP_ISOTP_N_CR_MS              1000U    // wait for next CF
#define DOIP_ISOTP_STMIN_MS             0U       // min separation between CFs
```

---

## Transport Features

### I/O Abstraction (`doip_io_t`)

Every accepted TCP client gets a `doip_io_t` vtable populated at accept time. All subsequent read / write / close operations are dispatched through it. This makes TLS completely transparent to the frame-processing layer.

```c
// doip_io.h
typedef struct {
    ssize_t (*read) (void *ctx, void *buf, size_t len);
    ssize_t (*write)(void *ctx, const void *buf, size_t len);
    int     (*getfd)(void *ctx);   // used by select() loop
    void    (*close)(void *ctx);
    void   *ctx;                   // plain: fd cast to void*; TLS: SSL*
} doip_io_t;

doip_io_t doip_io_plain(int fd);  // construct plain-fd implementation
```

The `ctx` field encodes the file descriptor as `(void*)(intptr_t)fd`, making the struct safe to copy by value without heap allocation.

---

### Non-blocking / Async I/O

The TCP layer no longer sets `SO_RCVTIMEO` on accepted sockets. Instead, each client has a per-client reassembly buffer (`rx_buf` + `rx_bytes` inside `doip_client_t`). A single non-blocking `recv()` accumulates bytes; once a complete header + payload is available the frame is dispatched. Partial frames wait for the next `select()` wake-up.

**Async event callbacks** in `DoIP_Config_t` let the application hook into the TCP lifecycle:

```c
// Called when a new TCP client connects
config.on_client_connect    = my_connect_cb;   // (int fd, void *ctx)

// Called when a client disconnects (timeout, error, or peer close)
config.on_client_disconnect = my_disconnect_cb;

// Called when a complete DoIP frame arrives, before built-in dispatch.
// Return 0 = application handled it; -1 = pass to built-in handler.
config.on_frame_received    = my_frame_cb;
```

---

### TLS (`make DOIP_TLS=1`)

Requires OpenSSL ≥ 1.1.1 (`sudo apt install libssl-dev`).

```bash
# Generate a self-signed test certificate
mkdir -p tests/certs
openssl req -x509 -newkey rsa:2048 -nodes \
    -out tests/certs/server.crt -keyout tests/certs/server.key \
    -subj "/CN=doip-test" -days 365

make clean && make DOIP_TLS=1

./doip_ecu_sim --tls-cert tests/certs/server.crt --tls-key tests/certs/server.key
# mutual TLS: also require a client certificate signed by this CA
./doip_ecu_sim --tls-cert server.crt --tls-key server.key --tls-ca ca.crt --tls-verify-peer
```

Through the API, set the same values before calling `DoIP_Init()`:

```c
DoIP_Config_t config = {0};
config.tls.cert_file   = "tests/certs/server.crt";
config.tls.key_file    = "tests/certs/server.key";
config.tls.ca_file     = NULL;          // NULL = skip client cert verification
config.tls.verify_peer = false;
DoIP_Init(handle, &config);
```

Internals: the TCP server reads the certificate paths from the instance's configuration when `DoIP_Init()` opens its sockets. At accept time, `doip_tls_accept()` performs the TLS handshake and returns a `doip_io_t` whose vtable calls `SSL_read`/`SSL_write` — the rest of the stack sees no difference.

Limits to know about:

- In a TLS build every TCP client must use TLS on the same port; cleartext clients are dropped.
- UDP (vehicle announcement / identification) is not encrypted. `doip_dtls_wrap()` exists in `doip_tls.c` but is not wired into the UDP path.
- The handshake is done synchronously at accept time (bounded by `DOIP_TLS_HANDSHAKE_TIMEOUT_S`, 2 s), so a client that connects and stays silent pauses the server for that long.
- The bundled client library and `doip_client_demo` do not speak TLS.
- `doip_tls_init()` sets `SIGPIPE` to ignored for the process, because OpenSSL writes with plain `write()`.

---

### IPv6 Dual-Stack (`make DOIP_IPV6=1`)

```bash
make clean && make DOIP_IPV6=1
./doip_ecu_sim      # listens on AF_INET6 + IPV6_V6ONLY=0 (accepts IPv4-mapped too)

./doip_client_demo ::1          # connect via IPv6 loopback
./doip_client_demo 127.0.0.1   # still works (IPv4-mapped)
```

- TCP and UDP sockets both switch to `AF_INET6`.
- `IPV6_V6ONLY=0` ensures IPv4-mapped addresses (`::ffff:127.0.0.1`) are accepted.
- UDP joins the DoIP IPv6 multicast group `ff02::1` (ISO 13400-2:2019).
- Client uses `getaddrinfo(AF_UNSPEC)` — any IPv4 or IPv6 address or hostname resolves correctly.
- Peer IP logging uses `inet_ntop()` with `INET6_ADDRSTRLEN`.

---

### ISO-TP Framing (`make DOIP_ISOTP=1`)

ISO 15765-2 segmentation/reassembly sits between the DoIP diagnostic payload and the UDS handler. DoIP and TCP framing are unchanged.

```bash
make DOIP_ISOTP=1 test-isotp   # run 14 unit tests, no sockets
./test-isotp
```

**Supported frame types:**

| Type | PCI nibble | Direction | Description |
|------|-----------|-----------|-------------|
| SF (Single Frame) | `0x0` | RX + TX | Payload ≤ 7 bytes, no handshake |
| FF (First Frame) | `0x1` | RX | Start of multi-frame; triggers FC CTS |
| CF (Consecutive Frame) | `0x2` | RX | Continuation; sequence number validated |
| FC (Flow Control) | `0x3` | TX | Emitted by receiver after FF (CTS / overflow) |

**RX reassembly API:**

```c
int doip_isotp_rx(doip_isotp_ctx_t *ctx,
                   const uint8_t *in, uint16_t in_len,
                   uint8_t *out_buf, uint16_t out_size,
                   uint8_t *fc_out, uint8_t *fc_len_out);
// Returns: >0 = complete UDS message length
//           0 = waiting for more consecutive frames
//          -1 = protocol error (close session)
```

**TX segmentation API:**

```c
int doip_isotp_tx(doip_isotp_ctx_t *ctx,
                   const uint8_t *uds_data, uint16_t uds_len,
                   uint8_t out_segs[][8], int max_segs);
// Returns: number of 8-byte N-PDU segments, or -1 on error
```

Each segment is exactly 8 bytes, padded with `0xCC` per ISO 15765-2.

---

## Configuring UDS Request / Response

UDS behaviour is controlled at four levels.

### 1. Runtime ECU identity — `DoIP_EcuIdentity_t`

```c
DoIP_EcuIdentity_t identity = {0};
strncpy(identity.vin,              "MYVIN00000000001", DOIP_VIN_LENGTH);
strncpy(identity.software_version, "V2.5.0",          sizeof(identity.software_version) - 1);
strncpy(identity.system_name,      "My ECU Node",     sizeof(identity.system_name) - 1);
strncpy(identity.serial_number,    "SN-98765",        sizeof(identity.serial_number) - 1);
memset(identity.eid, 0x11, DOIP_EID_LENGTH);
memset(identity.gid, 0x22, DOIP_GID_LENGTH);

DoIP_Config_t config = {0};
config.ecu_identity = &identity;
DoIP_Init(handle, &config);
```

Used in UDP announcements and UDS DIDs: `0xF190` (VIN), `0xF189` (SW version), `0xF197` (system name), `0xF18C` (serial number).

### 2. Runtime UDS hook — `on_uds_request`

```c
static int my_uds_hook(uint8_t sid,
                       const uint8_t *req, uint16_t req_len,
                       uint8_t *resp, uint16_t resp_size, uint16_t *resp_len_out,
                       void *ctx)
{
    if (sid == UDS_SID_READ_DATA_BY_ID && req_len >= 3) {
        uint16_t did = ((uint16_t)req[1] << 8) | req[2];
        if (did == 0xA001) {  // custom: live odometer
            uint32_t odo = nvm_read_odometer();
            resp[0] = UDS_SID_READ_DATA_BY_ID_RES;
            resp[1] = 0xA0; resp[2] = 0x01;
            resp[3] = (odo >> 16) & 0xFF;
            resp[4] = (odo >>  8) & 0xFF;
            resp[5] =  odo        & 0xFF;
            *resp_len_out = 6;
            return 0;   /* handled — skip built-in */
        }
    }
    return -1;  /* not handled — fall through to built-in */
}

config.on_uds_request = my_uds_hook;
config.user_context   = &my_nvm_handle;
```

### 3. Service enable toggles — `config/doip_uds_config.h`

```c
#define UDS_SUPPORT_SESSION_CONTROL   1   /* 0x10 */
#define UDS_SUPPORT_READ_DATA         1   /* 0x22 */
#define UDS_SUPPORT_WRITE_DATA        1   /* 0x2E */
#define UDS_SUPPORT_TESTER_PRESENT    1   /* 0x3E */
#define UDS_SUPPORT_ECU_RESET         1   /* 0x11 */
#define UDS_SUPPORT_ROUTINE_CONTROL   1   /* 0x31 */
```

### 4. Auto-respond and timing

```c
#define UDS_SIMULATOR_AUTO_RESPOND    1      // 1 = echo SID|0x40; 0 = return NRC
#define UDS_S3_SERVER_MS           5000U     // S3 server session timeout
#define UDS_P2_SERVER_MS             50U     // P2 response time
```

---

## Protocol Compliance (ISO 13400-2)

### Unicast responses

All UDP request/response exchanges (VIN, Entity Status, Power Mode) are sent **unicast to the requester's address** — not to the multicast group. This follows ISO 13400-2 §7.5 and §7.6 and is required for conformance test tools that use ephemeral, unbound sockets.

### Generic Header NACK (PT 0x0000)

| Condition | NACK code |
|-----------|-----------|
| Invalid sync pattern | `0x00` INVALID_PATTERN |
| Unknown payload type | `0x01` UNKNOWN_PAYLOAD_TYPE |

### Vehicle identification (UDP)

| Payload type | Meaning | Behaviour |
|--------------|---------|-----------|
| `0x0001` | Vehicle ID request | Always answered |
| `0x0002` | Vehicle ID request with EID (6 bytes) | Answered only if the EID matches |
| `0x0003` | Vehicle ID request with VIN (17 bytes) | Answered only if the VIN matches |
| `0x0004` | Vehicle announcement / ID response | 33 bytes: VIN(17) LA(2) EID(6) GID(6) further-action(1) sync-status(1) |

Requests using the default protocol version `0xFF`/`0x00` are accepted for these three request types.

### Routing activation

The response is 9 bytes: tester address(2), entity address(2), code(1), reserved(4). A denied activation closes the socket.

| Condition | Response code |
|-----------|---------------|
| Source address outside `0x0E00`–`0x0FFF` | `0x00` unknown source address |
| Different source address on an already-activated socket | `0x02` |
| Activation type other than `0x00` / `0x01` | `0x06` unsupported activation type |
| Otherwise (including re-activation with the same address) | `0x10` success |

### Diagnostic message NACK (PT 0x8003)

| Condition | NACK code |
|-----------|-----------|
| Routing not activated, or source address differs from the activated one | `0x02` invalid source address |
| Target address is not `DOIP_ECU_LOGICAL_ADDRESS` | `0x03` unknown target address |

### Functional addressing

The ECU also answers diagnostic messages sent to its functional group address (`DOIP_FUNCTIONAL_LOGICAL_ADDRESS`, default `0xE400`; `DoIP_Config_t::functional_address` or `--functional-address` at runtime).

- The DoIP ACK carries the functional address as its source; the UDS response comes from the ECU's own logical address.
- As ISO 14229-1 requires, negative responses with NRC `0x11`, `0x12`, `0x31`, `0x7E` and `0x7F` are not sent for functional requests. All other responses are.

### Fault injection

Make the simulator misbehave on purpose to test a tester's error handling. Set `DoIP_Config_t::faults` at init, call `DoIP_SetFaults(handle, &faults)` at runtime (`NULL` clears), or use the CLI flags.

| Field / flag | Effect |
|--------------|--------|
| `response_delay_ms` / `--response-delay <ms>` | Delays every UDS response. Above `UDS_P2_SERVER_MS` the ECU first sends `7F <sid> 78` (response pending), repeats it every `UDS_P2_STAR_SERVER_MS / 2`, and answers further requests with NRC `0x21` until the real response goes out |
| `drop_diag_ack` / `--drop-ack` | No DoIP Diagnostic ACK (`0x8002`) |
| `drop_response` / `--drop-response` | UDS responses are swallowed |
| `force_diag_nack` / `--force-nack <code>` | Every diagnostic message is rejected with Diagnostic NACK (`0x8003`) and this code |

The bundled client waits through `7F xx 78` automatically.

### Protocol version

`DOIP_VERSION_2012` sends header version `0x02`/`0xFD`; `DOIP_VERSION_2019` (or `make DOIP_2019=1`) sends `0x03`/`0xFC`. Logical addresses are 16-bit in both.

### Alive Check Timeline

```
t=0s   Client activates routing
t=5s   Server sends PT=0x0007 (Alive Check Request)   ← DOIP_ALIVE_CHECK_INTERVAL_MS
t=7s   No response → server closes connection          ← + DOIP_ALIVE_CHECK_TIMEOUT_MS
```

---

## Using the Library

### In-tree (development)

```c
#include "include/doip.h"
```

```bash
gcc -I. -Wall -O2 -std=c11 -D_DEFAULT_SOURCE \
    my_app.c -L. -ldoip -lpthread -o my_app
LD_LIBRARY_PATH=. ./my_app
```

### After `make install`

```bash
sudo make install PREFIX=/usr/local
```

```c
#include <doip/doip.h>
```

```bash
gcc $(pkg-config --cflags --libs doip) my_app.c -o my_app
```

---

## Library Version

Current: **1.0.0**

```c
#include "include/doip_version.h"
printf("%s\n", DOIP_LIB_VERSION_STR);   // "1.0.0"
```

SONAME versioning: `libdoip.so.1.0.0` → `libdoip.so.1` → `libdoip.so`

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `make test-lib` fails linking | Library not built | Run `make lib` first |
| `error while loading shared libraries: libdoip.so.1` | DSO not in path | Prefix with `LD_LIBRARY_PATH=.` or run `make install` |
| Client gets `ERR_TIMEOUT (-3)` | Server not running or wrong IP | Start simulator first; check port 13400 firewall |
| Client gets `ERR_REJECTED (-5)` | Routing activation rejected | Check `DOIP_TESTER_LOGICAL_ADDRESS` matches server |
| UDP discovery returns no result | Broadcast blocked | Pass IP directly: `./doip_client_demo 127.0.0.1` |
| TLS build exits at start-up | cert/key not configured | Pass `--tls-cert` / `--tls-key`, or set `DoIP_Config_t::tls` before `DoIP_Init()` |
| TLS link fails: `-lssl not found` | OpenSSL missing | `sudo apt install libssl-dev` |
| IPv6 bind fails | Kernel IPv6 module not loaded | `sudo modprobe ipv6` or build without `DOIP_IPV6=1` |
| `make docs` fails: `doxygen: No such file` | Doxygen not installed | `sudo apt install doxygen` |
