/**
 * @file launcher.cpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "launcher.h"
#include "hal/hal.h"
#include "mc_conf_internal.h"
#include "esp_log.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "common_define.h"
#include "apps/utils/flash/flash_tools.h"
#include "apps/utils/ui/dialog.h"
#include "apps/utils/screenshot/screenshot_tools.h"
#include "esp_partition.h"
#include "wifi/wifi.h"
#include <format>
#include "flood.h"
#include <ctime>

static const char* TAG = "APP_LAUNCHER";
#define KEY_REPEAT_MS 200
#include "apps/utils/ui/key_repeat.h"

#define BAT_UPDATE_INTERVAL 30000

static bool is_repeat = false;
static uint32_t next_fire_ts = 0;

// extern const uint8_t macbook_wav_start[] asm("_binary_macbook_wav_start");
// extern const uint8_t macbook_wav_end[] asm("_binary_macbook_wav_end");
extern const uint8_t boot_sound_wav_start[] asm("_binary_boot_sound_wav_start");
extern const uint8_t boot_sound_wav_end[] asm("_binary_boot_sound_wav_end");
extern const uint8_t clock_wav_start[] asm("_binary_clock_wav_start");
extern const uint8_t clock_wav_end[] asm("_binary_clock_wav_end");

using namespace MOONCAKE::APPS;

void Launcher::onCreate()
{
    // Get hal
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    _data.system_bar_force_update_flag = mcAppGetDatabase()->Get("SYSTEM_BAR_FORCE_UPDATE")->value<bool*>();
    // settings
    _data.hal->speaker()->setVolume(_data.hal->settings()->getNumber("system", "volume"));
    _data.hal->keyboard()->setDimmed(false);
    _data.hal->keyboard()->set_dim_time(_data.hal->settings()->getNumber("system", "dim_time") * 1000);

    _data.hal->keyboard()->setDimmedCallback(
        [this](bool dimmed)
        {
            if (!dimmed)
            {
                ESP_LOGD(TAG, "Screen on");
                _data.hal->displayWakeup();
                _data.hal->display()->setBrightness(_data.hal->settings()->getNumber("system", "brightness"));
            }
            else
            {
                ESP_LOGD(TAG, "Screen off");
            }
        });

    // Initialize WiFi module
    if (_data.hal->wifi()->init())
    {
        // Connect to WiFi if enabled
        if (_data.hal->settings()->getBool("wifi", "enabled"))
        {
            _data.hal->wifi()->connect();
        }
    }
    // Init
    _boot_anim();
    _start_menu();
    _start_system_bar();
    _start_space_bar();

    // Allow background running
    setAllowBgRunning(true);
    // Auto start
    startApp();
}
// onResume
void Launcher::onResume() { _stop_repeat(); }

// onRunning
void Launcher::onRunning()
{
    _update_menu();
    _update_system_bar();
    _update_space_bar();
    _update_keyboard_state();
}

void Launcher::onRunningBG()
{
    // If only launcher standing still
    if (mcAppGetFramework()->getAppManager().getCreatedAppNum() == 1)
    {
        // Close anim
        ANIM_APP_CLOSE();

        // Back to business
        mcAppGetFramework()->startApp(this);
    }

    _update_system_bar();
    _update_space_bar();
    _update_keyboard_state();
}

void Launcher::_init_progress_bar()
{
    _data.progress_bar = new LGFX_Sprite(_data.hal->display());
    _data.progress_bar->createSprite(_data.hal->display()->width(), 16);
}

void Launcher::_delete_progress_bar()
{
    _data.progress_bar->deleteSprite();
    delete _data.progress_bar;
    _data.progress_bar = nullptr;
}

void Launcher::_render_countdown_progress(int percent)
{
    // Progress bar dimensions - full width at bottom of screen
    int bar_h = _data.progress_bar->height();               // Height of the progress bar
    int bar_w = _data.progress_bar->width();                // Full width
    int bar_y = _data.hal->display()->height() - bar_h - 1; // Position at bottom

    // Draw progress bar frame
    _data.progress_bar->drawRect(0, 0, bar_w, bar_h, THEME_COLOR_BG_SELECTED);

    // Calculate fill width based on percentage
    int fill_width = (percent * bar_w) / 100;
    if (fill_width > 0)
    {
        _data.progress_bar->fillRect(0, 1, fill_width, bar_h - 2, THEME_COLOR_BG_SELECTED);
    }

    _data.progress_bar->pushSprite(_data.hal->display(), 0, bar_y);
}

void Launcher::_render_wait_progress()
{
    // Progress bar dimensions - full width at bottom of screen
    int bar_h = _data.progress_bar->height();               // Height of the progress bar
    int bar_w = _data.progress_bar->width();                // Full width
    int bar_y = _data.hal->display()->height() - bar_h - 1; // Position at bottom

    // Draw progress bar frame
    _data.progress_bar->fillRect(0, 0, bar_w, bar_h, TFT_BLACK);
    _data.progress_bar->drawRect(0, 0, bar_w, bar_h, THEME_COLOR_BG_SELECTED);

    // Create a pattern for the pending animation
    static int pattern_offset = 0;
    pattern_offset = (pattern_offset + 1) % bar_h;

    // Draw the pattern (diagonal stripes)
    for (int x = -bar_h; x < bar_w; x += bar_h)
    {
        for (int w = 0; w < 4; w++)
            _data.progress_bar->drawLine(x + pattern_offset + w,
                                         1,
                                         x + pattern_offset + w + bar_h - 2,
                                         1 + bar_h - 2,
                                         THEME_COLOR_BG_SELECTED);
    }

    _data.progress_bar->pushSprite(_data.hal->display(), 0, bar_y);
}

void Launcher::_boot_message(const std::string& message)
{
    _data.hal->display()->drawCenterString(message.c_str(),
                                           _data.hal->display()->width() / 2,
                                           _data.hal->display()->height() - 32 - 2);
}

void Launcher::_wait_enter()
{
    // init display
    _data.hal->display()->setFont(FONT_16);
    _data.hal->display()->setTextSize(1);
    _data.hal->display()->setTextColor(TFT_LIGHTGREY);

    _data.hal->keyboard()->updateKeyList();
    bool has_boot_sound = _data.hal->settings()->getBool("system", "boot_sound");
    if (_data.hal->keyboard()->isPressed())
    {
        if (has_boot_sound)
            _data.hal->playErrorSound();
        ESP_LOGI(TAG, "key pressed, entering menu");
        _boot_message("key pressed, start cancelled");
        delay(1500);
        while (_data.hal->keyboard()->keyList().size())
            _data.hal->keyboard()->updateKeyList();
        return;
    }

    ESP_LOGI(TAG, "Searching for bootable partition");
    const esp_partition_t* ota_partition = esp_ota_get_boot_partition();
    ESP_LOGI(TAG,
             "%s",
             ota_partition == nullptr ? "NOT FOUND" : std::format("FOUND: {}", (char*)&ota_partition->label).c_str());
    // factory means apps partition, skipping
    bool has_bootable_app = UTILS::FLASH_TOOLS::is_partition_bootable(ota_partition) &&
                            (ota_partition->subtype != ESP_PARTITION_SUBTYPE_APP_FACTORY) &&
                            _data.hal->settings()->getBool("system", "last_app");
    uint32_t timeout = _data.hal->settings()->getNumber("system", "last_app_to") * 1000;
    // check is it current boot partition
    if (has_bootable_app)
    {
        if (has_boot_sound)
            _data.hal->speaker()->playWav(clock_wav_start, clock_wav_end - clock_wav_start, 100);
        _boot_message(std::format("starting {}...", (char*)&ota_partition->label));
        ESP_LOGI(TAG, "Has bootable app in: %s, waiting for any key for %ldms", (char*)&ota_partition->label, timeout);
    }
    else
    {
        if (has_boot_sound)
            _data.hal->speaker()->playWav(boot_sound_wav_start, boot_sound_wav_end - boot_sound_wav_start);
        ESP_LOGI(TAG, "No bootable app, waiting for any key");
        _boot_message("press any key");
    }

#if 0
    // display raw color RED, GREEN, BLUE, BLACK, WHITE in a loop.
    uint32_t colors[] = {0xFF0000, 0x00FF00, 0x0000FF, 0x000000, 0xFFFFFF};
    while (1)
    {
        for (int i = 0; i < 5; i++)
        {
            _data.hal->display()->fillRect(0, 0, _data.hal->display()->width(), _data.hal->display()->height(), colors[i]);
            delay(2000);
        }
    }
#endif
    _init_progress_bar();

    uint32_t start_time = millis();
    uint32_t elapsed;
    bool need_restart = has_bootable_app;
    while (!has_bootable_app || (start_time + timeout) > millis())
    {
        // Render the progress bar
        if (has_bootable_app)
        {
            elapsed = millis() - start_time;
            // Calculate progress percentage (0-100)
            int progress = (elapsed * 100) / timeout;
            progress = std::min(progress, 100); // Ensure we don't exceed 100%
            _render_countdown_progress(progress);
        }
        else
        {
            _render_wait_progress();
        }

        // _data.hal->keyboard()->updateKeyList();
        _update_keyboard_state();
        if (_data.hal->keyboard()->keyList().size())
        {
            _data.hal->playNextSound();
            // Hold till release
            while (_data.hal->keyboard()->keyList().size())
                _data.hal->keyboard()->updateKeyList();
            // continue to run Launcher menu
            need_restart = false;
            break;
        }

        delay(50);
    }

    _delete_progress_bar();

    if (need_restart)
    {
        ESP_LOGI(TAG, "Starting app from: %s", (char*)&ota_partition->label);
        _data.hal->reboot();
    }
    if (has_boot_sound && has_bootable_app)
    {
        // stop the clock ticking sound
        _data.hal->speaker()->stop();
    }
}

void Launcher::_stop_repeat()
{
    is_repeat = false;
    next_fire_ts = 0;
}

bool Launcher::_check_next_pressed()
{
    bool pressed = _data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN);
    if (pressed && key_repeat_check(is_repeat, next_fire_ts, millis()))
    {
        _data.hal->playNextSound();
        return true;
    }
    return false;
}

bool Launcher::_check_last_pressed()
{
    bool pressed = _data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT) || _data.hal->keyboard()->isKeyPressing(KEY_NUM_UP);
    if (pressed && key_repeat_check(is_repeat, next_fire_ts, millis()))
    {
        _data.hal->playNextSound();
        return true;
    }
    return false;
}

bool Launcher::_check_info_pressed()
{
    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_I))
    {
        // Hold till release
        while (_data.hal->keyboard()->isKeyPressing(KEY_NUM_I))
        {
            _data.menu->update(millis());
            _data.hal->canvas_update();
            _data.hal->keyboard()->updateKeyList();
        }
        return true;
    }

    return false;
}

bool Launcher::_check_enter_pressed()
{
    if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
    {
        _data.hal->playLastSound();
        // Hold till release
        while (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.menu->update(millis());
            _data.hal->canvas_update();
            _data.hal->keyboard()->updateKeyList();
        }

        return true;
    }

    return false;
}

void Launcher::_update_keyboard_state()
{
    // Update key list (keyboard owns dim timeout logic internally)
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    // Dimming slowly, then put display to sleep for power saving
    uint32_t now = millis();
    static uint32_t last_dim_step_time = 0;
    if (now - last_dim_step_time > 50)
    {
        last_dim_step_time = now;
        auto brightness = _data.hal->display()->getBrightness();
        if (_data.hal->keyboard()->isDimmed() && brightness > 0)
        {
            uint8_t dim_step = 5;
            uint8_t new_brightness = brightness > dim_step ? brightness - dim_step : 0;
            _data.hal->display()->setBrightness(new_brightness);
            if (new_brightness == 0)
                _data.hal->displaySleep();
        }
    }
    // Check for screenshot key combination: CTRL + SPACE
    UTILS::SCREENSHOT_TOOLS::check_and_handle_screenshot(_data.hal, _data.system_bar_force_update_flag);
}

uint32_t _bat_update_time_count = 0;

void Launcher::_update_system_state()
{
    // brightness
    int32_t brightness = _data.hal->display()->getBrightness();
    int32_t new_brightness = _data.hal->settings()->getNumber("system", "brightness");
    if (!_data.hal->keyboard()->isDimmed() && brightness != new_brightness)
    {
        _data.hal->display()->setBrightness(new_brightness);
    }
    // volume
    int32_t volume = _data.hal->speaker()->getVolume();
    int32_t new_volume = _data.hal->settings()->getNumber("system", "volume");
    if (volume != new_volume)
    {
        _data.hal->speaker()->setVolume(new_volume);
    }
    // USB host
    bool usb_host_enabled = _data.hal->usb()->is_initialized();
    bool new_usb_host_enabled = _data.hal->settings()->getBool("system", "usb_host");
    if (usb_host_enabled != new_usb_host_enabled)
    {
        if (new_usb_host_enabled)
        {
            _data.hal->usb()->init();
        }
        else
        {
            _data.hal->usb()->deinit();
        }
    }
    // Time shit
    if (_data.hal->isSntpAdjusted())
    {
        static time_t now;
        static struct tm timeinfo;
        time(&now);
        localtime_r(&now, &timeinfo);

        _data.system_state.time = std::format("{:02d}:{:02d}", timeinfo.tm_hour, timeinfo.tm_min);
    }
    else
    {
        // Fake time
        _data.system_state.time =
            std::format("{:02d}:{:02d}", (int)((millis() / 3600000) % 60), (int)((millis() / 60000) % 60));
    }

    // Bat shit
    if ((millis() - _bat_update_time_count) > BAT_UPDATE_INTERVAL || _bat_update_time_count == 0)
    {
        _data.system_state.voltage = _data.hal->getBatVoltage();
        _data.system_state.bat_level = _data.hal->getBatLevel(_data.system_state.voltage);
        if (_data.system_state.bat_level >= 75)
            _data.system_state.bat_state = 1;
        else if (_data.system_state.bat_level >= 50)
            _data.system_state.bat_state = 2;
        else if (_data.system_state.bat_level >= 25)
            _data.system_state.bat_state = 3;
        else
            _data.system_state.bat_state = 4;

        _bat_update_time_count = millis();
    }
    // wifi
    _data.system_state.wifi_status = _data.hal->wifi()->get_status();
}
