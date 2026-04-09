/**
 * @file draw_helper.h
 * @brief Shared UI drawing helpers
 */
#pragma once

#include <cstdint>
#include "lgfx/v1/LGFX_Sprite.hpp"

namespace UTILS
{
    namespace UI
    {
        void draw_scrollbar(LGFX_Sprite* canvas,
                            int x,
                            int y,
                            int width,
                            int height,
                            int total,
                            int visible,
                            int offset,
                            int min_thumb = 10,
                            int track_color = TFT_DARKGREY,
                            int thumb_color = TFT_ORANGE);

        /**
         * @brief Get a distinct badge color (RGB565 int) for a MAC address
         */
        int mac_badge_color(const uint8_t* mac);

        /**
         * @brief Get readable text color (TFT_BLACK or TFT_WHITE) for a given MAC badge
         */
        int mac_badge_text_color(const uint8_t* mac);

        /**
         * @brief Get color representing signal quality percentage (0-100)
         */
        int signal_quality_color(uint8_t quality);

        /**
         * @brief Draw a rounded node badge with name and color derived from MAC
         *
         * @return badge width in pixels
         */
        int draw_node_badge(LGFX_Sprite* canvas,
                            int x,
                            int y,
                            int h,
                            const char* label,
                            const uint8_t* mac);

    } // namespace UI
} // namespace UTILS
