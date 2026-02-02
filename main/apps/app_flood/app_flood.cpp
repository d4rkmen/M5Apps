#include "app_flood.h"
#include "esp_log.h"
#include "esp_random.h"
#include <algorithm>
#include "flood.h"
#include "apps/utils/ui/dialog.h"
#include "common_define.h"

#define SCROLL_BAR_WIDTH 4
#define LIST_HEADER_HEIGHT 0
#define CHAT_HEADER_HEIGHT 14
#define LIST_ITEM_HEIGHT 14
#define CHAT_ITEM_HEIGHT 12
#define LIST_ITEM_LEFT_PADDING 4
#define LIST_ICON_WIDTH 20
#define LIST_ICON_HEIGHT 12
#define LIST_SCROLL_PAUSE 1000
#define LIST_SCROLL_SPEED 25
#define LIST_MAX_VISIBLE_ITEMS 7
#define LIST_MAX_DISPLAY_CHARS 12
#define CHAT_MAX_VISIBLE_ITEMS 7
#define KEY_HOLD_MS 500
#define KEY_REPEAT_MS 100
#define APP_RENDER_INTERVAL_MS 1000
#define SCROLLBAR_MIN_HEIGHT 10

using namespace MOONCAKE::APPS;

static const char* TAG = "APP_FLOOD";
static const char* FLOOD_CONTEXT_PATH = "/sdcard/flood";

static const char* HINT_DEVICES = "[Fn] [^][v] [<][>] [C][S] [ENTER][DEL] [ESC]";
static const char* HINT_DEVICES_FN = "[UP] [DOWN]";
static const char* HINT_CHAT = "[Fn] [^][v] [<][>] [ENTER][DEL] [ESC]";
static const char* HINT_CHAT_FN = "[UP] [DOWN] [<] [>]";

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;

static const char* SORT_MODE_NAMES[] = {"role", "name", "signal", "battery", "hops", "last seen"};
static int SORT_MODE_COUNT = sizeof(SORT_MODE_NAMES) / sizeof(SORT_MODE_NAMES[0]);

static void
_message_callback(const mesh_packet_header_t* header, const uint8_t* payload, uint16_t length, int8_t rssi, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        if (header->type == MESH_PACKET_TYPE_PRIVATE)
        {
            app->playMessageSound();
            app->needRefresh();
            app->wakeUpScreen();
        }
        else if (header->type == MESH_PACKET_TYPE_MESSAGE)
        {
            // check channel added
            mesh_channel_info_t channel_info = {0};
            mesh_message_packet_t* message_packet = (mesh_message_packet_t*)payload;
            esp_err_t ret = flood_find_channel((const char*)message_packet->channel_name, &channel_info);
            if (ret == ESP_OK)
            {
                app->playMessageSound();
                app->needRefresh();
                app->wakeUpScreen();
            }
        }
        else if (header->type == MESH_PACKET_TYPE_HELLO)
        {
            app->needRefresh();
        }
    }
}

static void _message_status_callback(const uint8_t* mac, int32_t message_id, uint8_t status, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        app->needRefresh();
    }
}

static void _device_callback(const mesh_device_info_t* device, bool added, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        app->needRefresh();
    }
}

static void _sent_packet_callback(const uint8_t* data, uint16_t length, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        // app->needRefresh();
        // blick with yellow led
        // rgb_led_blink_once({244, 255, 60}, 50);
        app->ledYellowBlink();
    }
}

static void _received_packet_callback(const uint8_t* data, uint16_t length, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        // app->needRefresh();
        // blick with green blue
        app->ledBlueBlink();
    }
}

static bool _sort_last_seen(const DeviceItem& a, const DeviceItem& b) { return a.last_seen > b.last_seen; }
static bool _sort_signal(const DeviceItem& a, const DeviceItem& b) { return a.signal_strength > b.signal_strength; }
static bool _sort_name(const DeviceItem& a, const DeviceItem& b) { return a.name < b.name; }
static bool _sort_role(const DeviceItem& a, const DeviceItem& b) { return a.role > b.role; }
static bool _sort_hops(const DeviceItem& a, const DeviceItem& b) { return a.hops < b.hops; }
static bool _sort_battery(const DeviceItem& a, const DeviceItem& b) { return a.battery_level > b.battery_level; }
// array of sort mode functions
static bool (*_sort_mode_funcs[])(const DeviceItem& a, const DeviceItem& b) = {
    _sort_role, _sort_name, _sort_signal, _sort_battery, _sort_hops, _sort_last_seen};

void AppFlood::onCreate()
{
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.system_bar_force_update_flag = mcAppGetDatabase()->Get("SYSTEM_BAR_FORCE_UPDATE")->value<bool*>();
    scroll_text_init_ex(&_data.name_scroll_ctx,
                        _data.hal->canvas(),
                        LIST_MAX_DISPLAY_CHARS * 6, // FONT_12 width
                        12,                         // FONT_12 height
                        LIST_SCROLL_SPEED,
                        LIST_SCROLL_PAUSE,
                        FONT_12);
    // keyboard hint text
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);

    // Mount SD card before flood init
    if (!_data.hal->sdcard()->mount(false))
    {
        UTILS::UI::show_error_dialog(_data.hal, "Error", "Plug an SD card and try again");
        destroyApp();
        return;
    }
    // deinit wifi if enabled
    _data.hal->wifi()->deinit();
    // default name
    std::string node_name = _data.hal->settings()->getString("flood", "node_name");
    if (node_name.empty() || node_name == CONFIG_FLOOD_DEVICE_NAME)
    {
        // read before flood init
        uint8_t our_mac[6];
        flood_get_our_mac(our_mac);
        node_name.append(std::format("_{:04X}", flood_get_device_id(our_mac)));
    }
    // Start flood
    if (flood_init(node_name.c_str(),
                   FLOOD_CONTEXT_PATH,
                   _data.hal->settings()->getNumber("flood", "channel"),
                   _data.hal->settings()->getNumber("flood", "max_ttl"),
                   _data.hal->settings()->getNumber("flood", "hello_interval")) != ESP_OK)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Init failed", "Check SD card and radio settings");
        destroyApp();
        return;
    }
    else
    {
        // register callbacks
        flood_register_message_callback(_message_callback, this);
        flood_register_device_callback(_device_callback, this);
        flood_register_sent_packet_callback(_sent_packet_callback, this);
        flood_register_received_packet_callback(_received_packet_callback, this);
        flood_register_message_status_callback(_message_status_callback, this);
        if (flood_start() != ESP_OK)
        {
            UTILS::UI::show_error_dialog(_data.hal, "Init failed", "Can't create task");
            destroyApp();
            return;
        }
        ESP_LOGI(TAG, "Flood component started");

        // Request system bar update to show flood status immediately
        _request_system_bar_update();
    }
