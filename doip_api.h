#ifndef DOIP_API_H
#define DOIP_API_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Opaque handle for the DoIP Simulator instance.
 * Usage: DoIP_Handle_t* handle = DoIP_Create();
 */
typedef struct DoIP_Context DoIP_Handle_t;

/**
 * @brief Configuration structure for the simulator.
 * Allows runtime configuration of logging, timeouts, and callbacks.
 */
typedef struct {
    /* Logging Configuration */
    uint8_t  log_level;           /* 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG, 4=VERBOSE */
    const char *log_file_path;    /* Path to log file, or NULL for console only */

    /* FSM / Timing Configuration */
    uint32_t s3_server_timeout_ms; /* UDS Session timeout (default 5000ms) */

    /* Event Callbacks (Optional) */
    /**
     * @brief Called when the FSM state changes (e.g., IDLE -> ROUTING_ACTIVE)
     * @param old_state Previous state enum value
     * @param new_state New state enum value
     * @param user_ctx  Context pointer passed during Init
     */
    void (*on_state_change)(uint8_t old_state, uint8_t new_state, void *user_ctx);

    /**
     * @brief Called when a DoIP message is received
     * @param payload_type DoIP Payload Type (e.g., 0x8001)
     * @param data         Payload data pointer
     * @param len          Payload length
     * @param user_ctx     Context pointer passed during Init
     */
    void (*on_rx_message)(uint16_t payload_type, const uint8_t *data, uint32_t len, void *user_ctx);

    void *user_context;           /* User data passed to callbacks */
} DoIP_Config_t;

/* ===== Lifecycle API ===== */

/**
 * @brief Create a new DoIP Simulator instance.
 * Allocates memory and initializes default values.
 * @return Handle to the instance, or NULL on allocation failure.
 */
DoIP_Handle_t* DoIP_Create(void);

/**
 * @brief Initialize the simulator with the given configuration.
 * Starts network sockets, validates config, and prepares the FSM.
 * @param handle The instance handle.
 * @param config Pointer to configuration structure (can be NULL for defaults).
 * @return 0 on success, negative error code on failure.
 */
int DoIP_Init(DoIP_Handle_t *handle, const DoIP_Config_t *config);

/**
 * @brief Non-blocking tick function.
 * Must be called periodically (e.g., every 1ms) to process network I/O and state machines.
 * @param handle The instance handle.
 */
void DoIP_Tick(DoIP_Handle_t *handle);

/**
 * @brief Deinitialize the simulator.
 * Closes sockets and stops services, but keeps the handle allocated.
 * @param handle The instance handle.
 */
void DoIP_DeInit(DoIP_Handle_t *handle);

/**
 * @brief Destroy the simulator instance and free memory.
 * @param handle The instance handle.
 */
void DoIP_Destroy(DoIP_Handle_t *handle);

/* ===== Runtime Control API ===== */

/**
 * @brief Set the logging level dynamically.
 * @param handle The instance handle.
 * @param level  New log level (0-4).
 */
void DoIP_SetLogLevel(DoIP_Handle_t *handle, uint8_t level);

#endif /* DOIP_API_H */