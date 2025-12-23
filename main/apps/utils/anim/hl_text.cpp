/**
 * @file hl_text.cpp
 * @brief Implementation of text highlighting animation utility
 * @version 0.1
 * @date 2024-06-18
 *
 * @copyright Copyright (c) 2024
 *
 */
#include "hl_text.h"
#include "common_define.h"
#include "../theme/theme_define.h"
#include <string.h>

namespace UTILS
{
    namespace HL_TEXT
    {
        bool hl_text_init(HLTextContext_t* ctx, lgfx::LovyanGFX* canvas, uint32_t speed_ms, uint32_t delay_ms)
        {
            if (!ctx || !canvas)
                return false;

            // Initialize context
            ctx->sprite = new LGFX_Sprite(canvas);
            if (!ctx->sprite)
                return false;
            ctx->sprite->createSprite(canvas->width(), ctx->sprite->fontHeight(FONT_10));
            ctx->sprite->setFont(FONT_10);
            ctx->sprite->setTextSize(1);
            ctx->canvas = canvas;
            ctx->animation_speed = speed_ms;
            ctx->animation_delay = delay_ms;
            ctx->current_char_index = -1;
            ctx->last_update_time = millis();
            ctx->timeout = delay_ms;
            ctx->is_rendered = false;

            return true;
        }

        bool hl_text_render(
            HLTextContext_t* ctx, const char* text, int x, int y, int normal_color, int highlight_color, uint32_t bg_color)
        {
            if (!ctx)
                return false;
            // Update animation state if needed
            uint32_t now = millis();
            bool updated = false;

            if (!ctx->is_rendered)
            {
                // Draw the full text in normal color
                ctx->sprite->fillScreen(bg_color);
                ctx->sprite->setTextColor(normal_color, bg_color);
                ctx->sprite->drawCenterString(text, ctx->sprite->width() / 2, 0);

                // check if text length is less then current_char_index
                if (ctx->current_char_index >= strlen(text))
                    ctx->current_char_index = -1;
                // If there's a character to highlight
                if (ctx->current_char_index >= 0)
                {
                    char highlighted_char[2] = {text[ctx->current_char_index], '\0'};
                    ctx->sprite->setTextColor(highlight_color, bg_color);
                    // Calculate position for the single character
                    int char_width = ctx->sprite->textWidth(text);
                    int start_x = ctx->sprite->width() / 2 - char_width / 2;
                    int char_pos = ctx->current_char_index * ctx->sprite->textWidth("0");
                    ctx->sprite->drawString(highlighted_char, start_x + char_pos, 0);
                }
                ctx->sprite->pushSprite(x, y, bg_color);
                updated = true;
            }

            // calculate next character index
            if (now - ctx->last_update_time > ctx->timeout)
            {

                ctx->current_char_index++;

                // If we've reached the end of the text, reset
                if (text[ctx->current_char_index] == '\0')
                {
                    ctx->current_char_index = -1;
                    ctx->timeout = ctx->animation_delay;
                }
                else
                {
                    ctx->timeout = ctx->animation_speed;
                }

                ctx->last_update_time = now;
                ctx->is_rendered = false;
                // push sprite to canvas
            }

            return updated;
        }

        void hl_text_reset(HLTextContext_t* ctx)
        {
            if (!ctx)
                return;

            ctx->current_char_index = -1;
            ctx->last_update_time = millis();
            ctx->timeout = ctx->animation_delay;
            ctx->is_rendered = false;
        }

        void hl_text_free(HLTextContext_t* ctx)
        {
            if (!ctx)
                return;
            if (ctx->sprite)
            {
                ctx->sprite->deleteSprite();
                delete ctx->sprite;
            }
        }
    } // namespace HL_TEXT
} // namespace UTILS