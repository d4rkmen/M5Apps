/**
 * @file flood.h
 * @brief Flood-based mesh networking component for ESP-NOW
 *
 * The Flood component provides a simple yet powerful mesh networking layer built on ESP-NOW.
 * It implements flooding-based message propagation with duplicate detection, device discovery,
 * and persistent message storage.
 *
 * Key Features:
 * - Automatic device discovery through periodic HELLO beacons
 * - Message flooding with TTL and duplicate detection
 * - Three device roles: CLIENT (end-user), ROUTER (store & forward), REPEATER (relay only)
 * - Persistent device metadata and message storage on SD/flash
 * - Efficient pagination for message history (O(1) random access)
 * - Callback-based event notification
 * - Thread-safe operation with mutex protection
 *
 * Typical Usage:
 * @code
 * // Initialize and start
 * flood_init("/sdcard/flood", 1);
 * @endcode
 *
 * @note Requires ESP-IDF with ESP-NOW support
 * @note WiFi must be available (initialized internally)
 */

#ifndef FLOOD_H
#define FLOOD_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_now.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ============================================================================
     * Protocol Constants
     * ========================================================================== */

#define MESH_PROTOCOL_VERSION 1            /**< Current protocol version */
#define MESH_PERSISTENT_VERSION 1          /**< Persistent data format version */
#define MESH_MAGIC_NUMBER 0x464C5544       /**< Protocol magic number ("FLUD" in ASCII) */
#define FLOOD_MAX_TTL CONFIG_FLOOD_MAX_TTL /**< Maximum time-to-live (hops) for packets */
#define MESH_CACHE_CLEANUP_INTERVAL 300000 /**< Cache cleanup interval: 5 minutes (ms) */
#define MESH_MAX_PAYLOAD_SIZE (ESP_NOW_MAX_DATA_LEN - sizeof(mesh_packet_header_t)) /**< Maximum payload size after header */
#define MESH_MAX_CHANNELS 100                          /**< Maximum number of channels (reserved for future use) */
#define MESH_SLEEP_TIMEOUT 300000                      /**< Sleep timeout: 5 minutes (ms) */
#define MESH_MAX_CACHE_ENTRIES CONFIG_FLOOD_CACHE_SIZE /**< Maximum duplicate detection cache entries */
#define MESH_MAX_QUEUE_SIZE CONFIG_FLOOD_QUEUE_SIZE    /**< Maximum queued packets */
#define MESH_CACHE_TIMEOUT 300000                      /**< Cache entry timeout: 5 minutes (ms) */
#define MESH_ACK_STATUS_SUCCESS 0x01                   /**< Acknowledgment status: success */
#define MESH_ACK_STATUS_FAILURE 0x00                   /**< Acknowledgment status: failure */
#define MESH_ACK_TIMEOUT 5000                          /**< ACK timeout: 5 seconds (ms) */
#define MESH_RESEND_MAX_TRIES 3                        /**< Maximum number of retries for ACK */
#define MESH_MAX_NAME_LENGTH 31                        /**< Maximum length of device / channel name */
    /* ========================================================================
     * Type Definitions
     * ======================================================================== */

    /**
     * @brief Packet type identifiers for mesh network communication
     */
    typedef enum
    {
        MESH_PACKET_TYPE_HELLO = 0x01,   /**< Hello/beacon packet for device discovery */
        MESH_PACKET_TYPE_MESSAGE = 0x02, /**< Channel/group chat message */
        MESH_PACKET_TYPE_PRIVATE = 0x03, /**< Private peer-to-peer message */
        MESH_PACKET_TYPE_ACK = 0x04,         /**< Acknowledgment packet */
        MESH_PACKET_TYPE_TRACE_REQ = 0x05,   /**< Traceroute request - collects path hops */
        MESH_PACKET_TYPE_TRACE_REPLY = 0x06, /**< Traceroute reply - returns collected path */
    } mesh_packet_type_t;

    /**
     * @brief Device role definitions determining forwarding behavior
     */
    typedef enum
    {
        MESH_ROLE_CHANNEL = 0x00, /**< Channel (group chat) */
        MESH_ROLE_CLIENT = 0x01,  /**< End-user devices (mobile, chat devices) - no forwarding */
        MESH_ROLE_ROUTER = 0x02,  /**< Infrastructure/relay devices (always-on)*/
        MESH_ROLE_REPEATER = 0x03 /**< Network infrastructure nodes (extend range) - retranslates packets, no store & forward */
    } FLOOD_DEVICE_ROLE_t;

    /**
     * @brief Device capability flags (can be combined with bitwise OR)
     */
    typedef enum
    {
        MESH_CAP_POWER_SAVE = 0x01,      /**< Device supports power saving modes */
        MESH_CAP_HIGH_THROUGHPUT = 0x02, /**< Device can handle high message volumes */
        MESH_CAP_STORAGE = 0x08,         /**< Device has persistent storage capability */
        MESH_CAP_ENCRYPTION = 0x10,      /**< Device supports message encryption */
    } FLOOD_DEVICE_CAPABILITIES_t;

    /**
     * @brief Packet control flags (can be combined with bitwise OR)
     */
    typedef enum
    {
        MESH_FLAG_BROADCAST = 0x01,    /**< Broadcast message to all devices */
        MESH_FLAG_ENCRYPTED = 0x02,    /**< Payload is encrypted */
        MESH_FLAG_ACK_REQUIRED = 0x04, /**< Acknowledgment is required from recipient */
        MESH_FLAG_RETRY = 0x08,        /**< Retry sending message */
        MESH_FLAG_FORWARDED = 0x10     /**< Message forwarded by another device (not original sender) */
    } mesh_flags_t;

    /**
     * @brief Base packet header structure for all mesh network packets
     * @note All multi-byte fields are in native byte order
     */
    typedef struct
    {
        uint32_t magic;        /**< Protocol magic number (0x464C5544 = "FLUD") */
        uint8_t version;       /**< Protocol version */
        uint8_t type;          /**< Packet type (mesh_packet_type_t) */
        uint8_t flags;         /**< Control flags (mesh_flags_t bitmask) */
        uint8_t hops;          /**< Number of hops traveled (incremented by forwarders) */
        uint8_t ttl;           /**< Time To Live - remaining hops allowed */
        uint32_t sequence;     /**< Unique sequence number for duplicate detection */
        uint8_t source_mac[6]; /**< Original sender MAC address */
        uint8_t dest_mac[6];   /**< Destination MAC address (0xFF...FF for broadcast) */
    } __attribute__((packed)) mesh_packet_header_t;

    /**
     * @brief Persistent device information stored in flash/SD storage
     * @note Stored in <context_path>/devices/<MAC>/meta.bin
     */
    typedef struct
    {
        uint32_t magic;           /**< Magic number for validation (MESH_MAGIC_NUMBER) */
        uint8_t version;          /**< Metadata structure version */
        uint8_t mac[6];           /**< Device MAC address (unique identifier) */
        uint8_t name[32];         /**< Device display name (UTF-8, null-terminated) */
        FLOOD_DEVICE_ROLE_t role; /**< Device role (client/router/repeater) */
        uint8_t capabilities;     /**< Device capabilities bitmask */
    } mesh_device_persistent_t;

    /**
     * @brief Volatile device information maintained in RAM only
     * @note Cleared on restart, managed as dynamic linked list
     */
    typedef struct
    {
        uint8_t mac[6];           /**< Device MAC address (for lookup) */
        uint32_t last_seen;       /**< Most recent contact timestamp */
        uint8_t signal_strength;  /**< Signal quality (0-100) */
        uint8_t hops;             /**< Number of hops to reach device (0-255) */
        uint8_t battery_level;    /**< Battery level (0-100, 255 = unknown/AC powered) */
        uint16_t unread_messages; /**< Unread message count in current session */
    } mesh_device_volatile_t;

    /**
     * @brief Combined device information structure
     * @note Used for API calls requiring both persistent and volatile data
     */
    typedef struct
    {
        mesh_device_persistent_t persistent;  /**< Persistent device data */
        mesh_device_volatile_t volatile_data; /**< Volatile runtime data */
    } mesh_device_info_t;

    /**
     * @brief Channel information structure
     * @note Stored in <context_path>/channels/<channel_name>/meta.bin
     */
    typedef struct
    {
        uint32_t magic;                                 /**< Magic number for validation (MESH_MAGIC_NUMBER) */
        uint8_t version;                                /**< Metadata structure version */
        uint8_t channel_name[MESH_MAX_NAME_LENGTH + 1]; /**< Channel name (UTF-8, null-terminated) */
        uint32_t channel_secret[32];                    /**< Channel authentication secret */
    } mesh_channel_persistent_t;

    /**
     * @brief Volatile channel information maintained in RAM only
     * @note Cleared on restart, managed as dynamic linked list
     */
    typedef struct
    {
        uint8_t channel_name[MESH_MAX_NAME_LENGTH + 1]; /**< Channel name (for lookup) */
        uint32_t last_seen;                             /**< Most recent message timestamp */
        uint16_t unread_messages;                       /**< Unread message count in current session */
    } mesh_channel_volatile_t;

    /**
     * @brief Combined channel information structure
     * @note Used for API calls requiring both persistent and volatile data
     */
    typedef struct
    {
        mesh_channel_persistent_t persistent;  /**< Persistent channel data */
        mesh_channel_volatile_t volatile_data; /**< Volatile channel data */
    } mesh_channel_info_t;
    /**
     * @brief Message cache entry for duplicate detection
     */
    typedef struct
    {
        uint32_t sequence;     /**< Message sequence number */
        uint8_t source_mac[6]; /**< Source device MAC address */
        uint32_t timestamp;    /**< Timestamp when message was received */
    } mesh_packet_cache_entry_t;

    /**
     * @brief Hello/beacon packet for device discovery and presence announcement
     */
    typedef struct
    {
        mesh_packet_header_t header;                   /**< Base packet header */
        uint8_t device_name[MESH_MAX_NAME_LENGTH + 1]; /**< Device name (UTF-8, null-terminated) */
        FLOOD_DEVICE_ROLE_t role;                      /**< Device role */
        uint8_t capabilities;                          /**< Device capabilities bitmask */
        uint8_t battery_level;                         /**< Battery level (0-100, 255 = unknown/AC) */
    } __attribute__((packed)) mesh_hello_packet_t;

    /**
     * @brief Channel/group chat message packet
     */
    typedef struct
    {
        mesh_packet_header_t header;                    /**< Base packet header */
        uint32_t message_id;                            /**< Message ID (index in messages.bin for delivery tracking) */
        uint8_t channel_name[MESH_MAX_NAME_LENGTH + 1]; /**< Channel name identifier */
        uint8_t channel_secret[32];                     /**< Channel authentication secret */
        uint8_t message_type;                           /**< Message content type (text, sticker, etc.) */
        uint16_t message_length;                        /**< Payload length in bytes */
        uint8_t message_text[];                         /**< Variable-length message payload */
    } __attribute__((packed)) mesh_message_packet_t;

    /**
     * @brief Private peer-to-peer message packet
     */
    typedef struct
    {
        mesh_packet_header_t header; /**< Base packet header */
        uint32_t message_id;         /**< Message ID (index in messages.bin for delivery tracking) */
        uint8_t peer_secret[32];     /**< Peer authentication secret */
        uint8_t message_type;        /**< Message content type */
        uint16_t message_length;     /**< Payload length in bytes */
        uint8_t message_text[];      /**< Variable-length message payload */
    } __attribute__((packed)) mesh_private_packet_t;

    /**
     * @brief Acknowledgment packet for reliable delivery
     */
    typedef struct
    {
        mesh_packet_header_t header; /**< Base packet header */
        uint32_t ack_sequence;       /**< Sequence number being acknowledged */
        uint8_t status;              /**< Acknowledgment status (success/failure) */
        uint8_t reserved[3];         /**< Reserved for future use (padding) */
    } __attribute__((packed)) mesh_ack_packet_t;

    /* ========================================================================
     * Traceroute Structures
     * ======================================================================== */

