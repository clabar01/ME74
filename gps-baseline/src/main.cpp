#include <Arduino.h>
// gps_baseline - Adafruit Ultimate GPS Breakout on ESP32, streaming JSON for dashboard.html
//
// Wiring (GPS -> ESP32):
//   VIN -> 3V3, GND -> GND, GPS TX -> ESP32 RX pin, GPS RX -> ESP32 TX pin
//
// Serial output (115200): one JSON object per line, each with a "type" field:
//   status   - every RMC sentence (5 Hz), or once per second if nothing is parsing
//   progress - once per second during an averaging run
//   result   - end of an "origin" or "measure" run
//   log      - messages / warnings
//
// Serial commands (newline terminated):
//   origin            average 30 s (HDOP <= 2) and set origin
//   measure <meters>  average 20 s (HDOP <= 2), report distance vs tape distance
//   cancel            stop an averaging run
//
// Go outside. Red FIX LED blinks every 1 s while searching, every 15 s once locked.

#include <Adafruit_GPS.h>
#include <math.h>

// ---- pick your board's pins ----
// Generic ESP32 dev board (uncomment these, comment out the three below):
// #define GPS_UART 2
// #define GPS_RX_PIN 16
// #define GPS_TX_PIN 17
// Seeed XIAO ESP32S3:
#define GPS_UART 1
#define GPS_RX_PIN 44   // D7
#define GPS_TX_PIN 43   // D6

#define FAST_BAUD 57600
#define ORIGIN_AVG_MS 30000UL
#define MEASURE_AVG_MS 20000UL
#define MAX_HDOP 2.0

#define WIRING_CHECK_MS 5000UL   // one-time wiring warning after this long
#define DATA_TIMEOUT_MS 2000UL   // no bytes this long -> no_data
#define PARSE_TIMEOUT_MS 3000UL  // bytes but no parse this long -> bad_baud
#define IDLE_STATUS_MS 1000UL    // status rate when nothing is parsing

HardwareSerial GPSSerial(GPS_UART);
Adafruit_GPS GPS(&GPSSerial);

double originLat = 0, originLon = 0;
bool haveOrigin = false;

// Wiring check counters
unsigned long bytesRx = 0;
unsigned long parsedOk = 0;
unsigned long lastByteMs = 0;
unsigned long lastParseMs = 0;
bool wiringChecked = false;

// Last raw line seen on the GPS UART (kept separately so garbage is visible too)
#define RAW_LINE_LEN 100
char rawBuf[RAW_LINE_LEN];
int rawIdx = 0;
char rawLine[RAW_LINE_LEN] = "";

// Averaging run
enum AvgMode { AVG_NONE, AVG_ORIGIN, AVG_MEASURE };
AvgMode avgMode = AVG_NONE;
unsigned long avgStart = 0, avgDuration = 0, lastProgressMs = 0;
double avgSumLat = 0, avgSumLon = 0, measureTrueM = 0;
long avgN = 0;

unsigned long lastStatusMs = 0;

// Incoming command line
#define CMD_LEN 64
char cmdBuf[CMD_LEN];
int cmdIdx = 0;

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
    // '.' for unprintables, and keep quotes/backslashes out so the line is JSON-safe
    rawBuf[rawIdx++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '.';
  }
}

void sendLog(const char *msg) {
  Serial.printf("{\"type\":\"log\",\"msg\":\"%s\"}\n", msg);
}

// Read serial and parse any complete sentence. Returns true on a new parsed sentence.
// isRMC is set when that sentence was an RMC (used to pace status lines at the fix rate).
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
    bool ok = GPS.parse(nmea);
    if (ok) {
      parsedOk++;
      lastParseMs = millis();
    }
    return ok;
  }
  return false;
}

const char *gpsState() {
  unsigned long now = millis();
  if (bytesRx == 0 || now - lastByteMs > DATA_TIMEOUT_MS) return "no_data";
  if (parsedOk == 0 ? now > PARSE_TIMEOUT_MS : now - lastParseMs > PARSE_TIMEOUT_MS) return "bad_baud";
  if (parsedOk == 0) return "searching";  // first few seconds of data
  return GPS.fix ? "fix" : "searching";
}

