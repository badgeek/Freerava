# Handover: background ride recording (tasks C and A)

Written 2026-09-14. Everything below was measured on the real device
(Redmi 9, `d2f9dc4a0404`), not inferred. Read `CLAUDE.md` first — the build
recipe, the zero-Java rule and the debugging toolbox all still apply.

---

## The problem, as measured

A live ride was started, then backgrounded with HOME for 45 s:

```
t+0s    elapsed=30.0s  samples=34  trackpoints=34
t+45s   elapsed=80.0s  samples=34  trackpoints=34    <- clock +50s, GPS +0
process alive,  /proc/<pid>/oom_score_adj = 700
```

Two independent problems fall out of that:

1. **The clock advances while distance does not.** A phone in a pocket
   accumulates moving time with zero distance, so `avg_kmh` is silently
   corrupted and the saved session is wrong. This is a correctness bug and is
   true regardless of any platform behaviour. -> **task C**.
2. **Recording does not continue in the background** and the process is a prime
   kill candidate (`oom_score_adj=700`; 0 = untouchable, 1000 = killed first).
   -> **task A**.

### Honest caveat on the cause of (2)

The GPS freeze is **not** cleanly attributed yet. The test ran indoors on a
desk and the app was showing `waiting for fix…` afterwards, so it genuinely
lost signal. Android's documented background-location throttling (API 26+,
and this app has `minSdk = 26` / `targetSdk = 35`) is the *expected* cause and
is why task A exists, but this particular measurement does not isolate it.

**Do this before building anything for A:** repeat the test outdoors, walking,
screen off, for ~5 minutes, and compare `samples` growth foreground vs
background. That distinguishes throttling from a lost fix. The snapshot below
is how to read the counters without stopping the ride:

```bash
D=d2f9dc4a0404
adb -s $D exec-out run-as dev.bauhouse.cyclomp cat files/cyclomp.db     > /tmp/bg.db
adb -s $D exec-out run-as dev.bauhouse.cyclomp cat files/cyclomp.db-wal > /tmp/bg.db-wal
python3 -c "
import sqlite3; c=sqlite3.connect('/tmp/bg.db').cursor()
print(c.execute('select elapsed_s,samples,(select count(*) from active_trackpoint) from active_ride').fetchone())"
```

Both files are required: the live ride sits in the WAL, and pulling the `.db`
alone gives `no such table: active_ride`.

---

## Task C — stop the clock lying (small, do this first)

### Where

`src/core/ride_engine.cpp:43` — `RideEngine::tick`:

```cpp
live_.elapsed_s += dt_s;   // line 46: runs BEFORE the validity check
if (!s.valid) return;      // line 48: "no fix: the clock moves, the stats don't"
```

Called from the 500 ms telemetry timer at `src/main.cpp:1271`.

### The decision to make

The current behaviour is deliberate, not accidental, and **there is an existing
test asserting it**: `tests/test_ride_engine.cpp:102`
`"invalid samples advance the clock only"` expects `elapsed_s == 1.5` after one
valid and two invalid ticks. Whatever you change, that test changes with it —
do not just delete it, restate what the new contract is.

The tension: a brief GPS dropout mid-ride *should* still count as moving time
(you were moving), but an hour in a pocket should not. Some options:

- **Grace window.** Keep counting for N seconds after the last valid fix, then
  stop. Preserves tunnel/underpass behaviour, bounds the damage. Needs a
  "seconds since last valid sample" counter in the engine.
- **Only count while moving.** Advance `elapsed_s` only on valid samples (and
  perhaps only above a small speed floor). Simplest, and arguably what "moving
  time" always meant — but it silently changes every historical comparison.
- **Count separately.** Track `elapsed_s` and `moving_s` as different numbers
  and let the UI show moving time. Most honest, most work, touches
  `SessionSummary`, the DB schema and the export.

No option is obviously right; pick one deliberately and write the reasoning
into the commit. The grace window is the smallest change that fixes the actual
harm.

### Done means

- `ctest --test-dir build` green (5486 assertions at time of writing).
- The old test at `test_ride_engine.cpp:102` rewritten to state the new rule,
  plus a new case proving a long dropout no longer inflates `avg_kmh`.
- A ride recorded on the device with the phone pocketed for a few minutes does
  not come back with minutes of moving time and 0.0 km.

---

## Task A — foreground service (the real fix, and it needs Java)

### Why there is no NDK path

`Context.startForegroundService(Intent)` needs an `Intent` naming a real
`Service` component, declared in the manifest, and a `Service` must be a JVM
class. There is no NDK equivalent. JNI can *call* Java but cannot *define* the
class without a dex.

