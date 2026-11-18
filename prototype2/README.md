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
```

The JSON status lines from the existing ME74 GPS firmware also work for **Set node**.
Default radio IDs: nodes `A`, `B`; drones `D1` (Typhoon G), `D2` (Contixo F28).

## Editing

Edit `src/dashboard.template.html`, then rebuild (Leaflet gets inlined so the map works offline):

```
npm install leaflet@1.9.4
python3 build_dashboard.py
```