// ! add test items to devices list
#if 0
    for (int i = 0; i < 20; i++)
    {
        DeviceItem di{};
        for (int j = 0; j < 6; j++)
        {
            di.mac[j] = esp_random() & 0xFF;
        }
        uint16_t short_id = flood_get_device_id(di.mac);
        di.name = std::format("Flooder_{:04X}", short_id);
        di.role = (FLOOD_DEVICE_ROLE_t)(1 + esp_random() % 3);
        di.capabilities = MESH_CAP_POWER_SAVE;
        di.last_seen = millis();
        di.battery_level = esp_random() % 100;
        di.signal_strength = esp_random() % 100;
        di.hops = esp_random() % 5;
        di.unread_messages = esp_random() % 100;
        _data.devices_test.push_back(di);
    }
#endif
    // sort mode
    std::string sort_mode = _data.hal->settings()->getString("flood", "sort_mode");
    // for loop to find sort mode index
    _data.sort_mode_index = 0;
    for (int i = 0; i < sizeof(SORT_MODE_NAMES) / sizeof(SORT_MODE_NAMES[0]); i++)
    {
        if (sort_mode == std::string(SORT_MODE_NAMES[i]))
        {
            _data.sort_mode_index = i;
            break;
        }
    }
    _data.chat_messages.reserve(CHAT_MAX_VISIBLE_ITEMS);
}

void AppFlood::onResume()
{
    ANIM_APP_OPEN();
    _data.need_render = true;
    _data.need_refresh = true;
    _data.last_render_tick = 0;
}

void AppFlood::onRunning()
{
    if (_data.hal->home_button()->is_pressed())
    {
        _data.hal->keyboard()->resetLastPressedTime();
        _data.hal->playNextSound();
        destroyApp();
        return;
    }

    // check dimmed
    if (_data.hal->keyboard()->isDimmed())
    {
        return;
    }
    // periodic render
    uint32_t now = millis();
    if ((uint32_t)(now - _data.last_render_tick) >= APP_RENDER_INTERVAL_MS)
    {
        _data.need_render = true;
        _data.last_render_tick = now;
    }

    bool updated = false;

    switch (_data.current_view)
    {
    case view_devices:
        if (_data.need_refresh)
        {
            _refresh_devices();
        }
        updated |= _render_devices();
        updated |= _render_scrolling_name();
        updated |= _render_devices_hint();
        break;
    case view_chat:
        if (_data.need_refresh)
        {
            _refresh_chat();
        }
        updated |= _render_chat();
        updated |= _render_chat_hint();
        break;
    }

    if (updated)
    {
        _data.hal->canvas_update();
    }

    switch (_data.current_view)
    {
    case view_devices:
        _handle_devices_navigation();
        break;
    case view_chat:
        _handle_chat_navigation();
        break;
    }
}

void AppFlood::onDestroy()
{
    scroll_text_free(&_data.name_scroll_ctx);
    hl_text_free(&_data.hint_hl_ctx);
    flood_deinit();
    _data.hal->sdcard()->eject();
    // reinit wifi if enabled
    _data.hal->wifi()->init();
    if (_data.hal->settings()->getBool("wifi", "enabled"))
    {
        delay(100);
        _data.hal->wifi()->connect();
    }

    // Request system bar update to remove flood status immediately
    _request_system_bar_update();
}

void AppFlood::_request_system_bar_update()
{
    if (_data.system_bar_force_update_flag)
    {
        *_data.system_bar_force_update_flag = true;
    }
}

bool AppFlood::_device_enum_callback(const mesh_device_info_t* device, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app)
    {
        DeviceItem di{};
        // fill device item from device info
        di.name = std::string((char*)device->persistent.name);
        memcpy(di.mac, device->persistent.mac, 6);
        di.role = device->persistent.role;
        di.capabilities = device->persistent.capabilities;
        di.last_seen = device->volatile_data.last_seen;
        di.battery_level = device->volatile_data.battery_level;
        di.signal_strength = device->volatile_data.signal_strength;
        di.hops = device->volatile_data.hops;
        di.unread_messages = device->volatile_data.unread_messages;
        app->_data.devices.push_back(di);
    }
    return true; // Continue enumeration
}

bool AppFlood::_channel_enum_callback(const mesh_channel_info_t* channel, void* user_data)
{
    AppFlood* app = static_cast<AppFlood*>(user_data);
    if (app && channel)
    {
        DeviceItem di{};
        // fill channel item with channel info
        di.name = std::string((const char*)channel->persistent.channel_name);
        memset(di.mac, 0, 6); // Channels don't have MAC addresses
        di.role = MESH_ROLE_CHANNEL;
        di.capabilities = 0;
        di.battery_level = 0;
        di.signal_strength = 0;
        di.hops = 0;

        // Use volatile data from the channel info structure
        di.last_seen = channel->volatile_data.last_seen;
        di.unread_messages = channel->volatile_data.unread_messages;

        app->_data.devices.push_back(di);
    }
    return true; // Continue enumeration
}

void AppFlood::_refresh_devices()
{
    // remember old mac
    if (_data.selected_index < _data.devices.size())
    {
        auto& d = _data.devices[_data.selected_index];
        memcpy(_data.chat_mac, d.mac, 6);
        _data.chat_role = d.role;
        _data.chat_with = d.name;
    }
    _data.devices.clear();
    // Use callback-based enumeration to populate devices list
    flood_enum_devices(_device_enum_callback, this);
    // Enumerate channels and add them to the devices list
    flood_enum_channels(_channel_enum_callback, this);
// ! test code
#if 0
    for (const auto& d : _data.devices_test)
    {
        _data.devices.push_back(d);
    }
#endif
    ESP_LOGD(TAG, "Devices enumerated: %d", _data.devices.size());
    _sort_devices();
    _data.need_refresh = false;
}

void AppFlood::_sort_devices()
{
    // Stable sort by last_seen desc (newest first)
    if (_sort_mode_funcs[_data.sort_mode_index])
        std::stable_sort(_data.devices.begin(), _data.devices.end(), _sort_mode_funcs[_data.sort_mode_index]);
    // find selected index
    uint8_t* chat_mac = _data.chat_mac;
    std::string chat_with = _data.chat_with;
    if (_data.chat_role == MESH_ROLE_CHANNEL)
    {
        _data.selected_index = std::find_if(_data.devices.begin(),
                                            _data.devices.end(),
                                            [chat_with](const DeviceItem& d) { return d.name == chat_with; }) -
                               _data.devices.begin();
    }
    else
    {
        _data.selected_index = std::find_if(_data.devices.begin(),
                                            _data.devices.end(),
                                            [chat_mac](const DeviceItem& d) { return memcmp(d.mac, chat_mac, 6) == 0; }) -
                               _data.devices.begin();
    }
    // first item if not found
    if (_data.selected_index == _data.devices.size() && _data.devices.size() > 0)
    {
        _data.selected_index = 0;
    }
    // move scroll offset to selected index
    if (_data.selected_index < _data.scroll_offset)
    {
        _data.scroll_offset = _data.selected_index;
    }
    else if (_data.selected_index >= _data.scroll_offset + LIST_MAX_VISIBLE_ITEMS)
    {
        _data.scroll_offset = _data.selected_index - LIST_MAX_VISIBLE_ITEMS + 1;
    }
    scroll_text_reset(&_data.name_scroll_ctx);
    _data.need_render = true;
}

