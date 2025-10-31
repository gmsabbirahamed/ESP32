
#define TINY_GSM_MODEM_SIM7600

#include <Arduino.h>
#include <TinyGsmClient.h>
#include <PubSubClient.h>
#include <Update.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>

#include <FastLED.h>
#define NUM_LEDS 1
#define DATA_PIN 4
CRGB leds[NUM_LEDS];

// ==================== Configuration ====================

// Hardware pins
#define SIM_RX 16
#define SIM_TX 17
#define GSM_PWRKEY_PIN 15
#define STATUS_LED_PIN 2
#define ANALOG_PIN 34

// Timings
#define SENSOR_INTERVAL_MS 300000    // 5 minutes (not used, mainTask uses 1 min in your sample — change if needed)
#define WDT_TIMEOUT_MS 120000        // 2 minutes
#define RECONNECT_INTERVAL_MS 60000  // 1 minute

// Task Priorities (higher number = higher priority)
#define NETWORK_TASK_PRIORITY 3
#define MAIN_TASK_PRIORITY 2

// Stack Sizes (bytes)
#define NETWORK_TASK_STACK_SIZE (10 * 1024)
#define MAIN_TASK_STACK_SIZE    (8 * 1024)
#define OTA_TASK_STACK_SIZE     (16 * 1024)

// Queue sizes
#define SENSOR_QUEUE_SIZE 5

// GSM / GPRS
const char apn[] = "internet";
const char gprsUser[] = "";
const char gprsPass[] = "";

// MQTT
const char* mqttBroker = "broker2.com"; //change
const int mqttPort = 1883;
const char* subTopic = "sabbir/device/sub";
const char* pubTopic = "sabbir/device/pub";
const char* heartbeatTopic = "sabbir/device/heartbeat";
const char* otaCommandTopic = "sabbir/device/ota_cmd"; // listens here for ota command (payload contains "ota" or JSON)

// OTA server (default) - used when OTA command doesn't supply a URL
const char* otaHostDefault = "iot.dma-bd.com";//change
const int otaPortDefault = 5000;
const char* otaPathDefault = "/download/MeshAC.bin";//change

// ==================== Data Structures ====================
struct SensorData {
    float temperature;
    int analog;
};

// ==================== Global Objects ====================
TinyGsm modem(Serial1);
TinyGsmClient gsmClient(modem);
PubSubClient mqtt(gsmClient);

bool firstBoot = true;
bool currentConnectionStatus = false;

// FreeRTOS objects
QueueHandle_t sensorQueue = NULL;
SemaphoreHandle_t networkMutex = NULL;
TaskHandle_t networkTaskHandle = NULL;
TaskHandle_t mainTaskHandle = NULL;
TaskHandle_t otaTaskHandle = NULL;

// OTA control
volatile bool otaRequested = false;
String otaHost = otaHostDefault;
int otaPort = otaPortDefault;
String otaPath = otaPathDefault;
volatile bool otaInProgress = false;

// Helper: feed WDT in tasks where needed (network & main). We'll add tasks to WDT.
void safeFeedWDT() {
    esp_task_wdt_reset();
}

// ==================== Helper Functions ====================

void initializeHardware() {
    Serial.begin(115200);
    delay(50);
    Serial1.begin(115200, SERIAL_8N1, SIM_RX, SIM_TX);

    pinMode(GSM_PWRKEY_PIN, OUTPUT);
    digitalWrite(GSM_PWRKEY_PIN, HIGH); // inactive high by your original code

    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);

    analogReadResolution(12);
}

void powerCycleModem() {
    Serial.println("[MODEM] powerCycleModem()");
    digitalWrite(GSM_PWRKEY_PIN, LOW);
    vTaskDelay(pdMS_TO_TICKS(1200)); // press
    digitalWrite(GSM_PWRKEY_PIN, HIGH);
    // wait a little for modem to come up
    vTaskDelay(pdMS_TO_TICKS(1500));
    // test AT
    int waitTime = 0;
    while (!modem.testAT() && waitTime < 15000) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waitTime += 500;
    }
}

