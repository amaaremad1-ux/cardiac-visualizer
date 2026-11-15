#include "cardiac/session_controller.hpp"
#include "cardiac/session_exporter.hpp"
#include "cardiac/types.hpp"
#include "synthetic_test_adapter.hpp"

#include <chrono>
#include <iostream>
#include <thread>

// Core-integration harness (NOT the ImGui/OpenGL UI yet). Runs an explicit
// SYNTHETIC_TEST session through the full pipeline and exports CSV/JSON.
// Live device sessions never synthesize data; they stay idle until the real
// DeviceAdapter delivers measurements.
int main() {
    using namespace cardiac;
    using namespace cardiac::testing;
    std::cout << "CardiacVisualizer core harness (SYNTHETIC_TEST, labeled)\n";
    SessionOptions options;
    options.mode = SessionMode::SyntheticTest;
    SessionController session(options);
    const UtcTime base = utc_now();
    SyntheticTestAdapter::Script script;
    script.measurements = make_script_beats(base, 24, 0.85, 0.85);
    // One gap + one out-of-range beat to demonstrate break-not-interpolate.
    script.measurements[12].timestamp_utc =
        script.measurements[11].timestamp_utc +
        std::chrono::duration_cast<UtcTime::duration>(std::chrono::duration<double>(6.0));
    script.measurements[18].interval = 2.5;
    auto adapter = std::make_shared<SyntheticTestAdapter>(std::move(script));
    try {
        session.connect(adapter);
    } catch (const std::exception& e) {
        std::cerr << "connect failed: " << e.what() << "\n";
        return 1;
    }
    for (int i = 0; i < 100 && session.snapshot().measurements.size() < 24; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // Allow background triangle-area worker to catch the latest revision.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const auto snapshot = session.snapshot();
    const auto viz = session.visualization_snapshot();
    std::cout << "state=" << to_string(snapshot.state) << " mode=" << to_string(snapshot.mode)
              << " measurements=" << snapshot.measurements.size()
              << " points=" << viz.points.size()
              << " areas=" << (viz.analysis ? viz.analysis->triangle_areas.size() : 0) << "\n";
    SessionExporter exporter;
    try {
        const auto result = exporter.export_session(snapshot, viz.points, viz.analysis, "exports");
        std::cout << "exported\n  " << result.csv_path.string() << "\n  "
                  << result.json_path.string() << "\n";
    } catch (const std::exception& e) {
        std::cerr << "export failed: " << e.what() << "\n";
        return 1;
    }
    session.stop();
    session.disconnect();
    return 0;
}
