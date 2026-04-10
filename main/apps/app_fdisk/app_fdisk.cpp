#include "app_fdisk.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "../utils/ui/dialog.h"
#include "../utils/ui/draw_helper.h"
#include <algorithm>
#include <format>

static const char* TAG = "APP_FDISK";
static const char* HINT_PARTITIONS = "[A]DD [R]ENAME [I]NFO [DEL] [ESC] [ENTER]";
static const char* HINT_HEX_VIEW = "[\u2191][\u2193][\u2190][\u2192] [ENTER] [DEL] [ESC]";

#include "apps/utils/ui/key_repeat.h"
static bool is_repeat = false;
static uint32_t next_fire_ts = 0xFFFFFFFF;
// UI constants
#define LIST_MAX_VISIBLE_ITEMS 4
#define LIST_MAX_DISPLAY_CHARS 22

using namespace MOONCAKE::APPS;
using namespace UTILS::FLASH_TOOLS;

void AppFdisk::onCreate()
{
    // Get hal
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    hl_text_init(&_data.hint_hl_ctx, _data.hal->canvas(), 20, 1500);
}

void AppFdisk::onResume()
{
    ANIM_APP_OPEN();

    _data.hal->canvas()->fillScreen(THEME_COLOR_BG);
    _data.hal->canvas_update();

    // Load partition table
    if (!_data.ptable.load())
    {
        ESP_LOGE(TAG, "Failed to load partition table");
        _data.error_message = "Failed to load partition table";
        _data.state = state_error;
        return;
    }

    // Update partition list
    _update_partition_list();
}

void AppFdisk::onRunning()
{
    bool is_update = false;

    // Handle home button
    if (_data.hal->home_button()->is_pressed())
    {
        _data.hal->keyboard()->resetLastPressedTime();
        if (_data.state == state_hex_view)
        {
            _data.state = state_browsing;
            _data.update_list = true;
            return;
        }

        _data.hal->playNextSound();
        destroyApp();
        return;
    }

    // Handle different app states
    switch (_data.state)
    {
    case state_browsing:
        if (_data.update_list)
            is_update |= _render_partition_list();
        is_update |= _render_control_hint(HINT_PARTITIONS);
        if (is_update)
            _data.hal->canvas_update();
        _handle_list_navigation();
        break;

    case state_add_partition:
        _add_data_partition();
        break;

    case state_info:
        _show_info_dialog();
        break;

    case state_erasing:
        // Handled in erase operation
        break;

    case state_error:
        _show_error_dialog(_data.error_message);
        break;

    case state_hex_view:
        if (_data.hex_view_needs_update)
        {
            _update_hex_view();
            is_update |= _render_hex_view();
        }
        is_update |= _render_control_hint(HINT_HEX_VIEW);
        if (is_update)
            _data.hal->canvas_update();
        _handle_hex_view_navigation();
        break;
    }
}

void AppFdisk::onDestroy() { hl_text_free(&_data.hint_hl_ctx); }

void AppFdisk::_update_partition_list()
{
    _data.partition_list.clear();

    // Convert partition table entries to display items
    for (const auto& partition : _data.ptable.listPartitions())
    {
        PartitionItem_t item;
        item.name = std::string((const char*)partition.label);
        item.type = partition.type;
        item.subtype = partition.subtype;
        item.subtype_str = PartitionTable::getSubtypeString(partition.type, partition.subtype);
        item.offset = partition.pos.offset;
        item.size = partition.pos.size;
        item.flags = partition.flags;

        // Check if partition is bootable
        const esp_partition_t temp_part = {.flash_chip = esp_flash_default_chip,
                                           .type = (esp_partition_type_t)partition.type,
                                           .subtype = (esp_partition_subtype_t)partition.subtype,
                                           .address = partition.pos.offset,
                                           .size = partition.pos.size,
                                           .erase_size = SPI_FLASH_SEC_SIZE,
                                           .label = {0},
                                           .encrypted = false,
                                           .readonly = false};
        item.is_bootable = is_partition_bootable(&temp_part);

        _data.partition_list.push_back(item);
    }

    _data.free_space = _data.ptable.getFreeSpace(ESP_PARTITION_TYPE_APP);
    _data.update_list = true;
}

