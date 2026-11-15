#include "cardiac/dashboard.hpp"

#include <cstring>
#include <iostream>
#include <string>

// Desktop entry: `cardiac_dashboard_app [--live [name_hint]] [--beats N]
// [--session <id>] [--no-autoload] [--grid-selftest [N]]`. Default auto-loads the
// imported CHF2DB research session (chf2db-chf207) from the SQLite history DB.
// --live opens a Live session bound to the BLE HRS adapter; without a WinRT
// transport it stays idle in CONNECTING/COLLECTING and never synthesizes data.
// --grid-selftest runs N frames (default 600), prints multi-subject grid and
// frame-rate diagnostics, then exits: the proof that the grid really renders the
// cohort rather than merely compiling.
int main(int argc, char** argv) {
    cardiac::ui::DashboardOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--live") {
            options.live_mode = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.device_name_hint = argv[++i];
            }
        } else if (arg == "--beats" && i + 1 < argc) {
            options.synthetic_beats = static_cast<std::size_t>(std::stoul(argv[++i]));
        } else if (arg == "--session" && i + 1 < argc) {
            options.startup_session_id = argv[++i];
        } else if (arg == "--no-autoload") {
            options.startup_session_id.clear();
        } else if (arg == "--grid-selftest") {
            options.selftest_frames = 600;
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                options.selftest_frames = std::stoi(argv[++i]);
            }
            if (options.selftest_frames < 1) options.selftest_frames = 1;
            std::cout << "[selftest] multi-subject grid: " << options.selftest_frames
                      << " frame(s)\n";
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "usage: cardiac_dashboard_app [--live [name_hint]] [--beats N] "
                         "[--session <id>] [--no-autoload] [--grid-selftest [N]]\n";
            return 0;
        } else {
            std::cerr << "unknown arg: " << arg << "\n";
            return 1;
        }
    }
    return cardiac::ui::run_dashboard(options);
}
