#include <Arduino.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include <Update.h>
#include <WiFi.h>
#include <mbedtls/md.h>

#if __has_include("config.h")
#include "config.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "YOUR_WIFI_SSID"
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
#endif

#ifndef THINGSBOARD_HOST
#define THINGSBOARD_HOST "demo.thingsboard.io"
#endif

#ifndef THINGSBOARD_PORT
#define THINGSBOARD_PORT 1883
#endif

#ifndef THINGSBOARD_TOKEN
#define THINGSBOARD_TOKEN "YOUR_DEVICE_ACCESS_TOKEN"
#endif

#ifndef FIRMWARE_TITLE
#define FIRMWARE_TITLE "esp32s3-thingsboard-ota"
#endif

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "1.0.0"
#endif

constexpr uint16_t mqttBufferSize = 8192;
constexpr size_t firmwareChunkSize = 4096;
constexpr uint32_t reconnectDelayMs = 5000;
constexpr uint32_t telemetryIntervalMs = 10000;
constexpr uint32_t chunkTimeoutMs = 30000;
constexpr uint8_t maxChunkRetries = 5;

const char wifiSsid[] = WIFI_SSID;
const char wifiPassword[] = WIFI_PASSWORD;
const char thingsboardHost[] = THINGSBOARD_HOST;
const uint16_t thingsboardPort = THINGSBOARD_PORT;
const char thingsboardToken[] = THINGSBOARD_TOKEN;
const char firmwareTitle[] = FIRMWARE_TITLE;
const char firmwareVersion[] = FIRMWARE_VERSION;

WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);

String targetFirmwareTitle;
String targetFirmwareVersion;
String targetFirmwareChecksum;
String targetFirmwareChecksumAlgorithm;
size_t targetFirmwareSize = 0;

bool otaInProgress = false;
bool requestNextChunkPending = false;
bool restartPending = false;
uint32_t restartAtMs = 0;
uint32_t otaRequestId = 0;
uint32_t otaChunkIndex = 0;
uint32_t chunkRetryCount = 0;
uint32_t lastChunkRequestAtMs = 0;
uint32_t lastMqttConnectAttemptMs = 0;
uint32_t lastTelemetryAtMs = 0;
size_t writtenFirmwareBytes = 0;
int lastProgressPercent = -1;

mbedtls_md_context_t checksumContext;
const mbedtls_md_info_t *checksumInfo = nullptr;
bool checksumActive = false;

String bytesToHex(const uint8_t *bytes, size_t length)
{
    static const char hex[] = "0123456789abcdef";
    String result;
    result.reserve(length * 2);

    for (size_t index = 0; index < length; index++)
    {
        result += hex[(bytes[index] >> 4) & 0x0f];
        result += hex[bytes[index] & 0x0f];
    }

    return result;
}

String normalizeChecksum(String value)
{
    value.trim();
    value.toLowerCase();
    value.replace(" ", "");
    value.replace(":", "");
    value.replace("-", "");
    return value;
}

String normalizeAlgorithm(String value)
{
    value.trim();
    value.toLowerCase();
    value.replace("-", "");
    value.replace("_", "");
    return value;
}

void publishJson(const char *topic, const JsonDocument &document)
{
    String payload;
    serializeJson(document, payload);
    mqttClient.publish(topic, payload.c_str());
}

void publishOtaState(const char *state)
{
    StaticJsonDocument<192> document;
    document["current_fw_title"] = firmwareTitle;
    document["current_fw_version"] = firmwareVersion;
    document["fw_state"] = state;
    publishJson("v1/devices/me/attributes", document);
}

void publishOtaState(const char *state, const String &error)
{
    StaticJsonDocument<320> document;
    document["current_fw_title"] = firmwareTitle;
    document["current_fw_version"] = firmwareVersion;
    document["fw_state"] = state;

    if (error.length() > 0)
    {
        document["fw_error"] = error;
    }

    publishJson("v1/devices/me/attributes", document);
}

void publishFirmwareProgress()
{
    if (targetFirmwareSize == 0)
    {
        return;
    }

    int progressPercent = static_cast<int>((writtenFirmwareBytes * 100) / targetFirmwareSize);

    if (progressPercent == lastProgressPercent || (progressPercent < 100 && progressPercent % 5 != 0))
    {
        return;
    }

    lastProgressPercent = progressPercent;

    StaticJsonDocument<128> document;
    document["fw_state"] = "DOWNLOADING";
    document["fw_progress"] = progressPercent;
    publishJson("v1/devices/me/attributes", document);
}

