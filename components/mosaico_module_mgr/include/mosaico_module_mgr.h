/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Mosaico module discovery and ownership manager.
 *
 * @note Public APIs are thread-safe. Change callbacks are serialized by the manager event task.
 */

#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MOSAICO_MODULE_MGR_EEPROM_MAGIC          "ESP"                          /*!< EEPROM descriptor magic. */
#define MOSAICO_MODULE_MGR_EEPROM_MAGIC_LEN      3U                             /*!< EEPROM descriptor magic length. */
#define MOSAICO_MODULE_MGR_EEPROM_IMAGE_SIZE     0x86U                          /*!< EEPROM V1 image size in bytes. */
#define MOSAICO_MODULE_MGR_SLOT_AUTO             MOSAICO_MODULE_MGR_SLOT_COUNT  /*!< Select any matching slot. */

/** @brief Default module manager configuration. */
#define MOSAICO_MODULE_MGR_DEFAULT_CONFIG() { \
    .scan_period_ms = 250,                    \
    .descriptor_retry_ms = 2000,              \
    .debounce_count = 3,                      \
}

/** @brief Mosaico module slot. */
typedef enum {
    MOSAICO_MODULE_MGR_SLOT_LEFT = 0,  /*!< Left slot. */
    MOSAICO_MODULE_MGR_SLOT_RIGHT,     /*!< Right slot. */
    MOSAICO_MODULE_MGR_SLOT_COUNT,     /*!< Number of slots. */
} mosaico_module_mgr_slot_t;

/** @brief Module board type stored in the EEPROM descriptor. */
typedef enum {
    MOSAICO_BOARD_TYPE_CORE        = 0x01, /*!< Core board. */
    MOSAICO_BOARD_TYPE_POWER       = 0x02, /*!< Power board. */
    MOSAICO_BOARD_TYPE_DOCK        = 0x03, /*!< Dock board. */
    MOSAICO_BOARD_TYPE_HANDLE      = 0x04, /*!< Handle board. */
    MOSAICO_BOARD_TYPE_BALANCE_CAR = 0x05, /*!< Balance car board. */
    MOSAICO_BOARD_TYPE_DISPLAY     = 0x06, /*!< Display board. */
    MOSAICO_BOARD_TYPE_CAMERA      = 0x07, /*!< Camera board. */
    MOSAICO_BOARD_TYPE_SENSOR      = 0x08, /*!< Sensor board. */
    MOSAICO_BOARD_TYPE_IO_EXP      = 0x09, /*!< I/O expansion board. */
    MOSAICO_BOARD_TYPE_TOF         = 0x10, /*!< Time-of-flight board. */
    MOSAICO_BOARD_TYPE_MATRIX_LED  = 0x11, /*!< Matrix LED board. */
    MOSAICO_BOARD_TYPE_THERMAL     = 0x12, /*!< Thermal imaging board. */
    MOSAICO_BOARD_TYPE_RELAY       = 0x13, /*!< Relay board. */
    MOSAICO_BOARD_TYPE_INTERACT    = 0x16, /*!< Interaction board. */
} mosaico_board_type_t;

/** @brief Debounced module presence state. */
typedef enum {
    MOSAICO_MODULE_PRESENCE_UNKNOWN = 0,  /*!< Presence has not been established. */
    MOSAICO_MODULE_PRESENCE_ABSENT,       /*!< The slot did not acknowledge after debounce. */
    MOSAICO_MODULE_PRESENCE_PRESENT,      /*!< The slot acknowledged after debounce. */
} mosaico_module_presence_t;

/** @brief EEPROM descriptor validation state. */
typedef enum {
    MOSAICO_MODULE_DESCRIPTOR_UNKNOWN = 0,  /*!< No current descriptor result is available. */
    MOSAICO_MODULE_DESCRIPTOR_VALID,        /*!< The descriptor format and CRC fields are valid. */
    MOSAICO_MODULE_DESCRIPTOR_INVALID,      /*!< The EEPROM was read, but descriptor validation failed. */
} mosaico_module_descriptor_state_t;

