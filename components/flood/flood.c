#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "flood.h"

#define PATH_BUF_SIZE (MAXNAMLEN + 1)
#define MACSTRSHORT "%02X%02X%02X%02X%02X%02X"

#define FLOOD_ACK_TIMEOUT 5000   // 5 seconds timeout for ACK
#define FLOOD_RESEND_MAX_TRIES 3 // Maximum number of retries

static const char* TAG = "flood";

static const char* DEVICE_META_FILE = "meta.bin";
static const char* DEVICES_DIRECTORY = "devices";
static const char* MESSAGES_FILE = "messages.bin";
static const char* TRACES_FILE = "traces.trc";
static const char* CHANNELS_DIRECTORY = "channels";
static const char* CHANNEL_META_FILE = "meta.bin";

/* Global state */
static bool s_flood_initialized = false;
static bool s_flood_running = false;
static TaskHandle_t s_flood_task_handle = NULL;
static QueueHandle_t s_message_queue = NULL;
static SemaphoreHandle_t s_flood_mutex = NULL;
static char s_context_path[PATH_BUF_SIZE] = {0};
static int s_channel = 0;
static int s_max_ttl = 5;
static int s_hello_interval = 60;
static uint8_t s_our_mac[6] = {0};

/* Device role and capabilities */
static char s_device_name[32] = CONFIG_FLOOD_DEVICE_NAME;
static FLOOD_DEVICE_ROLE_t s_device_role = MESH_ROLE_ROUTER;
static uint8_t s_device_capabilities = MESH_CAP_POWER_SAVE;

/* Device and message management */
static mesh_packet_cache_t s_packet_cache = {0};

/* Volatile device data (in-memory only) - dynamic linked list */
typedef struct volatile_device_node
{
    mesh_device_volatile_t data;
    struct volatile_device_node* next;
} volatile_device_node_t;

static volatile_device_node_t* s_volatile_devices_head = NULL;
static uint32_t s_volatile_device_count = 0;

/* Volatile channel data (in-memory only) - dynamic linked list */
typedef struct volatile_channel_node
{
    mesh_channel_volatile_t data;
    struct volatile_channel_node* next;
} volatile_channel_node_t;

static volatile_channel_node_t* s_volatile_channels_head = NULL;
static uint32_t s_volatile_channel_count = 0;

/* Waiting ACK list - dynamic linked list */
typedef struct waiting_ack_node
{
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    uint16_t packet_length;
    uint32_t timestamp;
    uint8_t try_num;
    struct waiting_ack_node* next;
} waiting_ack_node_t;

static waiting_ack_node_t* s_waiting_ack_head = NULL;
static uint32_t s_waiting_ack_count = 0;

/* Callbacks */
static flood_message_callback_t s_message_callback = NULL;
static void* s_message_callback_user_data = NULL;
static flood_message_status_callback_t s_message_status_callback = NULL;
static void* s_message_status_callback_user_data = NULL;
static flood_device_callback_t s_device_callback = NULL;
static void* s_device_callback_user_data = NULL;
static flood_packet_callback_t s_sent_packet_callback = NULL;
static void* s_sent_packet_callback_user_data = NULL;
static flood_packet_callback_t s_received_packet_callback = NULL;
static void* s_received_packet_callback_user_data = NULL;
static flood_trace_callback_t s_trace_callback = NULL;
static void* s_trace_callback_user_data = NULL;
/* Sequence numbers */
static uint32_t s_sequence_number = 0;

/* Device battery level */
static uint8_t s_device_battery_level = 100;

/* Broadcast MAC address */
static const uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* Forward declarations */
static void flood_task(void* pvParameter);
static esp_err_t flood_espnow_init(void);
static void flood_espnow_send_cb(const esp_now_send_info_t* tx_info, esp_now_send_status_t status);
static void flood_espnow_recv_cb(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len);
static esp_err_t flood_process_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi);
static esp_err_t flood_forward_packet(const uint8_t* data, uint16_t length);
static esp_err_t flood_process_trace_request(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi);
static esp_err_t flood_process_trace_reply(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi);
static bool flood_is_broadcast_mac(const uint8_t* mac);
static bool flood_is_our_mac(const uint8_t* mac);
static uint32_t flood_get_timestamp(void);
static esp_err_t flood_add_device_internal(const mesh_device_info_t* device);
static esp_err_t flood_find_device_internal(const uint8_t* mac, mesh_device_info_t* device);
static esp_err_t flood_find_device(const uint8_t* mac, mesh_device_info_t* device);
static esp_err_t flood_enqueue_packet(const uint8_t* data, uint16_t length);
static esp_err_t get_devices_path(char* path);
static esp_err_t get_device_path(const uint8_t* mac, char* path);
static esp_err_t get_device_meta_path(const uint8_t* mac, char* path);
static esp_err_t flood_save_device_persistent_internal(const mesh_device_persistent_t* device);
static esp_err_t create_device_directory(const uint8_t* mac);
static esp_err_t flood_load_device_persistent_from_meta_internal(const char* meta_path, mesh_device_persistent_t* device);
static esp_err_t flood_load_device_persistent_internal(const uint8_t* mac, mesh_device_persistent_t* device);
static esp_err_t flood_update_device_volatile_internal(const uint8_t* mac, const mesh_device_volatile_t* volatile_data);
static esp_err_t flood_get_device_volatile_internal(const uint8_t* mac, mesh_device_volatile_t* volatile_data);
static esp_err_t flood_remove_device_volatile_internal(const uint8_t* mac);
static void flood_cleanup_volatile_devices(void);
static esp_err_t get_messages_file_path(const uint8_t* mac, char* path);
static esp_err_t get_traces_file_path(const uint8_t* mac, char* path);
static esp_err_t
flood_load_messages_internal(const uint8_t* mac, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded);
static int32_t flood_save_message_internal(const uint8_t* mac,
                                           const uint8_t* sender_mac,
                                           uint32_t sequence,
                                           uint8_t message_type,
                                           const uint8_t* message_data,
                                           uint16_t message_len);
static esp_err_t flood_update_private_message_status_internal(const uint8_t* mac, uint32_t message_id, uint8_t status);
static esp_err_t flood_waiting_ack_add(const uint8_t* packet, uint16_t packet_length);
static esp_err_t flood_waiting_ack_remove(uint32_t sequence, const uint8_t* dest_mac, int32_t* out_message_id);
static esp_err_t flood_waiting_ack_implicit(uint32_t sequence);
static void flood_waiting_ack_check_timeouts(void);
static void flood_waiting_ack_cleanup(void);
static esp_err_t get_channels_path(char* path);
static esp_err_t get_channel_path(const char* channel_name, char* path);
static esp_err_t get_channel_meta_path(const char* channel_name, char* path);
static esp_err_t get_channel_messages_file_path(const char* channel_name, char* path);
static int32_t flood_save_channel_message_internal(const char* channel_name,
                                                   const uint8_t* sender_mac,
                                                   uint32_t sequence,
                                                   uint8_t status,
                                                   uint8_t message_type,
                                                   const uint8_t* message_data,
                                                   uint16_t message_length);
static esp_err_t flood_update_channel_message_status_internal(const char* channel_name, uint32_t message_id, uint8_t status);
static esp_err_t flood_load_channel_persistent_internal(const char* channel_name, mesh_channel_persistent_t* persistent);
static esp_err_t flood_save_channel_persistent_internal(const mesh_channel_persistent_t* persistent);
static esp_err_t flood_find_channel_internal(const char* channel_name, mesh_channel_info_t* channel_info);
static bool flood_channel_name_valid(const char* channel_name);
static esp_err_t flood_update_channel_volatile_internal(const char* channel_name, const mesh_channel_volatile_t* volatile_data);
static esp_err_t flood_get_channel_volatile_internal(const char* channel_name, mesh_channel_volatile_t* volatile_data);
static esp_err_t flood_remove_channel_volatile_internal(const char* channel_name);
static void flood_cleanup_volatile_channels(void);
/* Shared message file operations */
static esp_err_t flood_load_messages_from_file_internal(
    const char* messages_file_path, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded);
static esp_err_t flood_clear_messages_file_internal(const char* messages_file_path);
static esp_err_t flood_save_message_to_file_internal(const char* messages_file_path, const message_record_t* record);

/* Helper functions for file-based storage */
static void mac_to_string(const uint8_t* mac, char* str, size_t len)
{
    snprintf(str, len, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t string_to_mac(const char* str, uint8_t* mac)
{
    if (str == NULL || mac == NULL || strlen(str) != 12)
    {
        return ESP_ERR_INVALID_ARG;
    }

    for (int i = 0; i < 6; i++)
    {
        char hex_str[3] = {str[i * 2], str[i * 2 + 1], '\0'};
        char* endptr;
        unsigned long val = strtoul(hex_str, &endptr, 16);

        if (endptr != hex_str + 2 || val > 255)
        {
            return ESP_ERR_INVALID_ARG;
        }

        mac[i] = (uint8_t)val;
    }

    return ESP_OK;
}

/* Path utilities */

static bool path_join(char* path1, const char* path2)
{
    if (path1 == NULL || path2 == NULL)
    {
        return false;
    }

    size_t limit = 0;
#ifdef MAXNAMLEN
    limit = (size_t)MAXNAMLEN;
#else
    limit = 255;
#endif
    size_t len1 = strlen(path1);
    size_t len2 = strlen(path2);
    bool needs_sep = (len1 > 0 && path1[len1 - 1] != '/' && len2 > 0 && path2[0] != '/');
    size_t extra = len2 + (needs_sep ? 1 : 0);

    if (len1 + extra > limit)
    {
        ESP_LOGE(TAG, "Path too long: %s + %s", path1, path2);
        return false;
    }

    if (needs_sep)
    {
        path1[len1++] = '/';
        path1[len1] = '\0';
    }

    if (len2 > 0)
    {
        memcpy(path1 + len1, path2, len2);
        path1[len1 + len2] = '\0';
    }

    return true;
}

/* WiFi should start before using ESPNOW */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    //
    wifi_country_t country;
    ESP_ERROR_CHECK(esp_wifi_get_country(&country));
    ESP_LOGD(TAG, "WiFi country code: %s, max tx power: %d", country.cc, country.max_tx_power);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(s_channel, WIFI_SECOND_CHAN_NONE));
    // ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N |
    // WIFI_PROTOCOL_LR));
    ESP_ERROR_CHECK(esp_wifi_set_protocol(ESP_IF_WIFI_STA, WIFI_PROTOCOL_LR));
}

static void wifi_deinit(void)
{
    ESP_LOGI(TAG, "Deinitializing WiFi...");
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_event_loop_delete_default();
    esp_netif_deinit();
}

static char* flood_packet_type_to_string(uint8_t type)
{
    switch (type)
    {
    case MESH_PACKET_TYPE_HELLO:
        return "HELLO";
    case MESH_PACKET_TYPE_MESSAGE:
        return "MESSAGE";
    case MESH_PACKET_TYPE_PRIVATE:
        return "PRIVATE";
    case MESH_PACKET_TYPE_ACK:
        return "ACK";
    case MESH_PACKET_TYPE_TRACE_REQ:
        return "TRACE_REQ";
    case MESH_PACKET_TYPE_TRACE_REPLY:
        return "TRACE_RPL";
    default:
        return "UNKNOWN";
    }
}

