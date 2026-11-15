#pragma once

#include "cardiac/types.hpp"

namespace cardiac {

class DataNormalizer {
public:
    NormalizedMeasurement normalize(const RawMeasurement& raw);
private:
    std::optional<UtcTime> origin_;
};

} // namespace cardiac