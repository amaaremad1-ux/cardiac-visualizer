// database_manager.cpp - async SQLite3 session store. See header for contract.
#include "cardiac/database_manager.hpp"

#include "cardiac/trajectory_engine.hpp" // local_triangle_area for on-load analysis

#include "sqlite3.h"

#include <chrono>
#include <cstring>
#include <utility>

namespace cardiac {
namespace {

constexpr const char* kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS sessions(
  session_id TEXT PRIMARY KEY,
  mode TEXT, source TEXT, started_utc TEXT, saved_utc TEXT, final_state TEXT,
  measurement_count INTEGER, valid_count INTEGER, trajectory_point_count INTEGER,
  area_mean_s2 REAL, ibi_mean_s REAL, ibi_sd_s REAL,
  lf_power_ms2 REAL, hf_power_ms2 REAL, lf_hf_ratio REAL, disclaimer TEXT);
CREATE TABLE IF NOT EXISTS trajectory_points(
  session_id TEXT, idx INTEGER, segment_id INTEGER, t_micros INTEGER,
  x REAL, y REAL, z REAL);
CREATE INDEX IF NOT EXISTS tp_session ON trajectory_points(session_id, idx);
CREATE TABLE IF NOT EXISTS measurements(
  session_id TEXT, idx INTEGER, t_micros INTEGER, elapsed_s REAL,
  sensor INTEGER, mtype INTEGER, interval_s REAL, hr_bpm REAL,
  is_valid INTEGER, quality_flags TEXT);
CREATE INDEX IF NOT EXISTS m_session ON measurements(session_id, idx);
)SQL";

std::int64_t to_micros(UtcTime t) {
  return std::chrono::duration_cast<std::chrono::microseconds>(t.time_since_epoch()).count();
}
UtcTime from_micros(std::int64_t us) {
  return UtcTime{std::chrono::microseconds(us)};
}

QualityFlag flag_from_name(const std::string& s) {
  static const QualityFlag all[] = {
      QualityFlag::DeviceInvalid, QualityFlag::UnknownSensor, QualityFlag::MissingInterval,
      QualityFlag::NonFiniteInterval, QualityFlag::IntervalOutOfBounds,
      QualityFlag::InvalidHeartRate, QualityFlag::InconsistentMeasurementType,
      QualityFlag::DuplicateTimestamp, QualityFlag::NonMonotonicTimestamp,
      QualityFlag::GapBefore, QualityFlag::StreamInterrupted};
  for (QualityFlag f : all) {
    if (s == to_string(f)) return f;
  }
  return QualityFlag::GapBefore;
}

std::string flags_to_text(const std::vector<QualityFlag>& flags) {
  std::string out;
  for (std::size_t i = 0; i < flags.size(); ++i) {
    if (i) out += '|';
    out += to_string(flags[i]);
  }
  return out;
}

std::vector<QualityFlag> flags_from_text(const std::string& text) {
  std::vector<QualityFlag> out;
  if (text.empty()) return out;
  std::size_t start = 0;
  while (start <= text.size()) {
    std::size_t bar = text.find('|', start);
    std::string tok = text.substr(start, bar == std::string::npos ? std::string::npos : bar - start);
    if (!tok.empty()) out.push_back(flag_from_name(tok));
    if (bar == std::string::npos) break;
    start = bar + 1;
  }
  return out;
}

} // namespace

struct DatabaseManager::Impl {
  sqlite3* db = nullptr;
};

DatabaseManager::DatabaseManager() : impl_(std::make_unique<Impl>()) {
  worker_ = std::thread([this] { worker_loop(); });
}

DatabaseManager::~DatabaseManager() { close(); }

void DatabaseManager::close() {
  {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    if (stop_.load() && !worker_.joinable()) return;
    stop_.store(true);
  }
  queue_cv_.notify_all();
  if (worker_.joinable()) worker_.join();
  if (impl_->db) {
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
  }
  ready_.store(false);
}

void DatabaseManager::enqueue(std::function<void()> job) {
  {
    std::lock_guard<std::mutex> lk(queue_mutex_);
    queue_.push_back(std::move(job));
  }
  queue_cv_.notify_one();
}