void publishTelemetry()
{
    StaticJsonDocument<192> document;
    document["uptime"] = millis() / 1000;
    document["free_heap"] = ESP.getFreeHeap();

    if (WiFi.status() == WL_CONNECTED)
    {
        document["rssi"] = WiFi.RSSI();
    }

    publishJson("v1/devices/me/telemetry", document);
}

String getUpdateError()
{
    return String("Update error code ") + String(Update.getError());
}

bool beginChecksum(const String &algorithm)
{
    String normalizedAlgorithm = normalizeAlgorithm(algorithm);
    mbedtls_md_type_t checksumType = MBEDTLS_MD_NONE;

    if (normalizedAlgorithm == "md5")
    {
        checksumType = MBEDTLS_MD_MD5;
    }
    else if (normalizedAlgorithm == "sha1")
    {
        checksumType = MBEDTLS_MD_SHA1;
    }
    else if (normalizedAlgorithm == "sha256")
    {
        checksumType = MBEDTLS_MD_SHA256;
    }
    else if (normalizedAlgorithm == "sha384")
    {
        checksumType = MBEDTLS_MD_SHA384;
    }
    else if (normalizedAlgorithm == "sha512")
    {
        checksumType = MBEDTLS_MD_SHA512;
    }

    checksumInfo = mbedtls_md_info_from_type(checksumType);

    if (checksumInfo == nullptr)
    {
        return false;
    }

    mbedtls_md_init(&checksumContext);

    if (mbedtls_md_setup(&checksumContext, checksumInfo, 0) != 0)
    {
        mbedtls_md_free(&checksumContext);
        return false;
    }

    if (mbedtls_md_starts(&checksumContext) != 0)
    {
        mbedtls_md_free(&checksumContext);
        return false;
    }

    checksumActive = true;
    return true;
}

void updateChecksum(const uint8_t *payload, size_t length)
{
    if (checksumActive)
    {
        mbedtls_md_update(&checksumContext, payload, length);
    }
}

String finishChecksum()
{
    if (!checksumActive)
    {
        return "";
    }

    uint8_t checksum[64] = {};
    size_t checksumLength = mbedtls_md_get_size(checksumInfo);
    mbedtls_md_finish(&checksumContext, checksum);
    mbedtls_md_free(&checksumContext);
    checksumActive = false;
    checksumInfo = nullptr;
    return bytesToHex(checksum, checksumLength);
}

void abortChecksum()
{
    if (checksumActive)
    {
        mbedtls_md_free(&checksumContext);
        checksumActive = false;
        checksumInfo = nullptr;
    }
}

void failFirmwareUpdate(const String &reason)
{
    Serial.println("OTA failed: " + reason);
    Update.abort();
    abortChecksum();

    otaInProgress = false;
    requestNextChunkPending = false;
    writtenFirmwareBytes = 0;
    publishOtaState("FAILED", reason);
}

bool parseFirmwareResponseTopic(const String &topic, uint32_t &requestId, uint32_t &chunkIndex)
{
    const String prefix = "v2/fw/response/";
    const String chunkMarker = "/chunk/";

    if (!topic.startsWith(prefix))
    {
        return false;
    }

    int chunkMarkerIndex = topic.indexOf(chunkMarker, prefix.length());

    if (chunkMarkerIndex < 0)
    {
        return false;
    }

    requestId = static_cast<uint32_t>(topic.substring(prefix.length(), chunkMarkerIndex).toInt());
    chunkIndex = static_cast<uint32_t>(topic.substring(chunkMarkerIndex + chunkMarker.length()).toInt());
    return true;
}

void requestFirmwareChunk()
{
    if (!otaInProgress || !mqttClient.connected())
    {
        return;
    }

    String topic = "v2/fw/request/" + String(otaRequestId) + "/chunk/" + String(otaChunkIndex);
    String payload = String(firmwareChunkSize);

    if (mqttClient.publish(topic.c_str(), payload.c_str()))
    {
        requestNextChunkPending = false;
        lastChunkRequestAtMs = millis();
        chunkRetryCount++;
        Serial.printf("Requested OTA chunk %lu\n", static_cast<unsigned long>(otaChunkIndex));
    }
}

