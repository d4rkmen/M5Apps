/**
 * @file app_installer.cpp
 * @brief ROM Installer app implementation
 * @version 0.1
 * @date 2025-01-09
 *
 * @copyright Copyright (c) 2025
 *
 */
#include "app_installer.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_flash.h"
#include "../utils/theme/theme_define.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include "../utils/ui/dialog.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char* TAG = "APP_INSTALLER";

#define LIST_SCROLL_PAUSE 1000
#define LIST_SCROLL_SPEED 25
#define LIST_MAX_VISIBLE_ITEMS 4
#define LIST_MAX_DISPLAY_CHARS 22
#define DIALOG_SCROLL_SPEED 20
#define DIALOG_SCROLL_PAUSE 500
#define PATH_SCROLL_PAUSE 500
#define PATH_SCROLL_SPEED 10
#define PATH_MAX_DISPLAY_CHARS 19
#define DESC_SCROLL_PAUSE 1000
#define DESC_SCROLL_SPEED 20
#define DESC_MAX_DISPLAY_CHARS 19
#define WIFI_CONNECT_TIMEOUT_MS 10000
#define HTTP_RESPONSE_BUFFER_SIZE (16 * 1024)
#define FILE_DOWNLOAD_BUFFER_SIZE FLASH_BUFFER_SIZE
#include "apps/utils/ui/key_repeat.h"
#define SCROLLBAR_MIN_HEIGHT 10

static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;
static const char* CLOUD_API_URL = "http://m5apps.hexlook.com/api";
static const char* HINT_SOURCES = "[LEFT] [RIGHT] [ENTER] [HOME]";

using namespace MOONCAKE::APPS;
using namespace UTILS::FLASH_TOOLS;
using namespace UTILS::SCROLL_TEXT;

struct CloudAppInfo_t
{
    std::string name;
    std::string version;
    std::string author;
    std::string filename;
    uint32_t size;
    std::string datetime;
    std::string url;
};

void AppInstaller::onCreate()
{
    // Get hal
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();

    // Initialize scroll contexts
    scroll_text_init(&_data.list_scroll_ctx,
                     _data.hal->canvas(),
                     LIST_MAX_DISPLAY_CHARS * 8,
                     16,
                     LIST_SCROLL_SPEED,
                     LIST_SCROLL_PAUSE);
    scroll_text_init(&_data.path_scroll_ctx,
                     _data.hal->canvas(),
                     PATH_MAX_DISPLAY_CHARS * 8,
                     16,
                     PATH_SCROLL_SPEED,
                     PATH_SCROLL_PAUSE);
    scroll_text_init(&_data.desc_scroll_ctx,
                     _data.hal->canvas(),
                     DESC_MAX_DISPLAY_CHARS * 8,
                     16,
                     DESC_SCROLL_SPEED,
                     DESC_SCROLL_PAUSE);
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
    _update_source_list();
    _data.file_list.reserve(100);
}

void AppInstaller::onResume()
{
    ANIM_APP_OPEN();

    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_16);
    _data.hal->canvas()->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
    _data.hal->canvas()->setTextSize(1);
    _data.hal->canvas_update();
    _data.state = state_source;
    _data.update_source_list = true;
}

void AppInstaller::onRunning()
{
    static InstallerState_t last_state = state_source;
    bool state_changed = _data.state != last_state;
    last_state = _data.state;

    bool is_update = false;
    if (_data.hal->home_button()->is_pressed())
    {
        _data.hal->keyboard()->resetLastPressedTime();
        _data.hal->playNextSound();
        destroyApp();
        return;
    }

    // Handle different installer states
    switch (_data.state)
    {
    case state_source:
        if (state_changed)
        {
            scroll_text_reset(&_data.desc_scroll_ctx);
        }
        is_update |= _render_scrolling_desc();
        _data.update_source_list |= state_changed;
        if (_data.update_source_list)
            is_update |= _render_source_list();
        is_update |= _render_source_hint();
        if (is_update)
            _data.hal->canvas_update();
        _handle_source_selection();
        return;
    case state_browsing:
        if (state_changed)
        {
            // redraw all info
            _data.update_file_list = true;
            _data.update_cloud_info = true;
            _data.update_sdcard_info = true;
            _data.update_usb_info = true;
            scroll_text_reset(&_data.path_scroll_ctx);
            scroll_text_reset(&_data.desc_scroll_ctx);
            scroll_text_reset(&_data.list_scroll_ctx);
        }
        // Normal browsing mode
        switch (_data.source_type)
        {
        case source_cloud:
            _init_cloud_source();
            if (_data.cloud_initialized)
            {
                if (_data.update_cloud_info)
                    is_update |= _render_cloud_info();
                if (_data.update_file_list)
                    is_update |= _render_file_list();
                is_update |= _render_scrolling_path();
                is_update |= _render_scrolling_desc();
                is_update |= _render_scrolling_list();
                if (is_update)
                    _data.hal->canvas_update();
                _handle_file_selection();
            }
            else
            {
                UTILS::UI::show_error_dialog(_data.hal,
                                             "No connection",
                                             _data.error_message.empty() ? "Please check the WiFi connection and try again"
                                                                         : _data.error_message);
                _data.state = state_source;
            }
            break;
        case source_sdcard:
            _init_sdcard_source();
            if (_data.hal->sdcard()->is_mounted())
            {
                if (_data.update_sdcard_info)
                    is_update |= _render_sdcard_info();
                if (_data.update_file_list)
                    is_update |= _render_file_list();
                is_update |= _render_scrolling_path();
                is_update |= _render_scrolling_desc();
                is_update |= _render_scrolling_list();
                if (is_update)
                    _data.hal->canvas_update();
                _handle_file_selection();
            }
            else
            {
                _data.sdcard_initialized = false;
                UTILS::UI::show_error_dialog(_data.hal, "SD card error", "Please check the SD card and try again");
                _data.state = state_source;
            }
            break;
        case source_usb:
            // check if usb is enabled
            if (!_data.hal->settings()->getBool("system", "usb_host"))
            {
                _data.usb_initialized = false;
                // show error dialog
                UTILS::UI::show_error_dialog(_data.hal, "USB disabled", "USB host is disabled in Settings");
                _data.state = state_source;
                return;
            }
            _init_usb_source();
            if (_data.hal->usb()->is_mounted())
            {
                if (_data.update_usb_info)
                    is_update |= _render_usb_info();
                if (_data.update_file_list)
                    is_update |= _render_file_list();
                is_update |= _render_scrolling_path();
                is_update |= _render_scrolling_desc();
                is_update |= _render_scrolling_list();
                if (is_update)
                    _data.hal->canvas_update();
                _handle_file_selection();
            }
            else
            {
                _data.usb_initialized = false;
                UTILS::UI::show_error_dialog(_data.hal, "USB flash error", "Please check the USB flash drive and try again");
                _data.state = state_source;
            }
            break;
        }
        break;

    case state_installing:
        // Installation is being handled in _install_firmware and its callback
        // No additional handling needed here
        break;

    case state_complete:
    case state_error:
        // These states are handled by their respective methods
        // (_handle_installation_complete and _handle_installation_error)
        // No additional handling needed here
        break;
    }
}

void AppInstaller::onDestroy()
{
    // Unmount sources
    switch (_data.source_type)
    {
    case source_sdcard:
        _unmount_sdcard();
        break;
    case source_usb:
        _unmount_usb();
        break;
    case source_cloud:
        // TODO: auto disconnect wifi if set in settings
        break;
    }

    // Free scroll contexts
    scroll_text_free(&_data.list_scroll_ctx);
    scroll_text_free(&_data.path_scroll_ctx);
    scroll_text_free(&_data.desc_scroll_ctx);
    // Free hint text context
    hl_text_free(&_data.hint_hl_ctx);
    _clear_file_list();
}

void AppInstaller::_unmount_sdcard()
{
    if (_data.hal->sdcard()->is_mounted())
    {
        _data.hal->sdcard()->eject();
        ESP_LOGI(TAG, "SD card unmounted");
    }
    _data.sdcard_initialized = false;
}

void AppInstaller::_unmount_usb()
{
    _data.hal->usb()->unmount();
    ESP_LOGI(TAG, "USB unmounted");
    _data.usb_initialized = false;
}

