/**
 * @file es8311.cpp
 * @brief ES8311 audio codec driver (shared between speaker and mic)
 *
 * Pop/click suppression strategy (derived from Linux ALSA driver + ESP-ADF):
 *  - DAC soft-ramp enabled via reg 0x37 bits[7:4] (0.25 dB / 16 LRCK)
 *  - ADC soft-ramp enabled via reg 0x15 bits[7:4]
 *  - SDP data-path muted (regs 0x09, 0x0A bit 6) during power transitions
 *  - VMID reference kept warm on disable (VMIDSEL=2) to avoid cold-start pop
 */
#include "es8311.h"
#include "hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ES8311";

namespace HAL
{
    ES8311::ES8311(Hal* hal) : _hal(hal) {}

    ES8311::~ES8311()
    {
        if (_dev_handle && _hal && _hal->i2c())
        {
            _hal->i2c()->remove_device(_dev_handle);
            _dev_handle = nullptr;
        }
    }

    bool ES8311::init()
    {
        if (!_hal || !_hal->i2c() || !_hal->i2c()->is_initialized())
        {
            ESP_LOGE(TAG, "I2C not available");
            return false;
        }

        esp_err_t err = _hal->i2c()->add_device(ES8311_I2C_ADDR, 400000, nullptr, &_dev_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to add I2C device: %s", esp_err_to_name(err));
            return false;
        }

        ESP_LOGI(TAG, "Initialized");
        return true;
    }

    // ── Speaker (DAC) ──────────────────────────────────────────────

    bool ES8311::speaker_enable()
    {
        static const uint8_t regs[][2] = {
            {0x00, 0x80}, // RESET: CSM power on
            {0x01, 0xB5}, // CLK_MANAGER: MCLK=BCLK, BCLK_ON, CLKDAC_ON, ANACLKDAC_ON
            {0x02, 0x18}, // CLK_MANAGER: MULT_PRE=3
            {0x09, 0x40}, // SDP_IN: mute DAC data path during power-up
            {0x0D, 0x01}, // SYS3: power up analog, VMIDSEL=startup normal speed
            {0x12, 0x00}, // SYS8: power-up DAC
            {0x13, 0x10}, // SYS9: enable output to HP driver
            {0x32, 0x00}, // DAC2: volume muted (unmuted later via speaker_unmute)
            {0x37, 0x48}, // DAC6: soft-ramp rate=4 (0.25dB/16LRCK), EQ bypass
        };

        if (!_write_regs(regs, sizeof(regs) / sizeof(regs[0])))
        {
            ESP_LOGE(TAG, "speaker_enable failed");
            return false;
        }
        ESP_LOGD(TAG, "Speaker enabled (DAC muted, ramp on)");
        return true;
    }

    void ES8311::speaker_disable()
    {
        _write_reg(0x32, 0x00); // DAC2: volume → 0 (soft-ramped)
        _write_reg(0x09, 0x40); // SDP_IN: mute DAC data path
        vTaskDelay(pdMS_TO_TICKS(10));
        _write_reg(0x12, 0x02); // SYS8: power-down DAC
        ESP_LOGD(TAG, "Speaker disabled");
    }

    void ES8311::speaker_mute()
    {
        _write_reg(0x32, 0x00); // DAC2: volume → 0 (soft-ramped)
        _write_reg(0x09, 0x40); // SDP_IN: mute data path
    }

    void ES8311::speaker_unmute()
    {
        _write_reg(0x09, 0x00); // SDP_IN: unmute data path
        _write_reg(0x32, 0xBF); // DAC2: volume (soft-ramped up)
    }

    // ── Mic (ADC) ──────────────────────────────────────────────────

    bool ES8311::mic_enable()
    {
        static const uint8_t regs[][2] = {
            {0x00, 0x80}, // RESET: CSM power on
            {0x01, 0xBA}, // CLK_MANAGER: MCLK=BCLK, BCLK_ON, CLKADC_ON, ANACLKADC_ON
            {0x02, 0x18}, // CLK_MANAGER: MULT_PRE=3
            {0x0A, 0x40}, // SDP_OUT: mute ADC output during power-up
            {0x0D, 0x01}, // SYS3: power up analog, VMIDSEL=startup normal speed
            {0x0E, 0x02}, // SYS4: enable analog PGA, enable ADC modulator
            {0x14, 0x10}, // SYS10: select Mic1p-Mic1n, PGA gain minimum
            {0x15, 0x40}, // ADC1: soft-ramp rate=4 (0.25dB/16LRCK)
            {0x17, 0xBF}, // ADC3: volume 0 dB
            {0x1C, 0x6A}, // ADC8: EQ bypass, HPF on, cancel DC offset
        };

        if (!_write_regs(regs, sizeof(regs) / sizeof(regs[0])))
        {
            ESP_LOGE(TAG, "mic_enable failed");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
        _write_reg(0x0A, 0x00); // SDP_OUT: unmute ADC output after settling

        ESP_LOGI(TAG, "Mic enabled");
        return true;
    }

    void ES8311::mic_disable()
    {
        static const uint8_t regs[][2] = {
            {0x17, 0x00}, // ADC3: volume mute (soft-ramped)
            {0x0A, 0x40}, // SDP_OUT: mute ADC output
            {0x0E, 0x6A}, // SYS4: power down PGA + ADC modulator
            {0x0D, 0xFA}, // SYS3: power down analog blocks, keep Vref + VMIDSEL=2 warm
        };
        _write_regs(regs, sizeof(regs) / sizeof(regs[0]));
        ESP_LOGI(TAG, "Mic disabled");
    }

    // ── Internals ──────────────────────────────────────────────────

    bool ES8311::_write_reg(uint8_t reg, uint8_t val)
    {
        if (!_dev_handle)
            return false;
        const uint8_t buf[2] = {reg, val};
        esp_err_t err = i2c_master_transmit(_dev_handle, buf, 2, ES8311_I2C_TIMEOUT_MS);
        if (err != ESP_OK)
        {
            ESP_LOGW(TAG, "Reg 0x%02X write failed: %s", reg, esp_err_to_name(err));
            return false;
        }
        return true;
    }

    bool ES8311::_write_regs(const uint8_t (*pairs)[2], size_t count)
    {
        for (size_t i = 0; i < count; i++)
        {
            if (!_write_reg(pairs[i][0], pairs[i][1]))
                return false;
        }
        return true;
    }

} // namespace HAL
