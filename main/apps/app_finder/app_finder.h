/**
 * @file app_finder.h
 * @brief File Manager app for M5Cardputer
 * @version 0.1
 * @date 2025-01-09
 *
 * @copyright Copyright (c) 2025
 *
 */
#pragma once
#include <mooncake.h>
#include "hal.h"
#include "apps/utils/theme/theme_define.h"
#include "apps/utils/anim/anim_define.h"
#include "apps/utils/icon/icon_define.h"
#include "apps/utils/anim/scroll_text.h"
#include "apps/utils/anim/hl_text.h"
#include "apps/utils/ui/dialog.h"
#include "apps/utils/dir_list.h"

#include "assets/finder_big.h"
#include "assets/finder_small.h"
#include "assets/file.h"
#include "assets/file_sel.h"
#include "assets/folder.h"
#include "assets/folder_sel.h"

namespace MOONCAKE
{
    namespace APPS
    {
        class AppFinder : public APP_BASE
        {
        private:
            enum PanelType_t
            {
                panel_left = 0,
                panel_right
            };

            struct FileItem_t
            {
                std::string name;
                bool is_dir;
                uint64_t size;
                std::string fname;
                std::string info;

                FileItem_t(const std::string& n, bool dir, uint64_t s, const std::string& f, const std::string& i)
                    : name(n), is_dir(dir), size(s), fname(f), info(i)
                {
                }
            };

            struct SelectItem_t
            {
                std::string name;
                int x = 0;
                int y = 0;
                const uint16_t* image = nullptr;
                std::string hint;
                SelectItem_t(std::string name, int x, int y, const uint16_t* image, std::string hint)
                {
                    this->name = name;
                    this->x = x;
                    this->y = y;
                    this->image = image;
                    this->hint = hint;
                }
            };

            struct PanelData_t
            {
                bool initialized = false;
                std::string current_path = "/";
                UTILS::DirList dir_list;
                int selected_file = 0;
                int scroll_offset = 0;
                bool needs_update = true;
                bool panel_info_needs_update = true;

                // Scrolling text contexts
                UTILS::SCROLL_TEXT::ScrollTextContext_t list_scroll_ctx;
                UTILS::SCROLL_TEXT::ScrollTextContext_t path_scroll_ctx;
            };

            struct Data_t
            {
                HAL::Hal* hal = nullptr;
                PanelType_t active_panel = panel_left;

                // Two panels
                PanelData_t left_panel;
                PanelData_t right_panel;

                // UI elements
                UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
                // bool sdcard_initialized = false;
                // bool usb_initialized = false;
                bool panel_info_needs_update = false;
            };
            Data_t _data;

            const FileItem_t BACK_DIR_ITEM = {"..", true, 0, "", ""};
            const FileItem_t SD_CARD_ITEM = {"sdcard", true, 0, "sdcard", "SD card"};
            const FileItem_t USB_ITEM = {"usb", true, 0, "usb", "USB drive"};
            // Item access (virtual list — constructs on the fly from DirList)
            int _panel_item_count(const PanelData_t& panel);
            FileItem_t _panel_item_at(const PanelData_t& panel, int index);

            // Helper methods
            bool _has_extension(const std::string& filename, const std::string& ext);
            std::string _truncate_path(const std::string& path, int max_chars);
            void _clear_screen();

            // Panel management
            void _init_panel(PanelData_t& panel);
            void _update_panel_file_list(PanelData_t& panel);
            void _navigate_panel_directory(PanelData_t& panel, const std::string& path);

            // Rendering
            bool _render_panel_info(PanelData_t& panel, int panel_x, int panel_width, bool is_active = false);
            bool _render_panel_file_list(PanelData_t& panel, int panel_x, int panel_width, bool is_active = false);
            bool _render_panel_scrollbar(PanelData_t& panel, int panel_x, int panel_width);
            bool _render_scrolling_path(PanelData_t& panel, int panel_x, bool is_active = false);
            bool _render_scrolling_list(PanelData_t& panel, int panel_x, int panel_width);
            bool _render_hint();

            // Input handling
            bool _handle_file_selection(PanelData_t& panel);

            // File operations
            bool _copy_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name);
            bool _copy_single_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name);
            bool
            _copy_directory_recursive(const std::string& src_dir, const std::string& dest_dir, const std::string& display_name);
            bool _move_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name);
            bool _move_single_file(const std::string& src_path, const std::string& dest_path, const std::string& display_name);
            bool
            _move_directory_recursive(const std::string& src_dir, const std::string& dest_dir, const std::string& display_name);
            bool _delete_file_or_folder(const std::string& path, const std::string& display_name, bool is_dir);
            bool _delete_directory_recursive(const std::string& dir_path);

            // Mounting
            void _mount_sdcard();
            void _mount_usb();

        public:
            void onCreate() override;
            void onResume() override;
            void onRunning() override;
            void onDestroy() override;
        };

        class AppFinder_Packer : public APP_PACKER_BASE
        {
            std::string getAppName() override { return "FINDER"; }
            std::string getAppDesc() override { return "Two-panel file manager"; }
            void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_finder_big, nullptr)); }
            void* newApp() override { return new AppFinder; }
            void deleteApp(void* app) override { delete (AppFinder*)app; }
        };
    } // namespace APPS
} // namespace MOONCAKE
