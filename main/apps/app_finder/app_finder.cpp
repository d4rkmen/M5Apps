/**
 * @file app_finder.cpp
 * @brief File Manager app implementation
 * @version 0.1
 * @date 2025-01-09
 *
 * @copyright Copyright (c) 2025
 *
 */
#include "app_finder.h"
#include "esp_log.h"
#include "apps/utils/theme/theme_define.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <memory>
#include <cstdio>
#include "apps/utils/ui/dialog.h"
#include "apps/utils/ui/draw_helper.h"
#include "apps/utils/ui/key_repeat.h"
#include "apps/utils/flash/ptable_tools.h"

static const char* TAG = "APP_FINDER";

// Constants optimized for FONT_12
#define LIST_SCROLL_PAUSE 1000
#define LIST_SCROLL_SPEED 25
#define LIST_MAX_VISIBLE_ITEMS 5  // More items with FONT_12
#define LIST_MAX_DISPLAY_CHARS 14 // More chars with FONT_12
#define PATH_SCROLL_PAUSE 500
#define PATH_SCROLL_SPEED 10
#define PATH_MAX_DISPLAY_CHARS 18

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;
// static const char* HINT_PANELS = "[TAB] [UP] [DOWN] [ENTER] [HOME]";
static const char* HINT_PANELS = "[5]COPY [6]MOVE [7]MD [8]DEL [TAB] [ESC]";

using namespace MOONCAKE::APPS;
using namespace UTILS::SCROLL_TEXT;
using namespace UTILS::FLASH_TOOLS;

static bool is_same_mountpoint(const std::string& path1, const std::string& path2)
{
    return path1.substr(0, path1.find('/', 1)) == path2.substr(0, path2.find('/', 1));
}

void AppFinder::onCreate()
{
    // Get hal
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();

    // Initialize scroll contexts for both panels
    scroll_text_init_ex(&_data.left_panel.list_scroll_ctx,
                        _data.hal->canvas(),
                        LIST_MAX_DISPLAY_CHARS * 6, // FONT_12 width
                        12,                         // FONT_12 height
                        LIST_SCROLL_SPEED,
                        LIST_SCROLL_PAUSE,
                        FONT_12);
    scroll_text_init_ex(&_data.left_panel.path_scroll_ctx,
                        _data.hal->canvas(),
                        PATH_MAX_DISPLAY_CHARS * 6,
                        12,
                        PATH_SCROLL_SPEED,
                        PATH_SCROLL_PAUSE,
                        FONT_12);
    scroll_text_init_ex(&_data.right_panel.list_scroll_ctx,
                        _data.hal->canvas(),
                        LIST_MAX_DISPLAY_CHARS * 6,
                        12,
                        LIST_SCROLL_SPEED,
                        LIST_SCROLL_PAUSE,
                        FONT_12);
    scroll_text_init_ex(&_data.right_panel.path_scroll_ctx,
                        _data.hal->canvas(),
                        PATH_MAX_DISPLAY_CHARS * 6,
                        12,
                        PATH_SCROLL_SPEED,
                        PATH_SCROLL_PAUSE,
                        FONT_12);
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
    // update files lists fpr both panels
    _update_panel_file_list(_data.left_panel);
    _update_panel_file_list(_data.right_panel);
}

void AppFinder::onResume()
{
    ANIM_APP_OPEN();

    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_12);
    _data.hal->canvas()->setTextColor(TFT_ORANGE);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();

    // Initialize both panels directly with root filesystem
    _init_panel(_data.left_panel);
    _init_panel(_data.right_panel);
    _data.left_panel.needs_update = true;
    _data.right_panel.needs_update = true;
}

void AppFinder::onRunning()
{
    bool is_update = false;
    if (_data.hal->home_button()->is_pressed())
    {
        _data.hal->keyboard()->resetLastPressedTime();
        _data.hal->playNextSound();
        destroyApp();
        return;
    }

    // Render both panels
    int panel_width = _data.hal->canvas()->width() / 2;
    // Render panel info if needed (always render for active panel highlighting)
    // Render file lists only when needed
    if (_data.left_panel.panel_info_needs_update)
    {
        is_update |= _render_panel_info(_data.left_panel, 0, panel_width, _data.active_panel == panel_left);
    }
    if (_data.right_panel.panel_info_needs_update)
    {
        is_update |= _render_panel_info(_data.right_panel, panel_width, panel_width, _data.active_panel == panel_right);
    }

    if (_data.left_panel.needs_update)
    {
        is_update |= _render_panel_file_list(_data.left_panel, 0, panel_width, _data.active_panel == panel_left);
    }
    if (_data.right_panel.needs_update)
    {
        is_update |= _render_panel_file_list(_data.right_panel, panel_width, panel_width, _data.active_panel == panel_right);
    }
    is_update |= _render_scrolling_path(_data.left_panel, 0, _data.active_panel == panel_left);
    is_update |= _render_scrolling_path(_data.right_panel, panel_width, _data.active_panel == panel_right);
    if (_data.active_panel == panel_left)
        is_update |= _render_scrolling_list(_data.left_panel, 0, panel_width);
    if (_data.active_panel == panel_right)
        is_update |= _render_scrolling_list(_data.right_panel, panel_width, panel_width);
    is_update |= _render_hint();

    if (is_update)
    {
        _data.hal->canvas_update();
    }

    // Handle keyboard input for the active panel
    PanelData_t& active_panel = (_data.active_panel == panel_left) ? _data.left_panel : _data.right_panel;
    _handle_file_selection(active_panel);
}

