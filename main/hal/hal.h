/**
 * @file hal.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-18
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include "board.h"
#include "LovyanGFX.h"
#include "i2c/i2c_master.h"
#include "keyboard/keyboard.h"
#include "bat/battery.h"
#include "sdcard/sdcard.h"
#include "button/button.h"
#include "speaker/speaker.h"
#include "es8311/es8311.h"
#include "usb/usb.h"
#include "wifi/wifi.h"
#include "led/led.h"
#include "settings/settings.h"
#include <iostream>
#include <string>
#include <ctime>

namespace HAL
{
    /**
     * @brief Hal base class
     *
     */
    class Hal
    {
    protected:
        LGFX_Device* _display;
        LGFX_Sprite* _canvas;
        LGFX_Sprite* _canvas_system_bar;
        LGFX_Sprite* _canvas_space_bar;

        SETTINGS::Settings* _settings;
        KEYBOARD::Keyboard* _keyboard;
        I2CMaster* _i2c;
        Battery* _battery;
        Speaker* _speaker;
        Button* _homeButton;
        SDCard* _sdcard;
        USB* _usb;
        WiFi* _wifi;
        LED* _led;
        ES8311* _es8311;
        bool _sntp_adjusted;
        bool _display_sleeping = false;
        BoardType _board_type;

    public:
        Hal(SETTINGS::Settings* settings)
            : _settings(settings), _keyboard(nullptr), _i2c(nullptr), _battery(nullptr), _speaker(nullptr),
              _homeButton(nullptr), _sdcard(nullptr), _usb(nullptr), _wifi(nullptr), _led(nullptr), _es8311(nullptr),
              _sntp_adjusted(false),
              _board_type(BoardType::AUTO_DETECT)
        {
        }

        // Getter
        inline LGFX_Device* display() { return _display; }
        inline LGFX_Sprite* canvas() { return _canvas; }
        inline LGFX_Sprite* canvas_system_bar() { return _canvas_system_bar; }
        inline LGFX_Sprite* canvas_space_bar() { return _canvas_space_bar; }
        inline SETTINGS::Settings* settings() { return _settings; }
        inline KEYBOARD::Keyboard* keyboard() { return _keyboard; }
        inline I2CMaster* i2c() { return _i2c; }
        inline Battery* bat() { return _battery; }
        inline SDCard* sdcard() { return _sdcard; }
        inline USB* usb() { return _usb; }
        inline Button* home_button() { return _homeButton; }
        inline Speaker* speaker() { return _speaker; }
        inline WiFi* wifi() { return _wifi; }
        inline LED* led() { return _led; }
        inline ES8311* es8311() { return _es8311; }

        inline void setSntpAdjusted(bool isAdjusted) { _sntp_adjusted = isAdjusted; }
        inline bool isSntpAdjusted(void) { return _sntp_adjusted || time(nullptr) > 1704067200; }
        inline BoardType board_type() const { return _board_type; }

        // Canvas
        inline void canvas_system_bar_update() { _canvas_system_bar->pushSprite(_canvas_space_bar->width(), 0); }
        inline void canvas_space_bar_update() { _canvas_space_bar->pushSprite(0, 0); }
        inline void canvas_update()
        {
            if (!_display_sleeping)
                _canvas->pushSprite(_canvas_space_bar->width(), _canvas_system_bar->height());
        }
        inline bool isDisplaySleeping() const { return _display_sleeping; }
        inline void displaySleep()
        {
            if (!_display_sleeping)
            {
                _display_sleeping = true;
                _display->sleep();
            }
        }
        inline void displayWakeup()
        {
            if (_display_sleeping)
            {
                _display_sleeping = false;
                _display->wakeup();
            }
        }

        // Override
        virtual std::string type() { return "null"; }
        virtual void init() {}

        virtual void playLastSound() {}
        virtual void playNextSound() {}
        virtual void playKeyboardSound() {}
        virtual void playErrorSound() {}
        virtual void playDeviceConnectedSound() {}
        virtual void playDeviceDisconnectedSound() {}
        virtual void playMessageSound() {}
        virtual void playMessageSentSound() {}
        virtual void reboot() {}
        virtual void flash_activity() {}

        virtual uint8_t getBatLevel(float voltage) { return 100; }
        virtual float getBatVoltage() { return 4.15; }
    };
} // namespace HAL