#define FLOOD_TRACE_MAX_HOPS 9       /**< Maximum traceroute hop entries per direction */
#define FLOOD_TRACE_TIMEOUT_MS 15000 /**< Traceroute reply timeout (ms) */

#define FLOOD_TRACE_STATUS_PENDING 0
#define FLOOD_TRACE_STATUS_SUCCESS 1
#define FLOOD_TRACE_STATUS_FAILED 2

    /**
     * @brief Single hop entry in a traceroute path
     */
    typedef struct
    {
        uint8_t mac[6];  /**< Node MAC address at this hop */
        uint8_t signal;  /**< Signal quality (0-100) when packet was received at this hop */
    } __attribute__((packed)) flood_trace_hop_t;

    /**
     * @brief Traceroute request packet - each relay adds its hop info before forwarding
     */
    typedef struct
    {
        mesh_packet_header_t header;
        uint32_t trace_id;                          /**< Unique trace identifier */
        uint8_t hop_count;                          /**< Number of hops recorded so far */
        flood_trace_hop_t hops[FLOOD_TRACE_MAX_HOPS]; /**< Accumulated hop entries */
    } __attribute__((packed)) mesh_trace_request_t;

    /**
     * @brief Traceroute reply packet - carries forward path and accumulates return path
     */
    typedef struct
    {
        mesh_packet_header_t header;
        uint32_t trace_id;                                  /**< Matching trace ID from request */
        uint8_t route_to_count;                             /**< Number of hops in forward path */
        uint8_t route_back_count;                           /**< Number of hops in return path */
        flood_trace_hop_t route_to[FLOOD_TRACE_MAX_HOPS];   /**< Forward path (filled by request) */
        flood_trace_hop_t route_back[FLOOD_TRACE_MAX_HOPS]; /**< Return path (filled by reply relays) */
    } __attribute__((packed)) mesh_trace_reply_t;

    /**
     * @brief Traceroute result delivered to application via callback
     */
    typedef struct
    {
        uint32_t trace_id;
        uint8_t route_to_count;
        uint8_t route_back_count;
        flood_trace_hop_t route_to[FLOOD_TRACE_MAX_HOPS];
        flood_trace_hop_t route_back[FLOOD_TRACE_MAX_HOPS];
    } flood_trace_result_t;

    /* ---- Persistent trace record (stored in .trc files) ---- */

