// =============================================================================
// ME74 Prototype 2: DRONE LOGGER firmware (XIAO ESP32-S3)
// =============================================================================
//
// Runs on the payload board on each drone, on battery, with no laptop attached.
// It saves everything to the board's own flash memory (LittleFS):
//   - signal strength (RSSI) of every node beacon it hears   12034,RSSI,A,D1,-67,9.5
//   - IMU at 50 Hz (same 15 fields as the old LOG line)      12052,IMU,D1,t_us,ax,...
//   - GPS at 5 Hz if a GPS is plugged in (with UTC time)     12040,GPS,D1,lat,lon,alt,sats,hdop,hhmmss.ss
// Each line starts with millis(). Each trial is its own file (/t001.csv,
// /t002.csv, ...), started and stopped with the B button. That's exactly the
// format the dashboard reads.
//
// It also radios a short report twice a second, so a node plugged into the
// laptop can show the flight live (optional).
//
// Flash with PlatformIO:  pio run -e drone1 -t upload   (drone2 for the other one)
// The first upload also needs the bigger storage partition (already set in
// platformio.ini). Uploading erases nothing else, but changing partitions
// wipes old logs, so download them first.
//
// ---------------------------------------------------------------- Wiring ----
// Same pins as ME74_combined:
//   LoRa RFM95:  SCK -> D8   MISO -> D9   MOSI -> D10   CS -> D3   RST -> D2   G0 -> D1
//   GPS:         GPS TX -> D7 (RX)   GPS RX -> D6 (TX)      (optional)
//   IMU:         SDA -> D4   SCL -> D5
//   All:         VIN/VCC -> 3V3, GND -> GND
//
// ------------------------------------------------------- How to use it ----
// The battery stays connected, so the board is always on. Powering up does NOT
// start logging; the B (BOOT) button starts and stops each trial.
//  1. Put the drone on the ground and press B once, then take your hand away.
//     LED solid on = IMU calibrating, keep it still (~6 s).
//     Then slow blink = logging.
//  2. Fly.
//  3. After landing, press B again: the file closes, LED goes back to idle.
//  4. Plug the board into the laptop. Dashboard: "Read log from drone"
//     (sends "dump"), then "Erase logs on drone".
//  LED: short blip every 3 s = on and idle (ready), solid = calibrating,
//       slow blink = logging, fast blink = problem (no IMU, no radio, memory full).
//  The R (reset) button reboots the board into idle; it doesn't start a trial.
//
// USB commands (115200, one per line):
//   status        what's running, file, free space
//   list          the saved files
//   dump [N]      print file N (default: the newest), then a line "END"
//   erase N       delete file N        eraseall    delete every file
//   start / stop  start a trial (new file + IMU recalibration) / stop it, same as B
//   imu           toggle a live IMU printout       0 = recalibrate the IMU
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <RH_RF95.h>
#include <Wire.h>
#include <Adafruit_ICM20X.h>
#include <Adafruit_ICM20948.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_GPS.h>
#include <LittleFS.h>
#include <stdarg.h>
#include <math.h>
#include <vector>

#ifndef DRONE_NUM
#define DRONE_NUM 1          // set by the PlatformIO environment (drone1 / drone2)
#endif
const uint8_t RADIO_ADDR = 100 + DRONE_NUM;   // nodes are 1..4, drones 101, 102
char DRONE_ID[4];                              // "D1" / "D2", filled in setup()

#define IMUOUT Serial
#define LED_PIN 21           // XIAO ESP32-S3 user LED, on when LOW
#define BUTTON_PIN 0         // XIAO ESP32-S3 "B" (BOOT) button, LOW when pressed

