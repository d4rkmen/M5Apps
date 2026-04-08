/**
 * @file launcher_render_callback.hpp
 * @author Forairaaaaa
 * @brief
 * @version 0.1
 * @date 2023-07-25
 *
 * @copyright Copyright (c) 2023
 *
 */
#pragma once
#include "hal.h"
#include "apps/utils/smooth_menu/src/smooth_menu.h"
#include "apps/utils/icon/icon_define.h"
#include "common_define.h"
#include "apps/utils/anim/scroll_text.h"
#include "apps/utils/anim/hl_text.h"
#include "lgfx/v1/lgfx_fonts.hpp"
#include "lgfx/v1/misc/enum.hpp"
#include <cstdint>
#include <cstdio>

#include "apps/utils/theme/theme_define.h"

#define MAX_TAG_LENGTH 10

using namespace UTILS::HL_TEXT;

struct LauncherRender_CB_t : public SMOOTH_MENU::SimpleMenuCallback_t
{
private:
    HAL::Hal* _hal;
    UTILS::HL_TEXT::HLTextContext_t _hint_text_ctx;
    UTILS::SCROLL_TEXT::ScrollTextContext_t _scroll_text_ctx;
    bool _hint_text_initialized = false;

public:
    // constructor
    LauncherRender_CB_t(HAL::Hal* hal)
    {
        _hal = hal;
        // init hint text context
        hl_text_init(&_hint_text_ctx, _hal->canvas(), 20, 1500);
        scroll_text_init(&_scroll_text_ctx,
                         _hal->canvas(),
                         MAX_TAG_LENGTH * _hal->canvas()->textWidth("0", FONT_16),
                         _hal->canvas()->fontHeight(FONT_16),
                         20,
                         1000);
    }
    // destructor
    ~LauncherRender_CB_t()
    {
        hl_text_free(&_hint_text_ctx);
        scroll_text_free(&_scroll_text_ctx);
    }

    void resetScroll() { scroll_text_reset(&_scroll_text_ctx); }
    /* Override render callback */
    void renderCallback(const std::vector<SMOOTH_MENU::Item_t*>& menuItemList,
                        const SMOOTH_MENU::RenderAttribute_t& selector,
                        const SMOOTH_MENU::RenderAttribute_t& camera) override
    {
        // Clear
        _hal->canvas()->fillScreen(THEME_COLOR_BG);

        // X offset (keep selector ar the center)
        int x_offset = -(selector.x) + _hal->canvas()->width() / 2 - ICON_WIDTH / 2;

        // Font
        _hal->canvas()->setFont(FONT_16);
        _hal->canvas()->setTextSize(1);
        _hal->canvas()->setTextColor(THEME_COLOR_ICON);

        // Render items
        for (const auto& item : menuItemList)
        {
            // Draw icon
            if (item->id == selector.targetItem)
            {
                // Icon bg
                _hal->canvas()->fillSmoothRoundRect(item->x - (ICON_SELECTED_WIDTH - item->width) / 2 + x_offset,
                                                    item->y - (ICON_SELECTED_WIDTH - item->height) / 2,
                                                    ICON_SELECTED_WIDTH,
                                                    ICON_SELECTED_WIDTH,
                                                    8,
                                                    THEME_COLOR_ICON);

                // Icon
                if (item->userData != nullptr)
                {
                    _hal->canvas()->pushImage(item->x - (ICON_SELECTED_WIDTH - item->width) / 2 + x_offset + 4,
                                              item->y - (ICON_SELECTED_WIDTH - item->height) / 2 + 4,
                                              56,
                                              56,
                                              ((AppIcon_t*)(item->userData))->iconBig);
                }

                // Draw tag
                if (item->tag.length() > MAX_TAG_LENGTH)
                {
                    scroll_text_render(&_scroll_text_ctx,
                                       item->tag.c_str(),
                                       item->x + x_offset + (item->width - MAX_TAG_LENGTH * _hal->canvas()->textWidth("0")) / 2,
                                       item->y + item->height + ICON_TAG_MARGIN_TOP + (ICON_SELECTED_WIDTH - item->width) / 2,
                                       THEME_COLOR_ICON,
                                       THEME_COLOR_BG,
                                       true);
                }
                else
                {
                    _hal->canvas()->drawCenterString(item->tag.c_str(),
                                                     item->x + item->width / 2 + x_offset,
                                                     item->y + item->height + ICON_TAG_MARGIN_TOP +
                                                         (ICON_SELECTED_WIDTH - item->width) / 2);
                }
            }
            else
            {
                // Icon bg
                _hal->canvas()
                    ->fillSmoothRoundRect(item->x + x_offset, item->y, item->width, item->height, 8, THEME_COLOR_ICON);

                // Icon
                if (item->userData != nullptr)
                {
                    if (((AppIcon_t*)(item->userData))->iconSmall == nullptr)
                    {
                        // no small image, render with resize to 40x40 from big image
                        _hal->canvas()->pushImageRotateZoom((float)(item->x + x_offset + 4),
                                                            (float)(item->y + 4),
                                                            0.0,
                                                            0.0,
                                                            0.0,
                                                            0.7,
                                                            0.7,
                                                            56,
                                                            56,
                                                            ((AppIcon_t*)(item->userData))->iconBig);
                    }
                    else
                    {
                        // have small image, render it
                        _hal->canvas()->pushImage(item->x + x_offset + 4,
                                                  item->y + 4,
                                                  40,
                                                  40,
                                                  ((AppIcon_t*)(item->userData))->iconSmall);
                    }
                }

                // Draw tag
                std::string tag = item->tag;
                if (tag.length() > 10)
                {
                    tag = tag.substr(0, 9) + ">";
                }
                _hal->canvas()->drawCenterString(tag.c_str(),
                                                 item->x + item->width / 2 + x_offset,
                                                 item->y + item->height + ICON_TAG_MARGIN_TOP);
            }
        }

        // Draw controls hint
        // Render highlight text animation at the bottom of the screen
        hl_text_render(&_hint_text_ctx,
                       "[I]NFO [<] SELECT [>] [ENTER]",
                       0,
                       _hal->canvas()->height() - 12,
                       TFT_DARKGREY,    // normal color
                       TFT_WHITE,       // highlight color
                       THEME_COLOR_BG); // background color
    }
};