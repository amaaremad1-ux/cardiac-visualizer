#include "dashboard_session.hpp"
#include "cardiac/ble_hrs_adapter.hpp"
#include "synthetic_test_adapter.hpp"
#include <chrono>
#include <cmath>
#include <vector>
namespace cardiac::ui {
namespace {
class NullBle final : public ble::BleHrsTransport {
 public:
  void start(std::function<void(const std::vector<std::uint8_t>&, UtcTime)>,
             std::function<void()>) override {}
  void stop() noexcept override {}
};
}  // namespace
void build_dashboard_session(const DashboardOptions& options,
                             std::shared_ptr<SessionController>& session,
                             std::shared_ptr<DeviceAdapter>& adapter) {
  SessionOptions so;
  so.mode = options.live_mode ? SessionMode::Live : SessionMode::SyntheticTest;
  session = std::make_shared<SessionController>(so);
  if (options.live_mode) {
    ble::BleHrsAdapterOptions bo;
    bo.device_name_hint = options.device_name_hint;
    adapter = std::make_shared<ble::BleHrsAdapter>(bo, std::make_shared<NullBle>());
    session->connect(adapter);  // idle until real WinRT transport
    return;
  }
  auto base = utc_now();
  testing::SyntheticTestAdapter::Script sc;
  sc.measurements = testing::make_script_beats(base, options.synthetic_beats, 0.85, 0.85);
  for (std::size_t i = 0; i < sc.measurements.size(); ++i) {
    const double di = static_cast<double>(i);
    double wob = 0.06 * std::sin(di * 0.7) + 0.03 * std::sin(di * 0.23);
    sc.measurements[i].interval = 0.85 + wob;
    sc.measurements[i].timestamp_utc =
        base + std::chrono::duration_cast<UtcTime::duration>(
                   std::chrono::duration<double>(0.85 * di));
  }
  if (sc.measurements.size() > 150) {
    sc.measurements[120].timestamp_utc =
        sc.measurements[119].timestamp_utc +
        std::chrono::duration_cast<UtcTime::duration>(std::chrono::duration<double>(6.0));
    sc.measurements[150].interval = 2.5;
  }
  sc.inter_sample_delay_ms = 33;
  adapter = std::make_shared<testing::SyntheticTestAdapter>(std::move(sc));
  session->connect(adapter);
}
}  // namespace cardiac::ui
