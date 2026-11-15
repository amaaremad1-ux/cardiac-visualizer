// import_chf2db.cpp - headless bulk importer for the PhysioNet CHF2DB IBI CSVs.
//
// Scans a data folder for *_IBI.csv exports, replays each through the SAME
// pipeline the live dashboard uses (ibi_csv_importer -> normalize/quality/
// trajectory -> HRV), and persists every session into the runtime SQLite
// database via the asynchronous DatabaseManager. All disk work happens on the
// DatabaseManager worker thread; this tool only enqueues jobs and waits for the
// ordered result mailbox, so it never blocks a UI.
//
// Usage: cardiac_import_chf2db [data_dir] [db_path]
//   defaults: data_dir = "data", db_path = "exports/cardiac_history.db"
// Idempotent: re-running replaces existing sessions (INSERT OR REPLACE).
#include "cardiac/database_manager.hpp"
#include "cardiac/ibi_csv_importer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace {

bool wait_until(const std::function<bool()>& pred, int timeout_ms = 120000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

} // namespace

int main(int argc, char** argv) {
    const std::filesystem::path data_dir = (argc > 1) ? std::filesystem::path(argv[1])
                                                      : std::filesystem::path("data");
    const std::filesystem::path db_path = (argc > 2) ? std::filesystem::path(argv[2])
                                                     : std::filesystem::path("exports/cardiac_history.db");

    std::error_code ec;
    if (!std::filesystem::is_directory(data_dir, ec)) {
        std::cerr << "[import] data folder not found: " << data_dir.string() << "\n";
        return 1;
    }

    // Collect the CHF2DB IBI exports (deterministic order by filename).
    std::vector<std::filesystem::path> csvs;
    for (const auto& e : std::filesystem::directory_iterator(data_dir, ec)) {
        if (!e.is_regular_file()) continue;
        const auto p = e.path();
        if (p.extension() != ".csv") continue;
        if (p.filename().string().find("_IBI") == std::string::npos) continue;
        csvs.push_back(p);
    }
    std::sort(csvs.begin(), csvs.end());
    if (csvs.empty()) {
        std::cerr << "[import] no *_IBI.csv files in " << data_dir.string() << "\n";
        return 1;
    }

    cardiac::DatabaseManager db;
    db.open_async(db_path);
    if (!wait_until([&] { return db.poll_ready(); })) {
        std::cerr << "[import] timed out opening database\n";
        return 1;
    }
    if (!db.ready()) {
        std::cerr << "[import] database open failed: " << db.last_error() << "\n";
        return 1;
    }
    std::cout << "[import] database ready: " << db.path().string() << "\n";
    std::cout << "[import] found " << csvs.size() << " CHF2DB IBI file(s)\n";

    // Parse + enqueue every session. Import (CPU) runs here; SQLite writes are
    // handed to the DatabaseManager worker thread (async, non-blocking).
    int enqueued = 0, failed = 0;
    for (const auto& csv : csvs) {
        std::string err;
        auto imp = cardiac::import_ibi_csv(csv, {}, &err);
        if (!imp) {
            std::cerr << "[import] SKIP " << csv.filename().string() << ": " << err << "\n";
            ++failed;
            continue;
        }
        const auto& rec = imp->session.record;
        std::cout << "[import] " << rec.source << ": " << imp->rows_imported << " beats, "
                  << imp->session.points.size() << " points, IBI mean " << rec.ibi_mean_s
                  << " s, HRV " << (imp->hrv.reliable ? "reliable" : "unreliable") << "\n";
        db.save_session_async(std::move(imp->session));
        ++enqueued;
    }

    // The list job is enqueued AFTER all saves; the worker is strictly FIFO, so
    // when the list mailbox fires, every save has committed. This is the sync
    // point - no polling of the single-slot saved mailbox needed.
    db.request_session_list_async();
    std::vector<cardiac::SessionRecord> sessions;
    if (!wait_until([&] { return db.poll_session_list(sessions); })) {
        std::cerr << "[import] timed out waiting for session list\n";
        db.close();
        return 1;
    }

    std::cout << "[import] enqueued=" << enqueued << " failed=" << failed
              << " | database now indexes " << sessions.size() << " session(s):\n";
    for (const auto& r : sessions) {
        std::cout << "    - " << r.session_id << "  [" << r.mode << "/" << r.source << "]  "
                  << r.trajectory_point_count << " pts, IBI " << r.ibi_mean_s << " s";
        if (r.lf_hf_ratio > 0.0) std::cout << ", LF/HF " << r.lf_hf_ratio;
        std::cout << "\n";
    }

    db.close();
    if (enqueued == 0) {
        std::cerr << "[import] nothing imported\n";
        return 1;
    }
    std::cout << "[import] done.\n";
    return 0;
}