bool AppFdisk::_render_partition_list()
{
    _clear_screen();

    // Draw header
    _data.hal->canvas()->setTextColor(TFT_ORANGE);
    _data.hal->canvas()->setFont(FONT_16);
    _data.hal->canvas()->drawString(std::format("Free: {}KB", (uint32_t)(_data.free_space / 1024)).c_str(), 5, 0);
    _data.hal->canvas()->setTextColor(TFT_WHITE);
    _data.hal->canvas()->drawRightString(std::format("{} / {}", _data.selected_index + 1, _data.partition_list.size()).c_str(),
                                         _data.hal->canvas()->width() - 6 - 2,
                                         0);

    // Draw partition list
    int y_offset = 20;
    int items_drawn = 0;
    const int max_width = LIST_MAX_DISPLAY_CHARS * 8;

    for (int i = _data.scroll_offset; i < _data.partition_list.size() && items_drawn < LIST_MAX_VISIBLE_ITEMS; i++)
    {
        const auto& item = _data.partition_list[i];
        bool is_data = item.type == ESP_PARTITION_TYPE_DATA;

        if (i == _data.selected_index)
        {
            _data.hal->canvas()->fillRect(5, y_offset + 1, max_width + 25 + 5, 18, THEME_COLOR_BG_SELECTED);
            _data.hal->canvas()->pushImage(11, y_offset + 1 + 1, 16, 16, is_data ? image_data_data_sel : image_data_app_sel);
            _data.hal->canvas()->setTextColor(TFT_BLACK);
        }
        else
        {
            _data.hal->canvas()->pushImage(11, y_offset + 1 + 1, 16, 16, is_data ? image_data_data : image_data_app);
            if (is_data)
                _data.hal->canvas()->setTextColor(TFT_WHITE);
            else
                _data.hal->canvas()->setTextColor(item.is_bootable ? TFT_CYAN : TFT_WHITE);
        }
        // Format display string
        std::string display_text =
            is_data ? std::format("{:13.13s} {:1.1s} {:4d}KB", item.name, item.subtype_str, (uint32_t)(item.size / 1024))
                    : std::format("{:15.15s} {:4d}KB", item.name, (uint32_t)(item.size / 1024));
        _data.hal->canvas()->drawString(display_text.c_str(), 30, y_offset + 1);
        y_offset += 18 + 1;
        items_drawn++;
    }

    // Draw scrollbar if needed
    if (_data.partition_list.size() > LIST_MAX_VISIBLE_ITEMS)
    {
        _render_scrollbar();
    }

    _data.update_list = false;
    return true;
}

bool AppFdisk::_render_control_hint(const char* hint)
{
    return hl_text_render(&_data.hint_hl_ctx,
                          hint,
                          0,
                          _data.hal->canvas()->height() - 12,
                          TFT_DARKGREY,
                          TFT_WHITE,
                          THEME_COLOR_BG);
}

void AppFdisk::_render_scrollbar()
{
    const int scrollbar_width = 6;
    UTILS::UI::draw_scrollbar(_data.hal->canvas(),
                              _data.hal->canvas()->width() - scrollbar_width - 2,
                              20,
                              scrollbar_width,
                              19 * LIST_MAX_VISIBLE_ITEMS,
                              (int)_data.partition_list.size(),
                              LIST_MAX_VISIBLE_ITEMS,
                              _data.scroll_offset);
}