void sendStatus() {
  lastStatusMs = millis();
  bool fix = GPS.fix && parsedOk > 0;
  Serial.printf("{\"type\":\"status\",\"state\":\"%s\",\"sats\":%d,\"hdop\":%.2f,",
                gpsState(), (int)GPS.satellites, GPS.HDOP);
  if (fix) {
    Serial.printf("\"lat\":%.7f,\"lon\":%.7f,\"speed\":%.2f,",
                  GPS.latitudeDegrees, GPS.longitudeDegrees, GPS.speed * 0.514444);  // knots -> m/s
  } else {
    Serial.print("\"lat\":null,\"lon\":null,\"speed\":null,");
  }
  Serial.printf("\"utc\":\"%02d:%02d:%02d.%03d\",\"bytes\":%lu,\"parsed\":%lu,",
                GPS.hour, GPS.minute, GPS.seconds, GPS.milliseconds, bytesRx, parsedOk);
  if (haveOrigin) {
    Serial.printf("\"origin_lat\":%.7f,\"origin_lon\":%.7f,", originLat, originLon);
    if (fix) {
      Serial.printf("\"dist\":%.3f}\n",
                    haversineM(originLat, originLon, GPS.latitudeDegrees, GPS.longitudeDegrees));
    } else {
      Serial.print("\"dist\":null}\n");
    }
  } else {
    Serial.print("\"origin_lat\":null,\"origin_lon\":null,\"dist\":null}\n");
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
  Serial.printf("{\"type\":\"progress\",\"mode\":\"%s\",\"elapsed_ms\":%lu,\"total_ms\":%lu,\"samples\":%ld}\n",
                avgMode == AVG_ORIGIN ? "origin" : "measure", millis() - avgStart, avgDuration, avgN);
}

void finishAveraging() {
  AvgMode mode = avgMode;
  avgMode = AVG_NONE;
  if (avgN == 0) {
    Serial.printf("{\"type\":\"result\",\"mode\":\"%s\",\"ok\":false,\"samples\":0,"
                  "\"msg\":\"No good samples (no fix or HDOP too high). Try somewhere more open.\"}\n",
                  mode == AVG_ORIGIN ? "origin" : "measure");
    return;
  }
  double lat = avgSumLat / avgN, lon = avgSumLon / avgN;
  if (mode == AVG_ORIGIN) {
    originLat = lat;
    originLon = lon;
    haveOrigin = true;
    Serial.printf("{\"type\":\"result\",\"mode\":\"origin\",\"ok\":true,\"lat\":%.7f,\"lon\":%.7f,\"samples\":%ld}\n",
                  lat, lon, avgN);
  } else {
    double gpsM = haversineM(originLat, originLon, lat, lon);
    Serial.printf("{\"type\":\"result\",\"mode\":\"measure\",\"ok\":true,\"true_m\":%.3f,\"gps_m\":%.3f,"
                  "\"error_m\":%.3f,\"samples\":%ld,\"lat\":%.7f,\"lon\":%.7f}\n",
                  measureTrueM, gpsM, gpsM - measureTrueM, avgN, lat, lon);
  }
}

void handleCommand(char *cmd) {
  while (*cmd == ' ') cmd++;
  if (strcmp(cmd, "origin") == 0) {
    startAveraging(AVG_ORIGIN, ORIGIN_AVG_MS);
    sendLog("Averaging origin for 30 s, don't move...");
  } else if (strncmp(cmd, "measure", 7) == 0) {
    if (!haveOrigin) {
      sendLog("Set an origin first.");
      return;
    }
    char *end;
    double m = strtod(cmd + 7, &end);
    if (end == cmd + 7 || m < 0) {
      sendLog("Usage: measure <meters>");
      return;
    }
    measureTrueM = m;
    startAveraging(AVG_MEASURE, MEASURE_AVG_MS);
    sendLog("Measuring point for 20 s, hold still...");
  } else if (strcmp(cmd, "cancel") == 0) {
    if (avgMode != AVG_NONE) {
      Serial.printf("{\"type\":\"result\",\"mode\":\"%s\",\"ok\":false,\"cancelled\":true,\"samples\":%ld}\n",
                    avgMode == AVG_ORIGIN ? "origin" : "measure", avgN);
      avgMode = AVG_NONE;
    } else {
      sendLog("Nothing to cancel.");
    }
  } else if (*cmd) {
    sendLog("Unknown command. Use: origin, measure <meters>, cancel");
  }
}

void pollCommands() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdIdx] = '\0';
      if (cmdIdx > 0) handleCommand(cmdBuf);
      cmdIdx = 0;
    } else if (cmdIdx < CMD_LEN - 1) {
      cmdBuf[cmdIdx++] = c;
    }
  }
}

void wiringCheck() {
  if (wiringChecked || millis() < WIRING_CHECK_MS) return;
  wiringChecked = true;
  if (bytesRx == 0) {
    sendLog("No data from GPS. Check power, GND, and that GPS TX goes to D7 / GPS RX goes to D6 (try swapping them).");
  } else if (parsedOk == 0) {
    sendLog("Getting data but can't parse it. Likely a baud rate mismatch.");
    rawBuf[rawIdx] = '\0';  // in case no full line has arrived yet
    Serial.printf("{\"type\":\"log\",\"msg\":\"Raw: %s\"}\n", rawLine[0] ? rawLine : rawBuf);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  GPSSerial.setRxBufferSize(1024);  // headroom if a Serial write stalls

  // Module boots at 9600
  GPSSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  GPS.sendCommand(PMTK_SET_NMEA_OUTPUT_RMCGGA);  // only RMC + GGA
  delay(100);

  // Bump baud so 5 Hz output fits comfortably
  GPS.sendCommand(PMTK_SET_BAUD_57600);
  delay(200);
  GPSSerial.end();
  GPSSerial.begin(FAST_BAUD, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);
  delay(200);

  GPS.sendCommand("$PMTK313,1*2E");  // enable SBAS search
  GPS.sendCommand("$PMTK301,2*2E");  // use WAAS corrections
  GPS.sendCommand(PMTK_SET_NMEA_UPDATE_5HZ);

  sendLog("GPS configured. Waiting for fix (go outside)...");
}

void loop() {
  pollCommands();

  // Drain everything the UART has so far
  bool gotRMC = false;
  do {
    bool isRMC;
    if (pollGPS(isRMC)) {
      if (avgMode != AVG_NONE && GPS.fix && GPS.HDOP <= MAX_HDOP) {
        avgSumLat += GPS.latitudeDegrees;
        avgSumLon += GPS.longitudeDegrees;
        avgN++;
      }
      if (isRMC) gotRMC = true;
    }
  } while (GPSSerial.available());

  unsigned long now = millis();

  // One status line per fix update (RMC), or a heartbeat when nothing is parsing
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