#define FLOOD_TRACE_MAX_RECORDS 50 /**< Max trace records per device file */

    /**
     * @brief Fixed-size trace record stored in traces.trc (circular file)
     */
    typedef struct
    {
        uint32_t trace_id;
        uint32_t timestamp;      /**< millis() when request was sent */
        uint32_t elapsed_ms;     /**< Round-trip time (0 if pending/failed) */
        uint32_t wall_time;      /**< Unix epoch (time(nullptr)) for display */
        uint8_t status;          /**< FLOOD_TRACE_STATUS_* */
        uint8_t route_to_count;
        uint8_t route_back_count;
        uint8_t _pad;
        flood_trace_hop_t route_to[FLOOD_TRACE_MAX_HOPS];
        flood_trace_hop_t route_back[FLOOD_TRACE_MAX_HOPS];
    } __attribute__((packed)) flood_trace_record_t;

    /**
     * @brief Header of traces.trc circular file
     */
    typedef struct
    {
        uint16_t count;     /**< Valid records stored (0..FLOOD_TRACE_MAX_RECORDS) */
        uint16_t write_pos; /**< Next write slot (circular) */
    } __attribute__((packed)) flood_trace_file_header_t;

    /**
     * @brief Message packet cache for duplicate detection
     */
    typedef struct
    {
        mesh_packet_cache_entry_t cache[MESH_MAX_CACHE_ENTRIES]; /**< Cache entries array */
        uint8_t cache_count;                                     /**< Current number of cached entries */
        uint32_t last_cleanup;                                   /**< Timestamp of last cleanup operation */
    } mesh_packet_cache_t;

    /**
     * @brief Queued packet structure for message buffering
     */
    typedef struct
    {
        uint8_t data[ESP_NOW_MAX_DATA_LEN]; /**< Packet data buffer */
        uint16_t length;                    /**< Actual data length */
    } mesh_queued_packet_t;

    // typedef struct
    // {
    //     mesh_queued_packet_t queue[MESH_MAX_QUEUE_SIZE];
    //     uint8_t head;
    //     uint8_t tail;
    //     uint8_t count;
    // } mesh_message_queue_t;

    /**
     * @brief Device table structure (maintained for backward compatibility)
     */
    typedef struct
    {
        uint32_t last_discovery; /**< Timestamp of last device discovery operation */
    } mesh_device_table_t;

    /* ========================================================================
     * Message Storage Constants
     * ======================================================================== */

#define MESSAGE_STATUS_RECEIVED 0x00
#define MESSAGE_STATUS_SENT 0x01            /**< Message has been sent to network */
#define MESSAGE_STATUS_DELIVERED 0x02       /**< Message was delivered to recipient (ACK received) */
#define MESSAGE_STATUS_DELIVERY_FAILED 0x03 /**< Message was not delivered to recipient (ACK not received) */

