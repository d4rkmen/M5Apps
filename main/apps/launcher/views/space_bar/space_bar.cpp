#include "../../launcher.h"
#include "lgfx/v1/lgfx_fonts.hpp"
#include "apps/utils/theme/theme_define.h"
#include "common_define.h"
#include "apps/utils/flash/ptable_tools.h"

#include "assets/full.h"
#include "assets/empty.h"

using namespace MOONCAKE::APPS;

void Launcher::_start_space_bar()
{
    // _data.hal->canvas_keyboard_bar()->fillScreen(TFT_RED);
    UTILS::FLASH_TOOLS::PartitionTable::initFlashUsagePercent();
}

void Launcher::_update_space_bar()
{
    if ((millis() - _data.space_bar_update_count) > _data.space_bar_update_preiod)
    {
        // Background
        int margin_x = 1;
        //(_data.hal->canvas_keyboard_bar()->width() - 16) / 2;
        int margin_y = 2;

        _data.hal->canvas_space_bar()->fillScreen(THEME_COLOR_BG);
        _data.hal->canvas_space_bar()->fillSmoothRoundRect(0,
                                                           0,
                                                           _data.hal->canvas_space_bar()->width(),
                                                           _data.hal->canvas_space_bar()->height(),
                                                           2,
                                                           THEME_COLOR_KB_BAR);
        // Draw icons
        _data.hal->canvas_space_bar()->pushImage(margin_x, margin_y, 16, 16, image_data_full);
        _data.hal->canvas_space_bar()->pushImage(margin_x,
                                                 _data.hal->canvas_space_bar()->height() - margin_y - 16 + 1,
                                                 16,
                                                 16,
                                                 image_data_empty);

        // Get flash usage info
        int flash_usage_percent = UTILS::FLASH_TOOLS::PartitionTable::getFlashUsagePercent();
        // Gauge dimensions
        int gauge_x = margin_x - 1;
        int gauge_y = margin_y + 16;
        int gauge_width = 18;
        int gauge_height = _data.hal->canvas_space_bar()->height() - margin_y * 2 - 16 - 16;

        if (flash_usage_percent != -1)
        {
            int segment_height = 4; // Height of each segment
            int segment_gap = 2;    // Gap between segments
            int total_segments = gauge_height / (segment_height + segment_gap) + 1;

            // Draw gauge background (segments)
            for (int i = 0; i < total_segments; i++)
            {
                int y = gauge_y + i * (segment_height + segment_gap);
                _data.hal->canvas_space_bar()->fillRect(gauge_x + 1, y, gauge_width - 2, segment_height, TFT_DARKGREY);
            }

            // Calculate filled segments
            int filled_segments = (total_segments * flash_usage_percent) / 100;

            // Choose color based on usage
            uint16_t fill_color;
            if (flash_usage_percent > 90)
                fill_color = TFT_RED;
            else if (flash_usage_percent > 70)
                fill_color = TFT_ORANGE;
            else if (flash_usage_percent > 50)
                fill_color = TFT_YELLOW;
            else
                fill_color = TFT_CYAN;

            // Draw filled segments from bottom up
            for (int i = total_segments - 1; i >= (total_segments - filled_segments); i--)
            {
                int y = gauge_y + i * (segment_height + segment_gap);
                _data.hal->canvas_space_bar()->fillRect(gauge_x + 1, y, gauge_width - 2, segment_height, fill_color);
            }
        }
        // Push
        _data.hal->canvas_space_bar_update();

        // Reset flag
        _data.space_bar_update_count = millis();
    }
}
