// gps_baseline.ino - Adafruit Ultimate GPS Breakout on ESP32 (Arduino IDE)
//
// Setup:
//   1. Arduino IDE > Settings > Additional Boards Manager URLs, add:
//      https://espressif.github.io/arduino-esp32/package_esp32_index.json
//   2. Boards Manager: install "esp32 by Espressif Systems"
//   3. Library Manager: install "Adafruit GPS Library"
//   4. Tools > Board: pick your exact board (e.g. "XIAO_ESP32S3" or "ESP32 Dev Module")
//   5. Serial Monitor at 115200 baud
//
// Wiring (GPS -> ESP32):
//   VIN -> 3V3, GND -> GND, GPS TX -> ESP32 RX pin, GPS RX -> ESP32 TX pin
//
// Go outside. Red FIX LED blinks every 1 s while searching, every 15 s once locked.

#include <Adafruit_GPS.h>
#include <math.h>

// ---- pick your board's pins ----
// Generic ESP32 dev board:
#define GPS_UART 2
#define GPS_RX_PIN 16
#define GPS_TX_PIN 17
// Seeed XIAO ESP32S3 (uncomment these, comment out the three above):
// #define GPS_UART 1
// #define GPS_RX_PIN 44   // D7
// #define GPS_TX_PIN 43   // D6

#define FAST_BAUD 57600
#define ORIGIN_AVG_MS 30000UL
#define MAX_HDOP 2.0

HardwareSerial GPSSerial(GPS_UART);
Adafruit_GPS GPS(&GPSSerial);

double originLat = 0, originLon = 0;
bool haveOrigin = false;

double haversineM(double lat1, double lon1, double lat2, double lon2) {
  const double R = 6371008.8;
  double p1 = radians(lat1), p2 = radians(lat2);
  double dp = p2 - p1, dl = radians(lon2 - lon1);
  double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
  return 2 * R * asin(sqrt(a));
}

// Read serial and parse any complete sentence. Returns true on a new parsed sentence.
bool pollGPS() {
  GPS.read();
  if (GPS.newNMEAreceived()) {
    return GPS.parse(GPS.lastNMEA());
  }
  return false;
}

void setup() {
  Serial.begin(115200);
  delay(1000);

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

  Serial.println("Waiting for fix (go outside)...");
  unsigned long lastPrint = 0;
  while (!GPS.fix) {
    pollGPS();
    if (millis() - lastPrint > 2000) {
      lastPrint = millis();
      Serial.printf("searching... sats=%d\n", (int)GPS.satellites);
    }
  }

  Serial.println("Fix! Averaging origin for 30 s, don't move...");
  double sumLat = 0, sumLon = 0;
  long n = 0;
  unsigned long t0 = millis();
  while (millis() - t0 < ORIGIN_AVG_MS) {
    if (pollGPS() && GPS.fix && GPS.HDOP <= MAX_HDOP) {
      sumLat += GPS.latitudeDegrees;
      sumLon += GPS.longitudeDegrees;
      n++;
    }
  }
  if (n > 0) {
    originLat = sumLat / n;
    originLon = sumLon / n;
    haveOrigin = true;
    Serial.printf("Origin: %.7f, %.7f (%ld samples)\n", originLat, originLon, n);
  } else {
    Serial.println("No good samples (HDOP too high). Reset and try somewhere more open.");
  }
}

void loop() {
  if (pollGPS() && GPS.fix && haveOrigin) {
    double d = haversineM(originLat, originLon, GPS.latitudeDegrees, GPS.longitudeDegrees);
    Serial.printf("%02d:%02d:%02d  dist=%.2f m  sats=%d  hdop=%.1f\n",
                  GPS.hour, GPS.minute, GPS.seconds, d, (int)GPS.satellites, GPS.HDOP);
  }
}
