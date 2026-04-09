/**
 * @file cardputer.cpp
 * @author d4rkmen
 * @brief
 * @version 1.9
 * @date 2025-10-28
 *
 * @copyright Copyright (c) 2025
 *
 */
#include <stdio.h>
#include "mooncake.h"
#include "hal/hal_cardputer.h"
#include "settings/settings.h"
#include "apps/apps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "apps/utils/flash/flash_tools.h"
#include "flood.h"

static const char* TAG = "MAIN";

using namespace HAL;
using namespace SETTINGS;
using namespace MOONCAKE;

Settings settings;
HalCardputer hal(&settings);
Mooncake mooncake;
bool system_bar_force_update = false;

void _data_base_setup_callback(SIMPLEKV::SimpleKV& db)
{
    db.Add<HAL::Hal*>("HAL", &hal);
    db.Add<SETTINGS::Settings*>("SETTINGS", &settings);
    db.Add<bool*>("SYSTEM_BAR_FORCE_UPDATE", &system_bar_force_update);
}

// Create apps for all bootable OTA partitions
void install_ota_apps(Mooncake& mc)
{
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);

    while (it != NULL)
    {
        const esp_partition_t* partition = esp_partition_get(it);

        // Check if this is an OTA partition
        if (partition->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN && partition->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MAX)
        {
            // Check if partition is bootable
            if (UTILS::FLASH_TOOLS::is_partition_bootable(partition))
            {
                ESP_LOGI(TAG, "Found bootable OTA partition: %s at 0x%lx", partition->label, partition->address);
                // Create and install app packer
                auto packer = new APPS::OtaApp_Packer(partition);
                mc.installApp(packer);
            }
        }

        it = esp_partition_next(it);
    }

    if (it)
    {
        esp_partition_iterator_release(it);
    }
}

extern "C" void app_main(void)
{
    // Settings init
    settings.init();

    // Init hal
    hal.init();

    // Connect settings to HAL for callbacks
    settings.setHal(&hal);

    // Apply timezone from settings
    std::string tz = settings.getString("system", "timezone");
    if (!tz.empty())
        Settings::applyTimezone(tz);

    // Init framework
    mooncake.setDatabaseSetupCallback(_data_base_setup_callback);
    mooncake.init();

    // Install launcher
    auto launcher = new APPS::Launcher_Packer;
    mooncake.installApp(launcher);

    // Install system apps
    mooncake.installApp(new APPS::AppSettings_Packer);
    mooncake.installApp(new APPS::AppInstaller_Packer);
    mooncake.installApp(new APPS::AppFdisk_Packer);
    mooncake.installApp(new APPS::AppFinder_Packer);
    mooncake.installApp(new APPS::AppFlood_Packer);

    // Install OTA apps
    install_ota_apps(mooncake);
    // Create launcher
    mooncake.createApp(launcher);

    // Update framework
    while (1)
    {
        if (hal.isDisplaySleeping())
        {
            hal.keyboard()->updateKeyList();
            delay(50);
        }
        else
        {
            // Update UI framework
            mooncake.update();
            delay(1);
        }
    }
}
