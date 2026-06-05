# ESP32-S3 ThingsBoard Native OTA

Basic PlatformIO firmware for ESP32-S3 using Arduino C++ and the ThingsBoard native MQTT firmware OTA feature.

## Configure

Copy `include/config.example.h` to `include/config.h`, then set:

- `WIFI_SSID`
- `WIFI_PASSWORD`
- `THINGSBOARD_HOST`
- `THINGSBOARD_PORT`
- `THINGSBOARD_TOKEN`
- `FIRMWARE_TITLE`
- `FIRMWARE_VERSION`

The local `include/config.h` file is ignored by git so device tokens and Wi-Fi credentials stay out of commits.

## ThingsBoard OTA Setup

1. Create or open a device in ThingsBoard and use its access token as `THINGSBOARD_TOKEN`.
2. Create a firmware package in **OTA updates**.
3. Assign the firmware package to the device.
4. Build and upload this firmware once with the current `FIRMWARE_TITLE` and `FIRMWARE_VERSION`.
5. For a new OTA package, update `FIRMWARE_VERSION`, build the new `.bin`, upload it to ThingsBoard, then assign it to the device.

The device publishes `current_fw_title`, `current_fw_version`, and `fw_state`. It requests shared attributes for `fw_title`, `fw_version`, `fw_checksum`, `fw_checksum_algorithm`, and `fw_size`, then downloads chunks from the ThingsBoard `v2/fw` MQTT API.

## Build

```sh
pio run
```

## Upload

```sh
pio run -t upload
```

## Monitor

```sh
pio device monitor
```

