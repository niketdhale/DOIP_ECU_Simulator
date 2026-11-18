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
#include "core/doip_types.h"
#include "config/doip_config.h"

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
    uint8_t  log_level;           /**< Log verbosity: 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=VERBOSE (used as given when a config is passed; NULL config = INFO) */
    const char *log_file_path;    /**< Path to log file, or NULL for console-only output */

    /* FSM / Timing Configuration */
    uint32_t s3_server_timeout_ms; /**< UDS S3 session timeout in ms (default 5000) */

    /* Event Callbacks (Optional) */

    /**
     * @brief Called when the FSM state (DoIP_State_t in state/doip_fsm.h) changes.
     *
     * The FSM follows the UDS session: IDLE or ROUTING_ACTIVE -> DIAG_SESSION when
     * a tester enters a non-default session, -> ROUTING_ACTIVE when it returns to
     * the default session or resets the ECU, -> IDLE when S3 expires.
     * Runs inside DoIP_Tick(); do not call this handle's API from it.
     * @param[in] old_state  Previous state.
     * @param[in] new_state  New state.
     * @param[in] user_ctx   Context pointer passed in DoIP_Config_t::user_context.
     */
    void (*on_state_change)(uint8_t old_state, uint8_t new_state, void *user_ctx);

    /**
     * @brief Called for every complete DoIP frame received over TCP or UDP,
     *        before it is processed (vehicle announcements are not reported).
     *
     * Runs inside DoIP_Tick(); do not call this handle's API from it.
     * @param[in] payload_type  DoIP payload type (e.g., 0x8001 for Diagnostic Message).
     * @param[in] data          Payload data pointer (valid only during the call).
     * @param[in] len           Payload length in bytes.
     * @param[in] user_ctx      Context pointer passed in DoIP_Config_t::user_context.
     */
    void (*on_rx_message)(uint16_t payload_type, const uint8_t *data, uint32_t len, void *user_ctx);

    void *user_context;           /**< Opaque user data forwarded to all callbacks */

    /**
     * @brief ECU identity data (VIN, SW version, serial number, EID, GID).
     *
     * Set to a populated DoIP_EcuIdentity_t to override the built-in defaults.
     * NULL uses the compiled-in defaults ("WBAXXXXXXXXXXXXXX", "V1.0.0", etc.).
     * The pointed-to struct is copied internally during DoIP_Init().
     */
    const DoIP_EcuIdentity_t *ecu_identity;

    /**
     * @brief Runtime UDS request hook — called before built-in service handlers.
     *
     * Use this to serve DID / DTC data from NVM, live sensors, or any dynamic
     * source.  The hook is called for every incoming UDS request.
     *
     * @param[in]  sid           UDS Service ID (e.g. 0x22 = ReadDataByIdentifier).
     * @param[in]  req           Full UDS request bytes (SID + parameters).
     * @param[in]  req_len       Length of @p req.
     * @param[out] resp          Buffer for the positive UDS response (SID|0x40 + data).
     * @param[in]  resp_size     Size of @p resp buffer.
     * @param[out] resp_len_out  Set to the number of bytes written into @p resp.
     * @param[in]  user_ctx      Pointer from DoIP_Config_t::user_context.
     * @retval  0   Request handled — use @p resp directly, skip built-in handler.
     * @retval -1   Not handled — fall through to built-in handler.
     */
    int (*on_uds_request)(uint8_t sid,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_size, uint16_t *resp_len_out,
                          void *user_ctx);

    /* ----- Async / Non-blocking event callbacks (all optional, may be NULL) ----- */

    /**
     * @brief Called when a new TCP client successfully connects.
     *
     * @param[in]  client_fd  The accepted socket file descriptor.
     * @param[in]  user_ctx   Context pointer from DoIP_Config_t::user_context.
     */
    void (*on_client_connect)(int client_fd, void *user_ctx);

    /**
     * @brief Called when a TCP client disconnects (timeout, error, or peer close).
     *
     * @param[in]  client_fd  The file descriptor that was closed.
     * @param[in]  user_ctx   Context pointer from DoIP_Config_t::user_context.
     */
    void (*on_client_disconnect)(int client_fd, void *user_ctx);

    /**
     * @brief Called when a complete DoIP frame arrives, before built-in dispatch.
     *
     * Allows the application to intercept or log any DoIP payload.
     *
     * @param[in]  client_fd File descriptor of the source client.
     * @param[in]  pt        DoIP payload type (e.g., 0x8001 = Diagnostic Message).
     * @param[in]  payload   Raw payload bytes.
     * @param[in]  plen      Payload length in bytes.
     * @param[in]  user_ctx  Context pointer from DoIP_Config_t::user_context.
     * @retval  0   Application handled the frame — skip built-in handler.
     * @retval -1   Not handled — fall through to built-in handler.
     */
    int (*on_frame_received)(int client_fd,
                             uint16_t pt, const uint8_t *payload, uint32_t plen,
                             void *user_ctx);

    /** ECU logical address; 0 = compiled-in DOIP_ECU_LOGICAL_ADDRESS.
     *  Valid: 0x0001-0x0DFF, 0x1000-0x7FFF, 0x8000-0xDFFF. */
    uint16_t logical_address;

    /** Functional (group) address the ECU also answers to;
     *  0 = compiled-in DOIP_FUNCTIONAL_LOGICAL_ADDRESS (0xE400). */
    uint16_t functional_address;

    /** UDP + TCP port; 0 = compiled-in DOIP_TCP_PORT (13400). */
    uint16_t port;

    /** Fault injection active from start-up; all-zero = none. See DoIP_SetFaults(). */
    DoIP_FaultConfig_t faults;

