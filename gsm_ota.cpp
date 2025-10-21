#include <Arduino.h>
#include <TinyGsmClient.h>
#include <Update.h>

#define SerialMon Serial
#define SerialAT Serial1

// === Configuration ===
const char* otaHost = "example.com";       // your OTA server host
const int otaPort = 80;                    // your OTA server port
const char* firmwarePath = "/firmware.bin"; // path to .bin file
const char* versionPath = "/version.txt";   // optional, for version checking
const char* CURRENT_FW_VERSION = "1.0.0";   // current firmware version

#define MODEM_RX 26
#define MODEM_TX 27
#define MODEM_PWR 4

#define APN   "your.apn"
#define USER  ""
#define PASS  ""

// Objects
TinyGsm modem(SerialAT);
TinyGsmClient client(modem);

void initializeModem() {
  SerialMon.println("[MODEM] Powering on...");
  pinMode(MODEM_PWR, OUTPUT);
  digitalWrite(MODEM_PWR, LOW);
  delay(1000);
  digitalWrite(MODEM_PWR, HIGH);
  delay(3000);

  SerialMon.println("[MODEM] Starting modem...");
  if (!modem.restart()) {
    SerialMon.println("Modem restart failed!");
  }
  SerialMon.print("Modem Info: ");
  SerialMon.println(modem.getModemInfo());

  SerialMon.println("[MODEM] Waiting for network...");
  if (!modem.waitForNetwork()) {
    SerialMon.println("Network registration failed!");
  } else {
    SerialMon.println("Network registered.");
  }

  SerialMon.println("[MODEM] Connecting to GPRS...");
  if (!modem.gprsConnect(APN, USER, PASS)) {
    SerialMon.println("GPRS connection failed!");
  } else {
    SerialMon.println("GPRS connected.");
    SerialMon.print("IP: "); SerialMon.println(modem.getLocalIP());
  }
}

void performOTA() {
  SerialMon.println("\n=== OTA UPDATE START ===");

  if (!client.connect(otaHost, otaPort)) {
    SerialMon.println("[OTA] Connection to server failed!");
    return;
  }

  SerialMon.printf("[OTA] Connected to %s:%d\n", otaHost, otaPort);
  client.print(String("GET ") + firmwarePath + " HTTP/1.1\r\n" +
               "Host: " + otaHost + "\r\n" +
               "Connection: close\r\n\r\n");

  unsigned long timeout = millis();
  while (!client.available() && millis() - timeout < 8000) delay(100);

  // Parse headers
  int contentLength = 0;
  while (client.available()) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.startsWith("Content-Length:")) {
      contentLength = line.substring(15).toInt();
    }
    if (line.length() == 0) break; // End of headers
  }

  if (contentLength <= 0) {
    SerialMon.println("[OTA] Invalid Content-Length!");
    client.stop();
    return;
  }

  SerialMon.printf("[OTA] Firmware Size: %d bytes\n", contentLength);
  if (!Update.begin(contentLength)) {
    SerialMon.printf("[OTA] Update.begin() failed: %s\n", Update.errorString());
    return;
  }

  uint8_t buffer[1024];
  size_t written = 0;

  while (client.connected() && written < contentLength) {
    int bytesRead = client.readBytes(buffer, sizeof(buffer));
    if (bytesRead > 0) {
      size_t bytesWritten = Update.write(buffer, bytesRead);
      written += bytesWritten;
      SerialMon.printf("[Progress] %.1f%%\r", (100.0 * written / contentLength));
    } else {
      delay(10);
    }
  }

  SerialMon.println("\n[OTA] Download finished. Finalizing...");
  if (!Update.end()) {
    SerialMon.printf("[OTA] Update.end() failed: %s\n", Update.errorString());
    return;
  }

  if (!Update.isFinished()) {
    SerialMon.println("[OTA] Update not complete!");
    return;
  }

  SerialMon.println("[OTA] Update complete. Restarting...");
  delay(2000);
  ESP.restart();
}

void setup() {
  SerialMon.begin(115200);
  delay(2000);
  SerialMon.println("\n\n=== OTA Test Firmware v1.0.0 ===");

  SerialAT.begin(115200, SERIAL_8N1, MODEM_RX, MODEM_TX);
  delay(1000);

  initializeModem();

  performOTA();
}

void loop() {
  // nothing here
}
