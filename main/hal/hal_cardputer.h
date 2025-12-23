/**
 * @file hal_cardputer.h
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-09-22
 *
 * @copyright Copyright (c) 2023
 *
 */
#include "hal.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define delay(ms) vTaskDelay(pdMS_TO_TICKS(ms))

#define RGB_LED_GPIO 21

extern const uint8_t usb_connected_wav_start[] asm("_binary_usb_connected_wav_start");
extern const uint8_t usb_connected_wav_end[] asm("_binary_usb_connected_wav_end");
extern const uint8_t usb_disconnected_wav_start[] asm("_binary_usb_disconnected_wav_start");
extern const uint8_t usb_disconnected_wav_end[] asm("_binary_usb_disconnected_wav_end");
extern const uint8_t error_wav_start[] asm("_binary_error_wav_start");
extern const uint8_t error_wav_end[] asm("_binary_error_wav_end");

namespace HAL
{
    class HalCardputer : public Hal
    {
    private:
        void _init_i2c();
        void _init_display();
        void _init_keyboard();
        void _init_speaker();
        void _init_button();
        void _init_bat();
        void _init_sdcard();
        void _init_usb();
        void _init_wifi();
        void _init_led();

    public:
        HalCardputer(SETTINGS::Settings* settings) : Hal(settings) {}
        std::string type() override
        {
            switch (_board_type)
            {
            case HAL::BoardType::CARDPUTER:
                return "v1.x";
            case HAL::BoardType::CARDPUTER_ADV:
                return "ADV";
            default:
                return "unknown";
            }
        }
        void init() override;
        void playErrorSound() override { _speaker->playWav(error_wav_start, error_wav_end - error_wav_start); }
        void playKeyboardSound() override { _speaker->tone(5000, 20); }
        void playLastSound() override { _speaker->tone(6000, 20); }
        void playNextSound() override { _speaker->tone(7000, 20); }
        void playMessageSound() override
        {
            _speaker->tone(1633, 60);
            delay(50);
            _speaker->tone(1209, 60);
        }

        void playMessageSentSound() override
        {
            _speaker->tone(616, 60);
            delay(60);
            _speaker->tone(616, 60);
        }

        void playDeviceConnectedSound() override
        {
            _speaker->playWav(usb_connected_wav_start, usb_connected_wav_end - usb_connected_wav_start);
        }
        void playDeviceDisconnectedSound() override
        {
            _speaker->playWav(usb_disconnected_wav_start, usb_disconnected_wav_end - usb_disconnected_wav_start);
        }
        uint8_t getBatLevel(float voltage) override;
        float getBatVoltage() override;

    public:
    };
} // namespace HAL