void DatabaseManager::worker_loop() {
  for (;;) {
    std::function<void()> job;
    {
      std::unique_lock<std::mutex> lk(queue_mutex_);
      queue_cv_.wait(lk, [this] { return stop_.load() || !queue_.empty(); });
      if (stop_.load() && queue_.empty()) return;
      job = std::move(queue_.front());
      queue_.erase(queue_.begin());
    }
    if (job) job();
  }
}

std::string DatabaseManager::last_error() const {
  std::lock_guard<std::mutex> lk(result_mutex_);
  return error_;
}

std::filesystem::path DatabaseManager::path() const {
  std::lock_guard<std::mutex> lk(result_mutex_);
  return path_;
}

std::uint64_t DatabaseManager::pending_jobs() const {
  std::lock_guard<std::mutex> lk(queue_mutex_);
  return static_cast<std::uint64_t>(queue_.size());
}

void DatabaseManager::open_async(const std::filesystem::path& path) {
  enqueue([this, path] {
    bool ok = do_open(path);
    {
      std::lock_guard<std::mutex> lk(result_mutex_);
      path_ = path;
      open_settled_ = true;
      if (!ok && error_.empty()) error_ = "failed to open database";
    }
    ready_.store(ok);
  });
}

bool DatabaseManager::do_open(const std::filesystem::path& path) {
  if (impl_->db) {
    sqlite3_close(impl_->db);
    impl_->db = nullptr;
  }
  std::error_code ec;
  if (path.has_parent_path()) {
    std::filesystem::create_directories(path.parent_path(), ec);
  }
  int rc = sqlite3_open(path.string().c_str(), &impl_->db);
  if (rc != SQLITE_OK) {
    std::lock_guard<std::mutex> lk(result_mutex_);
    error_ = impl_->db ? sqlite3_errmsg(impl_->db) : "sqlite3_open failed";
    return false;
  }
  sqlite3_exec(impl_->db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(impl_->db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  char* err = nullptr;
  rc = sqlite3_exec(impl_->db, kSchema, nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    std::lock_guard<std::mutex> lk(result_mutex_);
    error_ = err ? err : "schema creation failed";
    if (err) sqlite3_free(err);
    return false;
  }
  std::lock_guard<std::mutex> lk(result_mutex_);
  error_.clear();
  return true;
}

void DatabaseManager::save_session_async(StoredSession session) {
  enqueue([this, s = std::move(session)]() mutable { do_save(s); });
}

void DatabaseManager::do_save(StoredSession& s) {
  if (!impl_->db) return;
  std::lock_guard<std::mutex> lk(result_mutex_);
  error_.clear();

  auto exec = [&](const char* sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(impl_->db, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
      error_ = err ? err : "exec failed";
      if (err) sqlite3_free(err);
      return false;
    }
    return true;
  };

  exec("BEGIN;");
  {
    const char* sql =
        "INSERT OR REPLACE INTO sessions(session_id,mode,source,started_utc,saved_utc,"
        "final_state,measurement_count,valid_count,trajectory_point_count,area_mean_s2,"
        "ibi_mean_s,ibi_sd_s,lf_power_ms2,hf_power_ms2,lf_hf_ratio,disclaimer) "
        "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      const SessionRecord& r = s.record;
      sqlite3_bind_text(st, 1, r.session_id.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 2, r.mode.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 3, r.source.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 4, r.started_utc.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 5, format_utc(utc_now()).c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_text(st, 6, r.final_state.c_str(), -1, SQLITE_TRANSIENT);
      sqlite3_bind_int64(st, 7, static_cast<sqlite3_int64>(r.measurement_count));
      sqlite3_bind_int64(st, 8, static_cast<sqlite3_int64>(r.valid_count));
      sqlite3_bind_int64(st, 9, static_cast<sqlite3_int64>(r.trajectory_point_count));
      sqlite3_bind_double(st, 10, r.area_mean_s2);
      sqlite3_bind_double(st, 11, r.ibi_mean_s);
      sqlite3_bind_double(st, 12, r.ibi_sd_s);
      sqlite3_bind_double(st, 13, r.lf_power_ms2);
      sqlite3_bind_double(st, 14, r.hf_power_ms2);
      sqlite3_bind_double(st, 15, r.lf_hf_ratio);
      sqlite3_bind_text(st, 16, r.disclaimer.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) != SQLITE_DONE) error_ = sqlite3_errmsg(impl_->db);
      sqlite3_finalize(st);
    } else {
      error_ = sqlite3_errmsg(impl_->db);
    }
  }

  // Replace child rows for this session.
  {
    const char* dels[] = {"DELETE FROM trajectory_points WHERE session_id=?;",
                          "DELETE FROM measurements WHERE session_id=?;"};
    for (const char* d : dels) {
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(impl_->db, d, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, s.record.session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
        sqlite3_finalize(st);
      }
    }
  }

  // Trajectory points (batched inside the open transaction).
  {
    const char* sql = "INSERT INTO trajectory_points(session_id,idx,segment_id,t_micros,x,y,z) "
                      "VALUES(?1,?2,?3,?4,?5,?6,?7);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      for (const auto& p : s.points) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, s.record.session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(p.index));
        sqlite3_bind_int64(st, 3, static_cast<sqlite3_int64>(p.segment_id));
        sqlite3_bind_int64(st, 4, to_micros(p.timestamp_utc));
        sqlite3_bind_double(st, 5, p.position.x);
        sqlite3_bind_double(st, 6, p.position.y);
        sqlite3_bind_double(st, 7, p.position.z);
        sqlite3_step(st);
      }
      sqlite3_finalize(st);
    }
  }

  // Measurements.
  {
    const char* sql =
        "INSERT INTO measurements(session_id,idx,t_micros,elapsed_s,sensor,mtype,interval_s,"
        "hr_bpm,is_valid,quality_flags) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10);";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      std::uint64_t idx = 0;
      for (const auto& m : s.measurements) {
        sqlite3_reset(st);
        sqlite3_bind_text(st, 1, s.record.session_id.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 2, static_cast<sqlite3_int64>(idx++));
        sqlite3_bind_int64(st, 3, to_micros(m.timestamp_utc));
        sqlite3_bind_double(st, 4, m.elapsed_time_s);
        sqlite3_bind_int(st, 5, static_cast<int>(m.sensor_type));
        sqlite3_bind_int(st, 6, static_cast<int>(m.measurement_type));
        if (m.interval_s) sqlite3_bind_double(st, 7, *m.interval_s);
        else sqlite3_bind_null(st, 7);
        if (m.heart_rate_bpm) sqlite3_bind_double(st, 8, *m.heart_rate_bpm);
        else sqlite3_bind_null(st, 8);
        sqlite3_bind_int(st, 9, m.is_valid ? 1 : 0);
        std::string ft = flags_to_text(m.quality_flags);
        sqlite3_bind_text(st, 10, ft.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(st);
      }
      sqlite3_finalize(st);
    }
  }

  exec("COMMIT;");
  saved_mailbox_ = s.record.session_id;
  saved_ready_ = true;
}

