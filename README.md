# DoIP ECU Simulator

A C implementation of the **Diagnostic over Internet Protocol (DoIP)** stack for automotive ECU simulation, following **ISO 13400-2**. Provides both a full-featured ECU simulator (server) and a reusable DoIP client library, exportable as `libdoip.so` / `libdoip.a`.

---

## Features

- **DoIP Server (ECU Simulator)**
  - UDP Vehicle Announcement broadcasts (periodic, configurable count)
  - UDP VIN/EID request handling
  - TCP routing activation with multi-client support (up to 5 simultaneous testers)
  - UDS diagnostic message forwarding (ISO 14229) with per-client session state
  - S3 server session timeout enforcement
  - AUTOSAR-aligned finite state machine (FSM)

- **DoIP Client Library**
  - UDP vehicle discovery (broadcast or unicast)
  - TCP connect + routing activation
  - UDS request/response transactions (`DoIP_Client_Transact`)
  - Transparent handling of DiagACK (0x8002) and Alive Check (0x0007)

- **Library Export**
  - Shared library (`libdoip.so`) with SONAME versioning
  - Static library (`libdoip.a`)
  - Symbol visibility control (`-fvisibility=hidden` + `DOIP_API`)
  - pkg-config integration (`doip.pc`)
  - Doxygen API documentation

- **Supported UDS Services** (ISO 14229)
  - `0x10` Session Control
  - `0x11` ECU Reset
  - `0x22` Read Data By Identifier (VIN, SW Version, Serial Number…)
  - `0x2E` Write Data By Identifier
  - `0x31` Routine Control
  - `0x3E` Tester Present
  - Auto-response mode for unsupported SIDs

---

## Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                    Application / Tests                       │
│              main.c          client/main_client.c            │
│         (ECU Simulator)          (DoIP Tester)               │
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
│(UDP/    │ │(TCP/    │  └───────────────────────┘
│ bcast)  │ │routing) │
└────┬────┘ └───┬─────┘
     │          │
     └────┬─────┘
          │
┌─────────▼────────────────────────────┐
│              core/                    │
│  doip_frame.c   doip_log.c           │
│  (serialize)    (logging)             │
└──────────────────────────────────────┘
```

---

## Directory Structure

```
.
├── main.c                  # ECU simulator entry point
├── doip_api.c/.h           # Public server API (DoIP_Create, DoIP_Init, DoIP_Tick…)
│
├── core/
│   ├── doip_frame.c/.h     # DoIP frame serialization / deserialization
│   ├── doip_log.c/.h       # Thread-safe logging (colors, timestamps, modules)
│   ├── doip_types.h        # Protocol structs & payload type constants
│   └── doip_det.c          # Development Error Tracer stub
│
├── transport/
│   ├── doip_udp.c/.h       # UDP: vehicle announcements + VIN/EID request handling
│   ├── doip_tcp.c/.h       # TCP: multi-client server, routing activation
│   └── doip_uds.c/.h       # UDS: diagnostic service handlers
│
├── state/
│   └── doip_fsm.c/.h       # Finite State Machine + S3 session timeout
│
├── client/
│   ├── doip_client.c/.h    # DoIP Client library implementation
│   └── main_client.c       # Client demo (discovery → connect → UDS transactions)
│
├── include/
│   ├── doip.h              # Umbrella header (consumers: #include "include/doip.h")
│   └── doip_version.h      # Version constants + DOIP_API visibility macro
│
├── config/
│   ├── doip_config.h       # Protocol version, ports, addresses, timeouts
│   ├── doip_log_config.h   # Log levels, module filter, feature toggles
│   └── doip_uds_config.h   # UDS SIDs, NRCs, DIDs, timing parameters
│
├── example/
│   └── consumer.c          # Minimal example linking against libdoip.so
│
├── test/
│   ├── udp_discovery_test.c  # Standalone UDP discovery test (legacy)
│   ├── test_routing_act.py   # Python integration test: routing activation + UDS
│   └── test_multi_client.py  # Python integration test: 3 concurrent clients
│
├── Makefile                # Build system
├── Doxyfile                # Doxygen configuration
└── doip.pc.in              # pkg-config template
```

---

## Requirements

- **GCC** (≥ 4.8) with C11 support
- **POSIX** sockets (`-D_DEFAULT_SOURCE`)
- **pthreads** (`-lpthread`)
- **Linux** (tested on Arch Linux; any modern distro works)
- Optional: `doxygen` for API docs (`sudo pacman -S doxygen`)
- Optional: `python3` for Python integration tests

---

## Build

```bash
# Build both executables (ECU simulator + client demo)
make

# Build shared + static library (libdoip.so.1.0.0 + libdoip.a)
make lib

# Build library AND link test binaries against it
make test-lib

# Generate Doxygen HTML documentation → docs/html/index.html
make docs