void AppFinder::onDestroy()
{
    // Free scroll contexts
    scroll_text_free(&_data.left_panel.list_scroll_ctx);
    scroll_text_free(&_data.left_panel.path_scroll_ctx);
    scroll_text_free(&_data.right_panel.list_scroll_ctx);
    scroll_text_free(&_data.right_panel.path_scroll_ctx);
    hl_text_free(&_data.hint_hl_ctx);
}

bool AppFinder::_has_extension(const std::string& filename, const std::string& ext)
{
    if (filename.length() <= ext.length())
    {
        return false;
    }

    auto file_ext = filename.substr(filename.length() - ext.length());
    return std::equal(file_ext.begin(),
                      file_ext.end(),
                      ext.begin(),
                      [](char a, char b) { return std::tolower(a) == std::tolower(b); });
}

std::string AppFinder::_truncate_path(const std::string& path, int max_chars)
{
    if (_data.hal->canvas()->textWidth(path.c_str()) <= max_chars * 6) // FONT_12 width
    {
        return path;
    }
    int half_max = max_chars / 2;
    return path.substr(0, half_max - 2) + "..." + path.substr(path.length() - half_max + 1);
}

void AppFinder::_clear_screen() { _data.hal->canvas()->fillScreen(THEME_COLOR_BG); }

void AppFinder::_init_panel(PanelData_t& panel)
{
    panel.initialized = true;
    panel.current_path = "/";
    _update_panel_file_list(panel);
}

void AppFinder::_update_panel_file_list(PanelData_t& panel)
{
    if (!panel.initialized)
    {
        return;
    }

    // Special-case root: manually expose mounted sources
    if (panel.current_path == "/")
    {
        // unmount usb and sdcard if mounted
        _data.hal->usb()->unmount();
        _data.hal->sdcard()->eject();
        // add sdcard and usb to file list
        panel.file_list.clear();
        panel.file_list.shrink_to_fit();
        panel.file_list.push_back(SD_CARD_ITEM);
        panel.file_list.push_back(USB_ITEM);
        // no more files in root
        return;
    }
    else
    {
        if (panel.current_path == "/sdcard")
        {
            _mount_sdcard();
            if (!_data.hal->sdcard()->is_mounted())
            {
                // show error dialog
                UTILS::UI::show_error_dialog(_data.hal, "SD card not found", "Plug an SD card and try again");
                // failback to root
                panel.current_path = "/";
                // redraw all
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                return;
            }
        }
        else if (panel.current_path == "/usb")
        {
            // check if usb is enabled
            if (!_data.hal->settings()->getBool("system", "usb_host"))
            {
                // show error dialog
                UTILS::UI::show_error_dialog(_data.hal, "USB disabled", "USB host is disabled in Settings");
                // failback to root
                panel.current_path = "/";
                // redraw all
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                return;
            }
            _mount_usb();
            if (!_data.hal->usb()->is_mounted())
            {
                // show error dialog
                UTILS::UI::show_error_dialog(_data.hal, "USB not found", "Plug a USB drive and try again");
                // failback to root
                panel.current_path = "/";
                // redraw all
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                return;
            }
        }
        // Add parent directory entry if not in root
        panel.file_list.clear();
        panel.file_list.shrink_to_fit();
        panel.file_list.push_back(BACK_DIR_ITEM);
    }
    DIR* dir = opendir(panel.current_path.c_str());
    if (dir)
    {
        struct dirent* entry;
        std::vector<FileItem_t> folders;
        std::vector<FileItem_t> files;

        while ((entry = readdir(dir)) != NULL)
        {
            std::string name = entry->d_name;
            if (name == "." || name == "..")
            {
                continue;
            }

            std::string full_path = panel.current_path + "/" + name;
            struct stat statbuf;
            if (stat(full_path.c_str(), &statbuf) == 0)
            {
                bool is_dir = S_ISDIR(statbuf.st_mode);
                // Show all files and directories
                uint64_t size = statbuf.st_size;
                auto tm = localtime(&statbuf.st_mtime);
                std::string info = std::format("{:04d}-{:02d}-{:02d}", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday);

                FileItem_t item(name, is_dir, size, name, info);
                if (is_dir)
                {
                    folders.push_back(item);
                }
                else
                {
                    files.push_back(item);
                }
            }
        }
        closedir(dir);

        // Sort folders by name
        std::sort(folders.begin(), folders.end(), [](const FileItem_t& a, const FileItem_t& b) { return a.name < b.name; });

        // Sort files by name
        std::sort(files.begin(), files.end(), [](const FileItem_t& a, const FileItem_t& b) { return a.name < b.name; });

        // Add folders first, then files
        for (const auto& folder : folders)
        {
            panel.file_list.push_back(folder);
        }
        for (const auto& file : files)
        {
            panel.file_list.push_back(file);
        }
        // check selected file (after deleting or moving)
        if (panel.selected_file >= panel.file_list.size())
        {
            panel.selected_file = panel.file_list.size() > 0 ? panel.file_list.size() - 1 : 0;
        }
    }
}

