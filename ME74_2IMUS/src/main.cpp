// =============================================================================
// ME74 combined firmware: LoRa radio + ESP-NOW + IMU + GPS on one XIAO ESP32-S3
// =============================================================================
//
// This merges four sketches from the ME74 repo into one program:
//   LORA    transceiver.ino            RFM95 LoRa link, RSSI-based distance
//   ESPNOW  espnow-rssi/src/main.cpp   ESP-NOW link, RSSI + packet loss
//   IMU     IMU_initialtest.cpp        ICM-20948 roll / pitch / heading
//   GPS     gps-baseline/src/main.cpp  Adafruit Ultimate GPS, JSON output
//
// Flash the SAME firmware on both boards. Only the LoRa node number differs:
// use the PlatformIO environment "node1" for one board and "node2" for the other.
// A sensor that isn't plugged in is skipped (the rest keeps running).
//
// ---------------------------------------------------------------- Wiring ----
//   LoRa RFM95:  SCK -> D8   MISO -> D9   MOSI -> D10   CS -> D3
//                RST -> D2   G0/INT -> D1               (same as before)
//   GPS:         GPS TX -> D7 (RX)   GPS RX -> D6 (TX)  (same as before)
//   IMU:         SDA -> D4   SCL -> D5                  (MOVED, see below)
//                Two IMUs can share these two pins (I2C bus, different addresses):
//                  IMU   = ICM-20948 (9-DoF, I2C 0x69/0x68)          -> [IMU] lines
//                  IMU 2 = SparkFun ISM330DHCX Qwiic (6-DoF, 0x6B/0x6A) -> [IMU2] lines
//                Either or both can be connected.
//                Qwiic cable colors: black GND, red 3V3, blue SDA (D4), yellow SCL (D5)
//   All:         VIN/VCC -> 3V3, GND -> GND
//
//   The IMU used D9/D10 in IMU_initialtest.cpp, but those are the LoRa SPI
//   MISO/MOSI pins. On one board they can't share, so the IMU moves to D4/D5
//   (the XIAO's standard I2C pins). Change IMU_SDA_PIN/IMU_SCL_PIN to move it.
//
// ---------------------------------------------------------- Serial output ---
// Every line starts with a tag saying which part of the code printed it:
//   [LORA] ...   [ESPNOW] ...   [IMU] ...   [GPS] ...   [SYS] ...
// After the tag, each line is exactly what the original sketch printed, so
// ME74_dashboard.html can route it to the right panel.
//
// -------------------------------------------------------- Serial commands ---
// Send one command per line (Serial Monitor at 115200):
//   IMU:    0 (recalibrate, keep still)   m (magnetometer cal, 20 s)
//           l (CSV logging lines)         p (normal printout)
//   IMU 2:  the same, prefixed with "imu2 ": imu2 0, imu2 l, imu2 p
//   GPS:    origin   measure <meters>   cancel
//   LORA:   c <meters> (calibrate)   n <exponent>   plot   text   status
//   SYSTEM: go                 start everything      stop          stop everything
//           go <module>        start one module      stop <module> stop one module
//           only <module>      run just this module (stops the others)
//           state              which modules are running / found
//           help   info        (module = lora, espnow, imu, imu2, gps)
//
// At power-up every module starts on its own (AUTO_START below), so a board on
// a battery with no computer attached still works as the other end of the link.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <Wire.h>
#include <Adafruit_ICM20X.h>
#include <Adafruit_ICM20948.h>
#include <Adafruit_ISM330DHCX.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GPS.h>
#include <math.h>

// LoRa node number: set by the PlatformIO environment (node1 / node2)
#ifndef NODE_ID
#define NODE_ID 1
#endif

// Turn a whole module off by setting it to 0
#define ENABLE_LORA   1
#define ENABLE_ESPNOW 1
#define ENABLE_IMU    1
#define ENABLE_GPS    1

// Start every module at power-up. Set to 0 to wait for "go" commands instead.
#define AUTO_START    1

// =============================================================================
// Tagged output: every line a module prints gets "[TAG] " in front of it.
// Each module prints through its own TaggedOut instead of Serial directly.
// =============================================================================
class TaggedOut : public Print {
 public:
  explicit TaggedOut(const char *tag) : tag_(tag) {}

  size_t write(uint8_t c) override {
    if (atLineStart_) {
      for (const char *t = tag_; *t; t++) put(*t);
      atLineStart_ = false;
    }
    put(c);
    if (c == '\n') {
      flush();
      atLineStart_ = true;
    }
    return 1;
  }

  size_t write(const uint8_t *buf, size_t n) override {
    for (size_t i = 0; i < n; i++) write(buf[i]);
    return n;
  }

  void flush() override {
    if (len_) Serial.write(buf_, len_);
    len_ = 0;
  }

 private:
  void put(uint8_t c) {
    buf_[len_++] = c;
    if (len_ >= sizeof(buf_)) flush();   // very long line: send what we have
  }
  const char *tag_;
  uint8_t buf_[512];
  size_t len_ = 0;
  bool atLineStart_ = true;
};

TaggedOut SYSOUT("[SYS] ");
TaggedOut LORAOUT("[LORA] ");
TaggedOut NOWOUT("[ESPNOW] ");
TaggedOut IMUOUT("[IMU] ");
TaggedOut IMU2OUT("[IMU2] ");
TaggedOut GPSOUT("[GPS] ");