/** @brief Slot ownership state. */
typedef enum {
    MOSAICO_MODULE_OWNER_FREE = 0,  /*!< The slot can be claimed. */
    MOSAICO_MODULE_OWNER_CLAIMED,   /*!< A client owns the slot. */
    MOSAICO_MODULE_OWNER_RESTORING, /*!< Slot discovery hardware is being restored. */
} mosaico_module_owner_state_t;

/**
 * @brief Decoded EEPROM V1 descriptor.
 *
 * This is a logical representation of the EEPROM data and is not a packed raw image.
 */
typedef struct {
    char magic[3];                /*!< Descriptor magic, not null-terminated. */
    uint8_t board_type;           /*!< Board type from @ref mosaico_board_type_t. */
    uint16_t board_id;            /*!< Vendor-defined board identifier. */
    uint16_t hw_version;          /*!< Hardware version. */
    uint16_t sw_version;          /*!< Software compatibility version. */
    uint16_t vendor_id;           /*!< Vendor identifier. */
    uint32_t board_flags;         /*!< Board capability flags. */
    uint32_t serial_number;       /*!< Board serial number. */
    char board_name[32];          /*!< Board name, not guaranteed to be null-terminated. */
    uint16_t desc_crc16;          /*!< Descriptor section CRC16. */
    uint32_t manufacture_date;    /*!< Vendor-defined manufacturing date. */
    uint16_t batch_number;        /*!< Manufacturing batch number. */
    uint16_t factory_id;          /*!< Factory identifier. */
    uint16_t mfg_crc16;           /*!< Manufacturing section CRC16. */
    uint16_t param_version;       /*!< Parameter block version. */
    uint16_t param_length;        /*!< Valid length of @c param_data. */
    uint8_t param_data[64];       /*!< Module-specific parameter data. */
    uint16_t param_crc16;         /*!< Parameter section CRC16. */
} mosaico_module_mgr_eeprom_v1_t;

/** @brief Snapshot of one module slot. */
typedef struct {
    mosaico_module_mgr_slot_t slot;                    /*!< Slot represented by this snapshot. */
    mosaico_module_presence_t presence;                 /*!< Debounced presence state. */
    mosaico_module_descriptor_state_t descriptor_state; /*!< Descriptor validation state. */
    mosaico_module_owner_state_t owner_state;          /*!< Current ownership state. */
    uint32_t generation;                               /*!< Incremented whenever this snapshot changes. */
    esp_err_t last_error;                              /*!< Current resource, probe, or descriptor error. */
    uint8_t eeprom_addr;                               /*!< Slot EEPROM 7-bit I2C address. */
    mosaico_module_mgr_eeprom_v1_t eeprom;             /*!< Last decoded descriptor; valid only when descriptor_state is VALID. */
} mosaico_module_mgr_info_t;

/** @brief Opaque lease returned by @ref mosaico_module_mgr_claim. */
typedef struct {
    mosaico_module_mgr_slot_t slot; /*!< Claimed slot. */
    uint32_t id;                    /*!< Lease identifier; zero is invalid. */
} mosaico_module_lease_t;

/**
 * @brief Module claim behavior flags.
 *
 * @note MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED requires MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO and is only valid when the EEPROM address is independent of the slot control GPIO.
 */
typedef enum {
    MOSAICO_MODULE_CLAIM_ALLOW_INVALID_DESCRIPTOR = (1U << 0), /*!< Allow an invalid descriptor in an explicit slot. */
    MOSAICO_MODULE_CLAIM_USE_SLOT_CONTROL_GPIO = (1U << 1),   /*!< Suspend discovery while the client reuses the control GPIO; restore it on release. */
    MOSAICO_MODULE_CLAIM_PROBE_WHILE_CLAIMED = (1U << 2),     /*!< Keep probing a fixed-address EEPROM while claimed. */
} mosaico_module_claim_flag_t;

/** @brief Module claim configuration. */
typedef struct {
    mosaico_board_type_t expected_type; /*!< Required board type for a valid descriptor. */
    mosaico_module_mgr_slot_t slot;     /*!< Explicit slot or @ref MOSAICO_MODULE_MGR_SLOT_AUTO. */
    uint32_t timeout_ms;                /*!< Wait timeout in milliseconds; zero performs one attempt. */
    uint32_t flags;                     /*!< Bitwise OR of @ref mosaico_module_claim_flag_t. */
} mosaico_module_mgr_claim_config_t;

