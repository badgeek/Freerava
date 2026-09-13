# cyclomp ride visualiser

A standalone, no-build Three.js viewer for a ride exported from the cyclomp
app. Clean, light, Strava-style design: route in 3D with elevation as height
and a translucent elevation curtain, coloured by speed on a single-hue orange
ramp (slow = light, fast = deep), fly-through reveal, chase cam, scrub
timeline, and a stat row (distance / moving time / avg / max / climb / HR).

## Get a ride file

In the app: **LOG → tap a ride → EXPORT**. It writes
`ride_<timestamp>.geojson`.

- **Android**: `/data/data/dev.bauhouse.cyclomp/files/exports/`. Pull it with
  `adb exec-out run-as dev.bauhouse.cyclomp cat files/exports/<name>.geojson > ride.geojson`
- **Desktop build**: `~/cyclomp_ride_<timestamp>.geojson`

A ready-made example is in `sample/`.

## Run

No build step. Serve the folder and open it (a local server avoids the
browser's file:// module restriction):

```
cd viz && python3 -m http.server 8080
# open http://localhost:8080
```

Then **drag the `.geojson` onto the window** (or click to choose it).
Needs internet once, to pull Three.js from the CDN.

## GeoJSON shape it reads

`FeatureCollection` → a `LineString` feature with `[lon, lat, alt]` vertices and
parallel `properties.speed_kmh` / `properties.t_s` arrays; ride totals in the
top-level `properties` (`dist_km`, `moving_s`, `avg_kmh`, `max_kmh`, `avg_hr`, …).
Produced by `core::session_to_geojson` in the app.

## Controls

- **PLAY / scrub** — fly through the ride (playback ≈ 6× real time).
- **CHASE CAM** — camera follows the head; off returns to the framed orbit.
- **RELIEF** — vertical exaggeration of the elevation.
- Drag to orbit, scroll to zoom.
