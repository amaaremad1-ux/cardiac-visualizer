# Device Integration (Module A — Live Path)

Implemented: portable `cardiac::ble` HRS layer + live `BleHrsAdapter`.
Remaining hardware step: a WinRT `BleHrsTransport` (needs Windows SDK +
C++/WinRT headers; this machine has neither — the llvm-mingw toolchain cannot
compile `Windows.Devices.Bluetooth`, so the adapter ships with a pluggable
seam and a null transport that stays idle rather than synthesizing).

## What exists now

- `include/cardiac/ble_hrs_parser.hpp` — GATT 0x2A37 parsing, tested.
- `include/cardiac/ble_hrs_adapter.hpp` — `BleHrsTransport` seam +
  `BleHrsAdapter` (`source_mode() == Live`), tested with a fake transport.
- `SensorType::BleHrs` → `"BLE_HRS"`; default label `"BLE Heart Rate Monitor"`.
- Dashboard `--live` opens a Live session on the null transport: shows
  `CONNECTING`/`COLLECTING` idle, `DISCONNECTED` + segment break on link loss,
  `reconnect()` preserving the session. Zero synthetic samples, ever.

## WinRT transport contract (to implement with MSVC + Windows SDK)

```cpp
class WinRtBleHrsTransport : public cardiac::ble::BleHrsTransport {
    // start(): BluetoothLEAdvertisementWatcher filter ServiceUuid 0x180D
    //   (or GattDeviceService::FromIdAsync with the device hint), then
    //   GetCharacteristicsForUuidAsync(0x2A37), WriteClientCharacteristic-
    //   ConfigurationDescriptorAsync(Notify), ValueChanged -> on_notification
    //   (bytes, cardiac::utc_now()). ConnectionStatusChanged ->
    //   on_link_loss() exactly once. Catch ACCESS_DENIED -> let the adapter
    //   emit PermissionDenied. stop(): unsubscribe + close watcher/session.
};
```

Notes: RR values arrive little-endian 1/1024 s, possibly batched per
notification — the parser already reconstructs arrival-anchored beat times.
Use `cardiac::utc_now()` at notification receipt (monotonic elapsed is derived
downstream by `DataNormalizer`). Standard Windows 10/11 BLE consent UX applies;
no admin rights expected. No device address/name may be logged or exported —
only the generic sensor label.

## MSVC + Windows SDK build (for the physical WinRT transport)

Prerequisites on the Windows demo machine:

1. Visual Studio 2022 Build Tools (or full VS) with "Desktop development
   with C++", Windows 10 SDK (10.0.22621+) and/or Windows 11 SDK, and
   CMake ≥ 3.16 (`winget install Kitware.CMake`).
2. C++/WinRT headers via NuGet: `nuget install Microsoft.Windows.CppWinRT`
   (or the VSIX extension). Consume with
   `#include <winrt/Windows.Devices.Bluetooth.h>` plus the NuGet
   include/lib paths; link `windowsapp.lib`.
3. Configure and build (x64 Native Tools prompt):
   ```powershell
   cmake -S E:\CardiacVisualizer -B E:\CardiacVisualizer\build-msvc `
     -G "Visual Studio 17 2022" -A x64 `
     -DBUILD_TESTING=ON -DCARDIAC_BUILD_DASHBOARD=ON
   cmake --build E:\CardiacVisualizer\build-msvc --config Release
   ctest --test-dir E:\CardiacVisualizer\build-msvc -C Release --output-on-failure
   ```
4. Add `src/winrt_ble_transport.cpp` implementing `WinRtBleHrsTransport`
   per the contract above, and instantiate it in
   `dashboard_session.cpp` when `--live` is passed. BLE permission: first
   device access raises the standard Windows consent prompt; no admin needed.
5. Pair the watch first in Windows Settings → Bluetooth, then pass its
   advertised name: `cardiac_dashboard_app.exe --live "Watch Name"`.

