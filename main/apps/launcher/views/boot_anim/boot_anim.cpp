/**
 * @file boot_anim.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "apps/launcher/launcher.h"
#include "lgfx/v1/misc/enum.hpp"
#include "esp_log.h"
#include "apps/utils/theme/theme_define.h"
#include "common_define.h"

using namespace MOONCAKE::APPS;

extern const uint8_t boot_logo_start[] asm("_binary_boot_logo_png_start");
extern const uint8_t boot_logo_end[] asm("_binary_boot_logo_png_end");

void Launcher::_boot_anim()
{

    // Show logo
    // * _data.hal->display()->pushImage(0, 0, 240, 135, image_data_logo);
    _data.hal->display()->drawPng(boot_logo_start, boot_logo_end - boot_logo_start);
    // Show version
    const int32_t pos_x = _data.hal->display()->width() - 4;
    const int32_t pos_y = _data.hal->display()->height() / 2;
    _data.hal->display()->setFont(FONT_12);
    _data.hal->display()->setTextColor(TFT_DARKGREY, TFT_BLACK);
    _data.hal->display()->drawRightString(_data.hal->type().c_str(), pos_x, pos_y);
    _data.hal->display()->setFont(FONT_16);
    _data.hal->display()->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    _data.hal->display()->drawRightString("M5Apps v" BUILD_NUMBER, pos_x, pos_y + 14);
    delay(500);
    // If software restart
    if (esp_reset_reason() != ESP_RST_POWERON)
        return;
    _wait_enter();
}