void AppFinder::_navigate_panel_directory(PanelData_t& panel, const std::string& path)
{
    panel.panel_info_needs_update = true;
    std::string old_path = panel.current_path;

    // Ensure path is within the root filesystem
    if (path.find("/") != 0)
    {
        panel.current_path = "/";
    }
    else
    {
        panel.current_path = path;
    }

    panel.selected_file = 0;
    panel.scroll_offset = 0;

    scroll_text_reset(&panel.path_scroll_ctx);
    scroll_text_reset(&panel.list_scroll_ctx);
    _update_panel_file_list(panel);

    // If navigating back to parent directory, try to position at the directory we came from
    if (old_path.length() > path.length())
    {
        // Extract the last segment of the old path
        size_t last_slash = old_path.find_last_of('/');
        if (last_slash != std::string::npos)
        {
            std::string last_segment = old_path.substr(last_slash + 1);
            // Find this segment in the file list
            for (size_t i = 0; i < panel.file_list.size(); i++)
            {
                if (panel.file_list[i].name == last_segment)
                {
                    panel.selected_file = i;
                    // Adjust scroll offset if needed
                    if (panel.selected_file >= LIST_MAX_VISIBLE_ITEMS)
                    {
                        panel.scroll_offset = panel.selected_file - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    break;
                }
            }
        }
    }
}

bool AppFinder::_render_panel_info(PanelData_t& panel, int panel_x, int panel_width, bool is_active)
{
    if (!panel.initialized)
    {
        return false;
    }

    // Draw panel header (clear previous highlighting)
    _data.hal->canvas()->fillRect(panel_x, 0, panel_width, 12, THEME_COLOR_BG);
    scroll_text_reset(&panel.path_scroll_ctx);
    // reset flag
    panel.panel_info_needs_update = false;
    return true;
}

bool AppFinder::_render_panel_file_list(PanelData_t& panel, int panel_x, int panel_width, bool is_active)
{
    // Clear panel area (leave space for info panel at bottom)
    _data.hal->canvas()->fillRect(panel_x, 12, panel_width, _data.hal->canvas()->height() - 12, THEME_COLOR_BG);
    // set font
    _data.hal->canvas()->setFont(FONT_12);

    if (!panel.initialized || panel.file_list.empty())
    {
        // draw string: no files to display
        _data.hal->canvas()->setTextColor(TFT_DARKGREY);
        _data.hal->canvas()->drawCenterString("No data",
                                              panel_x + panel_width / 2,
                                              12 + (LIST_MAX_VISIBLE_ITEMS / 2) * (14 + 1));
        return false;
    }

    // Draw file list
    int y_offset = 12;
    int items_drawn = 0;
    const int max_width = LIST_MAX_DISPLAY_CHARS * 6; // FONT_12 width

    for (int i = panel.scroll_offset; i < panel.file_list.size() && items_drawn < LIST_MAX_VISIBLE_ITEMS; i++)
    {
        bool is_dir = panel.file_list[i].is_dir;
        std::string display_name = panel.file_list[i].name;

        if (panel.file_list[i].is_dir)
        {
            display_name = "[" + display_name + "]";
        }
        if (_data.hal->canvas()->textWidth(display_name.c_str()) > max_width)
        {
            display_name = display_name.substr(0, LIST_MAX_DISPLAY_CHARS - 1) + ">";
        }

        if (is_active && i == panel.selected_file)
        {
            _data.hal->canvas()->fillRect(panel_x + 2, y_offset + 1, panel_width - 2 - 4 - 1, 14, THEME_COLOR_BG_SELECTED);
            _data.hal->canvas()->pushImage(panel_x + 6,
                                           y_offset + 1,
                                           14,
                                           14,
                                           is_dir ? image_data_folder_sel14 : image_data_file_sel14);
            _data.hal->canvas()->setTextColor(THEME_COLOR_SELECTED);
            _data.hal->canvas()->drawString(display_name.c_str(), panel_x + 20, y_offset + 1);
        }
        else
        {
            _data.hal->canvas()->pushImage(panel_x + 6, y_offset + 1, 14, 14, is_dir ? image_data_folder14 : image_data_file14);
            _data.hal->canvas()->setTextColor(is_dir ? TFT_GREENYELLOW : TFT_WHITE);
            _data.hal->canvas()->drawString(display_name.c_str(), panel_x + 20, y_offset + 1);
        }

        y_offset += 14 + 1;
        items_drawn++;
    }

    _render_panel_scrollbar(panel, panel_x, panel_width);

    // Draw info panel at the bottom
    if (!panel.file_list.empty() && panel.selected_file < panel.file_list.size())
    {
        const FileItem_t& selected_item = panel.file_list[panel.selected_file];
        std::string info_text;

        if (selected_item.name == "..")
        {
            info_text = "..";
        }
        else if (selected_item.is_dir)
        {
            // std::string name = selected_item.name;
            // if (name.length() > 16)
            // {
            //     name = name.substr(0, 15) + ">";
            // }
            // info_text = std::format("[{:16.16s}]", name);
            info_text = selected_item.info;
        }
        else
        {
            // std::string name = selected_item.name;
            // if (name.length() > 10)
            // {
            //     name = name.substr(0, 9) + ">";
            // }
            info_text = std::format("{:10.10s} {:>7s}", selected_item.info, PartitionTable::formatSize(selected_item.size));
        }

        // Draw info panel background
        int info_y = 12 + (14 + 1) * LIST_MAX_VISIBLE_ITEMS;
        // _data.hal->canvas()->fillRect(panel_x, info_y, panel_width, 12, THEME_COLOR_BG_SELECTED);

        // Draw info text
        _data.hal->canvas()->setTextColor(TFT_DARKGREY);
        _data.hal->canvas()->drawString(info_text.c_str(), panel_x + 2, info_y);
    }

    panel.needs_update = false;
    return true;
}

bool AppFinder::_render_panel_scrollbar(PanelData_t& panel, int panel_x, int panel_width)
{
    if (panel.file_list.size() <= LIST_MAX_VISIBLE_ITEMS)
    {
        return false;
    }
    const int scrollbar_width = 4;
    UTILS::UI::draw_scrollbar(_data.hal->canvas(),
                              panel_x + panel_width - scrollbar_width - 1,
                              12,
                              scrollbar_width,
                              (14 + 1) * LIST_MAX_VISIBLE_ITEMS,
                              (int)panel.file_list.size(),
                              LIST_MAX_VISIBLE_ITEMS,
                              panel.scroll_offset);
    return true;
}

bool AppFinder::_render_scrolling_path(PanelData_t& panel, int panel_x, bool is_active)
{
    return scroll_text_render(&panel.path_scroll_ctx,
                              panel.current_path.c_str(),
                              panel_x + 2,
                              0,
                              lgfx::v1::convert_to_rgb888(is_active ? TFT_SKYBLUE : TFT_WHITE),
                              THEME_COLOR_BG);
}

bool AppFinder::_render_scrolling_list(PanelData_t& panel, int panel_x, int panel_width)
{
    if (!panel.initialized || panel.file_list.empty())
    {
        return false;
    }

    // Get the text to display (file or directory name)
    std::string display_name = panel.file_list[panel.selected_file].name;
    if (panel.file_list[panel.selected_file].is_dir)
    {
        display_name = "[" + display_name + "]";
    }

    // Calculate y position of selected item
    int relative_pos = panel.selected_file - panel.scroll_offset;
    int y_offset = 12 + (relative_pos * (14 + 1)); // line height = 14 + spacing = 1

    // Render the scrolling text using our utility
    return scroll_text_render(&panel.list_scroll_ctx,
                              display_name.c_str(),
                              panel_x + 20,
                              y_offset + 1,
                              THEME_COLOR_SELECTED,
                              THEME_COLOR_BG_SELECTED);
}

bool AppFinder::_render_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_PANELS,
                          0,
                          _data.hal->canvas()->height() - 8,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

bool AppFinder::_copy_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name)
{
    // Check if source is a directory
    struct stat src_stat;
    if (stat(src_path.c_str(), &src_stat) != 0)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", std::string("Cannot access ") + src_path);
        return false;
    }

    if (S_ISDIR(src_stat.st_mode))
    {
        // Copy directory recursively
        return _copy_directory_recursive(src_path, dest_path, display_name);
    }
    else
    {
        // Copy file
        return _copy_single_file(src_path, dest_path, display_name);
    }
}

