# Live Cardiac Dynamics Visualizer — C++ Desktop App

High-performance **C++ desktop** app. Live sessions use only real device
samples; synthetic data exists solely in the explicit `SYNTHETIC_TEST` demo.

## Modules

- **A DeviceAdapter** (`include/cardiac/device_adapter.hpp`): connection,
  permission, streaming beat-event timestamps, disconnect/reconnect contract.
- **A-live BLE HRS** (`ble_hrs_parser` + `ble_hrs_adapter`): portable GATT
  0x2A37 parser (8/16-bit HR, energy flag, 1/1024 s LE RR, RR=0 skip,
  fragment drop), arrival-anchored beat reconstruction, `BLE_HRS` sensor
  label, pluggable `BleHrsTransport` (WinRT plugs in; null transport stays
  idle, never synthesizes). HR-only packets yield zero beats — never 60/HR.
- **B DataNormalizer**: schema
  `timestamp_utc, elapsed_time_s, sensor_type, measurement_type, interval_s,
  heart_rate_bpm, is_valid, quality_flags, audit_log`. Converts ms/BLE-1024
  units, derives supplemental HR as `60/IBI` only, never invents IBI from HR.
- **C QualityController**: CHF bounds **0.3–2.0 s** inclusive, duplicate and
  monotonic timestamp checks, absolute + relative gap detection, interrupts.
  Breaks trajectory segments; never interpolates.
- **D TrajectoryEngine + AsyncAnalyzer**: rolling delay embedding
  `X[i]=IBI[i], Y[i]=IBI[i-1], Z[i]=IBI[i-2]`, bounded snapshots for a 30 FPS
  renderer, background `A[i]=0.5*||cross||` areas. Geometric only — no
  diagnoses or disease scores.
- **D-ui Dashboard** (`cardiac_dashboard_app`, Dear ImGui + GLFW + OpenGL3
  vendored in `third_party/`): ~30 FPS render loop, LIVE vs SYNTHETIC_TEST
  banner, gap-split 3D trajectory (rotate/zoom/hover), rolling area stats
  (mean/p50/peak/min over adjustable window), IBI-X mean/SD, quality lane +
  area-vs-time timelines, pause/resume/reconnect-with-break/stop, export-dir
  field + Export CSV/JSON + Reveal-folder buttons.
- **SessionController**: all eight states
  (`DISCONNECTED … PAUSED/STOPPED`), pause/resume/stop, reconnect with breaks,
  mode-mismatch rejection.
- **E SessionExporter**: de-identified CSV + JSON metadata, temp session ID,
  synthetic labeling, no PII fields.

## 🎨 Web download page (Vercel)

A lightweight static landing page is deployed on Vercel so visitors can find and download the app:

- **Production URL**: https://cardiac-visualizer.vercel.app
- **Config**: `web/vercel.json` (pure static host, no framework)
- **Source**: `web/index.html` (download button, app info grid)
- **Deploy command**: `cd web && vercel deploy --prod --yes`

`index.html` ships with a placeholder download link to
`github.com/your-org/cardiac-visualizer/releases` — replace it with your real
release URL before deploying. You can also override the link at deploy time:

```powershell
cd web
vercel deploy --prod --yes --env DL_URL="https://github.com/.../latest/CardiacVisualizer.exe"
```

The page includes a `?dl=` URL param that auto-fills the download link.

## Build & test

Requires CMake ≥ 3.16 and a C++17 compiler. Core is stdlib-only; dashboard
uses vendored ImGui + GLFW (no install needed). A portable
clang/CMake toolchain is staged in `.tools/` (git-ignored).

```powershell
$cmake = 'E:\CardiacVisualizer\.tools\cmake\cmake-4.2.3-windows-x86_64\bin\cmake.exe'
$llvmBin = 'E:\CardiacVisualizer\.tools\llvm\llvm-mingw-20260421-ucrt-x86_64\bin'
$env:PATH = "$llvmBin;" + $env:PATH
& $cmake -S E:\CardiacVisualizer -B E:\CardiacVisualizer\build -G 'MinGW Makefiles' `
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON -DCARDIAC_BUILD_DASHBOARD=ON
& $cmake --build E:\CardiacVisualizer\build --config Release
Push-Location E:\CardiacVisualizer\build
& 'E:\CardiacVisualizer\.tools\cmake\cmake-4.2.3-windows-x86_64\bin\ctest.exe' --output-on-failure
.\cardiac_core_tests.exe
.\cardiac_dashboard_app.exe --help
Pop-Location
```

Run the UI:

```powershell
# One-command dev loop (like "npm run dev"):
.\dev.bat                    # build + synthetic-demo dashboard
.\dev.bat --live "Watch"     # build + LIVE BLE dashboard
.\dev.bat --test             # build + run core tests (105 checks)
.\dev.bat --configure        # force CMake re-configure, then build + run
.\dev.bat --clean            # delete the build directory
```

See `docs/DEVICE_INTEGRATION.md` for the WinRT transport seam (the one
remaining hardware step).

## Safety

- Never fabricates live measurements to keep animation active.
- Synthetic path is labeled `SYNTHETIC_TEST` in code, UI banner, files, JSON.
- Exports contain geometric descriptors only — no medical interpretation.
