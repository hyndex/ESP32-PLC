#pragma once

#include <cstdint>

namespace iso15118::d20 {

struct DcTransferLimits {
    uint32_t max_voltage{0};
    uint32_t min_voltage{0};
    uint32_t max_current{0};
    uint32_t max_power{0};
    uint32_t ramp_rate{0};
};

struct AcTransferLimits {
    uint32_t max_voltage{0};
    uint32_t max_current{0};
};

} // namespace iso15118::d20
