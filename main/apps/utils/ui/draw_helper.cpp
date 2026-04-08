#include "draw_helper.h"
#include <algorithm>

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

    } // namespace UI
} // namespace UTILS
