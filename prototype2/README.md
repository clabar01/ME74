# ME74 Prototype 2: drone positioning dashboard

`ME74_P2_position_dashboard.html` is a single file. Open it in Chrome or Edge (it needs Web Serial).

1. **Set the nodes.** Stand at each stationary node and press **Set node**. It places the node at the
   laptop's location right away, then averages the GPS board (if plugged in) for 30 s for a better fix.
   You can also drag a node on the satellite map.
2. **Connect receiver.** Plug in the board that receives the LoRa packets and press **Connect receiver**.
3. **Fly a trial.** Press **Start trial**. Drone positions are calculated from LoRa signal strength (RSSI)
   to each node and drawn on the map. **Save data** downloads one ZIP with every CSV and the setup.

**Demo** runs fake drones through the same math, so you can try it with no hardware.

The satellite imagery (Esri) needs internet. Open the page once at the field with a connection so it gets
cached. Positions still work offline, just without imagery.

## Serial lines the receiver must print (115200 baud)

```
RSSI,A,D1,-67        node A heard drone D1 at -67 dBm
RSSI,A,B,-58         node A heard node B
GPS,D1,42.4075120,-71.1190400,21.4,9,0.9
ALT,D1,8.2           drone height above takeoff (m)
IMU,D1,t_us,ax,ay,az,gx,gy,gz,mx,my,mz,roll,pitch,heading,dt_ms,overruns
```

The JSON status lines from the existing ME74 GPS firmware also work for **Set node**.
Default radio IDs: nodes `A`, `B`; drones `D1` (Typhoon G), `D2` (Contixo F28).

## IMU (merged from `ME74_dashboard_2D.html`)

The IMU dead reckoning is the same math as the 2D IMU dashboard: gravity removed with roll, pitch and heading,
integrated twice, velocity set to zero whenever the IMU is still, and each move's leftover drift taken back out
when it stops. Each drone gets three positions, all compared to its GPS:

- **Radio**: from the LoRa signal strength to the nodes
- **IMU only**: dead reckoning from where the IMU zeroed (its GPS or radio position at that moment)
- **Radio + IMU**: a Kalman filter where the IMU acceleration predicts and the radio fixes correct

Drones send `IMU,D1,` + the 15 LOG fields. A board on USB running the existing firmware works too: its
`[IMU] LOG,...` lines go to drone 1 and `[IMU2]` lines to drone 2 (changeable under More). The IMU zeros when you
press Start trial, as soon as the drone sits still. Under Setup, set which way the nose (IMU X axis) points when it
zeros, since the firmware's heading is relative (no magnetometer).

Known limit: an accelerometer can't tell steady cruising from being stopped, so the "still" check can fire while a
drone flies at constant speed and the IMU-only track comes up short. The combined filter only trusts "still" when
it already thinks the drone is slow.

## Editing

Edit `src/dashboard.template.html`, then rebuild (Leaflet gets inlined so the map works offline):

```
npm install leaflet@1.9.4
python3 build_dashboard.py
```
