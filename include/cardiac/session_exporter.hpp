#pragma once

#include "cardiac/types.hpp"

#include <filesystem>
#include <string>

namespace cardiac {

struct ExportResult {
    std::filesystem::path csv_path;
    std::filesystem::path json_path;
};

// Module E: de-identified CSV + JSON metadata export. Writes only the
// normalized schema/audit fields and trajectory statistics already in memory.
// Never writes names, device identifiers, or free-text health notes because
// the core never stores them. Synthetic sessions are labeled in filenames and
// JSON so they cannot be mistaken for live device data.
class SessionExporter {
public:
    ExportResult export_session(const SessionSnapshot& snapshot,
                                const std::vector<TrajectoryPoint>& points,
                                const std::optional<AnalysisResult>& analysis,
                                const std::filesystem::path& directory);
};

} // namespace cardiac