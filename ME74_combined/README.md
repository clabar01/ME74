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
| IMU ICM-20948 | **SDA D4, SCL D5** (moved: D9/D10 are the LoRa SPI pins) |
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

### IMU

The IMU tab shows acceleration (no step or walk counting): total |a|, the
acceleration with gravity removed, and its horizontal and vertical parts, with
peaks and a live graph. Logged CSVs include these as extra columns.

Serial output lines are tagged `[GPS]`, `[IMU]`, `[LORA]`, `[ESPNOW]`, `[SYS]`.
Commands: `0 m l p` (IMU), `origin`, `measure <m>`, `cancel` (GPS),
`c <m>`, `n <x>`, `plot`, `text`, `status` (LoRa), `go [module]`,
`stop [module]`, `only <module>`, `state`, `help`, `info`.

`tools/build_dashboard.py` regenerates the dashboard from the four separate
dashboard HTML files if any of them change.