// =============================================================================
// LOG: lines are collected in RAM and written to flash by a separate task on the
// other CPU core, so a slow flash write never stalls the IMU or the radio.
// =============================================================================
namespace logbuf {

const size_t BUF = 16384;
char bufA[BUF], bufB[BUF];
char *fill = bufA;
size_t fillLen = 0;
char *pendBuf = nullptr;
size_t pendLen = 0;
volatile bool writerBusy = false;
TaskHandle_t writerTask = nullptr;
File file;
bool active = false;
int fileNum = 0;
uint32_t bytesWritten = 0, linesDropped = 0;
unsigned long lastSwap = 0;
bool memoryFull = false;

// Free space is measured by the writer task, never by the main loop while logging:
// LittleFS.usedBytes() walks the whole filesystem and, on the main loop, stalled the
// IMU for up to ~280 ms every 5 s. The main loop only reads these two values.
const size_t STOP_FREE_BYTES = 48 * 1024;     // stop cleanly before the flash fills up
volatile size_t freeCache = 0;                // free bytes as of the last check
volatile bool spaceLow = false;               // set by the writer, acted on in service()
unsigned long lastSpaceCheck = 0;             // writer task only

size_t freeBytes() { return LittleFS.totalBytes() - LittleFS.usedBytes(); }

void writerLoop(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (file && pendLen) {
      file.write((const uint8_t *)pendBuf, pendLen);
      file.flush();                       // a dead battery loses at most ~1 s
      bytesWritten += pendLen;
      if (millis() - lastSpaceCheck >= 5000) {   // same 5 s cadence as before, now off the main loop
        lastSpaceCheck = millis();
        size_t fr = freeBytes();
        freeCache = fr;
        if (fr < STOP_FREE_BYTES) spaceLow = true;
      }
    }
    writerBusy = false;
  }
}

bool swapBuffers() {
  if (writerBusy || !fillLen) return false;
  pendBuf = fill;
  pendLen = fillLen;
  fill = (fill == bufA) ? bufB : bufA;
  fillLen = 0;
  writerBusy = true;
  lastSwap = millis();
  xTaskNotifyGive(writerTask);
  return true;
}

void append(const char *s, size_t n) {
  if (!active) return;
  if (fillLen + n > BUF && !swapBuffers()) { linesDropped++; return; }
  memcpy(fill + fillLen, s, n);
  fillLen += n;
  if (fillLen > BUF * 3 / 4) swapBuffers();
}

// One log line: "<millis>,<text>\n"
void linef(const char *fmt, ...) {
  if (!active) return;
  char line[256];
  int n = snprintf(line, sizeof(line), "%lu,", (unsigned long)millis());
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(line) - 2) n = sizeof(line) - 2;
  line[n++] = '\n';
  append(line, n);
}

void waitIdle() { while (writerBusy) delay(1); }

int newestFile() {
  int best = 0;
  File root = LittleFS.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    int n = 0;
    if (sscanf(f.name(), "t%d.csv", &n) == 1 || sscanf(f.name(), "/t%d.csv", &n) == 1) best = max(best, n);
  }
  return best;
}
String pathOf(int n) { char p[16]; snprintf(p, sizeof(p), "/t%03d.csv", n); return String(p); }
// For status: while logging, the writer's last measurement (no LittleFS call on the main loop)
size_t freeForStatus() { return active ? freeCache : freeBytes(); }

bool start() {
  if (active) return true;
  size_t fr = freeBytes();                    // not logging yet, so the main loop may ask LittleFS
  if (fr < 64 * 1024) { memoryFull = true; Serial.println("# Memory full: download the logs, then eraseall"); return false; }
  freeCache = fr; spaceLow = false; lastSpaceCheck = millis();
  fileNum = newestFile() + 1;
  file = LittleFS.open(pathOf(fileNum), "w");
  if (!file) { Serial.println("# Could not create a log file"); return false; }
  fill = bufA; fillLen = 0; bytesWritten = 0; linesDropped = 0; memoryFull = false;
  active = true;
  lastSwap = millis();
  char hdr[80];
  int hn = snprintf(hdr, sizeof(hdr), "# ME74 P2 drone logger %s, file t%03d\n", DRONE_ID, fileNum);
  append(hdr, hn);
  Serial.printf("# Logging to %s\n", pathOf(fileNum).c_str());
  return true;
}

