/**
 * @file text_utils.cpp
 * @brief Shared UTF-8 text utilities implementation
 */

#include "text_utils.h"
#include <algorithm>
#include <format>
#include <time.h>
#include <cstdio>
#include "lgfx/v1/LGFXBase.hpp"

namespace UTILS
{
    namespace TEXT
    {
        std::vector<std::string> wrap_text(const std::string& text, int chars_per_line)
        {
            std::vector<std::string> lines;
            if (text.empty())
            {
                lines.push_back("");
                return lines;
            }

            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                if (nl != std::string::npos)
                {
                    int nl_chars = utf8_count(text, pos, nl);
                    if (nl_chars <= chars_per_line)
                    {
                        lines.push_back(text.substr(pos, nl - pos));
                        pos = nl + 1;
                        continue;
                    }
                }

                size_t end_byte = utf8_advance(text, pos, chars_per_line);

                if (end_byte < text.size() && text[end_byte] != '\n')
                {
                    size_t last_space = text.rfind(' ', end_byte);
                    if (last_space != std::string::npos && last_space > pos)
                    {
                        end_byte = last_space + 1;
                    }
                }

                lines.push_back(text.substr(pos, end_byte - pos));
                pos = end_byte;
            }
            return lines;
        }

        uint16_t count_wrapped_lines(const std::string& text, int chars_per_line)
        {
            if (text.empty())
                return 1;

            uint16_t lines = 0;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                if (nl != std::string::npos)
                {
                    int nl_chars = utf8_count(text, pos, nl);
                    if (nl_chars <= chars_per_line)
                    {
                        lines++;
                        pos = nl + 1;
                        continue;
                    }
                }

                size_t end_byte = utf8_advance(text, pos, chars_per_line);

                if (end_byte < text.size() && text[end_byte] != '\n')
                {
                    size_t last_space = text.rfind(' ', end_byte);
                    if (last_space != std::string::npos && last_space > pos)
                    {
                        end_byte = last_space + 1;
                    }
                }

                lines++;
                pos = end_byte;
            }
            return lines > 0 ? lines : 1;
        }

        static size_t find_px_break(const char* seg, size_t seg_pos, size_t seg_len,
                                    int max_width_px, lgfx::LGFXBase* gfx)
        {
            int32_t fit_bytes = gfx->textLength(seg + seg_pos, max_width_px);
            if (fit_bytes <= 0)
                return seg_pos + utf8_char_len((unsigned char)seg[seg_pos]);

            size_t byte_end = seg_pos;
            while (byte_end < seg_len)
            {
                int cl = utf8_char_len((unsigned char)seg[byte_end]);
                if (byte_end + cl - seg_pos > (size_t)fit_bytes)
                    break;
                byte_end += cl;
            }
            if (byte_end <= seg_pos)
                byte_end = seg_pos + utf8_char_len((unsigned char)seg[seg_pos]);
            if (byte_end > seg_len)
                byte_end = seg_len;

            if (byte_end < seg_len)
            {
                for (size_t i = byte_end; i > seg_pos; i--)
                {
                    if (seg[i - 1] == ' ')
                    {
                        byte_end = i;
                        break;
                    }
                }
            }
            return byte_end;
        }

        std::vector<std::string> wrap_text_px(const std::string& text, int max_width_px, lgfx::LGFXBase* gfx)
        {
            std::vector<std::string> lines;
            if (text.empty())
            {
                lines.push_back("");
                return lines;
            }

            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                size_t seg_end = (nl != std::string::npos) ? nl : text.size();

                if (seg_end == pos || gfx->textWidth(text.substr(pos, seg_end - pos).c_str()) <= max_width_px)
                {
                    lines.push_back(text.substr(pos, seg_end - pos));
                    pos = seg_end;
                    if (nl != std::string::npos)
                        pos++;
                    continue;
                }

                const char* seg = text.c_str() + pos;
                size_t seg_len = seg_end - pos;
                size_t seg_pos = 0;

                while (seg_pos < seg_len)
                {
                    size_t byte_end = find_px_break(seg, seg_pos, seg_len, max_width_px, gfx);
                    lines.push_back(text.substr(pos + seg_pos, byte_end - seg_pos));
                    seg_pos = byte_end;
                }

                pos = seg_end;
                if (nl != std::string::npos)
                    pos++;
            }
            return lines;
        }

        uint16_t count_wrapped_lines_px(const std::string& text, int max_width_px, lgfx::LGFXBase* gfx)
        {
            if (text.empty())
                return 1;

            uint16_t lines = 0;
            size_t pos = 0;
            while (pos < text.size())
            {
                size_t nl = text.find('\n', pos);
                size_t seg_end = (nl != std::string::npos) ? nl : text.size();

                if (seg_end == pos || gfx->textWidth(text.substr(pos, seg_end - pos).c_str()) <= max_width_px)
                {
                    lines++;
                    pos = seg_end;
                    if (nl != std::string::npos)
                        pos++;
                    continue;
                }

                const char* seg = text.c_str() + pos;
                size_t seg_len = seg_end - pos;
                size_t seg_pos = 0;

                while (seg_pos < seg_len)
                {
                    size_t byte_end = find_px_break(seg, seg_pos, seg_len, max_width_px, gfx);
                    lines++;
                    seg_pos = byte_end;
                }

                pos = seg_end;
                if (nl != std::string::npos)
                    pos++;
            }
            return lines > 0 ? lines : 1;
        }

        std::string format_timestamp(uint32_t timestamp)
        {
            struct tm ti;
            time_t ts = (time_t)timestamp;
            localtime_r(&ts, &ti);

            time_t now = time(nullptr);
            struct tm now_tm;
            localtime_r(&now, &now_tm);

            std::string time_str = std::format("{:02d}:{:02d}", ti.tm_hour, ti.tm_min);

            if (ti.tm_year == now_tm.tm_year && ti.tm_yday == now_tm.tm_yday)
                return time_str;

            int days_ago = (now_tm.tm_year - ti.tm_year) * 365 + now_tm.tm_yday - ti.tm_yday;
            if (days_ago > 0 && days_ago < 7)
            {
                static constexpr const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                return std::format("{} {}", days[ti.tm_wday], time_str);
            }

            if (ti.tm_year == now_tm.tm_year)
                return std::format("{:02d}.{:02d} {}", ti.tm_mday, ti.tm_mon + 1, time_str);

            return std::format("{:02d}.{:02d}.{:04d} {}",
                ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900, time_str);
        }

    } // namespace TEXT
} // namespace UTILS
