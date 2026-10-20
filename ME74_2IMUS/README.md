# ME74 combined: LoRa + ESP-NOW + IMU + GPS

One firmware (`src/main.cpp`) and one dashboard (`ME74_dashboard.html`) for a
Seeed XIAO ESP32-S3 with all four parts connected. Flash the same code on both
boards: environment `node1` on one, `node2` on the other (only the LoRa node
number differs). Any part that isn't plugged in is skipped.

## Wiring

| Part | Connections |
|---|---|
| LoRa RFM95 | SCK D8, MISO D9, MOSI D10, CS D3, RST D2, G0/INT D1 |
| GPS | GPS TX -> D7, GPS RX -> D6 |
| IMU (ICM-20948 **or** SparkFun ISM330DHCX Qwiic) | **SDA D4, SCL D5** (moved: D9/D10 are the LoRa SPI pins). Qwiic cable: black GND, red 3V3, blue SDA, yellow SCL |
| All | VIN/VCC 3V3, GND GND |

## Use

1. Open this folder in VS Code (PlatformIO). Upload `node1` to one board and
   `node2` to the other, one board plugged in at a time.
2. Close the Serial Monitor, open `ME74_dashboard.html` in Chrome or Edge and
   click **Connect board**. The GPS, IMU, LoRa and ESP-NOW tabs are the original
   dashboards, all fed by the one USB connection; **Console** shows everything
   and lets you type commands.

### Run everything, or test one part at a time

The bar under the title has **Go all** / **Stop all**, and for each module a
**Start/Stop** button and **Only** (runs just that module and stops the others).
The same works from the Serial Monitor: `go`, `stop`, `go gps`, `stop lora`,
`only imu`, `state`. At power-up everything starts on its own (set
`AUTO_START 0` in `main.cpp` to start in standby instead), so a board on a
battery still works as the other end of the radio links.

### Home page and saving data

**Home** shows each technology's current distance side by side (GPS from its
origin, LoRa and ESP-NOW from signal strength; the IMU shows acceleration,
since it doesn't measure distance) and graphs the last 5 minutes. Type the
actual tape distance there to record it next to every reading.

Everything that arrives is recorded while connected. The button at the right of
the tab bar saves the current tab's data as a CSV; **Save all CSV** downloads
one ZIP with every module's CSV, the overview (one row per second, all
distances side by side) and the console log. **Clear data** starts over.

### IMU

Two IMUs can run at the same time on the same two pins (D4 SDA, D5 SCL), because
they have different I2C addresses:
- **IMU** = ICM-20948 (9-DoF, 0x69 or 0x68): `[IMU]` lines, **IMU** tab.
- **IMU 2** = SparkFun ISM330DHCX Qwiic (6-DoF, 0x6B or 0x6A): `[IMU2]` lines, **IMU 2** tab.
Connect either one or both. Commands for the second IMU start with `imu2 `
(`imu2 0`, `imu2 l`, `imu2 p`), and `go imu2` / `stop imu2` / `only imu2` work like
the other modules. The ISM330DHCX has no magnetometer, so its heading comes from
the gyro only.


The IMU tab shows acceleration (no step or walk counting): total |a|, the
acceleration with gravity removed, and its horizontal and vertical parts, with
peaks and a live graph. Logged CSVs include these as extra columns.

Serial output lines are tagged `[GPS]`, `[IMU]`, `[LORA]`, `[ESPNOW]`, `[SYS]`.
Commands: `0 m l p` (IMU), `origin`, `measure <m>`, `cancel` (GPS),
`c <m>`, `n <x>`, `plot`, `text`, `status` (LoRa), `go [module]`,
`stop [module]`, `only <module>`, `state`, `help`, `info`.

`tools/build_dashboard.py` regenerates the dashboard from the four separate
dashboard HTML files if any of them change.
