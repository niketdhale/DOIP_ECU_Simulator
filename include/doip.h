/**
 * @file doip.h
 * @brief Umbrella header for the DoIP ECU Simulator library.
 *
 * Including this single header gives access to the complete public API:
 *  - DoIP Server (ECU Simulator) — see @ref doip_server_api
 *  - DoIP Client (Tester)       — see @ref doip_client_api
 *  - Logging subsystem           — see @ref doip_log_api
 *  - Protocol types & constants  — see @ref doip_types
 *  - Configuration macros        — see @ref doip_config
 *
 * @par In-tree build
 * @code
 *   #include "include/doip.h"
 *   gcc -I. -c my_app.c
 * @endcode
 *
 * @par Installed (pkg-config)
 * @code
 *   #include <doip/doip.h>
 *   gcc $(pkg-config --cflags --libs doip) -o my_app my_app.c
 * @endcode
 */
#ifndef DOIP_H
#define DOIP_H

/* Version & visibility macro */
#include "doip_version.h"

/* Configuration (protocol, logging, UDS) */
#include "../config/doip_config.h"
#include "../config/doip_log_config.h"
#include "../config/doip_uds_config.h"

/* Core types & constants */
#include "../core/doip_types.h"

/* Logging API */
#include "../core/doip_log.h"

/* Server (ECU Simulator) API */
#include "../doip_api.h"

/* Client (Tester) API */
#include "../client/doip_client.h"

#endif /* DOIP_H */
