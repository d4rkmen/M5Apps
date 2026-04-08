/**
 * @file app_installer.h
 * @brief ROM Installer app for M5Cardputer
 * @version 0.1
 * @date 2024-01-09
 *
 * @copyright Copyright (c) 2024
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
#include "apps/utils/flash/flash_tools.h"
#include "apps/utils/flash/ptable_tools.h"
#include "apps/utils/ui/dialog.h"
#include "apps/utils/dir_list.h"

#include "assets/installer_big.h"
#include "assets/installer_small.h"
#include "assets/sd.h"
#include "assets/sd_big.h"
#include "assets/rom.h"
#include "assets/rom_sel.h"
#include "assets/folder.h"
#include "assets/folder_sel.h"
#include "assets/usb_flash.h"
#include "assets/cloud.h"

namespace MOONCAKE
{
    namespace APPS
    {
        class AppInstaller : public APP_BASE
        {
        private:
            enum SourceType_t
            {
                source_cloud = 0,
                source_sdcard,
                source_usb
            };

            enum InstallerState_t
            {
                state_source = 0,
                state_browsing,
                state_installing,
                state_complete,
                state_error
            };

            struct FileItem_t
            {
                std::string name;
                bool is_dir;
                uint64_t size;
                std::string fname;
                std::string info;

                // FileItem_t(const std::string& n, bool dir, uint64_t s, const std::string& f, const std::string& i)
                //     : name(n), is_dir(dir), size(s), fname(f), info(i)
                // {
                // }
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

            struct Data_t
            {
                HAL::Hal* hal = nullptr;
                SourceType_t source_type = source_sdcard;
                bool sdcard_initialized = false;
                bool usb_initialized = false;
                InstallerState_t state = state_source;

                // File listing data (SD/USB use dir_list, cloud uses cached JSON)
                UTILS::DirList dir_list;
                std::vector<SelectItem_t> sources;
                std::string current_path = "/";
                std::string current_desc = "";
                int selected_file = 0;
                int selected_source = 0;
                int scroll_offset = 0;
                bool list_needs_update = true;

                // Cloud JSON cache (lazy parsing: only visible items parsed on demand)
                // Offset index: 8 bytes per item for O(1) access (vs ~200 bytes/item for FileItem_t)
                struct JsonSpan
                {
                    int off, len;
                };
                char* cloud_json = nullptr;
                int cloud_json_len = 0;
                const char* cloud_col_tok = nullptr;
                const char* cloud_app_tok = nullptr;
                JsonSpan* cloud_idx = nullptr;
                int cloud_col_count = 0;
                int cloud_app_count = 0;
                bool cloud_has_back = false;
                // render flags
                bool update_sdcard_info = false;
                bool update_usb_info = false;
                bool update_cloud_info = false;
                bool update_file_list = false;
                bool update_source_list = false;
                // Scrolling text contexts
                UTILS::SCROLL_TEXT::ScrollTextContext_t list_scroll_ctx; // For list item scrolling
                UTILS::SCROLL_TEXT::ScrollTextContext_t path_scroll_ctx; // For current path scrolling
                UTILS::SCROLL_TEXT::ScrollTextContext_t desc_scroll_ctx; // For current description scrolling
                UTILS::HL_TEXT::HLTextContext_t hint_hl_ctx;
                // Installation data
                std::string firmware_path;
                int install_progress = 0;
                std::string install_title;
                std::string install_status;

                bool cloud_initialized = false;
                // base url to download firmware
                std::string current_base_url;
                std::string error_message;
            };
            Data_t _data;
            const FileItem_t BACK_DIR_ITEM = {"..", true, 0, "", ""};
            void _clear_file_list();
            int _item_count();
            FileItem_t _item_at(int index);
            void _free_cloud_cache();
            // Helper methods
            bool _has_extension(const std::string& filename, const std::string& ext);
            std::string _truncate_path(const std::string& path, int max_chars);
            // std::string formatSize(uint64_t bytes);
            void _clear_screen();
            bool _render_sdcard_info();
            bool _render_usb_info();
            bool _render_cloud_info();
            bool _render_scrolling_path();
            bool _render_scrolling_desc();
            bool _render_scrolling_list();
            bool _render_file_list();
            bool _render_source_list();
            bool _render_scrollbar();
            bool _render_source_hint();

            void _handle_source_selection();
            void _init_cloud_source();
            bool _init_sdcard_source();
            bool _init_usb_source();
            void _update_source_list();

            // SD card functions
            void _mount_sdcard();
            void _unmount_sdcard();
            void _mount_usb();
            void _unmount_usb();
            bool _is_source_mounted();
            void _update_file_list();
            void _update_cloud_file_list();
            void _update_source_file_list();
            bool _handle_file_selection();
            void _navigate_directory(const std::string& path);
            bool _show_confirmation_dialog(const std::string& title, const std::string& message);

            // Firmware installation functions
            void _install_firmware(const std::string& filepath);
            void _render_installation_progress();
            void _handle_installation_complete();
            void _handle_installation_error(UTILS::FLASH_TOOLS::FlashStatus status);
            static void _installation_progress_callback(int progress, const char* message, void* arg_cb);

            // Add this helper method declaration
            std::string _url_encode(const std::string& str);

            // Add this to the private methods section
            bool _download_cloud_file(const std::string& url, const std::string& dest_path, const std::string& display_name);

        public:
            void onCreate() override;
            void onResume() override;
            void onRunning() override;
            void onDestroy() override;
        };

        class AppInstaller_Packer : public APP_PACKER_BASE
        {
            std::string getAppName() override { return "INSTALLER"; }
            std::string getAppDesc() override { return "Install firmware apps to flash"; }
            void* getAppIcon() override { return (void*)(new AppIcon_t(image_data_installer_big, nullptr)); }
            void* newApp() override { return new AppInstaller; }
            void deleteApp(void* app) override { delete (AppInstaller*)app; }
        };
    } // namespace APPS
} // namespace MOONCAKE