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

    } // namespace UI
} // namespace UTILS