bool AppFinder::_copy_single_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name)
{
    // Show initial progress
    UTILS::UI::show_progress(_data.hal, std::string("Copying: ") + display_name, -1, "Starting...");

    // Open source
    FILE* src = fopen(src_path.c_str(), "rb");
    if (!src)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", std::string("Cannot open ") + src_path);
        return false;
    }

    // Open destination
    FILE* dst = fopen(dest_path.c_str(), "wb");
    if (!dst)
    {
        fclose(src);
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", std::string("Cannot create ") + dest_path);
        return false;
    }

    // Determine file size for progress
    fseek(src, 0, SEEK_END);
    long total_size = ftell(src);
    fseek(src, 0, SEEK_SET);

    const size_t buf_sz = 4096;
    std::unique_ptr<char[]> buf(new char[buf_sz]);
    long copied = 0;
    bool ok = true;
    while (ok)
    {
        size_t r = fread(buf.get(), 1, buf_sz, src);
        if (r == 0)
            break;
        size_t w = fwrite(buf.get(), 1, r, dst);
        if (w != r)
        {
            ok = false;
            break;
        }
        copied += (long)w;
        int progress = (total_size > 0) ? (int)((copied * 100) / total_size) : -1;
        UTILS::UI::show_progress(_data.hal,
                                 std::string("Copying: ") + display_name,
                                 progress,
                                 std::format("{} / {} KB", (uint32_t)(copied / 1024), (uint32_t)(total_size / 1024)));
    }

    fclose(src);
    fclose(dst);

    if (!ok)
    {
        // Remove partial file
        unlink(dest_path.c_str());
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", "Write error");
        return false;
    }

    // Completed
    UTILS::UI::show_progress(_data.hal, std::string("Copying: ") + display_name, 100, "Done");
    delay(300);
    return true;
}