bool initializeModem() {
    powerCycleModem();

    if (!modem.restart()) {
        Serial.println("[MODEM] restart failed");
        return false;
    }

    Serial.print("[MODEM] Info: ");
    Serial.println(modem.getModemInfo());
    return true;
}

bool connectToNetwork() {
    Serial.println("[NET] Connecting to network...");
    if (!modem.waitForNetwork()) {
        Serial.println("[NET] waitForNetwork failed");
        return false;
    }
    if (!modem.gprsConnect(apn, gprsUser, gprsPass)) {
        Serial.println("[NET] gprsConnect failed");
        return false;
    }
    Serial.printf("[NET] GPRS connected, IP: %s\n", modem.getLocalIP().c_str());
    return true;
}

bool publishData(const SensorData& data) {
    char payload[140];
    snprintf(payload, sizeof(payload), "{\"temp\":%.2f,\"a\":%d}", data.temperature, data.analog);
    if (mqtt.publish(pubTopic, payload)) {
        Serial.printf("[MQTT] Published: %s\n", payload);
        return true;
    }
    Serial.printf("[MQTT] Publish failed: %s\n", payload);
    return false;
}

void publishHeartbeat() {
    char payload[128];
    snprintf(payload, sizeof(payload),
             "{\"heap\":%u,\"uptime_min\":%lu}", (unsigned)ESP.getFreeHeap(), millis() / 60000);
    mqtt.publish(heartbeatTopic, payload);
    Serial.printf("[MQTT] Heartbeat: %s\n", payload);
}

SensorData readSensors() {
    SensorData d;
    d.temperature = 55;
    d.analog = analogRead(ANALOG_PIN);
    return d;
}

// Allow MQTT to receive "ota" messages or JSON with "host","port","path"
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message;
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    Serial.printf("[MQTT] Rx on %s: %s\n", topic, message.c_str());

    // If topic is otaCommandTopic or message contains "ota", prepare OTA
    String t(topic);
    t.toLowerCase();
    String m = message;
    m.toLowerCase();

    if (t == String(otaCommandTopic) || m.indexOf("ota") >= 0) {
        // Optional: parse payload for host/path/port (very minimal parsing)
        // Expect payload like: ota;http://host:port/path or {"host":"x","port":5000,"path":"/firm.bin"}
        // We'll do a simple check for "http://" or a JSON parse fallback.
        if (m.indexOf("http://") >= 0 || m.indexOf("https://") >= 0) {
            // Very basic parse: extract host, optional :port, and path
            int start = m.indexOf("http://");
            if (start < 0) start = m.indexOf("https://");
            String url = message.substring(start);
            // remove protocol
            if (url.startsWith("http://")) url = url.substring(7);
            else if (url.startsWith("https://")) url = url.substring(8);
            int slash = url.indexOf('/');
            String hostport = (slash >= 0) ? url.substring(0, slash) : url;
            String path = (slash >= 0) ? url.substring(slash) : "/";
            int colon = hostport.indexOf(':');
            if (colon >= 0) {
                otaHost = hostport.substring(0, colon);
                otaPort = hostport.substring(colon + 1).toInt();
            } else {
                otaHost = hostport;
                otaPort = 80;
            }
            otaPath = path;
        } else {
            // Minimal JSON-ish parse: look for "path":"...". Not robust — adjust per your payload format.
            int pIndex = message.indexOf("path");
            if (pIndex >= 0) {
                int quote1 = message.indexOf('"', pIndex);
                int quote2 = message.indexOf('"', quote1 + 1);
                int quote3 = message.indexOf('"', quote2 + 1);
                int quote4 = message.indexOf('"', quote3 + 1);
                if (quote3 >= 0 && quote4 > quote3) {
                    otaPath = message.substring(quote3 + 1, quote4);
                }
            }
            // if no host provided, use defaults
            otaHost = otaHostDefault;
            otaPort = otaPortDefault;
        }

        Serial.printf("[OTA] Request queued: host=%s port=%d path=%s\n", otaHost.c_str(), otaPort, otaPath.c_str());
        otaRequested = true;
    }

    // Example: simple LED control
    if (message.indexOf("on") >= 0) digitalWrite(STATUS_LED_PIN, HIGH);
    else if (message.indexOf("off") >= 0) digitalWrite(STATUS_LED_PIN, LOW);
}

