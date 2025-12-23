/**
 * @file system_bar.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "../../launcher.h"
#include "../menu/menu_render_callback.hpp"
#include "common_define.h"

#include "assets/bat1.h"
#include "assets/bat2.h"
#include "assets/bat3.h"
#include "assets/bat4.h"
#include "assets/wifi1.h"
#include "assets/wifi2.h"
#include "assets/wifi3.h"
#include "assets/wifi4.h"
#include "assets/wifi5.h"
#include "assets/wifi6.h"
#include "assets/usb1.h"
#include "flood.h"

using namespace MOONCAKE::APPS;

#define PADDING_X 4

void Launcher::_start_system_bar()
{
    // _data.hal->canvas_system_bar()->fillScreen(TFT_BLUE);
}

void Launcher::_update_system_bar()
{
    bool system_bar_force_update = _data.system_bar_force_update_flag && *_data.system_bar_force_update_flag;
    if (((millis() - _data.system_bar_update_count) > _data.system_bar_update_preiod) || system_bar_force_update)
    {
        // Reset force update flag
        if (system_bar_force_update)
        {
            *_data.system_bar_force_update_flag = false;
        }

        // Update state
        _update_system_state();

        // Backgound
        int margin_x = 5;
        int margin_y = 4;

        _data.hal->canvas_system_bar()->fillScreen(THEME_COLOR_BG);
        _data.hal->canvas_system_bar()->fillSmoothRoundRect(margin_x,
                                                            margin_y,
                                                            _data.hal->canvas_system_bar()->width() - margin_x * 2,
                                                            _data.hal->canvas_system_bar()->height() - margin_y * 2,
                                                            (_data.hal->canvas_system_bar()->height() - margin_y * 2) / 2,
                                                            THEME_COLOR_SYSTEM_BAR);

        int x = 10;
        int y = 5;

        // Flood running indicator
        bool flood_running = false;
        auto lc_list = mcAppGetFramework()->getAppManager().getAppLifecycleList();
        if (lc_list)
            for (auto& lc : *lc_list)
            {
                if (lc.app->getAppName() == "FLOOD")
                {
                    flood_running = true;
                    break;
                }
            }
        if (flood_running)
        {
            // Draw colored node identifier
            uint8_t our_mac[6];
            flood_get_our_mac(our_mac);
            int node_color = flood_get_device_color(our_mac);
            int node_text_color = flood_get_device_text_color(our_mac);
            std::string item_id = std::format("{:04x}", flood_get_device_id(our_mac));
            int short_width = 4 * 6 + 6;
            _data.hal->canvas_system_bar()->fillRoundRect(x, y + 1, short_width, 14, 4, node_color);
            _data.hal->canvas_system_bar()->setFont(FONT_12);
            _data.hal->canvas_system_bar()->setTextColor(node_text_color, node_color);
            _data.hal->canvas_system_bar()->drawCenterString(item_id.c_str(), x + short_width / 2, y + 1);
            x += short_width + PADDING_X;
        }
        else
        {
            // Wifi, show when no FLOOD
            uint16_t* image_data = nullptr;

            switch (_data.system_state.wifi_status)
            {
            case HAL::WIFI_STATUS_CONNECTED_STRONG:
                image_data = (uint16_t*)image_data_wifi1;
                break;
            case HAL::WIFI_STATUS_CONNECTED_GOOD:
                image_data = (uint16_t*)image_data_wifi2;
                break;
            case HAL::WIFI_STATUS_CONNECTED_WEAK:
                image_data = (uint16_t*)image_data_wifi3;
                break;
            case HAL::WIFI_STATUS_DISCONNECTED:
                image_data = (uint16_t*)image_data_wifi4;
                break;
            case HAL::WIFI_STATUS_CONNECTING:
                image_data = (uint16_t*)image_data_wifi5;
                break;
            case HAL::WIFI_STATUS_IDLE:
            default:
                image_data = (uint16_t*)image_data_wifi6;
                break;
            }
            _data.hal->canvas_system_bar()->pushImage(x, y, 16, 16, image_data, THEME_COLOR_ICON_16);
            x += 16 + PADDING_X;
        }
        // USB
        bool usb_connected = _data.hal->usb()->is_connected();
        if (usb_connected)
        {
            _data.hal->canvas_system_bar()->pushImage(x, y, 26, 16, image_data_usb1, THEME_COLOR_ICON_16);
            x += 26 + PADDING_X;
        }

        _data.hal->canvas_system_bar()->setFont(FONT_16);
        // Time
        bool show_time = _data.hal->settings()->getBool("system", "show_time");
        if (show_time)
        {
            _data.hal->canvas_system_bar()->setTextColor(THEME_COLOR_SYSTEM_BAR_TEXT);
            _data.hal->canvas_system_bar()->drawCenterString(_data.system_state.time.c_str(),
                                                             _data.hal->canvas_system_bar()->width() / 2 - 8,
                                                             _data.hal->canvas_system_bar()->height() / 2 - FONT_HEIGHT / 2 -
                                                                 1);
        }
        // Bat shit, comes last
        x = _data.hal->canvas_system_bar()->width() - 45;

        // Voltage
        bool show_voltage = _data.hal->settings()->getBool("system", "show_bat_volt");
        if (show_voltage)
        {
            _data.hal->canvas_system_bar()->setTextColor(TFT_BLACK);
            _data.hal->canvas_system_bar()->drawRightString(std::format("{:.1f}V", _data.system_state.voltage).c_str(),
                                                            x - 4,
                                                            _data.hal->canvas_system_bar()->height() / 2 - FONT_HEIGHT / 2 - 1);
            // _data.hal->canvas_system_bar()->drawRightString(
            //     std::format("{}", (uint32_t)(_data.system_state.voltage * 1000)).c_str(),
            //     x - 4,
            //     _data.hal->canvas_system_bar()->height() / 2 - FONT_HEIGHT / 2 - 1);
        }

        if (_data.system_state.bat_state == 1)
            _data.hal->canvas_system_bar()->pushImage(x, y, 32, 16, image_data_bat1, THEME_COLOR_ICON_16);
        else if (_data.system_state.bat_state == 2)
            _data.hal->canvas_system_bar()->pushImage(x, y, 32, 16, image_data_bat2, THEME_COLOR_ICON_16);
        else if (_data.system_state.bat_state == 3)
            _data.hal->canvas_system_bar()->pushImage(x, y, 32, 16, image_data_bat3, THEME_COLOR_ICON_16);
        else if (_data.system_state.bat_state == 4)
            _data.hal->canvas_system_bar()->pushImage(x, y, 32, 16, image_data_bat4, THEME_COLOR_ICON_16);

        // Push
        _data.hal->canvas_system_bar_update();

        // Reset flag
        _data.system_bar_update_count = millis();
    }
}
