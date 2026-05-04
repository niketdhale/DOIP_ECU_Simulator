# DoIP ECU Simulator

A C implementation of the **Diagnostic over Internet Protocol (DoIP)** stack for automotive ECU simulation, following **ISO 13400-2**. Provides both a full-featured ECU simulator (server) and a reusable DoIP client library, exportable as `libdoip.so` / `libdoip.a`.

---

## Features

- **DoIP Server (ECU Simulator)**
  - UDP Vehicle Announcement broadcasts (periodic, configurable count)
  - UDP VIN/EID request handling
  - UDP Entity Status (PT 0x4001) and Diagnostic Power Mode (PT 0x4003) responses — ISO 13400-2 conformance
  - TCP routing activation with multi-client support (up to 5 simultaneous testers)
  - Generic Header NACK (PT 0x0000) sent on malformed or unknown frames — ISO 13400-2 §7.2
  - Server-initiated Alive Check (PT 0x0007) with configurable interval and disconnect-on-timeout
  - UDS diagnostic message forwarding (ISO 14229) with per-client session state
  - S3 server session timeout enforcement
  - AUTOSAR-aligned finite state machine (FSM)
  - **Runtime-configurable ECU identity** — VIN, SW version, serial number, system name, EID, GID set via `DoIP_EcuIdentity_t` at init time (no recompile needed)
  - **Runtime UDS callback hook** (`on_uds_request`) — supply live NVM/sensor data before the built-in service table is consulted

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

/* Server-initiated Alive Check (ISO 13400-2) */
#define DOIP_ALIVE_CHECK_INTERVAL_MS    5000U   // probe idle client after 5 s
#define DOIP_ALIVE_CHECK_TIMEOUT_MS     2000U   // disconnect if no reply within 2 s
```

---

## Configuring UDS Request / Response

UDS behaviour in the simulator is controlled at four levels — from runtime (no recompile) down to compile-time toggles.

### 1. Runtime ECU identity — `DoIP_EcuIdentity_t`

Pass a populated identity struct to `DoIP_Init()` to set the values returned for VIN, software version, system name, serial number, EID, and GID without recompiling.

```c
DoIP_EcuIdentity_t identity = {0};
strncpy(identity.vin,              "MYVIN00000000001", DOIP_VIN_LENGTH);
strncpy(identity.software_version, "V2.5.0",          sizeof(identity.software_version) - 1);
strncpy(identity.system_name,      "My ECU Node",     sizeof(identity.system_name) - 1);
strncpy(identity.serial_number,    "SN-98765",        sizeof(identity.serial_number) - 1);
memset(identity.eid, 0x11, DOIP_EID_LENGTH);   /* 6-byte Entity ID */
memset(identity.gid, 0x22, DOIP_GID_LENGTH);   /* 6-byte Group ID  */
identity.further_action    = 0x00;              /* no further action required */
identity.vin_gw_sync_status = 0x00;             /* VIN/GW synchronized       */

DoIP_Config_t config = {0};
config.ecu_identity = &identity;
DoIP_Init(handle, &config);
```

If `ecu_identity` is `NULL`, the simulator falls back to the compile-time defaults in `doip_api.c`.

The identity is used in:
- **UDP responses** — VIN/EID/GID in Vehicle Announcement and VIN Request responses
- **UDS DID 0xF190** — VIN Number
- **UDS DID 0xF189** — Software Version
- **UDS DID 0xF197** — System Name
- **UDS DID 0xF18C** — ECU Serial Number

### 2. Runtime UDS hook — `on_uds_request`

Register a callback to intercept any UDS request before the built-in service table handles it. Return `0` to supply your own response, or `-1` to fall through to the built-in handler. This enables serving live data from NVM, sensors, or a DTC memory at runtime.

```c
/* Dispatch model:
 *
 *  Incoming UDS request
 *       |
 *       v
 *  on_uds_request callback (if set)
 *       |
 *  returns 0? ──> use callback response        (done)
 *       |
 *  returns -1
 *       |
 *       v
 *  built-in service table (session control, read DID, tester present…)
 *       |
 *  handled? ──> built-in response              (done)
 *       |
 *  not handled
 *       |
 *       v
 *  auto-respond (SID|0x40) or NRC per UDS_SIMULATOR_AUTO_RESPOND
 */
