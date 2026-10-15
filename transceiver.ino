
#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <math.h>

// Change to 1 or 2 on each ESP32
// (PlatformIO envs radio_node1 / radio_node2 set this for you)
#ifndef NODE_ID
#define NODE_ID 1
#endif

// XIAO ESP32S3 wiring
#define RFM95_CS  D3
#define RFM95_RST D2
#define RFM95_INT D1

#define LORA_FREQ 915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

// Settings
const unsigned long TX_INTERVAL = 3000;
const unsigned long PLOT_INTERVAL = 500;
const int CAL_SAMPLES = 20;
const float FILTER_ALPHA = 0.25;

float pathLossExponent = 2.0;

// Calibration
float referenceDistance = 1.0;
float referenceRSSI = -70.0;
float calibrationSum = 0;
int calibrationCount = 0;

bool calibrating = false;
bool calibrated = false;

// Measurement
float filteredRSSI = 0;
float estimatedDistance = 0;
int lastRSSI = 0;
float lastSNR = 0;

bool filterReady = false;
bool plotMode = false;
bool havePacket = false;

unsigned long nextTransmit = 0;
unsigned long lastPlot = 0;
uint32_t packetNumber = 0;

// ==================================
// DISTANCE ESTIMATE
// ==================================
float calculateDistance(float rssi) {
  float exponent =
    (referenceRSSI - rssi) /
    (10.0 * pathLossExponent);

  return referenceDistance * pow(10.0, exponent);
}

// ==================================
// START CALIBRATION
// ==================================
void startCalibration(float distance) {
  if (!isfinite(distance) || distance <= 0) {
    if (!plotMode)
      Serial.println("Invalid distance.");
    return;
  }

  referenceDistance = distance;
  calibrationSum = 0;
  calibrationCount = 0;
  calibrating = true;
  calibrated = false;
  filterReady = false;
  havePacket = false;

  // Calibration is intended in text mode
  plotMode = false;

  Serial.println();
  Serial.println("=== CALIBRATION ===");

  Serial.print("Known distance: ");
  Serial.print(distance, 3);
  Serial.println(" m");

  Serial.println("Collecting 20 packets...");
}

// ==================================
// SERIAL COMMANDS
// ==================================
void handleSerial() {
  if (!Serial.available()) return;

  String cmd = Serial.readStringUntil('\n');
  cmd.trim();

  if (cmd.startsWith("c ")) {
    float distance = cmd.substring(2).toFloat();
    startCalibration(distance);
  }

  else if (cmd.startsWith("n ")) {
    float n = cmd.substring(2).toFloat();

    if (n >= 1.0 && n <= 6.0) {
      pathLossExponent = n;

      if (calibrated && havePacket) {
        estimatedDistance =
          calculateDistance(filteredRSSI);
      }

      if (!plotMode) {
        Serial.print("Path loss exponent: ");
        Serial.println(n);
      }
    }
  }

  else if (cmd == "plot") {
    plotMode = true;
  }

  else if (cmd == "text") {
    plotMode = false;
    Serial.println("Text mode enabled.");
  }

  else if (cmd == "status") {
    if (plotMode) return;

    Serial.print("Node: ");
    Serial.println(NODE_ID);

    Serial.print("Calibrated: ");
    Serial.println(calibrated ? "YES" : "NO");

    Serial.print("Reference distance: ");
    Serial.println(referenceDistance, 3);

    Serial.print("Reference RSSI: ");
    Serial.println(referenceRSSI, 2);

    Serial.print("Path loss exponent: ");
    Serial.println(pathLossExponent, 2);
  }

  else if (!plotMode) {
    Serial.println("Commands:");
    Serial.println("c 3.5  - Calibrate at 3.5 m");
    Serial.println("n 2.5  - Set path loss exponent");
    Serial.println("plot   - Enable graph output");
    Serial.println("text   - Enable text output");
    Serial.println("status - Display settings");
  }
}

