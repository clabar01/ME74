// OLD firmware (before the accuracy changes), kept for before/after testing.
// Built with: pio run -e old_firmware -t upload
#include <Arduino.h>
// Basic demo for calibrated accelerometer & gyro readings from Adafruit ICM20948 on ESP32-S3

#include <Adafruit_ICM20X.h>
#include <Adafruit_ICM20948.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>

Adafruit_ICM20948 icm;
uint16_t measurement_delay_us = 65535;

// --- ESP32-S3 Custom I2C Pin Assignments ---
#define I2C_SDA_PIN 8
#define I2C_SCL_PIN 9

// --- Your Measured Calibration Offsets ---
const float ACCEL_BIAS_X = -0.10805f;
const float ACCEL_BIAS_Y = -0.11242f;
const float ACCEL_BIAS_Z = +0.34704f;

const float GYRO_BIAS_X  = -0.00043f;
const float GYRO_BIAS_Y  = +0.00728f;
const float GYRO_BIAS_Z  = -0.00517f;

// Standard local gravity constant
const float GRAVITY = 9.80665f;

// Runtime zero offsets (updated when typing '0')
float zero_offset_ax = 0.0f;
float zero_offset_ay = 0.0f;
float zero_offset_az = 0.0f;
float zero_offset_gx = 0.0f;
float zero_offset_gy = 0.0f;
float zero_offset_gz = 0.0f;

void zeroSensor() {
  // Average across 50 readings to set the new relative zero
  float sum_ax = 0.0f, sum_ay = 0.0f, sum_az = 0.0f;
  float sum_gx = 0.0f, sum_gy = 0.0f, sum_gz = 0.0f;
  const int SAMPLES = 50;

  for (int i = 0; i < SAMPLES; i++) {
    sensors_event_t a, g, m, t;
    icm.getEvent(&a, &g, &t, &m);

    // Apply baseline calibration
    sum_ax += (a.acceleration.x - ACCEL_BIAS_X);
    sum_ay += (a.acceleration.y - ACCEL_BIAS_Y);
    sum_az += ((a.acceleration.z - ACCEL_BIAS_Z) - GRAVITY);

    sum_gx += (g.gyro.x - GYRO_BIAS_X);
    sum_gy += (g.gyro.y - GYRO_BIAS_Y);
    sum_gz += (g.gyro.z - GYRO_BIAS_Z);

    delay(3);
  }

  zero_offset_ax = sum_ax / SAMPLES;
  zero_offset_ay = sum_ay / SAMPLES;
  zero_offset_az = sum_az / SAMPLES;

  zero_offset_gx = sum_gx / SAMPLES;
  zero_offset_gy = sum_gy / SAMPLES;
  zero_offset_gz = sum_gz / SAMPLES;

  Serial.println("\n>>> SENSOR ZEROED TO CURRENT POSITION <<<\n");
}

void setup(void) {
  Serial.setTxTimeoutMs(0);
  Serial.begin(115200);

  unsigned long t0 = millis();
  while (!Serial && (millis() - t0 < 2000)) {
    delay(10);
  }

  Serial.println("\n--- Adafruit ICM20948 Calibrated ESP32-S3 ---");
  Serial.println("Send '0' at any time to re-zero the sensor.\n");

  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  Wire.setClock(100000);

  bool found = icm.begin_I2C(0x69, &Wire);
  if (!found) {
    found = icm.begin_I2C(0x68, &Wire);
  }

  if (!found) {
    Serial.println("Failed to find ICM20948 chip! Check wiring.");
    while (1) {
      delay(100);
    }
  }

  Serial.println("ICM20948 Found!");

  icm.setAccelRange(ICM20948_ACCEL_RANGE_2_G);
  icm.setGyroRange(ICM20948_GYRO_RANGE_250_DPS);

  zeroSensor();
}

void loop() {
  // Check for the '0' character from the Serial Monitor
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '0') {
      zeroSensor();
    }
  }

  sensors_event_t accel;
  sensors_event_t gyro;
  sensors_event_t mag;
  sensors_event_t temp;
  icm.getEvent(&accel, &gyro, &temp, &mag);

  // 1. Apply calibration biases & remove nominal gravity on Z
  float ax = (accel.acceleration.x - ACCEL_BIAS_X) - zero_offset_ax;
  float ay = (accel.acceleration.y - ACCEL_BIAS_Y) - zero_offset_ay;
  float az = ((accel.acceleration.z - ACCEL_BIAS_Z) - GRAVITY) - zero_offset_az;

  float gx = (gyro.gyro.x - GYRO_BIAS_X) - zero_offset_gx;
  float gy = (gyro.gyro.y - GYRO_BIAS_Y) - zero_offset_gy;
  float gz = (gyro.gyro.z - GYRO_BIAS_Z) - zero_offset_gz;

  // Human-readable formatted readouts
  Serial.print("Accel X: ");
  Serial.print(ax, 4);
  Serial.print(" \tY: ");
  Serial.print(ay, 4);
  Serial.print(" \tZ: ");
  Serial.print(az, 4);
  Serial.println(" m/s^2");

  Serial.print("Gyro  X: ");
  Serial.print(gx, 4);
  Serial.print(" \tY: ");
  Serial.print(gy, 4);
  Serial.print(" \tZ: ");
  Serial.print(gz, 4);
  Serial.println(" radians/s");

  Serial.print("Mag   X: ");
  Serial.print(mag.magnetic.x, 2);
  Serial.print(" \tY: ");
  Serial.print(mag.magnetic.y, 2);
  Serial.print(" \tZ: ");
  Serial.print(mag.magnetic.z, 2);
  Serial.println(" uT");

  Serial.println();

  delay(100);
  delayMicroseconds(measurement_delay_us);
}