void stop() {
  if (!active) return;
  waitIdle(); swapBuffers(); waitIdle();
  active = false;
  file.close();
  Serial.printf("# Stopped. t%03d is %lu bytes (%lu lines dropped)\n", fileNum, (unsigned long)bytesWritten, (unsigned long)linesDropped);
}

void setup() {
  if (!LittleFS.begin(true)) Serial.println("# LittleFS mount failed");
  xTaskCreatePinnedToCore(writerLoop, "logwriter", 4096, nullptr, 1, &writerTask, 0);
}

void service() {
  if (!active) return;
  if (fillLen && millis() - lastSwap > 1000) swapBuffers();
  // Stop cleanly before the flash fills up (the writer task measured the free space)
  if (spaceLow) { append("# memory full, logging stopped\n", 31); stop(); memoryFull = true; }
}

}  // namespace logbuf

// Called by the IMU code at 50 Hz
void logImuSample(uint32_t tUs, float ax, float ay, float az, float gx, float gy, float gz,
                  float mx, float my, float mz, float rollDeg, float pitchDeg, float headDeg,
                  float dtMs, uint32_t overruns) {
  logbuf::linef("IMU,%s,%lu,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%lu",
                DRONE_ID, (unsigned long)tUs, ax, ay, az, gx, gy, gz, mx, my, mz, rollDeg, pitchDeg, headDeg,
                dtMs, (unsigned long)overruns);
}

// =============================================================================
// GPS (optional): Adafruit Ultimate GPS, same setup as ME74_combined.
// Logs every RMC sentence with a fix (5 per second) with the UTC time.
// =============================================================================
namespace gpsmod {

HardwareSerial GPSSerial(1);
Adafruit_GPS GPS(&GPSSerial);
const int RX_PIN = 44, TX_PIN = 43;   // D7, D6
unsigned long bytes = 0, lastByte = 0;
bool fix = false;
double lat = 0, lon = 0;

void setup() {
  GPSSerial.setRxBufferSize(1024);
  GPSSerial.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);   // module boots at 9600
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);
  delay(100);
  GPS.sendCommand(PMTK_SET_BAUD_57600);
  delay(200);
  GPSSerial.end();
  GPSSerial.begin(57600, SERIAL_8N1, RX_PIN, TX_PIN);
  delay(200);
  GPS.sendCommand("$PMTK313,1*2E");   // SBAS search
  GPS.sendCommand("$PMTK301,2*2E");   // WAAS corrections
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);
}

bool present() { return bytes > 0 && millis() - lastByte < 2000; }

void service() {
  while (GPSSerial.available()) {
    GPS.read();
    bytes++;
    lastByte = millis();
    if (!GPS.newNMEAreceived()) continue;
    char *s = GPS.lastNMEA();
    if (!GPS.parse(s) || !strstr(s, "RMC")) continue;
    fix = GPS.fix;
    if (!fix) continue;
    lat = GPS.latitudeDegrees;
    lon = GPS.longitudeDegrees;
    logbuf::linef("GPS,%s,%.7f,%.7f,%.1f,%d,%.2f,%02d%02d%02d.%02d", DRONE_ID, lat, lon, GPS.altitude,
                  (int)GPS.satellites, GPS.HDOP, GPS.hour, GPS.minute, GPS.seconds, GPS.milliseconds / 10);
  }
}

}  // namespace gpsmod