void AppFlood::_refresh_chat()
{
    _chat_reload_messages();
    if (_data.chat_role == MESH_ROLE_CHANNEL)
    {
        flood_get_channel_message_count(_data.chat_with.c_str(), &_data.total_messages);
    }
    else
    {
        flood_get_message_count(_data.chat_mac, &_data.total_messages);
    }
    // refresh unread messages from volatile data
    mesh_device_volatile_t chat_device;
    mesh_channel_volatile_t chat_channel;
    esp_err_t ret = _data.chat_role == MESH_ROLE_CHANNEL ? flood_get_channel_volatile(_data.chat_with.c_str(), &chat_channel)
                                                         : flood_get_device_volatile(_data.chat_mac, &chat_device);
    if (ret != ESP_OK)
    {
        _data.unread_messages = 0;
    }
    else
    {
        _data.unread_messages =
            _data.chat_role == MESH_ROLE_CHANNEL ? chat_channel.unread_messages : chat_device.unread_messages;
    }

    _data.need_render = true;
    _data.need_refresh = false;
}

std::string AppFlood::_time_ago(uint32_t last_seen_ms)
{
    if (last_seen_ms == 0)
    {
        return "...";
    }
    uint32_t now = millis();
    uint32_t diff = (now >= last_seen_ms) ? (now - last_seen_ms) : 0;
    uint32_t sec = diff / 1000;
    if (sec < 10)
    {
        return "now";
    }
    else if (sec < 60)
    {
        return std::format("{:>2}s", sec);
    }
    else if (sec < 3600)
    {
        return std::format("{:>2}m", (uint32_t)(sec / 60));
    }
    else if (sec < 86400)
    {
        return std::format("{:>2}h", (uint32_t)(sec / 3600));
    }
    else if (sec < 2592000)
    {
        return std::format("{:>2}d", (uint32_t)(sec / 86400));
    }
    else if (sec < 31536000)
    {
        return std::format("{:>2}M", (uint32_t)(sec / 2592000));
    }
    else
    {
        return std::format("{:>2}y", (uint32_t)(sec / 31536000));
    }
}

void AppFlood::_draw_battery_icon(int x, int y, uint8_t level, bool selected)
{
    // Placeholder battery: small rectangle segmented
    auto c = _data.hal->canvas();
    c->drawRoundRect(x, y, 12, 7, 2, selected ? TFT_BLACK : TFT_WHITE);
    c->fillRect(x + 12 + 1, y + 2, 1, 3, selected ? TFT_BLACK : TFT_WHITE); // tip
    uint8_t filled = (level >= 100 ? 5 : level >= 75 ? 4 : level >= 50 ? 3 : level >= 25 ? 2 : 1);
    uint16_t col = (filled == 1) ? TFT_RED : selected ? TFT_BLACK : TFT_WHITE;
    c->fillRect(x + 1, y + 1, 2 * filled, 5, col);
}

static const uint16_t* _role_icon(FLOOD_DEVICE_ROLE_t role, bool selected)
{
    switch (role)
    {
    case MESH_ROLE_CLIENT:
        return selected ? image_data_flood_client_sel : image_data_flood_client;
    case MESH_ROLE_ROUTER:
        return selected ? image_data_flood_router_sel : image_data_flood_router;
    case MESH_ROLE_REPEATER:
        return selected ? image_data_flood_repeater_sel : image_data_flood_repeater;
    case MESH_ROLE_CHANNEL:
        return selected ? image_data_flood_channel_sel : image_data_flood_channel;
    default:
        return selected ? image_data_flood_client_sel : image_data_flood_client;
    }
}

bool AppFlood::_render_scrolling_name()
{
    if (_data.devices.empty())
    {
        return false;
    }

    // Get the text to display (file or directory name)
    std::string display_name = _data.devices[_data.selected_index].name;

    // Calculate y position of selected item
    int relative_pos = _data.selected_index - _data.scroll_offset;
    int y_offset = LIST_HEADER_HEIGHT + (relative_pos * (LIST_ITEM_HEIGHT + 1));

    // Render the scrolling text using our utility
    return scroll_text_render(&_data.name_scroll_ctx,
                              display_name.c_str(),
                              LIST_ITEM_LEFT_PADDING + 2 + 4 * 6 + 6 + LIST_ICON_WIDTH + 2,
                              y_offset + 1,
                              THEME_COLOR_SELECTED,
                              THEME_COLOR_BG_SELECTED);
}

