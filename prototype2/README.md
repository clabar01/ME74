# ME74 Prototype 2: flight replay dashboard

`ME74_P2_position_dashboard.html` is a single file. Open it in Chrome or Edge.

Nothing has to be live. During a test the drones run on battery and save their readings to their own
memory; the dashboard is for looking at a flight afterwards.

1. **Set the nodes** (at the field, before flying). Stand at each stationary node and press **Set node**.
   It places the node at the laptop's location, and if the GPS board is plugged in it averages 30 s of GPS
   for a better fix. You can also drag a node on the satellite map.
2. **Load a flight.** Drag the drones' log files onto the page (one file per drone), or plug a drone in
   over USB and press **Read log from drone**.
3. **Replay and compare.** Press play or drag the timeline. Each drone gets three positions, all compared
   to its GPS: **radio** (LoRa signal strength to the nodes), **IMU only** (dead reckoning), and
   **radio + IMU** (Kalman filter). "Analyze from here / to here" leaves out takeoff and landing.
   **Save results** downloads one ZIP with every CSV, the original logs and the setup.

**Demo flight** makes a fake 2.5 minute flight so you can see how it works.

The satellite imagery (Esri) needs internet. Open the page once with a connection to cache it. Positions
still work offline, just without imagery.

## Log format

One reading per line, starting with the board's own time in milliseconds:

```
12034,RSSI,A,D1,-67              signal between node A and drone D1 (dBm)
12040,GPS,D1,42.4075120,-71.1190400,21.4,9,0.9,153012.00
12052,IMU,D1,t_us,ax,ay,az,gx,gy,gz,mx,my,mz,roll,pitch,heading,dt_ms,overruns
12060,ALT,D1,8.2                 height above takeoff (m)
```

The last GPS field is the GPS's UTC time (hhmmss.ss). It lines the two drones' logs up with each other;
without it, the logs start together and can be shifted by hand in the Flight tab.

The existing ME74 firmware printout also works with a time in front of each line:
`[IMU] LOG,...`, `[GPS] {...json...}`, and `[LORA] RX from node 1` followed by `[LORA] RSSI: -71 dBm`
(node 1 = A, node 2 = B). Lines without a drone ID belong to the drone picked for that file.

**Read log from drone** sends `dump` over USB and reads lines until `END` (the firmware needs to support it).

No ESP-NOW: Prototype 2 doesn't use it.

## IMU (merged from `ME74_dashboard_2D.html`)

Same dead reckoning math as the 2D IMU dashboard: gravity removed with roll, pitch and heading, integrated
twice, velocity set to zero whenever the IMU is still, and each move's leftover drift taken back out when it
stops. The IMU zeros the first time the drone sits still in the log (or wherever you press "Zero IMU here").
Under Setup, set which way the nose (IMU X axis) points when it zeros, since the firmware's heading is
relative (no magnetometer).

Known limit: an accelerometer can't tell steady cruising from being stopped, so the "still" check can fire
while a drone flies at constant speed. The radio + IMU filter only trusts "still" when it already thinks the
drone is slow.

## Editing

Edit `src/dashboard.template.html`, then rebuild (Leaflet gets inlined so the map works offline):

```
npm install leaflet@1.9.4
python3 build_dashboard.py
```