/** @brief Slot snapshot change flags. */
typedef enum {
    MOSAICO_MODULE_CHANGE_PRESENCE = (1U << 0),   /*!< Presence changed. */
    MOSAICO_MODULE_CHANGE_DESCRIPTOR = (1U << 1), /*!< Descriptor state or contents changed. */
    MOSAICO_MODULE_CHANGE_OWNER = (1U << 2),      /*!< Ownership changed. */
    MOSAICO_MODULE_CHANGE_ERROR = (1U << 3),      /*!< Current error changed. */
} mosaico_module_change_t;

/** @brief Coalesced module change event. */
typedef struct {
    uint32_t changes;                 /*!< Bitwise OR of @ref mosaico_module_change_t. */
    mosaico_module_mgr_info_t info;   /*!< Latest slot snapshot. */
} mosaico_module_mgr_event_t;

/**
 * @brief Module change callback.
 *
 * The callback runs from the manager event task. Keep it short and do not call
 * @ref mosaico_module_mgr_deinit from it. The event pointer is valid only for
 * the duration of the callback.
 *
 * @param[in] event Module change event.
 * @param[in] user_data User context passed to @ref mosaico_module_mgr_subscribe.
 */
typedef void (*mosaico_module_mgr_event_callback_t)(const mosaico_module_mgr_event_t *event, void *user_data);

/** @brief Opaque subscription handle. */
typedef struct {
    uint32_t index;       /*!< Internal subscriber slot. */
    uint32_t generation;  /*!< Handle generation used to reject stale subscriptions. */
} mosaico_module_subscription_t;

/** @brief Module manager configuration. */
typedef struct {
    uint32_t scan_period_ms;       /*!< Periodic presence scan interval in milliseconds. */
    uint32_t descriptor_retry_ms;  /*!< Invalid or failed descriptor retry interval in milliseconds. */
    uint8_t debounce_count;        /*!< Consecutive matching probes required for a presence change. */
} mosaico_module_mgr_config_t;

/**
 * @brief Start the singleton module manager.
 *
 * Calling this function again with @c NULL while the manager is running is
 * idempotent. A non-NULL configuration must match the active configuration.
 *
 * @param[in] config Manager configuration, or NULL to use @c MOSAICO_MODULE_MGR_DEFAULT_CONFIG().
 *
 * @return
 *      - ESP_OK: Manager started or already running with a compatible configuration.
 *      - ESP_ERR_INVALID_ARG: Configuration contains an invalid value.
 *      - ESP_ERR_INVALID_STATE: Manager is busy or running with a different configuration.
 *      - ESP_ERR_NO_MEM: A synchronization object or task could not be created.
 *      - ESP_ERR_TIMEOUT: Task rollback timed out.
 *      - Others: BSP or I2C initialization failed.
 */
esp_err_t mosaico_module_mgr_init(const mosaico_module_mgr_config_t *config);

/**
 * @brief Stop the module manager.
 *
 * All leases must be released before this function is called. The function may
 * be called again after ESP_ERR_TIMEOUT to continue an incomplete shutdown.
 *
 * @return
 *      - ESP_OK: Manager stopped or was already stopped.
 *      - ESP_ERR_INVALID_STATE: A slot is claimed, shutdown is already in progress, or the call originates from a module callback.
 *      - ESP_ERR_TIMEOUT: A manager task or claim waiter did not stop in time.
 *      - Others: An EEPROM device could not be detached.
 */
esp_err_t mosaico_module_mgr_deinit(void);

/**
 * @brief Subscribe to future module changes.
 *
 * Events may coalesce multiple changes and contain the latest slot snapshot.
 * Use @ref mosaico_module_mgr_get_info after subscribing when initial state is
 * required; existing state is not emitted automatically.
 *
 * @param[in] callback Event callback.
 * @param[in] user_data User context passed to the callback; it must remain valid until every callback using it returns.
 * @param[out] out_subscription Subscription handle.
 *
 * @return
 *      - ESP_OK: Subscription created.
 *      - ESP_ERR_INVALID_ARG: A required pointer is NULL.
 *      - ESP_ERR_INVALID_STATE: Manager is not running.
 *      - ESP_ERR_NO_MEM: The fixed subscriber table is full.
 */