bool AppFinder::_copy_directory_recursive(const std::string& src_dir,
                                          const std::string& dest_dir,
                                          const std::string& display_name)
{
    // ESP_LOGI(TAG, "Copy directory recursive: %s to %s", src_dir.c_str(), dest_dir.c_str());
    // Show initial progress
    UTILS::UI::show_progress(_data.hal, std::string("Copying: ") + display_name, -1, "Starting...");

    // Create destination directory
    if (mkdir(dest_dir.c_str(), 0777) != 0 && errno != EEXIST)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", std::string("Cannot create directory ") + dest_dir);
        return false;
    }

    DIR* dir = opendir(src_dir.c_str());
    if (!dir)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", std::string("Cannot open directory ") + src_dir);
        return false;
    }

    struct dirent* entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
        {
            continue;
        }

        std::string src_path = src_dir + "/" + name;
        std::string dest_path = dest_dir + "/" + name;

        struct stat statbuf;
        if (stat(src_path.c_str(), &statbuf) == 0)
        {
            if (S_ISDIR(statbuf.st_mode))
            {
                // Recursively copy subdirectory
                if (!_copy_directory_recursive(src_path, dest_path, name))
                {
                    success = false;
                    break;
                }
            }
            else
            {
                // Copy file
                if (!_copy_single_file(src_path, dest_path, name))
                {
                    success = false;
                    break;
                }
            }
        }
    }

    closedir(dir);

    if (success)
    {
        UTILS::UI::show_progress(_data.hal, std::string("Copying: ") + display_name, 100, "Done");
        delay(300);
    }
    else
    {
        UTILS::UI::show_error_dialog(_data.hal, "Copy failed", "Error copying folder contents");
    }

    return success;
}

bool AppFinder::_move_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name)
{
    // Check if source is a directory
    struct stat src_stat;
    if (stat(src_path.c_str(), &src_stat) != 0)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot access ") + src_path);
        return false;
    }

    if (S_ISDIR(src_stat.st_mode))
    {
        // Move directory recursively
        return _move_directory_recursive(src_path, dest_path, display_name);
    }
    else
    {
        // Move file
        return _move_single_file(src_path, dest_path, display_name);
    }
}

bool AppFinder::_move_single_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name)
{
    // Show initial progress
    UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, -1, "Starting...");

    // check src and dest mountpoints are the same, check first path segment
    if (is_same_mountpoint(src_path, dest_path))
    {
        // Try to rename first (fastest for same filesystem)
        if (rename(src_path.c_str(), dest_path.c_str()) == 0)
        {
            UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, 100, "Done");
            delay(300);
            return true;
        }
        else
        {
            UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot rename ") + src_path);
            return false;
        }
    }

    // Open source
    FILE* src = fopen(src_path.c_str(), "rb");
    if (!src)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot open ") + src_path);
        return false;
    }

    // Open destination
    FILE* dst = fopen(dest_path.c_str(), "wb");
    if (!dst)
    {
        fclose(src);
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot create ") + dest_path);
        return false;
    }

    // Determine file size for progress
    fseek(src, 0, SEEK_END);
    long total_size = ftell(src);
    fseek(src, 0, SEEK_SET);

    const size_t buf_sz = 4096;
    std::unique_ptr<char[]> buf(new char[buf_sz]);
    long copied = 0;
    bool ok = true;
    while (ok)
    {
        size_t r = fread(buf.get(), 1, buf_sz, src);
        if (r == 0)
            break;
        size_t w = fwrite(buf.get(), 1, r, dst);
        if (w != r)
        {
            ok = false;
            break;
        }
        copied += (long)w;
        int progress = (total_size > 0) ? (int)((copied * 100) / total_size) : -1;
        UTILS::UI::show_progress(_data.hal,
                                 std::string("Moving: ") + display_name,
                                 progress,
                                 std::format("{} / {} KB", (uint32_t)(copied / 1024), (uint32_t)(total_size / 1024)));
    }

    fclose(src);
    fclose(dst);

    if (!ok)
    {
        // Remove partial file
        unlink(dest_path.c_str());
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", "Write error");
        return false;
    }

    // Delete source file
    if (unlink(src_path.c_str()) != 0)
    {
        // Remove destination file since we couldn't delete source
        unlink(dest_path.c_str());
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", "Cannot delete source file");
        return false;
    }

    // Completed
    UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, 100, "Done");
    delay(300);
    return true;
}