void AppFdisk::_handle_list_navigation()
{
    if (_data.partition_list.empty())
    {
        return;
    }

    bool selection_changed = false;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();
    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();
        // Navigation
        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_index > 0)
                {
                    _data.hal->playNextSound();
                    _data.selected_index--;
                    if (_data.selected_index < _data.scroll_offset)
                    {
                        _data.scroll_offset = _data.selected_index;
                    }
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_index < _data.partition_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    _data.selected_index++;
                    if (_data.selected_index >= _data.scroll_offset + LIST_MAX_VISIBLE_ITEMS)
                    {
                        _data.scroll_offset = _data.selected_index - LIST_MAX_VISIBLE_ITEMS + 1;
                    }
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_index > 0)
                {
                    _data.hal->playNextSound();
                    // Jump up by visible_items count (page up)
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_index = std::max(0, _data.selected_index - jump);
                    _data.scroll_offset = std::max(0, _data.selected_index - (LIST_MAX_VISIBLE_ITEMS - 1));
                    selection_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                if (_data.selected_index < _data.partition_list.size() - 1)
                {
                    _data.hal->playNextSound();
                    // Jump down by visible_items count (page down)
                    int jump = LIST_MAX_VISIBLE_ITEMS;
                    _data.selected_index = std::min((int)_data.partition_list.size() - 1, _data.selected_index + jump);
                    _data.scroll_offset =
                        std::min(std::max(0, (int)_data.partition_list.size() - LIST_MAX_VISIBLE_ITEMS), _data.selected_index);
                    selection_changed = true;
                }
            }
        }
        // Actions
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_A))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_A);

            _data.state = state_add_partition;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_I))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_I);

            _data.state = state_info;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

            // Enter hex view mode
            _init_hex_view();
            _data.state = state_hex_view;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);
            _delete_partition();
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);
            destroyApp();
        }
#if 0
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_E))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_E);
            _erase_partition();
        }
#endif
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_R))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_R);
            _rename_partition();
        }
    }
    else
    {
        is_repeat = false;
    }

    if (selection_changed)
    {
        _data.update_list = true;
    }
}

void AppFdisk::_clear_screen() { _data.hal->canvas()->fillScreen(THEME_COLOR_BG); }

std::string AppFdisk::_format_size(uint32_t size) { return std::format("{}KB", (uint32_t)(size / 1024)); }

std::string AppFdisk::_format_offset(uint32_t offset) { return std::format("0x{:08X}", offset); }

bool AppFdisk::_is_system_partition(const PartitionItem_t& item)
{
    // Check if partition is critical for system operation
    return (item.name == "apps_nvs" || item.name == "apps_ota" || item.name == "phy_init");
}

bool AppFdisk::_changes_require_reflash()
{
    // TODO: Implement logic to check if partition table changes require reflashing
    return false;
}

bool AppFdisk::_show_confirmation_dialog(const std::string& message)
{
    return UTILS::UI::show_confirmation_dialog(_data.hal, "Confirm", message);
}

void AppFdisk::_show_error_dialog(const std::string& message)
{
    UTILS::UI::show_error_dialog(_data.hal, "Error", message);
    _data.state = state_browsing;
    _data.update_list = true;
}

void AppFdisk::_show_erase_progress(int progress)
{
    UTILS::UI::show_progress(_data.hal, "Erasing", progress, "Please wait...");
}

#if 0
void AppFdisk::_erase_partition()
{
    if (_data.selected_index >= _data.partition_list.size())
    {
        return;
    }

    const auto& selected_partition = _data.partition_list[_data.selected_index];

    // Check if this is a system partition
    if (_is_system_partition(selected_partition))
    {
        _data.error_message = "Can't erase system partition";
        _data.state = state_error;
        return;
    }

    // Check if this is the current running partition
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->address == selected_partition.offset)
    {
        _data.error_message = "Can't erase running partition";
        _data.state = state_error;
        return;
    }

    // Show confirmation dialog
    std::string title = std::format("Erase {}?", selected_partition.name);
    if (!UTILS::UI::show_confirmation_dialog(_data.hal, title, "Erase partition?", "Erase", "Cancel"))
    {
        _data.update_list = true;
        return;
    }

    _data.state = state_erasing;

    // TODO: Implement actual erase operation with progress updates
    for (int i = 0; i <= 100; i += 10)
    {
        _show_erase_progress(i);
        delay(100);
    }

    // Show completion message
    UTILS::UI::show_message_dialog(_data.hal, "Success", "Partition erased", 0);
    _data.state = state_browsing;
    _data.update_list = true;
}
#endif