// =============================================================================
// RADIO: hears the node beacons (logs their RSSI) and sends a short report
// =============================================================================
namespace radio {

#define RFM95_CS  D3
#define RFM95_RST D2
#define RFM95_INT D1
#define LORA_FREQ 915.0
// Every board (nodes and drones) must use the same modem setting.
// 500 kHz / SF7 keeps each packet ~10 ms so lots of beacons fit; range is still well over 100 m.
#define LORA_MODEM RH_RF95::Bw500Cr45Sf128

// The radio is POLLED, not interrupt-driven. RadioHead normally handles DIO0 (D1) in a
// GPIO interrupt that does SPI transactions; SPI.beginTransaction() takes a mutex with a
// blocking wait, which asserts ("xQueueSemaphoreTake ... xTaskGetSchedulerState") when the
// interrupt lands while the log writer task is writing to flash. So the interrupt is
// detached after init and service() runs RadioHead's handler whenever DIO0 is high
// (DIO0 = RxDone in receive, TxDone in transmit; it stays high until the handler clears it).
class PolledRF95 : public RH_RF95 {
 public:
  using RH_RF95::RH_RF95;
  void poll() { handleInterrupt(); }
};

PolledRF95 rf95(RFM95_CS, RFM95_INT);
bool ok = false;
int lastRssi[5] = {0};
unsigned long lastHeard[5] = {0};
unsigned long nextReport = 2000, txStart = 0;
bool txBusy = false;

void setup() {
  SPI.begin(D8, D9, D10, RFM95_CS);
  pinMode(RFM95_RST, OUTPUT);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  digitalWrite(RFM95_RST, LOW);  delay(10);
  digitalWrite(RFM95_RST, HIGH); delay(10);
  if (!rf95.init() || rf95.getDeviceVersion() != 0x12 || !rf95.setFrequency(LORA_FREQ)) {
    Serial.println("# LoRa radio not found, check wiring");
    return;
  }
  detachInterrupt(digitalPinToInterrupt(RFM95_INT));   // polled from service() instead (see above)
  pinMode(RFM95_INT, INPUT);
  rf95.setModemConfig(LORA_MODEM);
  rf95.setTxPower(13, false);
  rf95.setThisAddress(RADIO_ADDR);
  rf95.setHeaderFrom(RADIO_ADDR);
  rf95.setModeRx();
  ok = true;
}

int recentRssi(int node) { return (node >= 1 && node <= 4 && lastHeard[node] && millis() - lastHeard[node] < 2000) ? lastRssi[node] : 0; }

void sendReport() {
  // R,<drone>,<ms>,<rssi A>,<rssi B>,<rssi C>,<rssi D>,<lat>,<lon>,<fix>   (0 = not heard)
  char msg[96];
  int n = snprintf(msg, sizeof(msg), "R,%s,%lu,%d,%d,%d,%d,%.7f,%.7f,%d", DRONE_ID, (unsigned long)millis(),
                   recentRssi(1), recentRssi(2), recentRssi(3), recentRssi(4),
                   gpsmod::fix ? gpsmod::lat : 0.0, gpsmod::fix ? gpsmod::lon : 0.0, gpsmod::fix ? 1 : 0);
  rf95.setHeaderTo(RH_BROADCAST_ADDRESS);
  if (rf95.send((uint8_t *)msg, n)) { txBusy = true; txStart = millis(); }
  else rf95.setModeRx();
}

void service() {
  if (!ok) return;
  if (digitalRead(RFM95_INT) == HIGH) rf95.poll();   // RX done / TX done: what the interrupt used to do
  if (txBusy) {                                      // finish a transmission without blocking
    if (rf95.mode() != RHGenericDriver::RHModeTx) txBusy = false;
    else if (millis() - txStart > 500) { txBusy = false; rf95.setModeIdle(); }
  }
  if (!txBusy && rf95.available()) {
    uint8_t buf[RH_RF95_MAX_MESSAGE_LEN];
    uint8_t len = sizeof(buf);
    if (rf95.recv(buf, &len)) {
      uint8_t from = rf95.headerFrom();
      if (from >= 1 && from <= 4 && len >= 1 && buf[0] == 'B') {   // a node beacon
        int rssi = rf95.lastRssi();
        lastRssi[from] = rssi;
        lastHeard[from] = millis();
        logbuf::linef("RSSI,%c,%s,%d,%.1f", 'A' + from - 1, DRONE_ID, rssi, (float)rf95.lastSNR());
      }
    }
  }
  if (!txBusy && (int32_t)(millis() - nextReport) >= 0) {
    nextReport = millis() + 450 + random(0, 100);    // random gap so the two drones don't keep colliding
    sendReport();
  }
}

}  // namespace radio