bool AppFinder::_move_directory_recursive(const std::string& src_dir,
                                          const std::string& dest_dir,
                                          const std::string& display_name)
{
    // ESP_LOGI(TAG, "Move directory recursive: %s to %s", src_dir.c_str(), dest_dir.c_str());
    // Show initial progress
    UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, -1, "Starting...");

    // check src and dest mountpoints are the same, check first path segment
    if (is_same_mountpoint(src_dir, dest_dir))
    {
        // Try to rename directory first (fastest for same filesystem)
        if (rename(src_dir.c_str(), dest_dir.c_str()) == 0)
        {
            UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, 100, "Done");
            delay(300);
            return true;
        }
        else
        {
            UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot rename ") + src_dir);
            return false;
        }
    }

    // If rename failed, fall back to copy + delete
    // Create destination directory
    if (mkdir(dest_dir.c_str(), 0777) != 0 && errno != EEXIST)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot create directory ") + dest_dir);
        return false;
    }

    DIR* dir = opendir(src_dir.c_str());
    if (!dir)
    {
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", std::string("Cannot open directory ") + src_dir);
        return false;
    }

    struct dirent* entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
        {
            continue;
        }

        std::string src_path = src_dir + "/" + name;
        std::string dest_path = dest_dir + "/" + name;

        struct stat statbuf;
        if (stat(src_path.c_str(), &statbuf) == 0)
        {
            if (S_ISDIR(statbuf.st_mode))
            {
                // Recursively move subdirectory
                if (!_move_directory_recursive(src_path, dest_path, name))
                {
                    success = false;
                    break;
                }
            }
            else
            {
                // Move file
                if (!_move_single_file(src_path, dest_path, name))
                {
                    success = false;
                    break;
                }
            }
        }
    }

    closedir(dir);

    if (success)
    {
        // Delete source directory
        if (rmdir(src_dir.c_str()) != 0)
        {
            UTILS::UI::show_error_dialog(_data.hal, "Move failed", "Cannot delete source directory");
            success = false;
        }
        else
        {
            UTILS::UI::show_progress(_data.hal, std::string("Moving: ") + display_name, 100, "Done");
            delay(300);
        }
    }
    else
    {
        UTILS::UI::show_error_dialog(_data.hal, "Move failed", "Error moving folder contents");
    }

    return success;
}

bool AppFinder::_delete_file_or_folder(const std::string& path, const std::string& display_name, bool is_dir)
{
    // Show initial progress
    std::string operation = "Deleting: " + display_name;
    UTILS::UI::show_progress(_data.hal, operation, -1, "Starting...");

    if (is_dir)
    {
        // Delete directory recursively
        if (_delete_directory_recursive(path))
        {
            UTILS::UI::show_progress(_data.hal, operation, 100, "Done");
            delay(300);
            return true;
        }
        else
        {
            UTILS::UI::show_error_dialog(_data.hal, "Delete failed", "Cannot delete folder");
            return false;
        }
    }
    else
    {
        // Delete file
        if (unlink(path.c_str()) == 0)
        {
            UTILS::UI::show_progress(_data.hal, operation, 100, "Done");
            delay(300);
            return true;
        }
        else
        {
            UTILS::UI::show_error_dialog(_data.hal, "Delete failed", "Cannot delete file");
            return false;
        }
    }
}

bool AppFinder::_delete_directory_recursive(const std::string& dir_path)
{
    UTILS::UI::show_progress(_data.hal, "Deleting", -1, dir_path);
    DIR* dir = opendir(dir_path.c_str());
    if (!dir)
    {
        return false;
    }

    struct dirent* entry;
    bool success = true;

    while ((entry = readdir(dir)) != NULL)
    {
        std::string name = entry->d_name;
        if (name == "." || name == "..")
        {
            continue;
        }

        std::string full_path = dir_path + "/" + name;
        struct stat statbuf;

        if (stat(full_path.c_str(), &statbuf) == 0)
        {
            if (S_ISDIR(statbuf.st_mode))
            {
                // Recursively delete subdirectory
                if (!_delete_directory_recursive(full_path))
                {
                    success = false;
                    break;
                }
            }
            else
            {
                // Delete file
                if (unlink(full_path.c_str()) != 0)
                {
                    success = false;
                    break;
                }
            }
        }
    }

    closedir(dir);

    if (success)
    {
        // Remove the empty directory
        if (rmdir(dir_path.c_str()) != 0)
        {
            success = false;
        }
    }

    return success;
}