void AppFdisk::_delete_partition()
{
    if (_data.selected_index >= _data.partition_list.size())
    {
        return;
    }

    const auto& selected_partition = _data.partition_list[_data.selected_index];

    // Check if this is a system partition
    if (_is_system_partition(selected_partition))
    {
        _data.error_message = "Can't delete system partition";
        _data.state = state_error;
        return;
    }

    // Check if this is the current running partition
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->address == selected_partition.offset)
    {
        _data.error_message = "Can't delete running partition";
        _data.state = state_error;
        return;
    }

    // Show confirmation dialog with more details
    std::string title = std::format("{} {}KB @ {}",
                                    selected_partition.name,
                                    (uint32_t)(selected_partition.size / 1024),
                                    _format_offset(selected_partition.offset));
    std::vector<UTILS::UI::DialogButton_t> buttons;
    buttons.push_back(UTILS::UI::DialogButton_t("Delete", THEME_COLOR_BG_SELECTED, TFT_BLACK));
    buttons.push_back(UTILS::UI::DialogButton_t("Cancel", THEME_COLOR_BG, TFT_WHITE));

    // int result = UTILS::UI::show_dialog(_data.hal, "Warning", TFT_RED, confirm_msg, TFT_WHITE, buttons);
    if (!UTILS::UI::show_confirmation_dialog(_data.hal, title, "Delete partition?", "Delete", "Cancel"))
    {
        // not confirmed
        _data.update_list = true;
        return;
    }
    // sound glitch
    delay(100);
    // Delete the partition
    if (!_data.ptable.deletePartition(_data.selected_index, &AppFdisk::_delete_progress_callback, this))
    {
        _data.error_message = "Failed to delete partition";
        _data.state = state_error;
        return;
    }

    UTILS::UI::show_progress(_data.hal, "Deleting", 100, "Saving changes...");

    // Save changes
    if (!_data.ptable.save())
    {
        _data.error_message = "Failed to save partition table";
        _data.state = state_error;
        return;
    }
    delay(500);
    // Show completion message
    // reboot if partition type is app
    if (selected_partition.type == ESP_PARTITION_TYPE_APP)
    {
        if (UTILS::UI::show_message_dialog(_data.hal, "Partition deleted", "restart in", 5000) == 0)
        {
            _data.hal->reboot();
        }
    }
    else
    {
        UTILS::UI::show_message_dialog(_data.hal, "Success", "Partition deleted successfully");
    }

    // Update the display
    _data.selected_index = std::min(_data.selected_index, (int)_data.partition_list.size() - 2);
    _update_partition_list();
}

void AppFdisk::_delete_progress_callback(int progress, const char* message, void* arg_cb)
{
    AppFdisk* app = static_cast<AppFdisk*>(arg_cb);
    std::string msg = message;
    UTILS::UI::show_progress(app->_data.hal, "Deleting", progress, msg);
}

void AppFdisk::_show_info_dialog()
{
    const auto& selected_partition = _data.partition_list[_data.selected_index];

    std::string title = std::format("{}: {} / {}",
                                    selected_partition.name,
                                    selected_partition.type == ESP_PARTITION_TYPE_APP ? "APP" : "DATA",
                                    selected_partition.subtype_str);
    std::string message =
        std::format("{}KB @ {}", (uint32_t)(selected_partition.size / 1024), _format_offset(selected_partition.offset));
    UTILS::UI::show_message_dialog(_data.hal, title.c_str(), message.c_str(), 0);
    _data.state = state_browsing;
    _data.update_list = true;
}