// =============================================================================
// LORA: RFM95 transceiver (from transceiver.ino)
// Changes from the original: transmitting no longer waits for the packet to
// finish sending (that blocked everything else for ~50 ms every 3 s), and a
// missing radio no longer stops the whole board.
// =============================================================================
namespace lora {

#define RFM95_CS  D3
#define RFM95_RST D2
#define RFM95_INT D1
#define LORA_FREQ 915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);
bool ok = false;

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

// Non-blocking transmit
bool txInProgress = false;
unsigned long txStartMs = 0;

float calculateDistance(float rssi) {
  float exponent = (referenceRSSI - rssi) / (10.0 * pathLossExponent);
  return referenceDistance * pow(10.0, exponent);
}

void startCalibration(float distance) {
  if (!isfinite(distance) || distance <= 0) {
    if (!plotMode) LORAOUT.println("Invalid distance.");
    return;
  }
  referenceDistance = distance;
  calibrationSum = 0;
  calibrationCount = 0;
  calibrating = true;
  calibrated = false;
  filterReady = false;
  havePacket = false;
  plotMode = false;   // calibration is intended in text mode

  LORAOUT.println();
  LORAOUT.println("=== CALIBRATION ===");
  LORAOUT.print("Known distance: ");
  LORAOUT.print(distance, 3);
  LORAOUT.println(" m");
  LORAOUT.println("Collecting 20 packets...");
}

// Returns true if the command belonged to the LoRa module
bool command(const String &cmd) {
  if (cmd.startsWith("c ")) {
    startCalibration(cmd.substring(2).toFloat());
  } else if (cmd.startsWith("n ")) {
    float n = cmd.substring(2).toFloat();
    if (n >= 1.0 && n <= 6.0) {
      pathLossExponent = n;
      if (calibrated && havePacket) estimatedDistance = calculateDistance(filteredRSSI);
      if (!plotMode) {
        LORAOUT.print("Path loss exponent: ");
        LORAOUT.println(n);
      }
    }
  } else if (cmd == "plot") {
    plotMode = true;
  } else if (cmd == "text") {
    plotMode = false;
    LORAOUT.println("Text mode enabled.");
  } else if (cmd == "status") {
    if (plotMode) return true;
    if (!ok) LORAOUT.println("RFM95 INIT FAILED");
    LORAOUT.print("Node: ");
    LORAOUT.println(NODE_ID);
    LORAOUT.print("Calibrated: ");
    LORAOUT.println(calibrated ? "YES" : "NO");
    LORAOUT.print("Reference distance: ");
    LORAOUT.println(referenceDistance, 3);
    LORAOUT.print("Reference RSSI: ");
    LORAOUT.println(referenceRSSI, 2);
    LORAOUT.print("Path loss exponent: ");
    LORAOUT.println(pathLossExponent, 2);
  } else {
    return false;
  }
  return true;
}

void receivePackets() {
  if (!rf95.available()) return;   // also switches the radio back to receive

  uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
  uint8_t len = sizeof(buf);
  if (!rf95.recv(buf, &len)) return;

  uint8_t sender = rf95.headerFrom();
  if (sender != (NODE_ID == 1 ? 2 : 1)) return;   // only accept the other node

  int rssi = rf95.lastRssi();
  float snr = rf95.lastSNR();
  lastRSSI = rssi;
  lastSNR = snr;

  if (calibrating) {
    calibrationSum += rssi;
    calibrationCount++;
    LORAOUT.print("Calibration: ");
    LORAOUT.print(calibrationCount);
    LORAOUT.print("/");
    LORAOUT.print(CAL_SAMPLES);
    LORAOUT.print(" RSSI=");
    LORAOUT.println(rssi);

    if (calibrationCount >= CAL_SAMPLES) {
      referenceRSSI = calibrationSum / calibrationCount;
      calibrating = false;
      calibrated = true;
      filteredRSSI = referenceRSSI;
      filterReady = false;
      havePacket = false;

      LORAOUT.println();
      LORAOUT.println("CALIBRATION COMPLETE");
      LORAOUT.print("Reference RSSI: ");
      LORAOUT.print(referenceRSSI, 2);
      LORAOUT.println(" dBm");
      LORAOUT.println("Type 'plot' to graph.");
    }
    return;
  }

  // Exponential moving average
  if (!filterReady) {
    filteredRSSI = rssi;
    filterReady = true;
  } else {
    filteredRSSI = FILTER_ALPHA * rssi + (1.0 - FILTER_ALPHA) * filteredRSSI;
  }
  havePacket = true;
  if (calibrated) estimatedDistance = calculateDistance(filteredRSSI);

  if (!plotMode) {
    LORAOUT.println();
    LORAOUT.print("RX from node ");
    LORAOUT.println(sender);
    LORAOUT.print("RSSI: ");
    LORAOUT.print(rssi);
    LORAOUT.println(" dBm");
    LORAOUT.print("SNR: ");
    LORAOUT.print(snr, 2);
    LORAOUT.println(" dB");
    if (calibrated) {
      LORAOUT.print("Distance: ");
      LORAOUT.print(estimatedDistance, 2);
      LORAOUT.println(" m");
    } else {
      LORAOUT.println("Not calibrated.");
    }
  }
}

void transmitPacket() {
  char message[40];
  packetNumber++;
  snprintf(message, sizeof(message), "Node %d packet %lu", NODE_ID, (unsigned long)packetNumber);

  rf95.setHeaderFrom(NODE_ID);
  rf95.setHeaderTo(RH_BROADCAST_ADDRESS);

  if (rf95.send((uint8_t *)message, strlen(message))) {
    txInProgress = true;          // finished later, in service()
    txStartMs = millis();
  } else {
    if (!plotMode) LORAOUT.println("TX FAILED");
    rf95.setModeRx();
  }
}

void updatePlot() {
  if (!plotMode || !calibrated || !havePacket) return;
  if (millis() - lastPlot < PLOT_INTERVAL) return;
  lastPlot = millis();
  // Arduino IDE Serial Plotter format
  LORAOUT.print("Distance_m:");
  LORAOUT.print(estimatedDistance, 3);
  LORAOUT.print(",Reference_m:");
  LORAOUT.println(referenceDistance, 3);
}

void setup() {
  SPI.begin(D8, D9, D10, RFM95_CS);

  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);
  digitalWrite(RFM95_RST, LOW);
  delay(10);
  digitalWrite(RFM95_RST, HIGH);
  delay(10);

  if (!rf95.init()) {
    LORAOUT.println("RFM95 INIT FAILED");
    return;
  }
  if (rf95.getDeviceVersion() != 0x12) {
    LORAOUT.println("Unexpected radio chip version");
    return;
  }
  if (!rf95.setFrequency(LORA_FREQ)) {
    LORAOUT.println("Frequency setup failed");
    return;
  }

  rf95.setTxPower(13, false);
  rf95.setThisAddress(NODE_ID);
  rf95.setHeaderFrom(NODE_ID);
  rf95.setModeRx();
  ok = true;

  nextTransmit = millis() + (NODE_ID == 1 ? 500 : 2000);

  LORAOUT.println("LoRa radio ready.");
  LORAOUT.print("NODE ID: ");
  LORAOUT.println(NODE_ID);
  LORAOUT.println("Frequency: 915 MHz");
  LORAOUT.println("Calibrate using: c 3.5");
  LORAOUT.println("Plot using: plot");
}

void start() {
  if (!ok) return;
  txInProgress = false;
  rf95.setModeRx();
  nextTransmit = millis() + (NODE_ID == 1 ? 500 : 2000);
  LORAOUT.println("LoRa radio ready.");
}

void stop() {
  if (!ok) return;
  txInProgress = false;
  rf95.setModeIdle();
  LORAOUT.println("LoRa stopped.");
}

void service() {
  if (!ok) return;

  // Finish a transmission that's in progress
  if (txInProgress) {
    if (rf95.mode() != RHGenericDriver::RHModeTx) {
      txInProgress = false;
      if (!plotMode) LORAOUT.println("TX OK");
    } else if (millis() - txStartMs > 1500) {
      txInProgress = false;
      if (!plotMode) LORAOUT.println("TX FAILED");
      rf95.setModeIdle();
    }
  }

  if (!txInProgress) receivePackets();

  if (!txInProgress && (int32_t)(millis() - nextTransmit) >= 0) {
    nextTransmit = millis() + TX_INTERVAL;
    transmitPacket();
  }

  updatePlot();
}

}  // namespace lora

