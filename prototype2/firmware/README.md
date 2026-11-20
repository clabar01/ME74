# Prototype 2 firmware

Two new PlatformIO projects. They're separate from `ME74_combined/`, which is unchanged.

| Folder | Goes on | Environments |
|---|---|---|
| `p2_node/` | each stationary node | `node1` = node A, `node2` = node B, `node3` = node C |
| `p2_drone/` | the payload board on each drone | `drone1` = Typhoon G, `drone2` = Contixo F28 |

Both compiled cleanly against the same platform as ME74_combined (pioarduino / Arduino-ESP32 3.x,
XIAO ESP32-S3). They haven't been run on hardware yet, so bench-test them before a flight.

## Wiring

Same pins as ME74_combined, so the existing boards work as they are:

| Part | Pins |
|---|---|
| LoRa RFM95 | SCK D8, MISO D9, MOSI D10, CS D3, RST D2, G0 D1 |
| IMU ICM-20948 (drone) | SDA D4, SCL D5 |
| GPS (drone, optional) | GPS TX to D7, GPS RX to D6 |

## Uploading

Open the folder (`p2_node` or `p2_drone`) in VS Code with PlatformIO, then
Project Tasks > `node1` (or `drone1`, ...) > Upload. Or from a terminal:

```
cd prototype2/firmware/p2_drone
pio run -e drone1 -t upload
```

Plug in one board at a time. The drone project uses a bigger storage partition (about 6 MB for logs,
roughly 15 minutes of flying), so the first upload wipes anything stored on that board.

## What each one does

**Node** (`p2_node`): broadcasts a tiny beacon about 5 times a second and saves nothing. Plugged into the
laptop, it also prints what it hears in the dashboard's log format (drone reports, node-to-node signal),
which is the start of a live view.

**Drone** (`p2_drone`): on battery, it logs to its own flash memory:

```
12034,RSSI,A,D1,-67,9.5          signal from node A's beacon (dBm, SNR)
12040,GPS,D1,lat,lon,alt,sats,hdop,hhmmss.ss    5 per second, if a GPS is plugged in
12052,IMU,D1,t_us,ax,ay,az,gx,gy,gz,mx,my,mz,roll,pitch,heading,dt_ms,overruns   50 per second
```

Each trial is its own file (`t001.csv`, `t002.csv`, ...), started and stopped with the **B (BOOT) button**
on the XIAO, since the batteries stay connected and the boards are always on. Powering up never starts a file. It also radios a short report
twice a second for a node plugged into the laptop. The IMU code is copied unchanged from ME74_combined.

All boards use LoRa at 915 MHz, 500 kHz bandwidth, SF7 (`LORA_MODEM` at the top of each file).
That's a different setting from ME74_combined, so P2 boards and old boards won't hear each other.

## Flight day

1. **Nodes:** power them, set them in place about 3 ft up, and press "Set node" for each in the dashboard.
2. **Drones:** set the drone on the ground, **press B once**, take your hand away and **keep it still for about 6 s**
   while the IMU recalibrates (LED solid on). When the LED switches to a slow blink, it's logging.
3. Fly the path.
4. After landing, **press B again**. The file closes and the LED goes back to idle (a short blip every 3 s).
5. Plug the board into the laptop. In the dashboard press **Read log from drone**. It saves
   a copy to Downloads and loads the flight.
6. Once the log is safely on the laptop, press **Erase logs on drone** (More tab). Don't erase before that.

Repeat 2 to 4 for each trial; every B press pair is one file. Pressing **R** (reset) just reboots the board into idle.

| LED | Meaning |
|---|---|
| short blip every 3 s | on and idle, ready for a trial |
| solid on | trial started, IMU calibrating: keep still |
| slow blink (1 s) | logging |
| fast blink | problem: no IMU, no radio, or memory full |

## USB commands (drone)

| Command | What it does |
|---|---|
| `status` | logging or not, current file, free memory, radio/IMU/GPS state, latest signal from each node |
| `list` | files saved on the board |
| `dump` / `dump 3` | print the newest file (or file 3), then a line `END` |
| `erase 3` / `eraseall` | delete one file / every file |
| `start` / `stop` | same as pressing B: start a trial (new file + IMU recalibration) / stop it |
| `imu` | toggle a live IMU printout |
| `0` | recalibrate the IMU (keep it still) |

## Things to check on the bench first

- `status` on a drone shows both nodes' signal and "IMU: running".
- `start`, wait 30 s, `stop`, `dump`: the dashboard loads it and shows IMU and signal lines.
- On battery: press B, wait for the slow blink, wait a minute, press B again, then plug in: `list` shows a new file of a sensible size.
