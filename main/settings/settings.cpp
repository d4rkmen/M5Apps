/**
 * @file settings.cpp
 * @brief Settings management system implementation
 */

#include "settings.h"
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <stdexcept>
#include <map>
#include <algorithm>

static const char* TAG = "SETTINGS";

namespace SETTINGS
{

    const char* Settings::NVS_PARTITION = "apps_nvs";

    Settings::Settings() : _initialized(false)
    {
        // back item
        SettingItem_t back_item = {"", "[..]", TYPE_NONE, "back", "back", "", "", "Return back to the parent menu"};
        // Define metadata for all settings
        SettingGroup_t wifi_group;
        wifi_group.name = "WiFi settings";
        wifi_group.nvs_namespace = "wifi";
        wifi_group.items = {
            back_item,
            {"enabled", "Enabled", TYPE_BOOL, "false", "false", "", "", "Enable WiFi and connect to selected network"},
            {"ssid",
             "Network",
             TYPE_STRING,
             "",
             "",
             "",
             "",
             "WiFi network name (SSID) to connect to. Hold [Fn] key to enter SSID manually"},
            {"pass", "Password", TYPE_STRING, "", "", "", "", "WiFi network security password"},
            {"static_ip", "Static IP", TYPE_BOOL, "false", "false", "", "", "Use static IP address or let DHCP assign it"},
            {"ip", "IP addr", TYPE_STRING, "", "", "", "", "Static IP address to use or last DHCP assigned address"},
            {"mask", "Mask", TYPE_STRING, "", "", "", "", "Subnet mask. Need to be set if static IP is used"},
            {"gw", "Gateway", TYPE_STRING, "", "", "", "", "Gateway IP address. Need to be set if static IP is used"},
            {"dns", "DNS", TYPE_STRING, "", "", "", "", "DNS server IP address. Need to be set if static IP is used"}};

        SettingGroup_t sys_group;
        sys_group.name = "System settings";
        sys_group.nvs_namespace = "system";
        sys_group.items = {
            back_item,
            {"brightness", "Brightness", TYPE_NUMBER, "100", "100", "10", "255", "Screen brightness level (10-255)"},
            {"volume", "Volume", TYPE_NUMBER, "64", "64", "0", "255", "System sound volume level (0-255)"},
            {"usb_host", "USB host", TYPE_BOOL, "false", "false", "", "", "Enable USB host support for connecting USB devices"},
            {"use_led", "Use LED", TYPE_BOOL, "true", "true", "", "", "Use LED for system notifications"},
            {"dim_time", "Dim seconds", TYPE_NUMBER, "30", "30", "0", "3600", "Screen dimming time in seconds (0-3600)"},
            {"boot_sound", "Boot sound", TYPE_BOOL, "true", "true", "", "", "Play boot sound on startup"},
            {"show_bat_volt", "Battery voltage", TYPE_BOOL, "true", "true", "", "", "Show battery voltage on the system bar"},
            {"show_time", "Show time", TYPE_BOOL, "true", "true", "", "", "Show time on the system bar"},
            {"last_app", "Run last app", TYPE_BOOL, "true", "true", "", "", "Run the last used app on startup"},
            {"last_app_to",
             "Run timeout",
             TYPE_NUMBER,
             "3",
             "3",
             "0",
             "60",
             "Time before running last app in seconds (0-60). Holding any key on boot will cancel running last app and open "
             "Apps menu"},

        };
        SettingGroup_t installer_group;
        installer_group.name = "Installer settings";
        installer_group.nvs_namespace = "installer";
        installer_group.items = {back_item,
                                 {"run_on_install",
                                  "Run new app",
                                  TYPE_BOOL,
                                  "true",
                                  "true",
                                  "",
                                  "",
                                  "Make app bootable after installation and run it"},
                                 {"custom_install",
                                  "Custom install",
                                  TYPE_BOOL,
                                  "false",
                                  "false",
                                  "",
                                  "",
                                  "Ask confirmation for every partiotion for multi-partition image bundle. This helps to "
                                  "install only selected partiotions (for example only app, without spiffs)"},
                                 {"auto_delete",
                                  "Delete temp file",
                                  TYPE_BOOL,
                                  "false",
                                  "false",
                                  "",
                                  "",
                                  "Delete downloaded files after installation"},
                                 {"dl_path",
                                  "Downloads",
                                  TYPE_STRING,
                                  "/sdcard/downloads",
                                  "/sdcard/downloads",
                                  "",
                                  "",
                                  "Path to store temp files downloaded from the internet"}};
        SettingGroup_t flood_group;
        flood_group.name = "Flood settings";
        flood_group.nvs_namespace = "flood";
        flood_group.items = {
            back_item,
            {"node_name", "Node name", TYPE_STRING, "Flooder", "Flooder", "", "32", "Node name in the mesh network"},
            // {"device_role", "Device role", TYPE_STRING, "router", "router", "client;router;repeater", "", "Device role in the
            // mesh network"},
            {"hello_interval", "Hello interval", TYPE_NUMBER, "60", "60", "30", "3600", "Hello interval in the mesh network"},
            {"channel", "Channel", TYPE_NUMBER, "1", "1", "0", "14", "WiFi channel for the mesh network (0-14)"},
            {"max_ttl", "Max TTL", TYPE_NUMBER, "5", "5", "1", "9", "Max TTL (hops count for packets) in the mesh network"},
            {"sort_mode",
             "Sort mode",
             TYPE_STRING,
             "last seen",
             "last seen",
             "last seen;signal;name;role;hops;battery",
             "",
             "Sort mode for nodes list"},
        };

        SettingGroup_t export_group;
        export_group.name = "Export (SD card)";
        export_group.items = {};
        SettingGroup_t import_group;
        import_group.name = "Import (SD card)";
        import_group.items = {};

        _metadata = {wifi_group, sys_group, installer_group, flood_group, export_group, import_group};
    }