bool AppFlood::_render_devices()
{
    if (!_data.need_render)
    {
        return false;
    }
    auto c = _data.hal->canvas();
    int panel_x = 0;
    int panel_width = c->width();
    c->fillRect(panel_x, 0, panel_width, c->height(), THEME_COLOR_BG);
    // set font
    c->setFont(FONT_12);
    // draw scrollbar
    _render_devices_scrollbar(panel_x, panel_width);

    if (_data.devices.empty())
    {
        // draw string: no files to display
        c->setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
        c->drawCenterString("<no devices found>",
                            panel_x + panel_width / 2,
                            LIST_HEADER_HEIGHT + (LIST_MAX_VISIBLE_ITEMS / 2) * (LIST_ITEM_HEIGHT + 1));
        return false;
    }
    // draw sort mode indicator
    int short_x = LIST_ITEM_LEFT_PADDING;
    int short_width = 4 * 6 + 6;
    c->setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
    int sort_mode_x = 0;
    int sort_mode_width = 0;
    switch (_data.sort_mode_index)
    {
    case 0:
        sort_mode_x = short_x + short_width + 2;
        sort_mode_width = LIST_ICON_WIDTH;
        break;
    case 1:
        sort_mode_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2;
        sort_mode_width = LIST_MAX_DISPLAY_CHARS * 6;
        break;
    case 2:
        sort_mode_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2 + LIST_MAX_DISPLAY_CHARS * 6 + 1;
        sort_mode_width = 6;
        break;
    case 3:
        sort_mode_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2 + LIST_MAX_DISPLAY_CHARS * 6 + 2 + 4;
        sort_mode_width = 16;
        break;
    case 4:
        sort_mode_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2 + LIST_MAX_DISPLAY_CHARS * 6 + 2 + 4 + 16 + 2;
        sort_mode_width = 12;
        break;
    case 5:
        sort_mode_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2 + LIST_MAX_DISPLAY_CHARS * 6 + 2 + 4 + 16 + 10 + 4;
        sort_mode_width = 4 * 6;
        break;
    }
    c->drawFastHLine(sort_mode_x, LIST_HEADER_HEIGHT, sort_mode_width, TFT_YELLOW);
    // Draw file list
    int y_offset = LIST_HEADER_HEIGHT;
    int items_drawn = 0;
    const int max_width = LIST_MAX_DISPLAY_CHARS * 6; // FONT_12 width

    for (int i = _data.scroll_offset; i < _data.devices.size() && items_drawn < LIST_MAX_VISIBLE_ITEMS; i++)
    {
        const auto& d = _data.devices[i];
        std::string display_name = d.name;

        if (c->textWidth(display_name.c_str()) > max_width)
        {
            display_name = display_name.substr(0, LIST_MAX_DISPLAY_CHARS - 1) + ">";
        }
        bool is_selected = i == _data.selected_index;
        if (is_selected)
        {
            c->fillRect(panel_x + 2,
                        y_offset + 1,
                        panel_width - 2 - SCROLL_BAR_WIDTH - 1,
                        LIST_ITEM_HEIGHT,
                        THEME_COLOR_BG_SELECTED);
        }
        // Draw colored node identifier
        int node_color = flood_get_device_color(d.mac);
        int node_text_color = flood_get_device_text_color(d.mac);
        std::string node_id = (d.role == MESH_ROLE_CHANNEL) ? " ch " : std::format("{:04x}", flood_get_device_id(d.mac));
        c->fillRoundRect(short_x, y_offset + 1, short_width, LIST_ITEM_HEIGHT, 4, node_color);
        c->setTextColor(node_text_color, node_color);
        c->drawCenterString(node_id.c_str(), short_x + short_width / 2, y_offset + 1);
        // role icon
        c->pushImage(short_x + short_width + 2,
                     y_offset + 1 + 1,
                     LIST_ICON_WIDTH,
                     LIST_ICON_HEIGHT,
                     _role_icon(d.role, is_selected));
        // name
        c->setTextColor(is_selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED,
                        is_selected ? THEME_COLOR_BG_SELECTED : THEME_COLOR_BG);
        c->drawString(display_name.c_str(), short_x + short_width + 2 + LIST_ICON_WIDTH + 2, y_offset + 1);
        // WiFi signal gauge (after name, before battery)
        int signal_x = short_x + short_width + 2 + LIST_ICON_WIDTH + 2 + LIST_MAX_DISPLAY_CHARS * 6 + 2;
        int signal_width = 4;
        if (d.role != MESH_ROLE_CHANNEL)
        {
            int signal_height = LIST_ITEM_HEIGHT - 2;
            // Draw signal strength as vertical bars (0-255 mapped to 0-14 pixels)
            int filled_height = (d.signal_strength * signal_height) / 100;
            if (filled_height < 1 && d.signal_strength > 0)
                filled_height = 1; // Show at least 1px if signal exists

            // Background
            c->fillRect(signal_x,
                        y_offset + 1 + 1,
                        signal_width,
                        signal_height,
                        is_selected ? THEME_COLOR_BG_SELECTED_DARK : THEME_COLOR_BG_DARK);

            // Filled portion (bottom to top)
            if (filled_height > 0)
            {
                uint16_t signal_color;
                if (d.signal_strength > 60)
                    signal_color = TFT_GREEN; // Strong signal
                else if (d.signal_strength > 40)
                    signal_color = TFT_YELLOW; // Medium signal
                else if (d.signal_strength > 20)
                    signal_color = TFT_ORANGE; // Weak signal
                else
                    signal_color = TFT_RED; // Weak signal

                c->fillRect(signal_x,
                            y_offset + 1 + 1 + signal_height - filled_height,
                            signal_width,
                            filled_height,
                            signal_color);
            }

            // battery
            _draw_battery_icon(signal_x + signal_width + 2, y_offset + 1 + 4, d.battery_level, is_selected);
            // hops
            // _data.hal->canvas()->setTextColor(is_selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED,
            //                                   is_selected ? THEME_COLOR_BG_SELECTED_DARK : THEME_COLOR_BG_DARK);
            c->drawString(std::format("{}", d.hops).c_str(), signal_x + signal_width + 2 + 16 + 2, y_offset + 1);
            c->drawRect(signal_x + signal_width + 2 + 16 + 1,
                        y_offset + 1 + 1,
                        6 + 2 + 2,
                        LIST_ITEM_HEIGHT - 2,
                        is_selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED);
        }
        // last seen
        c->setTextColor(is_selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED,
                        is_selected ? THEME_COLOR_BG_SELECTED : THEME_COLOR_BG);
        c->drawString(_time_ago(d.last_seen).c_str(), signal_x + signal_width + 2 + 16 + 10 + 4, y_offset + 1);
        // new messages indicator
        if (d.unread_messages)
        {
            std::string ticker_str = std::format("+{}", d.unread_messages);
            uint8_t ticker_width = ticker_str.length() * 6 + 6;
            // draw red roundrect with number of unread messages
            c->fillRoundRect(panel_x + panel_width - ticker_width - 1 - SCROLL_BAR_WIDTH - 1,
                             y_offset + 1,
                             ticker_width,
                             LIST_ITEM_HEIGHT,
                             3,
                             TFT_RED);
            c->setTextColor(is_selected ? THEME_COLOR_SELECTED : THEME_COLOR_UNSELECTED, TFT_RED);
            c->drawRightString(ticker_str.c_str(), panel_x + panel_width - 1 - SCROLL_BAR_WIDTH - 6, y_offset + 1);
        }
        y_offset += LIST_ITEM_HEIGHT + 1;
        items_drawn++;
    }

    _render_devices_scrollbar(panel_x, panel_width);

    _data.need_render = false;
    return true;
}

bool AppFlood::_render_devices_scrollbar(int panel_x, int panel_width)
{
    if (_data.devices.size() <= LIST_MAX_VISIBLE_ITEMS)
    {
        return false;
    }

    // const int scrollbar_width = 4;
    const int scrollbar_x = panel_x + panel_width - SCROLL_BAR_WIDTH - 1;
    const int scrollbar_height = (LIST_ITEM_HEIGHT + 1) * LIST_MAX_VISIBLE_ITEMS;

    int thumb_height = std::max(SCROLLBAR_MIN_HEIGHT, (scrollbar_height * LIST_MAX_VISIBLE_ITEMS) / (int)_data.devices.size());
    int thumb_pos = LIST_HEADER_HEIGHT +
                    (scrollbar_height - thumb_height) * _data.scroll_offset / (_data.devices.size() - LIST_MAX_VISIBLE_ITEMS);

    // Draw scrollbar track
    auto c = _data.hal->canvas();
    c->drawRect(scrollbar_x, LIST_HEADER_HEIGHT, SCROLL_BAR_WIDTH, scrollbar_height, TFT_DARKGREY);
    // Draw scrollbar thumb
    c->fillRect(scrollbar_x, thumb_pos, SCROLL_BAR_WIDTH, thumb_height, TFT_ORANGE);
    return true;
}