void finishFirmwareUpdate()
{
    if (targetFirmwareSize > 0 && writtenFirmwareBytes != targetFirmwareSize)
    {
        failFirmwareUpdate("Received " + String(writtenFirmwareBytes) + " bytes, expected " + String(targetFirmwareSize));
        return;
    }

    String calculatedChecksum = finishChecksum();
    String expectedChecksum = normalizeChecksum(targetFirmwareChecksum);

    if (calculatedChecksum != expectedChecksum)
    {
        failFirmwareUpdate("Checksum mismatch");
        return;
    }

    publishOtaState("DOWNLOADED");

    if (!Update.end(true))
    {
        failFirmwareUpdate(getUpdateError());
        return;
    }

    publishOtaState("VERIFIED");
    publishOtaState("UPDATING");
    Serial.println("OTA update verified. Restarting...");

    otaInProgress = false;
    requestNextChunkPending = false;
    restartPending = true;
    restartAtMs = millis() + 1500;
}

void handleFirmwareChunk(const String &topic, uint8_t *payload, unsigned int length)
{
    uint32_t requestId = 0;
    uint32_t chunkIndex = 0;

    if (!parseFirmwareResponseTopic(topic, requestId, chunkIndex))
    {
        return;
    }

    if (!otaInProgress || requestId != otaRequestId || chunkIndex != otaChunkIndex)
    {
        return;
    }

    chunkRetryCount = 0;

    if (length == 0)
    {
        finishFirmwareUpdate();
        return;
    }

    size_t written = Update.write(payload, length);

    if (written != length)
    {
        failFirmwareUpdate(getUpdateError());
        return;
    }

    updateChecksum(payload, length);
    writtenFirmwareBytes += length;
    otaChunkIndex++;
    requestNextChunkPending = true;
    publishFirmwareProgress();
}

bool isConfigured()
{
    return strcmp(wifiSsid, "YOUR_WIFI_SSID") != 0 &&
           strcmp(thingsboardToken, "YOUR_DEVICE_ACCESS_TOKEN") != 0;
}

bool targetFirmwareIsCurrent()
{
    return targetFirmwareTitle == firmwareTitle && targetFirmwareVersion == firmwareVersion;
}

void startFirmwareUpdate()
{
    if (otaInProgress || targetFirmwareTitle.length() == 0 || targetFirmwareVersion.length() == 0)
    {
        return;
    }

    if (targetFirmwareIsCurrent())
    {
        publishOtaState("UPDATED");
        return;
    }

    if (targetFirmwareChecksum.length() == 0 || targetFirmwareChecksumAlgorithm.length() == 0)
    {
        publishOtaState("FAILED", "Firmware checksum or checksum algorithm is missing");
        return;
    }

    if (!beginChecksum(targetFirmwareChecksumAlgorithm))
    {
        publishOtaState("FAILED", "Unsupported checksum algorithm: " + targetFirmwareChecksumAlgorithm);
        return;
    }

    size_t updateSize = targetFirmwareSize > 0 ? targetFirmwareSize : UPDATE_SIZE_UNKNOWN;

    if (!Update.begin(updateSize))
    {
        abortChecksum();
        publishOtaState("FAILED", getUpdateError());
        return;
    }

    otaInProgress = true;
    requestNextChunkPending = true;
    otaChunkIndex = 0;
    chunkRetryCount = 0;
    writtenFirmwareBytes = 0;
    lastProgressPercent = -1;
    otaRequestId = millis();

    publishOtaState("DOWNLOADING");
    Serial.println("Starting OTA download for " + targetFirmwareTitle + " " + targetFirmwareVersion);
}

void updateTargetFirmware(JsonObject attributes)
{
    if (attributes.containsKey("fw_title"))
    {
        targetFirmwareTitle = attributes["fw_title"].as<String>();
    }

    if (attributes.containsKey("fw_version"))
    {
        targetFirmwareVersion = attributes["fw_version"].as<String>();
    }

    if (attributes.containsKey("fw_checksum"))
    {
        targetFirmwareChecksum = attributes["fw_checksum"].as<String>();
    }

    if (attributes.containsKey("fw_checksum_algorithm"))
    {
        targetFirmwareChecksumAlgorithm = attributes["fw_checksum_algorithm"].as<String>();
    }

    if (attributes.containsKey("fw_size"))
    {
        targetFirmwareSize = attributes["fw_size"].as<size_t>();
    }

    startFirmwareUpdate();
}

