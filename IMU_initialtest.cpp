// ICM-20948 orientation (roll, pitch, heading) on a Seeed XIAO ESP32-S3
//
// What this sketch does, in order:
//   1. Sets the sensor ranges, output rates and built-in low-pass filters.
//   2. Startup calibration: averages ~3 s of data while the sensor sits still
//      to measure gyro bias (and accel bias, if the sensor is level).
//   3. Runs a fixed 100 Hz loop and measures the real time step (dt) each pass.
//   4. A complementary filter fuses gyro + accel (roll, pitch) and
//      gyro + magnetometer (heading) into a stable orientation estimate.
//
// Serial commands (Serial Monitor at 115200, send one character):
//   '0' = redo the startup calibration (keep the sensor still)
//   'm' = magnetometer calibration (rotate the sensor in every direction for 20 s)
//   'l' = logging mode: print all data as CSV lines starting with "LOG," (used by
//         tools/imu_still_test.py)
//   'p' = back to the normal printout
//
// Axis convention (printed on the breakout board):
//   X and Y lie flat on the board, Z points up out of the chip when lying flat.
//   Roll  = rotation about X, Pitch = rotation about Y   (degrees)
//   Heading = compass heading, 0-360 deg, clockwise from north (same as GPS course)

#include <Arduino.h>          // needed in a .cpp file (Arduino adds it automatically to .ino files)
#include <Adafruit_ICM20X.h>
#include <Adafruit_ICM20948.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include <math.h>

// ======================= Settings you may want to change =======================

// --- I2C pins on the XIAO ESP32-S3 (GPIO8 = D9, GPIO9 = D10) ---
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

// --- Timing ---
const int   LOOP_HZ  = 100;   // how often we read the sensor and update the filter
const int   PRINT_HZ = 10;    // how often we print (printing every loop would slow it down)
const int   LOG_HZ   = 50;    // how often we print in logging mode ('l' command)

// --- Startup calibration ---
const float CAL_SECONDS   = 3.0f;   // how long to average while still
const int   SETTLE_MS     = 1000;   // wait after power-up so the sensor output settles
// Largest allowed change (max - min) during calibration. Bigger = someone moved it.
const float GYRO_STILL_LIMIT  = 0.05f;  // rad/s  (about 3 deg/s)
const float ACCEL_STILL_LIMIT = 0.30f;  // m/s^2

// If true, the sensor MUST be lying flat (Z up) during startup calibration.
// The code then treats any difference from (0, 0, +9.81) as accelerometer bias.
// Side effect: roll and pitch will read ~0 at startup by definition.
// If the sensor is not level within LEVEL_TOLERANCE, the code falls back to
// the fixed bias values below and prints a warning.
const bool  LEVEL_ACCEL_CAL = true;
const float LEVEL_TOLERANCE = 1.0f;     // m/s^2 (about 6 deg of tilt)

// Bench-measured accelerometer bias (m/s^2). Used when the level calibration is
// off or fails.
const float ACCEL_BIAS_X_FIXED = -0.10805f;
const float ACCEL_BIAS_Y_FIXED = -0.11242f;
const float ACCEL_BIAS_Z_FIXED = +0.34704f;

// --- Magnetometer ---
// false = ignore the magnetometer: heading (yaw) comes from the gyro only and
// starts at 0 deg at power-up/calibration. It drifts slowly (a few deg/min),
// which is fine for short movements. Set true to use magnetic heading
// (needs a magnetically clean mounting, see the offsets below).
const bool  USE_MAGNETOMETER = false;

// Hard-iron offsets (uT)
// Run the 'm' command once with the sensor mounted in its final enclosure,
// then paste the printed numbers here so they are used at every power-up.
// Measured 2026-10-14 on the breadboard prototype ('m' calibration for Z,
// flat 360-degree spin for X and Y). Redo if anything on the board changes.
const float MAG_OFFSET_X_FIXED = -101.97f;
const float MAG_OFFSET_Y_FIXED = 115.34f;
const float MAG_OFFSET_Z_FIXED = 126.82f;
const float MAG_CAL_SECONDS    = 20.0f;