void AppFdisk::_add_data_partition()
{
    // Define available data partition subtypes
    std::vector<std::string> subtype_options =
        {"fat", "spiffs", "littlefs", "nvs", "coredump", "ota", "nvs_keys", "efuse", "esphttpd"};

    // Show subtype selection dialog
    int selected_subtype = UTILS::UI::show_select_dialog(_data.hal, "Subtype to add", subtype_options, 0);
    if (selected_subtype < 0)
    {
        _data.state = state_browsing;
        _data.update_list = true;
        return; // User cancelled
    }

    // Get partition name
    std::string partition_name = subtype_options[selected_subtype];
    if (!UTILS::UI::show_edit_string_dialog(_data.hal, "Partition name", partition_name, false, 16))
    {
        _data.state = state_browsing;
        _data.update_list = true;
        return; // User cancelled
    }
    // get partition size, kb
    int partition_size = 1024;
    if (!UTILS::UI::show_edit_number_dialog(_data.hal, "Partition size, KB", partition_size, 1, _data.free_space / 1024))
    {
        _data.state = state_browsing;
        _data.update_list = true;
        return; // User cancelled
    }

    if (partition_name.empty() || partition_name.length() > 15)
    {
        _data.error_message = "Invalid name, should be 1-15 characters";
        _data.state = state_error;
        return;
    }

    // Convert string to ESP partition subtype
    esp_partition_subtype_t subtype;
    switch (selected_subtype)
    {
    case 0:
        subtype = ESP_PARTITION_SUBTYPE_DATA_FAT;
        break;
    case 1:
        subtype = ESP_PARTITION_SUBTYPE_DATA_SPIFFS;
        break;
    case 2:
        subtype = ESP_PARTITION_SUBTYPE_DATA_LITTLEFS;
        break;
    case 3:
        subtype = ESP_PARTITION_SUBTYPE_DATA_NVS;
        break;
    case 4:
        subtype = ESP_PARTITION_SUBTYPE_DATA_COREDUMP;
        break;
    case 5:
        subtype = ESP_PARTITION_SUBTYPE_DATA_OTA;
        break;
    case 6:
        subtype = ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS;
        break;
    case 7:
        subtype = ESP_PARTITION_SUBTYPE_DATA_EFUSE_EM;
        break;
    case 8:
        subtype = ESP_PARTITION_SUBTYPE_DATA_ESPHTTPD;
        break;
    default:
        subtype = ESP_PARTITION_SUBTYPE_DATA_FAT;
        break;
    }

    // Show progress while adding partition
    UTILS::UI::show_progress(_data.hal, "Adding partition", -1, "Creating partition...");

    // Add the partition to the partition table
    esp_partition_info_t* new_partition =
        _data.ptable.addPartition(ESP_PARTITION_TYPE_DATA, subtype, partition_name, 0, partition_size * 1024);
    if (!new_partition)
    {
        _data.error_message = "Failed to add partition";
        _data.state = state_error;
        return;
    }

    // Save the partition table
    if (!_data.ptable.save())
    {
        _data.error_message = "Failed to save partition table";
        _data.state = state_error;
        return;
    }

    // Show completion message
    UTILS::UI::show_message_dialog(_data.hal, "Success", "Partition added successfully");

    // Update the display
    _update_partition_list();
    _data.state = state_browsing;
    _data.update_list = true;
}

void AppFdisk::_init_hex_view()
{
    if (_data.selected_index >= _data.partition_list.size())
    {
        return;
    }

    _data.hex_view_offset = 0;
    _data.hex_view_size = _data.partition_list[_data.selected_index].size;
    _data.hex_view_cursor = 0;

    // Calculate lines per page based on screen size and font
    int line_height = 10; // Approximate line height for FONT_10
    _data.hex_view_lines_per_page = (_data.hal->canvas()->height() - 12 * 2) / line_height;

    _data.hex_view_needs_update = true;
}

void AppFdisk::_update_hex_view()
{
    if (_data.hex_view_needs_update)
    {
        // ESP_LOGI(TAG, "Reading %ld bytes from 0x%08lX", _data.hex_view_size,
        // _data.partition_list[_data.selected_index].offset);
        uint32_t read_size = std::min((uint32_t)sizeof(_data.hex_view_buffer), _data.hex_view_size);
        UTILS::FLASH_TOOLS::bootloader_flash_read(_data.partition_list[_data.selected_index].offset + _data.hex_view_offset,
                                                  _data.hex_view_buffer,
                                                  read_size,
                                                  false);
    }
}