bool AppFlood::_render_chat()
{
    if (!_data.need_render)
    {
        return false;
    }
    _data.need_render = false;
    auto c = _data.hal->canvas();
    c->fillScreen(THEME_COLOR_BG);
    c->setFont(FONT_12);

    // Header
    // c->drawRect(0, 0, c->width(), CHAT_HEADER_HEIGHT, THEME_COLOR_BG_SELECTED);
    std::string title = _data.chat_with;
    if (title.length() > 16)
    {
        title = title.substr(0, 15) + ">";
    }
    // c->setTextColor(THEME_COLOR_SELECTED, THEME_COLOR_BG_SELECTED);
    c->pushImage(2, 0, LIST_ICON_WIDTH, LIST_ICON_HEIGHT, _role_icon(_data.chat_role, false));
    c->setTextColor(TFT_SKYBLUE, THEME_COLOR_BG);
    c->drawString(title.c_str(), 2 + LIST_ICON_WIDTH + 2, 0);
    c->drawFastHLine(0, CHAT_HEADER_HEIGHT - 1, c->width() - 1, THEME_COLOR_BG_SELECTED);
    int x_offset = 2;
    // unread messages
    if (_data.unread_messages > 0)
    {
        std::string ticker_str = std::format("+{}", _data.unread_messages);
        uint8_t ticker_width = ticker_str.length() * 6 + 6;
        // draw red roundrect with number of unread messages
        c->fillRoundRect(c->width() - x_offset - ticker_width - 1, 0, ticker_width, CHAT_HEADER_HEIGHT, 3, TFT_RED);
        c->setTextColor(TFT_WHITE, TFT_RED);
        c->drawRightString(ticker_str.c_str(), c->width() - 1 - x_offset - 3, 0);
    }
    else
    {
        // total messages
        c->setTextColor(TFT_WHITE);
        c->drawRightString(std::format("{}", _data.total_messages).c_str(), c->width() - 1 - x_offset, 1);
    }
    // error case
    if (_data.chat_messages.empty())
    {
        c->setTextColor(TFT_DARKGREY, THEME_COLOR_BG);
        c->drawCenterString(_data.chat_info.c_str(), c->width() / 2, c->height() / 2);
        return true;
    }

    // Messages area calculation
    const int messages_area_top = CHAT_HEADER_HEIGHT;
    const int messages_area_bottom = c->height() - CHAT_HEADER_HEIGHT - 1;
    const int messages_area_height = messages_area_bottom - messages_area_top;

    // Calculate text rendering constraints
    const int node_id_width = 4 * 6 + 6; // "ab12 " width
    const int text_start_x = node_id_width + 2;

    // Render messages to canvas
    int y = messages_area_top;
    int current_line = 0;

    for (uint32_t i = 0; i < _data.chat_messages.size(); i++)
    {
        uint16_t device_id = std::get<0>(_data.chat_messages[i]);
        auto& lines = std::get<1>(_data.chat_messages[i]);
        uint8_t status = std::get<2>(_data.chat_messages[i]);
        int sender_color = flood_get_device_color_by_id(device_id);
        int sender_text_color = flood_get_device_text_color_by_id(device_id);

        for (size_t line_idx = 0; line_idx < lines.size(); line_idx++)
        {
            // Skip lines before scroll position
            if (current_line < _data.cur_line)
            {
                current_line++;
                continue;
            }

            // Stop if we've filled the sprite
            if ((current_line >= (_data.cur_line + CHAT_MAX_VISIBLE_ITEMS)))
                break;

            // Draw message text line
            c->setTextColor(TFT_WHITE, THEME_COLOR_BG);
            c->drawString(lines[line_idx].c_str(), text_start_x, y);
            // Draw sender ID only on first line of message
            if (line_idx == 0)
            {
                c->fillRoundRect(2, y, node_id_width, CHAT_ITEM_HEIGHT, 3, sender_color);
                c->setTextColor(sender_text_color, sender_color);
                std::string sender_id = std::format("{:04x}", device_id);
                c->drawString(sender_id.c_str(), 2 + 3, y);
                // draw status DELIVERED
                if (status > MESSAGE_STATUS_SENT)
                {
                    c->drawFastVLine(2 + node_id_width,
                                     y,
                                     CHAT_ITEM_HEIGHT,
                                     status == MESSAGE_STATUS_DELIVERED ? TFT_GREEN : TFT_RED);
                }
            }

            y += CHAT_ITEM_HEIGHT;
            current_line++;
        }
    }

    // Draw scroll bar if needed
    if (_data.total_messages > CHAT_MAX_VISIBLE_ITEMS)
    {
        int scrollbar_x = c->width() - SCROLL_BAR_WIDTH - 1;
        int scrollbar_height = messages_area_height;
        int thumb_height =
            std::max(SCROLLBAR_MIN_HEIGHT, (CHAT_MAX_VISIBLE_ITEMS * scrollbar_height) / (int)_data.total_messages);
        int thumb_y = messages_area_top + ((_data.cur_index * scrollbar_height) / _data.total_messages);

        c->fillRect(scrollbar_x, messages_area_top, SCROLL_BAR_WIDTH, scrollbar_height, TFT_DARKGREY);
        c->fillRect(scrollbar_x, thumb_y, SCROLL_BAR_WIDTH, thumb_height, TFT_ORANGE);
    }

    return true;
}