// Magnetic declination at your location (deg, east = positive).
// Look it up at https://www.ngdc.noaa.gov/geomag/calculators/magcalc.shtml
// so heading matches GPS (true north) instead of magnetic north.
const float DECLINATION_DEG = 0.0f;

// --- Complementary filter time constants (seconds) ---
// Bigger = trust the gyro longer = smoother but slower to correct drift.
const float TAU_TILT    = 1.0f;   // roll and pitch (corrected by accelerometer)
const float TAU_HEADING = 2.0f;   // heading (corrected by magnetometer, which is noisier)

// The accel/mag correction is skipped when the reading looks disturbed.
const float ACCEL_TRUST_BAND = 0.10f * 9.80665f;  // |accel| must be within 0.1 g of 1 g
const float MAG_MIN_UT = 20.0f;   // Earth's field is about 25-65 uT everywhere
const float MAG_MAX_UT = 70.0f;

// ===============================================================================

const float GRAVITY = 9.80665f;   // m/s^2

struct Vec3 {
  float x, y, z;
};

Adafruit_ICM20948 icm;
uint8_t icmAddress = 0x69;

// Calibration results (filled in at startup)
Vec3 gyroBias  = {0, 0, 0};
Vec3 accelBias = {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
Vec3 magOffset = {MAG_OFFSET_X_FIXED, MAG_OFFSET_Y_FIXED, MAG_OFFSET_Z_FIXED};

// Orientation estimate (radians). heading is clockwise from magnetic north.
float roll = 0, pitch = 0, heading = 0;
bool  filterStarted = false;

// Loop timing
const uint32_t LOOP_PERIOD_US = 1000000UL / LOOP_HZ;
uint32_t nextLoopUs   = 0;   // when the next loop pass should start
uint32_t lastSampleUs = 0;   // time of the previous sample (for dt)
uint32_t loopCount    = 0;
uint32_t overruns     = 0;   // number of times a loop pass took too long

bool logMode = false;        // true = print CSV data lines instead of the normal printout

// ------------------------------------------------------------------------------
// Keep an angle between -PI and +PI (so 359 deg and 1 deg are 2 deg apart, not 358)
float wrapPi(float a) {
  while (a >  PI) a -= TWO_PI;
  while (a < -PI) a += TWO_PI;
  return a;
}

// ------------------------------------------------------------------------------
// The Adafruit library's enableGyrolDLPF() writes to the ACCEL config register
// by mistake, so we set the gyro low-pass filter directly.
// Datasheet: register bank 2, GYRO_CONFIG_1 (0x01):
//   bit 0 = GYRO_FCHOICE (1 = low-pass filter on), bits 5:3 = GYRO_DLPFCFG
void setGyroLowPass(uint8_t dlpfcfg) {
  const uint8_t REG_BANK_SEL  = 0x7F;
  const uint8_t GYRO_CONFIG_1 = 0x01;

  Wire.beginTransmission(icmAddress);   // switch to register bank 2
  Wire.write(REG_BANK_SEL);
  Wire.write(2 << 4);
  Wire.endTransmission();

  Wire.beginTransmission(icmAddress);   // read the current value
  Wire.write(GYRO_CONFIG_1);
  Wire.endTransmission(false);
  Wire.requestFrom(icmAddress, (uint8_t)1);
  uint8_t value = Wire.read();

  value &= ~0b00111001;                 // clear DLPFCFG and FCHOICE, keep range bits
  value |= (dlpfcfg & 0b111) << 3;      // new filter setting
  value |= 0b1;                         // FCHOICE = 1: filter on

  Wire.beginTransmission(icmAddress);   // write it back
  Wire.write(GYRO_CONFIG_1);
  Wire.write(value);
  Wire.endTransmission();

  Wire.beginTransmission(icmAddress);   // back to bank 0 (the library expects it)
  Wire.write(REG_BANK_SEL);
  Wire.write(0);
  Wire.endTransmission();
}

// ------------------------------------------------------------------------------
// Read one sample and return it in consistent units and axes:
//   accel in m/s^2, gyro in rad/s, mag in uT, all in the accel/gyro axis frame.
// Biases are removed here so the rest of the code works with clean values.
void readSensors(Vec3 &accel, Vec3 &gyro, Vec3 &mag) {
  sensors_event_t a, g, t, m;
  icm.getEvent(&a, &g, &t, &m);

  accel.x = a.acceleration.x - accelBias.x;
  accel.y = a.acceleration.y - accelBias.y;
  accel.z = a.acceleration.z - accelBias.z;

  gyro.x = g.gyro.x - gyroBias.x;
  gyro.y = g.gyro.y - gyroBias.y;
  gyro.z = g.gyro.z - gyroBias.z;

  // The magnetometer is a separate chip (AK09916) inside the package and its
  // Y and Z axes point the opposite way from the accel/gyro axes (datasheet,
  // "Orientation of Axes"). Flip them so all three sensors share one frame.
  mag.x =  m.magnetic.x - magOffset.x;
  mag.y = -m.magnetic.y - magOffset.y;
  mag.z = -m.magnetic.z - magOffset.z;
}

// ------------------------------------------------------------------------------
// Startup calibration. The sensor must be still. Returns false if it moved.
bool calibrateBias() {
  Serial.println("\nCalibrating... keep the sensor still.");

  // Measure with no bias removed
  gyroBias  = {0, 0, 0};
  Vec3 savedAccelBias = accelBias;
  accelBias = {0, 0, 0};

  const int N = (int)(CAL_SECONDS * LOOP_HZ);
  Vec3 sumA = {0, 0, 0}, sumG = {0, 0, 0};
  Vec3 minG = { 1e9,  1e9,  1e9}, maxG = {-1e9, -1e9, -1e9};
  Vec3 minA = { 1e9,  1e9,  1e9}, maxA = {-1e9, -1e9, -1e9};

  for (int i = 0; i < N; i++) {
    Vec3 a, g, m;
    readSensors(a, g, m);

    sumA.x += a.x;  sumA.y += a.y;  sumA.z += a.z;
    sumG.x += g.x;  sumG.y += g.y;  sumG.z += g.z;

    // Track the spread so we can tell if the sensor was bumped
    minG.x = min(minG.x, g.x);  maxG.x = max(maxG.x, g.x);
    minG.y = min(minG.y, g.y);  maxG.y = max(maxG.y, g.y);
    minG.z = min(minG.z, g.z);  maxG.z = max(maxG.z, g.z);
    minA.x = min(minA.x, a.x);  maxA.x = max(maxA.x, a.x);
    minA.y = min(minA.y, a.y);  maxA.y = max(maxA.y, a.y);
    minA.z = min(minA.z, a.z);  maxA.z = max(maxA.z, a.z);

    delay(1000 / LOOP_HZ);
  }

  bool gyroMoved  = (maxG.x - minG.x > GYRO_STILL_LIMIT) ||
                    (maxG.y - minG.y > GYRO_STILL_LIMIT) ||
                    (maxG.z - minG.z > GYRO_STILL_LIMIT);
  bool accelMoved = (maxA.x - minA.x > ACCEL_STILL_LIMIT) ||
                    (maxA.y - minA.y > ACCEL_STILL_LIMIT) ||
                    (maxA.z - minA.z > ACCEL_STILL_LIMIT);
  if (gyroMoved || accelMoved) {
    Serial.println("Movement detected during calibration, trying again.");
    accelBias = savedAccelBias;
    return false;
  }

  Vec3 meanA = {sumA.x / N, sumA.y / N, sumA.z / N};

  // Gyro: when still, the true rotation rate is zero, so the average IS the bias.
  gyroBias = {sumG.x / N, sumG.y / N, sumG.z / N};

  // Accel: when still and level, the true reading is (0, 0, +g).
  // (The accelerometer measures the push of the table holding it up, so +Z.)
  bool isLevel = fabs(meanA.x) < LEVEL_TOLERANCE &&
                 fabs(meanA.y) < LEVEL_TOLERANCE &&
                 fabs(meanA.z - GRAVITY) < LEVEL_TOLERANCE;

  if (LEVEL_ACCEL_CAL && isLevel) {
    accelBias = {meanA.x, meanA.y, meanA.z - GRAVITY};
  } else {
    accelBias = {ACCEL_BIAS_X_FIXED, ACCEL_BIAS_Y_FIXED, ACCEL_BIAS_Z_FIXED};
    if (LEVEL_ACCEL_CAL) {
      Serial.println("WARNING: sensor is not level (Z up), using fixed accel bias values.");
    }
  }

  Serial.printf("Gyro bias  (rad/s): %.5f  %.5f  %.5f\n", gyroBias.x, gyroBias.y, gyroBias.z);
  Serial.printf("Accel bias (m/s^2): %.4f  %.4f  %.4f\n", accelBias.x, accelBias.y, accelBias.z);
  Serial.println("Calibration done.\n");
  return true;
}

// ------------------------------------------------------------------------------
// Hard-iron magnetometer calibration. Nearby steel, magnets and the circuit
// board itself add a constant offset to the magnetic reading. Rotating the
// sensor through every direction lets us find the center of the readings.
void calibrateMag() {
  Serial.println("\nMagnetometer calibration: slowly rotate the sensor through");
  Serial.printf("every orientation (figure-8s, flip it over) for %.0f seconds...\n", MAG_CAL_SECONDS);

  magOffset = {0, 0, 0};
  Vec3 lo = { 1e9,  1e9,  1e9};
  Vec3 hi = {-1e9, -1e9, -1e9};

  uint32_t start = millis();
  while (millis() - start < (uint32_t)(MAG_CAL_SECONDS * 1000)) {
    Vec3 a, g, m;
    readSensors(a, g, m);
    lo.x = min(lo.x, m.x);  hi.x = max(hi.x, m.x);
    lo.y = min(lo.y, m.y);  hi.y = max(hi.y, m.y);
    lo.z = min(lo.z, m.z);  hi.z = max(hi.z, m.z);
    delay(10);
  }

  // The center of the min/max range is the offset
  magOffset = {(hi.x + lo.x) / 2, (hi.y + lo.y) / 2, (hi.z + lo.z) / 2};

  Serial.println("Done. Paste these into the MAG_OFFSET_*_FIXED constants:");
  Serial.printf("  MAG_OFFSET_X_FIXED = %.2ff;\n", magOffset.x);
  Serial.printf("  MAG_OFFSET_Y_FIXED = %.2ff;\n", magOffset.y);
  Serial.printf("  MAG_OFFSET_Z_FIXED = %.2ff;\n\n", magOffset.z);
}

// ------------------------------------------------------------------------------
// Complementary filter.
//   Gyro:  very smooth over short times, but its small errors add up (drift).
//   Accel: knows which way is down (no drift), but is noisy and upset by vibration.
//   Mag:   knows which way is north (no drift), but is noisy.
// Each step we move the angle using the gyro, then nudge it a small amount
// toward the accel/mag answer. The nudge size comes from the time constant tau.
void updateOrientation(const Vec3 &a, const Vec3 &g, const Vec3 &m, float dt) {

  // --- Angles measured directly by the accelerometer (gravity direction) ---
  float rollAcc  = atan2(a.y, a.z);
  float pitchAcc = atan2(-a.x, sqrt(a.y * a.y + a.z * a.z));

  // --- Heading measured by the magnetometer ---
  // First "un-tilt" the magnetic vector using roll and pitch so we only use
  // its horizontal part (tilt compensation). Use the current estimate if we
  // have one, otherwise the accel angles.
  float r = filterStarted ? roll  : rollAcc;
  float p = filterStarted ? pitch : pitchAcc;
  float mxLevel = m.x * cos(p) + (m.y * sin(r) + m.z * cos(r)) * sin(p);
  float myLevel = m.y * cos(r) - m.z * sin(r);
  float headingMag = atan2(myLevel, mxLevel);   // clockwise from magnetic north

  // First sample: start directly from the measured angles
  if (!filterStarted) {
    roll = rollAcc;
    pitch = pitchAcc;
    heading = USE_MAGNETOMETER ? headingMag : 0;
    filterStarted = true;
    return;
  }

  // --- 1. Predict with the gyro ---
  // Gyro rates are about the sensor's own axes. Convert them to rates of
  // change of roll, pitch and yaw (standard Euler-angle kinematics).
  float sr = sin(roll), cr = cos(roll);
  float cp = cos(pitch), tp = tan(pitch);
  if (fabs(cp) < 0.1f) cp = (cp < 0 ? -0.1f : 0.1f);   // avoid divide-by-zero near pitch = +/-90 deg

  float rollRate  = g.x + (g.y * sr + g.z * cr) * tp;
  float pitchRate = g.y * cr - g.z * sr;
  float yawRate   = (g.y * sr + g.z * cr) / cp;   // counter-clockwise, seen from above

  roll    += rollRate  * dt;
  pitch   += pitchRate * dt;
  heading -= yawRate   * dt;   // heading is clockwise, so it goes the other way

  // --- 2. Correct with the accelerometer (roll, pitch) ---
  // Skip if the total acceleration isn't close to 1 g (the sensor is being
  // shaken or accelerated, so the accel no longer points straight down).
  float alphaTilt = TAU_TILT / (TAU_TILT + dt);
  float accelMag  = sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
  if (fabs(accelMag - GRAVITY) < ACCEL_TRUST_BAND) {
    roll  += (1 - alphaTilt) * wrapPi(rollAcc  - roll);
    pitch += (1 - alphaTilt) * wrapPi(pitchAcc - pitch);
  }

  // --- 3. Correct with the magnetometer (heading) ---
  // Skip if the field strength is unrealistic (magnet or steel nearby).
  float alphaHead = TAU_HEADING / (TAU_HEADING + dt);
  float fieldUT   = sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
  if (USE_MAGNETOMETER && fieldUT > MAG_MIN_UT && fieldUT < MAG_MAX_UT) {
    heading += (1 - alphaHead) * wrapPi(headingMag - heading);
  }

  roll    = wrapPi(roll);
  heading = wrapPi(heading);
}

// ------------------------------------------------------------------------------
// Restart the fixed-rate loop timer (after calibration or a long pause)
void restartLoopTimer() {
  nextLoopUs    = micros();
  lastSampleUs  = nextLoopUs;
  filterStarted = false;
}

// ------------------------------------------------------------------------------
void setup(void) {
  Serial.setTxTimeoutMs(0);
  Serial.begin(115200);

  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) {
    delay(10);
  }

  Serial.println("\n--- ICM-20948 orientation, ESP32-S3 ---");
  Serial.println("Send '0' to recalibrate (keep still), 'm' for magnetometer calibration.\n");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(400000);   // ICM-20948 supports 400 kHz "fast mode" I2C

  bool found = icm.begin_I2C(0x69, &Wire);
  if (!found) {
    icmAddress = 0x68;
    found = icm.begin_I2C(0x68, &Wire);
  }
  if (!found) {
    Serial.println("Failed to find ICM20948 chip! Check wiring.");
    while (1) {
      delay(100);
    }
  }
  Serial.printf("ICM20948 found at 0x%02X\n", icmAddress);

  // --- Ranges (datasheet: smaller range = finer resolution) ---
  // Stationary nodes see ~1 g and almost no rotation, so the most sensitive
  // ranges fit: +/-2 g (16384 LSB/g) and +/-250 deg/s (131 LSB per deg/s).
  // If the sensor will be on a moving vehicle, use 4 G and 500 DPS instead
  // so bumps and turns don't max out (clip) the readings.
  icm.setAccelRange(ICM20948_ACCEL_RANGE_2_G);
  icm.setGyroRange(ICM20948_GYRO_RANGE_250_DPS);

  // --- Sensor output rates (datasheet: rate = base / (1 + divisor)) ---
  // Run the sensor about 2x faster than our 100 Hz loop so every read gets a
  // fresh sample.
  icm.setGyroRateDivisor(4);    // 1100 Hz / 5 = 220 Hz
  icm.setAccelRateDivisor(4);   // 1125 Hz / 5 = 225 Hz
  icm.setMagDataRate(AK09916_MAG_DATARATE_100_HZ);   // fastest magnetometer mode

  // --- Built-in low-pass filters: remove noise above ~24 Hz ---
  // (Our angles change far slower than that, so nothing useful is lost.)
  icm.enableAccelDLPF(true, ICM20X_ACCEL_FREQ_23_9_HZ);
  setGyroLowPass(4);            // DLPFCFG = 4 -> 23.9 Hz (datasheet table)

  delay(SETTLE_MS);             // let the sensor and filters settle

  while (!calibrateBias()) {
    // keep trying until the sensor was still for the whole calibration
  }
  restartLoopTimer();
}