esp_err_t mosaico_module_mgr_subscribe(mosaico_module_mgr_event_callback_t callback, void *user_data,
                                       mosaico_module_subscription_t *out_subscription);

/**
 * @brief Remove a module change subscription.
 *
 * This function waits for an active callback to finish. When called by that
 * callback itself, it disables future callbacks and returns immediately.
 *
 * @param[in] subscription Subscription returned by @ref mosaico_module_mgr_subscribe.
 *
 * @return
 *      - ESP_OK: Subscription removed.
 *      - ESP_ERR_INVALID_ARG: Subscription pointer or index is invalid.
 *      - ESP_ERR_INVALID_STATE: Manager is not running, the handle is stale, or another unsubscribe is waiting.
 */
esp_err_t mosaico_module_mgr_unsubscribe(const mosaico_module_subscription_t *subscription);

/**
 * @brief Get the latest snapshot for a slot.
 *
 * @param[in] slot Slot to query.
 * @param[out] out_info Slot snapshot.
 *
 * @return
 *      - ESP_OK: Snapshot copied.
 *      - ESP_ERR_INVALID_ARG: Slot or output pointer is invalid.
 *      - ESP_ERR_INVALID_STATE: Manager is not running.
 */
esp_err_t mosaico_module_mgr_get_info(mosaico_module_mgr_slot_t slot, mosaico_module_mgr_info_t *out_info);

/**
 * @brief Atomically claim a matching free module.
 *
 * A successful call returns an exclusive lease that must be released with
 * @ref mosaico_module_mgr_release. When an invalid descriptor is explicitly
 * allowed, the expected board type cannot be verified.
 *
 * @param[in] config Claim criteria and timeout.
 * @param[out] out_lease Exclusive lease; cleared before the claim is attempted.
 *
 * @return
 *      - ESP_OK: A matching module was claimed.
 *      - ESP_ERR_INVALID_ARG: Claim criteria or flags are invalid.
 *      - ESP_ERR_INVALID_STATE: Manager stopped while waiting.
 *      - ESP_ERR_NOT_FOUND: No module matched a non-blocking claim.
 *      - ESP_ERR_TIMEOUT: No module matched before the timeout.
 *      - ESP_ERR_NO_MEM: The fixed claim waiter table is full.
 */
esp_err_t mosaico_module_mgr_claim(const mosaico_module_mgr_claim_config_t *config, mosaico_module_lease_t *out_lease);

/**
 * @brief Release an exclusive module lease.
 *
 * If slot GPIO restoration fails, ownership returns to CLAIMED and the same
 * lease remains valid so the release can be retried.
 *
 * @param[in] lease Lease returned by @ref mosaico_module_mgr_claim.
 *
 * @return
 *      - ESP_OK: Lease released and slot discovery restored.
 *      - ESP_ERR_INVALID_ARG: Lease pointer, slot, or identifier is invalid.
 *      - ESP_ERR_INVALID_STATE: Manager is not running or the lease is stale.
 *      - Others: Slot GPIO restoration failed.
 */
esp_err_t mosaico_module_mgr_release(const mosaico_module_lease_t *lease);

/**
 * @brief Request an asynchronous immediate scan.
 *
 * @param[in] slot Explicit slot or @ref MOSAICO_MODULE_MGR_SLOT_AUTO for all slots.
 *
 * @return
 *      - ESP_OK: Scan request accepted.
 *      - ESP_ERR_INVALID_ARG: Slot is invalid.
 *      - ESP_ERR_INVALID_STATE: Manager is not running.
 */
esp_err_t mosaico_module_mgr_request_rescan(mosaico_module_mgr_slot_t slot);

/**
 * @brief Convert a slot to a stable log string.
 *
 * @param[in] slot Module slot.
 * @return "left", "right", "auto", or "unknown".
 */
const char *mosaico_module_mgr_slot_to_name(mosaico_module_mgr_slot_t slot);

/**
 * @brief Convert a board type to a stable log string.
 *
 * @param[in] type Module board type.
 * @return Board type name, or "Unknown" for an unsupported value.
 */
const char *mosaico_module_mgr_type_to_name(mosaico_board_type_t type);

#ifdef __cplusplus
}
#endif
