# Flight Replay Local

A local browser UI for Hawkeye ULog replay, with a small standalone launcher.
The first validated package targets macOS Apple Silicon. The web UI and WASM
assets are shared across platforms; Windows/Linux launcher code is present but
those platforms have not yet had an end-to-end package validation.

Flight Replay Local is an independent application based on Hawkeye. It is not
affiliated with or endorsed by the Hawkeye project, PX4, or Dronecode Foundation.
The upstream license and third-party notices accompany the app.

## Using the macOS package

1. Extract `Flight-Replay-Local-macos.zip` and double-click `Flight Replay Local.app`.
2. The launcher opens your default browser at a random `127.0.0.1` port.
3. Click **開啟 ULog** or drop one `.ulg` file onto the page. The first version
   accepts single files up to 100 MiB. A successfully loaded flight starts paused.
4. Use **播放 / 暫停**, drag the timeline in either direction, or click **回到起點**.
   Scrubbing pauses playback temporarily and restores its previous state on release.
5. Click **結束服務** to stop the local server, then close the browser tab.

Opening the app again while it is running reuses the existing server. If every
tab is closed, the server exits after about ten minutes without a heartbeat.
No Python, Node.js, Go or Emscripten installation is needed by the end user.
All assets are packaged locally, so network access is not needed for playback.

The macOS package is ad-hoc signed for local validation, not Developer ID signed
or notarized. This does not establish Gatekeeper acceptance on another Mac.
The launcher has no Dock window; the browser's **結束服務** button ends it.

## Data and runtime state

The browser reads the selected ULog directly with the File API. There is no
upload endpoint and the launcher does not read or modify flight logs. Its static
server binds only to `127.0.0.1`, uses a random URL token, validates the Host header,
and checks the Origin of shutdown requests. The page's connection policy allows
only same-origin requests.

The launcher stores its PID and URL in the current user's cache directory under
`FlightReplayLocal/instance.json`, with owner-only permissions. That file and its
startup lock are removed on normal shutdown. Stale startup locks are recovered
on a subsequent launch. Flight files are never included in that state.

## Build

Build requirements: a recursive repository checkout, CMake, Emscripten 6.0.10,
Go 1.24 or newer, and Python 3 for the packaging script. macOS also needs the
Command Line Tools (`codesign` and `ditto`). The validated Go compiler is 1.27.1.
Use a local build/cache directory outside cloud-synced Documents folders.

```sh
git submodule update --init --recursive
source /path/to/emsdk/emsdk_env.sh
emcmake cmake -S wasm -B /local/cache/hawkeye-wasm -DCMAKE_BUILD_TYPE=Release
cmake --build /local/cache/hawkeye-wasm -j 6
python3 local/scripts/package_macos.py \
  --wasm-build /local/cache/hawkeye-wasm \
  --output /path/to/package-output
```

The script refuses to replace an existing app. It signs in a temporary local
directory before creating the ZIP, and removes only signing-incompatible Finder
metadata from the generated copy if a cloud File Provider adds it. Repository
source files and flight logs are untouched.

For cloud-synced output folders, pass `--archive-only` and extract the signed ZIP
into a local folder such as Downloads for use. Some File Providers reattach
Finder metadata to `.app` folders after packaging, invalidating strict signature
verification of the copied bundle. The ZIP preserves the clean, signed bundle.

For development, compile `local/launcher` with `go build` and supply `--web-dir`
containing the files in `local/web` plus `engine/replay-core.{js,wasm,data}`
(rename the three `hawkeye.*` build outputs, as the packaging script does). Use
`--no-browser` and `--state-dir` to isolate automated test instances. The launcher
prints its URL to stdout.

## Tests

```sh
bash local/scripts/test_core.sh                 # synthetic regression, no log needed
bash local/scripts/test_core.sh /path/test.ulg  # additional real-log checks
(cd local/launcher && go test ./...)
```

The core test compiles the WASM replay code as a native C test executable, using
the WASM data layout. It checks sparse-topic backward seeks, start/end bounds,
non-finite input and optional real-log seek results against linear topic lookups.
The shared synthetic suite also checks frame-rate-independent timing, CAS/THR
validity and zero values, stale/future throttle, Home priority/validity and loops.
It is not a substitute for rendering tests in a browser.

For browser validation, install Playwright in a development environment (tested
with 1.62.1), start a packaged launcher, and run:

```sh
REPLAY_URL=http://127.0.0.1:PORT/TOKEN/ \
REPLAY_LOG=/path/to/flight.ulg \
REPLAY_SECOND_LOG=/path/to/another.ulg \
REPLAY_EVIDENCE=/path/to/evidence \
node local/tests/browser_smoke.cjs
```

The default browser is installed Chrome. Set `REPLAY_BROWSER=webkit` to use the
Playwright WebKit build (install it with `npx playwright install webkit`). This
is WebKit compatibility evidence, not a claim of testing the shipped Safari app.
The test exercises the file chooser, actual mouse drags, pause restoration,
keyboard synchronization, replacement/reload, invalid-log recovery, resizing,
and absence of external requests or file uploads. Evidence can contain private
filenames/screenshots: keep it outside the repository.

## Scope and known limits

- File extraction runs synchronously in WASM on the browser thread. A loading
  message is painted first; larger files can briefly block the UI. A worker or
  streaming parser is a future extension.
- Maximum WASM memory remains 512 MiB; the input file, extracted timeline and
  render resources coexist temporarily. A 100 MiB input is not a promise of
  100 MiB peak memory use.
- WebGL 2 is required. Graphics drivers, browsers and another Mac still require
  independent verification.
- Native live MAVLink, multi-file UI, auto-updates, installers and public release
  signing are outside the first version.
- The original 3D canvas and HUD remain Hawkeye's; only the outer controls are new.

WASM integration changes add an actual playback-state getter, transactional
single-log replacement, end-of-flight pause, and seek reconstruction from each
topic's latest sample. This avoids leaving sparse flight-mode/airspeed data at
a later time after scrubbing backwards. Version 0.2.0 also integrates PR #1 into native and WASM replay: CAS, optional
THR %, validated Home origin, and held-state resets on seeks/loops. See
[the telemetry contract](../REPLAY_TELEMETRY.md). A PX4 log without the custom
`replay_throttle` topic shows `--`; this is missing source data, not 0% throttle.
No logo is included in this stage.