#define MESSAGE_MAX_PAYLOAD 200 /**< Maximum size of message payload in bytes */

    /**
     * @brief Fixed-size message record stored in messages.bin
     * @note Total size: 256 bytes per record for efficient file operations
     */
    typedef struct
    {
        uint8_t sender_mac[6];                     /**< Sender MAC address */
        uint32_t sequence;                         /**< Message sequence number */
        uint32_t timestamp;                        /**< Message timestamp (milliseconds since boot) */
        uint8_t status;                            /**< Message status flags (MESSAGE_STATUS_*) */
        uint8_t message_type;                      /**< Message content type (text, binary, etc.) */
        uint16_t message_length;                   /**< Actual message length (0-MESSAGE_MAX_PAYLOAD) */
        uint8_t message_data[MESSAGE_MAX_PAYLOAD]; /**< Fixed-size message payload buffer */
        uint8_t reserved[9];                       /**< Reserved for future use (padding to 256 bytes) */
    } __attribute__((packed)) message_record_t;

    /**
     * @brief Power management information (reserved for future use)
     */
    typedef struct
    {
        uint8_t mac[6];       /**< Device MAC address */
        int8_t rssi;          /**< Received Signal Strength Indicator */
        uint32_t last_update; /**< Last update timestamp */
        uint8_t power_level;  /**< Device power level (0-100) */
        uint8_t sleep_mode;   /**< Device sleep mode status */
    } mesh_power_info_t;

    /**
     * @brief Message authentication structure (reserved for future use)
     */
    typedef struct
    {
        uint8_t hmac[32];  /**< HMAC-SHA256 signature */
        uint8_t nonce[16]; /**< Random nonce for replay protection */
    } mesh_auth_t;

    /**
     * @brief Encryption header (reserved for future use)
     */
    typedef struct
    {
        uint8_t iv[16];       /**< Initialization vector */
        uint8_t auth_tag[16]; /**< Authentication tag for AEAD */
        uint8_t key_id[16];   /**< Key identifier */
    } mesh_crypto_header_t;

    /* ========================================================================
     * Core Lifecycle Functions
     * ======================================================================== */

    /**
     * @brief Initialize the flood mesh network component
     *
     * Initializes WiFi, creates necessary directories, allocates internal data structures,
     * and prepares the component for operation. Must be called before any other flood functions.
     *
     * @param context_path Root directory path for persistent storage (e.g., "/sdcard/flood")
     *                     Creates subdirectories: devices/ for device metadata and messages
     * @param channel WiFi channel to use for the mesh network (0-14)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if context_path is NULL or empty
     *         ESP_FAIL on initialization failure
     *
     * @note Creates context_path and devices/ subdirectory if they don't exist
     * @note Only needs to be called once; subsequent calls return ESP_OK
     */
    esp_err_t flood_init(const char* name, const char* context_path, int channel, int max_ttl, int hello_interval);

    /**
     * @brief Start the flood mesh network operation
     *
     * Initializes ESP-NOW, registers callbacks, and starts the background task that
     * processes incoming packets and manages the message queue.
     *
     * @return ESP_OK on success
     *         ESP_FAIL if not initialized or already running
     *
     * @note Must call flood_init() first
     * @note Automatically sends initial HELLO packet
     */
    esp_err_t flood_start(void);

    /**
     * @brief Stop the flood mesh network operation
     *
     * Stops the background task and pauses network operations while keeping
     * internal state intact. Can be restarted with flood_start().
     *
     * @return ESP_OK on success
     *         ESP_FAIL if not running
     *
     * @note Does not deinitialize ESP-NOW or WiFi
     * @note Useful for temporary suspension to save power
     */
    esp_err_t flood_stop(void);

    /**
     * @brief Deinitialize the flood mesh network component
     *
     * Stops operations, cleans up ESP-NOW, deinitializes WiFi, and frees all
     * allocated resources including device lists and message queues.
     *
     * @return ESP_OK on success
     *         ESP_FAIL on error
     *
     * @note Must call flood_stop() first if currently running
     * @note After calling this, must call flood_init() again to restart
     */
    esp_err_t flood_deinit(void);

    /* ========================================================================
     * Message Transmission Functions
     * ======================================================================== */

    /**
     * @brief Send an acknowledgment packet
     *
     * Sends an ACK packet to confirm receipt of a message. Used for reliable
     * message delivery confirmation.
     *
     * @param dest_mac Destination MAC address (6 bytes)
     * @param sequence Sequence number being acknowledged
     * @param status Acknowledgment status (MESH_ACK_STATUS_SUCCESS or MESH_ACK_STATUS_FAILURE)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if dest_mac is NULL
     *         ESP_FAIL on transmission error
     *
     * @note ACK packets are not forwarded by other nodes
     */
    esp_err_t flood_send_ack(const uint8_t* dest_mac, uint32_t sequence, uint8_t status);

    /**
     * @brief Send a raw message packet
     *
     * Sends a custom message with specified data payload. Used for sending
     * pre-formatted mesh packets.
     *
     * @param dest_mac Destination MAC address (use broadcast MAC for all devices)
     * @param data Packet data buffer (must include mesh_packet_header_t)
     * @param length Total packet length in bytes
     * @param flags Control flags (MESH_FLAG_*)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if dest_mac or data is NULL, or length is invalid
     *         ESP_FAIL on transmission error
     *
     * @note Caller is responsible for properly formatting the packet header
     * @note Maximum length is ESP_NOW_MAX_DATA_LEN (250 bytes)
     */
    esp_err_t flood_send_message(const uint8_t* dest_mac, const uint8_t* data, uint16_t length, uint8_t flags);

    /**
     * @brief Send a channel message
     *
     * Sends a message to a channel (group chat). The message is broadcast
     * to all devices in the mesh network.
     *
     * @param channel_name Channel name (null-terminated string)
     * @param data Message data buffer
     * @param length Message length in bytes
     * @param message_type Message content type (text, sticker, etc.)
     * @param flags Control flags (MESH_FLAG_*)
     * @return Message ID on success (>=0), -1 on error
     *         ESP_ERR_INVALID_ARG if channel_name or data is NULL, or length is invalid
     *         ESP_ERR_INVALID_STATE if flood is not initialized/running
     *         ESP_ERR_INVALID_SIZE if message length exceeds maximum
     *
     * @note Message is broadcast to all devices
     * @note Maximum length is MESSAGE_MAX_PAYLOAD (200 bytes)
     * @note Message is automatically saved to channel storage
     */
    esp_err_t flood_send_channel_message(
        const char* channel_name, const uint8_t* data, uint16_t length, uint8_t message_type, uint8_t flags);

    /**
     * @brief Send a private message
     *
     * Sends a private message to a specific device. Used for sending
     * private messages to a specific device.
     *
     * @param dest_mac Destination MAC address (6 bytes)
     * @param data Message data buffer
     * @param length Message length in bytes
     * @param flags Control flags (MESH_FLAG_*)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if dest_mac or data is NULL, or length is invalid
     *         ESP_FAIL on transmission error
     *
     * @note Maximum length is MESSAGE_MAX_PAYLOAD (200 bytes)
     */
    esp_err_t flood_send_private_message(const uint8_t* dest_mac, const uint8_t* data, uint16_t length, uint8_t flags);

    /**
     * @brief Send a HELLO/beacon packet
     *
     * Broadcasts device presence, name, role, capabilities, and battery level.
     * Used for device discovery and periodic presence announcement.
     *
     * @return ESP_OK on success
     *         ESP_FAIL on transmission error
     *
     * @note Automatically called periodically based on hello interval
     * @note Can be called manually to force immediate announcement
     */
    esp_err_t flood_send_hello(void);

    /* ========================================================================
     * Device Management Functions
     * ======================================================================== */

    /**
     * @brief Add a device to the mesh network
     *
     * Registers a new device or updates existing device information in both
     * persistent storage and volatile memory.
     *
     * @param device Pointer to device information structure
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if device is NULL or has invalid data
     *         ESP_FAIL on storage error
     *
     * @note Creates device directory and meta.bin file if new device
     * @note Updates both persistent and volatile device data
     */
    esp_err_t flood_add_device(const mesh_device_info_t* device);

    /**
     * @brief Remove a device from the mesh network
     *
     * Removes device from volatile memory. Persistent data remains on storage
     * but device will not appear in active device list.
     *
     * @param mac Device MAC address (6 bytes)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac is NULL
     *         ESP_ERR_NOT_FOUND if device not in volatile list
     *
     * @note Does not delete persistent files (messages.bin, meta.bin)
     * @note Device can be re-added by receiving packets from it
     */
    esp_err_t flood_remove_device(const uint8_t* mac);

    /* ========================================================================
     * Persistent Device Storage Functions
     * ======================================================================== */

    /**
     * @brief Save device persistent data to storage
     *
     * Writes device metadata to <context_path>/devices/<MAC>/meta.bin file.
     *
     * @param device Pointer to persistent device data structure
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if device is NULL or invalid
     *         ESP_FAIL on file I/O error
     *
     * @note Creates device directory if it doesn't exist
     * @note Overwrites existing meta.bin file
     */
    esp_err_t flood_save_device_persistent(const mesh_device_persistent_t* device);

    /**
     * @brief Load device persistent data from storage
     *
     * Reads device metadata from <context_path>/devices/<MAC>/meta.bin file.
     *
     * @param mac Device MAC address (6 bytes)
     * @param device Pointer to buffer for receiving device data
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac or device is NULL
     *         ESP_ERR_NOT_FOUND if device file doesn't exist
     *         ESP_FAIL on file I/O error or data corruption
     *
     * @note Validates magic number and version before returning data
     */
    esp_err_t flood_load_device_persistent(const uint8_t* mac, mesh_device_persistent_t* device);

    /**
     * @brief Load device persistent data from specific metadata file path
     *
     * Loads device metadata from an arbitrary meta.bin file path.
     * Useful for enumeration and batch operations.
     *
     * @param meta_path Full path to meta.bin file
     * @param device Pointer to buffer for receiving device data
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if meta_path or device is NULL
     *         ESP_ERR_NOT_FOUND if file doesn't exist
     *         ESP_FAIL on file I/O error or data corruption
     *
     * @note Validates magic number and version
     */
    esp_err_t flood_load_device_persistent_from_meta(const char* meta_path, mesh_device_persistent_t* device);

    /* ========================================================================
     * Volatile Device Management Functions
     * ======================================================================== */

    /**
     * @brief Update volatile (runtime) device information
     *
     * Updates in-memory device data such as signal strength, battery level,
     * last seen timestamp, etc. Data is lost on restart.
     *
     * @param mac Device MAC address (6 bytes)
     * @param volatile_data Pointer to volatile data structure
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac or volatile_data is NULL
     *         ESP_ERR_NO_MEM if failed to allocate memory for new device
     *
     * @note Creates new entry if device doesn't exist in volatile list
     * @note Uses dynamic linked list for storage
     */
    esp_err_t flood_update_device_volatile(const uint8_t* mac, const mesh_device_volatile_t* volatile_data);

    /**
     * @brief Get volatile (runtime) device information
     *
     * Retrieves current in-memory device data.
     *
     * @param mac Device MAC address (6 bytes)
     * @param volatile_data Pointer to buffer for receiving volatile data
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac or volatile_data is NULL
     *         ESP_ERR_NOT_FOUND if device not in volatile list
     */
    esp_err_t flood_get_device_volatile(const uint8_t* mac, mesh_device_volatile_t* volatile_data);

    /**
     * @brief Update volatile (runtime) channel information
     *
     * Updates or creates in-memory channel runtime data.
     *
     * @param channel_name Channel name (null-terminated string)
     * @param volatile_data Pointer to volatile data structure
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_name or volatile_data is NULL
     *         ESP_ERR_NO_MEM if failed to allocate memory for new channel
     *
     * @note Creates new entry if channel doesn't exist in volatile list
     * @note Uses dynamic linked list for storage
     */
    esp_err_t flood_update_channel_volatile(const char* channel_name, const mesh_channel_volatile_t* volatile_data);

    /**
     * @brief Get volatile (runtime) channel information
     *
     * Retrieves current in-memory channel data.
     *
     * @param channel_name Channel name (null-terminated string)
     * @param volatile_data Pointer to buffer for receiving volatile data
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_name or volatile_data is NULL
     *         ESP_ERR_NOT_FOUND if channel not in volatile list
     */
    esp_err_t flood_get_channel_volatile(const char* channel_name, mesh_channel_volatile_t* volatile_data);

    /* ========================================================================
     * Role and Capability Management Functions
     * ======================================================================== */

    /**
     * @brief Set this device's role in the mesh network
     *
     * Configures device behavior for packet forwarding and store & forward.
     *
     * @param role Device role (MESH_ROLE_CLIENT/ROUTER/REPEATER)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if role is invalid
     *
     * @note CLIENT: no forwarding
     * @note ROUTER: full forwarding + store & forward
     * @note REPEATER: retranslate packets only, no store & forward
     */
    esp_err_t flood_set_device_role(FLOOD_DEVICE_ROLE_t role);

    /**
     * @brief Get this device's current role
     *
     * @return Current device role
     */
    FLOOD_DEVICE_ROLE_t flood_get_device_role(void);

    /**
     * @brief Set this device's capability flags
     *
     * Configures device capabilities advertised in HELLO packets.
     *
     * @param capabilities Capability bitmask (MESH_CAP_*)
     * @return ESP_OK on success
     *
     * @note Can be combined: MESH_CAP_POWER_SAVE | MESH_CAP_STORAGE
     */
    esp_err_t flood_set_device_capabilities(uint8_t capabilities);

    /**
     * @brief Get this device's current capabilities
     *
     * @return Current capability bitmask
     */
    uint8_t flood_get_device_capabilities(void);

    /* ========================================================================
     * Battery Management Functions
     * ======================================================================== */

    /**
     * @brief Set this device's battery level
     *
     * Updates battery level advertised in HELLO packets.
     *
     * @param battery_level Battery level (0-100, or 255 for unknown/AC powered)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if battery_level > 100 and != 255
     */
    esp_err_t flood_set_battery_level(uint8_t battery_level);

    /**
     * @brief Get this device's current battery level
     *
     * @return Current battery level (0-100, or 255 for unknown/AC)
     */
    uint8_t flood_get_battery_level(void);

    /* ========================================================================
     * Signal Quality Functions
     * ======================================================================== */

    /**
     * @brief Convert RSSI value to signal quality percentage
     *
     * Converts raw RSSI (dBm) to a user-friendly percentage value.
     *
     * @param rssi RSSI value in dBm (typically -10 to -100)
     * @return Signal quality percentage (0-100)
     *
     * @note -10 dBm = 100%, -100 dBm = 0%
     * @note Uses linear mapping
     */
    uint8_t flood_rssi_to_percentage(int8_t rssi);

    /* ========================================================================
     * Context Management Functions
     * ======================================================================== */

    /**
     * @brief Get the configured context path
     *
     * Returns the root directory path used for persistent storage.
     *
     * @return Context path string (do not free)
     *
     * @note Returns pointer to internal buffer
     */
    const char* flood_get_context_path(void);

    /* ========================================================================
     * Packet Cache Management Functions
     * ======================================================================== */

    /**
     * @brief Add a packet to the duplicate detection cache
     *
     * Records a packet's sequence number and source to prevent reprocessing
     * duplicate packets.
     *
     * @param sequence Message sequence number
     * @param source_mac Source device MAC address (6 bytes)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if source_mac is NULL
     *         ESP_ERR_NO_MEM if cache is full (auto-cleanup attempted)
     *
     * @note Cache automatically cleaned when full (oldest entries removed)
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_cache_add(uint32_t sequence, const uint8_t* source_mac);

    /**
     * @brief Check if a packet exists in the cache
     *
     * Tests whether a packet has already been seen/processed.
     *
     * @param sequence Message sequence number
     * @param source_mac Source device MAC address (6 bytes)
     * @return true if packet is in cache (duplicate)
     *         false if packet is new
     *
     * @note Thread-safe with mutex protection
     */
    bool flood_cache_check(uint32_t sequence, const uint8_t* source_mac);

    /**
     * @brief Clean up expired cache entries
     *
     * Removes cache entries older than MESH_CACHE_TIMEOUT (5 minutes).
     *
     * @return ESP_OK on success
     *
     * @note Called automatically during normal operation
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_cache_cleanup(void);

    /* ========================================================================
     * Callback Types and Registration
     * ======================================================================== */

    /**
     * @brief Message received callback function type
     *
     * @param header Pointer to packet header
     * @param payload Pointer to payload data
     * @param length Payload length in bytes
     * @param rssi Received signal strength indicator (dBm)
     * @param user_data User-provided context pointer
     */
    typedef void (*flood_message_callback_t)(
        const mesh_packet_header_t* header, const uint8_t* payload, uint16_t length, int8_t rssi, void* user_data);

    /**
     * @brief
     *
     *
     *
     * @param mac MAC address of the device
     * @param message_id Message ID
     * @param status Message status
     * @param user_data User-provided context pointer
     */
    typedef void (*flood_message_status_callback_t)(const uint8_t* mac, int32_t message_id, uint8_t status, void* user_data);

    /**
     * @brief Packet sent / received callback function type
     *
     *
     *
     * @param data Pointer to packet data
     * @param length Packet length in bytes
     * @param user_data User-provided context pointer
     */
    typedef void (*flood_packet_callback_t)(const uint8_t* data, uint16_t length, void* user_data);

    /**
     * @brief Device added/updated callback function type
     *
     * @param device Pointer to device information
     * @param added true if new device, false if updated
     * @param user_data User-provided context pointer
     */
    typedef void (*flood_device_callback_t)(const mesh_device_info_t* device, bool added, void* user_data);

    /**
     * @brief Device enumeration callback function type
     *
     * Called once for each device during enumeration.
     *
     * @param device Pointer to device information
     * @param user_data User-provided context pointer
     * @return true to continue enumeration, false to stop
     */
    typedef bool (*flood_device_enum_callback_t)(const mesh_device_info_t* device, void* user_data);

    /**
     * @brief Channel enumeration callback function type
     *
     * Called once for each channel during enumeration.
     *
     * @param channel_id Channel identifier string
     * @param user_data User-provided context pointer
     * @return true to continue enumeration, false to stop
     */
    typedef bool (*flood_channel_enum_callback_t)(const mesh_channel_info_t* channel_info, void* user_data);

    /**
     * @brief Register callback for received messages
     *
     * Sets a callback function to be invoked when messages are received.
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     *
     * @note Only one callback can be registered at a time
     * @note Called from flood task context
     */
    esp_err_t flood_register_message_callback(flood_message_callback_t callback, void* user_data);

    /**
     * @brief Register callback for message status
     *
     * Sets a callback function to be invoked when message status is updated.
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     */
    esp_err_t flood_register_message_status_callback(flood_message_status_callback_t callback, void* user_data);
    /**
     * @brief Register callback for sent packets
     *
     * Sets a callback function to be invoked when packets are sent or received.
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     */
    esp_err_t flood_register_sent_packet_callback(flood_packet_callback_t callback, void* user_data);

    /**
     * @brief Register callback for received packets
     *
     * Sets a callback function to be invoked when packets are received.
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     */
    esp_err_t flood_register_received_packet_callback(flood_packet_callback_t callback, void* user_data);

    /**
     * @brief Register callback for device discovery/updates
     *
     * Sets a callback function to be invoked when devices are discovered or updated.
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     *
     * @note Only one callback can be registered at a time
     * @note Called from flood task context
     */
    esp_err_t flood_register_device_callback(flood_device_callback_t callback, void* user_data);

    /* ========================================================================
     * Traceroute Callback and API
     * ======================================================================== */

    /**
     * @brief Traceroute result callback function type
     *
     * @param result Pointer to traceroute result with route_to and route_back paths
     * @param user_data User-provided context pointer
     */
    typedef void (*flood_trace_callback_t)(const flood_trace_result_t* result, void* user_data);

    /**
     * @brief Register callback for traceroute results
     *
     * @param callback Callback function pointer (NULL to unregister)
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     */
    esp_err_t flood_register_trace_callback(flood_trace_callback_t callback, void* user_data);

    /**
     * @brief Send a traceroute request to a specific device
     *
     * Sends a trace request that collects path and signal info at each hop.
     * The destination sends a reply back, also collecting return path info.
     *
     * @param dest_mac Destination MAC address (6 bytes)
     * @param out_trace_id Pointer to receive the trace ID for tracking (can be NULL)
     * @return ESP_OK on success
     */
    esp_err_t flood_send_trace_request(const uint8_t* dest_mac, uint32_t* out_trace_id);

    /* ========================================================================
     * Traceroute File Storage (per-device .trc files, circular buffer)
     * ======================================================================== */

    /**
     * @brief Append a trace record to the device's .trc file (circular, max 50)
     *
     * @param mac Device MAC address (6 bytes)
     * @param record Record to save
     * @return ESP_OK on success
     */
    esp_err_t flood_trace_save(const uint8_t* mac, const flood_trace_record_t* record);

    /**
     * @brief Update an existing trace record by trace_id (in-place seek + write)
     *
     * @param mac Device MAC address (6 bytes)
     * @param trace_id Trace ID to find and update
     * @param record New record data
     * @return ESP_OK if found and updated, ESP_ERR_NOT_FOUND if not found
     */
    esp_err_t flood_trace_update(const uint8_t* mac, uint32_t trace_id, const flood_trace_record_t* record);

    /**
     * @brief Get the number of trace records stored for a device
     *
     * @param mac Device MAC address (6 bytes)
     * @param count Output: number of records
     * @return ESP_OK on success
     */
    esp_err_t flood_trace_get_count(const uint8_t* mac, uint32_t* count);

    /**
     * @brief Load all trace records in chronological order
     *
     * Resolves the circular buffer order so records[0] is the oldest.
     *
     * @param mac Device MAC address (6 bytes)
     * @param records Pre-allocated buffer (at least FLOOD_TRACE_MAX_RECORDS)
     * @param loaded Output: actual records loaded
     * @return ESP_OK on success
     */
    esp_err_t flood_trace_load_all(const uint8_t* mac, flood_trace_record_t* records, uint32_t* loaded);

    /* ========================================================================
     * Device Enumeration and Information Functions
     * ======================================================================== */

    /**
     * @brief Enumerate all known devices
     *
     * Iterates through all devices (both persistent and volatile) and calls
     * the callback function for each one.
     *
     * @param callback Function to call for each device
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if callback is NULL
     *
     * @note Enumerates persistent devices from storage first, then volatile devices
     * @note Callback can return false to stop enumeration early
     */
    esp_err_t flood_enum_devices(flood_device_enum_callback_t callback, void* user_data);

    /**
     * @brief Get a color for device visualization (by MAC address)
     *
     * Returns a consistent color value derived from device MAC address.
     * Useful for UI color coding.
     *
     * @param mac Device MAC address (6 bytes)
     * @return RGB565 color value
     *
     * @note Same MAC always returns same color
     * @note Returns 0xFFFF (white) if mac is NULL
     */
    int flood_get_device_color(const uint8_t* mac);

    /**
     * @brief Get a text color for device visualization (by MAC address)
     *
     * Returns a contrasting text color for readability against device color.
     *
     * @param mac Device MAC address (6 bytes)
     * @return RGB565 text color (black or white)
     *
     * @note Automatically chooses black/white for best contrast
     */
    int flood_get_device_text_color(const uint8_t* mac);

    /**
     * @brief Get a color for device visualization (by device ID)
     *
     * Returns a consistent color value derived from device ID.
     *
     * @param device_id Device identifier (lower 2 bytes of MAC)
     * @return RGB565 color value
     */
    int flood_get_device_color_by_id(uint16_t device_id);

    /**
     * @brief Get a text color for device visualization (by device ID)
     *
     * Returns a contrasting text color for readability.
     *
     * @param device_id Device identifier (lower 2 bytes of MAC)
     * @return RGB565 text color (black or white)
     */
    int flood_get_device_text_color_by_id(uint16_t device_id);

    /**
     * @brief Get a simple device ID from MAC address
     *
     * Extracts a 16-bit identifier from the last 2 bytes of MAC address.
     * Useful for short device references.
     *
     * @param mac Device MAC address (6 bytes)
     * @return 16-bit device ID, or 0xFFFF if mac is NULL
     *
     * @note ID = (mac[4] << 8) | mac[5]
     */
    uint16_t flood_get_device_id(const uint8_t* mac);

    /**
     * @brief Get this device's MAC address
     *
     * Retrieves the WiFi station MAC address of this device.
     *
     * @param mac Buffer to receive MAC address (must be 6 bytes)
     *
     * @note Uses ESP_MAC_WIFI_STA MAC address type
     */
    void flood_get_our_mac(uint8_t* mac);

    /**
     * @brief Get this device's ID
     *
     * Retrieves the device ID of this device.
     *
     * @return Device ID
     */
    uint16_t flood_get_our_device_id(void);

    /**
     * @brief Get current count of volatile devices in memory
     *
     * Returns the number of devices currently in the volatile device list
     * (devices seen since last boot).
     *
     * @return Number of volatile devices
     */
    uint32_t flood_get_volatile_device_count(void);

    /* ========================================================================
     * Private Message Storage API
     * ======================================================================== */

    /**
     * @brief Save a private message to filesystem
     *
     * Appends a fixed-size message record to <device_path>/messages.bin.
     * Each message is stored as a 256-byte record for efficient random access.
     *
     * @param mac Device MAC address (6 bytes)
     * @param sender_mac Sender MAC address (6 bytes)
     * @param sequence Message sequence number
     * @param message_type Message content type (text, binary, etc.)
     * @param message_data Message payload data
     * @param message_len Length of message data (max MESSAGE_MAX_PAYLOAD = 200 bytes)
     * @return Message ID (index in messages.bin, 0-based) on success, or -1 on error
     *
     * @note Message ID can be used with flood_update_message_status() for ACK tracking
     * @note Creates messages.bin if it doesn't exist
     * @note Thread-safe with mutex protection
     */
    int32_t flood_save_private_message(const uint8_t* mac,
                                       const uint8_t* sender_mac,
                                       uint32_t sequence,
                                       uint8_t message_type,
                                       const uint8_t* message_data,
                                       uint16_t message_len);

    /**
     * @brief Get total count of messages for a device
     *
     * Returns the number of message records stored in messages.bin.
     *
     * @param mac Device MAC address (6 bytes)
     * @param count Pointer to receive message count
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac or count is NULL
     *         ESP_ERR_NOT_FOUND if device directory doesn't exist
     *
     * @note Returns count=0 if messages.bin doesn't exist or is empty
     * @note Count = file_size / sizeof(message_record_t)
     */
    esp_err_t flood_get_message_count(const uint8_t* mac, uint32_t* count);

    /**
     * @brief Load a page of message records (pagination support)
     *
     * Loads multiple message records starting from a specific index.
     * Uses O(1) fseek for instant access at any position.
     *
     * @param mac Device MAC address (6 bytes)
     * @param start Starting index (0-based)
     * @param count Maximum number of records to load
     * @param records Pre-allocated buffer to receive records (must be at least count * sizeof(message_record_t) bytes)
     * @param loaded Pointer to receive actual number of records loaded
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if any pointer is NULL
     *         ESP_ERR_NOT_FOUND if messages.bin doesn't exist
     *
     * @note Does NOT allocate memory - caller must provide buffer
     * @note Returns loaded=0 if start index is beyond available messages
     * @note Efficiently loads only requested range (no full file read)
     * @note Thread-safe with mutex protection
     */
    esp_err_t
    flood_load_messages(const uint8_t* mac, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded);

    /**
     * @brief Update message status by message ID
     *
     * Modifies the status field of a specific message record.
     * Uses O(1) fseek to directly update record at position.
     *
     * @param mac Device MAC address (6 bytes)
     * @param message_id Message ID (index in messages.bin, 0-based)
     * @param status New status flags (MESSAGE_STATUS_SENT | MESSAGE_STATUS_DELIVERED | MESSAGE_STATUS_READ)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac is NULL
     *         ESP_ERR_NOT_FOUND if message_id doesn't exist or messages.bin not found
     *
     * @note Efficiently updates single byte without reading entire file
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_update_message_status(const uint8_t* mac, uint32_t message_id, uint8_t status);

    /**
     * @brief Clear all messages for a device (delete chat history)
     *
     * Deletes the messages.bin file, removing all message records.
     *
     * @param mac Device MAC address (6 bytes)
     * @return ESP_OK on success (or if messages.bin doesn't exist)
     *         ESP_ERR_INVALID_ARG if mac is NULL
     *
     * @note Does not delete device metadata (meta.bin remains)
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_clear_chat(const uint8_t* mac);

    /**
     * @brief Mark all messages from a device as read
     *
     * Iterates through all message records and sets MESSAGE_STATUS_READ flag,
     * then resets the unread message counter in volatile device data.
     *
     * @param mac Device MAC address (6 bytes)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if mac is NULL
     *         ESP_ERR_NOT_FOUND if device not found
     *
     * @note Updates both message records and volatile device state
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_private_mark_read(const uint8_t* mac);

    /* ========================================================================
     * Channel Management API
     * ======================================================================== */

    /**
     * @brief Add/create a channel
     *
     * Creates a new channel directory for storing group chat messages.
     * Channels are identified by a string ID and stored under <context_path>/channels/<channel_id>/.
     *
     * @param channel_id Channel identifier string (e.g., "general", "ch_00", etc.)
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_id is NULL or empty
     *         ESP_ERR_INVALID_STATE if not initialized
     *         ESP_FAIL on directory creation failure
     *
     * @note Creates channel directory if it doesn't exist
     * @note Thread-safe with mutex protection
     * @note No security or encryption is implemented yet
     */
    esp_err_t flood_add_channel(const char* channel_id);

    /**
     * @brief Remove a channel
     *
     * Deletes a channel and all its messages from persistent storage.
     *
     * @param channel_id Channel identifier string
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_id is NULL or empty
     *         ESP_ERR_INVALID_STATE if not initialized
     *         ESP_FAIL on deletion failure
     *
     * @note Permanently deletes all messages in the channel
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_remove_channel(const char* channel_id);

    /**
     * @brief Enumerate all channels
     *
     * Iterates through all channel directories and calls the callback function
     * for each one.
     *
     * @param callback Function to call for each channel
     * @param user_data Context pointer passed to callback
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if callback is NULL
     *         ESP_ERR_INVALID_STATE if not initialized
     *
     * @note Callback can return false to stop enumeration early
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_enum_channels(flood_channel_enum_callback_t callback, void* user_data);

    /**
     * @brief Get total count of messages in a channel
     *
     * Returns the number of message records stored in channel's messages.bin.
     *
     * @param channel_id Channel identifier string
     * @param count Pointer to receive message count
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_id or count is NULL
     *         ESP_ERR_INVALID_STATE if not initialized
     *
     * @note Returns count=0 if messages.bin doesn't exist or is empty
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_get_channel_message_count(const char* channel_id, uint32_t* count);

    /**
     * @brief Load a page of channel message records (pagination support)
     *
     * Loads multiple message records from a channel starting from a specific index.
     * Uses O(1) fseek for instant access at any position.
     *
     * @param channel_id Channel identifier string
     * @param start Starting index (0-based)
     * @param count Maximum number of records to load
     * @param records Pre-allocated buffer to receive records (must be at least count * sizeof(message_record_t) bytes)
     * @param loaded Pointer to receive actual number of records loaded
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if any pointer is NULL or count is 0
     *         ESP_ERR_INVALID_STATE if not initialized
     *
     * @note Does NOT allocate memory - caller must provide buffer
     * @note Returns loaded=0 if start index is beyond available messages
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_load_channel_messages(
        const char* channel_id, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded);

    /**
     * @brief Clear all messages in a channel
     *
     * Deletes the messages.bin file for a channel, removing all message records.
     *
     * @param channel_id Channel identifier string
     * @return ESP_OK on success (or if messages.bin doesn't exist)
     *         ESP_ERR_INVALID_ARG if channel_id is NULL or empty
     *         ESP_ERR_INVALID_STATE if not initialized
     *
     * @note Does not delete the channel directory itself
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_clear_channel(const char* channel_name);

    /**
     * @brief Mark all messages in a channel as read
     *
     * Resets the unread message counter for the specified channel to 0.
     * Only affects volatile (runtime) data - does not modify message records.
     *
     * @param channel_name Channel name string
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_name is NULL
     *         ESP_ERR_TIMEOUT if mutex acquisition fails
     *         ESP_ERR_NOT_FOUND if channel not found in volatile list
     *
     * @note Only updates volatile channel state, not message records
     * @note Thread-safe with mutex protection
     */
    esp_err_t flood_channel_mark_read(const char* channel_name);

    /**
     * @brief Save a message to a channel
     *
     * Appends a message record to the specified channel's messages.bin file.
     * Creates the channel directory if it doesn't exist.
     *
     * @param channel_name Channel name string
     * @param sender_mac Sender MAC address (6 bytes)
     * @param sequence Message sequence number
     * @param status Message status (SENT, RECEIVED, etc.)
     * @param message_type Message type identifier
     * @param message_data Message payload data
     * @param message_length Length of message data (max MESSAGE_MAX_PAYLOAD)
     * @return Message ID (>= 0) on success, -1 on failure
     *
     * @note Creates channel directory and messages.bin if they don't exist
     * @note Thread-safe with mutex protection
     */
    int32_t flood_save_channel_message(const char* channel_name,
                                       const uint8_t* sender_mac,
                                       uint32_t sequence,
                                       uint8_t status,
                                       uint8_t message_type,
                                       const uint8_t* message_data,
                                       uint16_t message_length);

    // flood_find_channel/
    /**
     * @brief
     *
     * @param channel_name Channel name string
     * @return ESP_OK on success
     *         ESP_ERR_INVALID_ARG if channel_name is NULL
     *         ESP_ERR_INVALID_STATE if not initialized
     *         ESP_FAIL on file I/O error
     */
    esp_err_t flood_find_channel(const char* channel_name, mesh_channel_info_t* channel_info);
#ifdef __cplusplus
}
#endif

#endif // FLOOD_H