void AppInstaller::_clear_file_list()
{
    for (auto* item : _data.file_list)
    {
        delete item;
    }
    _data.file_list.clear();
}

void AppInstaller::_handle_source_selection()
{
    bool selection_changed = false;

    if (_data.hal->home_button()->is_pressed())
    {
        _data.hal->keyboard()->resetLastPressedTime();
        _data.hal->playNextSound();
        destroyApp();
        return;
    }
    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                _data.selected_source++;
                if (_data.selected_source >= _data.sources.size())
                    _data.selected_source = 0;
                selection_changed = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                _data.hal->playNextSound();
                _data.selected_source--;
                if (_data.selected_source < 0)
                    _data.selected_source = _data.sources.size() - 1;
                selection_changed = true;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

            _data.source_type = static_cast<SourceType_t>(_data.selected_source);
            _data.state = state_browsing;
            return;
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
        // reset scroll context to start from the beginning
        scroll_text_reset(&_data.desc_scroll_ctx);
    }
    // flag to render source list
    _data.update_source_list |= selection_changed;
}

bool AppInstaller::_render_source_list()
{
    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas()->setTextColor(TFT_WHITE, THEME_COLOR_BG);
    _data.hal->canvas()->setFont(FONT_16);
    _data.hal->canvas()->drawString("Select source", 5, 0);
    // draw source image
    _data.hal->canvas()->pushImage(_data.hal->canvas()->width() - 8 * 8 - 1,
                                   0,
                                   64,
                                   32,
                                   _data.sources[_data.selected_source].image);

    for (int i = 0; i < _data.sources.size(); i++)
    {
        if (i == _data.selected_source)
        {
            _data.hal->canvas()->fillSmoothCircle(_data.sources[i].x + 12, _data.sources[i].y + 8, 6, TFT_GREENYELLOW);
            _data.hal->canvas()->setTextColor(TFT_GREENYELLOW, THEME_COLOR_BG);
        }
        else
            _data.hal->canvas()->setTextColor(TFT_ORANGE, THEME_COLOR_BG);

        _data.hal->canvas()->setCursor(_data.sources[i].x + 26, _data.sources[i].y);
        _data.hal->canvas()->print(_data.sources[i].name.c_str());
    }
    _data.update_source_list = false;
    return true;
}

bool AppInstaller::_render_source_hint()
{
    return hl_text_render(&_data.hint_hl_ctx,
                          HINT_SOURCES,
                          0,
                          _data.hal->canvas()->height() - 12,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

void AppInstaller::_init_cloud_source()
{
    _data.error_message = "";
    if (_data.cloud_initialized)
    {
        return;
    }
    // check wifi is enabled
    if (!_data.hal->settings()->getBool("wifi", "enabled"))
    {
        _data.error_message = "WiFi is disabled in Settings";
        _data.state = state_source;
        return;
    }
    // Check WiFi connection
    if (!_data.hal->wifi()->is_connected())
    {
        _data.error_message = "WiFi is not connected, check SSID and password in Settings";
        _data.state = state_source;
        return;
    }

    _clear_screen();
    _data.cloud_initialized = true;
    _data.update_cloud_info = true;
    _data.update_file_list = true;
    _navigate_directory("/cloud"); // Start at root for cloud repository
}

bool AppInstaller::_init_sdcard_source()
{
    if (_data.sdcard_initialized)
    {
        return false;
    }

    _mount_sdcard();
    if (_data.hal->sdcard()->is_mounted())
    {
        // _data.sdcard_initialized = true;
        _clear_screen();
        _data.update_sdcard_info = true;
        _data.update_file_list = true;
        _navigate_directory("/sdcard");
        return true;
    }
    return false;
}

void AppInstaller::_mount_sdcard()
{
    if (!_data.hal->sdcard()->mount(false))
    {
        _data.sdcard_initialized = false;
        _data.state = state_source;
        return;
    }
    _data.sdcard_initialized = true;
    ESP_LOGI(TAG, "SD card mounted at /sdcard");
}

void AppInstaller::_mount_usb()
{
    // Check if USB device is connected
    if (!_data.hal->usb()->is_connected())
    {
        _data.usb_initialized = false;
        _data.state = state_source;
        return;
    }

    if (!_data.hal->usb()->mount())
    {
        _data.usb_initialized = false;
        _data.state = state_source;
        return;
    }
    _data.usb_initialized = true;

    ESP_LOGI(TAG, "USB mounted at /usb");
}

bool AppInstaller::_has_extension(const std::string& filename, const std::string& ext)
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

void AppInstaller::_update_source_file_list()
{
    _clear_file_list();

    // Add parent directory entry if not in root
    if (_data.current_path.find('/', 1) != std::string::npos)
    {
        _data.file_list.push_back(new FileItem_t(BACK_DIR_ITEM));
    }

    switch (_data.source_type)
    {
    case source_cloud:
        if (!_data.hal->wifi()->is_connected())
        {
            return;
        }
        _update_cloud_file_list();
        break;
    case source_sdcard:
    case source_usb:
        if (!_is_source_mounted())
        {
            return;
        }
        // Add parent directory entry if not in root
        _update_file_list();
        break;
    default:
        break;
    }
}

void AppInstaller::_update_file_list()
{
    _data.current_desc = "";
    DIR* dir = opendir(_data.current_path.c_str());
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

            std::string full_path = _data.current_path + "/" + name;
            struct stat statbuf;
            if (stat(full_path.c_str(), &statbuf) == 0)
            {
                bool is_dir = S_ISDIR(statbuf.st_mode);
                // Only show .bin files and directories
                if (is_dir)
                {
                    folders.push_back({name, true, 0, name, ""});
                }
                else if (_has_extension(name, ".bin"))
                {
                    uint64_t size = statbuf.st_size;
                    auto tm = localtime(&statbuf.st_mtime);
                    std::string info = std::format("{} {:04d}-{:02d}-{:02d}",
                                                   PartitionTable::formatSize(size),
                                                   tm->tm_year + 1900,
                                                   tm->tm_mon + 1,
                                                   tm->tm_mday);

                    std::string app_name = name.substr(0, name.find_last_of("."));
                    files.push_back({app_name, false, size, name, info});
                }
            }
        }
        closedir(dir);

        // Sort folders and files by display name
        std::sort(folders.begin(), folders.end(), [](const FileItem_t& a, const FileItem_t& b) { return a.name < b.name; });
        std::sort(files.begin(), files.end(), [](const FileItem_t& a, const FileItem_t& b) { return a.name < b.name; });

        // Append folders first, then files
        for (const auto& f : folders)
            _data.file_list.push_back(new FileItem_t(f));
        for (const auto& f : files)
            _data.file_list.push_back(new FileItem_t(f));
    }
}

std::string AppInstaller::_truncate_path(const std::string& path, int max_chars)
{
    if (_data.hal->canvas()->textWidth(path.c_str()) <= max_chars * 8)
    {
        return path;
    }
    int half_max = max_chars / 2;
    return path.substr(0, half_max - 2) + "..." + path.substr(path.length() - half_max + 1);
}

bool AppInstaller::_render_sdcard_info()
{
    // Get volume label and size info
    std::string name = _data.hal->sdcard()->get_device_name();
    uint64_t totalBytes = _data.hal->sdcard()->get_capacity();

    // Draw volume label and size info
    const int width = FONT_WIDTH * 8; // _999.9MB
    std::string sd_size = PartitionTable::formatSize(totalBytes);
    std::string sd_label;
    if (_data.hal->canvas()->textWidth(name.c_str()) > width)
        sd_label = name.substr(0, 7) + ">";
    else
        sd_label = name;
    _data.hal->canvas()->pushImage(_data.hal->canvas()->width() - width - 1, 0, 64, 32, image_data_sd_big);
    // create prite to draw transparent background
    LGFX_Sprite* sprite = new LGFX_Sprite(_data.hal->canvas());
    sprite->createSprite(64, 32);
    sprite->fillScreen(THEME_COLOR_TRANSPARENT);
    sprite->setTextColor(TFT_BLACK, THEME_COLOR_TRANSPARENT);
    sprite->setTextSize(1);
    sprite->setFont(FONT_16);
    sprite->drawRightString(sd_label.c_str(), sprite->width() - 1, 0);
    sprite->drawRightString(sd_size.c_str(), sprite->width() - 1, 16);
    sprite->pushSprite(_data.hal->canvas(), _data.hal->canvas()->width() - width - 1, 0, THEME_COLOR_TRANSPARENT);
    sprite->deleteSprite();
    delete sprite;

    // _data.hal->canvas()->setTextColor(TFT_BLACK, THEME_COLOR_ICON);
    // _data.hal->canvas()->drawRightString(sd_label.c_str(), _data.hal->canvas()->width() - 1, 0);
    // _data.hal->canvas()->drawRightString(sd_size.c_str(), _data.hal->canvas()->width() - 1, 15);
    _data.update_sdcard_info = false;
    return true;
}