// ==================================
// RECEIVE RADIO PACKETS
// ==================================
void receivePackets() {
  if (!rf95.available()) return;

  uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);

  if (!rf95.recv(buf, &len)) return;

  uint8_t sender = rf95.headerFrom();

  // Only accept the other node
  if (sender != (NODE_ID == 1 ? 2 : 1)) return;

  int rssi = rf95.lastRssi();
  float snr = rf95.lastSNR();

  lastRSSI = rssi;
  lastSNR = snr;

  // Calibration
  if (calibrating) {
    calibrationSum += rssi;
    calibrationCount++;

    Serial.print("Calibration: ");
    Serial.print(calibrationCount);
    Serial.print("/");
    Serial.print(CAL_SAMPLES);
    Serial.print(" RSSI=");
    Serial.println(rssi);

    if (calibrationCount >= CAL_SAMPLES) {
      referenceRSSI =
        calibrationSum / calibrationCount;

      calibrating = false;
      calibrated = true;

      // Reset filtering after calibration
      filteredRSSI = referenceRSSI;
      filterReady = false;
      havePacket = false;

      Serial.println();
      Serial.println("CALIBRATION COMPLETE");

      Serial.print("Reference RSSI: ");
      Serial.print(referenceRSSI, 2);
      Serial.println(" dBm");

      Serial.println("Type 'plot' to graph.");
    }

    return;
  }

  // Exponential moving average
  if (!filterReady) {
    filteredRSSI = rssi;
    filterReady = true;
  } else {
    filteredRSSI =
      FILTER_ALPHA * rssi +
      (1.0 - FILTER_ALPHA) * filteredRSSI;
  }

  havePacket = true;

  if (calibrated) {
    estimatedDistance =
      calculateDistance(filteredRSSI);
  }

  if (!plotMode) {
    Serial.println();
    Serial.print("RX from node ");
    Serial.println(sender);

    Serial.print("RSSI: ");
    Serial.print(rssi);
    Serial.println(" dBm");

    Serial.print("SNR: ");
    Serial.print(snr, 2);
    Serial.println(" dB");

    if (calibrated) {
      Serial.print("Distance: ");
      Serial.print(estimatedDistance, 2);
      Serial.println(" m");
    } else {
      Serial.println("Not calibrated.");
    }
  }
}

// ==================================
// TRANSMIT RADIO PACKET
// ==================================
void transmitPacket() {
  char message[40];
  packetNumber++;

  snprintf(
    message,
    sizeof(message),
    "Node %d packet %lu",
    NODE_ID,
    (unsigned long)packetNumber
  );

  rf95.setHeaderFrom(NODE_ID);
  rf95.setHeaderTo(RH_BROADCAST_ADDRESS);

  bool started = rf95.send(
    (uint8_t*)message,
    strlen(message)
  );

  bool sent = started &&
    rf95.waitPacketSent(1500);

  if (!plotMode) {
    Serial.println(sent ? "TX OK" : "TX FAILED");
  }

  rf95.setModeRx();
}

// ==================================
// SERIAL PLOTTER OUTPUT
// ==================================
void updatePlot() {
  if (!plotMode || !calibrated || !havePacket)
    return;

  if (millis() - lastPlot < PLOT_INTERVAL)
    return;

  lastPlot = millis();

  // Arduino IDE Serial Plotter format
  Serial.print("Distance_m:");
  Serial.print(estimatedDistance, 3);

  Serial.print(",Reference_m:");
  Serial.println(referenceDistance, 3);
}

// ==================================
// SETUP
// ==================================
void setup() {
  Serial.begin(115200);
  Serial.setTimeout(100);
  delay(2000);

  SPI.begin(D8, D9, D10, RFM95_CS);

  pinMode(RFM95_RST, OUTPUT);

  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    Serial.println("RFM95 INIT FAILED");
    while (true) delay(1000);
  }

  uint8_t version = rf95.getDeviceVersion();
  if (version != 0x12) {
    Serial.println("Unexpected radio chip version");
    while (true) delay(1000);
  }

  if (!rf95.setFrequency(LORA_FREQ)) {
    Serial.println("Frequency setup failed");
    while (true) delay(1000);
  }

  rf95.setTxPower(13, false);
  rf95.setThisAddress(NODE_ID);
  rf95.setHeaderFrom(NODE_ID);
  rf95.setModeRx();

  nextTransmit = millis() +
    (NODE_ID == 1 ? 500 : 2000);

  Serial.println("LoRa radio ready.");
  Serial.print("NODE ID: ");
  Serial.println(NODE_ID);
  Serial.println("Frequency: 915 MHz");
  Serial.println();
  Serial.println("Calibrate using: c 3.5");
  Serial.println("Plot using: plot");
}

// ==================================
// LOOP
// ==================================
void loop() {
  handleSerial();
  receivePackets();

  if ((int32_t)(millis() - nextTransmit) >= 0) {
    nextTransmit = millis() + TX_INTERVAL;
    transmitPacket();
  }

  updatePlot();
}