bool AppFlood::_handle_chat_navigation()
{
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        bool handle = false;
        bool changed = false;
        // check Fn key hold
        auto keys_state = _data.hal->keyboard()->keysState();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                bool sound = true;
                // Fn holding? = home
                if (keys_state.fn)
                {
                    // load first page
                    if (_data.cur_index > 0)
                    {
                        sound = _chat_load_messages(0);
                    }
                }
                else
                {
                    if (_data.cur_line > 0)
                    {
                        _data.cur_line--;
                    }
                    else
                    {
                        sound = _chat_load_prev();
                    }
                }
                if (sound)
                {
                    _data.hal->playNextSound();
                    _data.need_render = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                bool sound = true;
                // Fn holding? = end
                if (keys_state.fn)
                {
                    // load last page
                    sound = _chat_load_messages(-1);
                }
                else
                {
                    if (_data.cur_line < _data.tot_lines - CHAT_MAX_VISIBLE_ITEMS)
                    {
                        _data.cur_line++;
                    }
                    else
                    {
                        sound = _chat_load_next();
                    }
                    // mark read
                    if (_data.cur_index >= _data.total_messages - CHAT_MAX_VISIBLE_ITEMS &&
                        _data.cur_line == _data.tot_lines - CHAT_MAX_VISIBLE_ITEMS)
                    {
                        if (_data.chat_role == MESH_ROLE_CHANNEL)
                        {
                            flood_channel_mark_read(_data.chat_with.c_str());
                        }
                        else
                        {
                            flood_private_mark_read(_data.chat_mac);
                        }
                        _data.unread_messages = 0;
                    }
                }
                if (sound)
                {
                    _data.hal->playNextSound();
                    _data.need_render = true;
                }
            }
        }
        // page up
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                bool sound = false;
                if (keys_state.fn)
                {
                    int index = _data.cur_index > 20 ? _data.cur_index - 20 : 0;
                    sound = _chat_load_messages(index);
                }
                else
                {
                    int step = CHAT_MAX_VISIBLE_ITEMS;
                    while (step > 0)
                    {
                        if (_data.cur_line > 0)
                        {
                            _data.cur_line--;
                            sound = true;
                        }
                        else
                        {
                            sound |= _chat_load_prev();
                        }
                        step--;
                        if (!sound)
                        {
                            break;
                        }
                    }
                }
                if (sound)
                {
                    _data.hal->playNextSound();
                    _data.need_render = true;
                }
            }
        }
        // page down
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                bool sound = false;
                if (keys_state.fn)
                {
                    int index =
                        ((_data.cur_index + 20) < (_data.total_messages - CHAT_MAX_VISIBLE_ITEMS)) ? _data.cur_index + 20 : -1;
                    sound = _chat_load_messages(index);
                }
                else
                {
                    int step = CHAT_MAX_VISIBLE_ITEMS * (keys_state.fn ? 10 : 1);
                    while (step > 0)
                    {
                        if (_data.cur_line < _data.tot_lines - CHAT_MAX_VISIBLE_ITEMS)
                        {
                            _data.cur_line++;
                            sound = true;
                        }
                        else
                        {
                            sound |= _chat_load_next();
                        }
                        step--;
                        if (!sound)
                        {
                            break;
                        }
                    }
                }
                // mark read
                if (_data.cur_index >= _data.total_messages - CHAT_MAX_VISIBLE_ITEMS &&
                    _data.cur_line == _data.tot_lines - CHAT_MAX_VISIBLE_ITEMS)
                {
                    if (_data.chat_role == MESH_ROLE_CHANNEL)
                    {
                        flood_channel_mark_read(_data.chat_with.c_str());
                    }
                    else
                    {
                        flood_private_mark_read(_data.chat_mac);
                    }
                    _data.unread_messages = 0;
                }

                if (sound)
                {
                    _data.hal->playNextSound();
                    _data.need_render = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            // show string edit dialog
            std::string message;
            bool result = UTILS::UI::show_edit_string_dialog(_data.hal, "Enter message", message, false, MESSAGE_MAX_PAYLOAD);
            if (result && !message.empty())
            {
                // send message
                esp_err_t ret =
                    _data.chat_role == MESH_ROLE_CHANNEL
                        ? flood_send_channel_message(_data.chat_with.c_str(), (uint8_t*)message.c_str(), message.length(), 0, 0)
                        : flood_send_private_message(_data.chat_mac, (uint8_t*)message.c_str(), message.length(), 0);
                if (ret != ESP_OK)
                {
                    UTILS::UI::show_error_dialog(_data.hal, "Error", "Failed to send message");
                }
                else
                {
                    _chat_load_messages(-1);
                    playMessageSentSound();
                }
            }
            _data.need_render = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            // mark read
            if (_data.chat_role == MESH_ROLE_CHANNEL)
            {
                flood_channel_mark_read(_data.chat_with.c_str());
            }
            else
            {
                flood_private_mark_read(_data.chat_mac);
            }
            _data.unread_messages = 0;
            // Go back to devices view
            _data.current_view = view_devices;
            _data.need_refresh = true;
            _data.need_render = true;
            changed = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            // show confirmation dialog
            bool result = UTILS::UI::show_confirmation_dialog(_data.hal, "Confirm", "Delete all messages?");
            if (result)
            {
                // clear chat
                esp_err_t ret = _data.chat_role == MESH_ROLE_CHANNEL ? flood_clear_channel(_data.chat_with.c_str())
                                                                     : flood_clear_chat(_data.chat_mac);
                if (ret != ESP_OK)
                {
                    UTILS::UI::show_error_dialog(_data.hal, "Error", "Failed to clear chat");
                }
                else
                {
                    _chat_load_messages(-1);
                    _data.need_refresh = true;
                }
            }
            _data.need_render = true;
            changed = true;
        }
        return changed;
    }
    else
    {
        is_repeat = false;
        return false;
    }
}

