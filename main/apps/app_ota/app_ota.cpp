#include "app_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

static const char* TAG = "APP_OTA";

using namespace MOONCAKE::APPS;
using namespace UTILS::FLASH_TOOLS;

void OtaApp::onCreate()
{
    // Get hal
    _data.hal = mcAppGetDatabase()->Get("HAL")->value<HAL::Hal*>();
    ANIM_APP_OPEN();

    ESP_LOGI(TAG, "Setting boot partition to %s at 0x%lx", _partition->label, _partition->address);

    if (esp_ota_set_boot_partition(_partition) == ESP_OK)
    {
        ESP_LOGI(TAG, "Boot partition set successfully, restarting...");
        _data.hal->reboot();
    }
    else
    {
        ESP_LOGE(TAG, "Failed to set boot partition");
        destroyApp();
    }
}