bool AppInstaller::_render_usb_info()
{
    // Add USB info if mounted
    // if (!_data.hal->usb()->is_mounted())
    //     return false;
    // Get volume label and size info
    std::string name = _data.hal->usb()->get_device_name();
    uint64_t totalBytes = _data.hal->usb()->get_capacity();

    // Draw volume label and size info
    const int width = 8 * 8; // _999.9MB
    std::string usb_size = PartitionTable::formatSize(totalBytes);
    std::string usb_label;
    if (_data.hal->canvas()->textWidth(name.c_str()) > width)
        usb_label = name.substr(0, 7) + ">";
    else
        usb_label = name;
    _data.hal->canvas()->pushImage(_data.hal->canvas()->width() - width - 1, 0, 64, 32, image_data_usb_flash);
    // create prite to draw transparent background
    LGFX_Sprite* sprite = new LGFX_Sprite(_data.hal->canvas());
    sprite->createSprite(64, 32);
    sprite->fillScreen(THEME_COLOR_TRANSPARENT);
    sprite->setTextColor(TFT_WHITE, THEME_COLOR_TRANSPARENT);
    sprite->setTextSize(1);
    sprite->setFont(FONT_16);
    sprite->drawRightString(usb_label.c_str(), sprite->width() - 1, 0);
    sprite->drawRightString(usb_size.c_str(), sprite->width() - 1, 16);
    sprite->pushSprite(_data.hal->canvas(), _data.hal->canvas()->width() - width - 1, 0, THEME_COLOR_TRANSPARENT);
    sprite->deleteSprite();
    delete sprite;

    _data.update_usb_info = false;
    return true;
}

bool AppInstaller::_render_cloud_info()
{
    const int width = 8 * 8; // _999.9MB
    _data.hal->canvas()->pushImage(_data.hal->canvas()->width() - width - 1, 0, 64, 32, image_data_cloud);
    _data.update_cloud_info = false;
    return true;
}

void AppInstaller::_clear_screen() { _data.hal->canvas()->fillScreen(THEME_COLOR_BG); }

bool AppInstaller::_render_file_list()
{
    int width = _data.hal->canvas()->width();
    int height = _data.hal->canvas()->height();
    _data.hal->canvas()->fillRect(0, 32, width, height - 32, THEME_COLOR_BG);

    // Add SD card info if mounted
    if ((_data.source_type == source_sdcard && _data.hal->sdcard()->is_mounted()) ||
        (_data.source_type == source_usb && _data.hal->usb()->is_mounted()) ||
        (_data.source_type == source_cloud && _data.cloud_initialized))
    {

        _data.hal->canvas()->fillRect(0, 16, width - 8 * 8 - 1, 16, THEME_COLOR_BG);
        _data.hal->canvas()->setTextColor(TFT_ORANGE, THEME_COLOR_BG);
        std::string sizeInfo = _data.file_list[_data.selected_file]->is_dir || (_data.source_type == source_cloud)
                                   ? ">>"
                                   : PartitionTable::formatSize(_data.file_list[_data.selected_file]->size);
        _data.hal->canvas()->drawString(
            std::format("{} / {} : {}", _data.selected_file + 1, _data.file_list.size(), sizeInfo).c_str(),
            5,
            16);
        // Draw file list
        int y_offset = 32;
        int items_drawn = 0;
        const int max_width = LIST_MAX_DISPLAY_CHARS * 8; // Maximum width for text display

        for (int i = _data.scroll_offset; i < _data.file_list.size() && items_drawn < LIST_MAX_VISIBLE_ITEMS; i++)
        {
            bool is_dir = _data.file_list[i]->is_dir;
            std::string display_name = _data.file_list[i]->name;

            if (_data.file_list[i]->is_dir)
            {
                display_name = "[" + display_name + "]";
            }
            if (_data.hal->canvas()->textWidth(display_name.c_str()) > max_width)
            {
                display_name = display_name.substr(0, LIST_MAX_DISPLAY_CHARS - 1) + ">";
            }

            if (i == _data.selected_file)
            {
                _data.hal->canvas()->fillRect(5, y_offset + 1, max_width + 25 + 5, 18, THEME_COLOR_BG_SELECTED);
                _data.hal->canvas()->pushImage(11,
                                               y_offset + 1 + 1,
                                               16,
                                               16,
                                               is_dir ? image_data_folder_sel : image_data_rom_sel);
                _data.hal->canvas()->setTextColor(THEME_COLOR_SELECTED, THEME_COLOR_BG_SELECTED);
                _data.hal->canvas()->drawString(display_name.c_str(), 30, y_offset + 1);
            }
            else
            {
                _data.hal->canvas()->pushImage(11, y_offset + 1 + 1, 16, 16, is_dir ? image_data_folder : image_data_rom);
                _data.hal->canvas()->setTextColor(is_dir ? TFT_GREENYELLOW : TFT_WHITE, THEME_COLOR_BG);
                _data.hal->canvas()->drawString(display_name.c_str(), 30, y_offset + 1);
            }

            y_offset += 18 + 1;
            items_drawn++;
        }

        _render_scrollbar();
    }
    else
    {
        switch (_data.source_type)
        {
        case source_sdcard:
            UTILS::UI::show_error_dialog(_data.hal, "SD card removed", "Please insert SD card and try again");
            break;
        case source_usb:
            UTILS::UI::show_error_dialog(_data.hal, "USB removed", "Please insert USB device and try again");
            break;
        case source_cloud:
            UTILS::UI::show_error_dialog(_data.hal, "Connection lost", "Please check the WiFi connection and try again");
            break;
        }
        // back to source selection
        _data.state = state_source;
    }
    _data.update_file_list = false;
    return true;
}

bool AppInstaller::_render_scrollbar()
{
    if (_data.file_list.size() <= LIST_MAX_VISIBLE_ITEMS)
    {
        return false;
    }

    const int width = _data.hal->canvas()->width();
    const int scrollbar_width = 6;
    const int scrollbar_x = width - scrollbar_width - 2;      // 2 pixels padding from edge
    const int scrollbar_height = 19 * LIST_MAX_VISIBLE_ITEMS; // Height of scrollbar area

    int thumb_height =
        std::max(SCROLLBAR_MIN_HEIGHT, (scrollbar_height * LIST_MAX_VISIBLE_ITEMS) / (int)_data.file_list.size());
    int thumb_pos =
        32 + (scrollbar_height - thumb_height) * _data.scroll_offset / (_data.file_list.size() - LIST_MAX_VISIBLE_ITEMS);

    // Draw scrollbar track
    _data.hal->canvas()->drawRect(scrollbar_x, 32, scrollbar_width, scrollbar_height, TFT_DARKGREY);

    // Draw scrollbar thumb
    _data.hal->canvas()->fillRect(scrollbar_x, thumb_pos, scrollbar_width, thumb_height, TFT_ORANGE);
    return true;
}

