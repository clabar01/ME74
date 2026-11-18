// =============================================================================
// ME74 Prototype 2: NODE BEACON firmware (XIAO ESP32-S3 + RFM95 LoRa)
// =============================================================================
//
// Runs on each stationary node (A, B, C...). It saves nothing. It does two things:
//
//  1. Broadcasts a tiny beacon about 5 times a second. The drones hear it and
//     log its signal strength (RSSI): that's what the dashboard turns into distance.
//
//  2. Prints whatever it hears over USB, in the dashboard's log format, so a node
//     plugged into the laptop shows the flight live (optional):
//       12034,RSSI,A,D1,-67          drone D1 heard node A at -67 dBm (from the drone's report)
//       12034,GPS,D1,42.40751,-71.11904,,,,   the drone's GPS (from its report)
//       12034,RSSI,A,B,-58           this node heard node B (node-to-node, for calibration)
//       12034,LINK,A,D1,-66,8.5      this node heard drone D1's report directly
//     On battery with no laptop this output just goes nowhere, which is fine.
//
// Flash with PlatformIO:  pio run -e node1 -t upload   (node2 for B, node3 for C)
//
// Wiring (same as ME74_combined):
//   LoRa RFM95:  SCK -> D8   MISO -> D9   MOSI -> D10   CS -> D3   RST -> D2   G0 -> D1
//   VIN/VCC -> 3V3, GND -> GND
// LED: short blink on every beacon. Fast blinking = radio not found.
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>

#ifndef NODE_NUM
#define NODE_NUM 1           // set by the PlatformIO environment (node1 = A, node2 = B, ...)
#endif
const char NODE_LETTER = 'A' + NODE_NUM - 1;

#define RFM95_CS  D3
#define RFM95_RST D2
#define RFM95_INT D1
#define LORA_FREQ 915.0
// Must match the drone firmware. 500 kHz / SF7 keeps each beacon ~10 ms.
#define LORA_MODEM RH_RF95::Bw500Cr45Sf128
#define LED_PIN 21           // XIAO ESP32-S3 user LED, on when LOW

const unsigned long BEACON_MS = 200;   // ~5 beacons a second
const unsigned long JITTER_MS = 40;    // random +/- so two nodes don't keep colliding

RH_RF95 rf95(RFM95_CS, RFM95_INT);
bool ok = false, txBusy = false;
unsigned long nextBeacon = 0, txStart = 0, ledOffAt = 0;
uint32_t seq = 0, sent = 0, heard = 0;

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);                 // never stall when no laptop is listening
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);
  randomSeed(esp_random());

  SPI.begin(D8, D9, D10, RFM95_CS);
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  if (!rf95.init() || rf95.getDeviceVersion() != 0x12 || !rf95.setFrequency(LORA_FREQ)) {
    Serial.println("# LoRa radio not found, check wiring");
    return;
  }
  rf95.setModemConfig(LORA_MODEM);
  rf95.setTxPower(13, false);
  rf95.setThisAddress(NODE_NUM);
  rf95.setHeaderFrom(NODE_NUM);
  rf95.setModeRx();
  ok = true;
  nextBeacon = millis() + random(0, BEACON_MS);
  Serial.printf("# ME74 P2 node %c ready (radio address %d)\n", NODE_LETTER, NODE_NUM);
}

void sendBeacon() {
  // 'B' + sequence number. The drones only need to know who sent it (the header).
  uint8_t msg[5] = {'B', (uint8_t)(seq >> 24), (uint8_t)(seq >> 16), (uint8_t)(seq >> 8), (uint8_t)seq};
  seq++;
  rf95.setHeaderTo(RH_BROADCAST_ADDRESS);
  if (rf95.send(msg, sizeof(msg))) { txBusy = true; txStart = millis(); sent++; digitalWrite(LED_PIN, LOW); ledOffAt = millis() + 15; }
  else rf95.setModeRx();
}

// A drone's report: R,<drone>,<ms>,<rssi A>,<rssi B>,<rssi C>,<rssi D>,<lat>,<lon>,<fix>
void printReport(const char *s, int rssi, float snr) {
  char drone[8] = "";
  unsigned long ms = 0;
  int r[4] = {0, 0, 0, 0}, fix = 0;
  double lat = 0, lon = 0;
  if (sscanf(s, "R,%7[^,],%lu,%d,%d,%d,%d,%lf,%lf,%d", drone, &ms, &r[0], &r[1], &r[2], &r[3], &lat, &lon, &fix) < 7) return;
  unsigned long now = millis();
  for (int k = 0; k < 4; k++) if (r[k]) Serial.printf("%lu,RSSI,%c,%s,%d\n", now, 'A' + k, drone, r[k]);
  if (fix) Serial.printf("%lu,GPS,%s,%.7f,%.7f\n", now, drone, lat, lon);
  Serial.printf("%lu,LINK,%c,%s,%d,%.1f\n", now, NODE_LETTER, drone, rssi, snr);
}

void loop() {
  if (!ok) { digitalWrite(LED_PIN, (millis() % 200) < 100 ? LOW : HIGH); return; }
  if (ledOffAt && (int32_t)(millis() - ledOffAt) >= 0) { digitalWrite(LED_PIN, HIGH); ledOffAt = 0; }

  if (txBusy) {                                       // finish a transmission without blocking
    if (rf95.mode() != RHGenericDriver::RHModeTx) txBusy = false;
    else if (millis() - txStart > 500) { txBusy = false; rf95.setModeIdle(); }
  }

  if (!txBusy && rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN + 1];
    uint8_t len = RH_RF95_MAX_MESSAGE_LEN;
    if (rf95.recv(buf, &len)) {
      heard++;
      uint8_t from = rf95.headerFrom();
      int rssi = rf95.lastRssi();
      float snr = rf95.lastSNR();
      buf[len] = 0;
      if (from >= 1 && from <= 4 && from != NODE_NUM && buf[0] == 'B')
        Serial.printf("%lu,RSSI,%c,%c,%d,%.1f\n", millis(), NODE_LETTER, 'A' + from - 1, rssi, snr);
      else if (from > 100 && buf[0] == 'R')
        printReport((const char *)buf, rssi, snr);
    }
  }

  if (!txBusy && (int32_t)(millis() - nextBeacon) >= 0) {
    nextBeacon = millis() + BEACON_MS - JITTER_MS + random(0, 2 * JITTER_MS + 1);
    sendBeacon();
  }

  // "status" over USB
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      line.trim();
      if (line == "status") Serial.printf("# Node %c: %lu beacons sent, %lu packets heard\n", NODE_LETTER, (unsigned long)sent, (unsigned long)heard);
      line = "";
    } else if (line.length() < 40) line += c;
  }
}