void DatabaseManager::request_session_list_async() { enqueue([this] { do_list(); }); }

void DatabaseManager::request_sessions_batch_async(std::vector<std::string> session_ids) {
  enqueue([this, ids = std::move(session_ids)] { do_load_batch(ids); });
}

void DatabaseManager::do_load_batch(const std::vector<std::string>& session_ids) {
  if (!impl_->db) return;
  std::vector<StoredSession> results;
  results.reserve(session_ids.size());
  for (const auto& session_id : session_ids) {
    auto out = std::make_shared<StoredSession>();
    // Session row.
    {
      const char* sql =
          "SELECT mode,source,started_utc,saved_utc,final_state,measurement_count,"
          "valid_count,trajectory_point_count,area_mean_s2,ibi_mean_s,ibi_sd_s,"
          "lf_power_ms2,hf_power_ms2,lf_hf_ratio,disclaimer FROM sessions WHERE "
          "session_id=?;";
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
          auto ct = [&](int i) {
            const unsigned char* t = sqlite3_column_text(st, i);
            return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
          };
          out->record.session_id = session_id;
          out->record.mode = ct(0);
          out->record.source = ct(1);
          out->record.started_utc = ct(2);
          out->record.saved_utc = ct(3);
          out->record.final_state = ct(4);
          out->record.measurement_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 5));
          out->record.valid_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 6));
          out->record.trajectory_point_count =
              static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
          out->record.area_mean_s2 = sqlite3_column_double(st, 8);
          out->record.ibi_mean_s = sqlite3_column_double(st, 9);
          out->record.ibi_sd_s = sqlite3_column_double(st, 10);
          out->record.lf_power_ms2 = sqlite3_column_double(st, 11);
          out->record.hf_power_ms2 = sqlite3_column_double(st, 12);
          out->record.lf_hf_ratio = sqlite3_column_double(st, 13);
          out->record.disclaimer = ct(14);
        }
        sqlite3_finalize(st);
      }
    }
    // Trajectory points.
    {
      const char* sql =
          "SELECT idx,segment_id,t_micros,x,y,z FROM trajectory_points WHERE "
          "session_id=? ORDER BY idx;";
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
          TrajectoryPoint p;
          p.index = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
          p.segment_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
          p.timestamp_utc = from_micros(sqlite3_column_int64(st, 2));
          p.position.x = sqlite3_column_double(st, 3);
          p.position.y = sqlite3_column_double(st, 4);
          p.position.z = sqlite3_column_double(st, 5);
          out->points.push_back(std::move(p));
        }
        sqlite3_finalize(st);
      }
    }
    // Measurements.
    {
      const char* sql =
          "SELECT t_micros,elapsed_s,sensor,mtype,interval_s,hr_bpm,is_valid,"
          "quality_flags FROM measurements WHERE session_id=? ORDER BY idx;";
      sqlite3_stmt* st = nullptr;
      if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
        while (sqlite3_step(st) == SQLITE_ROW) {
          NormalizedMeasurement m;
          m.timestamp_utc = from_micros(sqlite3_column_int64(st, 0));
          m.elapsed_time_s = sqlite3_column_double(st, 1);
          m.sensor_type = static_cast<SensorType>(sqlite3_column_int(st, 2));
          m.measurement_type = static_cast<MeasurementType>(sqlite3_column_int(st, 3));
          if (sqlite3_column_type(st, 4) != SQLITE_NULL) m.interval_s = sqlite3_column_double(st, 4);
          if (sqlite3_column_type(st, 5) != SQLITE_NULL) m.heart_rate_bpm = sqlite3_column_double(st, 5);
          m.is_valid = sqlite3_column_int(st, 6) != 0;
          const unsigned char* ft = sqlite3_column_text(st, 7);
          if (ft) m.quality_flags = flags_from_text(reinterpret_cast<const char*>(ft));
          out->measurements.push_back(std::move(m));
        }
        sqlite3_finalize(st);
      }
    }
    // Recompute triangle areas deterministically from the loaded points.
    if (out->points.size() >= 3) {
      AnalysisResult ar;
      ar.revision = static_cast<std::uint64_t>(out->points.size());
      ar.triangle_areas.reserve(out->points.size());
      for (std::size_t i = 1; i + 1 < out->points.size(); ++i) {
        TriangleArea ta;
        ta.center_point_index = out->points[i].index;
        ta.segment_id = out->points[i].segment_id;
        ta.area_s2 = local_triangle_area(out->points[i - 1], out->points[i], out->points[i + 1]);
        ar.triangle_areas.push_back(ta);
      }
      out->analysis = std::move(ar);
    }
    results.push_back(std::move(*out));
  }
  std::lock_guard<std::mutex> lk(result_mutex_);
  batch_mailbox_ = std::move(results);
  batch_ready_ = true;
}

