#pragma once

namespace iso15118::d20 {

struct ControlEvent {
    enum class Type { StartCharging, StopCharging, Pause, Resume } type{Type::StartCharging};
};

} // namespace iso15118::d20