bool AppInstaller::_handle_file_selection()
{
    if (_data.file_list.empty())
    {
        return false;
    }

    bool selection_changed = false;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        auto keys_state = _data.hal->keyboard()->keysState();
        // up navigation
        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_file > 0)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = home
                    if (keys_state.fn)
                    {
                        _data.selected_file = 0;
                    }
                    else
                    {
                        _data.selected_file--;
                    }
                    if (_data.selected_file < _data.scroll_offset)
                    {
                        _data.scroll_offset = _data.selected_file;
                    }
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_file > 0)
                {
                    _data.hal->playNextSound();
                    // Jump up by visible_items count (page up)
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_file = std::max(0, _data.selected_file - jump);
                    _data.scroll_offset = std::max(0, _data.selected_file - (LIST_MAX_VISIBLE_ITEMS - 1));
                    selection_changed = true;
                }
            }
        }
        // down navigation
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_file < _data.file_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    // Fn holding? = end
                    if (keys_state.fn)
                    {
                        _data.selected_file = _data.file_list.size() - 1;
                    }
                    else
                    {
                        _data.selected_file++;
                    }
                    if (_data.selected_file >= _data.scroll_offset + LIST_MAX_VISIBLE_ITEMS)
                    {
                        _data.scroll_offset = _data.selected_file - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_file < _data.file_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    // Jump down by visible_items count (page down)
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_file = std::min((int)_data.file_list.size() - 1, _data.selected_file + jump);
                    _data.scroll_offset =
                        std::min(std::max(0, (int)_data.file_list.size() - LIST_MAX_VISIBLE_ITEMS), _data.selected_file);
                    selection_changed = true;
                }
            }
        }
        // Enter
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

            const FileItem_t* selected_item = _data.file_list[_data.selected_file];
            if (selected_item->is_dir)
            {
                std::string new_path;
                if (selected_item->name == "..")
                {
                    // Go to parent directory
                    size_t last_slash = _data.current_path.find_last_of('/');
                    if (last_slash != std::string::npos)
                    {
                        new_path = _data.current_path.substr(0, last_slash);
                        if (new_path.empty())
                            new_path = "/";
                    }
                }
                else
                {
                    // Enter directory
                    new_path = _data.current_path;
                    if (new_path != "/")
                        new_path += "/";
                    new_path += selected_item->name;
                }
                _navigate_directory(new_path);
            }
            else if (_has_extension(selected_item->fname, ".bin"))
            {
                if (_data.source_type == source_cloud)
                {
                    if (_show_confirmation_dialog(selected_item->name, "Install the app?"))
                    {
                        std::string url = _data.current_base_url + selected_item->fname;
                        if (!_download_cloud_file(url, "", selected_item->name) && !_data.error_message.empty())
                        {
                            UTILS::UI::show_error_dialog(_data.hal, "Install failed", _data.error_message);
                        }
                    }
                    // Show confirmation dialog for .bin files
                }
                else if (_show_confirmation_dialog(selected_item->name, "Install the app?"))
                {
                    // Get the full path to the firmware file
                    std::string filepath = _data.current_path;
                    if (filepath.back() != '/')
                        filepath += '/';
                    filepath += selected_item->fname;
                    // Start the installation process
                    _install_firmware(filepath);
                }

                // Redraw the file list after dialog is closed
                _data.update_sdcard_info = true;
                _data.update_usb_info = true;
                _data.update_cloud_info = true;
                // Redraw the path scroll
                scroll_text_reset(&_data.path_scroll_ctx);
                scroll_text_reset(&_data.desc_scroll_ctx);
            }
            selection_changed = true;
        }
        // Backspace
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);

            // jump parent folder if not in root
            if (_data.current_path.find('/', 1) != std::string::npos)
            {
                _navigate_directory(_data.current_path.substr(0, _data.current_path.find_last_of('/')));
                selection_changed = true;
            }
            else
            {
                switch (_data.source_type)
                {
                case source_sdcard:
                    _unmount_sdcard();
                    break;
                case source_usb:
                    _unmount_usb();
                    break;
                case source_cloud:
                    _data.cloud_initialized = false;
                    break;
                }
                _data.state = state_source;
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);

            switch (_data.source_type)
            {
            case source_sdcard:
                _unmount_sdcard();
                break;
            case source_usb:
                _unmount_usb();
                break;
            case source_cloud:
                _data.cloud_initialized = false;
                break;
            }
            _data.state = state_source;
        }

    } // isPressed
    else
    {
        is_repeat = false;
    }

    if (selection_changed)
    {
        // Reset scroll animation for new selection
        scroll_text_reset(&_data.list_scroll_ctx);
        scroll_text_reset(&_data.desc_scroll_ctx);
        _data.update_file_list = true;
    }
    return selection_changed;
}

