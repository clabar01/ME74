// =============================================================================
// ME74 radios: LoRa + ESP-NOW on one XIAO ESP32-S3
// =============================================================================
//
// Just the two radio links from ME74_combined (both need two boards):
//   LORA    RFM95 LoRa link, RSSI-based distance        (from transceiver.ino)
//   ESPNOW  ESP-NOW link, RSSI + packet loss            (from espnow-rssi)
//
// Flash the SAME firmware on both boards: PlatformIO environment "node1" on one
// board and "node2" on the other (only the LoRa node number differs).
// If the LoRa radio isn't connected, ESP-NOW keeps running on its own.
//
// ---------------------------------------------------------------- Wiring ----
//   LoRa RFM95:  SCK -> D8   MISO -> D9   MOSI -> D10   CS -> D3
//                RST -> D2   G0/INT -> D1   VIN -> 3V3   GND -> GND
//   ESP-NOW uses the ESP32's built-in Wi-Fi radio: nothing to wire.
//
// ---------------------------------------------------------- Serial output ---
// Every line starts with [LORA], [ESPNOW] or [SYS]; after the tag it's exactly
// what the original sketches printed. ME74_radio_dashboard.html reads this.
//
// -------------------------------------------------------- Serial commands ---
//   LORA:   c <meters> (calibrate)   n <exponent>   plot   text   status
//   RUN:    go | go lora | go espnow | stop | stop lora | stop espnow
//           only lora | only espnow | state
//   SYSTEM: help   info
// At power-up both radios start on their own (AUTO_START), so a board on a
// battery with no computer attached works as the other end of both links.
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <math.h>

// LoRa node number: set by the PlatformIO environment (node1 / node2)
#ifndef NODE_ID
#define NODE_ID 1
#endif

// Start both radios at power-up. Set to 0 to wait for "go" commands instead.
#define AUTO_START 1

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
// Run control: go / stop / only / state
// =============================================================================
namespace control {

enum Module { LORA, ESPNOW, COUNT };
const char *NAMES[COUNT] = {"lora", "espnow"};
bool running[COUNT] = {false, false};
uint32_t lastStateMs = 0;

// One line the dashboard reads to show which radios are running and found
void printState() {
  lastStateMs = millis();
  SYSOUT.printf("STATE lora=%d espnow=%d lora_hw=%d\n", running[LORA], running[ESPNOW], lora::ok);
}

void setRunning(int m, bool on) {
  if (running[m] == on) return;
  running[m] = on;
  if (m == LORA) on ? lora::start() : lora::stop();
  else           on ? espnow_link::start() : espnow_link::stop();
}

int find(const String &name) {
  for (int m = 0; m < COUNT; m++) if (name == NAMES[m]) return m;
  return -1;
}

bool command(const String &cmd) {
  if (cmd == "state") { printState(); return true; }

  int sp = cmd.indexOf(' ');
  String verb = sp < 0 ? cmd : cmd.substring(0, sp);
  String arg = sp < 0 ? "all" : cmd.substring(sp + 1);
  arg.trim();
  if (verb != "go" && verb != "stop" && verb != "only") return false;

  int m = arg == "all" ? -1 : find(arg);
  if (arg != "all" && m < 0) {
    SYSOUT.println("Unknown module. Use: lora, espnow (or all)");
    return true;
  }
  if (verb == "only" && m < 0) {
    SYSOUT.println("Usage: only <lora|espnow>");
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

// =============================================================================
// Serial command router
// =============================================================================
namespace commands {

char buf[96];
size_t len = 0;
uint32_t lastCharMs = 0;

void printHelp() {
  SYSOUT.println("Commands (one per line):");
  SYSOUT.println("  LORA: c <meters> = calibrate | n <exponent> | plot | text | status");
  SYSOUT.println("  RUN:  go | go <lora|espnow> | stop | stop <lora|espnow> | only <lora|espnow> | state");
  SYSOUT.println("  SYS:  help | info");
}

void printInfo() {
  SYSOUT.printf("LoRa node %d | LoRa %s | ESP-NOW %s\n", NODE_ID,
                lora::ok ? "OK" : "not found", espnow_link::ok ? "OK" : "off");
  SYSOUT.print("MAC: ");
  SYSOUT.println(WiFi.macAddress());
}

void dispatch(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;
  if (control::command(cmd)) return;
  if (lora::command(cmd)) return;
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
  Serial.setTxBufferSize(4096);
  Serial.setTxTimeoutMs(0);       // never stall if no computer is reading
  Serial.begin(115200);

  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) delay(10);

  SYSOUT.println("=== ME74 radios: LoRa + ESP-NOW ===");
  espnow_link::setup();
  lora::setup();

  commands::printInfo();
  commands::printHelp();

  if (AUTO_START) {
    for (int m = 0; m < control::COUNT; m++) control::setRunning(m, true);
  } else {
    SYSOUT.println("Standby: send \"go\" to start both radios, or \"go lora\" / \"go espnow\".");
  }
  control::printState();
}

void loop() {
  commands::service();
  if (control::running[control::LORA]) lora::service();
  if (control::running[control::ESPNOW]) espnow_link::service();
  control::service();
}