// =============================================================================
// ESPNOW: ESP-NOW RSSI link (from espnow-rssi/src/main.cpp)
// =============================================================================
namespace espnow_link {

#define WIFI_CHANNEL      1      // both boards must use the same channel
#define SEND_INTERVAL_MS  100    // how often each board transmits
#define PRINT_INTERVAL_MS 500    // how often results are printed
#define LOST_TIMEOUT_MS   2000   // no packets for this long = "lost"
#define MAC_PRINT_MS      10000  // repeat "My MAC" so a dashboard opened later sees it

// WIFI_POWER_19_5dBm (max), WIFI_POWER_11dBm, WIFI_POWER_5dBm, WIFI_POWER_2dBm
#define TX_POWER          WIFI_POWER_19_5dBm

// Rough distance estimate (log-distance path loss model)
#define RSSI_AT_1M        -45.0
#define PATH_LOSS_N       2.5

const uint32_t MAGIC = 0xB10C5EED;
const int8_t NO_RSSI = 127;

typedef struct __attribute__((packed)) {
  uint32_t magic;
  uint32_t seq;
  int8_t rssiHeard;
} Packet;

uint8_t broadcastAddr[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
bool ok = false;

// Shared between the receive callback (WiFi task) and service()
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
volatile int8_t latestRssi = NO_RSSI;
volatile int8_t peerReported = NO_RSSI;
volatile int32_t rssiSum = 0;
volatile uint32_t rssiCount = 0;
volatile uint32_t lastSeq = 0;
volatile bool haveSeq = false;
volatile uint32_t received = 0;
volatile uint32_t missed = 0;
volatile uint32_t lastRxMs = 0;
uint8_t peerMac[6] = {0};

uint32_t txSeq = 0;
float smoothedRssi = NAN;
uint32_t lastSend = 0, lastPrint = 0, lastMacPrint = 0;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len != sizeof(Packet)) return;
  Packet p;
  memcpy(&p, data, sizeof(p));
  if (p.magic != MAGIC) return;

  int8_t rssi = info->rx_ctrl->rssi;

  portENTER_CRITICAL(&mux);
  latestRssi = rssi;
  peerReported = p.rssiHeard;
  rssiSum += rssi;
  rssiCount = rssiCount + 1;
  if (haveSeq && p.seq > lastSeq) missed += (p.seq - lastSeq - 1);
  lastSeq = p.seq;
  haveSeq = true;
  received = received + 1;
  lastRxMs = millis();
  memcpy(peerMac, info->src_addr, 6);
  portEXIT_CRITICAL(&mux);
}

const char *qualityLabel(float rssi) {
  if (rssi >= -50) return "Excellent";
  if (rssi >= -60) return "Good";
  if (rssi >= -70) return "Fair";
  if (rssi >= -80) return "Weak";
  return "Very weak";
}

float estimateDistance(float rssi) {
  return powf(10.0f, (RSSI_AT_1M - rssi) / (10.0f * PATH_LOSS_N));
}

String signalBar(float rssi) {
  int n = (int)((constrain(rssi, -100.0f, -30.0f) + 100.0f) * 30.0f / 70.0f);
  String bar = "[";
  for (int i = 0; i < 30; i++) bar += (i < n) ? '#' : ' ';
  bar += "]";
  return bar;
}

void printMac() {
  NOWOUT.print("My MAC: ");
  NOWOUT.println(WiFi.macAddress());
}

void setup() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  WiFi.setTxPower(TX_POWER);

  if (esp_now_init() != ESP_OK) {
    NOWOUT.println("ESP-NOW init failed");
    return;
  }
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, broadcastAddr, 6);
  peer.channel = 0;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  if (esp_now_add_peer(&peer) != ESP_OK) NOWOUT.println("Failed to add broadcast peer");

  ok = true;
  NOWOUT.println("=== ESP-NOW RSSI Monitor ===");
  printMac();
  NOWOUT.printf("Channel %d, waiting for the other board...\n", WIFI_CHANNEL);
  lastMacPrint = millis();
}

void start() {
  if (!ok) return;
  smoothedRssi = NAN;
  NOWOUT.println("ESP-NOW started.");
  printMac();
}

void stop() {
  if (ok) NOWOUT.println("ESP-NOW stopped.");
}

void service() {
  if (!ok) return;
  uint32_t now = millis();

  if (now - lastMacPrint >= MAC_PRINT_MS) {
    lastMacPrint = now;
    printMac();
  }

  // ---- Transmit ----
  if (now - lastSend >= SEND_INTERVAL_MS) {
    lastSend = now;
    Packet p;
    p.magic = MAGIC;
    p.seq = ++txSeq;
    portENTER_CRITICAL(&mux);
    bool fresh = (now - lastRxMs) < LOST_TIMEOUT_MS && latestRssi != NO_RSSI;
    p.rssiHeard = fresh ? latestRssi : NO_RSSI;
    portEXIT_CRITICAL(&mux);
    esp_now_send(broadcastAddr, (uint8_t *)&p, sizeof(p));
  }

  // ---- Report ----
  if (now - lastPrint < PRINT_INTERVAL_MS) return;
  lastPrint = now;

  portENTER_CRITICAL(&mux);
  int32_t sum = rssiSum;    rssiSum = 0;
  uint32_t count = rssiCount; rssiCount = 0;
  int8_t peerR = peerReported;
  uint32_t rx = received;
  uint32_t lost = missed;
  uint32_t lastRx = lastRxMs;
  uint8_t mac[6];
  memcpy(mac, peerMac, 6);
  portEXIT_CRITICAL(&mux);

  if (rx == 0 || (now - lastRx) > LOST_TIMEOUT_MS) {
    // DATA,ms,status,raw,smoothed,peer,loss,rx,missed
    NOWOUT.printf("DATA,%lu,LOST,nan,nan,nan,nan,%lu,%lu\n",
                  (unsigned long)now, (unsigned long)rx, (unsigned long)lost);
    NOWOUT.println("No signal from peer (out of range or not powered)");
    smoothedRssi = NAN;
    return;
  }

  float windowAvg = NAN;
  if (count > 0) {
    windowAvg = (float)sum / count;
    smoothedRssi = isnan(smoothedRssi) ? windowAvg : 0.7f * smoothedRssi + 0.3f * windowAvg;
  }
  float lossPct = 100.0f * lost / (float)(rx + lost);

  NOWOUT.printf("DATA,%lu,OK,%.1f,%.2f,", (unsigned long)now, windowAvg, smoothedRssi);
  if (peerR != NO_RSSI) NOWOUT.printf("%d,", peerR);
  else                  NOWOUT.print("nan,");
  NOWOUT.printf("%.2f,%lu,%lu\n", lossPct, (unsigned long)rx, (unsigned long)lost);

  NOWOUT.printf("Peer %02X:%02X:%02X:%02X:%02X:%02X | I hear peer: %6.1f dBm %s %-9s",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                smoothedRssi, signalBar(smoothedRssi).c_str(), qualityLabel(smoothedRssi));
  if (peerR != NO_RSSI) NOWOUT.printf(" | Peer hears me: %4d dBm", peerR);
  else                  NOWOUT.print(" | Peer hears me:   ---  ");
  NOWOUT.printf(" | Loss: %4.1f%% | ~%.1f m\n", lossPct, estimateDistance(smoothedRssi));
}

}  // namespace espnow_link