bool AppFlood::_handle_devices_navigation()
{
    // Devices view navigation
    bool changed = false;
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        bool handle = false;
        // check Fn key hold
        auto keys_state = _data.hal->keyboard()->keysState();

        // up navigation
        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                if (_data.selected_index > 0)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = home
                    if (keys_state.fn)
                    {
                        _data.selected_index = 0;
                    }
                    else
                    {
                        _data.selected_index--;
                    }
                    if (_data.selected_index < _data.scroll_offset)
                    {
                        _data.scroll_offset = _data.selected_index;
                    }
                    _data.need_render = true;
                    changed = true;
                }
            }
        }
        // down navigation
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                int maxIndex = (int)_data.devices.size() - 1;
                if (_data.selected_index < maxIndex)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = end
                    if (keys_state.fn)
                    {
                        _data.selected_index = maxIndex;
                    }
                    else
                    {
                        _data.selected_index++;
                    }
                    if (_data.selected_index >= _data.scroll_offset + LIST_MAX_VISIBLE_ITEMS)
                    {
                        _data.scroll_offset = _data.selected_index - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    _data.need_render = true;
                    changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                if (_data.selected_index > 0)
                {
                    _data.hal->playNextSound();
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_index = std::max(0, _data.selected_index - jump);
                    _data.scroll_offset = std::max(0, _data.selected_index - (LIST_MAX_VISIBLE_ITEMS - 1));
                }
                _data.need_render = true;
                changed = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                if (_data.selected_index < (int)_data.devices.size() - 1)
                {
                    _data.hal->playNextSound();
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_index = std::min((int)_data.devices.size() - 1, _data.selected_index + jump);
                    _data.scroll_offset =
                        std::min(std::max(0, (int)_data.devices.size() - LIST_MAX_VISIBLE_ITEMS), _data.selected_index);
                }
                _data.need_render = true;
                changed = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_S))
        {
            if (!is_repeat)
            {
                is_repeat = true;
                next_fire_ts = now + KEY_HOLD_MS;
                handle = true;
            }
            else if (now >= next_fire_ts)
            {
                next_fire_ts = now + KEY_REPEAT_MS;
                handle = true;
            }
            if (handle)
            {
                _data.hal->playNextSound();
                _data.sort_mode_index = (_data.sort_mode_index + 1) % SORT_MODE_COUNT;
                // just sort devices list, no reload metadata
                _sort_devices();
                _data.need_render = true;
                changed = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);
            if (_data.selected_index >= 0 && _data.selected_index < (int)_data.devices.size())
            {
                const auto& d = _data.devices[_data.selected_index];
                _data.chat_with = d.name;
                memcpy(_data.chat_mac, d.mac, 6);
                _data.chat_role = d.role;
                _data.current_view = view_chat;
                _chat_load_messages(-1);
                _data.need_render = true;
                return true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            // Close app
            destroyApp();
            changed = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_C))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_C);
            // add new channel
            std::string channel_name;
            bool result =
                UTILS::UI::show_edit_string_dialog(_data.hal, "Channel name", channel_name, false, MESH_MAX_NAME_LENGTH);
            if (result)
            {
                esp_err_t ret = flood_add_channel(channel_name.c_str());
                if (ret != ESP_OK)
                {
                    UTILS::UI::show_error_dialog(_data.hal, "Error", "Failed to add channel");
                }
                _data.need_refresh = true;
            }
            _data.need_render = true;
            changed = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            // ignore if not selected
            if (_data.selected_index >= 0 && _data.selected_index < (int)_data.devices.size())
            {
                auto& d = _data.devices[_data.selected_index];
                _data.chat_with = d.name;
                _data.chat_role = d.role;
                memcpy(_data.chat_mac, d.mac, 6);
                // show confirmation dialog
                bool result = UTILS::UI::show_confirmation_dialog(_data.hal,
                                                                  "Confirm",
                                                                  std::format("Delete {}?", _data.chat_with).c_str());
                if (result)
                {
                    // delete device or channel
                    esp_err_t ret = _data.chat_role == MESH_ROLE_CHANNEL ? flood_remove_channel(_data.chat_with.c_str())
                                                                         : flood_remove_device(_data.chat_mac);
                    if (ret != ESP_OK)
                    {
                        UTILS::UI::show_error_dialog(_data.hal,
                                                     "Error",
                                                     std::format("Failed to delete {}", _data.chat_with).c_str());
                    }
                    // reload data, item deleted
                    _data.need_refresh = true;
                }
                // render anyway to restore from dialog
                _data.need_render = true;
                changed = true;
            }
        }
        return changed;
    }
    else
    {
        is_repeat = false;
        return false;
    }
}

static std::vector<std::string> wrap_text(const std::string& text, int chars_per_line)
{
    std::vector<std::string> lines;
    if (text.empty())
    {
        lines.push_back("");
        return lines;
    }

    size_t pos = 0;
    while (pos < text.length())
    {
        size_t line_len = std::min((size_t)chars_per_line, text.length() - pos);

        // Try to break at word boundary if not at end
        if (pos + line_len < text.length())
        {
            size_t last_space = text.rfind(' ', pos + line_len);
            if (last_space != std::string::npos && last_space > pos)
            {
                line_len = last_space - pos + 1; // Include the space
            }
        }

        lines.push_back(text.substr(pos, line_len));
        pos += line_len;
    }
    return lines;
};

bool AppFlood::_chat_load_messages(int index)
{
    _data.chat_messages.clear();
    _data.cur_line = 0;
    _data.tot_lines = 0;
    _data.total_messages = 0;
    if ((_data.chat_role == MESH_ROLE_CHANNEL ? flood_get_channel_message_count(_data.chat_with.c_str(), &_data.total_messages)
                                              : flood_get_message_count(_data.chat_mac, &_data.total_messages)) != ESP_OK)
    {
        _data.chat_info = "<error reading messages>";
        return false;
    }
    // No messages case
    if (_data.total_messages == 0)
    {
        _data.chat_info = "<no messages yet>";
        return false;
    }
    if (index == -1)
    {
        // -1 = last message
        // _data.cur_index = _data.total_messages - 1;
        _data.cur_index = _data.total_messages -
                          ((_data.total_messages > CHAT_MAX_VISIBLE_ITEMS) ? CHAT_MAX_VISIBLE_ITEMS : _data.total_messages);
        if (_data.chat_role == MESH_ROLE_CHANNEL)
        {
            flood_channel_mark_read(_data.chat_with.c_str());
        }
        else
        {
            flood_private_mark_read(_data.chat_mac);
        }
        _data.unread_messages = 0;
    }
    else
    {
        _data.cur_index = index;
    }

    // load messages
    message_record_t* all_records = (message_record_t*)malloc(CHAT_MAX_VISIBLE_ITEMS * sizeof(message_record_t));
    if (all_records == NULL)
    {
        _data.chat_info = "<out of memory>";
        return false;
    }
    uint32_t loaded = 0;
    esp_err_t ret = _data.chat_role == MESH_ROLE_CHANNEL
                        ? flood_load_channel_messages(_data.chat_with.c_str(),
                                                      _data.cur_index,
                                                      CHAT_MAX_VISIBLE_ITEMS,
                                                      all_records,
                                                      &loaded)
                        : flood_load_messages(_data.chat_mac, _data.cur_index, CHAT_MAX_VISIBLE_ITEMS, all_records, &loaded);
    if (ret != ESP_OK)
    {
        free(all_records);
        _data.chat_info = "<error loading messages>";
        return false;
    }
    const int node_id_width = 4 * 6 + 6; // "ab12 " width
    const int text_start_x = node_id_width + 2;
    const int max_text_width = _data.hal->canvas()->width() - text_start_x - 2;
    const int chars_per_line = max_text_width / 6; // FONT_12 is 6px wide

    // format messages
    _data.tot_lines = 0;
    for (uint32_t i = 0; i < loaded; i++)
    {
        std::string msg_text;
        if (all_records[i].message_length > 0)
        {
            // ESP_LOGW(TAG, "MESSAGE: %s, status: %d", all_records[i].message_data, all_records[i].status);
            msg_text = std::string((char*)all_records[i].message_data, all_records[i].message_length);
            auto wrapped = wrap_text(msg_text, chars_per_line);
            _data.tot_lines += wrapped.size();
            _data.chat_messages.push_back(
                std::make_tuple(flood_get_device_id(all_records[i].sender_mac), wrapped, all_records[i].status));
        }
    }
    if (index == -1)
    {
        _data.cur_line = _data.tot_lines > CHAT_MAX_VISIBLE_ITEMS ? _data.tot_lines - CHAT_MAX_VISIBLE_ITEMS : 0;
    }
    else
    {
        _data.cur_line = 0;
    }
    free(all_records);
    return true;
}

