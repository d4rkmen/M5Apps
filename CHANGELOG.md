Install with M5Burner!

v2.3

- exFAT filesystem support added for SD card and USB drive. Now can use huge volumes media (not limited by 32Gb)
- LED activity indicator added for system events (installation, file operations, etc)
- FLOOD: Fixed screen wake up on message received in chat view
- FLOOD: Fixed device and channel deletion issue
- FLOOD: Fixed view refresh on new channel

v2.2

- INSTALLER: New feature: direct flashing (no SD card required anymore for cloud source installation)
- INSTALLER: Fixed heap allocation during cloud source browsing
- HAL: Turn off the LED before reboot to avoid ON state after reboot
- WiFi: Fixed missing signal level update
- DISPLAY: Turn off the screen before initialization to avoid flickering

v2.1

- M5GFX dependency removed, using LovyanGFX 1.2.7 instead
- LovyanGFX driver added for ST7789V2 panel using ESP-IDF SPI master driver on full speeed 80 MHz
- I2C driver wrapper added for ESP-IDF I2C master driver
- Refactored HAL class to reflect driver changes
- ADV speaker init fixed (was no sound due to lack of ES8311 initialization)
- Speaker class fixed for small samples frames (to use with TTS etc)
- FLOOD: Fixed device deletion, when no device selected
- FLOOD: Fixed device list refresh on new device discovery
- SETTINGS: Added System - USB host setting to enable/disable USB host

v2.0

- CardPuter ADV support added
- Added new app FLOOD: advanced mesh chat with channels and history by ESP-NOW
- Added LED indicator for system event notifications (WiFi heart beat etc)
- Added screenshot option (key combination: CTRL + SPACE)
- Boot sound changed to smaller one (to free more space for other apps)
- Boot logo changed from BMP to PNG (to free even more space)
- FINDER: Improved progress during recursive deletition
- INSTALLER: Fixed HEAP corruption during cloud source browsing
- DIALOGS: Fixed ESC handling
- DIALOGS: Added maximum string size indication
- SETTINGS: Fixed error handling on import settings
- M5GFX driver updated (using new I2C master driver)

v1.9

- Added new app FINDER: two-panel file manager for SD card and USB drive
- Improved keyboard navigation in all apps
- Fixed keyboard sound issues
- INSTALLER: Added sorting files and folders by name
- INSTALLER: Cloud source fixed HTTP buffers
- FDISK: Added creation of new data partitions (press A)
- FDISK: No reboot required after deleting data partition
- Updated to the latest ESP-IDF v5.5.1
- Updated USB MSC driver

v1.8

- SETTINGS: Scroll position for selection dialogs fixed
- INSTALLER: Fixed bug with creating download directories

v1.7

- SETTINGS: Added export/import settings

v1.6

- Speed and stability improvements
- Minor UI fixes

v1.5

- Added version to boot animation
- Cloud source error messages are now more informative
- Minor UI fixes (hints, navigation, sound)

v1.4

- New soundsfor: boot, error, usb
- Minor UI changes (hints, scrolls, navigation)
- Now even more free space in flash for other apps

v1.3

- Fixed progress bar in dialogs
- Improved keyboard navigation in all apps
- FDISK: Added partition hex/ascii view mode
- FDISK: Added partition rename
- SETTINGS: Added hint for settings items
- SETTINGS: Added Installer settings to cusomize installation process
- SETTINGS: Added settings for tuning auto run on boot

v1.2

- Added WiFi scan. Now can see all available networks and select it in Settings.
  Holding Fn will open manual network name dialog.
- WiFi disable by default
- Removed unicode from fonts. Freed much space in flash for other apps.
- Repository improvements: authors and apps collections are sorted now. App name includes name and version.

v1.1

- Added Settings app. Now can edit user settings
- Added screen dim by timeout. Usefull when charging the device. (Not active in dialogs)
- Added WiFi stack, Yes, it costs ~500Kb of space, but now can use remote repository.
  Thete is only M5Burner repo only, currently. But can add custom repos later.
- Added settings to dissable auto boot of last app

Flash your M5 Cardputer using your preferred source! This app supports remote repositories, SD cards, and USB drives, all within a convenient UI. Install up to 16 applications, limited only by your Cardputer flash size, and have them ready to run at any time.
Installation: this is full firmaware image bundle, thats why it has to be flashed with M5Burner or esptool offset 0
Source code: will be available soon.
Discussion: https://community.m5stack.com/topic/7388/m5apps-multiple-apps-installer