void handleAttributes(uint8_t *payload, unsigned int length)
{
    StaticJsonDocument<768> document;
    DeserializationError error = deserializeJson(document, payload, length);

    if (error)
    {
        Serial.println("Failed to parse attributes: " + String(error.c_str()));
        return;
    }

    JsonObject root = document.as<JsonObject>();
    JsonObject attributes = root;

    if (root.containsKey("shared"))
    {
        attributes = root["shared"].as<JsonObject>();
    }

    updateTargetFirmware(attributes);
}

void onMqttMessage(char *topic, uint8_t *payload, unsigned int length)
{
    String topicString(topic);

    if (topicString.startsWith("v2/fw/response/"))
    {
        handleFirmwareChunk(topicString, payload, length);
        return;
    }

    handleAttributes(payload, length);
}

void requestSharedFirmwareInfo()
{
    StaticJsonDocument<128> document;
    document["sharedKeys"] = "fw_title,fw_version,fw_checksum,fw_checksum_algorithm,fw_size";
    publishJson("v1/devices/me/attributes/request/1", document);
}

void subscribeThingsBoardTopics()
{
    mqttClient.subscribe("v1/devices/me/attributes");
    mqttClient.subscribe("v1/devices/me/attributes/response/+");
    mqttClient.subscribe("v2/fw/response/+/chunk/+");
}

void connectWifi()
{
    if (!isConfigured())
    {
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        return;
    }

    Serial.print("Connecting to Wi-Fi");
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid, wifiPassword);

    while (WiFi.status() != WL_CONNECTED)
    {
        Serial.print(".");
        delay(500);
    }

    Serial.println();
    Serial.println("Wi-Fi connected: " + WiFi.localIP().toString());
}

void connectMqtt()
{
    if (mqttClient.connected() || WiFi.status() != WL_CONNECTED)
    {
        return;
    }

    if (millis() - lastMqttConnectAttemptMs < reconnectDelayMs)
    {
        return;
    }

    lastMqttConnectAttemptMs = millis();
    String clientId = "esp32s3-ota-" + String(static_cast<uint32_t>(ESP.getEfuseMac()), HEX);

    Serial.print("Connecting to ThingsBoard MQTT...");

    if (mqttClient.connect(clientId.c_str(), thingsboardToken, ""))
    {
        Serial.println("connected");
        subscribeThingsBoardTopics();
        publishOtaState("UPDATED");
        requestSharedFirmwareInfo();
    }
    else
    {
        Serial.println("failed, rc=" + String(mqttClient.state()));
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1500);

    Serial.println();
    Serial.println("ESP32-S3 ThingsBoard native OTA");
    Serial.println("Firmware: " + String(firmwareTitle) + " " + String(firmwareVersion));

    if (!isConfigured())
    {
        Serial.println("Please configure include/config.h before running on hardware.");
    }

    mqttClient.setServer(thingsboardHost, thingsboardPort);
    mqttClient.setCallback(onMqttMessage);
    mqttClient.setBufferSize(mqttBufferSize);

    connectWifi();
}

void loop()
{
    connectWifi();
    connectMqtt();
    mqttClient.loop();

    if (otaInProgress && requestNextChunkPending)
    {
        requestFirmwareChunk();
    }

    if (otaInProgress && !requestNextChunkPending && millis() - lastChunkRequestAtMs > chunkTimeoutMs)
    {
        if (chunkRetryCount >= maxChunkRetries)
        {
            failFirmwareUpdate("Timed out waiting for firmware chunk " + String(otaChunkIndex));
        }
        else
        {
            requestNextChunkPending = true;
        }
    }

    if (mqttClient.connected() && !otaInProgress && millis() - lastTelemetryAtMs > telemetryIntervalMs)
    {
        lastTelemetryAtMs = millis();
        publishTelemetry();
    }

    if (restartPending && millis() >= restartAtMs)
    {
        ESP.restart();
    }
}