// =============================================================================
// IMU: ICM-20948 orientation, copied unchanged from ME74_combined (which took it
// from IMU_initialtest.cpp). Only the output at the end of service() changed:
// samples go to the log instead of USB.
// =============================================================================
// (ME74_combined notes:)
// Changes from the original: the 100 Hz loop and both calibrations no longer
// block (the board keeps serving the radios and GPS while you calibrate), the
// I2C pins moved to D4/D5, and a missing IMU no longer stops the whole board.
// =============================================================================
namespace imu {

// --- I2C pins (D4 = GPIO5 = SDA, D5 = GPIO6 = SCL) ---
#define IMU_SDA_PIN 5
#define IMU_SCL_PIN 6

// --- Timing ---
const int LOOP_HZ = 100;
const int PRINT_HZ = 10;
const int LOG_HZ = 50;

// --- Startup calibration ---
const float CAL_SECONDS = 3.0f;
const int SETTLE_MS = 1000;
const float GYRO_STILL_LIMIT = 0.05f;   // rad/s
const float ACCEL_STILL_LIMIT = 0.30f;  // m/s^2

const bool LEVEL_ACCEL_CAL = true;
const float LEVEL_TOLERANCE = 1.0f;     // m/s^2

const float ACCEL_BIAS_X_FIXED = -0.10805f;
const float ACCEL_BIAS_Y_FIXED = -0.11242f;
const float ACCEL_BIAS_Z_FIXED = +0.34704f;

// --- Magnetometer ---
const bool USE_MAGNETOMETER = false;
const float MAG_OFFSET_X_FIXED = -101.97f;
const float MAG_OFFSET_Y_FIXED = 115.34f;
const float MAG_OFFSET_Z_FIXED = 126.82f;
const float MAG_CAL_SECONDS = 20.0f;
const float DECLINATION_DEG = 0.0f;

// --- Complementary filter ---
const float TAU_TILT = 1.0f;
const float TAU_HEADING = 2.0f;
const float ACCEL_TRUST_BAND = 0.10f * 9.80665f;
const float MAG_MIN_UT = 20.0f;
const float MAG_MAX_UT = 70.0f;

const float GRAVITY = 9.80665f;

struct Vec3 {
  float x, y, z;
};

Adafruit_ICM20948 icm;
uint8_t icmAddress = 0x69;
bool ok = false;

Vec3 gyroBias = {0, 0, 0};
Vec3 accelBias = {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
Vec3 magOffset = {MAG_OFFSET_X_FIXED, MAG_OFFSET_Y_FIXED, MAG_OFFSET_Z_FIXED};

float roll = 0, pitch = 0, heading = 0;
bool filterStarted = false;

const uint32_t LOOP_PERIOD_US = 1000000UL / LOOP_HZ;
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
const int CAL_N = (int)(CAL_SECONDS * LOOP_HZ);
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
  icm.getEvent(&a, &g, &t, &m);

  accel.x = a.acceleration.x - accelBias.x;
  accel.y = a.acceleration.y - accelBias.y;
  accel.z = a.acceleration.z - accelBias.z;

  gyro.x = g.gyro.x - gyroBias.x;
  gyro.y = g.gyro.y - gyroBias.y;
  gyro.z = g.gyro.z - gyroBias.z;

  // Magnetometer Y and Z point the opposite way from the accel/gyro axes
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
  IMUOUT.println("Calibrating... keep the sensor still.");
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
    IMUOUT.println("Movement detected during calibration, trying again.");
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
    accelBias = {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
    if (LEVEL_ACCEL_CAL) IMUOUT.println("WARNING: sensor is not level (Z up), using fixed accel bias values.");
  }

  IMUOUT.printf("Gyro bias  (rad/s): %.5f  %.5f  %.5f\n", gyroBias.x, gyroBias.y, gyroBias.z);
  IMUOUT.printf("Accel bias (m/s^2): %.4f  %.4f  %.4f\n", accelBias.x, accelBias.y, accelBias.z);
  IMUOUT.println("Calibration done.");
  restartLoopTimer();
  state = RUNNING;
}

// ---- Hard-iron magnetometer calibration (rotate the sensor every way) ----
void startMagCal() {
  IMUOUT.println("Magnetometer calibration: slowly rotate the sensor through");
  IMUOUT.printf("every orientation (figure-8s, flip it over) for %.0f seconds...\n", MAG_CAL_SECONDS);
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
  IMUOUT.println("Done. Paste these into the MAG_OFFSET_*_FIXED constants:");
  IMUOUT.printf("  MAG_OFFSET_X_FIXED = %.2ff;\n", magOffset.x);
  IMUOUT.printf("  MAG_OFFSET_Y_FIXED = %.2ff;\n", magOffset.y);
  IMUOUT.printf("  MAG_OFFSET_Z_FIXED = %.2ff;\n", magOffset.z);
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
    heading = USE_MAGNETOMETER ? headingMag : 0;
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
  if (USE_MAGNETOMETER && fieldUT > MAG_MIN_UT && fieldUT < MAG_MAX_UT) {
    heading += (1 - alphaHead) * wrapPi(headingMag - heading);
  }

  roll = wrapPi(roll);
  heading = wrapPi(heading);
}

// Returns true if the command belonged to the IMU module
bool command(const String &cmd) {
  if (cmd == "imu") {
    logMode = !logMode;
  } else if (cmd == "0") {
    if (ok) startBiasCal();
  } else if (cmd == "m") {
    if (ok) startMagCal();
  } else {
    return false;
  }
  if (!ok && (cmd == "0" || cmd == "m")) IMUOUT.println("Failed to find ICM20948 chip! Check wiring.");
  return true;
}

void setup() {
  Wire.begin(IMU_SDA_PIN, IMU_SCL_PIN);
  Wire.setClock(400000);

  bool found = icm.begin_I2C(0x69, &Wire);
  if (!found) {
    icmAddress = 0x68;
    found = icm.begin_I2C(0x68, &Wire);
  }
  if (!found) {
    IMUOUT.println("Failed to find ICM20948 chip! Check wiring.");
    return;
  }
  IMUOUT.printf("ICM20948 found at 0x%02X\n", icmAddress);

  icm.setAccelRange(ICM20948_ACCEL_RANGE_2_G);
  icm.setGyroRange(ICM20948_GYRO_RANGE_250_DPS);
  icm.setGyroRateDivisor(4);    // 220 Hz
  icm.setAccelRateDivisor(4);   // 225 Hz
  icm.setMagDataRate(AK09916_MAG_DATARATE_100_HZ);
  icm.enableAccelDLPF(true, ICM20X_ACCEL_FREQ_23_9_HZ);
  setGyroLowPass(4);            // 23.9 Hz

  ok = true;
  settleUntilMs = millis() + SETTLE_MS;   // let the sensor settle, then calibrate
  state = SETTLING;
  restartLoopTimer();
}

// (Re)start: settle briefly, then run the still-calibration again
void start() {
  if (!ok) {
    IMUOUT.println("Failed to find ICM20948 chip! Check wiring.");
    return;
  }
  settleUntilMs = millis() + SETTLE_MS;
  state = SETTLING;
  restartLoopTimer();
}

void stop() {
  if (ok) IMUOUT.println("IMU stopped.");
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

  // Every LOG_HZ step: hand the sample to the logger (same 15 fields as the old LOG line)
  if (loopCount % (LOOP_HZ / LOG_HZ) == 0) {
    logImuSample(now, accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z, mag.x, mag.y, mag.z,
                 roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg, dt * 1000, overruns);
  }
  // Optional live printout over USB (command "imu" toggles it)
  if (logMode && loopCount % (LOOP_HZ / PRINT_HZ) == 0) {
    IMUOUT.printf("Ax:%.3f,Ay:%.3f,Az:%.3f,Roll:%.2f,Pitch:%.2f,Heading:%.1f,dt_ms:%.2f,Overruns:%lu\n",
                  accel.x, accel.y, accel.z, roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg,
                  dt * 1000, (unsigned long)overruns);
  }
}

}  // namespace imu

