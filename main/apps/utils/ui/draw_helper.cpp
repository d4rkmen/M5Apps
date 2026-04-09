#include "draw_helper.h"
#include "flood.h"
#include <algorithm>
#include <cstring>

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
                            int min_thumb,
                            int track_color,
                            int thumb_color)
        {
            if (total <= visible)
                return;
            int thumb_h = std::max(min_thumb, height * visible / total);
            int thumb_y = y + (height - thumb_h) * offset / (total - visible);
            canvas->drawRect(x, y, width, height, track_color);
            canvas->fillRect(x, thumb_y, width, thumb_h, thumb_color);
        }

        int mac_badge_color(const uint8_t* mac)
        {
            return flood_get_device_color(mac);
        }

        int mac_badge_text_color(const uint8_t* mac)
        {
            return flood_get_device_text_color(mac);
        }

        int signal_quality_color(uint8_t quality)
        {
            if (quality >= 70)
                return TFT_GREEN;
            if (quality >= 40)
                return TFT_YELLOW;
            if (quality >= 15)
                return TFT_ORANGE;
            return TFT_RED;
        }

        int draw_node_badge(LGFX_Sprite* canvas,
                            int x,
                            int y,
                            int h,
                            const char* label,
                            const uint8_t* mac)
        {
            int w = canvas->textWidth(label) + 6;
            if (w < 20)
                w = 20;
            int bg = mac_badge_color(mac);
            int fg = mac_badge_text_color(mac);
            canvas->fillRoundRect(x, y, w, h, 4, bg);
            canvas->setTextColor(fg);
            canvas->drawCenterString(label, x + w / 2, y + 1);
            return w;
        }

    } // namespace UI
} // namespace UTILS