bool AppFlood::_chat_load_next()
{

    if ((_data.chat_role == MESH_ROLE_CHANNEL) ? flood_get_channel_message_count(_data.chat_with.c_str(), &_data.total_messages)
                                               : flood_get_message_count(_data.chat_mac, &_data.total_messages) != ESP_OK)
    {
        return false;
    }
    if (_data.cur_index >= _data.total_messages - CHAT_MAX_VISIBLE_ITEMS)
    {
        return false;
    }
    // load next messages
    message_record_t* record = (message_record_t*)malloc(sizeof(message_record_t));
    if (record == NULL)
    {
        return false;
    }
    uint32_t loaded = 0;
    esp_err_t ret =
        _data.chat_role == MESH_ROLE_CHANNEL
            ? flood_load_channel_messages(_data.chat_with.c_str(), _data.cur_index + CHAT_MAX_VISIBLE_ITEMS, 1, record, &loaded)
            : flood_load_messages(_data.chat_mac, _data.cur_index + CHAT_MAX_VISIBLE_ITEMS, 1, record, &loaded);
    if (ret != ESP_OK || loaded == 0)
    {
        free(record);
        return false;
    }
    const int node_id_width = 4 * 6 + 6; // "ab12 " width
    const int text_start_x = node_id_width + 2;
    const int max_text_width = _data.hal->canvas()->width() - text_start_x - 2;
    const int chars_per_line = max_text_width / 6; // FONT_12 is 6px wide
    // format message
    std::string msg_text;
    int lines_count = 0;
    if (record->message_length > 0)
    {
        msg_text = std::string((char*)record->message_data, record->message_length);
        auto wrapped = wrap_text(msg_text, chars_per_line);
        lines_count = wrapped.size();
        _data.chat_messages.push_back(std::make_tuple(flood_get_device_id(record->sender_mac), wrapped, record->status));
    }
    free(record);
    int lines_removed = 0;
    if (_data.chat_messages.size() > 0)
    {
        // delete first message
        lines_removed = std::get<1>(_data.chat_messages.front()).size();
        _data.chat_messages.erase(_data.chat_messages.begin());
    }
    _data.cur_index++;
    _data.cur_line -= lines_removed;
    _data.cur_line += 1; // moving 1st line new message
    _data.tot_lines -= lines_removed;
    _data.tot_lines += lines_count;
    _data.need_render = true;
    return true;
}

bool AppFlood::_chat_load_prev()
{
    if (_data.cur_index <= 0)
    {
        return false;
    }
    int lines_removed = 0;
    if (_data.chat_messages.size() > 0)
    {
        // delete last message
        lines_removed = std::get<1>(_data.chat_messages.back()).size();
        _data.chat_messages.erase(_data.chat_messages.end() - 1);
    }
    // load previous messages
    message_record_t* record = (message_record_t*)malloc(sizeof(message_record_t));
    if (record == NULL)
    {
        return false;
    }
    uint32_t loaded = 0;
    esp_err_t ret = _data.chat_role == MESH_ROLE_CHANNEL
                        ? flood_load_channel_messages(_data.chat_with.c_str(), _data.cur_index - 1, 1, record, &loaded)
                        : flood_load_messages(_data.chat_mac, _data.cur_index - 1, 1, record, &loaded);
    if (ret != ESP_OK || loaded == 0)
    {
        free(record);
        return false;
    }
    const int node_id_width = 4 * 6 + 6; // "ab12 " width
    const int text_start_x = node_id_width + 2;
    const int max_text_width = _data.hal->canvas()->width() - text_start_x - 2;
    const int chars_per_line = max_text_width / 6; // FONT_12 is 6px wide
    // format message
    std::string msg_text;
    int lines_count = 0;
    if (record->message_length > 0)
    {
        msg_text = std::string((char*)record->message_data, record->message_length);
        auto wrapped = wrap_text(msg_text, chars_per_line);
        lines_count = wrapped.size();
        // ESP_LOGW(TAG, "PREV: %s", msg_text.c_str());
        _data.chat_messages.insert(_data.chat_messages.begin(),
                                   std::make_tuple(flood_get_device_id(record->sender_mac), wrapped, record->status));
    }
    free(record);
    _data.cur_index--;
    // message end
    _data.cur_line = lines_count - 1;
    _data.tot_lines -= lines_removed;
    _data.tot_lines += lines_count;
    _data.need_render = true;
    if ((_data.chat_role == MESH_ROLE_CHANNEL) ? flood_get_channel_message_count(_data.chat_with.c_str(), &_data.total_messages)
                                               : flood_get_message_count(_data.chat_mac, &_data.total_messages) != ESP_OK)
    {
        return false;
    }
    return true;
}

bool AppFlood::_chat_reload_messages()
{
    int cur_line = _data.cur_line;
    bool ret = _chat_load_messages(_data.cur_index);
    if (ret)
    {
        _data.cur_line = cur_line;
    }
    _data.need_render = true;
    return ret;
}

bool AppFlood::_render_devices_hint()
{
    static bool last_fn = false;
    auto c = _data.hal->canvas();
    auto keys_state = _data.hal->keyboard()->keysState();
    // clear before put text
    if (last_fn != keys_state.fn)
    {
        last_fn = keys_state.fn;
        c->fillRect(0, c->height() - 8, c->width(), 10, THEME_COLOR_BG);
    }
    return hl_text_render(&_data.hint_hl_ctx,
                          last_fn ? HINT_DEVICES_FN : HINT_DEVICES,
                          0,
                          _data.hal->canvas()->height() - 8,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

bool AppFlood::_render_chat_hint()
{
    static bool last_fn = false;
    auto c = _data.hal->canvas();
    auto keys_state = _data.hal->keyboard()->keysState();
    // clear before put text
    if (last_fn != keys_state.fn)
    {
        last_fn = keys_state.fn;
        c->fillRect(0, c->height() - 8, c->width(), 10, THEME_COLOR_BG);
    }
    return hl_text_render(&_data.hint_hl_ctx,
                          last_fn ? HINT_CHAT_FN : HINT_CHAT,
                          0,
                          c->height() - 8,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

void AppFlood::needRefresh() { _data.need_refresh = true; }

void AppFlood::wakeUpScreen() { _data.hal->keyboard()->resetLastPressedTime(); }

void AppFlood::goLastMessage()
{
    if (_data.cur_index >= _data.total_messages - CHAT_MAX_VISIBLE_ITEMS)
    {
        _chat_load_messages(-1);
    }
}
