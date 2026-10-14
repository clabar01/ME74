
#include <SPI.h>
#include <RH_RF95.h>

#define RFM95_CS  D3
#define RFM95_RST D2
#define RFM95_INT D1
#define RFM95_FREQ 915.0

RH_RF95 rf95(RFM95_CS, RFM95_INT);

void setup() {
  Serial.begin(115200);
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
    Serial.println("FAIL: Radio initialization failed");
    while (1) delay(1000);
  }

  if (!rf95.setFrequency(RFM95_FREQ)) {
    Serial.println("FAIL: Frequency setup failed");
    while (1) delay(1000);
  }

  rf95.setTxPower(13, false);

  Serial.println("Radio ready!");
}

void loop() {
  uint8_t message[] = "Hello LoRa!";

  Serial.println("Starting transmission...");

  if (rf95.send(message, sizeof(message))) {
    if (rf95.waitPacketSent(3000)) {
      Serial.println("PASS: Transmission completed!");
    } else {
      Serial.println("FAIL: Transmission timed out!");
      Serial.println("Check G0 interrupt wiring.");
    }
  } else {
    Serial.println("FAIL: Could not start transmission.");
  }

  delay(5000);
}
