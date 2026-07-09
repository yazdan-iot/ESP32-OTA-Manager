/**
 * ESP32-OTA-Manager
 *
 * Complete OTA management system for ESP32
 * Combines ArduinoOTA + HTTP OTA + FreeRTOS + NVS + Rollback
 *
 * Tasks:
 *   OTA Task (Core 1, Priority 5):    ArduinoOTA.handle() in Loop
 *   HTTP Check Task (Core 1, P 1):    Check for new version every 6 hours
 *   Sensor Task (Core 0, P 2):        Simulated sensor data
 *   WiFi Monitor Task (Core 1, P 3):  Reconnect if disconnected
 *
 * Hardware: ESP32 only
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_system.h>

// ==================== Settings — everything here ====================
#define FIRMWARE_VERSION    "1.0.0"
#define DEVICE_NAME         "ota-manager-01"
#define OTA_PASSWORD        "str0ng_0ta_p4ss"
#define WIFI_SSID           "SSID"
#define WIFI_PASS           "PASSWORD"

// HTTP OTA server (for testing: python3 -m http.server 8080)
#define OTA_SERVER_HOST     "192.168.1.100"
#define OTA_SERVER_PORT     8080
#define OTA_VERSION_URL     "http://" OTA_SERVER_HOST ":" STRINGIFY(OTA_SERVER_PORT) "/version.txt"
#define OTA_FIRMWARE_URL    "http://" OTA_SERVER_HOST ":" STRINGIFY(OTA_SERVER_PORT) "/firmware.bin"

#define STRINGIFY(x) STRINGIFY2(x)
#define STRINGIFY2(x) #x

// HTTP OTA check interval (ms)
#define HTTP_OTA_CHECK_INTERVAL_MS  (6UL * 60 * 60 * 1000) // 6 hours
// For testing:
// #define HTTP_OTA_CHECK_INTERVAL_MS  (60UL * 1000) // 1 minute

// Rollback validation time
#define ROLLBACK_VALIDATION_MS  30000  // 30 seconds

// ==================== NVS Keys ====================
#define NS_OTA_MGR  "ota-mgr"
#define NS_OTA_LOG  "ota-log"
#define KEY_BOOT_CNT    "boot_cnt"
#define KEY_FW_VER      "fw_ver"
#define KEY_UPD_CNT     "upd_cnt"
#define KEY_LAST_VER    "last_ver"
#define KEY_LAST_UPD_TS "last_upd"
#define KEY_SUCCESS_CNT "succ_cnt"
#define KEY_FAIL_CNT    "fail_cnt"

// ==================== State ====================
SemaphoreHandle_t nvsMutex;
SemaphoreHandle_t wifiMutex;
volatile bool otaActive    = false;
volatile bool systemValid  = false;
uint32_t      bootCount    = 0;
uint32_t      updateCount  = 0;

// ==================== NVS Helpers ====================

uint32_t incrementNVSCounter(const char* ns, const char* key) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return 0;
    
    Preferences p;
    p.begin(ns, false);
    uint32_t val = p.getUInt(key, 0) + 1;
    p.putUInt(key, val);
    p.end();
    
    xSemaphoreGive(nvsMutex);
    return val;
}

void logUpdateResult(bool success, const char* fromVer, const char* toVer) {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    
    Preferences p;
    p.begin(NS_OTA_LOG, false);
    p.putString(KEY_LAST_VER, fromVer);
    p.putString(KEY_FW_VER, toVer);
    p.putULong(KEY_LAST_UPD_TS, millis());
    
    const char* cntKey = success ? KEY_SUCCESS_CNT : KEY_FAIL_CNT;
    p.putUInt(cntKey, p.getUInt(cntKey, 0) + 1);
    
    p.end();
    xSemaphoreGive(nvsMutex);
}

void printNVSReport() {
    if (xSemaphoreTake(nvsMutex, pdMS_TO_TICKS(1000)) != pdTRUE) return;
    
    Preferences p;
    p.begin(NS_OTA_MGR, true);
    uint32_t boots = p.getUInt(KEY_BOOT_CNT, 0);
    String   ver   = p.getString(KEY_FW_VER, "?");
    uint32_t upds  = p.getUInt(KEY_UPD_CNT, 0);
    p.end();
    
    p.begin(NS_OTA_LOG, true);
    uint32_t succ = p.getUInt(KEY_SUCCESS_CNT, 0);
    uint32_t fail = p.getUInt(KEY_FAIL_CNT, 0);
    String   lastV = p.getString(KEY_LAST_VER, "none");
    p.end();
    
    xSemaphoreGive(nvsMutex);
    
    Serial.println("\n╔══════════════════════════════════════╗");
    Serial.println("║         OTA Manager Report            ║");
    Serial.println("╠══════════════════════════════════════╣");
    Serial.printf( "║ Device:    %-27s║\n", DEVICE_NAME);
    Serial.printf( "║ Firmware:  %-27s║\n", FIRMWARE_VERSION);
    Serial.printf( "║ Boot Count:%-27u║\n", boots);
    Serial.printf( "║ Updates:   %-27u║\n", upds);
    Serial.printf( "║ Successes: %-27u║\n", succ);
    Serial.printf( "║ Failures:  %-27u║\n", fail);
    Serial.printf( "║ Prev. Ver: %-27s║\n", lastV.c_str());
    Serial.println("╠══════════════════════════════════════╣");
    Serial.printf( "║ Free Heap: %-22d bytes║\n", ESP.getFreeHeap());
    Serial.printf( "║ WiFi RSSI: %-22d dBm║\n", WiFi.RSSI());
    Serial.println("╚══════════════════════════════════════╝\n");
}

// ==================== OTA Rollback ====================

void validateFirmware() {
    const esp_partition_t* running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    
    if (state == ESP_OTA_IMG_PENDING_VERIFY) {
        Serial.println("[ROLLBACK] ⚠️  New firmware — validating...");
        
        // Wait for the system to stabilize
        vTaskDelay(pdMS_TO_TICKS(ROLLBACK_VALIDATION_MS));
        
        // Health checks
        bool healthy = (WiFi.status() == WL_CONNECTED) && (ESP.getFreeHeap() > 10000);
        
        if (healthy) {
            esp_ota_mark_app_valid_cancel_rollback();
            Serial.println("[ROLLBACK] ✅ Firmware validated!");
            systemValid = true;
        } else {
            Serial.println("[ROLLBACK] ❌ Validation failed! Rolling back...");
            logUpdateResult(false, "new", FIRMWARE_VERSION);
            delay(500);
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
    } else {
        systemValid = true;
    }
}

// ==================== HTTP OTA Check ====================

bool checkAndApplyHTTPUpdate() {
    if (WiFi.status() != WL_CONNECTED) return false;
    
    // Check version
    HTTPClient http;
    http.begin(OTA_VERSION_URL);
    http.setTimeout(5000);
    
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        Serial.printf("[HTTP-OTA] Version server error: %d\n", code);
        http.end();
        return false;
    }
    
    String latestVer = http.getString();
    latestVer.trim();
    http.end();
    
    Serial.printf("[HTTP-OTA] Current: %s | Server: %s\n",
                  FIRMWARE_VERSION, latestVer.c_str());
    
    if (latestVer == FIRMWARE_VERSION) {
        Serial.println("[HTTP-OTA] Already up to date.");
        return false;
    }
    
    // Start update
    Serial.printf("[HTTP-OTA] 🆕 Updating %s → %s\n", FIRMWARE_VERSION, latestVer.c_str());
    
    WiFiClient client;
    
    httpUpdate.onProgress([](int cur, int total) {
        static int lastPct = -1;
        int pct = (cur * 100) / total;
        if (pct != lastPct && pct % 10 == 0) {
            lastPct = pct;
            Serial.printf("[HTTP-OTA] %d%%\r", pct);
        }
    });
    
    httpUpdate.rebootOnUpdate(false); // Restart manually
    
    t_httpUpdate_return ret = httpUpdate.update(client, OTA_FIRMWARE_URL);
    
    if (ret == HTTP_UPDATE_OK) {
        logUpdateResult(true, FIRMWARE_VERSION, latestVer.c_str());
        incrementNVSCounter(NS_OTA_MGR, KEY_UPD_CNT);
        
        Serial.println("\n[HTTP-OTA] ✅ Update done! Rebooting...");
        delay(500);
        ESP.restart();
        return true;
    }
    
    Serial.printf("[HTTP-OTA] ❌ Failed: %s\n", httpUpdate.getLastErrorString().c_str());
    logUpdateResult(false, FIRMWARE_VERSION, latestVer.c_str());
    return false;
}

// ==================== Tasks ====================

// OTA Task: ArduinoOTA.handle() in Loop
void otaTask(void* pvParams) {
    Serial.printf("[OTA Task] Running on Core %d\n", xPortGetCoreID());
    
    while (true) {
        if (WiFi.status() == WL_CONNECTED && !otaActive) {
            ArduinoOTA.handle();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// HTTP Check Task: check every N hours
void httpCheckTask(void* pvParams) {
    // Wait a bit first for the system to stabilize
    vTaskDelay(pdMS_TO_TICKS(60000)); // Wait 1 minute
    
    while (true) {
        Serial.println("[HTTP-OTA] Periodic check starting...");
        checkAndApplyHTTPUpdate();
        
        vTaskDelay(pdMS_TO_TICKS(HTTP_OTA_CHECK_INTERVAL_MS));
    }
}

// Sensor Task: main system work
void sensorTask(void* pvParams) {
    while (true) {
        // In a real project: read the actual sensor, send to MQTT
        float temp = 22.0f + (random(-20, 50) / 10.0f);
        float hum  = 55.0f + (random(-100, 100) / 10.0f);
        
        Serial.printf("[SENSOR] Temp: %.1f°C | Humidity: %.1f%% | WiFi: %s\n",
                      temp, hum,
                      WiFi.status() == WL_CONNECTED ? "✅" : "❌");
        
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// WiFi Monitor Task: reconnect
void wifiMonitorTask(void* pvParams) {
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected! Reconnecting...");
            WiFi.disconnect();
            WiFi.begin(WIFI_SSID, WIFI_PASS);
            
            int attempts = 0;
            while (WiFi.status() != WL_CONNECTED && attempts < 20) {
                vTaskDelay(pdMS_TO_TICKS(500));
                attempts++;
            }
            
            if (WiFi.status() == WL_CONNECTED) {
                Serial.printf("[WiFi] Reconnected! IP: %s\n",
                              WiFi.localIP().toString().c_str());
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

// ==================== setup ====================

void setup() {
    Serial.begin(115200);
    delay(1000);
    
    // Create mutexes
    nvsMutex  = xSemaphoreCreateMutex();
    wifiMutex = xSemaphoreCreateMutex();
    
    // Boot Counter
    bootCount = incrementNVSCounter(NS_OTA_MGR, KEY_BOOT_CNT);
    
    Serial.printf("\n╔════════════════════════════════════╗\n");
    Serial.printf("║  %s                     ║\n", DEVICE_NAME);
    Serial.printf("║  Firmware: %-23s║\n", FIRMWARE_VERSION);
    Serial.printf("║  Boot #:   %-23u║\n", bootCount);
    Serial.printf("╚════════════════════════════════════╝\n\n");
    
    // WiFi
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    
    Serial.print("[WiFi] Connecting");
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
        delay(500); Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] ✅ Connected | IP: %s | RSSI: %d dBm\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    } else {
        Serial.println("\n[WiFi] ❌ Connection failed! Continuing offline...");
    }
    
    // ArduinoOTA Setup
    ArduinoOTA.setHostname(DEVICE_NAME);
    ArduinoOTA.setPassword(OTA_PASSWORD);
    
    ArduinoOTA.onStart([]() {
        String type = ArduinoOTA.getCommand() == U_FLASH ? "firmware" : "filesystem";
        Serial.printf("\n[ArduinoOTA] 🚀 Start: %s\n", type.c_str());
        otaActive = true;
    });
    
    ArduinoOTA.onEnd([]() {
        Serial.println("[ArduinoOTA] ✅ Done!");
        otaActive = false;
    });
    
    ArduinoOTA.onProgress([](unsigned int p, unsigned int t) {
        static uint8_t last = 255;
        uint8_t pct = p / (t / 100);
        if (pct != last && pct % 5 == 0) { last = pct; Serial.printf("[ArduinoOTA] %u%%\r", pct); }
    });
    
    ArduinoOTA.onError([](ota_error_t e) {
        Serial.printf("\n[ArduinoOTA] ❌ Error[%u]\n", e);
        otaActive = false;
    });
    
    ArduinoOTA.begin();
    Serial.printf("[ArduinoOTA] Ready at %s.local (pass: set)\n\n", DEVICE_NAME);
    
    // NVS Report
    printNVSReport();
    
    // Tasks
    xTaskCreatePinnedToCore(otaTask,         "OTA",       8192, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(httpCheckTask,   "HTTPCheck", 8192, NULL, 1, NULL, 1);
    xTaskCreatePinnedToCore(sensorTask,      "Sensor",    4096, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(wifiMonitorTask, "WiFiMon",   4096, NULL, 3, NULL, 1);
    
    // Rollback validation in this same task (setup)
    validateFirmware();
    
    Serial.println("[SETUP] All tasks created. System running.\n");
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(1000));
}