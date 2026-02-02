#pragma once

#include "../apps.h"
#include <vector>
#include <string>

#include "flood.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/anim/scroll_text.h"

#include "assets/flood_big.h"
#include "assets/flood_small.h"
#include "assets/flood_client.h"
#include "assets/flood_client_sel.h"
#include "assets/flood_repeater.h"
#include "assets/flood_repeater_sel.h"
#include "assets/flood_router.h"
#include "assets/flood_router_sel.h"
#include "assets/flood_channel.h"
#include "assets/flood_channel_sel.h"

namespace MOONCAKE::APPS
{
    struct DeviceItem
    {
        std::string name;
        uint8_t mac[6];
        FLOOD_DEVICE_ROLE_t role;
        uint8_t capabilities;
        uint32_t last_seen;
        uint8_t battery_level;
        uint8_t signal_strength;
        uint8_t hops;
        uint8_t unread_messages = 0;
    };

    class AppFlood : public APP_BASE
    {

    private:
        enum View
        {
            view_devices = 0,
            view_chat = 1,
        };

        struct
        {
            HAL::Hal* hal = nullptr;
            bool* system_bar_force_update_flag = nullptr;
            View current_view = view_devices;
            FLOOD_DEVICE_ROLE_t chat_role = MESH_ROLE_CLIENT;
            int selected_index = 0;
            int scroll_offset = 0;
            int sort_mode_index = 0;
            std::vector<DeviceItem> devices;
            // std::vector<DeviceItem> devices_test;
            std::string chat_with; // MAC or channel name
            std::string chat_info;
            bool need_render = true;
            bool need_refresh = true;
            UTILS::SCROLL_TEXT::ScrollTextContext_t name_scroll_ctx;
            UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
            uint32_t last_render_tick = 0;
            // chat view data:device_id -> message_lines formatted
            std::vector<std::tuple<uint16_t, std::vector<std::string>, uint8_t>> chat_messages;
            uint32_t total_messages = 0;
            uint16_t unread_messages = 0;
            int cur_index = 0;
            int cur_line = 0;
            int tot_lines = 0;
            int max_lines = 0;
            uint8_t chat_mac[6] = {0};
        } _data;

        // Data
        void _refresh_devices();
        void _sort_devices();
        void _refresh_chat();
        static bool _device_enum_callback(const mesh_device_info_t* device, void* user_data);
        static bool _channel_enum_callback(const mesh_channel_info_t* channel, void* user_data);
        std::string _time_ago(uint32_t last_seen_ms);
        void _draw_battery_icon(int x, int y, uint8_t level, bool selected);

        // Views
        bool _render_devices();
        bool _render_chat();
        bool _render_devices_scrollbar(int panel_x, int panel_width);
        bool _render_scrolling_name();
        bool _handle_devices_navigation();
        bool _handle_chat_navigation();
        bool _render_devices_hint();
        bool _render_chat_hint();

        // System bar integration
        void _request_system_bar_update();
        // chat
        bool _chat_load_messages(int index);
        bool _chat_reload_messages();
        bool _chat_load_next();
        bool _chat_load_prev();
        // sort
        // bool _sort_last_seen(const DeviceItem& a, const DeviceItem& b);
        // bool _sort_signal(const DeviceItem& a, const DeviceItem& b);
        // bool _sort_name(const DeviceItem& a, const DeviceItem& b);
        // bool _sort_role(const DeviceItem& a, const DeviceItem& b);
        // bool _sort_hops(const DeviceItem& a, const DeviceItem& b);
        // bool _sort_battery(const DeviceItem& a, const DeviceItem& b);
        // assignable sort mode function
        bool (*_sort_mode_func)(const DeviceItem& a, const DeviceItem& b);

    public:
        void onCreate() override;
        void onResume() override;
        void onRunning() override;
        void onDestroy() override;
        void needRefresh();
        void wakeUpScreen();
        void goLastMessage();
        void playMessageSound() { _data.hal->playMessageSound(); }
        void playMessageSentSound() { _data.hal->playMessageSentSound(); }
        void ledYellowBlink() { _data.hal->led()->blink_once({255, 255, 0}, 50); }
        void ledBlueBlink() { _data.hal->led()->blink_once({0, 255, 255}, 50); }
        // void reloadChatMessages() { _chat_reload_messages(); }
    };

    class AppFlood_Packer : public APP_PACKER_BASE
    {
        std::string getAppName() override { return "FLOOD"; }
        std::string getAppDesc() override { return "Mesh chat by ESP-NOW"; }
        void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_flood_big, nullptr)); }
        void* newApp() override { return new AppFlood; }
        void deleteApp(void* app) override { delete (AppFlood*)app; }
    };
} // namespace MOONCAKE::APPS