static int my_uds_hook(uint8_t sid,
                       const uint8_t *req, uint16_t req_len,
                       uint8_t *resp, uint16_t resp_size, uint16_t *resp_len_out,
                       void *ctx)
{
    (void)ctx; (void)resp_size;

    if (sid == UDS_SID_READ_DATA_BY_ID && req_len >= 3) {
        uint16_t did = ((uint16_t)req[1] << 8) | req[2];

        if (did == 0xA001) {                /* custom: odometer from NVM */
            uint32_t odo = nvm_read_odometer();
            resp[0] = UDS_SID_READ_DATA_BY_ID_RES;
            resp[1] = 0xA0; resp[2] = 0x01;
            resp[3] = (odo >> 16) & 0xFF;
            resp[4] = (odo >>  8) & 0xFF;
            resp[5] =  odo        & 0xFF;
            *resp_len_out = 6;
            return 0;   /* handled */
        }
    }
    return -1;  /* not handled — use built-in */
}

DoIP_Config_t config = {0};
config.ecu_identity   = &identity;
config.on_uds_request = my_uds_hook;
config.user_context   = &my_nvm_handle;
DoIP_Init(handle, &config);
```

### 3. Enable / disable services — `config/doip_uds_config.h`

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

### 4. Auto-respond and timing parameters

```c
/* config/doip_uds_config.h */
#define UDS_SIMULATOR_AUTO_RESPOND    1      /* 1 = echo SID|0x40; 0 = return NRC */
#define UDS_S3_SERVER_MS           5000U     /* S3 server session timeout (ms) */
#define UDS_P2_SERVER_MS             50U     /* P2 response time (ms) */

/* Session type flags */
#define UDS_ECU_SUPPORTS_DEFAULT      1
#define UDS_ECU_SUPPORTS_EXTENDED     1
#define UDS_ECU_SUPPORTS_PROGRAMMING  0     /* set to 1 to allow programming session */
```

Per-client session state (`UdsClientContext_t`) is maintained automatically — each TCP connection gets its own context tracking the active session, last activity timestamp, and security state.

---

## Protocol Compliance (ISO 13400-2)

The following ISO 13400-2 server behaviours are implemented for conformance testing compatibility.

### Generic Header NACK (PT 0x0000)

The server sends a Generic NACK when it receives an unrecognised or malformed frame, rather than silently dropping it.

| Condition | NACK code |
|-----------|-----------|
| Invalid sync pattern in header | `0x00` (INVALID_PATTERN) |
| Unknown payload type | `0x01` (UNKNOWN_PAYLOAD_TYPE) |

### Entity Status Response (PT 0x4002)

Send a UDP request with payload type `0x4001` to query server capacity:

```
Request:  PT=0x4001, payload length=0
Response: PT=0x4002
  [0]    node_type          0x01 = DoIP node
  [1]    max_open_sockets   DOIP_MAX_TCP_CLIENTS (default: 5)
  [2]    curr_open_sockets  number of currently connected TCP clients
  [3-6]  max_data_size      DOIP_MAX_PAYLOAD_SIZE in big-endian
```

### Diagnostic Power Mode Response (PT 0x4004)

Send a UDP request with payload type `0x4003`:

```
Request:  PT=0x4003, payload length=0
Response: PT=0x4004
  [0]    power_mode   0x01 = ready for diagnostics
```

### Server-initiated Alive Check (PT 0x0007)

The server monitors ACTIVATED TCP clients for inactivity. After `DOIP_ALIVE_CHECK_INTERVAL_MS` (default 5 s) with no frames received from a client, the server sends an Alive Check Request (PT 0x0007, empty payload). If the client does not respond with PT 0x0008 within `DOIP_ALIVE_CHECK_TIMEOUT_MS` (default 2 s), the connection is closed.

```
Timeline for idle client:

  t=0s   Client activates routing
  t=5s   Server sends PT=0x0007 (Alive Check Request)
  t=7s   No response → server closes client connection
```

These timing constants can be adjusted in `config/doip_config.h`.

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