void AppInstaller::_navigate_directory(const std::string& path)
{
    std::string old_path = _data.current_path;

    if (_data.source_type == source_sdcard && path.find("/sdcard") != 0)
    {
        _data.current_path = "/sdcard";
    }
    else if (_data.source_type == source_usb && path.find("/usb") != 0)
    {
        _data.current_path = "/usb";
    }
    else if (_data.source_type == source_cloud && path.find("/cloud") != 0)
    {
        _data.current_path = "/cloud";
    }
    else
    {
        _data.current_path = path;
    }

    _data.selected_file = 0;
    _data.scroll_offset = 0;

    scroll_text_reset(&_data.path_scroll_ctx);
    scroll_text_reset(&_data.desc_scroll_ctx);
    scroll_text_reset(&_data.list_scroll_ctx);
    if (_data.source_type == source_cloud)
    {
        // display loading dialog
        UTILS::UI::show_progress(_data.hal, "Loading", -1, "Please wait...");
        _data.update_cloud_info = true;
    }
    _update_source_file_list();

    // If navigating back to parent directory, try to position at the directory we came from
    if (old_path.length() > path.length())
    {
        // Extract the last segment of the old path
        size_t last_slash = old_path.find_last_of('/');
        if (last_slash != std::string::npos)
        {
            std::string last_segment = old_path.substr(last_slash + 1);
            // Find this segment in the file list
            for (size_t i = 0; i < _data.file_list.size(); i++)
            {
                if (_data.file_list[i]->name == last_segment)
                {
                    _data.selected_file = i;
                    // Adjust scroll offset if needed
                    if (_data.selected_file >= LIST_MAX_VISIBLE_ITEMS)
                    {
                        _data.scroll_offset = _data.selected_file - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    break;
                }
            }
        }
    }
}

bool AppInstaller::_init_usb_source()
{
    if (_data.usb_initialized)
    {
        return false;
    }

    _mount_usb();
    if (_data.hal->usb()->is_mounted())
    {
        _clear_screen();
        _data.update_usb_info = true;
        _data.update_file_list = true;
        _navigate_directory("/usb");
        return true;
    }
    return false;
}

void AppInstaller::_update_source_list()
{
    _data.sources.clear();
    _data.sources.push_back(
        SelectItem_t("Cloud",
                     0,
                     32,
                     image_data_cloud,
                     "Connect to the cloud repository. Make sure WiFi is enabled in Settings and connected to the internet"));
    _data.sources.push_back(
        SelectItem_t("SD Card",
                     0,
                     56,
                     image_data_sd_big,
                     "Install apps from SD card. Supported media: SDHC (up to 32Gb). Supported file systems: FAT32"));
    _data.sources.push_back(SelectItem_t("USB Drive",
                                         0,
                                         80,
                                         image_data_usb_flash,
                                         "Install apps from USB drive. Supported media: USB flash drive, partition size up to "
                                         "32Gb. Supported file systems: FAT32"));
    _data.update_source_list = true;
}

bool AppInstaller::_show_confirmation_dialog(const std::string& title, const std::string& message)
{
    return UTILS::UI::show_confirmation_dialog(_data.hal, title, message);
}

bool AppInstaller::_is_source_mounted()
{
    switch (_data.source_type)
    {
    case source_sdcard:
        return _data.hal->sdcard()->is_mounted();
    case source_usb:
        return _data.hal->usb()->is_mounted();
    case source_cloud:
        return _data.cloud_initialized;
    default:
        return false;
    }
}

bool AppInstaller::_render_scrolling_list()
{
    if (!_is_source_mounted() || _data.file_list.empty())
    {
        return false;
    }

    // Get the text to display (file or directory name)
    std::string display_name = _data.file_list[_data.selected_file]->name;
    if (_data.file_list[_data.selected_file]->is_dir)
    {
        display_name = "[" + display_name + "]";
    }

    // Calculate y position of selected item
    int relative_pos = _data.selected_file - _data.scroll_offset;
    int y_offset = 32 + (relative_pos * (18 + 1)); // line height = 18 + spacing = 1

    // Render the scrolling text using our utility
    // Only update the canvas if the text scrolled
    return scroll_text_render(&_data.list_scroll_ctx,
                              display_name.c_str(),
                              30,                       // x position
                              y_offset + 1,             // y position
                              THEME_COLOR_SELECTED,     // text color
                              THEME_COLOR_BG_SELECTED); // force scroll even if text fits
}

bool AppInstaller::_render_scrolling_path()
{
    // Only update the canvas if the text scrolled
    return scroll_text_render(&_data.path_scroll_ctx,
                              _data.current_path.c_str(),
                              5,                                        // x position
                              0,                                        // y position
                              lgfx::v1::convert_to_rgb888(TFT_SKYBLUE), // text color
                              THEME_COLOR_BG);                          // background color
}

bool AppInstaller::_render_scrolling_desc()
{
    // display only after delay 3000
    if (millis() - _data.hal->keyboard()->lastPressedTime() < 3000)
    {
        return false;
    }
    std::string desc =
        _data.state == state_source ? _data.sources[_data.selected_source].hint : _data.file_list[_data.selected_file]->info;
    // no info, use current desc
    if (desc.length() == 0)
    {
        desc = _data.current_desc;
    }
    if (desc.length() == 0)
    {
        return false;
    }
    // Only update the canvas if the text scrolled
    return scroll_text_render(
        &_data.desc_scroll_ctx,
        desc.c_str(),
        5,                                                                                    // x position
        16,                                                                                   // y position
        lgfx::v1::convert_to_rgb888(_data.state == state_source ? TFT_DARKGREY : TFT_ORANGE), // text color
        THEME_COLOR_BG);                                                                      // background color
}

// Static callback function to handle installation progress updates
void AppInstaller::_installation_progress_callback(int progress, const char* message, void* arg_cb)
{
    AppInstaller* app = static_cast<AppInstaller*>(arg_cb);
    app->_data.install_progress = progress;
    app->_data.install_status = message;
    app->_render_installation_progress();
}

void AppInstaller::_install_firmware(const std::string& filepath)
{
    uint32_t start_time = millis();
    std::string filename = filepath.substr(filepath.find_last_of('/') + 1);
    std::string app_name = filename.substr(0, filename.find_last_of("."));

    _data.firmware_path = filepath;
    _data.state = state_installing;
    _data.install_title = app_name;

    // read settings
    bool custom_install = _data.hal->settings()->getBool("installer", "custom_install");

    _installation_progress_callback(-1, "Reading PT...", this);
    delay(500);
    // Read partition table
    UTILS::FLASH_TOOLS::PartitionTable file_ptable;
    FlashStatus status = file_ptable.loadFromFile(filepath);
    if (status != FlashStatus::SUCCESS)
    {
        _handle_installation_error(status);
        return;
    }
    PartitionTable flash_ptable;
    // check for full image or single app
    size_t p_count = file_ptable.getCount();
    if (p_count == 0)
    {
        _handle_installation_error(FlashStatus::ERROR_INVALID_FIRMWARE);
        return;
    }
    else if (p_count == 1)
    {
        // single app partition
        if (!flash_ptable.load())
        {
            _handle_installation_error(FlashStatus::ERROR_PARTITION_TABLE);
            return;
        }
    }
    else
    {
        if (custom_install)
        {
            // load existing partition table for custom install
            if (!flash_ptable.load())
            {
                _handle_installation_error(FlashStatus::ERROR_PARTITION_TABLE);
                return;
            }
        }
        else
        {
            // show partition selection dialog
            if (!_show_confirmation_dialog(std::format("Image bundle has {} partitions", p_count), "Erase other apps?"))
            {
                // Return to browsing mode
                _data.state = state_browsing;
                return;
            }
            // remove all partitions and create default ones
            if (!flash_ptable.makeDefaultPartitions())
            {
                _handle_installation_error(FlashStatus::ERROR_UNKNOWN);
                return;
            }
        }
    }

    // add file partitions to flash partition table
    // every app partition we add as ota_x
    esp_partition_info_t* boot_partition = nullptr;
    size_t p_index = 0;
    for (const auto& partition : file_ptable.listPartitions())
    {
        uint8_t subtype = partition.subtype;
        std::string label((const char*)&partition.label);
        p_index++;
        if (partition.type == ESP_PARTITION_TYPE_DATA)
        {
            if (partition.subtype == ESP_PARTITION_SUBTYPE_DATA_OTA)
            {
                // skip ota data partition
                _installation_progress_callback(-1, "Skipping OTADATA...", this);
                delay(500);
                continue;
            }
            else if (partition.subtype == ESP_PARTITION_SUBTYPE_DATA_PHY)
            {
                // skip phy data partition
                _installation_progress_callback(-1, "Skipping PHY...", this);
                delay(500);
                continue;
            }
        }
        else if (partition.type == ESP_PARTITION_TYPE_APP)
        {
            if (boot_partition != nullptr)
            {
                // flash just one app partition, the rest is OTA unused
                _installation_progress_callback(-1, "Skipping OTA...", this);
                delay(500);
                continue;
            }
            subtype = flash_ptable.getNextOTA();
            if (subtype == ESP_PARTITION_SUBTYPE_ANY)
            {
                _handle_installation_error(FlashStatus::ERROR_PARTITION_ADD);
                return;
            }
            if (app_name.length() > 15)
            {
                app_name = app_name.substr(0, 14) + ">";
            }
            // set APP partition label
            label = app_name;
        }
        // check custom install
        std::string subtype_str = PartitionTable::getSubtypeString(partition.type, partition.subtype);
        if (p_count > 1 && custom_install &&
            !UTILS::UI::show_confirmation_dialog(_data.hal,
                                                 "Confirm custom install",
                                                 std::format("{}: {}?", subtype_str, label)))
        {
            _installation_progress_callback(-1, std::format("Skipping {}...", subtype_str).c_str(), this);
            delay(500);
            continue;
        }

        // check free space for partition
        size_t free_space = flash_ptable.getFreeSpace(partition.type);
        if (free_space < partition.pos.size)
        {
            _handle_installation_error(FlashStatus::ERROR_INSUFFICIENT_SPACE);
            return;
        }
        esp_partition_info_t* pi =
            flash_ptable.addPartition(partition.type, subtype, label, 0, partition.pos.size, partition.flags);
        if (pi == nullptr)
        {
            _handle_installation_error(FlashStatus::ERROR_UNKNOWN);
            return;
        }
        // setup progress title
        if (p_count > 1)
        {
            _data.install_title =
                std::format("{} / {}: {} {}KB", p_index, p_count, label, (uint32_t)(partition.pos.size / 1024));
        }
        // Flash the firmware
        status = flash_partition(filepath,
                                 partition.pos.offset,
                                 partition.pos.size,
                                 pi,
                                 &AppInstaller::_installation_progress_callback,
                                 this);
        if (status != FlashStatus::SUCCESS)
        {
            _handle_installation_error(status);
            return;
        }
        if (partition.type == ESP_PARTITION_TYPE_APP)
        {
            boot_partition = pi;
        }
    }
    _data.install_title = app_name;
    // saving partition table to flash
    _installation_progress_callback(-1, "Saving PT...", this);
    delay(500);
    flash_ptable.save();
    // setting boot partition
    if (boot_partition != nullptr && _data.hal->settings()->getBool("installer", "run_on_install"))
    {
        _installation_progress_callback(-1, "Making bootable...", this);
        delay(500);
        FlashStatus status = set_boot_partition(boot_partition);
        if (status != FlashStatus::SUCCESS)
        {
            _handle_installation_error(status);
            return;
        }
    }
    // Installation complete
    _installation_progress_callback(100, std::format("Done: {} sec", (uint32_t)((millis() - start_time) / 1000)).c_str(), this);
    // Wait a moment before showing completion screen
    delay(2000);
    // Handle completion
    _handle_installation_complete();
}

void AppInstaller::_render_installation_progress()
{
    UTILS::UI::show_progress(_data.hal, _data.install_title, _data.install_progress, _data.install_status);
}

void AppInstaller::_handle_installation_complete()
{
    _data.state = state_complete;

    // Show completion dialog with restart button and 5-second countdown
    std::vector<UTILS::UI::DialogButton_t> buttons;
    buttons.push_back(UTILS::UI::DialogButton_t("Restart", THEME_COLOR_BG_SELECTED, TFT_BLACK));

    int result = UTILS::UI::show_dialog(_data.hal,
                                        "Success",
                                        lgfx::v1::convert_to_rgb888(TFT_GREEN),
                                        "restart in", // Will be formatted with remaining time
                                        lgfx::v1::convert_to_rgb888(TFT_LIGHTGREY),
                                        buttons,
                                        5000); // 5 second timeout

    if (result >= 0) // Button pressed or timeout
    {
        _data.hal->reboot();
    }
    else // Cancelled
    {
        _data.state = state_browsing;
        _data.update_file_list = true;
    }
}

void AppInstaller::_handle_installation_error(UTILS::FLASH_TOOLS::FlashStatus status)
{
    _data.state = state_error;

    // Show error dialog
    UTILS::UI::show_error_dialog(_data.hal, "Installation failed", flash_status_to_string(status));
    // Return to browsing mode
    _data.state = state_browsing;
}

// Add this helper function for URL encoding
std::string AppInstaller::_url_encode(const std::string& str)
{
    std::string encoded;
    for (char c : str)
    {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
        {
            encoded += c;
        }
        else if (c == ' ')
        {
            encoded += "%20";
        }
        else
        {
            // char hex[4];
            // snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
            encoded += std::format("%{:02X}", (unsigned char)c);
        }
    }
    return encoded;
}

void AppInstaller::_update_cloud_file_list()
{
    ESP_LOGI(TAG, "Updating cloud: %s", _data.current_path.c_str());

    // URL encode the path component
    std::string url = std::format("{}{}", CLOUD_API_URL, _url_encode(_data.current_path));

    esp_http_client_config_t config;
    memset(&config, 0, sizeof(esp_http_client_config_t));
    config.url = url.c_str();
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;

    // display free heap
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        // ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        _data.cloud_initialized = false;
        // display error message
        UTILS::UI::show_error_dialog(_data.hal, "Error", std::format("Failed to open connection (0x%x)", (int)err));
        return;
    }

    int content_length = esp_http_client_fetch_headers(client);
    ESP_LOGI(TAG, "Content length: %d", content_length);
    // Read response
    char* buffer = (char*)malloc(content_length + 1);
    if (!buffer)
    {
        // ESP_LOGE(TAG, "Failed to allocate memory for response");
        esp_http_client_cleanup(client);
        _data.cloud_initialized = false;
        // display error message
        UTILS::UI::show_error_dialog(_data.hal, "Error", "Failed to allocate memory for response");
        return;
    }
    int total_read = 0;
    if (content_length > 0)
    {
        while (total_read < content_length)
        {
            int read_len = esp_http_client_read(client, buffer + total_read, content_length - total_read);
            if (read_len <= 0)
                break;
            total_read += read_len;
        }
        buffer[total_read] = 0; // Null terminate
    }

    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "Free heap before parse: %d", esp_get_free_heap_size());
    // Parse JSON response
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    ESP_LOGI(TAG, "Free heap after parse: %d", esp_get_free_heap_size());

    if (!root)
    {
        // ESP_LOGE(TAG, "Failed to parse JSON");
        _data.cloud_initialized = false;
        // display error message
        UTILS::UI::show_error_dialog(_data.hal, "Error", "Failed to parse JSON");
        return;
    }

    // Parse collections (directories)
    cJSON* base_url = cJSON_GetObjectItem(root, "b");
    if (base_url)
    {
        // update current collection url, if base url is set
        _data.current_base_url = base_url->valuestring;
    }
    cJSON* descr = cJSON_GetObjectItem(root, "d");
    if (descr)
    {
        // set current descr to the current collection listing
        // visible when child items has no description
        _data.current_desc = descr->valuestring;
    }
    else
    {
        // no descr, use blank
        _data.current_desc = "";
    }
    // ESP_LOGD(TAG, "Current base URL: %s, desc: %s", _data.current_base_url.c_str(), _data.current_desc.c_str());
    cJSON* collections = cJSON_GetObjectItem(root, "c");
    if (collections)
    {
        cJSON* collection;
        cJSON_ArrayForEach(collection, collections)
        {
            cJSON* name = cJSON_GetObjectItem(collection, "n");
            cJSON* descr = cJSON_GetObjectItem(collection, "d");
            // ESP_LOGD(TAG,
            //          "Collection: %s, desc: %s, free ram: %d, items: %d",
            //          name->valuestring,
            //          descr->valuestring,
            //          esp_get_free_heap_size(),
            //          _data.file_list.size());
            if (name && descr)
            {
                _data.file_list.push_back(new FileItem_t({name->valuestring,
                                                          true, // is_dir
                                                          0,    // size
                                                          "",   // fname
                                                          descr->valuestring}));
            }
        }
    }

    // Parse apps (files)
    cJSON* apps = cJSON_GetObjectItem(root, "a");
    if (apps)
    {
        cJSON* app;
        cJSON_ArrayForEach(app, apps)
        {
            cJSON* name = cJSON_GetObjectItem(app, "n");
            cJSON* fname = cJSON_GetObjectItem(app, "f");
            cJSON* descr = cJSON_GetObjectItem(app, "d");
            cJSON* size = cJSON_GetObjectItem(app, "s");
            if (name && fname)
            {
                // ESP_LOGI(TAG, "Free heap: %d, stack: %d", esp_get_free_heap_size(), _free_stack_size());
                _data.file_list.push_back(new FileItem_t({name->valuestring,
                                                          false,                                 // is_dir
                                                          (uint64_t)(size ? size->valueint : 0), // size
                                                          fname ? fname->valuestring : "",       // fname
                                                          descr ? descr->valuestring : ""}));
            }
        }
    }

    cJSON_Delete(root);
}