bool AppFdisk::_render_hex_view()
{
    const int bytes_per_line = 16;

    _clear_screen();

    // Draw header
    _data.hal->canvas()->setFont(FONT_10);
    _data.hal->canvas()->setTextColor(TFT_ORANGE);

    // Show partition name and current offset
    std::string header = std::format("{}: {:06X}", _data.partition_list[_data.selected_index].name, _data.hex_view_offset);
    _data.hal->canvas()->drawString(header.c_str(), 0, 0);

    // Draw the buffer content in hex and ASCII
    int y = 12; // Start position after header

    for (uint32_t i = 0; i < _data.hex_view_lines_per_page; i++)
    {
        uint32_t offset = _data.hex_view_offset + (i * bytes_per_line);

        // Don't go beyond partition size
        if (offset >= _data.hex_view_size)
            break;

        // Calculate how many bytes to display on this line
        uint32_t bytes_this_line = std::min((uint32_t)bytes_per_line, _data.hex_view_size - offset);

        // Line address
        std::string addr = std::format("{:06X}", offset);
        _data.hal->canvas()->setTextColor(TFT_CYAN);
        _data.hal->canvas()->drawString(addr.c_str(), 0, y);

        // Highlight the current line if it's at the cursor
        bool is_cursor_line = (_data.hex_view_cursor / bytes_per_line == i);
        uint16_t hex_color = is_cursor_line ? TFT_YELLOW : TFT_WHITE;

        // checl mode, hex or ascii
        std::string hex_str;
        if (_data.hex_view_ascii)
        {
            // ASCII representation
            hex_str = "| ";
            for (uint32_t j = 0; j < bytes_this_line; j++)
            {
                uint8_t byte = _data.hex_view_buffer[i * bytes_per_line + j];
                // Only printable ASCII characters
                if (byte >= 32 && byte < 127)
                    hex_str += (char)byte;
                else
                    hex_str += '.';
                hex_str += ' ';
            }
            hex_str += "|";
        }
        else
        {
            // Hex values
            for (uint32_t j = 0; j < bytes_this_line; j++)
            {
                uint8_t byte = _data.hex_view_buffer[i * bytes_per_line + j];
                hex_str += std::format("{:02X}", byte);
                if (j % 8 == 7)
                    hex_str += " | ";
                else if (j % 4 == 3)
                    hex_str += " ";
            }
        }

        _data.hal->canvas()->setTextColor(hex_color);
        _data.hal->canvas()->drawString(hex_str.c_str(), 34, y);

        y += 10; // Move to next line
    }

    _data.hex_view_needs_update = false;
    return true;
}

