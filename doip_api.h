/**
 * @file doip_api.h
 * @brief DoIP Server (ECU Simulator) public API.
 *
 * Provides the lifecycle, configuration, and runtime control functions
 * for a DoIP entity that simulates an ECU.  The implementation handles
 * UDP vehicle announcements, TCP routing activation, and UDS diagnostic
 * message forwarding according to ISO 13400-2.
 *
 * @defgroup doip_server_api Server API
 * @{
 */
#ifndef DOIP_API_H
#define DOIP_API_H

#include <stdint.h>
#include <stdbool.h>
#include "include/doip_version.h"

/**
 * @brief Opaque handle for the DoIP Simulator instance.
 *
 * All server API calls require a valid handle obtained from DoIP_Create().
 * The internal layout is hidden to preserve ABI stability.
 *
 * @code
 *   DoIP_Handle_t *handle = DoIP_Create();
 * @endcode
 */
typedef struct DoIP_Context DoIP_Handle_t;

/**
 * @brief Configuration structure for the DoIP simulator.
 *
 * Allows runtime configuration of logging, UDS session timeouts,
 * and optional event callbacks.  Pass NULL to DoIP_Init() to use
 * compiled-in defaults.
 */
typedef struct {
    /* Logging Configuration */
    uint8_t  log_level;           /**< Log verbosity: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=VERBOSE */
    const char *log_file_path;    /**< Path to log file, or NULL for console-only output */

    /* FSM / Timing Configuration */
    uint32_t s3_server_timeout_ms; /**< UDS S3 session timeout in ms (default 5000) */

    /* Event Callbacks (Optional) */

    /**
     * @brief Called when the FSM state changes (e.g., IDLE -> ROUTING_ACTIVE).
     * @param[in] old_state  Previous state enum value.
     * @param[in] new_state  New state enum value.
     * @param[in] user_ctx   Context pointer passed in DoIP_Config_t::user_context.
     */
    void (*on_state_change)(uint8_t old_state, uint8_t new_state, void *user_ctx);

    /**
     * @brief Called when a DoIP message is received.
     * @param[in] payload_type  DoIP payload type (e.g., 0x8001 for Diagnostic Message).
     * @param[in] data          Payload data pointer.
     * @param[in] len           Payload length in bytes.
     * @param[in] user_ctx      Context pointer passed in DoIP_Config_t::user_context.
     */
    void (*on_rx_message)(uint16_t payload_type, const uint8_t *data, uint32_t len, void *user_ctx);

    void *user_context;           /**< Opaque user data forwarded to all callbacks */
} DoIP_Config_t;

/* ===== Lifecycle API ===== */

/**
 * @brief Create a new DoIP Simulator instance.
 *
 * Allocates internal memory and initialises all fields to safe defaults.
 * The returned handle must eventually be freed with DoIP_Destroy().
 *
 * @return Opaque handle on success, or NULL if memory allocation fails.
 */
DOIP_API DoIP_Handle_t* DoIP_Create(void);

/**
 * @brief Initialise the simulator with the given configuration.
 *
 * Opens network sockets (UDP + TCP), validates the configuration,
 * and prepares the internal FSM.  If @p config is NULL, compiled-in
 * defaults from @c doip_config.h are used.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 * @param[in]     config  Configuration structure, or NULL for defaults.
 * @retval  0  Initialisation succeeded.
 * @retval <0  Error code (e.g., socket bind failure).
 */
DOIP_API int DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config);

/**
 * @brief Non-blocking tick function — drives network I/O and state machines.
 *
 * Must be called periodically (e.g., every 1 ms) from the application's
 * main loop.  Each call processes pending UDP/TCP events and advances
 * the DoIP FSM.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 */
DOIP_API void DoIP_Tick(DoIP_Handle_t *handle);

/**
 * @brief De-initialise the simulator.
 *
 * Closes all sockets and stops background services while keeping
 * the handle allocated.  The handle may be re-initialised by calling
 * DoIP_Init() again.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 */
DOIP_API void DoIP_DeInit(DoIP_Handle_t *handle);

/**
 * @brief Destroy the simulator instance and free all memory.
 *
 * After this call the handle is invalid and must not be used again.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 */
DOIP_API void DoIP_Destroy(DoIP_Handle_t *handle);

/* ===== Runtime Control API ===== */

/**
 * @brief Change the logging level at runtime.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 * @param[in]     level   New log level (0=ERROR .. 4=VERBOSE).
 */
DOIP_API void DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level);

/**
 * @brief Enable or disable periodic UDP Vehicle Announcements at runtime.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 * @param[in]     enable  @c true to start announcements, @c false to stop.
 */
DOIP_API void DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable);

/** @} */ /* end of doip_server_api group */

#endif /* DOIP_API_H */