bool AppFinder::_handle_file_selection(PanelData_t& panel)
{
    if (panel.file_list.empty())
    {
        return false;
    }

    bool selection_changed = false;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        // check Fn key hold
        auto keys_state = _data.hal->keyboard()->keysState();
        // Tab to switch between panels
        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_TAB))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                _data.active_panel = (_data.active_panel == panel_left) ? panel_right : panel_left;
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                return true;
            }
        }
        // up navigation
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (panel.selected_file > 0)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = home
                    if (keys_state.fn)
                    {
                        panel.selected_file = 0;
                    }
                    else
                    {
                        panel.selected_file--;
                    }
                    if (panel.selected_file < panel.scroll_offset)
                    {
                        panel.scroll_offset = panel.selected_file;
                    }
                    selection_changed = true;
                }
            }
        }
        // page up
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (panel.selected_file > 0)
                {
                    _data.hal->playNextSound();
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    panel.selected_file = std::max(0, panel.selected_file - jump);
                    panel.scroll_offset = std::max(0, panel.selected_file - (LIST_MAX_VISIBLE_ITEMS - 1));
                    selection_changed = true;
                }
            }
        }
        // down navigation
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (panel.selected_file < panel.file_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = end
                    if (keys_state.fn)
                    {
                        panel.selected_file = panel.file_list.size() - 1;
                    }
                    else
                    {
                        panel.selected_file++;
                    }
                    if (panel.selected_file >= panel.scroll_offset + LIST_MAX_VISIBLE_ITEMS)
                    {
                        panel.scroll_offset = panel.selected_file - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    selection_changed = true;
                }
            }
        }
        // page down
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (panel.selected_file < (int)panel.file_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    panel.selected_file = std::min((int)panel.file_list.size() - 1, panel.selected_file + jump);
                    panel.scroll_offset =
                        std::min(std::max(0, (int)panel.file_list.size() - LIST_MAX_VISIBLE_ITEMS), panel.selected_file);
                    selection_changed = true;
                }
            }
        }
        // Enter
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

            const FileItem_t& selected_item = panel.file_list[panel.selected_file];
            if (selected_item.is_dir)
            {
                std::string new_path;
                if (selected_item.name == "..")
                {
                    size_t last_slash = panel.current_path.find_last_of('/');
                    if (last_slash != std::string::npos)
                    {
                        new_path = panel.current_path.substr(0, last_slash);
                        if (new_path.empty())
                            new_path = "/";
                    }
                }
                else
                {
                    new_path = panel.current_path;
                    if (new_path != "/")
                        new_path += "/";
                    new_path += selected_item.name;
                }
                _navigate_panel_directory(panel, new_path);
            }
            selection_changed = true;
        }
        // Copy (KEY 5)
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_5))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_5);

            // Determine target panel
            PanelData_t& other_panel = (&panel == &_data.left_panel) ? _data.right_panel : _data.left_panel;
            // Copy files and directories (skip "..")
            const FileItem_t& selected_item = panel.file_list[panel.selected_file];
            if (selected_item.name != ".." && panel.current_path != "/" && other_panel.current_path != "/")
            {
                // Build absolute source path
                std::string src_path = panel.current_path;
                if (src_path != "/")
                    src_path += "/";
                src_path += selected_item.fname.empty() ? selected_item.name : selected_item.fname;

                // Build destination path
                std::string dest_dir = other_panel.current_path;
                if (dest_dir != "/")
                    dest_dir += "/";
                std::string dest_path = dest_dir + (selected_item.fname.empty() ? selected_item.name : selected_item.fname);

                // Confirm
                std::string title = selected_item.name;
                std::string message = std::string("Copy to: ") + dest_dir;
                if (UTILS::UI::show_confirmation_dialog(_data.hal, title, message))
                {
                    // Ensure destination directory exists (create recursively)
                    for (size_t i = 1; i < dest_dir.length(); i++)
                    {
                        if (dest_dir[i] == '/')
                        {
                            std::string temp = dest_dir.substr(0, i);
                            mkdir(temp.c_str(), 0777);
                        }
                    }
                    mkdir(dest_dir.c_str(), 0777);

                    if (_copy_file(src_path, dest_path, selected_item.name))
                    {
                        // Refresh destination panel
                        _update_panel_file_list(other_panel);
                        other_panel.needs_update = true;
                    }
                }
                // redraw all anyway
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                // selection_changed = true;
            }
        }
        // Move (KEY 6)
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_6))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_6);

            // Determine target panel
            PanelData_t& other_panel = (&panel == &_data.left_panel) ? _data.right_panel : _data.left_panel;
            // Move files and directories (skip "..")
            const FileItem_t& selected_item = panel.file_list[panel.selected_file];
            if (selected_item.name != ".." && panel.current_path != "/" && other_panel.current_path != "/")
            {
                // Build absolute source path
                std::string src_path = panel.current_path;
                if (src_path != "/")
                    src_path += "/";
                src_path += selected_item.fname.empty() ? selected_item.name : selected_item.fname;

                // Build destination path
                std::string dest_dir = other_panel.current_path;
                if (dest_dir != "/")
                    dest_dir += "/";
                std::string dest_path = dest_dir + (selected_item.fname.empty() ? selected_item.name : selected_item.fname);

                // Confirm
                std::string title = selected_item.name;
                std::string message = std::string("Move to: ") + dest_dir;
                if (UTILS::UI::show_confirmation_dialog(_data.hal, title, message))
                {
                    // Ensure destination directory exists (create recursively)
                    for (size_t i = 1; i < dest_dir.length(); i++)
                    {
                        if (dest_dir[i] == '/')
                        {
                            std::string temp = dest_dir.substr(0, i);
                            mkdir(temp.c_str(), 0777);
                        }
                    }
                    mkdir(dest_dir.c_str(), 0777);

                    if (_move_file(src_path, dest_path, selected_item.name))
                    {
                        // Refresh both panels
                        _update_panel_file_list(panel);
                        _update_panel_file_list(other_panel);
                        panel.needs_update = true;
                        other_panel.needs_update = true;
                    }
                }
                // redraw all anyway
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
                // selection_changed = true;
            }
        }
        // Make new folder (KEY 7)
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_7))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_7);

            if (panel.current_path != "/")
            {
                std::string folder_name = "";
                if (UTILS::UI::show_edit_string_dialog(_data.hal, "New folder name", folder_name, false, 64))
                {
                    if (!folder_name.empty())
                    {
                        // Build full path for new folder
                        std::string new_folder_path = panel.current_path;
                        if (new_folder_path != "/")
                            new_folder_path += "/";
                        new_folder_path += folder_name;

                        // Create the directory
                        if (mkdir(new_folder_path.c_str(), 0777) == 0)
                        {
                            // Refresh current panel
                            _update_panel_file_list(panel);
                            panel.needs_update = true;
                        }
                        else
                        {
                            UTILS::UI::show_error_dialog(_data.hal, "Create failed", "Cannot create folder");
                        }
                    }
                }
                // redraw all anyway
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
            }
        }
        // Delete (KEY 8)
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_8))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_8);

            const FileItem_t& selected_item = panel.file_list[panel.selected_file];
            if (selected_item.name != ".." && panel.current_path != "/")
            {
                // Build absolute path
                std::string full_path = panel.current_path;
                if (full_path != "/")
                    full_path += "/";
                full_path += selected_item.fname.empty() ? selected_item.name : selected_item.fname;

                // Confirm deletion
                std::string title = selected_item.name;
                std::string message = selected_item.is_dir ? "Delete folder and all contents?" : "Delete the file?";
                if (UTILS::UI::show_confirmation_dialog(_data.hal, title, message))
                {
                    if (_delete_file_or_folder(full_path, selected_item.name, selected_item.is_dir))
                    {
                        // Refresh current panel
                        _update_panel_file_list(panel);
                        panel.needs_update = true;
                    }
                }
                // redraw all anyway
                _data.left_panel.panel_info_needs_update = true;
                _data.right_panel.panel_info_needs_update = true;
                _data.left_panel.needs_update = true;
                _data.right_panel.needs_update = true;
            }
        }
        // Backspace
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();

                if (panel.current_path != "/")
                {
                    _navigate_panel_directory(panel, panel.current_path.substr(0, panel.current_path.find_last_of('/')));
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            destroyApp();
        }
    }
    else
    {
        is_repeat = false;
    }

    if (selection_changed)
    {
        // Reset scroll animation for new selection
        scroll_text_reset(&panel.list_scroll_ctx);
        panel.needs_update = true;
    }
    return selection_changed;
}

void AppFinder::_mount_sdcard()
{
    if (!_data.hal->sdcard()->mount(false))
    {
        // _data.sdcard_initialized = false;
        return;
    }
    // _data.sdcard_initialized = true;
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
}

void AppFinder::_mount_usb()
{
    // Check if USB device is connected
    if (!_data.hal->usb()->is_connected())
    {
        // _data.usb_initialized = false;
        return;
    }

    if (!_data.hal->usb()->mount())
    {
        // _data.usb_initialized = false;
        return;
    }
    // _data.usb_initialized = true;

    ESP_LOGI(TAG, "USB mounted at /usb");
}