    Settings::~Settings()
    {
        if (_initialized)
        {
            _deinitNvs();
        }
    }

    bool Settings::init()
    {
        if (_initialized)
        {
            return true;
        }
        ESP_LOGW(TAG, "Settings init");

        if (!_initNvs())
        {
            return false;
        }

        _loadSettings();
        _initialized = true;
        return true;
    }

    std::vector<SettingGroup_t> Settings::getMetadata() const { return _metadata; }

    bool Settings::_initNvs()
    {
        esp_err_t err = nvs_flash_init_partition(NVS_PARTITION);
        if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
        {
            ESP_LOGW(TAG, "NVS partition was truncated or new version found, erasing...");
            err = nvs_flash_erase_partition(NVS_PARTITION);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Failed to erase NVS partition: %s", esp_err_to_name(err));
                return false;
            }
            err = nvs_flash_init_partition(NVS_PARTITION);
        }
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to initialize NVS: %s", esp_err_to_name(err));
        }
        return err == ESP_OK;
    }

    void Settings::_deinitNvs()
    {
        esp_err_t err = nvs_flash_deinit_partition(NVS_PARTITION);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Failed to deinitialize NVS: %s", esp_err_to_name(err));
        }
    }

    std::string Settings::_makeKey(const std::string& ns, const std::string& key) const
    {
        return std::format("{}-{}", ns, key);
    }

    const SettingItem_t* Settings::_findItem(const std::string& ns, const std::string& key) const
    {
        for (const auto& group : _metadata)
        {
            if (group.nvs_namespace == ns)
            {
                for (const auto& item : group.items)
                {
                    if (item.key == key)
                    {
                        return &item;
                    }
                }
            }
        }
        return nullptr;
    }

    void Settings::_loadSettings()
    {
        for (const auto& group : _metadata)
        {
            if (group.nvs_namespace.empty())
                continue;
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(NVS_PARTITION, group.nvs_namespace.c_str(), NVS_READONLY, &nvs_handle);
            if (err != ESP_OK)
            {
                ESP_LOGE(TAG, "Error opening NVS namespace %s", group.nvs_namespace.c_str());
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                CachedValue cached_value;
                cached_value.type = item.type;

                switch (item.type)
                {
                case TYPE_BOOL:
                {
                    uint8_t value;
                    if (nvs_get_u8(nvs_handle, item.key.c_str(), &value) != ESP_OK)
                    {
                        value = item.default_val == "true" ? 1 : 0;
                    }
                    cached_value.bool_val = value == 1;
                    ESP_LOGI(TAG, "Loaded bool %s = %d", cache_key.c_str(), cached_value.bool_val);
                    break;
                }
                case TYPE_NUMBER:
                {
                    int32_t value;
                    if (nvs_get_i32(nvs_handle, item.key.c_str(), &value) != ESP_OK)
                    {
                        value = std::stoi(item.default_val);
                    }
                    cached_value.num_val = value;
                    ESP_LOGI(TAG, "Loaded number %s = %ld", cache_key.c_str(), cached_value.num_val);
                    break;
                }
                case TYPE_STRING:
                {
                    size_t required_size = 0;
                    if (nvs_get_str(nvs_handle, item.key.c_str(), nullptr, &required_size) == ESP_OK)
                    {
                        std::vector<char> value(required_size);
                        if (nvs_get_str(nvs_handle, item.key.c_str(), value.data(), &required_size) == ESP_OK)
                        {
                            cached_value.str_val = std::string(value.data());
                        }
                    }
                    if (cached_value.str_val.empty())
                    {
                        cached_value.str_val = item.default_val;
                    }
                    ESP_LOGI(TAG, "Loaded string %s = %s", cache_key.c_str(), cached_value.str_val.c_str());
                    break;
                }
                default:
                    break;
                }

                _cache[cache_key] = cached_value;
            }
            nvs_close(nvs_handle);
        }
    }

    bool Settings::getBool(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_BOOL)
        {
            return it->second.bool_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? (item->default_val == "true") : false;
    }

    int32_t Settings::getNumber(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_NUMBER)
        {
            return it->second.num_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? std::stoi(item->default_val) : 0;
    }

    std::string Settings::getString(const std::string& ns, const std::string& key)
    {
        std::string cache_key = _makeKey(ns, key);
        auto it = _cache.find(cache_key);
        if (it != _cache.end() && it->second.type == TYPE_STRING)
        {
            return it->second.str_val;
        }

        const SettingItem_t* item = _findItem(ns, key);
        return item ? item->default_val : "";
    }

    bool Settings::setBool(const std::string& ns, const std::string& key, bool value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_BOOL)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_BOOL;
        cached_value.bool_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_u8(nvs_handle, key.c_str(), value ? 1 : 0);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setNumber(const std::string& ns, const std::string& key, int32_t value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_NUMBER)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_NUMBER;
        cached_value.num_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_i32(nvs_handle, key.c_str(), value);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::setString(const std::string& ns, const std::string& key, const std::string& value)
    {
        const SettingItem_t* item = _findItem(ns, key);
        if (!item || item->type != TYPE_STRING)
        {
            return false;
        }

        std::string cache_key = _makeKey(ns, key);
        CachedValue cached_value;
        cached_value.type = TYPE_STRING;
        cached_value.str_val = value;
        _cache[cache_key] = cached_value;

        nvs_handle_t nvs_handle;
        esp_err_t err = nvs_open_from_partition(NVS_PARTITION, ns.c_str(), NVS_READWRITE, &nvs_handle);
        if (err != ESP_OK)
        {
            return false;
        }

        err = nvs_set_str(nvs_handle, key.c_str(), value.c_str());
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);

        return err == ESP_OK;
    }

    bool Settings::saveAll()
    {
        bool success = true;
        for (const auto& group : _metadata)
        {
            nvs_handle_t nvs_handle;
            esp_err_t err = nvs_open_from_partition(NVS_PARTITION, group.nvs_namespace.c_str(), NVS_READWRITE, &nvs_handle);
            if (err != ESP_OK)
            {
                success = false;
                continue;
            }

            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                    continue;

                switch (item.type)
                {
                case TYPE_BOOL:
                    err = nvs_set_u8(nvs_handle, item.key.c_str(), it->second.bool_val ? 1 : 0);
                    break;
                case TYPE_NUMBER:
                    err = nvs_set_i32(nvs_handle, item.key.c_str(), it->second.num_val);
                    break;
                case TYPE_STRING:
                    err = nvs_set_str(nvs_handle, item.key.c_str(), it->second.str_val.c_str());
                    break;
                default:
                    break;
                }

                if (err != ESP_OK)
                {
                    success = false;
                }
            }

            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
        }

        return success;
    }

    bool Settings::exportToFile(const std::string& filename) const
    {
        ESP_LOGI(TAG, "Exporting settings to %s", filename.c_str());

        std::map<std::string, std::string> existing_settings;

        std::ifstream infile(filename);
        if (infile.is_open())
        {
            std::string line;
            while (std::getline(infile, line))
            {
                line.erase(0, line.find_first_not_of(" \t\n\r"));
                line.erase(line.find_last_not_of(" \t\n\r") + 1);

                if (line.empty() || line[0] == '#')
                {
                    continue;
                }

                size_t equals_pos = line.find('=');
                if (equals_pos == std::string::npos)
                {
                    ESP_LOGW(TAG, "Skipping invalid line during export pre-read: %s", line.c_str());
                    continue;
                }

                std::string cache_key = line.substr(0, equals_pos);
                std::string value_str = line.substr(equals_pos + 1);

                size_t separator_pos = cache_key.find('-');
                if (separator_pos == std::string::npos)
                {
                    ESP_LOGW(TAG,
                             "Invalid key format (missing namespace separator '-') in existing file: %s",
                             cache_key.c_str());
                    continue;
                }
                existing_settings[cache_key] = value_str;
            }
            infile.close();
            ESP_LOGI(TAG, "Read file %s. Found %d settings", filename.c_str(), existing_settings.size());
        }
        else
        {
            ESP_LOGI(TAG, "File %s does not exist, creating new", filename.c_str());
        }
        // replacing settings in map with current values
        for (const auto& group : _metadata)
        {
            for (const auto& item : group.items)
            {
                if (item.type == TYPE_NONE)
                    continue;

                std::string cache_key = _makeKey(group.nvs_namespace, item.key);
                auto it = _cache.find(cache_key);
                if (it == _cache.end())
                {
                    ESP_LOGW(TAG, "Setting %s not found in cache during export, skipping", cache_key.c_str());
                    continue;
                }

                // outfile << cache_key << "=";
                std::string str_val;
                switch (item.type)
                {
                case TYPE_BOOL:
                    str_val = (it->second.bool_val ? "true" : "false");
                    break;
                case TYPE_NUMBER:
                    str_val = std::to_string(it->second.num_val);
                    break;
                case TYPE_STRING:
                {
                    std::string escaped_str;
                    for (char c : it->second.str_val)
                    {
                        if (c == '\n')
                        {
                            escaped_str += "\\n";
                        }
                        else
                        {
                            escaped_str += c;
                        }
                    }
                    str_val = escaped_str;
                }
                break;
                default:
                    break;
                }
                // rewriting if exists
                existing_settings[cache_key] = str_val;
            }
        }
        // saving to file
        std::ofstream outfile(filename);
        if (!outfile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for writing", filename.c_str());
            return false;
        }
        for (const auto& [key, value] : existing_settings)
        {
            outfile << key << "=" << value << std::endl;
        }
        outfile.close();
        ESP_LOGI(TAG, "Settings successfully exported to %s", filename.c_str());
        return true;
    }

    bool Settings::importFromFile(const std::string& filename)
    {
        ESP_LOGI(TAG, "Importing settings from %s", filename.c_str());
        std::ifstream infile(filename);
        if (!infile.is_open())
        {
            ESP_LOGE(TAG, "Failed to open file %s for reading", filename.c_str());
            return false;
        }

        std::string line;
        bool success = false;
        int line_num = 0;

        while (std::getline(infile, line))
        {
            line_num++;
            line.erase(0, line.find_first_not_of(" \t\n\r"));
            line.erase(line.find_last_not_of(" \t\n\r") + 1);

            if (line.empty() || line[0] == '#')
                continue;

            size_t equals_pos = line.find('=');
            if (equals_pos == std::string::npos)
            {
                ESP_LOGW(TAG, "Invalid format on line %d in %s: %s", line_num, filename.c_str(), line.c_str());
                continue;
            }

            std::string cache_key = line.substr(0, equals_pos);
            std::string value_str = line.substr(equals_pos + 1);

            size_t separator_pos = cache_key.find('-');
            if (separator_pos == std::string::npos)
            {
                ESP_LOGW(TAG,
                         "Invalid key format on line %d (missing namespace separator '-'): %s",
                         line_num,
                         cache_key.c_str());
                continue;
            }
            std::string ns = cache_key.substr(0, separator_pos);
            std::string key = cache_key.substr(separator_pos + 1);

            const SettingItem_t* item = _findItem(ns, key);
            if (!item)
            {
                ESP_LOGW(TAG,
                         "Setting %s (ns=%s, key=%s) not found in metadata, skipping line %d",
                         cache_key.c_str(),
                         ns.c_str(),
                         key.c_str(),
                         line_num);
                continue;
            }

            bool import_ok = false;
            switch (item->type)
            {
            case TYPE_BOOL:
            {
                bool val = (value_str == "true");
                if (value_str != "true" && value_str != "false")
                {
                    ESP_LOGW(TAG,
                             "Invalid boolean value '%s' for %s on line %d, using default",
                             value_str.c_str(),
                             cache_key.c_str(),
                             line_num);
                    val = (item->default_val == "true");
                }
                import_ok = setBool(ns, key, val);
                break;
            }
            case TYPE_NUMBER:
            {
                int32_t val = std::stoi(value_str);
                import_ok = setNumber(ns, key, val);
                break;
            }
            case TYPE_STRING:
            {
                std::string unescaped_str;
                for (size_t i = 0; i < value_str.length(); ++i)
                {
                    if (value_str[i] == '\\' && i + 1 < value_str.length() && value_str[i + 1] == 'n')
                    {
                        unescaped_str += '\n';
                        i++; // Skip the 'n'
                    }
                    else
                    {
                        unescaped_str += value_str[i];
                    }
                }
                import_ok = setString(ns, key, unescaped_str);
                break;
            }
            case TYPE_NONE:
            default:
                break;
            }

            if (import_ok)
            {
                ESP_LOGI(TAG, "Imported setting: %s = %s", cache_key.c_str(), value_str.c_str());
                success = true;
            }
            else
            {
                ESP_LOGW(TAG, "Failed to import setting %s on line %d", cache_key.c_str(), line_num);
            }
        }

        infile.close();

        if (success)
        {
            ESP_LOGI(TAG, "Settings successfully imported from %s", filename.c_str());
        }
        else
        {
            ESP_LOGW(TAG, "No settings were imported from %s", filename.c_str());
        }

        return success;
    }

} // namespace SETTINGS
