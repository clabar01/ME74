# ME74 radios: LoRa + ESP-NOW

Just the two radio links from `ME74_combined`, since both need two boards.
Flash `node1` on one board and `node2` on the other (same code; only the LoRa
node number differs). If the LoRa radio isn't wired, ESP-NOW still runs.

## Wiring (XIAO ESP32-S3)

| Part | Connections |
|---|---|
| LoRa RFM95 | SCK D8, MISO D9, MOSI D10, CS D3, RST D2, G0/INT D1, VIN 3V3, GND GND |
| ESP-NOW | built-in Wi-Fi radio, nothing to wire |

## Use

1. Open this folder in VS Code (PlatformIO). Upload `node1` to one board and
   `node2` to the other, one board plugged in at a time.
2. Close the Serial Monitor, open `ME74_radio_dashboard.html` in Chrome or Edge
   and click **Connect board** (with either board plugged into the computer).
3. **Home** shows both distances side by side with a 5-minute graph and an
   actual (tape) distance box. **Go all / Stop all**, and **Start/Stop** or
   **Only** on each radio, let you run both together or test one at a time.
4. Save the current tab's data with the button at the right of the tab bar, or
   everything at once (separate CSVs inside one ZIP) with **Save all CSV**.

Commands (Serial Monitor, 115200): `c <m>`, `n <x>`, `plot`, `text`, `status`
(LoRa), `go`, `go lora`, `go espnow`, `stop`, `stop lora`, `stop espnow`,
`only lora`, `only espnow`, `state`, `help`, `info`.

The other board can run on a battery: both radios start on their own at
power-up (set `AUTO_START 0` in `main.cpp` to wait for `go` instead).