#if DOIP_ENABLE_TLS
    /** TLS server certificate, key, and optional CA bundle. */
    struct {
        const char *cert_file;   /**< Path to PEM server certificate.              */
        const char *key_file;    /**< Path to PEM private key.                     */
        const char *ca_file;     /**< CA bundle for peer verification (NULL=skip). */
        bool        verify_peer; /**< If true, require a valid client certificate. */
    } tls;
#endif /* DOIP_ENABLE_TLS */
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
 * Announcements are on by default (DOIP_ENABLE_PERIODIC_ANNOUNCE): a burst of
 * DOIP_ANNOUNCE_COUNT_MAX, DOIP_ANNOUNCE_INTERVAL_MS apart, after DoIP_Init().
 * Disabling stops a running burst; enabling starts a new complete burst on the
 * next DoIP_Tick(). May be called before DoIP_Init() to start silent.
 *
 * @param[in,out] handle  Handle returned by DoIP_Create().
 * @param[in]     enable  @c true to announce (again), @c false to stop.
 */
DOIP_API void DoIP_EnablePeriodicAnnounce(DoIP_Handle_t *handle, bool enable);

/**
 * @brief Add or replace a data identifier answered by ReadDataByIdentifier (0x22).
 *
 * Lets the ECU serve extra DIDs without an on_uds_request hook. The data is
 * copied. Up to UDS_DID_TABLE_SIZE DIDs of up to UDS_DID_DATA_MAX bytes each;
 * they belong to the handle and survive DoIP_DeInit(). May be called before
 * or after DoIP_Init().
 *
 * The identity DIDs 0xF190 (VIN), 0xF186, 0xF182 and 0xF18C cannot be
 * registered here: they come from DoIP_Config_t::ecu_identity.
 *
 * @param[in,out] handle    Handle returned by DoIP_Create().
 * @param[in]     did       Data identifier.
 * @param[in]     data      Value (1 .. UDS_DID_DATA_MAX bytes).
 * @param[in]     len       Length of @p data.
 * @param[in]     writable  Also accept WriteDataByIdentifier (0x2E) for this DID,
 *                          under the same session and security conditions as
 *                          the built-in writable DIDs.
 * @retval  0  Registered.
 * @retval -1  Invalid argument, identity DID, or table full.
 */
DOIP_API int DoIP_SetDid(DoIP_Handle_t *handle, uint16_t did,
                         const void *data, uint16_t len, bool writable);

/**
 * @brief Change fault injection while the simulator is running.
 *
 * @param[in,out] handle  Initialised handle.
 * @param[in]     faults  New settings, or NULL to clear all faults.
 * @retval  0  Applied.
 * @retval -1  @p handle is NULL or not initialised.
 */
DOIP_API int DoIP_SetFaults(DoIP_Handle_t *handle, const DoIP_FaultConfig_t *faults);

/** @} */ /* end of doip_server_api group */

#endif /* DOIP_API_H */