bool DatabaseManager::poll_sessions_batch(std::vector<StoredSession>& out) {
  std::lock_guard<std::mutex> lk(result_mutex_);
  if (!batch_ready_) return false;
  out = std::move(batch_mailbox_);
  batch_mailbox_.clear();
  batch_ready_ = false;
  return true;
}

void DatabaseManager::do_list() {
  if (!impl_->db) return;
  std::vector<SessionRecord> rows;
  const char* sql =
      "SELECT session_id,mode,source,started_utc,saved_utc,final_state,measurement_count,"
      "valid_count,trajectory_point_count,area_mean_s2,ibi_mean_s,ibi_sd_s,lf_power_ms2,"
      "hf_power_ms2,lf_hf_ratio,disclaimer FROM sessions ORDER BY saved_utc DESC;";
  sqlite3_stmt* st = nullptr;
  if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) != SQLITE_OK) {
    std::lock_guard<std::mutex> lk(result_mutex_);
    error_ = sqlite3_errmsg(impl_->db);
    return;
  }
  auto col_text = [&](int i) {
    const unsigned char* t = sqlite3_column_text(st, i);
    return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
  };
  while (sqlite3_step(st) == SQLITE_ROW) {
    SessionRecord r;
    r.session_id = col_text(0);
    r.mode = col_text(1);
    r.source = col_text(2);
    r.started_utc = col_text(3);
    r.saved_utc = col_text(4);
    r.final_state = col_text(5);
    r.measurement_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 6));
    r.valid_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
    r.trajectory_point_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 8));
    r.area_mean_s2 = sqlite3_column_double(st, 9);
    r.ibi_mean_s = sqlite3_column_double(st, 10);
    r.ibi_sd_s = sqlite3_column_double(st, 11);
    r.lf_power_ms2 = sqlite3_column_double(st, 12);
    r.hf_power_ms2 = sqlite3_column_double(st, 13);
    r.lf_hf_ratio = sqlite3_column_double(st, 14);
    r.disclaimer = col_text(15);
    rows.push_back(std::move(r));
  }
  sqlite3_finalize(st);
  std::lock_guard<std::mutex> lk(result_mutex_);
  list_mailbox_ = std::move(rows);
  list_ready_ = true;
}

