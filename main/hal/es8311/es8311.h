/**
 * @file es8311.h
 * @brief ES8311 audio codec driver (shared between speaker and mic)
 */
#pragma once

#include <cstdint>
#include "driver/i2c_master.h"

#define ES8311_I2C_ADDR 0x18
#define ES8311_I2C_TIMEOUT_MS 100

namespace HAL
{
    class Hal;

    class ES8311
    {
    public:
        ES8311(Hal* hal);
        ~ES8311();

        bool init();

        bool speaker_enable();
        void speaker_disable();
        void speaker_mute();
        void speaker_unmute();

        bool mic_enable();
        void mic_disable();

    private:
        Hal* _hal;
        i2c_master_dev_handle_t _dev_handle = nullptr;

        bool _write_reg(uint8_t reg, uint8_t val);
        bool _write_regs(const uint8_t (*pairs)[2], size_t count);
    };

} // namespace HAL
