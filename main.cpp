#define TINY_GSM_MODEM_SIM900

#include <TinyGsmClient.h>
#include <PubSubClient.h>

// Debug Serial and SIM900L connection pins
#define SerialMon Serial
#define SerialAT Serial1
#define MODEM_RST 5
#define MODEM_TX 17
#define MODEM_RX 16
#define ADC_PIN 34

//Device config....--------------
#define WORK_PACKAGE                                "1101"
#define DEVICE_TYPE                                 "00"
#define DEVICE_CODE_UPLOAD_DATE                     "250912"
#define DEVICE_SERIAL_ID                            "0002"

#define UNIQUE_DEVICE_ID                            WORK_PACKAGE DEVICE_TYPE DEVICE_CODE_UPLOAD_DATE DEVICE_SERIAL_ID

float filteredValue;

const char apn[] = "gpinternet";
const char user[] = "";
const char pass[] = "";

const char* broker = "broker.hivemq.com";
const char* mqttUser = "";
const char* mqttPass = "";
const char* topic = "sabbir/test";

unsigned long lastGprsConnectedTime = 0;


// GSM & MQTT Objects
TinyGsm modem(SerialAT);
TinyGsmClient mqttClient(modem);
PubSubClient mqtt(mqttClient);



void connectNetwork() {
  SerialMon.println("Connecting to GPRS...");
  if (!modem.gprsConnect(apn, user, pass)) {
    SerialMon.println("GPRS failed, restarting modem...");
    modem.restart();
    delay(1000);
    if (!modem.gprsConnect(apn, user, pass)) {
      SerialMon.println("GPRS connection failed again.");
      return;  // Don't halt the system
    }
  }
  SerialMon.println("GPRS connected.");
  lastGprsConnectedTime = millis();  // Update timestamp
}

void reconnectMqtt() {
  SerialMon.print("Connecting to MQTT...");
  while (!mqtt.connect("SIM900Client", mqttUser, mqttPass)) {
    SerialMon.print("Failed, rc=");
    SerialMon.print(mqtt.state());
    SerialMon.println(" retrying in 5 seconds...");
    delay(5000);
  }
  SerialMon.println("MQTT connected.");
}

void publishMqttWithRetry() {
    lastGprsConnectedTime = millis();  // Reset GPRS connection time
    int maxRetry = 3;
    int retryCount = 0;
    bool success = false;

    // Prepare payload
    // int randomValue = random(1, 100);
    //======================================================
    int rawValue = 0;
    // Take 20 readings and average them
    for (int i = 0; i < 20; i++) {  
        rawValue += analogRead(ADC_PIN);
        delay(2);  // Small delay for stability
    }
    rawValue /= 20;  // Get the average value

    // Apply Exponential Moving Average (EMA) Filter
    filteredValue = 0.1 * rawValue + 0.9 * filteredValue;  
    //======================================================

    int randomValue = filteredValue;
    // String payload = String("{\"random\":") + randomValue + ",\"message\": hello sabbir\"" + "\"}";
    String payload = String(UNIQUE_DEVICE_ID) + ",25.32,46," + String(randomValue) + "Dhaka,12/9/2025,1";
    // snprintf(payload, sizeof(payload), "%s,25.32,46,%d,84,Dhaka,12/9/2025,1", DEVICE_ID.c_str(), tempreture, moisture, mappedValue, battery_level, City, date, type);

    SerialMon.print("Publishing message: ");
    SerialMon.println(payload);

    while (retryCount < maxRetry) {
        if (mqtt.publish(topic, payload.c_str())) {
        SerialMon.println("Message published.");
        success = true;
        break;
        } else {
        SerialMon.println("Publish failed. Retrying...");
        retryCount++;
        delay(2000);
        }
    }

    if (!success) {
        SerialMon.println("MQTT publish failed after retries. Checking network...");
        
        if (!modem.isNetworkConnected()) {
        SerialMon.println("No network. Restarting modem...");
        modem.restart();
        delay(1000);
        }

        if (!modem.gprsConnect(apn, user, pass)) {
        SerialMon.println("GPRS reconnect failed.");
        } else {
        SerialMon.println("GPRS reconnected.");
        }

        reconnectMqtt();
    }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  SerialMon.print("Message arrived on topic: ");
  SerialMon.println(topic);
  SerialMon.print("Message: ");
  for (int i = 0; i < length; i++) {
    SerialMon.print((char)payload[i]);
  }
  SerialMon.println();
}





void setup() {
  SerialMon.begin(115200);
  delay(10);
  SerialMon.println("Initializing...");

  // SIM900L Serial
  SerialAT.begin(9600, SERIAL_8N1, MODEM_RX, MODEM_TX);

  // Modem Reset Pin
  pinMode(MODEM_RST, OUTPUT);
  digitalWrite(MODEM_RST, LOW);
  delay(1000);
  digitalWrite(MODEM_RST, HIGH);
  delay(1000);

  // Start Modem
  SerialMon.println("Starting modem...");
  modem.restart();

  SerialMon.print("Modem Info: ");
  SerialMon.println(modem.getModemInfo());

  connectNetwork();

  // MQTT Setup
  mqtt.setServer(broker, 1883);
  mqtt.setCallback(mqttCallback);
}

void loop() {
    // Check if GPRS disconnected for too long (20 minutes = 1,200,000 ms)
    if (!modem.isNetworkConnected()) {
    unsigned long disconnectedTime = millis() - lastGprsConnectedTime;
    if (disconnectedTime > 1200000) {
        SerialMon.println("GPRS disconnected for more than 20 minutes. Restarting ESP...");
        ESP.restart();
    }
    } else {
    lastGprsConnectedTime = millis();  // Still connected, update time
    }


  if (!mqtt.connected()) {
    reconnectMqtt();
  }
  mqtt.loop();

  static unsigned long lastSend = 0;
  if (millis() - lastSend > 10000) {
    lastSend = millis();
    publishMqttWithRetry();
  }
}