// Add this helper method to install firmware directly from the cloud repository
bool AppInstaller::_download_cloud_file(const std::string& url, const std::string& dest_path, const std::string& display_name)
{
    ESP_LOGI(TAG, "Installing firmware from %s", url.c_str());
    _data.error_message.clear();
    (void)dest_path;
    uint32_t start_time = millis();

    auto is_block_empty = [](const uint8_t* data, size_t len) -> bool
    {
        if (!len || len % sizeof(uint32_t))
            return false;
        const uint32_t* data32 = (const uint32_t*)data;
        size_t count = len / sizeof(uint32_t);
        for (size_t i = 0; i < count; i++)
        {
            if (data32[i] != 0xFFFFFFFF)
                return false;
        }
        return true;
    };

    auto format_size = [](size_t current, size_t total) -> std::string
    { return std::format("{} / {} KB", (size_t)(current / 1024), (size_t)(total / 1024)); };

    std::string app_name = display_name;
    size_t dot_pos = app_name.find_last_of('.');
    if (dot_pos != std::string::npos)
    {
        app_name = app_name.substr(0, dot_pos);
    }
    if (app_name.length() > 15)
    {
        app_name = app_name.substr(0, 14) + ">";
    }

    _data.firmware_path = url;
    _data.state = state_installing;
    _data.install_title = app_name;

    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.buffer_size = FILE_DOWNLOAD_BUFFER_SIZE;
    config.buffer_size_tx = FILE_DOWNLOAD_BUFFER_SIZE;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        _data.error_message = "Failed to initialize HTTP client";
        _data.state = state_browsing;
        return false;
    }

    auto cleanup_client = [&]()
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to open HTTP connection: %s", esp_err_to_name(err));
        _data.error_message = "Failed to open HTTP connection: " + std::string(esp_err_to_name(err));
        esp_http_client_cleanup(client);
        _data.state = state_browsing;
        return false;
    }

    int content_length = esp_http_client_fetch_headers(client);
    if (content_length <= 0)
    {
        ESP_LOGE(TAG, "Failed to get content length");
        _data.error_message = "Failed to fetch response headers";
        cleanup_client();
        _data.state = state_browsing;
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200)
    {
        ESP_LOGE(TAG, "Error response: %d", status_code);
        _data.error_message = "Error response: " + std::to_string(status_code);
        cleanup_client();
        _data.state = state_browsing;
        return false;
    }

    const size_t file_size = static_cast<size_t>(content_length);
    ESP_LOGI(TAG, "Free heap before download buffers: %u", (uint32_t)esp_get_free_heap_size());
    uint8_t* io_buffer = (uint8_t*)malloc(FILE_DOWNLOAD_BUFFER_SIZE);
    if (!io_buffer)
    {
        _data.error_message = "No memory for download buffer";
        cleanup_client();
        _data.state = state_browsing;
        return false;
    }

    uint8_t* prefix_buffer = nullptr;
    size_t prefix_size = 0;
    bool prefix_allocated = false;

    auto free_buffers = [&]()
    {
        if (prefix_allocated && prefix_buffer)
        {
            free(prefix_buffer);
            prefix_buffer = nullptr;
        }
        if (io_buffer)
        {
            free(io_buffer);
            io_buffer = nullptr;
        }
    };

    size_t total_read = 0;

    _installation_progress_callback(-1, "Reading firmware header...", this);
    constexpr size_t header_needed = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (header_needed > file_size)
    {
        _data.error_message = "Firmware too small";
        cleanup_client();
        free_buffers();
        _data.state = state_browsing;
        return false;
    }

    uint8_t header_buf[header_needed];
    size_t header_read = 0;
    while (header_read < header_needed)
    {
        int read_len = esp_http_client_read(client, (char*)(header_buf + header_read), (int)(header_needed - header_read));
        if (read_len <= 0)
        {
            _data.error_message = "Failed to read firmware header";
            cleanup_client();
            free_buffers();
            _data.state = state_browsing;
            return false;
        }
        header_read += (size_t)read_len;
        total_read += (size_t)read_len;
    }

    esp_image_header_t header = {};
    memcpy(&header, header_buf, sizeof(esp_image_header_t));
    if (header.chip_id != CONFIG_IDF_FIRMWARE_CHIP_ID)
    {
        cleanup_client();
        free_buffers();
        _handle_installation_error(FlashStatus::ERROR_INVALID_CHIP_ID);
        return false;
    }

    esp_app_desc_t app_desc = {};
    memcpy(&app_desc, header_buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t), sizeof(esp_app_desc_t));

    std::vector<esp_partition_info_t> file_partitions;
    if (app_desc.magic_word == ESP_APP_DESC_MAGIC_WORD)
    {
        esp_partition_info_t part = {};
        part.magic = ESP_PARTITION_MAGIC;
        part.type = ESP_PARTITION_TYPE_APP;
        part.subtype = ESP_PARTITION_SUBTYPE_APP_OTA_MIN;
        part.pos.offset = 0;
        part.pos.size = file_size;
        memset(part.label, 0, sizeof(part.label));
        strncpy((char*)part.label, app_name.c_str(), sizeof(part.label) - 1);
        part.flags = 0;
        file_partitions.push_back(part);
        prefix_buffer = header_buf;
        prefix_size = header_needed;
    }
    else if (app_desc.magic_word == ESP_BOOTLOADER_MAGIC_WORD1 || app_desc.magic_word == ESP_BOOTLOADER_MAGIC_WORD2)
    {
        _installation_progress_callback(-1, "Reading PT...", this);
        const size_t table_end = ESP_PARTITION_TABLE_OFFSET + ESP_PARTITION_TABLE_MAX_LEN;
        if (table_end > file_size)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_FILE_READ);
            return false;
        }

        size_t skip_remaining = 0;
        if (ESP_PARTITION_TABLE_OFFSET > header_read)
        {
            skip_remaining = ESP_PARTITION_TABLE_OFFSET - header_read;
        }
        while (skip_remaining > 0)
        {
            size_t to_read = std::min(skip_remaining, (size_t)FILE_DOWNLOAD_BUFFER_SIZE);
            int read_len = esp_http_client_read(client, (char*)io_buffer, (int)to_read);
            if (read_len <= 0)
            {
                cleanup_client();
                free_buffers();
                _handle_installation_error(FlashStatus::ERROR_FILE_READ);
                return false;
            }
            skip_remaining -= (size_t)read_len;
            total_read += (size_t)read_len;
        }

        uint8_t table_buf[ESP_PARTITION_TABLE_MAX_LEN];
        size_t table_read = 0;
        while (table_read < ESP_PARTITION_TABLE_MAX_LEN)
        {
            int read_len = esp_http_client_read(client,
                                                (char*)(table_buf + table_read),
                                                (size_t)(ESP_PARTITION_TABLE_MAX_LEN - table_read));
            if (read_len <= 0)
            {
                cleanup_client();
                free_buffers();
                _handle_installation_error(FlashStatus::ERROR_FILE_READ);
                return false;
            }
            table_read += (size_t)read_len;
            total_read += (size_t)read_len;
        }

        prefix_buffer = header_buf;
        prefix_size = header_needed;

        const uint8_t* table_ptr = table_buf;
        const esp_partition_info_t* partitions = reinterpret_cast<const esp_partition_info_t*>(table_ptr);
        if (partitions[0].magic != ESP_PARTITION_MAGIC)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_PARTITION_TABLE);
            return false;
        }

        for (int i = 0; i < ESP_PARTITION_TABLE_MAX_ENTRIES; i++)
        {
            const esp_partition_info_t* part = &partitions[i];
            if (part->magic != ESP_PARTITION_MAGIC)
            {
                break;
            }
            file_partitions.push_back(*part);
            if (part->type == PART_TYPE_END)
            {
                break;
            }
        }
    }
    else
    {
        cleanup_client();
        free_buffers();
        _handle_installation_error(FlashStatus::ERROR_INVALID_FIRMWARE);
        return false;
    }

    size_t p_count = file_partitions.size();
    if (p_count == 0)
    {
        cleanup_client();
        free_buffers();
        _handle_installation_error(FlashStatus::ERROR_INVALID_FIRMWARE);
        return false;
    }

    bool custom_install = _data.hal->settings()->getBool("installer", "custom_install");
    PartitionTable flash_ptable;
    if (p_count == 1)
    {
        if (!flash_ptable.load())
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_PARTITION_TABLE);
            return false;
        }
    }
    else
    {
        if (custom_install)
        {
            if (!flash_ptable.load())
            {
                cleanup_client();
                free_buffers();
                _handle_installation_error(FlashStatus::ERROR_PARTITION_TABLE);
                return false;
            }
        }
        else
        {
            if (!_show_confirmation_dialog(std::format("Image bundle has {} partitions", p_count), "Erase other apps?"))
            {
                _data.state = state_browsing;
                cleanup_client();
                free_buffers();
                return false;
            }
            if (!flash_ptable.makeDefaultPartitions())
            {
                cleanup_client();
                free_buffers();
                _handle_installation_error(FlashStatus::ERROR_UNKNOWN);
                return false;
            }
        }
    }

    struct StreamTarget
    {
        size_t src_offset = 0;
        size_t size = 0;
        esp_partition_info_t flash_info = {};
        esp_partition_t update_partition = {};
        std::string label;
        std::string title;
        size_t written = 0;
        size_t first_block_copied = 0;
        size_t first_block_len = 0;
        uint8_t first_block[ENCRYPTED_BLOCK_SIZE];
    };

    std::vector<StreamTarget> targets;
    targets.reserve(p_count);

    esp_partition_info_t boot_partition_info = {};
    bool has_boot_partition = false;
    size_t p_index = 0;

    for (const auto& partition : file_partitions)
    {
        uint8_t subtype = partition.subtype;
        std::string label((const char*)&partition.label);
        p_index++;

        if (partition.type == ESP_PARTITION_TYPE_DATA)
        {
            if (partition.subtype == ESP_PARTITION_SUBTYPE_DATA_OTA)
            {
                _installation_progress_callback(-1, "Skipping OTADATA...", this);
                delay(500);
                continue;
            }
            else if (partition.subtype == ESP_PARTITION_SUBTYPE_DATA_PHY)
            {
                _installation_progress_callback(-1, "Skipping PHY...", this);
                delay(500);
                continue;
            }
        }
        else if (partition.type == ESP_PARTITION_TYPE_APP)
        {
            if (has_boot_partition)
            {
                _installation_progress_callback(-1, "Skipping OTA...", this);
                delay(500);
                continue;
            }
            subtype = flash_ptable.getNextOTA();
            if (subtype == ESP_PARTITION_SUBTYPE_ANY)
            {
                cleanup_client();
                _handle_installation_error(FlashStatus::ERROR_PARTITION_ADD);
                return false;
            }
            label = app_name;
        }

        std::string subtype_str = PartitionTable::getSubtypeString(partition.type, partition.subtype);
        if (p_count > 1 && custom_install &&
            !UTILS::UI::show_confirmation_dialog(_data.hal,
                                                 "Confirm custom install",
                                                 std::format("{}: {}?", subtype_str, label)))
        {
            _installation_progress_callback(-1, std::format("Skipping {}...", subtype_str).c_str(), this);
            delay(500);
            continue;
        }

        size_t free_space = flash_ptable.getFreeSpace(partition.type);
        if (free_space < partition.pos.size)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_INSUFFICIENT_SPACE);
            return false;
        }

        esp_partition_info_t* pi =
            flash_ptable.addPartition(partition.type, subtype, label, 0, partition.pos.size, partition.flags);
        if (pi == nullptr)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_UNKNOWN);
            return false;
        }

        size_t data_size = partition.pos.size;
        if (partition.pos.offset >= file_size)
        {
            ESP_LOGW(TAG,
                     "Skipping partition %s: no data in file (offset 0x%08lx >= file_size 0x%08lx)",
                     label.c_str(),
                     (uint32_t)partition.pos.offset,
                     file_size);
            continue;
        }
        size_t remaining = file_size - partition.pos.offset;
        if (remaining < data_size)
        {
            ESP_LOGW(TAG,
                     "Partition %s truncated by file size: expected 0x%08lx bytes, got 0x%08lx bytes",
                     label.c_str(),
                     (uint32_t)partition.pos.size,
                     remaining);
            data_size = remaining;
        }
        if (data_size == 0)
        {
            ESP_LOGW(TAG, "Skipping partition %s: zero data size", label.c_str());
            continue;
        }

        StreamTarget target;
        target.src_offset = partition.pos.offset;
        target.size = data_size;
        target.flash_info = *pi;
        target.label = label;
        if (p_count > 1)
        {
            target.title = std::format("{} / {}: {} {}KB", p_index, p_count, label, (uint32_t)(target.size / 1024));
        }
        else
        {
            target.title = app_name;
        }

        target.update_partition = {.flash_chip = esp_flash_default_chip,
                                   .type = (esp_partition_type_t)pi->type,
                                   .subtype = (esp_partition_subtype_t)pi->subtype,
                                   .address = pi->pos.offset,
                                   .size = pi->pos.size,
                                   .erase_size = SPI_FLASH_SEC_SIZE,
                                   .label = {0},
                                   .encrypted = false,
                                   .readonly = false};
        strlcpy((char*)&target.update_partition.label, (const char*)&pi->label, sizeof(target.update_partition.label));

        target.first_block_len = std::min((size_t)ENCRYPTED_BLOCK_SIZE, target.size);
        memset(target.first_block, 0, sizeof(target.first_block));

        targets.push_back(target);

        if (partition.type == ESP_PARTITION_TYPE_APP && !has_boot_partition)
        {
            boot_partition_info = *pi;
            has_boot_partition = true;
        }
    }

    if (targets.empty())
    {
        _data.state = state_browsing;
        cleanup_client();
        free_buffers();
        return false;
    }

    std::sort(targets.begin(),
              targets.end(),
              [](const StreamTarget& a, const StreamTarget& b) { return a.src_offset < b.src_offset; });

    size_t target_index = 0;
    size_t stream_offset = 0;

    auto prepare_partition = [&](StreamTarget& target) -> FlashStatus
    {
        _data.install_title = target.title;
        _installation_progress_callback(-1, "Erasing partition...", this);
        delay(500);
        esp_err_t erase_err = esp_partition_erase_range(&target.update_partition, 0, target.update_partition.size);
        if (erase_err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to erase partition: %s", esp_err_to_name(erase_err));
            return FlashStatus::ERROR_FLASH_WRITE;
        }
        return FlashStatus::SUCCESS;
    };

    auto process_chunk = [&](uint8_t* data, size_t len, size_t chunk_offset) -> FlashStatus
    {
        size_t offset = 0;
        while (offset < len)
        {
            if (target_index >= targets.size())
            {
                return FlashStatus::SUCCESS;
            }

            StreamTarget& target = targets[target_index];
            size_t target_start = target.src_offset;
            size_t target_end = target.src_offset + target.size;
            size_t current_offset = chunk_offset + offset;

            if (current_offset < target_start)
            {
                size_t skip = std::min(len - offset, target_start - current_offset);
                offset += skip;
                continue;
            }

            if (current_offset >= target_end)
            {
                target_index++;
                continue;
            }

            size_t in_target_offset = current_offset - target_start;
            size_t write_len = std::min(len - offset, target_end - current_offset);
            if (in_target_offset != target.written)
            {
                ESP_LOGE(TAG, "Stream offset mismatch: expected %zu, got %zu", target.written, in_target_offset);
                return FlashStatus::ERROR_FILE_READ;
            }

            if (target.written == 0)
            {
                FlashStatus prep_status = prepare_partition(target);
                if (prep_status != FlashStatus::SUCCESS)
                {
                    return prep_status;
                }
            }

            uint8_t* write_ptr = data + offset;
            if (target.first_block_copied < target.first_block_len)
            {
                size_t to_copy = std::min(target.first_block_len - target.first_block_copied, write_len);
                memcpy(target.first_block + target.first_block_copied, write_ptr, to_copy);
                memset(write_ptr, 0xFF, to_copy);
                target.first_block_copied += to_copy;
            }

            if (!is_block_empty(write_ptr, write_len))
            {
                esp_err_t write_err = esp_partition_write(&target.update_partition, target.written, write_ptr, write_len);
                if (write_err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to write to flash: %s", esp_err_to_name(write_err));
                    return FlashStatus::ERROR_FLASH_WRITE;
                }
            }

            target.written += write_len;
            std::string status = format_size(target.written, target.size);
            _installation_progress_callback((target.written * 100) / target.size, status.c_str(), this);

            if (target.written >= target.size)
            {
                if (target.first_block_copied < target.first_block_len)
                {
                    return FlashStatus::ERROR_FILE_READ;
                }
                esp_err_t first_err =
                    esp_partition_write(&target.update_partition, 0, target.first_block, target.first_block_len);
                if (first_err != ESP_OK)
                {
                    ESP_LOGE(TAG, "Failed to write first block: %s", esp_err_to_name(first_err));
                    return FlashStatus::ERROR_FLASH_WRITE;
                }
                target_index++;
            }

            offset += write_len;
        }
        return FlashStatus::SUCCESS;
    };

    FlashStatus stream_status = process_chunk(prefix_buffer, prefix_size, stream_offset);
    if (stream_status != FlashStatus::SUCCESS)
    {
        cleanup_client();
        free_buffers();
        _handle_installation_error(stream_status);
        return false;
    }
    stream_offset = total_read;

    while (total_read < file_size)
    {
        int bytes_read = esp_http_client_read(client, (char*)io_buffer, FILE_DOWNLOAD_BUFFER_SIZE);
        if (bytes_read <= 0)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(FlashStatus::ERROR_FILE_READ);
            return false;
        }

        total_read += bytes_read;
        stream_status = process_chunk(io_buffer, (size_t)bytes_read, stream_offset);
        if (stream_status != FlashStatus::SUCCESS)
        {
            cleanup_client();
            free_buffers();
            _handle_installation_error(stream_status);
            return false;
        }
        stream_offset += (size_t)bytes_read;
    }

    cleanup_client();
    free_buffers();

    if (target_index < targets.size())
    {
        _handle_installation_error(FlashStatus::ERROR_FILE_READ);
        return false;
    }

    _installation_progress_callback(-1, "Saving PT...", this);
    delay(500);
    flash_ptable.save();

    if (has_boot_partition && _data.hal->settings()->getBool("installer", "run_on_install"))
    {
        _installation_progress_callback(-1, "Making bootable...", this);
        delay(500);
        FlashStatus status = set_boot_partition(&boot_partition_info);
        if (status != FlashStatus::SUCCESS)
        {
            _handle_installation_error(status);
            return false;
        }
    }

    _installation_progress_callback(100, std::format("Done: {} sec", (uint32_t)((millis() - start_time) / 1000)).c_str(), this);
    delay(2000);
    _handle_installation_complete();
    return true;
}