# Clean all build artifacts
make clean
```

---

## Running the Simulator

### Terminal 1 — ECU Simulator (server)

```bash
./doip_ecu_sim
# With file logging:
./doip_ecu_sim --log-file /tmp/doip.log
```

Expected output:
```
[INFO ] DoIP API: Initializing with S3=5000 ms
[INFO ] Configuration validation passed.
[INFO ] UDP socket bound on port 13400
[INFO ] TCP server listening on port 13400 (max 5 clients)
DoIP Simulator Running. Press Ctrl+C to exit.
[INFO ] Sending vehicle announcement #1/5
```

### Terminal 2 — C Client Demo

```bash
# Auto-discover via UDP broadcast:
./doip_client_demo

# Connect directly by IP:
./doip_client_demo 127.0.0.1
```

Expected output:
```
[CLIENT] Connecting to 127.0.0.1:13400...
[CLIENT] TCP connection established
[CLIENT] Routing activated (code=0x10)
[CLIENT] OK: Extended session active
[CLIENT] OK: VIN: WBAXXXXXXXXXXXXXX
[CLIENT] OK: SW Version read
[CLIENT] OK: Tester Present
[CLIENT] OK: ECU Reset sent
[CLIENT] Test PASSED
```

### Terminal 2 — Python Integration Tests

```bash
# Single client: routing activation + UDS transactions
python3 test/test_routing_act.py

# 3 concurrent clients
python3 test/test_multi_client.py
```

---

## Using the Library

### In-tree (development)

```c
#include "include/doip.h"   /* pulls in entire public API */
```

```bash
gcc -I. -Wall -O2 -std=c11 -D_DEFAULT_SOURCE \
    my_app.c -L. -ldoip -lpthread -o my_app

LD_LIBRARY_PATH=. ./my_app
```

### After `make install` (via pkg-config)

```bash
make install PREFIX=/usr/local   # or: sudo make install
```

```c
#include <doip/doip.h>
```

```bash
gcc $(pkg-config --cflags --libs doip) my_app.c -o my_app
```

### Library Test Binaries

`make test-lib` builds three programs all linked against `libdoip.so`:

| Binary | Source | Purpose |
|--------|--------|---------|
| `test_lib_server` | `main.c` | Full ECU simulator via library |
| `test_lib_client` | `client/main_client.c` | Full client demo via library |
| `test_lib_example` | `example/consumer.c` | Minimal API usage example |

```bash
LD_LIBRARY_PATH=. ./test_lib_server           # Terminal 1
LD_LIBRARY_PATH=. ./test_lib_example 127.0.0.1  # Terminal 2
```

---

## Public API Reference

### Server API (`doip_api.h`)

```c
DoIP_Handle_t* DoIP_Create(void);
int            DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config);
void           DoIP_Tick(DoIP_Handle_t *handle);   // call every ~1ms
void           DoIP_DeInit(DoIP_Handle_t *handle);
void           DoIP_Destroy(DoIP_Handle_t *handle);
void           DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level);
void           DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable);
```

### Client API (`client/doip_client.h`)

```c
// Lifecycle
DoIP_Client_t*      DoIP_Client_Create(void);
DoIP_ClientStatus_t DoIP_Client_Init(DoIP_Client_t *c, const DoIP_ClientConfig_t *cfg);
void                DoIP_Client_Destroy(DoIP_Client_t *c);

// Discovery (stateless — no handle needed)
DoIP_ClientStatus_t DoIP_Client_Discover(const char *ip, uint16_t port,
                                          uint32_t timeout_ms,
                                          DoIP_DiscoveryResult_t *out);

// Connection
DoIP_ClientStatus_t DoIP_Client_Connect(DoIP_Client_t *c, const char *ip, uint16_t port);
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

// State
bool DoIP_Client_IsConnected(const DoIP_Client_t *c);
bool DoIP_Client_IsRoutingActive(const DoIP_Client_t *c);
```

Return codes: `DOIP_CLIENT_OK (0)`, `ERR_PARAM (-1)`, `ERR_SOCKET (-2)`, `ERR_TIMEOUT (-3)`, `ERR_NACK (-4)`, `ERR_REJECTED (-5)`, `ERR_NOT_READY (-6)`, `ERR_IO (-7)`

---

## Configuration

All configuration is in the `config/` headers — no runtime config files needed.

| Header | What it controls |
|--------|-----------------|
| `config/doip_config.h` | Protocol version (2012/2019), logical addresses, ports, timeouts, feature toggles |
| `config/doip_log_config.h` | Default log level, ANSI colours, timestamps, per-module enable bitmask |
| `config/doip_uds_config.h` | UDS SIDs, NRCs, DIDs, timing (P2, S3), session support flags |

### Key tunables in `doip_config.h`

```c
#define DOIP_PROTOCOL_VERSION_SELECTED  DOIP_VERSION_2012   // or DOIP_VERSION_2019
#define DOIP_ECU_LOGICAL_ADDRESS        0x1003U
#define DOIP_TESTER_LOGICAL_ADDRESS     0x0E00U
#define DOIP_UDP_PORT                   13400U
#define DOIP_TCP_PORT                   13400U
#define DOIP_ANNOUNCE_INTERVAL_MS       2000U   // 2 s between announcements
#define DOIP_ANNOUNCE_COUNT_MAX         5       // send 5 then stop
#define DOIP_RX_TIMEOUT_MS              2000U
#define DOIP_ROUTING_ACTIVATION_TIMEOUT_MS  5000U
```

---

## Configuring UDS Request / Response

UDS behaviour in the simulator is controlled at three levels:

### 1. Enable / disable services — `config/doip_uds_config.h`

Each UDS service can be compiled in or out with a toggle macro:

```c
#define UDS_SUPPORT_SESSION_CONTROL   1   /* 0x10 — set to 0 to disable */
#define UDS_SUPPORT_READ_DATA         1   /* 0x22 */
#define UDS_SUPPORT_WRITE_DATA        0   /* 0x2E — disabled by default */
#define UDS_SUPPORT_TESTER_PRESENT    1   /* 0x3E */
#define UDS_SUPPORT_ECU_RESET         1   /* 0x11 */
#define UDS_SUPPORT_ROUTINE_CONTROL   1   /* 0x31 */
```

The list of SIDs that the server claims to support (sent back in NRC responses) is controlled by:

```c
#define UDS_SUPPORTED_SIDS  \
    UDS_SID_SESSION_CONTROL, UDS_SID_ECU_RESET, \
    UDS_SID_READ_DATA_BY_ID, UDS_SID_TESTER_PRESENT, \
    UDS_SID_ROUTINE_CONTROL