// ------------------------------------------------------------------------------
void loop() {
  // --- Serial commands ---
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '0') {
      while (!calibrateBias()) {}
      restartLoopTimer();
    } else if (c == 'm') {
      calibrateMag();
      restartLoopTimer();
    } else if (c == 'l') {
      logMode = true;
    } else if (c == 'p') {
      logMode = false;
    }
  }

  // --- Wait for the next loop time so samples are evenly spaced ---
  while ((int32_t)(micros() - nextLoopUs) < 0) {
    // waiting
  }
  nextLoopUs += LOOP_PERIOD_US;

  // If we fell more than one period behind (e.g. slow printing), resync
  // instead of running many fast catch-up loops.
  uint32_t now = micros();
  if ((int32_t)(now - nextLoopUs) > 0) {
    nextLoopUs = now + LOOP_PERIOD_US;
    overruns++;
  }

  // --- Measure the actual time step ---
  float dt = (now - lastSampleUs) * 1e-6f;   // seconds
  lastSampleUs = now;

  // --- Read sensors and update the orientation estimate ---
  Vec3 accel, gyro, mag;
  readSensors(accel, gyro, mag);
  updateOrientation(accel, gyro, mag, dt);

  // --- Print at a slower rate ---
  loopCount++;
  float headingDeg = heading * RAD_TO_DEG + DECLINATION_DEG;
  if (headingDeg < 0)    headingDeg += 360;
  if (headingDeg >= 360) headingDeg -= 360;

  if (logMode) {
    // Logging mode: one CSV line with everything, for the test script.
    // time (us), accel (m/s^2), gyro (rad/s), mag (uT), angles (deg), dt (ms), overruns
    if (loopCount % (LOOP_HZ / LOG_HZ) == 0) {
      Serial.printf("LOG,%lu,%.4f,%.4f,%.4f,%.5f,%.5f,%.5f,%.2f,%.2f,%.2f,%.3f,%.3f,%.3f,%.3f,%lu\n",
                    (unsigned long)now,
                    accel.x, accel.y, accel.z, gyro.x, gyro.y, gyro.z, mag.x, mag.y, mag.z,
                    roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg,
                    dt * 1000, (unsigned long)overruns);
    }
  } else if (loopCount % (LOOP_HZ / PRINT_HZ) == 0) {
    // "Name:value" format also works with the Arduino Serial Plotter
    Serial.printf("Roll:%.2f,Pitch:%.2f,Heading:%.1f,dt_ms:%.2f,Overruns:%lu\n",
                  roll * RAD_TO_DEG, pitch * RAD_TO_DEG, headingDeg,
                  dt * 1000, (unsigned long)overruns);
  }
}