// =============================================================================
// TRIALS: the B button (or the USB start/stop commands) starts and stops a trial.
// Starting one opens a new log file and recalibrates the IMU, since the board may
// have been sitting on all day and the gyro bias drifts with temperature.
// =============================================================================
namespace trial {

const int HAND_AWAY_MS = 2000;   // extra wait after a button press so your hand is off the drone

void start(bool fromButton) {
  if (logbuf::active) return;
  if (!logbuf::start()) return;
  imu::start();                              // IMU lines start once it's calibrated
  if (fromButton) imu::settleUntilMs = millis() + HAND_AWAY_MS + imu::SETTLE_MS;
  Serial.println("# Trial started: keep the drone still until the LED starts blinking");
}

void stop() {
  if (!logbuf::active) return;
  logbuf::stop();
  Serial.println("# Trial stopped");
}

void toggle(bool fromButton) { if (logbuf::active) stop(); else start(fromButton); }

}  // namespace trial

// One action per press, debounced. GPIO0 is only a boot-mode pin while the board
// is resetting, so reading it while running is fine.
namespace button {

bool raw = true, stable = true;
unsigned long changedAt = 0, lastAction = 0;

void setup() { pinMode(BUTTON_PIN, INPUT_PULLUP); }

void service() {
  bool r = digitalRead(BUTTON_PIN);
  unsigned long now = millis();
  if (r != raw) { raw = r; changedAt = now; }
  if (r != stable && now - changedAt > 30) {
    stable = r;
    if (!stable && now - lastAction > 1000) {   // pressed; ignore a second press within 1 s
      lastAction = now;
      trial::toggle(true);
    }
  }
}

}  // namespace button