esp_err_t flood_init(const char* name, const char* context_path, int channel, int max_ttl, int hello_interval)
{
    if (name == NULL || strlen(name) == 0)
    {
        ESP_LOGE(TAG, "Device name cannot be NULL or empty");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_device_name, name, sizeof(s_device_name) - 1);

    if (s_flood_initialized)
    {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    if (context_path == NULL || strlen(context_path) == 0)
    {
        ESP_LOGE(TAG, "Context path cannot be NULL or empty");
        return ESP_ERR_INVALID_ARG;
    }
    if (channel < 0 || channel > 14)
    {
        ESP_LOGE(TAG, "Invalid channel: %d", channel);
        return ESP_ERR_INVALID_ARG;
    }
    s_channel = channel;
    if (max_ttl < 1 || max_ttl > 9)
    {
        ESP_LOGE(TAG, "Invalid max TTL: %d", max_ttl);
        return ESP_ERR_INVALID_ARG;
    }
    s_max_ttl = max_ttl;
    if (hello_interval < 10 || hello_interval > 3600)
    {
        ESP_LOGE(TAG, "Invalid hello interval: %d", hello_interval);
        return ESP_ERR_INVALID_ARG;
    }
    s_hello_interval = hello_interval;
    // Store context path for persistent storage operations
    strncpy(s_context_path, context_path, PATH_BUF_SIZE - 1);

    ESP_LOGI(TAG, "Initializing with context path: %s", s_context_path);

    // create context path if not exists
    if (access(s_context_path, F_OK) != 0)
    {
        ESP_LOGW(TAG, "Creating context path: %s", s_context_path);
        if (mkdir(s_context_path, 0777) != 0)
        {
            ESP_LOGE(TAG, "Failed to create context path: %s", s_context_path);
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGD(TAG, "Context path already exists: %s", s_context_path);
    }
    // creating devices directory if not exists
    char path_buf[PATH_BUF_SIZE];
    if (get_devices_path(path_buf) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get devices path: %s", path_buf);
        return ESP_FAIL;
    }
    if (access(path_buf, F_OK) != 0)
    {
        ESP_LOGW(TAG, "Creating devices directory: %s", path_buf);
        if (mkdir(path_buf, 0777) != 0)
        {
            ESP_LOGE(TAG, "Failed to create devices directory: %s", path_buf);
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGD(TAG, "Devices directory already exists: %s", path_buf);
    }
    // creating channels directory if not exists
    if (get_channels_path(path_buf) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to get channels path: %s", path_buf);
        return ESP_FAIL;
    }
    if (access(path_buf, F_OK) != 0)
    {
        ESP_LOGW(TAG, "Creating channels directory: %s", path_buf);
        if (mkdir(path_buf, 0777) != 0)
        {
            ESP_LOGE(TAG, "Failed to create channels directory: %s", path_buf);
            return ESP_FAIL;
        }
    }
    else
    {
        ESP_LOGD(TAG, "Channels directory already exists: %s", path_buf);
    }
    //
    wifi_init();
    // save our mac
    esp_read_mac(s_our_mac, ESP_MAC_WIFI_STA);
    // Initialize mutex
    s_flood_mutex = xSemaphoreCreateMutex();
    if (s_flood_mutex == NULL)
    {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_FAIL;
    }

    // Initialize message queue
    s_message_queue = xQueueCreate(MESH_MAX_QUEUE_SIZE, sizeof(mesh_queued_packet_t));
    if (s_message_queue == NULL)
    {
        ESP_LOGE(TAG, "Failed to create message queue");
        vSemaphoreDelete(s_flood_mutex);
        return ESP_FAIL;
    }

    // Initialize message cache
    memset(&s_packet_cache, 0, sizeof(s_packet_cache));
    s_packet_cache.last_cleanup = flood_get_timestamp();

    // Initialize device role and capabilities from config
#ifdef CONFIG_FLOOD_MESH_ROLE_CLIENT
    s_device_role = MESH_ROLE_CLIENT;
#elif CONFIG_FLOOD_MESH_ROLE_ROUTER
    s_device_role = MESH_ROLE_ROUTER;
#elif CONFIG_FLOOD_MESH_ROLE_REPEATER
    s_device_role = MESH_ROLE_REPEATER;
#else
    s_device_role = MESH_ROLE_ROUTER; // Default to router for forwarding
#endif

    s_device_capabilities = 0x01; // Default capabilities

    // Initialize sequence number
    s_sequence_number = esp_random();

    // Initialize ESP-NOW
    esp_err_t ret = flood_espnow_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW");
        vQueueDelete(s_message_queue);
        vSemaphoreDelete(s_flood_mutex);
        return ret;
    }

    s_flood_initialized = true;
    ESP_LOGI(TAG, "Initialized successfully");
    return ESP_OK;
}

esp_err_t flood_start(void)
{
    if (!s_flood_initialized)
    {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (s_flood_running)
    {
        ESP_LOGW(TAG, "Already running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting...");

    // Create flood task
    BaseType_t ret = xTaskCreate(flood_task, "flood_task", 4096, NULL, 5, &s_flood_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Started successfully");
    return ESP_OK;
}

esp_err_t flood_stop(void)
{
    if (!s_flood_running)
    {
        ESP_LOGW(TAG, "Not running");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Stopping...");

    s_flood_running = false;

    while (s_flood_task_handle != NULL)
    {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGI(TAG, "Stopped successfully");
    return ESP_OK;
}

esp_err_t flood_deinit(void)
{
    if (!s_flood_initialized)
    {
        ESP_LOGW(TAG, "Not initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Deinitializing...");

    // Stop if running
    flood_stop();

    // Deinitialize ESP-NOW
    esp_now_deinit();

    // Deinitialize WiFi
    wifi_deinit();

    // Clean up volatile devices list
    flood_cleanup_volatile_devices();
    // Clean up volatile channels list
    flood_cleanup_volatile_channels();
    // Clean up waiting ACK list

    flood_waiting_ack_cleanup();

    // Clean up resources
    if (s_message_queue != NULL)
    {
        vQueueDelete(s_message_queue);
        s_message_queue = NULL;
    }

    if (s_flood_mutex != NULL)
    {
        vSemaphoreDelete(s_flood_mutex);
        s_flood_mutex = NULL;
    }

    s_flood_initialized = false;
    ESP_LOGI(TAG, "Deinitialized successfully");
    return ESP_OK;
}

static esp_err_t flood_espnow_init(void)
{
    esp_err_t ret = esp_now_init();
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to initialize ESP-NOW: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_now_register_send_cb(flood_espnow_send_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register send callback: %s", esp_err_to_name(ret));
        esp_now_deinit();
        return ret;
    }

    ret = esp_now_register_recv_cb(flood_espnow_recv_cb);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to register receive callback: %s", esp_err_to_name(ret));
        esp_now_deinit();
        return ret;
    }

    // Add broadcast peer
    esp_now_peer_info_t peer = {0};
    peer.channel = CONFIG_FLOOD_CHANNEL;
    peer.ifidx = ESP_IF_WIFI_STA;
    peer.encrypt = false;
    memcpy(peer.peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);

    ret = esp_now_add_peer(&peer);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to add broadcast peer: %s", esp_err_to_name(ret));
        esp_now_deinit();
        return ret;
    }

    return ESP_OK;
}

static esp_err_t flood_enqueue_packet(const uint8_t* data, uint16_t length)
{
    if (data == NULL || length == 0 || length > ESP_NOW_MAX_DATA_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_message_queue == NULL)
    {
        return ESP_ERR_INVALID_STATE;
    }

    mesh_queued_packet_t item = {0};
    memcpy(item.data, data, length);
    item.length = length;

    BaseType_t ok = xQueueSend(s_message_queue, &item, 0);
    return ok == pdTRUE ? ESP_OK : ESP_ERR_NO_MEM;
}

static void flood_espnow_send_cb(const esp_now_send_info_t* tx_info, esp_now_send_status_t status)
{
    if (status == ESP_NOW_SEND_SUCCESS)
    {
        ESP_LOGD(TAG, "Message sent successfully to " MACSTR, MAC2STR(tx_info->des_addr));
    }
    else
    {
        ESP_LOGW(TAG, "Message send failed to " MACSTR, MAC2STR(tx_info->des_addr));
    }
}

static void flood_espnow_recv_cb(const esp_now_recv_info_t* recv_info, const uint8_t* data, int len)
{
    if (data == NULL || len <= 0)
    {
        ESP_LOGE(TAG, "Invalid receive data");
        return;
    }

    // Get RSSI from receive info
    int8_t rssi = recv_info->rx_ctrl->rssi;

    ESP_LOGD(TAG, "Received %d bytes from " MACSTR " (RSSI: %d dBm)", len, MAC2STR(recv_info->src_addr), rssi);

    // Process packet in flood task
    flood_process_packet(data, len, recv_info->src_addr, rssi);
}

static esp_err_t flood_process_hello_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    const mesh_packet_header_t* header = (const mesh_packet_header_t*)data;
    if (header->type != MESH_PACKET_TYPE_HELLO)
    {
        ESP_LOGE(TAG, "Invalid hello packet, type: %d", header->type);
        return ESP_ERR_INVALID_STATE;
    }
    // Update device info - separate persistent and volatile data
    mesh_device_persistent_t persistent_info = {0};
    mesh_device_volatile_t volatile_info = {0};
    // find device by mac
    mesh_device_info_t device = {0};
    if (flood_find_device(header->source_mac, &device) != ESP_OK)
    {
        // Common fields
        persistent_info.magic = MESH_MAGIC_NUMBER;
        persistent_info.version = MESH_PERSISTENT_VERSION;
        memcpy(persistent_info.mac, src_mac, 6);
        memcpy(volatile_info.mac, src_mac, 6);
        // reset unread messages
        volatile_info.unread_messages = 0;
    }
    else
    {
        persistent_info = device.persistent;
        volatile_info = device.volatile_data;
    }

    // Volatile data (changes frequently)
    volatile_info.last_seen = flood_get_timestamp();
    volatile_info.signal_strength = flood_rssi_to_percentage(rssi);
    volatile_info.hops = header->hops;

    // Extract role and capabilities from hello packet if available
    if (length >= sizeof(mesh_hello_packet_t))
    {
        const mesh_hello_packet_t* hello_packet = (const mesh_hello_packet_t*)data;
        strncpy((char*)persistent_info.name, (char*)hello_packet->device_name, sizeof(persistent_info.name) - 1);
        persistent_info.role = hello_packet->role;
        persistent_info.capabilities = hello_packet->capabilities;
        volatile_info.battery_level = hello_packet->battery_level;
        // Set last_seen if this is a new device
    }
    // print name and mac
    ESP_LOGI(TAG,
             "HELLO from %s (mac:" MACSTR ", role: %d, capabilities: %d, battery level: %d)",
             persistent_info.name,
             MAC2STR(header->source_mac),
             persistent_info.role,
             persistent_info.capabilities,
             volatile_info.battery_level);
    // Update device data
    flood_save_device_persistent(&persistent_info);
    flood_update_device_volatile(header->source_mac, &volatile_info);
    // Call message callback if registered
    if (s_message_callback != NULL)
    {
        s_message_callback(header, data, length, rssi, s_message_callback_user_data);
    }
    // forward hello packet always (implicit ACK via overheard retransmission)
    flood_forward_packet(data, length);
    return ESP_OK;
}

static esp_err_t flood_process_message_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    const mesh_packet_header_t* header = (const mesh_packet_header_t*)data;
    if (header->type != MESH_PACKET_TYPE_MESSAGE)
    {
        ESP_LOGE(TAG, "Invalid message packet, type: %d", header->type);
        return ESP_ERR_INVALID_STATE;
    }
    if (length >= sizeof(mesh_message_packet_t))
    {
        const mesh_message_packet_t* message_packet = (const mesh_message_packet_t*)data;

        // Find device by mac
        mesh_device_info_t device = {0};
        esp_err_t find_ret = flood_find_device(src_mac, &device);
        const char* sender_name = (find_ret != ESP_OK) ? "[?]" : (const char*)device.persistent.name;

        // print message
        ESP_LOGI(TAG,
                 "MESSAGE *%08x from %s (mac: " MACSTR ") to channel \"%s\": [%d] \"%.*s\" (RSSI: %d dBm)",
                 header->sequence,
                 sender_name,
                 MAC2STR(header->source_mac),
                 (const char*)message_packet->channel_name,
                 message_packet->message_length,
                 message_packet->message_length,
                 (char*)message_packet->message_text,
                 rssi);
        // Find channel
        mesh_channel_info_t channel_info = {0};
        esp_err_t channel_ret = flood_find_channel((const char*)message_packet->channel_name, &channel_info);
        if (channel_ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to find channel: %s", message_packet->channel_name);
        }
        else
        {
            // get channel volatile data
            mesh_channel_volatile_t channel_volatile = {0};
            esp_err_t volatile_ret = flood_get_channel_volatile((const char*)message_packet->channel_name, &channel_volatile);
            if (volatile_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to get channel volatile data: 0x%x", volatile_ret);
            }
            else
            {
                // update channel volatile data
                channel_volatile.last_seen = flood_get_timestamp();
                channel_volatile.unread_messages++;
            }
            // update channel volatile data
            esp_err_t ret = flood_update_channel_volatile((const char*)message_packet->channel_name, &channel_volatile);
            if (ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to update channel volatile data: %s", esp_err_to_name(ret));
            }
            // Save message to channel using internal function (no mutex - already held by caller)
            int32_t message_id = flood_save_channel_message((char*)message_packet->channel_name,
                                                            header->source_mac,
                                                            header->sequence,
                                                            MESSAGE_STATUS_RECEIVED,
                                                            message_packet->message_type,
                                                            message_packet->message_text,
                                                            message_packet->message_length);
            if (message_id < 0)
            {
                ESP_LOGW(TAG, "Failed to save channel message");
            }
        }
        // Call message callback if registered
        if (s_message_callback != NULL)
        {
            s_message_callback(header, data, length, rssi, s_message_callback_user_data);
        }
    }
    // forward message packet always (implicit ACK via overheard retransmission)
    flood_forward_packet(data, length);
    return ESP_OK;
}

static esp_err_t flood_process_private_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    const mesh_packet_header_t* header = (const mesh_packet_header_t*)data;
    if (header->type != MESH_PACKET_TYPE_PRIVATE)
    {
        ESP_LOGE(TAG, "Invalid private packet, type: %d", header->type);
        return ESP_ERR_INVALID_STATE;
    }

    if (!flood_is_our_mac(header->dest_mac))
    {
        // Relay node: forward only, no explicit ACK (implicit ACK via retransmission)
        flood_forward_packet(data, length);
    }
    else
    {
        // Destination node: send explicit ACK back to sender
        if (header->flags & MESH_FLAG_ACK_REQUIRED)
        {
            flood_send_ack(header->source_mac, header->sequence, MESH_ACK_STATUS_SUCCESS);
        }
        if (length >= sizeof(mesh_private_packet_t))
        {
            // find device by mac
            mesh_device_info_t device = {0};
            esp_err_t find_ret = flood_find_device(header->source_mac, &device);
            const char* name = (find_ret != ESP_OK) ? "[?]" : (const char*)device.persistent.name;
            const mesh_private_packet_t* private_packet = (const mesh_private_packet_t*)data;
            // print message
            ESP_LOGI(TAG,
                     "PRIVATE *%08x from %s (mac: " MACSTR "): [%d] \"%.*s\" (RSSI: %d dBm)",
                     header->sequence,
                     name,
                     MAC2STR(header->source_mac),
                     private_packet->message_length,
                     private_packet->message_length,
                     (char*)private_packet->message_text,
                     rssi);
            if (find_ret == ESP_OK)
            {
                device.volatile_data.unread_messages++;
                // Update volatile data
                esp_err_t ret = flood_update_device_volatile(device.persistent.mac, &device.volatile_data);
                if (ret != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to update volatile data: %s", esp_err_to_name(ret));
                }
                // save message to file
                int32_t message_id = flood_save_private_message(device.persistent.mac,
                                                                device.persistent.mac,
                                                                header->sequence,
                                                                private_packet->message_type,
                                                                private_packet->message_text,
                                                                private_packet->message_length);
                if (message_id == -1)
                {
                    ESP_LOGW(TAG, "Failed to save message");
                }
            }
            // Call message callback if registered
            if (s_message_callback != NULL)
            {
                s_message_callback(header, data, length, rssi, s_message_callback_user_data);
            }
        }
    }
    return ESP_OK;
}

static esp_err_t flood_process_ack_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    const mesh_packet_header_t* header = (const mesh_packet_header_t*)data;
    if (header->type != MESH_PACKET_TYPE_ACK)
    {
        ESP_LOGE(TAG, "Invalid ack packet, type: %d", header->type);
        return ESP_ERR_INVALID_STATE;
    }
    if (!flood_is_our_mac(header->dest_mac))
    {
        ESP_LOGD(TAG, "Packet is not for us, forwarding");
        return flood_forward_packet(data, length);
    }
    else
    {
        // mark message as delivered
        mesh_ack_packet_t* ack_packet = (mesh_ack_packet_t*)data;
        uint32_t ack_sequence = ack_packet->ack_sequence;
        ESP_LOGD(TAG, "Packet is for us, processing: " MACSTR ", sequence: *%08x", MAC2STR(header->source_mac), ack_sequence);
        uint8_t status = ack_packet->status ? MESSAGE_STATUS_DELIVERED : MESSAGE_STATUS_DELIVERY_FAILED;

        // Remove from waiting ACK list and get message_id
        int32_t message_id = -1;
        esp_err_t ret = flood_waiting_ack_remove(ack_sequence, header->source_mac, &message_id);
        if (ret == ESP_OK && message_id >= 0)
        {
            // Update message status using the message_id
            return flood_update_message_status(header->source_mac, message_id, status);
        }

        return ret;
    }
}

static esp_err_t flood_process_trace_request(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    if (length < sizeof(mesh_packet_header_t) + 5)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    memcpy(packet, data, length);
    mesh_trace_request_t* trace_req = (mesh_trace_request_t*)packet;
    mesh_packet_header_t* header = &trace_req->header;

    if (trace_req->hop_count < FLOOD_TRACE_MAX_HOPS)
    {
        memcpy(trace_req->hops[trace_req->hop_count].mac, s_our_mac, 6);
        trace_req->hops[trace_req->hop_count].signal = flood_rssi_to_percentage(rssi);
        trace_req->hop_count++;
    }

    if (flood_is_our_mac(header->dest_mac))
    {
        ESP_LOGI(TAG, "TRACE_REQ *%08x arrived, sending reply (%d hops)", trace_req->trace_id, trace_req->hop_count > 0 ? trace_req->hop_count - 1 : 0);
        mesh_trace_reply_t reply = {0};
        reply.header.magic = MESH_MAGIC_NUMBER;
        reply.header.version = MESH_PROTOCOL_VERSION;
        reply.header.type = MESH_PACKET_TYPE_TRACE_REPLY;
        reply.header.flags = 0;
        reply.header.hops = 0;
        reply.header.ttl = s_max_ttl;
        reply.header.sequence = s_sequence_number++;
        memcpy(reply.header.source_mac, s_our_mac, 6);
        memcpy(reply.header.dest_mac, header->source_mac, 6);
        reply.trace_id = trace_req->trace_id;
        reply.route_to_count = trace_req->hop_count;
        reply.route_back_count = 0;
        memcpy(reply.route_to, trace_req->hops, trace_req->hop_count * sizeof(flood_trace_hop_t));
        return flood_enqueue_packet((uint8_t*)&reply, sizeof(mesh_trace_reply_t));
    }
    else
    {
        if (header->ttl > 0)
        {
            header->ttl--;
            header->hops++;
            header->flags |= MESH_FLAG_FORWARDED;
            return flood_enqueue_packet(packet, sizeof(mesh_trace_request_t));
        }
        return ESP_OK;
    }
}

static esp_err_t flood_process_trace_reply(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    if (length < sizeof(mesh_packet_header_t) + 6)
    {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    memcpy(packet, data, length);
    mesh_trace_reply_t* trace_rpl = (mesh_trace_reply_t*)packet;
    mesh_packet_header_t* header = &trace_rpl->header;

    if (trace_rpl->route_back_count < FLOOD_TRACE_MAX_HOPS)
    {
        memcpy(trace_rpl->route_back[trace_rpl->route_back_count].mac, s_our_mac, 6);
        trace_rpl->route_back[trace_rpl->route_back_count].signal = flood_rssi_to_percentage(rssi);
        trace_rpl->route_back_count++;
    }

    if (flood_is_our_mac(header->dest_mac))
    {
        ESP_LOGI(TAG,
                 "TRACE_REPLY *%08x: %d hops to, %d hops back",
                 trace_rpl->trace_id,
                 trace_rpl->route_to_count > 0 ? trace_rpl->route_to_count - 1 : 0,
                 trace_rpl->route_back_count > 0 ? trace_rpl->route_back_count - 1 : 0);
        if (s_trace_callback != NULL)
        {
            flood_trace_result_t result = {0};
            result.trace_id = trace_rpl->trace_id;
            result.route_to_count = trace_rpl->route_to_count;
            result.route_back_count = trace_rpl->route_back_count;
            memcpy(result.route_to, trace_rpl->route_to, trace_rpl->route_to_count * sizeof(flood_trace_hop_t));
            memcpy(result.route_back, trace_rpl->route_back, trace_rpl->route_back_count * sizeof(flood_trace_hop_t));
            s_trace_callback(&result, s_trace_callback_user_data);
        }
        return ESP_OK;
    }
    else
    {
        if (header->ttl > 0)
        {
            header->ttl--;
            header->hops++;
            header->flags |= MESH_FLAG_FORWARDED;
            return flood_enqueue_packet(packet, sizeof(mesh_trace_reply_t));
        }
        return ESP_OK;
    }
}

static esp_err_t flood_process_packet(const uint8_t* data, uint16_t length, const uint8_t* src_mac, int8_t rssi)
{
    if (length < sizeof(mesh_packet_header_t))
    {
        ESP_LOGW(TAG, "Packet too short: %d bytes", length);
        return ESP_ERR_INVALID_SIZE;
    }

    // check magic number
    const mesh_packet_header_t* header = (const mesh_packet_header_t*)data;
    if (header->magic != MESH_MAGIC_NUMBER)
    {
        // could be any random ESP-NOW packat
        ESP_LOGD(TAG, "Invalid magic number: %08x", header->magic);
        return ESP_ERR_INVALID_STATE;
    }
    // Check protocol version
    if (header->version != MESH_PROTOCOL_VERSION)
    {
        ESP_LOGW(TAG, "Unsupported protocol version: %d", header->version);
        return ESP_ERR_INVALID_VERSION;
    }

    // check source is not our mac
    if (flood_is_our_mac(header->source_mac))
    {
        if (header->flags & MESH_FLAG_FORWARDED)
        {
            if (flood_waiting_ack_implicit(header->sequence) == ESP_OK)
            {
                ESP_LOGI(TAG, "Implicit ACK for *%08x (overheard retransmission)", header->sequence);
            }
        }
        return ESP_OK;
    }

    // const uint8_t* payload = data + sizeof(mesh_packet_header_t);
    // uint16_t payload_length = length - sizeof(mesh_packet_header_t);

    // Check for duplicates
    if (flood_cache_check(header->sequence, header->source_mac))
    {
        ESP_LOGW(TAG, "Duplicate packet detected *%08x, dropping", header->sequence);
        return ESP_OK;
    }
    // Add to cache
    flood_cache_add(header->sequence, header->source_mac);

    ESP_LOGI(TAG,
             "[>>] %s *%08x from " MACSTR " (RSSI: %d dBm)",
             flood_packet_type_to_string(header->type),
             header->sequence,
             MAC2STR(header->source_mac),
             rssi);
    // call received packet callback
    if (s_received_packet_callback != NULL)
    {
        s_received_packet_callback(data, length, s_received_packet_callback_user_data);
    }

    esp_err_t ret = ESP_OK;
    // Process packet based on type
    switch (header->type)
    {
    case MESH_PACKET_TYPE_HELLO:
        ret = flood_process_hello_packet(data, length, src_mac, rssi);
        break;
    case MESH_PACKET_TYPE_MESSAGE:
        ret = flood_process_message_packet(data, length, src_mac, rssi);
        break;
    case MESH_PACKET_TYPE_PRIVATE:
        ret = flood_process_private_packet(data, length, src_mac, rssi);
        break;
    case MESH_PACKET_TYPE_ACK:
        ret = flood_process_ack_packet(data, length, src_mac, rssi);
        break;
    case MESH_PACKET_TYPE_TRACE_REQ:
        ret = flood_process_trace_request(data, length, src_mac, rssi);
        break;
    case MESH_PACKET_TYPE_TRACE_REPLY:
        ret = flood_process_trace_reply(data, length, src_mac, rssi);
        break;
    default:
        ret = ESP_ERR_INVALID_STATE;
        break;
    }

    if (ret != ESP_OK)
    {
        ESP_LOGW(TAG, "Failed to process packet: 0x%x (%s)", (int)ret, esp_err_to_name(ret));
        return ret;
    }
    return ESP_OK;
}

static esp_err_t flood_forward_packet(const uint8_t* data, uint16_t length)
{
    // Role-based forwarding behavior
    switch (s_device_role)
    {
    case MESH_ROLE_CLIENT:
        // TTL decrement is handled in the main loop
        ESP_LOGD(TAG, "Client role: forwarding packet");
        break;
    case MESH_ROLE_ROUTER:
        // TTL decrement is handled in the main loop
        ESP_LOGD(TAG, "Router role: forwarding packet");
        break;
    case MESH_ROLE_REPEATER:
        // todo Repeaters extend network range with optimized forwarding
        break;
    case MESH_ROLE_CHANNEL:
        // not a role, just for internal use
        break;
    }

    // Create new packet with decremented TTL
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    memcpy(packet, data, length);
    mesh_packet_header_t* header = (mesh_packet_header_t*)packet;
    if (header->ttl > 0)
    {
        header->ttl--;
        header->hops++;
        header->flags |= MESH_FLAG_FORWARDED;

        ESP_LOGI(TAG,
                 "[Q] FORWARD *%08x from " MACSTR " >> " MACSTR " TTL: %d",
                 header->sequence,
                 MAC2STR(header->source_mac),
                 MAC2STR(header->dest_mac),
                 header->ttl);
        // Enqueue for sending
        esp_err_t ret = flood_enqueue_packet(packet, length);
        if (ret != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to enqueue forward *%08x: %s", header->sequence, esp_err_to_name(ret));
            return ret;
        }
    }

    return ESP_OK;
}

static bool flood_is_broadcast_mac(const uint8_t* mac) { return memcmp(mac, s_broadcast_mac, ESP_NOW_ETH_ALEN) == 0; }

static bool flood_is_our_mac(const uint8_t* mac) { return memcmp(mac, s_our_mac, 6) == 0; }

static uint32_t flood_get_timestamp(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000); // Convert to milliseconds
}

static esp_err_t flood_add_device_internal(const mesh_device_info_t* device)
{
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Check if device already exists in persistent storage
    mesh_device_persistent_t existing_persistent = {0};
    esp_err_t ret = flood_load_device_persistent_internal(device->persistent.mac, &existing_persistent);

    if (ret == ESP_OK)
    {
        // Update existing device - save persistent data
        flood_save_device_persistent_internal(&device->persistent);

        // Update volatile data
        flood_update_device_volatile_internal(device->persistent.mac, &device->volatile_data);

        if (s_device_callback != NULL)
        {
            s_device_callback(device, false, s_device_callback_user_data); // false = updated, not added
        }

        ESP_LOGD(TAG, "Updated device " MACSTR, MAC2STR(device->persistent.mac));
    }
    else
    {
        // Add new device
        // Save persistent data
        flood_save_device_persistent_internal(&device->persistent);

        // Save volatile data
        flood_update_device_volatile_internal(device->persistent.mac, &device->volatile_data);

        if (s_device_callback != NULL)
        {
            s_device_callback(device, true, s_device_callback_user_data); // true = added
        }

        ESP_LOGD(TAG, "Added device " MACSTR, MAC2STR(device->persistent.mac));
    }

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

static esp_err_t flood_find_device_internal(const uint8_t* mac, mesh_device_info_t* device)
{
    if (mac == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Try to find in persistent storage
    mesh_device_persistent_t persistent_data = {0};
    esp_err_t ret = flood_load_device_persistent_internal(mac, &persistent_data);
    if (ret != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND; // Device not found
    }

    // Get volatile data
    mesh_device_volatile_t volatile_data = {0};
    ret = flood_get_device_volatile_internal(mac, &volatile_data);
    if (ret != ESP_OK)
    {
        // No volatile data, initialize with defaults
        memcpy(volatile_data.mac, mac, 6);
        volatile_data.last_seen = 0;
        volatile_data.signal_strength = 0;
        volatile_data.hops = 0;
        volatile_data.battery_level = 255; // Unknown
        volatile_data.unread_messages = 0;
    }

    // Combine the data
    device->persistent = persistent_data;
    device->volatile_data = volatile_data;

    return ESP_OK;
}

static esp_err_t flood_find_device(const uint8_t* mac, mesh_device_info_t* device)
{
    if (mac == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = flood_find_device_internal(mac, device);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

static void flood_task(void* pvParameter)
{
    ESP_LOGI(TAG, "Flood task started");

    s_flood_running = true;

    // Initialize timers
    uint32_t last_hello = 0;
    uint32_t last_cache_cleanup = 0;
    uint32_t last_ack_check = 0;

    // Enqueue initial hello immediately
    flood_send_hello();
    last_hello = flood_get_timestamp();

    while (s_flood_running)
    {
        // Compute wait time until next periodic hello
        uint32_t now = flood_get_timestamp();
        uint32_t next_hello_at = last_hello + s_hello_interval * 1000;

        mesh_queued_packet_t queued = {0};

        // Wait for next packet in queue for 1 second
        if (xQueueReceive(s_message_queue, &queued, pdMS_TO_TICKS(1000)) == pdTRUE)
        {
            // Got a packet to send
            mesh_packet_header_t* header = (mesh_packet_header_t*)queued.data;
            ESP_LOGI(TAG, "[<<] %s *%08x", flood_packet_type_to_string(header->type), header->sequence);
            esp_err_t send_ret = esp_now_send(s_broadcast_mac, queued.data, queued.length);
            if (send_ret != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to send queued packet: %s", esp_err_to_name(send_ret));
            }
            else
            {
                // add to waiting ACK list (skip retries - they're already in the list)
                if (header->type != MESH_PACKET_TYPE_ACK && header->flags & MESH_FLAG_ACK_REQUIRED &&
                    !(header->flags & MESH_FLAG_RETRY))
                {
                    esp_err_t ret = flood_waiting_ack_add(queued.data, queued.length);
                    if (ret != ESP_OK)
                    {
                        ESP_LOGE(TAG,
                                 "Failed to add message *%08x to waiting ACK list: %s",
                                 header->sequence,
                                 esp_err_to_name(ret));
                    }
                }
                // call callback
                if (s_sent_packet_callback != NULL)
                {
                    s_sent_packet_callback(queued.data, queued.length, s_sent_packet_callback_user_data);
                }
            }
        }

        // After handling a packet (or timeout), ensure HELLO schedule isn't starved under load
        now = flood_get_timestamp();
        if (now >= next_hello_at)
        {
            flood_send_hello();
            last_hello = now;
        }

        // Periodic cache cleanup
        if (now - last_cache_cleanup >= MESH_CACHE_CLEANUP_INTERVAL)
        {
            flood_cache_cleanup();
            last_cache_cleanup = now;
        }

        // Check waiting ACK list for timeouts and retry
        if (now - last_ack_check >= 1000) // Check every second
        {
            flood_waiting_ack_check_timeouts();
            last_ack_check = now;
        }
    }

    s_flood_task_handle = NULL;
    ESP_LOGI(TAG, "Flood task ended");
    vTaskDelete(NULL);
}

static void get_context_path(char* path) { strncpy(path, s_context_path, MAXNAMLEN); }

static esp_err_t get_devices_path(char* path)
{
    get_context_path(path);
    if (path_join(path, DEVICES_DIRECTORY))
    {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t get_device_path(const uint8_t* mac, char* path)
{
    char mac_str[13];
    if (get_devices_path(path) == ESP_OK)
    {
        mac_to_string(mac, mac_str, sizeof(mac_str));
        if (path_join(path, mac_str))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t get_device_meta_path(const uint8_t* mac, char* path)
{
    if (get_device_path(mac, path) == ESP_OK)
    {
        if (path_join(path, DEVICE_META_FILE))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t get_messages_file_path(const uint8_t* mac, char* path)
{
    if (get_device_path(mac, path) == ESP_OK)
    {
        if (path_join(path, MESSAGES_FILE))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t get_traces_file_path(const uint8_t* mac, char* path)
{
    if (get_device_path(mac, path) == ESP_OK)
    {
        if (path_join(path, TRACES_FILE))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* Channel path helper functions */

/**
 * @brief Validate channel name for safe filesystem usage
 *
 * Checks if a channel name is valid for use as a directory name.
 * Validates length, character set, and prevents directory traversal attacks.
 *
 * @param channel_name Channel name to validate
 * @return true if name is valid, false otherwise
 *
 * Valid names must:
 * - Not be NULL or empty
 * - Be between 1 and MESH_MAX_NAME_LENGTH characters
 * - Contain only alphanumeric, underscore, hyphen, and dot characters
 * - Not be "." or ".." (directory traversal prevention)
 * - Not contain filesystem special characters (/, \, :, *, ?, ", <, >, |)
 */
static bool flood_channel_name_valid(const char* channel_name)
{
    if (channel_name == NULL || channel_name[0] == '\0')
    {
        ESP_LOGW(TAG, "Channel name is NULL or empty");
        return false;
    }

    size_t len = strlen(channel_name);

    // Check length
    if (len > MESH_MAX_NAME_LENGTH)
    {
        ESP_LOGW(TAG, "Channel name too long: %zu (max %d)", len, MESH_MAX_NAME_LENGTH);
        return false;
    }

    // Prevent directory traversal
    if (strcmp(channel_name, ".") == 0 || strcmp(channel_name, "..") == 0)
    {
        ESP_LOGW(TAG, "Channel name cannot be '.' or '..'");
        return false;
    }

    // Check for invalid filesystem characters
    const char* invalid_chars = "/\\:*?\"<>|";
    for (size_t i = 0; i < len; i++)
    {
        char c = channel_name[i];

        // Check for control characters (0x00-0x1F, 0x7F)
        if (c <= 0x1F || c == 0x7F)
        {
            ESP_LOGW(TAG, "Channel name contains control character at position %zu", i);
            return false;
        }

        // Check for invalid filesystem characters
        if (strchr(invalid_chars, c) != NULL)
        {
            ESP_LOGW(TAG, "Channel name contains invalid character '%c' at position %zu", c, i);
            return false;
        }
    }

    return true;
}

static esp_err_t get_channels_path(char* path)
{
    get_context_path(path);
    if (path_join(path, CHANNELS_DIRECTORY))
    {
        return ESP_OK;
    }
    return ESP_FAIL;
}

static esp_err_t get_channel_path(const char* channel_name, char* path)
{
    if (!flood_channel_name_valid(channel_name))
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (get_channels_path(path) == ESP_OK)
    {
        if (path_join(path, channel_name))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t get_channel_meta_path(const char* channel_name, char* path)
{
    if (get_channel_path(channel_name, path) == ESP_OK)
    {
        if (path_join(path, CHANNEL_META_FILE))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

static esp_err_t get_channel_messages_file_path(const char* channel_name, char* path)
{
    if (get_channel_path(channel_name, path) == ESP_OK)
    {
        if (path_join(path, MESSAGES_FILE))
        {
            return ESP_OK;
        }
    }
    return ESP_FAIL;
}

/* Channel Management Functions */

static esp_err_t create_channel_directory(const char* channel_name)
{
    char channel_path[PATH_BUF_SIZE];
    if (get_channel_path(channel_name, channel_path) != ESP_OK)
    {
        return ESP_FAIL;
    }
    // Create directory if it doesn't exist
    struct stat st = {0};
    if (stat(channel_path, &st) == -1)
    {
        if (mkdir(channel_path, 0755) == -1)
        {
            ESP_LOGE(TAG, "Failed to create channel directory: %s", channel_path);
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

static esp_err_t flood_load_channel_persistent_internal(const char* channel_name, mesh_channel_persistent_t* persistent)
{
    if (channel_name == NULL || persistent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Get meta file path
    char meta_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_meta_path(channel_name, meta_path);
    if (ret != ESP_OK)
    {
        return ESP_FAIL;
    }

    // Check if file exists
    struct stat st;
    if (stat(meta_path, &st) != 0)
    {
        ESP_LOGD(TAG, "Channel metadata file not found: %s", meta_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Read channel metadata from binary file
    FILE* file = fopen(meta_path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open channel meta file for reading: %s", meta_path);
        return ESP_FAIL;
    }

    size_t read = fread(persistent, sizeof(mesh_channel_persistent_t), 1, file);
    fclose(file);

    if (read != 1)
    {
        ESP_LOGE(TAG, "Failed to read channel metadata from file: %s", meta_path);
        return ESP_FAIL;
    }

    if (persistent->magic != MESH_MAGIC_NUMBER)
    {
        ESP_LOGE(TAG, "Invalid channel metadata magic number: %08X", persistent->magic);
        return ESP_FAIL;
    }

    if (persistent->version != MESH_PERSISTENT_VERSION)
    {
        ESP_LOGE(TAG, "Invalid channel metadata version: %d", persistent->version);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Loaded channel metadata from: %s", meta_path);
    return ESP_OK;
}

static esp_err_t flood_save_channel_persistent_internal(const mesh_channel_persistent_t* persistent)
{
    if (persistent == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Create channel directory
    esp_err_t ret = create_channel_directory((const char*)persistent->channel_name);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Get meta file path
    char meta_path[PATH_BUF_SIZE];
    ret = get_channel_meta_path((const char*)persistent->channel_name, meta_path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Write channel metadata to binary file
    FILE* file = fopen(meta_path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open channel meta file for writing: %s", meta_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(persistent, sizeof(mesh_channel_persistent_t), 1, file);
    fclose(file);

    if (written != 1)
    {
        ESP_LOGE(TAG, "Failed to write channel metadata to file: %s", meta_path);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Saved channel metadata to: %s", meta_path);
    return ESP_OK;
}

static esp_err_t flood_find_channel_internal(const char* channel_name, mesh_channel_info_t* channel_info)
{
    if (channel_name == NULL || channel_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Try to find in persistent storage
    mesh_channel_persistent_t persistent_data = {0};
    esp_err_t ret = flood_load_channel_persistent_internal(channel_name, &persistent_data);
    if (ret != ESP_OK)
    {
        return ESP_ERR_NOT_FOUND; // Channel not found
    }

    // Get volatile data
    mesh_channel_volatile_t volatile_data = {0};
    ret = flood_get_channel_volatile_internal(channel_name, &volatile_data);
    if (ret != ESP_OK)
    {
        // No volatile data, initialize with defaults
        strncpy((char*)volatile_data.channel_name, channel_name, MESH_MAX_NAME_LENGTH);
        volatile_data.last_seen = 0;
        volatile_data.unread_messages = 0;
    }

    // Combine the data
    channel_info->persistent = persistent_data;
    channel_info->volatile_data = volatile_data;

    return ESP_OK;
}

static int32_t flood_save_channel_message_internal(const char* channel_name,
                                                   const uint8_t* sender_mac,
                                                   uint32_t sequence,
                                                   uint8_t status,
                                                   uint8_t message_type,
                                                   const uint8_t* message_data,
                                                   uint16_t message_length)
{
    if (channel_name == NULL || sender_mac == NULL || message_data == NULL || message_length == 0 ||
        message_length > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG,
                 "Invalid channel message data: channel: %s, sequence: *%08x, message_type: %d, message_length: %d",
                 channel_name ? channel_name : "NULL",
                 sequence,
                 message_type,
                 message_length);
        return -1;
    }

    // Create channel directory if needed
    esp_err_t ret = create_channel_directory(channel_name);
    if (ret != ESP_OK)
    {
        return -1;
    }

    // Get messages file path
    char messages_file_path[PATH_BUF_SIZE];
    ret = get_channel_messages_file_path(channel_name, messages_file_path);
    if (ret != ESP_OK)
    {
        return -1;
    }

    ESP_LOGD(TAG, "Saving channel message to %s ...", messages_file_path);

    // Get current message count (file size / record size) before saving
    struct stat st;
    int32_t message_id = 0;
    if (stat(messages_file_path, &st) == 0)
    {
        message_id = st.st_size / sizeof(message_record_t);
    }
    ESP_LOGD(TAG, "Channel message count: %ld", message_id);

    // Create message record
    message_record_t record;
    memset(&record, 0, sizeof(message_record_t));
    memcpy(record.sender_mac, sender_mac, 6);
    record.sequence = sequence;
    record.timestamp = flood_get_timestamp();
    record.status = status;
    record.message_type = message_type;
    record.message_length = message_length;
    memcpy(record.message_data, message_data, message_length);

    // Use shared implementation to save message
    ret = flood_save_message_to_file_internal(messages_file_path, &record);
    if (ret != ESP_OK)
    {
        return -1;
    }

    ESP_LOGD(TAG, "Saved channel message #%ld to %s *%08X", message_id, channel_name, sequence);
    return message_id;
}

static esp_err_t flood_update_channel_message_status_internal(const char* channel_name, uint32_t message_id, uint8_t status)
{
    if (channel_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_messages_file_path(channel_name, messages_file_path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Check if file exists
    struct stat st;
    if (stat(messages_file_path, &st) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Check if message_id is valid
    uint32_t total_records = st.st_size / sizeof(message_record_t);
    if (message_id >= total_records)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Open file for read/write
    FILE* file = fopen(messages_file_path, "r+b");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open channel messages file: %s", messages_file_path);
        return ESP_FAIL;
    }

    // Seek to message record (O(1) access)
    if (fseek(file, message_id * sizeof(message_record_t), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek to channel message #%lu", message_id);
        fclose(file);
        return ESP_FAIL;
    }

    // Read current record
    message_record_t record;
    if (fread(&record, sizeof(message_record_t), 1, file) != 1)
    {
        ESP_LOGE(TAG, "Failed to read channel message record");
        fclose(file);
        return ESP_FAIL;
    }

    // Update status
    record.status = status;

    // Seek back and write
    if (fseek(file, message_id * sizeof(message_record_t), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek for write");
        fclose(file);
        return ESP_FAIL;
    }

    if (fwrite(&record, sizeof(message_record_t), 1, file) != 1)
    {
        ESP_LOGE(TAG, "Failed to write updated channel message record");
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);

    ESP_LOGI(TAG, "Updated channel message #%lu status to 0x%02X for channel %s", message_id, status, channel_name);
    // call callback if set
    if (s_message_status_callback != NULL)
    {
        // For channel messages, use NULL mac address in callback
        uint8_t null_mac[6] = {0};
        s_message_status_callback(null_mac, message_id, status, s_message_status_callback_user_data);
    }
    return ESP_OK;
}

esp_err_t flood_send_ack(const uint8_t* dest_mac, uint32_t sequence, uint8_t status)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    mesh_ack_packet_t* ack_packet = (mesh_ack_packet_t*)packet;
    mesh_packet_header_t* header = &ack_packet->header;
    // fill header
    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_ACK;
    header->flags = 0;
    header->hops = 0;
    header->ttl = s_max_ttl;
    header->sequence = s_sequence_number++;
    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, dest_mac, 6);
    ack_packet->status = status;
    ack_packet->ack_sequence = sequence;

    // Enqueue packet
    esp_err_t ret = flood_enqueue_packet(packet, sizeof(mesh_ack_packet_t));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enqueue ack *%08x: %s", header->sequence, esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "[Q] ACK *%08x to " MACSTR " (status: %d)", header->sequence, MAC2STR(dest_mac), status);
    return ESP_OK;
}

#if 0
esp_err_t flood_send_message(const uint8_t* dest_mac, const uint8_t* data, uint16_t length, uint8_t flags)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (length > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG, "Message too large: %d bytes", length);
        return ESP_ERR_INVALID_SIZE;
    }
    if (length == 0)
    {
        ESP_LOGE(TAG, "Message is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    // Create packet
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    mesh_message_packet_t* message_packet = (mesh_message_packet_t*)packet;
    mesh_packet_header_t* header = &message_packet->header;

    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_MESSAGE;
    header->flags = flags | MESH_FLAG_ACK_REQUIRED;
    header->hops = 0;
    header->ttl = s_max_ttl;
    // header->length = sizeof(mesh_message_packet_t) - sizeof(mesh_packet_header_t);
    header->sequence = s_sequence_number++;

    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, dest_mac, 6);

    message_packet->message_length = length;
    if (length > 0)
    {
        memcpy(message_packet->message_text, data, length);
    }
    // save message to file
    int32_t message_id = flood_save_private_message(header->dest_mac,
                                            header->source_mac,
                                            header->sequence,
                                            message_packet->message_type,
                                            message_packet->message_text,
                                            message_packet->message_length);

    // Enqueue packet
    esp_err_t ret = flood_enqueue_packet(packet, sizeof(mesh_message_packet_t) + length);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enqueue *%08x: %s", header->sequence, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG,
             "[Q] MESSAGE *%08x to " MACSTR ": [%d] \"%.*s\"",
             header->sequence,
             MAC2STR(header->dest_mac),
             message_packet->message_length,
             message_packet->message_length,
             (char*)message_packet->message_text);

    return ESP_OK;
}
#endif

esp_err_t
flood_send_channel_message(const char* channel_name, const uint8_t* data, uint16_t length, uint8_t message_type, uint8_t flags)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        ESP_LOGE(TAG, "Flood not initialized or not running");
        return ESP_ERR_INVALID_STATE;
    }

    if (!flood_channel_name_valid(channel_name))
    {
        ESP_LOGE(TAG, "Invalid channel name");
        return ESP_ERR_INVALID_ARG;
    }

    if (data == NULL)
    {
        ESP_LOGE(TAG, "Data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (length > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG, "Message too large: %d bytes (max %d)", length, MESSAGE_MAX_PAYLOAD);
        return ESP_ERR_INVALID_SIZE;
    }

    if (length == 0)
    {
        ESP_LOGE(TAG, "Message is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    // Take mutex for thread safety
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        ESP_LOGE(TAG, "Failed to acquire mutex");
        return ESP_ERR_INVALID_STATE;
    }

    // Create packet
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    mesh_message_packet_t* message_packet = (mesh_message_packet_t*)packet;
    mesh_packet_header_t* header = &message_packet->header;

    // Fill header
    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_MESSAGE;
    header->flags = flags | MESH_FLAG_ACK_REQUIRED;
    header->hops = 0;
    header->ttl = s_max_ttl;
    header->sequence = s_sequence_number++;

    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, s_broadcast_mac, 6); // Broadcast to all devices

    // Fill channel message fields
    strncpy((char*)message_packet->channel_name, channel_name, MESH_MAX_NAME_LENGTH);
    // message_packet->channel_name[MESH_MAX_NAME_LENGTH] = '\0';
    message_packet->message_type = message_type; // unused for now
    message_packet->message_length = length;
    memcpy(message_packet->message_text, data, length);

    // Save message to channel storage and get message_id
    message_packet->message_id = flood_save_channel_message_internal(channel_name,
                                                                     s_our_mac,
                                                                     header->sequence,
                                                                     MESSAGE_STATUS_SENT,
                                                                     message_type,
                                                                     data,
                                                                     length);

    // Enqueue packet for transmission
    esp_err_t ret = flood_enqueue_packet(packet, sizeof(mesh_message_packet_t) + length);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enqueue channel message *%08x: %s", header->sequence, esp_err_to_name(ret));
        xSemaphoreGive(s_flood_mutex);
        return -1;
    }

    ESP_LOGD(TAG,
             "[Q] CHANNEL MESSAGE *%08x to #%s: [%d] \"%.*s\"",
             header->sequence,
             channel_name,
             message_packet->message_length,
             message_packet->message_length,
             (char*)message_packet->message_text);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

esp_err_t flood_send_private_message(const uint8_t* dest_mac, const uint8_t* data, uint16_t length, uint8_t flags)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (length > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG, "Message too large: %d bytes", length);
        return ESP_ERR_INVALID_SIZE;
    }
    if (length == 0)
    {
        ESP_LOGE(TAG, "Message is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    // Create packet
    uint8_t packet[ESP_NOW_MAX_DATA_LEN];
    mesh_private_packet_t* private_packet = (mesh_private_packet_t*)packet;
    mesh_packet_header_t* header = &private_packet->header;

    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_PRIVATE;
    header->flags = flags | MESH_FLAG_ACK_REQUIRED;
    header->hops = 0;
    header->ttl = s_max_ttl;
    // header->length = sizeof(mesh_message_packet_t) - sizeof(mesh_packet_header_t);
    header->sequence = s_sequence_number++;

    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, dest_mac, 6);

    private_packet->message_length = length;
    if (length > 0)
    {
        memcpy(private_packet->message_text, data, length);
    }
    // save message to file and get message_id for delivery tracking
    private_packet->message_id = flood_save_private_message(header->dest_mac,
                                                            header->source_mac,
                                                            header->sequence,
                                                            private_packet->message_type,
                                                            private_packet->message_text,
                                                            private_packet->message_length);

    // Enqueue packet
    esp_err_t ret = flood_enqueue_packet(packet, sizeof(mesh_private_packet_t) + length);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enqueue *%08x: %s", header->sequence, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG,
             "[Q] PRIVATE *%08x to " MACSTR ": [%d] \"%.*s\"",
             header->sequence,
             MAC2STR(header->dest_mac),
             private_packet->message_length,
             private_packet->message_length,
             (char*)private_packet->message_text);

    return ESP_OK;
}

esp_err_t flood_send_hello(void)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        return ESP_ERR_INVALID_STATE;
    }

    mesh_hello_packet_t hello_packet = {0};
    mesh_packet_header_t* header = &hello_packet.header;

    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_HELLO;
    header->hops = 0;
    header->flags = 0;
    header->ttl = s_max_ttl;
    // header->length = sizeof(mesh_hello_packet_t) - sizeof(mesh_packet_header_t);
    header->sequence = s_sequence_number++;

    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, s_broadcast_mac, 6);

    // Create device name with MAC last 4 characters
    // snprintf((char*)hello_packet.device_name,
    //          sizeof(hello_packet.device_name),
    //          "%s_%02X%02X",
    //          s_device_name,
    //          our_mac[4],
    //          our_mac[5]);
    strncpy((char*)hello_packet.device_name, s_device_name, sizeof(hello_packet.device_name) - 1);
    hello_packet.role = s_device_role;
    hello_packet.capabilities = s_device_capabilities;
    hello_packet.battery_level = s_device_battery_level;

    esp_err_t ret = flood_enqueue_packet((uint8_t*)&hello_packet, sizeof(hello_packet));
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to enqueue hello *%08x: %s", header->sequence, esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGD(TAG, "[Q] HELLO *%08x", header->sequence);
    return ESP_OK;
}

esp_err_t flood_add_device(const mesh_device_info_t* device)
{
    if (device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    return flood_add_device_internal(device);
}

esp_err_t flood_remove_device(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Remove from persistent storage (file-based)
    char meta_path[PATH_BUF_SIZE];
    esp_err_t ret = get_device_meta_path(mac, meta_path);
    if (ret == ESP_OK)
    {
        struct stat st;
        if (stat(meta_path, &st) == 0)
        {
            if (unlink(meta_path) == 0)
            {
                ESP_LOGD(TAG, "Removed device metadata file: %s", meta_path);
            }

            // Remove messages file
            char msg_path[PATH_BUF_SIZE];
            if (get_messages_file_path(mac, msg_path) == ESP_OK)
            {
                unlink(msg_path);
            }

            // Remove traces file
            char trc_path[PATH_BUF_SIZE];
            if (get_traces_file_path(mac, trc_path) == ESP_OK)
            {
                unlink(trc_path);
            }

            // Remove the device directory (should be empty now)
            char* device_path = meta_path;
            if (get_device_path(mac, device_path) == ESP_OK)
            {
                rmdir(device_path);
            }
        }

        // Remove from volatile storage
        flood_remove_device_volatile_internal(mac);

        ESP_LOGD(TAG, "Removed device " MACSTR, MAC2STR(mac));
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    xSemaphoreGive(s_flood_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flood_cache_add(uint32_t sequence, const uint8_t* source_mac)
{
    if (source_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Find empty slot or oldest entry
    int slot = -1;
    uint32_t oldest_time = UINT32_MAX;
    int oldest_slot = 0;

    for (int i = 0; i < MESH_MAX_CACHE_ENTRIES; i++)
    {
        if (s_packet_cache.cache[i].timestamp < oldest_time)
        {
            oldest_time = s_packet_cache.cache[i].timestamp;
            oldest_slot = i;
        }
    }

    if (slot == -1)
    {
        slot = oldest_slot; // Replace oldest entry
    }

    s_packet_cache.cache[slot].sequence = sequence;
    memcpy(s_packet_cache.cache[slot].source_mac, source_mac, 6);
    s_packet_cache.cache[slot].timestamp = flood_get_timestamp();

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

bool flood_cache_check(uint32_t sequence, const uint8_t* source_mac)
{
    if (source_mac == NULL)
    {
        return false;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return false;
    }

    for (int i = 0; i < MESH_MAX_CACHE_ENTRIES; i++)
    {
        if (s_packet_cache.cache[i].sequence == sequence && memcmp(s_packet_cache.cache[i].source_mac, source_mac, 6) == 0)
        {
            xSemaphoreGive(s_flood_mutex);
            return true;
        }
    }

    xSemaphoreGive(s_flood_mutex);
    return false;
}

esp_err_t flood_cache_cleanup(void)
{
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t now = flood_get_timestamp();
    int cleaned = 0;

    for (int i = 0; i < MESH_MAX_CACHE_ENTRIES; i++)
    {
        if ((now - s_packet_cache.cache[i].timestamp) > MESH_CACHE_TIMEOUT)
        {
            memset(&s_packet_cache.cache[i], 0, sizeof(mesh_packet_cache_entry_t));
            cleaned++;
        }
    }

    s_packet_cache.last_cleanup = now;
    xSemaphoreGive(s_flood_mutex);

    if (cleaned > 0)
    {
        ESP_LOGD(TAG, "Cleaned up %d cache entries", cleaned);
    }

    return ESP_OK;
}

esp_err_t flood_register_message_callback(flood_message_callback_t callback, void* user_data)
{
    s_message_callback = callback;
    s_message_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_register_message_status_callback(flood_message_status_callback_t callback, void* user_data)
{
    s_message_status_callback = callback;
    s_message_status_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_register_device_callback(flood_device_callback_t callback, void* user_data)
{
    s_device_callback = callback;
    s_device_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_register_sent_packet_callback(flood_packet_callback_t callback, void* user_data)
{
    s_sent_packet_callback = callback;
    s_sent_packet_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_register_received_packet_callback(flood_packet_callback_t callback, void* user_data)
{
    s_received_packet_callback = callback;
    s_received_packet_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_register_trace_callback(flood_trace_callback_t callback, void* user_data)
{
    s_trace_callback = callback;
    s_trace_callback_user_data = user_data;
    return ESP_OK;
}

esp_err_t flood_send_trace_request(const uint8_t* dest_mac, uint32_t* out_trace_id)
{
    if (!s_flood_initialized || !s_flood_running)
    {
        return ESP_ERR_INVALID_STATE;
    }
    if (dest_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    mesh_trace_request_t trace_req = {0};
    mesh_packet_header_t* header = &trace_req.header;
    header->magic = MESH_MAGIC_NUMBER;
    header->version = MESH_PROTOCOL_VERSION;
    header->type = MESH_PACKET_TYPE_TRACE_REQ;
    header->flags = 0;
    header->hops = 0;
    header->ttl = s_max_ttl;
    header->sequence = s_sequence_number++;
    memcpy(header->source_mac, s_our_mac, 6);
    memcpy(header->dest_mac, dest_mac, 6);
    trace_req.trace_id = header->sequence;
    trace_req.hop_count = 0;

    if (out_trace_id != NULL)
    {
        *out_trace_id = trace_req.trace_id;
    }

    esp_err_t ret = flood_enqueue_packet((uint8_t*)&trace_req, sizeof(mesh_trace_request_t));
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "[Q] TRACE_REQ *%08x to " MACSTR, header->sequence, MAC2STR(dest_mac));
    }
    return ret;
}

/* ========================================================================
 * Traceroute File Storage (.trc circular files)
 * ======================================================================== */

esp_err_t flood_trace_save(const uint8_t* mac, const flood_trace_record_t* record)
{
    if (mac == NULL || record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    create_device_directory(mac);

    char path[PATH_BUF_SIZE];
    esp_err_t ret = get_traces_file_path(mac, path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    flood_trace_file_header_t hdr = {0};
    FILE* file = fopen(path, "r+b");
    if (file == NULL)
    {
        file = fopen(path, "w+b");
        if (file == NULL)
        {
            ESP_LOGE(TAG, "Failed to create traces file: %s", path);
            xSemaphoreGive(s_flood_mutex);
            return ESP_FAIL;
        }
        flood_trace_file_header_t empty_hdr = {0};
        fwrite(&empty_hdr, sizeof(empty_hdr), 1, file);
        flood_trace_record_t empty_rec;
        memset(&empty_rec, 0, sizeof(empty_rec));
        for (int i = 0; i < FLOOD_TRACE_MAX_RECORDS; i++)
            fwrite(&empty_rec, sizeof(empty_rec), 1, file);
        fseek(file, 0, SEEK_SET);
    }

    if (fread(&hdr, sizeof(hdr), 1, file) != 1)
    {
        hdr.count = 0;
        hdr.write_pos = 0;
    }

    long record_offset = sizeof(flood_trace_file_header_t) + hdr.write_pos * sizeof(flood_trace_record_t);
    fseek(file, record_offset, SEEK_SET);
    fwrite(record, sizeof(flood_trace_record_t), 1, file);

    if (hdr.count < FLOOD_TRACE_MAX_RECORDS)
    {
        hdr.count++;
    }
    hdr.write_pos = (hdr.write_pos + 1) % FLOOD_TRACE_MAX_RECORDS;

    fseek(file, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, file);
    fclose(file);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

esp_err_t flood_trace_update(const uint8_t* mac, uint32_t trace_id, const flood_trace_record_t* record)
{
    if (mac == NULL || record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char path[PATH_BUF_SIZE];
    esp_err_t ret = get_traces_file_path(mac, path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    FILE* file = fopen(path, "r+b");
    if (file == NULL)
    {
        xSemaphoreGive(s_flood_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    flood_trace_file_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, file) != 1)
    {
        fclose(file);
        xSemaphoreGive(s_flood_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    for (uint16_t i = 0; i < hdr.count; i++)
    {
        long offset = sizeof(flood_trace_file_header_t) + i * sizeof(flood_trace_record_t);
        fseek(file, offset, SEEK_SET);
        flood_trace_record_t tmp;
        if (fread(&tmp, sizeof(tmp), 1, file) != 1)
        {
            break;
        }
        if (tmp.trace_id == trace_id)
        {
            fseek(file, offset, SEEK_SET);
            fwrite(record, sizeof(flood_trace_record_t), 1, file);
            fclose(file);
            xSemaphoreGive(s_flood_mutex);
            return ESP_OK;
        }
    }

    fclose(file);
    xSemaphoreGive(s_flood_mutex);
    return ESP_ERR_NOT_FOUND;
}

esp_err_t flood_trace_get_count(const uint8_t* mac, uint32_t* count)
{
    if (mac == NULL || count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char path[PATH_BUF_SIZE];
    esp_err_t ret = get_traces_file_path(mac, path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    FILE* file = fopen(path, "rb");
    if (file == NULL)
    {
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    flood_trace_file_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, file) == 1)
    {
        *count = hdr.count;
    }
    fclose(file);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

esp_err_t flood_trace_load_all(const uint8_t* mac, flood_trace_record_t* records, uint32_t* loaded)
{
    if (mac == NULL || records == NULL || loaded == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    *loaded = 0;
    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, pdMS_TO_TICKS(500)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char path[PATH_BUF_SIZE];
    esp_err_t ret = get_traces_file_path(mac, path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    FILE* file = fopen(path, "rb");
    if (file == NULL)
    {
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    flood_trace_file_header_t hdr;
    if (fread(&hdr, sizeof(hdr), 1, file) != 1 || hdr.count == 0)
    {
        fclose(file);
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    if (hdr.count < FLOOD_TRACE_MAX_RECORDS)
    {
        fseek(file, sizeof(flood_trace_file_header_t), SEEK_SET);
        size_t rd = fread(records, sizeof(flood_trace_record_t), hdr.count, file);
        *loaded = (uint32_t)rd;
    }
    else
    {
        uint16_t oldest = hdr.write_pos;
        uint32_t idx = 0;
        for (uint16_t i = 0; i < hdr.count; i++)
        {
            uint16_t slot = (oldest + i) % FLOOD_TRACE_MAX_RECORDS;
            long offset = sizeof(flood_trace_file_header_t) + slot * sizeof(flood_trace_record_t);
            fseek(file, offset, SEEK_SET);
            if (fread(&records[idx], sizeof(flood_trace_record_t), 1, file) == 1)
            {
                idx++;
            }
        }
        *loaded = idx;
    }

    fclose(file);
    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

/* Role Management Functions */
esp_err_t flood_set_device_role(FLOOD_DEVICE_ROLE_t role)
{
    if (role < MESH_ROLE_CLIENT || role > MESH_ROLE_REPEATER)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_device_role = role;
    ESP_LOGI(TAG, "Device role set to %d", role);
    return ESP_OK;
}

FLOOD_DEVICE_ROLE_t flood_get_device_role(void) { return s_device_role; }

esp_err_t flood_set_device_capabilities(uint8_t capabilities)
{
    s_device_capabilities = capabilities;
    ESP_LOGI(TAG, "Device capabilities set to 0x%02X", capabilities);
    return ESP_OK;
}

uint8_t flood_get_device_capabilities(void) { return s_device_capabilities; }

esp_err_t flood_set_battery_level(uint8_t battery_level)
{
    if (battery_level > 100)
    {
        return ESP_ERR_INVALID_ARG;
    }

    s_device_battery_level = battery_level;
    ESP_LOGD(TAG, "Device battery level set to %d%%", battery_level);
    return ESP_OK;
}

uint8_t flood_get_battery_level(void) { return s_device_battery_level; }

/* Device Enumeration */
esp_err_t flood_enum_devices(flood_device_enum_callback_t callback, void* user_data)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Enumerate persistent devices (file-based)
    char path_buf[PATH_BUF_SIZE];

    if (get_devices_path(path_buf) != ESP_OK)
    {
        return ESP_FAIL;
    }
    size_t path_len = strlen(path_buf);

    DIR* dir = opendir(path_buf);
    if (dir != NULL)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL)
        {
            // Skip . and .. entries
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            // cutting last name from path_buf
            path_buf[path_len] = '\0';
            // adding new name to path_buf
            if (path_join(path_buf, entry->d_name))
            {
                struct stat st;
                if (stat(path_buf, &st) == 0 && S_ISDIR(st.st_mode))
                {
                    // Check if meta.bin exists in this directory
                    if (!path_join(path_buf, DEVICE_META_FILE))
                    {
                        ESP_LOGE(TAG, "Failed to join path: %s", path_buf);
                        continue;
                    }

                    if (stat(path_buf, &st) == 0 && S_ISREG(st.st_mode))
                    {
                        // load persistent data inline using the new function
                        mesh_device_persistent_t device_persistent;
                        if (flood_load_device_persistent_from_meta_internal(path_buf, &device_persistent) == ESP_OK)
                        {
                            // Create combined device info
                            mesh_device_info_t device_info = {0};
                            device_info.persistent = device_persistent;

                            // Try to find corresponding volatile data
                            mesh_device_volatile_t volatile_data = {0};
                            esp_err_t ret = flood_get_device_volatile_internal(device_persistent.mac, &volatile_data);
                            if (ret == ESP_OK)
                            {
                                device_info.volatile_data = volatile_data;
                            }
                            else
                            {
                                // No volatile data found, initialize with defaults
                                memcpy(device_info.volatile_data.mac, device_persistent.mac, 6);
                                device_info.volatile_data.last_seen = 0;
                                device_info.volatile_data.signal_strength = 0;
                                device_info.volatile_data.hops = 0;
                                device_info.volatile_data.battery_level = 255; // Unknown
                                device_info.volatile_data.unread_messages = 0;
                            }

                            // Call the callback - if it returns false, stop enumeration
                            if (!callback(&device_info, user_data))
                            {
                                break;
                            }
                        }
                    }
                }
            }
        }
        closedir(dir);
    }
    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

uint8_t flood_rssi_to_percentage(int8_t rssi)
{
    // RSSI range: -100 dBm (weak) to -30 dBm (strong)
    // Convert to percentage: 0% = -100 dBm, 100% = -30 dBm
    if (rssi >= -40)
    {
        return 100;
    }
    else if (rssi <= -90)
    {
        return 0;
    }
    else
    {
        // Linear conversion: percentage = (rssi + 100) * 100 / 70
        return (uint8_t)((int16_t)(rssi + 90) * 100 / 50);
    }
}

/* Context Management */
const char* flood_get_context_path(void) { return s_context_path; }

static esp_err_t create_device_directory(const uint8_t* mac)
{
    char device_path[PATH_BUF_SIZE];
    if (get_device_path(mac, device_path) != ESP_OK)
    {
        return ESP_FAIL;
    }
    // Create directory if it doesn't exist
    struct stat st = {0};
    if (stat(device_path, &st) == -1)
    {
        if (mkdir(device_path, 0755) == -1)
        {
            ESP_LOGE(TAG, "Failed to create device directory: %s", device_path);
            return ESP_FAIL;
        }
    }

    return ESP_OK;
}

/* Persistent Device Management Functions */

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_save_device_persistent_internal(const mesh_device_persistent_t* device)
{
    if (device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Create device directory
    esp_err_t ret = create_device_directory(device->mac);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Get meta file path
    char meta_path[PATH_BUF_SIZE];
    ret = get_device_meta_path(device->mac, meta_path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Write device metadata to binary file
    FILE* file = fopen(meta_path, "wb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open meta file for writing: %s", meta_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(device, sizeof(mesh_device_persistent_t), 1, file);
    fclose(file);

    if (written != 1)
    {
        ESP_LOGE(TAG, "Failed to write device metadata to file: %s", meta_path);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Saved device metadata to: %s", meta_path);
    return ESP_OK;
}

esp_err_t flood_save_device_persistent(const mesh_device_persistent_t* device)
{
    if (device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_save_device_persistent_internal(device);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_load_device_persistent_from_meta_internal(const char* meta_path, mesh_device_persistent_t* device)
{
    if (meta_path == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Check if file exists
    struct stat st;
    if (stat(meta_path, &st) != 0)
    {
        ESP_LOGD(TAG, "Device metadata file not found: %s", meta_path);
        return ESP_ERR_NOT_FOUND;
    }

    // Read device metadata from binary file
    FILE* file = fopen(meta_path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open meta file for reading: %s", meta_path);
        return ESP_FAIL;
    }

    size_t read = fread(device, sizeof(mesh_device_persistent_t), 1, file);
    fclose(file);

    if (read != 1)
    {
        ESP_LOGE(TAG, "Failed to read device metadata from file: %s", meta_path);
        return ESP_FAIL;
    }
    if (device->magic != MESH_MAGIC_NUMBER)
    {
        ESP_LOGE(TAG, "Invalid device metadata magic number: %08X", device->magic);
        return ESP_FAIL;
    }
    if (device->version != MESH_PERSISTENT_VERSION)
    {
        ESP_LOGE(TAG, "Invalid device metadata version: %d", device->version);
        return ESP_FAIL;
    }

    ESP_LOGD(TAG, "Loaded device metadata from: %s", meta_path);
    return ESP_OK;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_load_device_persistent_internal(const uint8_t* mac, mesh_device_persistent_t* device)
{
    if (mac == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Get meta file path
    char meta_path[PATH_BUF_SIZE];
    esp_err_t ret = get_device_meta_path(mac, meta_path);
    if (ret != ESP_OK)
    {
        return ESP_FAIL;
    }
    return flood_load_device_persistent_from_meta_internal(meta_path, device);
}

esp_err_t flood_load_device_persistent(const uint8_t* mac, mesh_device_persistent_t* device)
{
    if (mac == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_load_device_persistent_internal(mac, device);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_load_device_persistent_from_meta(const char* meta_path, mesh_device_persistent_t* device)
{
    if (meta_path == NULL || device == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_load_device_persistent_from_meta_internal(meta_path, device);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

/* Volatile Device Management Functions */

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_update_device_volatile_internal(const uint8_t* mac, const mesh_device_volatile_t* volatile_data)
{
    if (mac == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Search for existing node
    volatile_device_node_t* current = s_volatile_devices_head;
    while (current != NULL)
    {
        if (memcmp(current->data.mac, mac, 6) == 0)
        {
            // Found existing device, update data
            memcpy(&current->data, volatile_data, sizeof(mesh_device_volatile_t));
            return ESP_OK;
        }
        current = current->next;
    }

    // Device not found, create new node
    volatile_device_node_t* new_node = (volatile_device_node_t*)malloc(sizeof(volatile_device_node_t));
    if (new_node == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for volatile device");
        return ESP_ERR_NO_MEM;
    }

    // Initialize new node
    memcpy(&new_node->data, volatile_data, sizeof(mesh_device_volatile_t));
    new_node->next = s_volatile_devices_head;
    s_volatile_devices_head = new_node;
    s_volatile_device_count++;

    ESP_LOGD(TAG, "Added volatile device " MACSTR " (total: %lu)", MAC2STR(mac), s_volatile_device_count);
    return ESP_OK;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_get_device_volatile_internal(const uint8_t* mac, mesh_device_volatile_t* volatile_data)
{
    if (mac == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Search for device in linked list
    volatile_device_node_t* current = s_volatile_devices_head;
    while (current != NULL)
    {
        if (memcmp(current->data.mac, mac, 6) == 0)
        {
            // Found device, copy data
            memcpy(volatile_data, &current->data, sizeof(mesh_device_volatile_t));
            return ESP_OK;
        }
        current = current->next;
    }

    return ESP_ERR_NOT_FOUND;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_remove_device_volatile_internal(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    volatile_device_node_t* current = s_volatile_devices_head;
    volatile_device_node_t* prev = NULL;

    while (current != NULL)
    {
        if (memcmp(current->data.mac, mac, 6) == 0)
        {
            // Found device, remove from list
            if (prev == NULL)
            {
                // Removing head node
                s_volatile_devices_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }

            free(current);
            s_volatile_device_count--;
            ESP_LOGD(TAG, "Removed volatile device " MACSTR " (remaining: %lu)", MAC2STR(mac), s_volatile_device_count);
            return ESP_OK;
        }
        prev = current;
        current = current->next;
    }

    return ESP_ERR_NOT_FOUND;
}

// Cleanup all volatile devices
static void flood_cleanup_volatile_devices(void)
{
    volatile_device_node_t* current = s_volatile_devices_head;
    volatile_device_node_t* next;

    while (current != NULL)
    {
        next = current->next;
        free(current);
        current = next;
    }

    s_volatile_devices_head = NULL;
    s_volatile_device_count = 0;
    ESP_LOGI(TAG, "Cleaned up all volatile devices");
}

/* Channel Volatile Data Management */

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_update_channel_volatile_internal(const char* channel_name, const mesh_channel_volatile_t* volatile_data)
{
    if (channel_name == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Search for existing node
    volatile_channel_node_t* current = s_volatile_channels_head;
    while (current != NULL)
    {
        if (strcmp((char*)current->data.channel_name, channel_name) == 0)
        {
            // Found existing channel, update data
            memcpy(&current->data, volatile_data, sizeof(mesh_channel_volatile_t));
            ESP_LOGD(TAG, "Updated volatile channel: %s", channel_name);
            return ESP_OK;
        }
        current = current->next;
    }

    // Channel not found, create new node
    volatile_channel_node_t* new_node = (volatile_channel_node_t*)malloc(sizeof(volatile_channel_node_t));
    if (new_node == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for volatile channel");
        return ESP_ERR_NO_MEM;
    }

    // Initialize new node
    memcpy(&new_node->data, volatile_data, sizeof(mesh_channel_volatile_t));
    new_node->next = s_volatile_channels_head;
    s_volatile_channels_head = new_node;
    s_volatile_channel_count++;

    ESP_LOGD(TAG, "Added volatile channel: %s (total: %lu)", channel_name, s_volatile_channel_count);
    return ESP_OK;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_get_channel_volatile_internal(const char* channel_name, mesh_channel_volatile_t* volatile_data)
{
    if (channel_name == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Search for channel in linked list
    volatile_channel_node_t* current = s_volatile_channels_head;
    while (current != NULL)
    {
        if (strcmp((char*)current->data.channel_name, channel_name) == 0)
        {
            // Found channel, copy data
            memcpy(volatile_data, &current->data, sizeof(mesh_channel_volatile_t));
            return ESP_OK;
        }
        current = current->next;
    }
    // channel not found, create new node
    volatile_channel_node_t* new_node = (volatile_channel_node_t*)malloc(sizeof(volatile_channel_node_t));
    if (new_node == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for volatile channel");
        return ESP_ERR_NO_MEM;
    }
    strncpy((char*)new_node->data.channel_name, channel_name, MESH_MAX_NAME_LENGTH);
    new_node->data.last_seen = 0;
    new_node->data.unread_messages = 0;
    new_node->next = s_volatile_channels_head;
    s_volatile_channels_head = new_node;
    s_volatile_channel_count++;
    ESP_LOGD(TAG, "Added volatile channel: %s (total: %lu)", channel_name, s_volatile_channel_count);
    return ESP_OK;
}

// Internal function to remove channel from volatile list
static esp_err_t flood_remove_channel_volatile_internal(const char* channel_name)
{
    if (channel_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    volatile_channel_node_t* current = s_volatile_channels_head;
    volatile_channel_node_t* prev = NULL;

    while (current != NULL)
    {
        if (strcmp((char*)current->data.channel_name, channel_name) == 0)
        {
            // Found channel to remove
            if (prev == NULL)
            {
                // Removing head node
                s_volatile_channels_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }
            free(current);
            s_volatile_channel_count--;
            ESP_LOGD(TAG, "Removed volatile channel: %s", channel_name);
            return ESP_OK;
        }
        prev = current;
        current = current->next;
    }

    return ESP_ERR_NOT_FOUND;
}

// Cleanup all volatile channels
static void flood_cleanup_volatile_channels(void)
{
    volatile_channel_node_t* current = s_volatile_channels_head;
    volatile_channel_node_t* next;

    while (current != NULL)
    {
        next = current->next;
        free(current);
        current = next;
    }

    s_volatile_channels_head = NULL;
    s_volatile_channel_count = 0;
    ESP_LOGI(TAG, "Cleaned up all volatile channels");
}

/* Shared Message File Operations */

/**
 * @brief Load messages from a file (internal shared implementation)
 *
 * Generic function to load message records from any messages file.
 * Used by both device and channel message loading functions.
 *
 * @param messages_file_path Full path to messages file
 * @param start Starting record index
 * @param count Number of records to load
 * @param records Buffer to store loaded records
 * @param loaded Pointer to store actual number of records loaded
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t flood_load_messages_from_file_internal(
    const char* messages_file_path, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded)
{
    if (messages_file_path == NULL || records == NULL || loaded == NULL || count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *loaded = 0;

    // Check if file exists
    struct stat st;
    if (stat(messages_file_path, &st) != 0)
    {
        // No file = no messages
        return ESP_OK;
    }

    // Calculate total number of records
    uint32_t total_records = st.st_size / sizeof(message_record_t);
    if (start >= total_records)
    {
        // Start position is beyond available records
        return ESP_OK;
    }

    // Calculate how many records we can actually load
    uint32_t available = total_records - start;
    uint32_t to_load = (count < available) ? count : available;

    // Open file and seek to start position
    FILE* file = fopen(messages_file_path, "rb");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open messages file: %s", messages_file_path);
        return ESP_FAIL;
    }

    // Seek to start position
    if (fseek(file, start * sizeof(message_record_t), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek in messages file");
        fclose(file);
        return ESP_FAIL;
    }

    // Read records
    size_t read = fread(records, sizeof(message_record_t), to_load, file);
    fclose(file);

    if (read != to_load)
    {
        ESP_LOGW(TAG, "Read %zu records instead of %lu", read, to_load);
    }

    *loaded = (uint32_t)read;

    ESP_LOGD(TAG, "Loaded %lu/%lu message records (start=%lu)", *loaded, count, start);
    return ESP_OK;
}

/**
 * @brief Clear messages file (internal shared implementation)
 *
 * Generic function to delete a messages file.
 * Used by both device and channel clear functions.
 *
 * @param messages_file_path Full path to messages file to delete
 * @return ESP_OK on success (even if file doesn't exist)
 */
static esp_err_t flood_clear_messages_file_internal(const char* messages_file_path)
{
    if (messages_file_path == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Delete messages file
    if (unlink(messages_file_path) != 0)
    {
        // File might not exist, which is OK
        ESP_LOGD(TAG, "Messages file not found or already deleted: %s", messages_file_path);
    }

    return ESP_OK;
}

/**
 * @brief Save message record to file (internal shared implementation)
 *
 * Generic function to append a message record to any messages file.
 * Used by both device and channel message saving functions.
 *
 * @param messages_file_path Full path to messages file
 * @param record Message record to save
 * @return ESP_OK on success, error code otherwise
 */
static esp_err_t flood_save_message_to_file_internal(const char* messages_file_path, const message_record_t* record)
{
    if (messages_file_path == NULL || record == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // Append record to file
    FILE* file = fopen(messages_file_path, "ab");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open messages file: %s", messages_file_path);
        return ESP_FAIL;
    }

    size_t written = fwrite(record, sizeof(message_record_t), 1, file);
    fclose(file);

    if (written != 1)
    {
        ESP_LOGE(TAG, "Failed to write message record");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t flood_update_device_volatile(const uint8_t* mac, const mesh_device_volatile_t* volatile_data)
{
    if (mac == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_update_device_volatile_internal(mac, volatile_data);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_get_device_volatile(const uint8_t* mac, mesh_device_volatile_t* volatile_data)
{
    if (mac == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_get_device_volatile_internal(mac, volatile_data);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_update_channel_volatile(const char* channel_name, const mesh_channel_volatile_t* volatile_data)
{
    if (channel_name == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_update_channel_volatile_internal(channel_name, volatile_data);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_get_channel_volatile(const char* channel_name, mesh_channel_volatile_t* volatile_data)
{
    if (channel_name == NULL || volatile_data == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_flood_mutex, portMAX_DELAY);
    esp_err_t ret = flood_get_channel_volatile_internal(channel_name, volatile_data);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

uint32_t flood_get_volatile_device_count(void)
{
    uint32_t count = 0;
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) == pdTRUE)
    {
        count = s_volatile_device_count;
        xSemaphoreGive(s_flood_mutex);
    }
    return count;
}

int flood_get_device_color_by_id(uint16_t device_id)
{
    // Combine last 2 MAC bytes to get a unique seed
    uint16_t seed = device_id;
    seed = seed * seed ^ seed;

    // Generate vibrant colors that are visible on dark background
    // Use the seed to select from predefined good color palette
    const int colors[] = {
        0x0000, // Black
        0x000F, // Navy
        0x03E0, // Dark Green
        0x03EF, // Dark Cyan
        0x7800, // Maroon
        0x780F, // Purple
        0x7BE0, // Olive
        0x001F, // Blue
        0x07E0, // Green
        0x07FF, // Cyan
        0xF800, // Red
        0xF81F, // Magenta
        0xFFE0, // Yellow
        0xFFFF, // White
        0xFDA0, // Orange
        0xB7E0, // Green Yellow
        0xFE19, // Pink
        0x9A60, // Brown
        0xFEA0, // Gold
        0xC618, // Silver
        0x867D, // Sky Blue
        0x915C  // Violet
    };

    return colors[seed % (sizeof(colors) / sizeof(colors[0]))];
}

int flood_get_device_text_color_by_id(uint16_t device_id)
{
    // Combine last 2 MAC bytes to get a unique seed
    uint16_t seed = device_id;
    seed = seed * seed ^ seed;

    const int colors[] = {
        0xFFFF, // White
        0x0000, // Black
        0xFFFF, // White
        0xFFFF, // White
        0xFFFF, // White
        0xFFFF, // White
        0x0000, // Black
        0xFFFF, // White
        0x0000, // White
        0x0000, // Black
        0xFFFF, // White
        0x0000, // Black
        0x0000, // Black
        0xFFFF, // White
        0x0000, // Black
        0x0000, // Black
        0x0000, // Black
        0xFFFF, // White
        0x0000, // Black
        0x0000, // Black
        0x0000, // Black
        0xFFFF  // White
    };

    return colors[seed % (sizeof(colors) / sizeof(colors[0]))];
}

int flood_get_device_color(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return 0xFFFF; // White as default
    }

    // Use last 2 bytes of MAC address for color generation
    uint8_t mac_byte1 = mac[4];
    uint8_t mac_byte2 = mac[5];
    return flood_get_device_color_by_id((mac_byte1 << 8) | mac_byte2);
}

int flood_get_device_text_color(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return 0x0000; // Black as default
    }

    // Use last 2 bytes of MAC address for color generation
    uint8_t mac_byte1 = mac[4];
    uint8_t mac_byte2 = mac[5];
    return flood_get_device_text_color_by_id((mac_byte1 << 8) | mac_byte2);
}

uint16_t flood_get_device_id(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return 0xFFFF;
    }
    return (mac[4] << 8) | mac[5];
}

void flood_get_our_mac(uint8_t* mac) { esp_read_mac(mac, ESP_MAC_WIFI_STA); }

uint16_t flood_get_our_device_id(void) { return flood_get_device_id(s_our_mac); }

/* Private Message Storage API */

// Internal function without mutex (assumes mutex is already held)
static int32_t flood_save_message_internal(const uint8_t* mac,
                                           const uint8_t* sender_mac,
                                           uint32_t sequence,
                                           uint8_t message_type,
                                           const uint8_t* message_data,
                                           uint16_t message_len)
{
    if (mac == NULL || message_data == NULL || message_len == 0 || message_len > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG,
                 "Invalid message data: mac: " MACSTRSHORT ", sequence: *%08lx, message_type: %d, message_len: %d",
                 MAC2STR(mac),
                 sequence,
                 message_type,
                 message_len);
        return -1;
    }
    if (sender_mac == NULL)
    {
        sender_mac = mac;
    }

    // Get messages file path
    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_messages_file_path(mac, messages_file_path);
    if (ret != ESP_OK)
    {
        return -1;
    }

    ESP_LOGD(TAG, "Saving message to %s ...", messages_file_path);

    // Get current message count (file size / record size) before saving
    struct stat st;
    int32_t message_id = 0;
    if (stat(messages_file_path, &st) == 0)
    {
        message_id = st.st_size / sizeof(message_record_t);
    }
    ESP_LOGD(TAG, "Message count: %ld", message_id);

    // Create message record
    message_record_t record;
    memset(&record, 0, sizeof(message_record_t));
    memcpy(record.sender_mac, sender_mac, 6);
    record.sequence = sequence;
    record.timestamp = flood_get_timestamp();
    record.status = MESSAGE_STATUS_SENT;
    record.message_type = message_type;
    record.message_length = message_len;
    memcpy(record.message_data, message_data, message_len);

    // Use shared implementation to save message
    ret = flood_save_message_to_file_internal(messages_file_path, &record);
    if (ret != ESP_OK)
    {
        return -1;
    }

    ESP_LOGD(TAG, "Saved message #%ld " MACSTRSHORT " *%08lX", message_id, MAC2STR(mac), sequence);
    return message_id;
}

int32_t flood_save_private_message(const uint8_t* mac,
                                   const uint8_t* sender_mac,
                                   uint32_t sequence,
                                   uint8_t message_type,
                                   const uint8_t* message_data,
                                   uint16_t message_len)
{
    if (mac == NULL || message_data == NULL || message_len == 0 || message_len > MESSAGE_MAX_PAYLOAD)
    {
        ESP_LOGE(TAG,
                 "Invalid message data: mac: " MACSTRSHORT ", sequence: *%08lx, message_type: %d, message_len: %d",
                 MAC2STR(mac),
                 sequence,
                 message_type,
                 message_len);
        return -1;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return -1;
    }

    int32_t message_id = flood_save_message_internal(mac, sender_mac, sequence, message_type, message_data, message_len);

    xSemaphoreGive(s_flood_mutex);
    return message_id;
}

esp_err_t flood_get_message_count(const uint8_t* mac, uint32_t* count)
{
    if (mac == NULL || count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_messages_file_path(mac, messages_file_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Check if file exists
    struct stat st;
    if (stat(messages_file_path, &st) != 0)
    {
        // No file = no messages
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    // Calculate number of records
    *count = st.st_size / sizeof(message_record_t);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t
flood_load_messages_internal(const uint8_t* mac, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded)
{
    if (mac == NULL || records == NULL || loaded == NULL || count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_messages_file_path(mac, messages_file_path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Use shared implementation
    return flood_load_messages_from_file_internal(messages_file_path, start, count, records, loaded);
}

esp_err_t flood_load_messages(const uint8_t* mac, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded)
{
    if (mac == NULL || records == NULL || loaded == NULL || count == 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Loading messages start: %lu, count: %lu", start, count);
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = flood_load_messages_internal(mac, start, count, records, loaded);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

// Internal function without mutex (assumes mutex is already held)
static esp_err_t flood_update_private_message_status_internal(const uint8_t* mac, uint32_t message_id, uint8_t status)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_messages_file_path(mac, messages_file_path);
    if (ret != ESP_OK)
    {
        return ret;
    }

    // Check if file exists
    struct stat st;
    if (stat(messages_file_path, &st) != 0)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Check if message_id is valid
    uint32_t total_records = st.st_size / sizeof(message_record_t);
    if (message_id >= total_records)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // Open file for read/write
    FILE* file = fopen(messages_file_path, "r+b");
    if (file == NULL)
    {
        ESP_LOGE(TAG, "Failed to open messages file: %s", messages_file_path);
        return ESP_FAIL;
    }

    // Seek to message record (O(1) access)
    if (fseek(file, message_id * sizeof(message_record_t), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek to message #%lu", message_id);
        fclose(file);
        return ESP_FAIL;
    }

    // Read current record
    message_record_t record;
    if (fread(&record, sizeof(message_record_t), 1, file) != 1)
    {
        ESP_LOGE(TAG, "Failed to read message record");
        fclose(file);
        return ESP_FAIL;
    }

    // Update status
    record.status = status;

    // Seek back and write
    if (fseek(file, message_id * sizeof(message_record_t), SEEK_SET) != 0)
    {
        ESP_LOGE(TAG, "Failed to seek for write");
        fclose(file);
        return ESP_FAIL;
    }

    if (fwrite(&record, sizeof(message_record_t), 1, file) != 1)
    {
        ESP_LOGE(TAG, "Failed to write updated record");
        fclose(file);
        return ESP_FAIL;
    }

    fclose(file);

    ESP_LOGI(TAG, "Updated message #%lu status to 0x%02X", message_id, status);
    // call callback if set
    if (s_message_status_callback != NULL)
    {
        s_message_status_callback(mac, message_id, status, s_message_status_callback_user_data);
    }
    return ESP_OK;
}

esp_err_t flood_update_message_status(const uint8_t* mac, uint32_t message_id, uint8_t status)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = flood_update_private_message_status_internal(mac, message_id, status);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_clear_chat(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_messages_file_path(mac, messages_file_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Use shared implementation
    ret = flood_clear_messages_file_internal(messages_file_path);

    ESP_LOGI(TAG, "Cleared chat for " MACSTR, MAC2STR(mac));
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_private_mark_read(const uint8_t* mac)
{
    if (mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Marking as read for " MACSTR, MAC2STR(mac));
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    // Search for existing node
    volatile_device_node_t* current = s_volatile_devices_head;
    while (current != NULL)
    {
        if (memcmp(current->data.mac, mac, 6) == 0)
        {
            // Found existing device, update data
            current->data.unread_messages = 0;
            ret = ESP_OK;
            break;
        }
        current = current->next;
    }
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_channel_mark_read(const char* channel_name)
{
    if (channel_name == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGD(TAG, "Marking channel as read: %s", channel_name);
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }
    esp_err_t ret = ESP_ERR_NOT_FOUND;
    // Search for existing channel node
    volatile_channel_node_t* current = s_volatile_channels_head;
    while (current != NULL)
    {
        if (strcmp((char*)current->data.channel_name, channel_name) == 0)
        {
            // Found existing channel, reset unread messages
            current->data.unread_messages = 0;
            ret = ESP_OK;
            break;
        }
        current = current->next;
    }
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

/* Waiting ACK Management Functions */

static esp_err_t flood_waiting_ack_add(const uint8_t* packet, uint16_t packet_length)
{
    if (packet == NULL || packet_length == 0 || packet_length > ESP_NOW_MAX_DATA_LEN)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Create new node
    waiting_ack_node_t* new_node = (waiting_ack_node_t*)malloc(sizeof(waiting_ack_node_t));
    if (new_node == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for waiting ACK node");
        xSemaphoreGive(s_flood_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Initialize node - store the entire packet
    memcpy(new_node->packet, packet, packet_length);
    new_node->packet_length = packet_length;
    new_node->timestamp = flood_get_timestamp();
    new_node->try_num = 0;
    new_node->next = s_waiting_ack_head;

    // Add to head of list
    s_waiting_ack_head = new_node;
    s_waiting_ack_count++;

    const mesh_packet_header_t* header = (const mesh_packet_header_t*)packet;
    ESP_LOGD(TAG, "Added message *%08x to waiting ACK list (total: %lu)", header->sequence, s_waiting_ack_count);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

static esp_err_t flood_waiting_ack_remove(uint32_t sequence, const uint8_t* dest_mac, int32_t* out_message_id)
{
    if (dest_mac == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    waiting_ack_node_t* current = s_waiting_ack_head;
    waiting_ack_node_t* prev = NULL;

    while (current != NULL)
    {
        const mesh_packet_header_t* header = (const mesh_packet_header_t*)current->packet;
        if (header->sequence == sequence && memcmp(header->dest_mac, dest_mac, 6) == 0)
        {
            // Extract message_id if requested (only for MESSAGE and PRIVATE packets)
            if (out_message_id != NULL)
            {
                *out_message_id = -1;
                if (header->type == MESH_PACKET_TYPE_MESSAGE)
                {
                    mesh_message_packet_t* msg_packet = (mesh_message_packet_t*)current->packet;
                    *out_message_id = msg_packet->message_id;
                }
                else if (header->type == MESH_PACKET_TYPE_PRIVATE)
                {
                    mesh_private_packet_t* priv_packet = (mesh_private_packet_t*)current->packet;
                    *out_message_id = priv_packet->message_id;
                }
            }

            // Remove the node
            if (prev == NULL)
            {
                // Removing head node
                s_waiting_ack_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }

            free(current);
            s_waiting_ack_count--;

            ESP_LOGD(TAG, "Removed message *%08x from waiting ACK list (remaining: %lu)", sequence, s_waiting_ack_count);

            xSemaphoreGive(s_flood_mutex);
            return ESP_OK;
        }

        prev = current;
        current = current->next;
    }

    xSemaphoreGive(s_flood_mutex);
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t flood_waiting_ack_implicit(uint32_t sequence)
{
    if (xSemaphoreTake(s_flood_mutex, pdMS_TO_TICKS(100)) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    waiting_ack_node_t* current = s_waiting_ack_head;
    waiting_ack_node_t* prev = NULL;

    while (current != NULL)
    {
        const mesh_packet_header_t* header = (const mesh_packet_header_t*)current->packet;
        if (header->sequence == sequence)
        {
            int32_t message_id = -1;
            if (header->type == MESH_PACKET_TYPE_MESSAGE)
            {
                mesh_message_packet_t* msg = (mesh_message_packet_t*)current->packet;
                message_id = msg->message_id;
                if (message_id >= 0)
                {
                    flood_update_channel_message_status_internal(
                        (char*)msg->channel_name, message_id, MESSAGE_STATUS_DELIVERED);
                }
            }
            else if (header->type == MESH_PACKET_TYPE_PRIVATE)
            {
                mesh_private_packet_t* priv = (mesh_private_packet_t*)current->packet;
                message_id = priv->message_id;
                if (message_id >= 0)
                {
                    flood_update_private_message_status_internal(
                        header->dest_mac, message_id, MESSAGE_STATUS_DELIVERED);
                }
            }

            if (prev == NULL)
            {
                s_waiting_ack_head = current->next;
            }
            else
            {
                prev->next = current->next;
            }

            free(current);
            s_waiting_ack_count--;

            if (s_message_status_callback != NULL)
            {
                s_message_status_callback(
                    header->dest_mac, message_id, MESSAGE_STATUS_DELIVERED, s_message_status_callback_user_data);
            }

            xSemaphoreGive(s_flood_mutex);
            return ESP_OK;
        }

        prev = current;
        current = current->next;
    }

    xSemaphoreGive(s_flood_mutex);
    return ESP_ERR_NOT_FOUND;
}

static void flood_waiting_ack_check_timeouts(void)
{
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    uint32_t now = flood_get_timestamp();
    waiting_ack_node_t* current = s_waiting_ack_head;
    waiting_ack_node_t* prev = NULL;

    while (current != NULL)
    {
        uint32_t elapsed = now - current->timestamp;
        mesh_packet_header_t* header = (mesh_packet_header_t*)current->packet;

        if (elapsed >= MESH_ACK_TIMEOUT)
        {
            // Timeout occurred
            if (current->try_num < MESH_RESEND_MAX_TRIES)
            {
                // Retry sending the message
                current->try_num++;
                current->timestamp = now;

                ESP_LOGW(TAG,
                         "Retrying message *%08x to " MACSTR " (try %d/%d)",
                         header->sequence,
                         MAC2STR(header->dest_mac),
                         current->try_num,
                         MESH_RESEND_MAX_TRIES);

                // Update header flags to add RETRY flag and reset hops/ttl
                header->flags |= MESH_FLAG_RETRY;
                header->hops = 0;
                header->ttl = s_max_ttl;

                // Enqueue the stored packet for resending
                flood_enqueue_packet(current->packet, current->packet_length);

                // Move to next node
                prev = current;
                current = current->next;
            }
            else
            {
                // Max retries reached, mark as failed and remove
                ESP_LOGE(TAG,
                         "Message *%08x to " MACSTR " failed after %d tries",
                         header->sequence,
                         MAC2STR(header->dest_mac),
                         MESH_RESEND_MAX_TRIES);

                // Extract message_id to mark as failed (only for MESSAGE and PRIVATE packets)
                int32_t message_id = -1;
                if (header->type == MESH_PACKET_TYPE_MESSAGE)
                {
                    mesh_message_packet_t* msg_packet = (mesh_message_packet_t*)current->packet;
                    message_id = msg_packet->message_id;
                    const char* channel_name = (char*)msg_packet->channel_name;
                    // Mark message as delivery failed if we have a message_id
                    if (message_id >= 0)
                    {
                        flood_update_channel_message_status_internal(channel_name, message_id, MESSAGE_STATUS_DELIVERY_FAILED);
                    }
                }
                else if (header->type == MESH_PACKET_TYPE_PRIVATE)
                {
                    mesh_private_packet_t* priv_packet = (mesh_private_packet_t*)current->packet;
                    message_id = priv_packet->message_id;
                    // Mark message as delivery failed if we have a message_id
                    if (message_id >= 0)
                    {
                        flood_update_private_message_status_internal(header->dest_mac,
                                                                     message_id,
                                                                     MESSAGE_STATUS_DELIVERY_FAILED);
                    }
                }

                // Remove from list
                waiting_ack_node_t* to_remove = current;
                if (prev == NULL)
                {
                    // Removing head node
                    s_waiting_ack_head = current->next;
                    current = s_waiting_ack_head;
                }
                else
                {
                    prev->next = current->next;
                    current = current->next;
                }

                free(to_remove);
                s_waiting_ack_count--;
            }
        }
        else
        {
            // Not timed out yet, move to next
            prev = current;
            current = current->next;
        }
    }

    xSemaphoreGive(s_flood_mutex);
}

static void flood_waiting_ack_cleanup(void)
{
    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return;
    }

    waiting_ack_node_t* current = s_waiting_ack_head;
    waiting_ack_node_t* next;

    while (current != NULL)
    {
        next = current->next;
        free(current);
        current = next;
    }

    s_waiting_ack_head = NULL;
    s_waiting_ack_count = 0;

    xSemaphoreGive(s_flood_mutex);
    ESP_LOGI(TAG, "Cleaned up all waiting ACK entries");
}

/* Public Channel API Functions */

esp_err_t flood_add_channel(const char* channel_name)
{
    if (!flood_channel_name_valid(channel_name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // add meta file
    mesh_channel_persistent_t channel_persistent;
    memset(&channel_persistent, 0, sizeof(mesh_channel_persistent_t));
    strncpy((char*)channel_persistent.channel_name, channel_name, MESH_MAX_NAME_LENGTH);
    channel_persistent.magic = MESH_MAGIC_NUMBER;
    channel_persistent.version = MESH_PERSISTENT_VERSION;
    esp_err_t ret = flood_save_channel_persistent_internal(&channel_persistent);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }
    // add channel to volatile list
    mesh_channel_volatile_t channel_volatile;
    memset(&channel_volatile, 0, sizeof(mesh_channel_volatile_t));
    strncpy((char*)channel_volatile.channel_name, channel_name, MESH_MAX_NAME_LENGTH);
    channel_volatile.last_seen = 0;
    channel_volatile.unread_messages = 0;
    ret = flood_update_channel_volatile_internal(channel_name, &channel_volatile);
    xSemaphoreGive(s_flood_mutex);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "Added channel: %s", channel_name);
    }
    return ret;
}

esp_err_t flood_remove_channel(const char* channel_name)
{
    if (!flood_channel_name_valid(channel_name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    // Get channel path
    char channel_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_path(channel_name, channel_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Delete messages file if exists
    char messages_file_path[PATH_BUF_SIZE];
    if (get_channel_messages_file_path(channel_name, messages_file_path) == ESP_OK)
    {
        unlink(messages_file_path);
    }

    // Delete meta file if exists
    char meta_path[PATH_BUF_SIZE];
    if (get_channel_meta_path(channel_name, meta_path) == ESP_OK)
    {
        unlink(meta_path);
    }

    // Remove channel directory
    if (rmdir(channel_path) != 0)
    {
        ESP_LOGW(TAG, "Failed to remove channel directory: %s", channel_path);
    }
    // Remove channel from volatile list
    ret = flood_remove_channel_volatile_internal(channel_name);

    ESP_LOGI(TAG, "Removed channel: %s", channel_name);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_enum_channels(flood_channel_enum_callback_t callback, void* user_data)
{
    if (callback == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char channels_path[PATH_BUF_SIZE];
    if (get_channels_path(channels_path) != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ESP_FAIL;
    }

    DIR* dir = opendir(channels_path);
    if (dir != NULL)
    {
        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL)
        {
            // Skip . and .. entries
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;

            char channel_path[PATH_BUF_SIZE];
            strncpy(channel_path, channels_path, PATH_BUF_SIZE - 1);
            if (path_join(channel_path, entry->d_name))
            {
                struct stat st;
                if (stat(channel_path, &st) == 0 && S_ISDIR(st.st_mode))
                {
                    // Load combined channel info (persistent + volatile)
                    mesh_channel_info_t channel_info;
                    memset(&channel_info, 0, sizeof(mesh_channel_info_t));

                    esp_err_t ret = flood_find_channel_internal(entry->d_name, &channel_info);
                    if (ret != ESP_OK)
                    {
                        // Channel not found or error loading, skip
                        continue;
                    }

                    // Call the callback with full channel info - if it returns false, stop enumeration
                    if (!callback(&channel_info, user_data))
                    {
                        break;
                    }
                }
            }
        }
        closedir(dir);
    }

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

esp_err_t flood_get_channel_message_count(const char* channel_name, uint32_t* count)
{
    if (!flood_channel_name_valid(channel_name) || count == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    *count = 0;

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_messages_file_path(channel_name, messages_file_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Check if file exists
    struct stat st;
    if (stat(messages_file_path, &st) != 0)
    {
        // No file = no messages
        xSemaphoreGive(s_flood_mutex);
        return ESP_OK;
    }

    // Calculate number of records
    *count = st.st_size / sizeof(message_record_t);

    xSemaphoreGive(s_flood_mutex);
    return ESP_OK;
}

esp_err_t flood_load_channel_messages(
    const char* channel_name, uint32_t start, uint32_t count, message_record_t* records, uint32_t* loaded)
{
    if (!flood_channel_name_valid(channel_name) || records == NULL || loaded == NULL || count == 0)
    {
        ESP_LOGE(TAG, "Invalid parameters for loading channel messages");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_messages_file_path(channel_name, messages_file_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Use shared implementation
    ret = flood_load_messages_from_file_internal(messages_file_path, start, count, records, loaded);

    ESP_LOGD(TAG, "Loaded %lu channel message records from %s", *loaded, channel_name);

    xSemaphoreGive(s_flood_mutex);
    return ret;
}

esp_err_t flood_clear_channel(const char* channel_name)
{
    if (!flood_channel_name_valid(channel_name))
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    char messages_file_path[PATH_BUF_SIZE];
    esp_err_t ret = get_channel_messages_file_path(channel_name, messages_file_path);
    if (ret != ESP_OK)
    {
        xSemaphoreGive(s_flood_mutex);
        return ret;
    }

    // Use shared implementation
    ret = flood_clear_messages_file_internal(messages_file_path);

    ESP_LOGI(TAG, "Cleared channel: %s", channel_name);
    xSemaphoreGive(s_flood_mutex);
    return ret;
}

int32_t flood_save_channel_message(const char* channel_name,
                                   const uint8_t* sender_mac,
                                   uint32_t sequence,
                                   uint8_t status,
                                   uint8_t message_type,
                                   const uint8_t* message_data,
                                   uint16_t message_length)
{
    if (!flood_channel_name_valid(channel_name))
    {
        return -1;
    }

    if (!s_flood_initialized)
    {
        return -1;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return -1;
    }

    // Call internal function
    int32_t message_id = flood_save_channel_message_internal(channel_name,
                                                             sender_mac,
                                                             sequence,
                                                             status,
                                                             message_type,
                                                             message_data,
                                                             message_length);

    xSemaphoreGive(s_flood_mutex);
    return message_id;
}

esp_err_t flood_find_channel(const char* channel_name, mesh_channel_info_t* channel_info)
{
    if (channel_name == NULL || channel_info == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_flood_initialized)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(s_flood_mutex, portMAX_DELAY) != pdTRUE)
    {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = flood_find_channel_internal(channel_name, channel_info);

    xSemaphoreGive(s_flood_mutex);
    return ret;
}