void AppFdisk::_handle_hex_view_navigation()
{
    const int bytes_per_line = 16;
    bool data_changed = false;

    _data.hal->keyboard()->updateKeyList();
    _data.hal->keyboard()->updateKeysState();

    if (_data.hal->keyboard()->isPressed())
    {
        uint32_t now = millis();

        if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_UP))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                // Move cursor up one line
                if (_data.hex_view_offset > 0)
                {
                    _data.hal->playNextSound();
                    _data.hex_view_offset -= bytes_per_line;
                    _data.hex_view_cursor = 0; // Reset cursor to start of visible area
                    data_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_DOWN))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                // Move cursor down one line
                uint32_t next_offset = _data.hex_view_offset + bytes_per_line;
                uint32_t page_size = _data.hex_view_lines_per_page * bytes_per_line;
                if (next_offset <= (_data.hex_view_size < page_size ? _data.hex_view_size : _data.hex_view_size - page_size))
                {
                    _data.hal->playNextSound();
                    _data.hex_view_offset = next_offset;
                    _data.hex_view_cursor = 0; // Reset cursor to start of visible area
                    data_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_LEFT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                // Move cursor left one page
                uint32_t page_size = _data.hex_view_lines_per_page * bytes_per_line;
                if (_data.hex_view_offset >= page_size)
                {
                    _data.hal->playNextSound();
                    _data.hex_view_offset -= page_size;
                    _data.hex_view_cursor = 0; // Reset cursor to start of visible area
                    data_changed = true;
                }
                else if (_data.hex_view_offset > 0)
                {
                    _data.hal->playNextSound();
                    _data.hex_view_offset = 0;
                    _data.hex_view_cursor = 0;
                    data_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_RIGHT))
        {
            if (key_repeat_check(is_repeat, next_fire_ts, now))
            {
                // Move cursor right one page
                uint32_t page_size = _data.hex_view_lines_per_page * bytes_per_line;
                uint32_t next_offset = _data.hex_view_offset + page_size;
                if (next_offset < _data.hex_view_size)
                {
                    _data.hal->playNextSound();
                    _data.hex_view_offset = next_offset;
                    _data.hex_view_cursor = 0; // Reset cursor to start of visible area
                    data_changed = true;
                }
            }
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_BACKSPACE))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_BACKSPACE);

            // Exit hex view mode
            _data.state = state_browsing;
            _data.update_list = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ESC))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ESC);

            // Exit hex view mode
            _data.state = state_browsing;
            _data.update_list = true;
        }
        else if (_data.hal->keyboard()->isKeyPressing(KEY_NUM_ENTER))
        {
            _data.hal->playNextSound();
            _data.hal->keyboard()->waitForRelease(KEY_NUM_ENTER);

            // Toggle hex view mode
            _data.hex_view_ascii = !_data.hex_view_ascii;
            _data.hex_view_needs_update = true;
        }
    }
    else
    {
        is_repeat = false;
    }

    // If the view has changed, read new data and update display
    if (data_changed)
    {
        _data.hex_view_needs_update = true;
    }
}

void AppFdisk::_rename_partition()
{
    if (_data.selected_index >= _data.partition_list.size())
    {
        return;
    }

    const auto& selected_partition = _data.partition_list[_data.selected_index];

    // Check if this is a system partition
    if (_is_system_partition(selected_partition))
    {
        _data.error_message = "Can't rename system partition";
        _data.state = state_error;
        return;
    }

    // Check if this is the current running partition
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running && running->address == selected_partition.offset)
    {
        _data.error_message = "Can't rename running partition";
        _data.state = state_error;
        return;
    }

    // Get current name as default text
    std::string name = selected_partition.name;

    // Show text input dialog
    if (!UTILS::UI::show_edit_string_dialog(_data.hal, "Rename partition", name, false, 15))
    {
        // cancelled by user
        _data.update_list = true;
        return;
    }

    // Check if user cancelled
    if (name.empty() || name == selected_partition.name)
    {
        _data.update_list = true;
        return;
    }

    // Update the partition label
    esp_partition_info_t* pi = _data.ptable.getPartition(_data.selected_index);
    if (pi == nullptr)
    {
        _data.error_message = "Failed to get partition";
        _data.state = state_error;
        return;
    }
    memset(pi->label, 0, sizeof(pi->label));
    strncpy((char*)pi->label, name.c_str(), sizeof(pi->label) - 1);

    // Show progress while saving
    UTILS::UI::show_progress(_data.hal, "Renaming", -1, "Saving changes...");

    // Save changes
    if (!_data.ptable.save())
    {
        _data.error_message = "Failed to save partition table";
        _data.state = state_error;
        return;
    }
    // TODO: remove menu item
    // remove menu item. calculate index, consider creation order
    // int index = 0;
    // for (const auto& item : _data.partition_list)
    // {
    //     if (item.offset == selected_partition.offset)
    //         break;
    //     index++;
    // }
    // g_shared_menu->removeItem(index);
    // Show success message
    UTILS::UI::show_message_dialog(_data.hal, "Success", "close in");

    // Update the display
    _update_partition_list();
}