// =============================================================================
// USB commands
// =============================================================================
namespace commands {

String line;

void dumpFile(int n) {
  if (n <= 0) n = logbuf::newestFile();
  logbuf::stop();
  File f = n > 0 ? LittleFS.open(logbuf::pathOf(n), "r") : File();
  if (!f) { Serial.println("# No log file to send"); Serial.println("END"); return; }
  Serial.setTxTimeoutMs(2000);                // wait for the laptop instead of dropping data
  uint8_t buf[1024];
  bool lastWasNewline = true;
  while (f.available()) {
    size_t k = f.read(buf, sizeof(buf));
    Serial.write(buf, k);
    if (k) lastWasNewline = buf[k - 1] == '\n';
  }
  f.close();
  if (!lastWasNewline) Serial.println();
  Serial.println("END");
  Serial.flush();
  Serial.setTxTimeoutMs(0);
}

void list() {
  logbuf::waitIdle();
  File root = LittleFS.open("/");
  int count = 0;
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    Serial.printf("# %s  %lu bytes\n", f.name(), (unsigned long)f.size());
    count++;
  }
  Serial.printf("# %d file(s), %lu KB free of %lu KB\n", count, (unsigned long)(logbuf::freeBytes() / 1024),
                (unsigned long)(LittleFS.totalBytes() / 1024));
}