void DatabaseManager::request_session_async(std::string session_id) {
  enqueue([this, id = std::move(session_id)] { do_load(id); });
}

void DatabaseManager::do_load(const std::string& session_id) {
  if (!impl_->db) return;
  auto out = std::make_shared<StoredSession>();
  // Session row.
  {
    const char* sql = "SELECT mode,source,started_utc,saved_utc,final_state,measurement_count,"
                      "valid_count,trajectory_point_count,area_mean_s2,ibi_mean_s,ibi_sd_s,"
                      "lf_power_ms2,hf_power_ms2,lf_hf_ratio,disclaimer FROM sessions WHERE "
                      "session_id=?;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
      if (sqlite3_step(st) == SQLITE_ROW) {
        auto ct = [&](int i) {
          const unsigned char* t = sqlite3_column_text(st, i);
          return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
        };
        out->record.session_id = session_id;
        out->record.mode = ct(0);
        out->record.source = ct(1);
        out->record.started_utc = ct(2);
        out->record.saved_utc = ct(3);
        out->record.final_state = ct(4);
        out->record.measurement_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 5));
        out->record.valid_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 6));
        out->record.trajectory_point_count = static_cast<std::uint64_t>(sqlite3_column_int64(st, 7));
        out->record.area_mean_s2 = sqlite3_column_double(st, 8);
        out->record.ibi_mean_s = sqlite3_column_double(st, 9);
        out->record.ibi_sd_s = sqlite3_column_double(st, 10);
        out->record.lf_power_ms2 = sqlite3_column_double(st, 11);
        out->record.hf_power_ms2 = sqlite3_column_double(st, 12);
        out->record.lf_hf_ratio = sqlite3_column_double(st, 13);
        out->record.disclaimer = ct(14);
      }
      sqlite3_finalize(st);
    }
  }
  // Trajectory points.
  {
    const char* sql = "SELECT idx,segment_id,t_micros,x,y,z FROM trajectory_points WHERE "
                      "session_id=? ORDER BY idx;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW) {
        TrajectoryPoint p;
        p.index = static_cast<std::uint64_t>(sqlite3_column_int64(st, 0));
        p.segment_id = static_cast<std::uint64_t>(sqlite3_column_int64(st, 1));
        p.timestamp_utc = from_micros(sqlite3_column_int64(st, 2));
        p.position.x = sqlite3_column_double(st, 3);
        p.position.y = sqlite3_column_double(st, 4);
        p.position.z = sqlite3_column_double(st, 5);
        out->points.push_back(p);
      }
      sqlite3_finalize(st);
    }
  }
  // Measurements.
  {
    const char* sql = "SELECT t_micros,elapsed_s,sensor,mtype,interval_s,hr_bpm,is_valid,"
                      "quality_flags FROM measurements WHERE session_id=? ORDER BY idx;";
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(impl_->db, sql, -1, &st, nullptr) == SQLITE_OK) {
      sqlite3_bind_text(st, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
      while (sqlite3_step(st) == SQLITE_ROW) {
        NormalizedMeasurement m;
        m.timestamp_utc = from_micros(sqlite3_column_int64(st, 0));
        m.elapsed_time_s = sqlite3_column_double(st, 1);
        m.sensor_type = static_cast<SensorType>(sqlite3_column_int(st, 2));
        m.measurement_type = static_cast<MeasurementType>(sqlite3_column_int(st, 3));
        if (sqlite3_column_type(st, 4) != SQLITE_NULL) m.interval_s = sqlite3_column_double(st, 4);
        if (sqlite3_column_type(st, 5) != SQLITE_NULL) m.heart_rate_bpm = sqlite3_column_double(st, 5);
        m.is_valid = sqlite3_column_int(st, 6) != 0;
        const unsigned char* ft = sqlite3_column_text(st, 7);
        if (ft) m.quality_flags = flags_from_text(reinterpret_cast<const char*>(ft));
        out->measurements.push_back(std::move(m));
      }
      sqlite3_finalize(st);
    }
  }
  // Recompute triangle areas deterministically from the loaded points so the
  // viewer shows the same analysis without persisting a fourth table.
  if (out->points.size() >= 3) {
    AnalysisResult ar;
    ar.revision = static_cast<std::uint64_t>(out->points.size());
    ar.triangle_areas.reserve(out->points.size());
    for (std::size_t i = 1; i + 1 < out->points.size(); ++i) {
      TriangleArea ta;
      ta.center_point_index = out->points[i].index;
      ta.segment_id = out->points[i].segment_id;
      ta.area_s2 = local_triangle_area(out->points[i - 1], out->points[i], out->points[i + 1]);
      ar.triangle_areas.push_back(ta);
    }
    out->analysis = std::move(ar);
  }
  std::lock_guard<std::mutex> lk(result_mutex_);
  session_mailbox_ = out;
  session_ready_ = true;
}

bool DatabaseManager::poll_ready() {
  std::lock_guard<std::mutex> lk(result_mutex_);
  if (open_settled_) {
    open_settled_ = false;
    return true;
  }
  return false;
}

bool DatabaseManager::poll_session_list(std::vector<SessionRecord>& out) {
  std::lock_guard<std::mutex> lk(result_mutex_);
  if (!list_ready_) return false;
  out = std::move(list_mailbox_);
  list_mailbox_.clear();
  list_ready_ = false;
  return true;
}

bool DatabaseManager::poll_session(std::shared_ptr<StoredSession>& out) {
  std::lock_guard<std::mutex> lk(result_mutex_);
  if (!session_ready_) return false;
  out = std::move(session_mailbox_);
  session_mailbox_.reset();
  session_ready_ = false;
  return true;
}

bool DatabaseManager::poll_saved(std::string& session_id_out) {
  std::lock_guard<std::mutex> lk(result_mutex_);
  if (!saved_ready_) return false;
  session_id_out = std::move(saved_mailbox_);
  saved_mailbox_.clear();
  saved_ready_ = false;
  return true;
}

} // namespace cardiac
