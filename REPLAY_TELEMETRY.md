# Rhyne Flight Replay: native replay changes

This is an independent research build based on Hawkeye v0.3.0
(`e8f191ce40a6516595e4c607f793f125a0aeafae`). It is not affiliated with,
authorized by, sponsored by, or approved by the Hawkeye project, PX4, or the
Dronecode Foundation. Original copyright and license notices are retained.

## What changes

- The primary HUD displays `THR` as a percentage, including a valid `0%`.
  Missing, invalid, out-of-range or stale throttle displays `--`.
- Native ULog replay reads `airspeed_validated.calibrated_airspeed_m_s`
  separately from IAS/TAS and labels it `CAS`. Invalid CAS displays `--`;
  valid zero displays `0.0`. Logs without the CAS field retain the IAS path.
  Negative or non-finite IAS/TAS values are rejected before unsigned conversion.
- Automatic scene origin uses valid Home coordinates/altitude, preserving the
  relative height of logs trimmed while airborne. Explicit `-origin` still wins.
- Data and logging messages wait for their timestamps, including sparse logs.
  All messages at a due timestamp are processed together. The final sample is
  applied before playback ends. No artificial clock topic is needed.
- Backward seeks and loops rebuild held state from the beginning, so future
  telemetry cannot leak into an earlier frame. This trades seek latency on very
  large files for correctness; state checkpoints are a possible future improvement.
- `--paused` and `--seek <seconds>` support reproducible replay inspection.
  A macOS research app containing `Contents/Resources/replay.ulg` opens that
  file offline and paused on double-click. CLI replay also works normally.
- The independent app uses its own settings directory (`rhyne-flight-replay`).

These additions target **native desktop replay**. No new CAS/throttle feature is
claimed for the separate WASM extractor, Android application, or live MAVLink
path. Without the optional replay fields the new HUD displays missing throttle.
The standard HUD can still be hidden using the existing HUD controls.

## Optional throttle topic contract

`replay_throttle` is a custom, self-describing ULog topic, not a PX4 actuator topic:

```text
uint64_t timestamp;        // publication time, FC boot microseconds
uint64_t timestamp_sample; // original/mapped source time, same clock
float throttle_pct;       // reported command in [0, 100]
bool valid;
```

The parser requires `valid=true` and finite percent in range. Samples older than
2.5 seconds, or with a future timestamp_sample, are invalid. Zero is valid.
The value is the reported command percentage; it is not measured thrust, RPM,
or necessarily the RC stick. Reconstructed inputs should use hold-last-value
resampling and keep the original sample timestamp rather than inventing ramps.

A reconstructed `VFR_HUD.airspeed` may be CAS depending on source firmware.
Verify that firmware before labeling it. Preserve missing IAS/TAS as NaN rather
than copying CAS into differently defined fields. Resampling telemetry to 5 Hz
does not increase the original measurement rate or remove receiver latency.

## Build and validation

```sh
git submodule update --init --recursive
git lfs pull                       # official flight fixtures for the full suite
cmake -S . -B build -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/hawkeye --replay /path/to/flight.ulg --paused
```

`test_sparse_replay` creates synthetic data at runtime and requires no LFS data.
It checks 30/60/144 FPS, 0.5x/1x/2x, timestamp gates, all same-time messages,
logging timing, the final sample, seeks, loops, paused data-source propagation,
missing CAS fields, NaN/negative CAS, valid zero CAS/throttle, invalid throttle
and stale throttle. `test_vehicle_telemetry` calls the actual vehicle_update()
without opening a window to check Home altitude, explicit origin, CAS, THR and
negative descent VS. Existing fixture tests account for the fixed-wing file's
real initial data gap rather than relying on the former fast-forward bug.

Only source, documentation and synthetic tests belong in this fork. Personal
flight recordings, coordinates and reconstructed outputs are kept outside it.