void status() {
  Serial.printf("# Drone %s (radio address %d)\n", DRONE_ID, RADIO_ADDR);
  Serial.printf("# Logging: %s", logbuf::active ? "yes" : "no (press B to start a trial)");
  if (logbuf::active) Serial.printf(", t%03d, %lu bytes, %lu lines dropped", logbuf::fileNum, (unsigned long)logbuf::bytesWritten, (unsigned long)logbuf::linesDropped);
  Serial.printf("\n# Free memory: %lu KB%s\n", (unsigned long)(logbuf::freeForStatus() / 1024), logbuf::memoryFull ? " (FULL)" : "");
  Serial.printf("# Radio: %s", radio::ok ? "ok" : "NOT FOUND");
  for (int k = 1; k <= 4; k++) if (radio::recentRssi(k)) Serial.printf(", node %c %d dBm", 'A' + k - 1, radio::recentRssi(k));
  Serial.printf("\n# IMU: %s\n", !imu::ok ? "NOT FOUND" : imu::state == imu::RUNNING ? "running" : "calibrating (keep still)");
  Serial.printf("# GPS: %s\n", !gpsmod::present() ? "no data" : gpsmod::fix ? "fix" : "no fix yet");
}

void run(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  if (cmd == "status") status();
  else if (cmd == "list") list();
  else if (cmd == "dump") dumpFile(0);
  else if (cmd.startsWith("dump ")) dumpFile(cmd.substring(5).toInt());
  else if (cmd == "start") trial::start(false);
  else if (cmd == "stop") trial::stop();
  else if (cmd == "eraseall") {
    logbuf::stop();
    File root = LittleFS.open("/");
    std::vector<String> names;
    for (File f = root.openNextFile(); f; f = root.openNextFile()) names.push_back(String("/") + f.name());
    for (auto &p : names) LittleFS.remove(p);
    logbuf::memoryFull = false;
    Serial.printf("# Erased %d file(s)\n", (int)names.size());
  } else if (cmd.startsWith("erase ")) {
    int n = cmd.substring(6).toInt();
    if (logbuf::active && n == logbuf::fileNum) logbuf::stop();
    Serial.println(LittleFS.remove(logbuf::pathOf(n)) ? "# Erased" : "# No such file");
  } else if (!imu::command(cmd)) {
    Serial.println("# Commands: status, list, dump [N], erase N, eraseall, start, stop, imu, 0");
  }
}

void service() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { run(line); line = ""; }
    else if (line.length() < 80) line += c;
  }
}

}  // namespace commands

// =============================================================================
void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);                 // never stall when no laptop is listening
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  snprintf(DRONE_ID, sizeof(DRONE_ID), "D%d", DRONE_NUM);

  logbuf::setup();
  radio::setup();
  imu::setup();
  gpsmod::setup();
  button::setup();

  // The battery is always connected, so powering up never starts a file.
  // Press B (or send "start") to begin a trial.
  Serial.printf("# ME74 P2 drone %s ready, not logging. Press B to start a trial, or type status, list, dump, start.\n", DRONE_ID);
}

void loop() {
  radio::service();
  imu::service();
  gpsmod::service();
  logbuf::service();
  commands::service();
  button::service();

  // LED: fast blink = problem, solid = trial started but IMU still calibrating,
  // slow blink = logging, short blip every 3 s = on and idle
  bool problem = !imu::ok || !radio::ok || logbuf::memoryFull;
  unsigned long ms = millis();
  bool on;
  if (problem) on = (ms % 200) < 100;
  else if (logbuf::active && imu::state != imu::RUNNING) on = true;
  else if (logbuf::active) on = (ms % 1000) < 500;
  else on = (ms % 3000) < 60;
  digitalWrite(LED_PIN, on ? LOW : HIGH);
}