So task A breaks the project's zero-Java/Kotlin rule. That rule is in
`CLAUDE.md` as a hard constraint and the user owns it — **confirm with them
before writing the first `.java` file.** Everything else in this repo
(camera, compass, GPS, HTTP, map) genuinely avoided Java; this is the one
place where the platform refuses.

### Minimum shape

1. `android/src/main/java/dev/bauhouse/cyclomp/RideService.java` — roughly 40
   lines: `onStartCommand` builds a `NotificationChannel` + `Notification` and
   calls `startForeground(id, n, FOREGROUND_SERVICE_TYPE_LOCATION)`.
   It does **not** need to own the GPS: the existing
   `gps_telemetry_android.cpp` keeps its own `requestLocationUpdates`, and the
   service exists to raise process priority and lift the throttle.
2. `AndroidManifest.xml`:
   - `android:hasCode="false"` -> `"true"` (currently line 17)
   - `<uses-permission android:name="android.permission.FOREGROUND_SERVICE"/>`
   - `<uses-permission android:name="android.permission.FOREGROUND_SERVICE_LOCATION"/>`
     (required from API 34; targetSdk is 35)
   - `<uses-permission android:name="android.permission.POST_NOTIFICATIONS"/>`
     (runtime permission on API 33+, and the notification is mandatory)
   - `<service android:name=".RideService" android:foregroundServiceType="location" android:exported="false"/>`
3. Start it from `on_toggle_ride` when the engine leaves `Idle`, stop it in
   `on_stop_ride`. Both live in `src/main.cpp`. Start/stop over JNI from C++ —
   copy the shape of `request_permission()` in `src/camera_android.cpp:203`
   (PushLocalFrame / FindClass / CallVoidMethod / clear exceptions / pop).
4. `POST_NOTIFICATIONS` needs the same request-once-then-recheck dance as
   CAMERA and location, because **NativeActivity never forwards
   `onRequestPermissionsResult`** (see `CLAUDE.md`). Request codes used so far:
   1 = location, 2 = camera. Use 3.

### Gradle

`android/build.gradle.kts` already runs `dexBuilderDebug`, so adding Java
should not need new plugins — but it has never actually compiled a `.java` in
this project. Expect to discover something here; budget for it.

### Traps specific to this codebase

- **The activity is `android.app.NativeActivity` with no subclass.** Anything
  requiring an `Activity` reference uses `cyclomp_activity()` from
  `src/android_env.h` (the pinned `NewGlobalRef` taken in the interposed
  `ANativeActivity_onCreate`). Do not try to `FindClass` your way to the
  activity instance.
- **MIUI is hostile to background work.** Even with a correct foreground
  service, MIUI's own battery saver may kill or freeze the app unless the user
  grants "Autostart" and sets battery saver to "No restrictions" for cyclomp.
  Verify on the device and, if it matters, tell the user the setting rather
  than assuming the code is wrong.
- The device is the referee (`CLAUDE.md`): the emulator will not reproduce
  MIUI behaviour or real GNSS throttling.

### Done means

- Ride recording continues with the screen off and the app backgrounded for
  ≥10 minutes outdoors: `samples` and `active_trackpoint` keep growing at
  roughly the foreground rate.
- `oom_score_adj` drops well below 700 while a ride is running.
- The notification appears while recording and disappears on TERMINATE.
- `gradle assembleDebug` green; `ctest` still green.

---

## Option B, if the user declines Java

Keep the activity foreground instead: `FLAG_KEEP_SCREEN_ON` plus
`WindowManager.LayoutParams.screenBrightness ≈ 0.01` via JNI window
attributes. No throttling, no kill, zero Java — but the screen stays on
(battery) and the phone must stay unlocked. This was offered and not chosen;
it remains the fallback.

---

## State of the tree

Nothing here is started. Recent related work, all device-verified and
committed (no remote, nothing pushed):

| commit | what |
|---|---|
| `1cef2b1` | camera spike — NDK Camera2, zero Java |
| `4d42b8a` | camera lessons + corrected memory facts |
| `089bd6a` | `core::Photo`, EXIF thumbnail, base64 |
| `52a8bc3` | photo persistence + GeoJSON export |
| `e7edd5a` | viewer photo pins + lightbox |

Relevant existing machinery you should reuse rather than rebuild:

- **Active-ride snapshot/restore** (`core/session_db.h`, `ActiveRide`) already
  survives a background kill: saved on HOLD, every ~10 s while running, and
  from the interposed `onPause`, then restored at launch. Task A reduces how
  often that path is needed; it does not replace it.
- `MEMORY` note: the device has **5.6 GB**, not 2 GB, and the app runs at
  ~190 MB PSS with the map live. Do not design around a ceiling that is not
  there. (The 1.4 GB figure in the mbgl notes was the `theJVM` thread-storm
  bug.)
