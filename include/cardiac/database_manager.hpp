#pragma once
// database_manager.hpp - asynchronous SQLite3 persistence for session history.
//
// All SQLite work happens on a single dedicated worker thread fed by a job
// queue, so the ~30 FPS UI thread NEVER blocks on disk. The UI enqueues
// operations (save / list / load) and polls mutex-protected mailboxes each
// frame; polling is a cheap flag check, never a query.
//
// Privacy: the stored schema is de-identified - only synthetic session ids,
// timestamps, geometric/statistical descriptors and the IBI series are kept.
// No names, device identifiers, or free-text notes exist in the core model and
// none are created here. The SQLite file is local application storage, NOT a
// certified medical record store; no HIPAA/GDPR compliance is claimed.
#include "cardiac/types.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cardiac {

// Row shown in the Session History Viewer.
struct SessionRecord {
  std::string session_id;
  std::string mode;    // "LIVE" / "SYNTHETIC_TEST" / "CHF2DB_RESEARCH"
  std::string source;  // e.g. "chf207" or "synthetic-demo"
  std::string started_utc;
  std::string saved_utc;
  std::string final_state;
  std::uint64_t measurement_count{0};
  std::uint64_t valid_count{0};
  std::uint64_t trajectory_point_count{0};
  double area_mean_s2{0.0};
  double ibi_mean_s{0.0};
  double ibi_sd_s{0.0};
  double lf_power_ms2{0.0};
  double hf_power_ms2{0.0};
  double lf_hf_ratio{0.0};
  std::string disclaimer;
};

// A full session as loaded from (or destined for) the database.
struct StoredSession {
  SessionRecord record;
  std::vector<TrajectoryPoint> points;
  std::vector<NormalizedMeasurement> measurements;
  std::optional<AnalysisResult> analysis;
};

class DatabaseManager {
public:
  DatabaseManager();
  ~DatabaseManager();
  DatabaseManager(const DatabaseManager&) = delete;
  DatabaseManager& operator=(const DatabaseManager&) = delete;

  // Enqueues "open (and create/migrate) the DB file" on the worker. Returns
  // immediately. Poll ready()/last_error() to learn the outcome.
  void open_async(const std::filesystem::path& path);
  // Flushes the queue and stops the worker thread (blocks only on shutdown).
  void close();

  bool ready() const { return ready_.load(); }
  std::string last_error() const;
  std::filesystem::path path() const;

  // --- asynchronous operations (all return immediately) ---  
  void request_sessions_batch_async(std::vector<std::string> session_ids);
  bool poll_sessions_batch(std::vector<StoredSession>& out);
  void save_session_async(StoredSession session);
  void request_session_list_async();
  void request_session_async(std::string session_id);

  // --- non-blocking result mailboxes for the UI thread ---
  bool poll_ready();                                  // true once open finished
  bool poll_session_list(std::vector<SessionRecord>& out);
  bool poll_session(std::shared_ptr<StoredSession>& out);
  bool poll_saved(std::string& session_id_out);

  std::uint64_t pending_jobs() const;

private:
  void worker_loop();
  void enqueue(std::function<void()> job);
  // Worker-thread-only SQLite routines.
  bool do_open(const std::filesystem::path& path);
  void do_save(StoredSession& s);
  void do_list();
  void do_load(const std::string& session_id);
  // Loads many sessions in one worker job and publishes them to the batch
  // mailbox. Used by the multi-subject grid to page through the 500+ cohort
  // without ever touching SQLite on the UI thread.
  void do_load_batch(const std::vector<std::string>& session_ids);

  struct Impl;
  std::unique_ptr<Impl> impl_;   // hides the sqlite3* handle from the header

  mutable std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::vector<std::function<void()>> queue_;
  std::thread worker_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> ready_{false};

  mutable std::mutex result_mutex_;
  std::string error_;
  std::filesystem::path path_;
  bool list_ready_{false};
  std::vector<SessionRecord> list_mailbox_;
  bool session_ready_{false};
  std::shared_ptr<StoredSession> session_mailbox_;
  bool saved_ready_{false};
  std::string saved_mailbox_;
  bool open_settled_{false};
  bool batch_ready_{false};
  std::vector<StoredSession> batch_mailbox_;
};

} // namespace cardiac