// ==================== FreeRTOS Tasks ====================

void reconnectMqtt() {
    Serial.println("[MQTT] Attempting connection...");
    String clientId = "GMS12Client-" + String(millis());
    if (mqtt.connect(clientId.c_str())) {
        Serial.println("[MQTT] Connected");
        mqtt.subscribe(subTopic);
        mqtt.subscribe(otaCommandTopic);
        Serial.printf("[MQTT] Subscribed: %s and %s\n", subTopic, otaCommandTopic);
        if (firstBoot) {
            mqtt.publish(pubTopic, "initialized");
            firstBoot = false;
        }
    } else {
        Serial.printf("[MQTT] Connect failed, rc=%d\n", mqtt.state());
    }
}

// networkTask handles GPRS/MQTT, processes queue, publishes data.
void networkTask(void *pvParameters) {
    Serial.println("[NET] networkTask started");

    uint8_t connectionRetries = 0;
    const uint8_t MAX_CONNECTION_RETRIES = 5;
    const unsigned long CONNECTION_RETRY_DELAY = 30000UL;  // 30 seconds between retries

    // Initialize modem & GPRS
    if (!initializeModem()) {
        Serial.println("[NET] Modem init failed -> restart");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    if (!connectToNetwork()) {
        Serial.println("[NET] connectToNetwork failed -> restart");
        vTaskDelay(pdMS_TO_TICKS(1000));
        ESP.restart();
    }

    mqtt.setServer(mqttBroker, mqttPort);
    mqtt.setCallback(mqttCallback);
    mqtt.setBufferSize(2048);

    unsigned long lastHeartbeat = millis();
    unsigned long lastReconnectAttempt = 0;

    // Register WDT for this task
    esp_task_wdt_add(NULL); // add current running task to wdt

    for (;;) {
        safeFeedWDT();

        // If OTA requested, start shutdown & create otaTask
        if (otaRequested && !otaInProgress) {
            otaInProgress = true;
            Serial.println("[NET] OTA requested - preparing for OTA");

            leds[0] = CRGB::White;
            FastLED.show();
            delay(50);

            // Publish ack
            mqtt.publish(pubTopic, "ota_starting");

            // Stop MQTT gracefully
            mqtt.disconnect();

            // Stop GPRS data or keep modem powered for OTA (we need client)
            // We'll keep modem powering, and reuse gsmClient in otaTask

            // Remove WDT registrations for tasks we will delete
            if (mainTaskHandle) {
                esp_task_wdt_delete(mainTaskHandle);
            }
            // delete mainTask
            if (mainTaskHandle) {
                vTaskDelete(mainTaskHandle);
                mainTaskHandle = NULL;
                Serial.println("[NET] mainTask deleted");
            }

            // Create otaTask pinned to core 1 (or 0) - give it a larger stack
            BaseType_t created = xTaskCreatePinnedToCore(
                [](void*)->void {
                    // stub (real function defined below). This lambda placeholder shouldn't be used.
                    // We'll never call this lambda; create below with otaTask function pointer.
                    vTaskDelete(NULL);
                },
                "otaTask", OTA_TASK_STACK_SIZE, NULL, NETWORK_TASK_PRIORITY, &otaTaskHandle, 0
            );
            // The above lambda is just a placeholder to reserve handle; we'll delete it and create proper one
            if (created == pdPASS) {
                vTaskDelete(otaTaskHandle); // remove placeholder
                otaTaskHandle = NULL;
            }

            // Proper creation using otaTask function below
            extern void otaTask(void* pv);
            if (xTaskCreatePinnedToCore(otaTask, "otaTask", OTA_TASK_STACK_SIZE, NULL, NETWORK_TASK_PRIORITY, &otaTaskHandle, 1) == pdPASS) {
                Serial.println("[NET] otaTask created");
            } else {
                Serial.println("[NET] otaTask creation FAILED");
                // Try to recover: reboot
                ESP.restart();
            }

            // Remove networkTask from WDT and delete self (networkTask) to let otaTask take over
            esp_task_wdt_delete(NULL);
            Serial.println("[NET] networkTask self-deleting to hand control to otaTask");
            vTaskDelete(NULL); // delete network task (this function returns no further)
        }

        // Network connection management
        if (!modem.isGprsConnected()) {
            if (!connectToNetwork()) {
                // If connection fails, try resetting modem
                if (connectionRetries >= MAX_CONNECTION_RETRIES) {
                    Serial.println("Max connection retries reached. Resetting modem...");
                    initializeModem();
                    connectionRetries = 0;
                } else {
                    Serial.printf("Retry %d/%d in %ds...\n", 
                                connectionRetries + 1, 
                                MAX_CONNECTION_RETRIES,
                                CONNECTION_RETRY_DELAY/1000);
                    
                    // Non-blocking wait with WDT feed
                    uint32_t retryStart = millis();
                    while (millis() - retryStart < CONNECTION_RETRY_DELAY) {
                        esp_task_wdt_reset();
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                    
                    connectionRetries++;
                }
                continue;  // Skip rest until connected
            }
            connectionRetries = 0;  // Reset on successful connection
        }

        // Ensure MQTT connected
        if (modem.isGprsConnected() && !mqtt.connected()) {

            leds[0] = CRGB::Pink;
            FastLED.show();
            delay(50);

            unsigned long now = millis();
            if (now - lastReconnectAttempt > 5000) {
                lastReconnectAttempt = now;
                reconnectMqtt();
            }
        }


        // Publish items from queue if connected
        if (mqtt.connected()) {
            mqtt.loop();
    
            leds[0] = CRGB::Black;
            FastLED.show();
            delay(50);
            SensorData data;
            if (xQueueReceive(sensorQueue, &data, pdMS_TO_TICKS(100)) == pdPASS) {
                publishData(data);
            }
        }

        // Periodic heartbeat
        if (millis() - lastHeartbeat > 60000) {
            lastHeartbeat = millis();
            if (mqtt.connected()) publishHeartbeat();
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    } // end loop
}

// mainTask reads sensors and enqueues to sensorQueue periodically
void mainTask(void* pvParameters) {
    Serial.println("[MAIN] mainTask started");
    esp_task_wdt_add(NULL);

    const unsigned long interval = 60000; // 1 minute
    unsigned long previousMillis = 0;

    for (;;) {
        esp_task_wdt_reset();
        SensorData d = readSensors();

        if (millis() - previousMillis >= interval) {
            if (xQueueSend(sensorQueue, &d, pdMS_TO_TICKS(100)) == pdPASS) {
                Serial.println("[MAIN] Data enqueued");
            } else {
                Serial.println("[MAIN] Queue full - dropping oldest");
                SensorData tmp;
                if (xQueueReceive(sensorQueue, &tmp, 0) == pdPASS) {
                    Serial.println("[MAIN] Discarded oldest");
                    if (xQueueSend(sensorQueue, &d, pdMS_TO_TICKS(100)) == pdPASS) {
                        Serial.println("[MAIN] New data enqueued after drop");
                    } else {
                        Serial.println("[MAIN] Still failed to enqueue");
                    }
                } else {
                    Serial.println("[MAIN] Failed to discard oldest (unexpected)");
                }
            }
            previousMillis = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// OTA task: uses gsmClient (TinyGsmClient) to make HTTP GET and Update.flash
void otaTask(void* pvParameters) {
    Serial.println("[OTA] otaTask started");
    // We will not register otaTask to WDT to avoid unwanted resets during long flash.
    // If WDT enabled globally, ensure it won't kill this task:
    // (We deliberately don't call esp_task_wdt_add for this task.)

    // Ensure modem & GPRS connected (if networkTask gracefully kept modem up, this should be fine)
    if (!modem.isGprsConnected()) {
        Serial.println("[OTA] GPRS not connected, trying to connect...");
        if (!connectToNetwork()) {
            Serial.println("[OTA] Failed to connect to network - aborting OTA");
            mqtt.publish(pubTopic, "ota_failed_connect");
            otaRequested = false;
            otaInProgress = false;
            vTaskDelay(pdMS_TO_TICKS(2000));
            ESP.restart();
        }
    }

    // Connect to OTA server
    Serial.printf("[OTA] Connecting to %s:%d\n", otaHost.c_str(), otaPort);
    if (!gsmClient.connect(otaHost.c_str(), otaPort)) {
        Serial.println("[OTA] gsmClient.connect failed!");
        // attempt to report via serial and reboot
        mqtt.publish(pubTopic, "ota_connect_failed");
        otaRequested = false;
        otaInProgress = false;
        vTaskDelay(pdMS_TO_TICKS(2000));
        ESP.restart();
    }

    // Send HTTP GET
    String getReq = String("GET ") + otaPath + " HTTP/1.1\r\n" +
                    "Host: " + otaHost + "\r\n" +
                    "Connection: close\r\n\r\n";
    gsmClient.print(getReq);
    Serial.println("[OTA] HTTP GET sent, waiting for response...");

    // wait for headers
    unsigned long start = millis();
    while (!gsmClient.available() && (millis() - start) < 10000) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (!gsmClient.available()) {
        Serial.println("[OTA] No response from server");
        mqtt.publish(pubTopic, "ota_no_response");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    // Parse HTTP response headers
    int contentLength = -1;
    bool isChunked = false;
    bool httpOk = false;
    while (gsmClient.available()) {
        String line = gsmClient.readStringUntil('\n');
        line.trim();
        if (line.length() == 0) break; // end headers
        line.toLowerCase();
        if (line.startsWith("http/1.1 200") || line.startsWith("http/1.0 200")) httpOk = true;
        if (line.startsWith("content-length:")) {
            contentLength = line.substring(15).toInt();
            contentLength = atoi(line.substring(15).c_str());
        }
        if (line.startsWith("transfer-encoding:") && line.indexOf("chunked") >= 0) {
            isChunked = true;
        }
    }

    if (!httpOk) {
        Serial.println("[OTA] HTTP not OK");
        mqtt.publish(pubTopic, "ota_http_not_ok");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    if (isChunked) {
        Serial.println("[OTA] Chunked transfer- not supported by this simple OTA (abort).");
        mqtt.publish(pubTopic, "ota_chunked_not_supported");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    if (contentLength <= 0) {
        Serial.println("[OTA] Invalid content length");
        mqtt.publish(pubTopic, "ota_invalid_length");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    Serial.printf("[OTA] Content-Length: %d\n", contentLength);

    // Start Update
    if (!Update.begin(contentLength)) {
        Serial.printf("[OTA] Update.begin failed: %s\n", Update.errorString());
        mqtt.publish(pubTopic, "ota_update_begin_failed");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    // Read body and write to flash
    uint8_t buf[1024];
    int totalRead = 0;
    while (totalRead < contentLength) {
        int toRead = sizeof(buf);
        if (contentLength - totalRead < toRead) toRead = contentLength - totalRead;
        int r = gsmClient.readBytes(buf, toRead);
        if (r <= 0) {
            // wait a bit for more data (but not forever)
            int waitCount = 0;
            while (gsmClient.available() == 0 && waitCount++ < 50) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }
            if (gsmClient.available() == 0) {
                Serial.println("[OTA] Read timeout");
                Update.abort();
                mqtt.publish(pubTopic, "ota_read_timeout");
                otaRequested = false;
                otaInProgress = false;
                gsmClient.stop();
                ESP.restart();
            } else continue;
        }
        size_t written = Update.write(buf, r);
        if (written != (size_t)r) {
            Serial.printf("[OTA] Write mismatch: wrote %u expected %d\n", (unsigned)written, r);
            Update.abort();
            mqtt.publish(pubTopic, "ota_write_error");
            otaRequested = false;
            otaInProgress = false;
            gsmClient.stop();
            ESP.restart();
        }
        totalRead += r;
        float pct = (100.0 * totalRead) / contentLength;
        Serial.printf("\r[OTA] Progress: %.1f%%", pct);
    }
    Serial.println();

    // Finalize
    if (!Update.end()) {
        Serial.printf("[OTA] Update.end failed: %s\n", Update.errorString());
        mqtt.publish(pubTopic, "ota_end_failed");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    if (!Update.isFinished()) {
        Serial.println("[OTA] Update not finished");
        mqtt.publish(pubTopic, "ota_not_finished");
        otaRequested = false;
        otaInProgress = false;
        gsmClient.stop();
        ESP.restart();
    }

    Serial.println("[OTA] Update successful - restarting...");
    mqtt.publish(pubTopic, "ota_success");
    delay(1500);
    ESP.restart();

    // Should never reach here
    vTaskDelete(NULL);
}

// ==================== Setup & Loop ====================

void setup() {
    initializeHardware();

    // quick blink on startup
    for (int i = 0; i < 3; ++i) {
        digitalWrite(STATUS_LED_PIN, HIGH);
        delay(100);
        digitalWrite(STATUS_LED_PIN, LOW);
        delay(100);
    }
    FastLED.addLeds<NEOPIXEL, DATA_PIN>(leds, NUM_LEDS);

    // Create FreeRTOS objects
    sensorQueue = xQueueCreate(SENSOR_QUEUE_SIZE, sizeof(SensorData));
    networkMutex = xSemaphoreCreateMutex();
    if (sensorQueue == NULL || networkMutex == NULL) {
        Serial.println("[SETUP] Failed to create FreeRTOS objects - restarting");
        delay(1000);
        ESP.restart();
    }

    // Initialize TinyGSM Serial1 already in initializeHardware()

    // Create tasks
    BaseType_t n1 = xTaskCreatePinnedToCore(networkTask, "NetworkTask",
                                            NETWORK_TASK_STACK_SIZE, NULL,
                                            NETWORK_TASK_PRIORITY,
                                            &networkTaskHandle, 0);
    if (n1 != pdPASS) {
        Serial.println("[SETUP] Failed to create networkTask - restart");
        delay(1000);
        ESP.restart();
    }

    BaseType_t n2 = xTaskCreatePinnedToCore(mainTask, "mainTask",
                                            MAIN_TASK_STACK_SIZE, NULL,
                                            MAIN_TASK_PRIORITY,
                                            &mainTaskHandle, 1);
    if (n2 != pdPASS) {
        Serial.println("[SETUP] Failed to create mainTask - restart");
        delay(1000);
        ESP.restart();
    }

    // Setup watchdog for tasks (network & main)
    esp_task_wdt_init(WDT_TIMEOUT_MS / 1000, true); // panic on timeout
    if (networkTaskHandle) esp_task_wdt_add(networkTaskHandle);
    if (mainTaskHandle) esp_task_wdt_add(mainTaskHandle);

    Serial.println("[SETUP] System ready");

    leds[0] = CRGB::Red;
    FastLED.show();
    delay(50);
    leds[0] = CRGB::Green;
    FastLED.show();
    delay(50);
    leds[0] = CRGB::Blue;
    FastLED.show();
    delay(50);
    leds[0] = CRGB::Pink;
    FastLED.show();
    delay(50);
    leds[0] = CRGB::Black;
    FastLED.show();
    delay(50);
}

void loop() {
    // All work is done in tasks. Idle here.
    vTaskDelay(portMAX_DELAY);
}
