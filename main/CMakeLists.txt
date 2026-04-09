file(GLOB_RECURSE APPS_SRCS
    apps/*.c
    apps/*.cpp
)

file(GLOB_RECURSE HAL_SRCS
    hal/*.c
    hal/*.cpp
)

file(GLOB_RECURSE SETTINGS_SRCS
    settings/*.c
    settings/*.cpp
)

idf_component_register(SRCS "main.cpp" ${APPS_SRCS} ${HAL_SRCS} ${SETTINGS_SRCS}
                    INCLUDE_DIRS "." "hal"
                    REQUIRES LovyanGFX mooncake spi_flash bootloader_support app_update usb usb_host_msc fatfs esp_wifi esp_http_client mjson flood esp_adc driver
                    WHOLE_ARCHIVE
                    EMBED_FILES sound/usb_connected.wav sound/usb_disconnected.wav sound/error.wav sound/boot_sound.wav sound/clock.wav image/boot_logo.png apps/launcher/views/system_bar/assets/time.png
                    )
                    

nvs_create_partition_image(apps_nvs ../apps_nvs.csv FLASH_IN_PROJECT)