```

If a client sends a SID not in this list the server returns NRC `0x11` (serviceNotSupported).

### 2. Customise response data — `transport/doip_uds.c`

Each service has a dedicated handler function in the dispatch table. Edit the handler to change what data is returned.

**Example — add a new DID to `handle_read_data_by_id`:**

```c
/* transport/doip_uds.c — inside handle_read_data_by_id() */
switch (did) {
    case UDS_DID_VIN_NUMBER:
        memcpy(&res[3], "MYVIN00000000001", 17);   /* <- change VIN here */
        *res_len = 20;
        break;

    case UDS_DID_SOFTWARE_VERSION:
        memcpy(&res[3], "V2.3.1", 6);              /* <- change SW version */
        *res_len = 3 + 6;
        break;

    case 0xF199:                                    /* <- add a new DID */
        memcpy(&res[3], "2024-12-01", 10);
        *res_len = 3 + 10;
        break;

    default:
        res[0] = 0x7F; res[1] = UDS_SID_READ_DATA_BY_ID;
        res[2] = UDS_NRC_REQUEST_OUT_OF_RANGE;
        *res_len = 3;
        return -1;
}
```

Add corresponding DID constant in `config/doip_uds_config.h`:

```c
#define UDS_DID_PRODUCTION_DATE     0xF199U
```

### 3. Auto-respond to unhandled services — `UDS_SIMULATOR_AUTO_RESPOND`

When a SID is in the supported list but has no explicit handler, the auto-respond fallback returns a positive response by mirroring the request with `SID | 0x40`:

```c
/* config/doip_uds_config.h */
#define UDS_SIMULATOR_AUTO_RESPOND  1   /* 1 = echo positive response; 0 = return NRC */
```

Set to `0` if you want unsupported sub-functions to return a NRC instead.

### 4. Session and timing parameters

```c
/* config/doip_uds_config.h */
#define UDS_S3_SERVER_MS            5000U   /* S3 server session timeout (ms) */
#define UDS_P2_SERVER_MS            50U     /* P2 response time (ms) */

/* Session type flags */
#define UDS_ECU_SUPPORTS_DEFAULT      1
#define UDS_ECU_SUPPORTS_EXTENDED     1
#define UDS_ECU_SUPPORTS_PROGRAMMING  0     /* set to 1 to allow programming session */
```

Per-client session state (`UdsClientContext_t`) is maintained automatically by the server — each TCP connection gets its own context tracking the active session, last activity timestamp, and security state.

---

## Library Version

Current: **1.0.0**

```c
#include "include/doip_version.h"
printf("%s\n", DOIP_LIB_VERSION_STR);   // "1.0.0"
```

SONAME versioning: `libdoip.so.1.0.0` → `libdoip.so.1` → `libdoip.so`

---

## Verifying Symbol Visibility

```bash
# Only DOIP_API-annotated functions should appear as exported (T) symbols
nm -D libdoip.so | grep ' T '
```

All internal helpers (frame serialization, transport internals) should be absent — only the public API surfaces.

---

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| `make test-lib` fails linking | Library not built | Run `make lib` first, then `make test-lib` |
| `error while loading shared libraries: libdoip.so.1` | DSO not in linker path | Prefix with `LD_LIBRARY_PATH=.` or run `make install` |
| Client gets `ERR_TIMEOUT (-3)` | Server not running or wrong IP | Start `doip_ecu_sim` first; check firewall on port 13400 |
| Client gets `ERR_REJECTED (-5)` | Routing activation rejected | Check `DOIP_TESTER_LOGICAL_ADDRESS` matches server config |
| UDP discovery returns no result | Broadcast blocked | Pass server IP directly: `./doip_client_demo 127.0.0.1` |
| `make docs` fails with `doxygen: No such file` | Doxygen not installed | `sudo pacman -S doxygen` (Arch) / `sudo apt install doxygen` (Debian) |