// =============================================================================
// IMU: ICM-20948 orientation (from IMU_initialtest.cpp)
// Changes from the original: the 100 Hz loop and both calibrations no longer
// block (the board keeps serving the radios and GPS while you calibrate), the
// I2C pins moved to D4/D5, and a missing IMU no longer stops the whole board.
// =============================================================================
struct ImuUnit {
  TaggedOut &out;   // where this IMU prints ([IMU] or [IMU2])
  ImuUnit(TaggedOut &o, int which) : out(o), role(which == 2 ? CHIP_ISM330DHCX : CHIP_ICM20948) {}


// --- I2C pins (D4 = GPIO5 = SDA, D5 = GPIO6 = SCL) ---
#define IMU_SDA_PIN 5
#define IMU_SCL_PIN 6

// --- Timing ---
static constexpr int LOOP_HZ = 100;
static constexpr int PRINT_HZ = 10;
static constexpr int LOG_HZ = 50;

// --- Startup calibration ---
static constexpr float CAL_SECONDS = 3.0f;
static constexpr int SETTLE_MS = 1000;
static constexpr float GYRO_STILL_LIMIT = 0.05f;   // rad/s
static constexpr float ACCEL_STILL_LIMIT = 0.30f;  // m/s^2

static constexpr bool LEVEL_ACCEL_CAL = true;
static constexpr float LEVEL_TOLERANCE = 1.0f;     // m/s^2

static constexpr float ACCEL_BIAS_X_FIXED = -0.10805f;
static constexpr float ACCEL_BIAS_Y_FIXED = -0.11242f;
static constexpr float ACCEL_BIAS_Z_FIXED = +0.34704f;

// --- Magnetometer ---
static constexpr bool USE_MAGNETOMETER = false;
static constexpr float MAG_OFFSET_X_FIXED = -101.97f;
static constexpr float MAG_OFFSET_Y_FIXED = 115.34f;
static constexpr float MAG_OFFSET_Z_FIXED = 126.82f;
static constexpr float MAG_CAL_SECONDS = 20.0f;
static constexpr float DECLINATION_DEG = 0.0f;

// --- Complementary filter ---
static constexpr float TAU_TILT = 1.0f;
static constexpr float TAU_HEADING = 2.0f;
static constexpr float ACCEL_TRUST_BAND = 0.10f * 9.80665f;
static constexpr float MAG_MIN_UT = 20.0f;
static constexpr float MAG_MAX_UT = 70.0f;

static constexpr float GRAVITY = 9.80665f;

struct Vec3 {
  float x, y, z;
};

Adafruit_ICM20948 icm;
uint8_t icmAddress = 0x69;
Adafruit_ISM330DHCX ism;          // SparkFun Micro 6DoF IMU (Qwiic)
uint8_t ismAddress = 0x6B;
enum Chip { CHIP_NONE, CHIP_ICM20948, CHIP_ISM330DHCX };
Chip role;               // which chip this unit looks for
Chip chip = CHIP_NONE;   // which chip it found
bool ok = false;

const char *chipName() { return chip == CHIP_ISM330DHCX ? "ISM330DHCX" : "ICM-20948"; }
const char *notFound() {
  return role == CHIP_ISM330DHCX ? "Failed to find ISM330DHCX chip! Check wiring."
                                 : "Failed to find ICM20948 chip! Check wiring.";
}

// Fallback accel bias if the sensor isn't level at calibration (measured for the ICM-20948 only)
Vec3 fixedAccelBias() {
  if (chip == CHIP_ISM330DHCX) return {0, 0, 0};
  return {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
}

Vec3 gyroBias = {0, 0, 0};
Vec3 accelBias = {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
Vec3 magOffset = {MAG_OFFSET_X_FIXED, MAG_OFFSET_Y_FIXED, MAG_OFFSET_Z_FIXED};

float roll = 0, pitch = 0, heading = 0;
bool filterStarted = false;

static constexpr uint32_t LOOP_PERIOD_US = 1000000UL / LOOP_HZ;
uint32_t nextLoopUs = 0;
uint32_t lastSampleUs = 0;
uint32_t loopCount = 0;
uint32_t overruns = 0;

bool logMode = false;

// What the IMU is doing right now (replaces the original blocking loops)
enum State { SETTLING, CALIBRATING, MAG_CALIBRATING, RUNNING };
State state = SETTLING;
uint32_t settleUntilMs = 0;

// Startup-calibration accumulators
static constexpr int CAL_N = (int)(CAL_SECONDS * LOOP_HZ);
int calCount = 0;
Vec3 sumA, sumG, minG, maxG, minA, maxA;
Vec3 savedAccelBias;

// Magnetometer-calibration accumulators
uint32_t magCalStartMs = 0;
Vec3 magLo, magHi;

float wrapPi(float a) {
  while (a > PI) a -= TWO_PI;
  while (a < -PI) a += TWO_PI;
  return a;
}

// The Adafruit library's enableGyrolDLPF() writes the wrong register,
// so set the gyro low-pass filter directly (bank 2, GYRO_CONFIG_1).
void setGyroLowPass(uint8_t dlpfcfg) {
  const uint8_t REG_BANK_SEL = 0x7F;
  const uint8_t GYRO_CONFIG_1 = 0x01;

  Wire.beginTransmission(icmAddress);
  Wire.write(REG_BANK_SEL);
  Wire.write(2 << 4);
  Wire.endTransmission();

  Wire.beginTransmission(icmAddress);
  Wire.write(GYRO_CONFIG_1);
  Wire.endTransmission(false);
  Wire.requestFrom(icmAddress, (uint8_t)1);
  uint8_t value = Wire.read();

  value &= ~0b00111001;
  value |= (dlpfcfg & 0b111) << 3;
  value |= 0b1;

  Wire.beginTransmission(icmAddress);
  Wire.write(GYRO_CONFIG_1);
  Wire.write(value);
  Wire.endTransmission();

  Wire.beginTransmission(icmAddress);
  Wire.write(REG_BANK_SEL);
  Wire.write(0);
  Wire.endTransmission();
}

void readSensors(Vec3 &accel, Vec3 &gyro, Vec3 &mag) {
  sensors_event_t a, g, t, m;
  if (chip == CHIP_ISM330DHCX) {
    ism.getEvent(&a, &g, &t);     // 6-DoF: no magnetometer
  } else {
    icm.getEvent(&a, &g, &t, &m);
  }

  accel.x = a.acceleration.x - accelBias.x;
  accel.y = a.acceleration.y - accelBias.y;
  accel.z = a.acceleration.z - accelBias.z;

  gyro.x = g.gyro.x - gyroBias.x;
  gyro.y = g.gyro.y - gyroBias.y;
  gyro.z = g.gyro.z - gyroBias.z;

  if (chip == CHIP_ISM330DHCX) {
    mag = {0, 0, 0};
    return;
  }
  // ICM-20948: magnetometer Y and Z point the opposite way from the accel/gyro axes
  mag.x = m.magnetic.x - magOffset.x;
  mag.y = -m.magnetic.y - magOffset.y;
  mag.z = -m.magnetic.z - magOffset.z;
}

void restartLoopTimer() {
  nextLoopUs = micros();
  lastSampleUs = nextLoopUs;
  filterStarted = false;
}

// ---- Startup calibration (sensor must be still) ----
void startBiasCal() {
  out.println("Calibrating... keep the sensor still.");
  gyroBias = {0, 0, 0};
  savedAccelBias = accelBias;
  accelBias = {0, 0, 0};
  calCount = 0;
  sumA = sumG = {0, 0, 0};
  minG = minA = {1e9, 1e9, 1e9};
  maxG = maxA = {-1e9, -1e9, -1e9};
  state = CALIBRATING;
}

void biasCalSample(const Vec3 &a, const Vec3 &g) {
  sumA.x += a.x;  sumA.y += a.y;  sumA.z += a.z;
  sumG.x += g.x;  sumG.y += g.y;  sumG.z += g.z;
  minG.x = min(minG.x, g.x);  maxG.x = max(maxG.x, g.x);
  minG.y = min(minG.y, g.y);  maxG.y = max(maxG.y, g.y);
  minG.z = min(minG.z, g.z);  maxG.z = max(maxG.z, g.z);
  minA.x = min(minA.x, a.x);  maxA.x = max(maxA.x, a.x);
  minA.y = min(minA.y, a.y);  maxA.y = max(maxA.y, a.y);
  minA.z = min(minA.z, a.z);  maxA.z = max(maxA.z, a.z);
  if (++calCount < CAL_N) return;

  // ---- Enough samples: evaluate ----
  bool gyroMoved = (maxG.x - minG.x > GYRO_STILL_LIMIT) || (maxG.y - minG.y > GYRO_STILL_LIMIT) ||
                   (maxG.z - minG.z > GYRO_STILL_LIMIT);
  bool accelMoved = (maxA.x - minA.x > ACCEL_STILL_LIMIT) || (maxA.y - minA.y > ACCEL_STILL_LIMIT) ||
                    (maxA.z - minA.z > ACCEL_STILL_LIMIT);
  if (gyroMoved || accelMoved) {
    out.println("Movement detected during calibration, trying again.");
    accelBias = savedAccelBias;
    startBiasCal();
    return;
  }

  const float N = (float)CAL_N;
  Vec3 meanA = {sumA.x / N, sumA.y / N, sumA.z / N};
  gyroBias = {sumG.x / N, sumG.y / N, sumG.z / N};

  bool isLevel = fabs(meanA.x) < LEVEL_TOLERANCE && fabs(meanA.y) < LEVEL_TOLERANCE &&
                 fabs(meanA.z - GRAVITY) < LEVEL_TOLERANCE;
  if (LEVEL_ACCEL_CAL && isLevel) {
    accelBias = {meanA.x, meanA.y, meanA.z - GRAVITY};
  } else {
    accelBias = fixedAccelBias();
    if (LEVEL_ACCEL_CAL) out.println("WARNING: sensor is not level (Z up), using fixed accel bias values.");
  }

  out.printf("Gyro bias  (rad/s): %.5f  %.5f  %.5f\n", gyroBias.x, gyroBias.y, gyroBias.z);
  out.printf("Accel bias (m/s^2): %.4f  %.4f  %.4f\n", accelBias.x, accelBias.y, accelBias.z);
  out.println("Calibration done.");
  restartLoopTimer();
  state = RUNNING;
}

// ---- Hard-iron magnetometer calibration (rotate the sensor every way) ----
void startMagCal() {
  if (chip == CHIP_ISM330DHCX) {
    out.println("The ISM330DHCX has no magnetometer, so there's nothing to calibrate.");
    return;
  }
  out.println("Magnetometer calibration: slowly rotate the sensor through");
  out.printf("every orientation (figure-8s, flip it over) for %.0f seconds...\n", MAG_CAL_SECONDS);
  magOffset = {0, 0, 0};
  magLo = {1e9, 1e9, 1e9};
  magHi = {-1e9, -1e9, -1e9};
  magCalStartMs = millis();
  state = MAG_CALIBRATING;
}

void magCalSample(const Vec3 &m) {
  magLo.x = min(magLo.x, m.x);  magHi.x = max(magHi.x, m.x);
  magLo.y = min(magLo.y, m.y);  magHi.y = max(magHi.y, m.y);
  magLo.z = min(magLo.z, m.z);  magHi.z = max(magHi.z, m.z);
  if (millis() - magCalStartMs < (uint32_t)(MAG_CAL_SECONDS * 1000)) return;

  magOffset = {(magHi.x + magLo.x) / 2, (magHi.y + magLo.y) / 2, (magHi.z + magLo.z) / 2};
  out.println("Done. Paste these into the MAG_OFFSET_*_FIXED constants:");
  out.printf("  MAG_OFFSET_X_FIXED = %.2ff;\n", magOffset.x);
  out.printf("  MAG_OFFSET_Y_FIXED = %.2ff;\n", magOffset.y);
  out.printf("  MAG_OFFSET_Z_FIXED = %.2ff;\n", magOffset.z);
  restartLoopTimer();
  state = RUNNING;
}

// ---- Complementary filter (unchanged from IMU_initialtest.cpp) ----
void updateOrientation(const Vec3 &a, const Vec3 &g, const Vec3 &m, float dt) {
  float rollAcc = atan2(a.y, a.z);
  float pitchAcc = atan2(-a.x, sqrt(a.y * a.y + a.z * a.z));

  float r = filterStarted ? roll : rollAcc;
  float p = filterStarted ? pitch : pitchAcc;
  float mxLevel = m.x * cos(p) + (m.y * sin(r) + m.z * cos(r)) * sin(p);
  float myLevel = m.y * cos(r) - m.z * sin(r);
  float headingMag = atan2(myLevel, mxLevel);

  if (!filterStarted) {
    roll = rollAcc;
    pitch = pitchAcc;
    heading = (USE_MAGNETOMETER && chip == CHIP_ICM20948) ? headingMag : 0;
    filterStarted = true;
    return;
  }

  float sr = sin(roll), cr = cos(roll);
  float cp = cos(pitch), tp = tan(pitch);
  if (fabs(cp) < 0.1f) cp = (cp < 0 ? -0.1f : 0.1f);

  float rollRate = g.x + (g.y * sr + g.z * cr) * tp;
  float pitchRate = g.y * cr - g.z * sr;
  float yawRate = (g.y * sr + g.z * cr) / cp;

  roll += rollRate * dt;
  pitch += pitchRate * dt;
  heading -= yawRate * dt;

  float alphaTilt = TAU_TILT / (TAU_TILT + dt);
  float accelMag = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
  if (fabs(accelMag - GRAVITY) < ACCEL_TRUST_BAND) {
    roll += (1 - alphaTilt) * wrapPi(rollAcc - roll);
    pitch += (1 - alphaTilt) * wrapPi(pitchAcc - pitch);
  }

  float alphaHead = TAU_HEADING / (TAU_HEADING + dt);
  float fieldUT = sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
  if (USE_MAGNETOMETER && chip == CHIP_ICM20948 && fieldUT > MAG_MIN_UT && fieldUT < MAG_MAX_UT) {
    heading += (1 - alphaHead) * wrapPi(headingMag - heading);
  }

  roll = wrapPi(roll);
  heading = wrapPi(heading);
}

// Returns true if the command belonged to the IMU module
bool command(const String &cmd) {
  if (cmd == "0") {
    if (ok) startBiasCal();
  } else if (cmd == "m") {
    if (ok) startMagCal();
  } else if (cmd == "l") {
    logMode = true;
  } else if (cmd == "p") {
    logMode = false;
  } else {
    return false;
  }
  if (!ok && (cmd == "0" || cmd == "m")) out.println(notFound());
  return true;
}

void setup() {
  // Unit 1 looks for the ICM-20948 (0x69 / 0x68), unit 2 for the ISM330DHCX (0x6B / 0x6A)
  if (role == CHIP_ISM330DHCX) {
    if (ism.begin_I2C(0x6B, &Wire)) {
      chip = CHIP_ISM330DHCX;
    } else if (ism.begin_I2C(0x6A, &Wire)) {
      chip = CHIP_ISM330DHCX;
      ismAddress = 0x6A;
    }
  } else {
    if (icm.begin_I2C(0x69, &Wire)) {
      chip = CHIP_ICM20948;
    } else if (icm.begin_I2C(0x68, &Wire)) {
      chip = CHIP_ICM20948;
      icmAddress = 0x68;
    }
  }
  if (chip == CHIP_NONE) {
    out.println(notFound());
    return;
  }

  if (chip == CHIP_ISM330DHCX) {
    out.printf("ISM330DHCX found at 0x%02X (6-DoF: heading comes from the gyro only)\n", ismAddress);
    ism.setAccelRange(LSM6DS_ACCEL_RANGE_2_G);
    ism.setGyroRange(LSM6DS_GYRO_RANGE_250_DPS);
    ism.setAccelDataRate(LSM6DS_RATE_208_HZ);
    ism.setGyroDataRate(LSM6DS_RATE_208_HZ);
    accelBias = fixedAccelBias();
    magOffset = {0, 0, 0};
  } else {
    out.printf("ICM20948 found at 0x%02X\n", icmAddress);
    icm.setAccelRange(ICM20948_ACCEL_RANGE_2_G);
    icm.setGyroRange(ICM20948_GYRO_RANGE_250_DPS);
    icm.setGyroRateDivisor(4);    // 220 Hz
    icm.setAccelRateDivisor(4);   // 225 Hz
    icm.setMagDataRate(AK09916_MAG_DATARATE_100_HZ);
    icm.enableAccelDLPF(true, ICM20X_ACCEL_FREQ_23_9_HZ);
    setGyroLowPass(4);            // 23.9 Hz
  }

  ok = true;
  settleUntilMs = millis() + SETTLE_MS;   // let the sensor settle, then calibrate
  state = SETTLING;
  restartLoopTimer();
}

// (Re)start: settle briefly, then run the still-calibration again
void start() {
  if (!ok) {
    out.println(notFound());
    return;
  }
  settleUntilMs = millis() + SETTLE_MS;
  state = SETTLING;
  restartLoopTimer();
}

void stop() {
  if (ok) out.println("IMU stopped.");
}

// Called every pass of loop(); does one 100 Hz step when it's time
void service() {
  if (!ok) return;

  uint32_t now = micros();
  if ((int32_t)(now - nextLoopUs) < 0) return;   // not time yet
  nextLoopUs += LOOP_PERIOD_US;
  if ((int32_t)(now - nextLoopUs) > 0) {         // fell behind: resync
    nextLoopUs = now + LOOP_PERIOD_US;
    if (state == RUNNING) overruns++;
  }

  float dt = (now - lastSampleUs) * 1e-6f;
  lastSampleUs = now;

  Vec3 accel, gyro, mag;
  readSensors(accel, gyro, mag);

  switch (state) {
    case SETTLING:
      if ((int32_t)(millis() - settleUntilMs) >= 0) startBiasCal();
      return;
    case CALIBRATING:
      biasCalSample(accel, gyro);
      return;
    case MAG_CALIBRATING:
      magCalSample(mag);
      return;
    case RUNNING:
      break;
  }

  updateOrientation(accel, gyro, mag, dt);

  loopCount++;
  float headingDeg = heading * RAD_TO_DEG + DECLINATION_DEG;
  if (headingDeg < 0) headingDeg += 360;
  if (headingDeg >= 360) headingDeg -= 360;

  if (logMode) {
    // LOG,t_us,ax,ay,az,gx,gy,gz,mx,my,mz,roll,pitch,heading,dt_ms,overruns
    if (loopCount % (LOOP_HZ / LOG_HZ) == 0) {
      out.printf("LOG,%lu,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%lu\n",
                    (unsigned long)now, accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z,
                    mag.x, mag.y, mag.z, roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg,
                    dt * 1000, (unsigned long)overruns);
    }
  } else if (loopCount % (LOOP_HZ / PRINT_HZ) == 0) {
    // Acceleration (m/s^2) first, then orientation. "Name:value" also works in the Serial Plotter.
    out.printf("Ax:%.3f,Ay:%.3f,Az:%.3f,Roll:%.2f,Pitch:%.2f,Heading:%.1f,dt_ms:%.2f,Overruns:%lu\n",
                  accel.x, accel.y, accel.z, roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg,
                  dt * 1000, (unsigned long)overruns);
  }
}

};  // struct ImuUnit

// Two IMUs on the same I2C bus (D4 / D5), each with its own serial tag and dashboard tab
ImuUnit imu1(IMUOUT, 1);   // ICM-20948, 9-DoF   -> [IMU]
ImuUnit imu2(IMU2OUT, 2);  // ISM330DHCX, 6-DoF  -> [IMU2]

// =============================================================================
// GPS: Adafruit Ultimate GPS (from gps-baseline/src/main.cpp)
// =============================================================================
namespace gps {

#define GPS_UART 1
#define GPS_RX_PIN 44   // D7
#define GPS_TX_PIN 43   // D6

#define FAST_BAUD 57600
#define ORIGIN_AVG_MS 30000UL
#define MEASURE_AVG_MS 20000UL
#define MAX_HDOP 2.0

#define WIRING_CHECK_MS 5000UL
#define DATA_TIMEOUT_MS 2000UL
#define PARSE_TIMEOUT_MS 3000UL
#define IDLE_STATUS_MS 1000UL

HardwareSerial GPSSerial(GPS_UART);
Adafruit_GPS GPS(&GPSSerial);

double originLat = 0, originLon = 0;
bool haveOrigin = false;

unsigned long bytesRx = 0;
unsigned long parsedOk = 0;
unsigned long lastByteMs = 0;
unsigned long lastParseMs = 0;
unsigned long startMs = 0;      // when the GPS module was set up
bool wiringChecked = false;

#define RAW_LINE_LEN 100
char rawBuf[RAW_LINE_LEN];
int rawIdx = 0;
char rawLine[RAW_LINE_LEN] = "";

enum AvgMode { AVG_NONE, AVG_ORIGIN, AVG_MEASURE };
AvgMode avgMode = AVG_NONE;
unsigned long avgStart = 0, avgDuration = 0, lastProgressMs = 0;
double avgSumLat = 0, avgSumLon = 0, measureTrueM = 0;
long avgN = 0;

unsigned long lastStatusMs = 0;

double haversineM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371008.8;
  double p1 = radians(lat1), p2 = radians(lat2);
  double dp = p2 - p1, dl = radians(lon2 - lon1);
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2 * R * asin(sqrt(a));
}

void captureRaw(char c) {
  if (c == '\n' || c == '\r') {
    if (rawIdx > 0) {
      rawBuf[rawIdx] = '\0';
      strcpy(rawLine, rawBuf);
      rawIdx = 0;
    }
  } else if (rawIdx < RAW_LINE_LEN - 1) {
    rawBuf[rawIdx++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '.';
  }
}

void sendLog(const char *msg) {
  GPSOUT.printf("{\"type\":\"log\",\"msg\":\"%s\"}\n", msg);
}

bool pollGPS(bool &isRMC) {
  isRMC = false;
  if (GPSSerial.available()) {
    captureRaw(GPS.read());
    bytesRx++;
    lastByteMs = millis();
  }
  if (GPS.newNMEAreceived()) {
    char *nmea = GPS.lastNMEA();
    isRMC = strstr(nmea, "RMC") != NULL;
    bool parsed = GPS.parse(nmea);
    if (parsed) {
      parsedOk++;
      lastParseMs = millis();
    }
    return parsed;
  }
  return false;
}

const char *gpsState() {
  unsigned long now = millis();
  if (bytesRx == 0 || now - lastByteMs > DATA_TIMEOUT_MS) return "no_data";
  if (parsedOk == 0 ? now - startMs > PARSE_TIMEOUT_MS : now - lastParseMs > PARSE_TIMEOUT_MS) return "bad_baud";
  if (parsedOk == 0) return "searching";
  return GPS.fix ? "fix" : "searching";
}

void sendStatus() {
  lastStatusMs = millis();
  bool fix = GPS.fix && parsedOk > 0;
  GPSOUT.printf("{\"type\":\"status\",\"state\":\"%s\",\"sats\":%d,\"hdop\":%.2f,",
                gpsState(), (int)GPS.satellites, GPS.HDOP);
  if (fix) {
    GPSOUT.printf("\"lat\":%.7f,\"lon\":%.7f,\"speed\":%.2f,",
                  GPS.latitudeDegrees, GPS.longitudeDegrees, GPS.speed * 0.514444);
  } else {
    GPSOUT.print("\"lat\":null,\"lon\":null,\"speed\":null,");
  }
  GPSOUT.printf("\"utc\":\"%02d:%02d:%02d.%03d\",\"bytes\":%lu,\"parsed\":%lu,",
                GPS.hour, GPS.minute, GPS.seconds, GPS.milliseconds, bytesRx, parsedOk);
  if (haveOrigin) {
    GPSOUT.printf("\"origin_lat\":%.7f,\"origin_lon\":%.7f,", originLat, originLon);
    if (fix) {
      GPSOUT.printf("\"dist\":%.3f}\n",
                    haversineM(originLat, originLon, GPS.latitudeDegrees, GPS.longitudeDegrees));
    } else {
      GPSOUT.print("\"dist\":null}\n");
    }
  } else {
    GPSOUT.print("\"origin_lat\":null,\"origin_lon\":null,\"dist\":null}\n");
  }
}

void startAveraging(AvgMode mode, unsigned long duration) {
  avgMode = mode;
  avgStart = millis();
  lastProgressMs = avgStart;
  avgDuration = duration;
  avgSumLat = avgSumLon = 0;
  avgN = 0;
}

void sendProgress() {
  GPSOUT.printf("{\"type\":\"progress\",\"mode\":\"%s\",\"elapsed_ms\":%lu,\"total_ms\":%lu,\"samples\":%ld}\n",
                avgMode == AVG_ORIGIN ? "origin" : "measure", millis() - avgStart, avgDuration, avgN);
}

void finishAveraging() {
  AvgMode mode = avgMode;
  avgMode = AVG_NONE;
  if (avgN == 0) {
    GPSOUT.printf("{\"type\":\"result\",\"mode\":\"%s\",\"ok\":false,\"samples\":0,"
                  "\"msg\":\"No good samples (no fix or HDOP too high). Try somewhere more open.\"}\n",
                  mode == AVG_ORIGIN ? "origin" : "measure");
    return;
  }
  double lat = avgSumLat / avgN, lon = avgSumLon / avgN;
  if (mode == AVG_ORIGIN) {
    originLat = lat;
    originLon = lon;
    haveOrigin = true;
    GPSOUT.printf("{\"type\":\"result\",\"mode\":\"origin\",\"ok\":true,\"lat\":%.7f,\"lon\":%.7f,\"samples\":%ld}\n",
                  lat, lon, avgN);
  } else {
    double gpsM = haversineM(originLat, originLon, lat, lon);
    GPSOUT.printf("{\"type\":\"result\",\"mode\":\"measure\",\"ok\":true,\"true_m\":%.3f,\"gps_m\":%.3f,"
                  "\"error_m\":%.3f,\"samples\":%ld,\"lat\":%.7f,\"lon\":%.7f}\n",
                  measureTrueM, gpsM, gpsM - measureTrueM, avgN, lat, lon);
  }
}

// Returns true if the command belonged to the GPS module
bool command(const String &cmd) {
  if (cmd == "origin") {
    startAveraging(AVG_ORIGIN, ORIGIN_AVG_MS);
    sendLog("Averaging origin for 30 s, don't move...");
  } else if (cmd.startsWith("measure")) {
    if (!haveOrigin) {
      sendLog("Set an origin first.");
      return true;
    }
    const char *arg = cmd.c_str() + 7;
    char *end;
    double m = strtod(arg, &end);
    if (end == arg || m < 0) {
      sendLog("Usage: measure <meters>");
      return true;
    }
    measureTrueM = m;
    startAveraging(AVG_MEASURE, MEASURE_AVG_MS);
    sendLog("Measuring point for 20 s, hold still...");
  } else if (cmd == "cancel") {
    if (avgMode != AVG_NONE) {
      GPSOUT.printf("{\"type\":\"result\",\"mode\":\"%s\",\"ok\":false,\"cancelled\":true,\"samples\":%ld}\n",
                    avgMode == AVG_ORIGIN ? "origin" : "measure", avgN);
      avgMode = AVG_NONE;
    } else {
      sendLog("Nothing to cancel.");
    }
  } else {
    return false;
  }
  return true;
}

void wiringCheck() {
  if (wiringChecked || millis() - startMs < WIRING_CHECK_MS) return;
  wiringChecked = true;
  if (bytesRx == 0) {
    sendLog("No data from GPS. Check power, GND, and that GPS TX goes to D7 / GPS RX goes to D6 (try swapping them).");
  } else if (parsedOk == 0) {
    sendLog("Getting data but can't parse it. Likely a baud rate mismatch.");
    rawBuf[rawIdx] = '\0';
    GPSOUT.printf("{\"type\":\"log\",\"msg\":\"Raw: %s\"}\n", rawLine[0] ? rawLine : rawBuf);
  }
}

void setup() {
  GPSSerial.setRxBufferSize(1024);

  // Module boots at 9600; switch it to 57600 so 5 Hz output fits
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  delay(100);
  GPS.sendCommand(PMTK_SET_BAUD_57600);
  delay(200);
  GPSSerial.end();
  GPSSerial.begin(FAST_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  GPS.sendCommand("$PMTK313,1*2E");   // enable SBAS search
  GPS.sendCommand("$PMTK301,2*2E");   // use WAAS corrections
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);

  startMs = millis();
  sendLog("GPS configured. Waiting for fix (go outside)...");
}

void start() {
  lastStatusMs = 0;
  sendLog("GPS started.");
}

void stop() {
  if (avgMode != AVG_NONE) command("cancel");   // tells the dashboard the run was cancelled
  sendLog("GPS stopped.");
}

// True if bytes are arriving from the GPS module
bool present() {
  return bytesRx > 0 && millis() - lastByteMs < DATA_TIMEOUT_MS;
}

// report = false keeps reading the GPS (so its data stays fresh and the UART
// buffer never overflows) but prints nothing
void service(bool report) {
  bool gotRMC = false;
  while (GPSSerial.available()) {
    bool isRMC;
    if (pollGPS(isRMC)) {
      if (avgMode != AVG_NONE && GPS.fix && GPS.HDOP <= MAX_HDOP) {
        avgSumLat += GPS.latitudeDegrees;
        avgSumLon += GPS.longitudeDegrees;
        avgN++;
      }
      if (isRMC) gotRMC = true;
    }
  }

  if (!report) return;

  unsigned long now = millis();
  if (gotRMC || now - lastStatusMs >= IDLE_STATUS_MS) sendStatus();

  if (avgMode != AVG_NONE) {
    if (now - avgStart >= avgDuration) {
      finishAveraging();
    } else if (now - lastProgressMs >= 1000) {
      lastProgressMs = now;
      sendProgress();
    }
  }

  wiringCheck();
}

}  // namespace gps

// =============================================================================
// Serial command router: reads one line and hands it to the right module.
// Single-character commands with no newline (what the IMU dashboard sends)
// are accepted after a short pause.
// =============================================================================
namespace control {

enum Module { LORA, ESPNOW, IMU, IMU2, GPS, COUNT };
const char *NAMES[COUNT] = {"lora", "espnow", "imu", "imu2", "gps"};
const bool ENABLED[COUNT] = {ENABLE_LORA, ENABLE_ESPNOW, ENABLE_IMU, ENABLE_IMU, ENABLE_GPS};
bool running[COUNT] = {false, false, false, false, false};
uint32_t lastStateMs = 0;

// One line the dashboard reads to show which modules are running and found
void printState() {
  lastStateMs = millis();
  SYSOUT.printf("STATE lora=%d espnow=%d imu=%d imu2=%d gps=%d lora_hw=%d imu_hw=%d imu2_hw=%d gps_hw=%d\n",
                running[LORA], running[ESPNOW], running[IMU], running[IMU2], running[GPS],
                lora::ok, imu1.ok, imu2.ok, gps::present());
}

void setRunning(int m, bool on) {
  if (!ENABLED[m] || running[m] == on) return;
  running[m] = on;
  switch (m) {
    case LORA:   on ? lora::start() : lora::stop(); break;
    case ESPNOW: on ? espnow_link::start() : espnow_link::stop(); break;
    case IMU:    on ? imu1.start() : imu1.stop(); break;
    case IMU2:   on ? imu2.start() : imu2.stop(); break;
    case GPS:    on ? gps::start() : gps::stop(); break;
  }
}

int find(const String &name) {
  for (int m = 0; m < COUNT; m++) if (name == NAMES[m]) return m;
  return -1;
}

// go / go <m> / stop / stop <m> / only <m> / state. Returns true if handled.
bool command(const String &cmd) {
  if (cmd == "state") { printState(); return true; }

  int sp = cmd.indexOf(' ');
  String verb = sp < 0 ? cmd : cmd.substring(0, sp);
  String arg = sp < 0 ? "all" : cmd.substring(sp + 1);
  arg.trim();
  if (verb != "go" && verb != "stop" && verb != "only") return false;

  int m = arg == "all" ? -1 : find(arg);
  if (arg != "all" && m < 0) {
    SYSOUT.println("Unknown module. Use: lora, espnow, imu, imu2, gps (or all)");
    return true;
  }
  if (verb == "only" && m < 0) {
    SYSOUT.println("Usage: only <module>");
    return true;
  }
  for (int i = 0; i < COUNT; i++) {
    if (verb == "go" && (m < 0 || i == m)) setRunning(i, true);
    if (verb == "stop" && (m < 0 || i == m)) setRunning(i, false);
    if (verb == "only") setRunning(i, i == m);
  }
  printState();
  return true;
}

void service() {
  if (millis() - lastStateMs >= 5000) printState();   // keeps a dashboard in sync
}

}  // namespace control

namespace commands {

char buf[96];
size_t len = 0;
uint32_t lastCharMs = 0;

void printHelp() {
  SYSOUT.println("Commands (one per line):");
  SYSOUT.println("  IMU:  0 = recalibrate (keep still), m = magnetometer cal, l = CSV logging, p = normal printout");
  SYSOUT.println("  IMU2: imu2 0 | imu2 l | imu2 p   (same commands for the second IMU)");
  SYSOUT.println("  GPS:  origin | measure <meters> | cancel");
  SYSOUT.println("  LORA: c <meters> = calibrate | n <exponent> | plot | text | status");
  SYSOUT.println("  RUN:  go | go <module> | stop | stop <module> | only <module> | state");
  SYSOUT.println("        (module = lora, espnow, imu, imu2, gps)");
  SYSOUT.println("  SYS:  help | info");
}

void printInfo() {
  SYSOUT.printf("LoRa node %d | LoRa %s | ESP-NOW %s | IMU (ICM-20948) %s | IMU 2 (ISM330DHCX) %s | GPS %s\n", NODE_ID,
                lora::ok ? "OK" : "not found", espnow_link::ok ? "OK" : "off",
                imu1.ok ? "OK" : "not found", imu2.ok ? "OK" : "not found", ENABLE_GPS ? "on" : "off");
  SYSOUT.print("MAC: ");
  SYSOUT.println(WiFi.macAddress());
}

void dispatch(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (control::command(cmd)) return;
  if (ENABLE_IMU && cmd.startsWith("imu2 ")) {
    String sub = cmd.substring(5);
    sub.trim();
    if (!imu2.command(sub)) printHelp();
    return;
  }
  if (ENABLE_IMU && imu1.command(cmd)) return;
  if (ENABLE_GPS && gps::command(cmd)) return;
  if (ENABLE_LORA && lora::command(cmd)) return;
  if (cmd == "info") {
    printInfo();
    return;
  }
  printHelp();
}

void service() {
  while (Serial.available()) {
    char c = Serial.read();
    lastCharMs = millis();
    if (c == '\n' || c == '\r') {
      if (len) {
        buf[len] = '\0';
        dispatch(String(buf));
        len = 0;
      }
    } else if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
  if (len && millis() - lastCharMs > 50) {   // no newline: treat a pause as the end
    buf[len] = '\0';
    dispatch(String(buf));
    len = 0;
  }
}

}  // namespace commands

// =============================================================================
void setup() {
  Serial.setTxBufferSize(4096);   // room for bursts of output from all modules
  Serial.setTxTimeoutMs(0);       // never stall the program if no computer is reading
  Serial.begin(115200);

  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) delay(10);

  SYSOUT.println("=== ME74 combined firmware: LoRa + ESP-NOW + IMU + GPS ===");

  if (ENABLE_ESPNOW) espnow_link::setup();
  if (ENABLE_LORA) lora::setup();
  if (ENABLE_GPS) gps::setup();
  if (ENABLE_IMU) {
    Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN);   // one I2C bus shared by both IMUs
    Wire.setClock(400000);
    imu1.setup();
    imu2.setup();
  }

  commands::printInfo();
  commands::printHelp();

  if (AUTO_START) {
    for (int m = 0; m < control::COUNT; m++) control::setRunning(m, true);
  } else {
    SYSOUT.println("Standby: send \"go\" to start everything, or \"go <module>\" for one.");
  }
  control::printState();
}

void loop() {
  using namespace control;
  commands::service();
  if (running[IMU]) imu1.service();
  if (running[IMU2]) imu2.service();
  if (ENABLE_GPS) gps::service(running[GPS]);   // always read, only print when running
  if (running[LORA]) lora::service();
  if (running[ESPNOW]) espnow_link::service();
  control::service();
}
