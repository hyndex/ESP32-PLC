#pragma once

#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace iso15118::session::feedback {

enum class Signal {
    REQUIRE_AUTH_EIM,
    START_CABLE_CHECK,
    SETUP_FINISHED,
    PRE_CHARGE_STARTED,
    CHARGE_LOOP_STARTED,
    CHARGE_LOOP_FINISHED,
    DC_OPEN_CONTACTOR,
    DLINK_TERMINATE,
    DLINK_ERROR,
    DLINK_PAUSE,
};

struct DcMaximumLimits {
    float voltage{0.0f};
    float current{0.0f};
    float power{0.0f};
};

struct DcChargeLoopReq {
    bool has_targets{false};
    float target_voltage{0.0f};
    float target_current{0.0f};
};

struct AcChargeLoopReq {
    bool placeholder{false};
};

struct Callbacks {
    std::function<void(Signal)> signal;
    std::function<void(float)> dc_pre_charge_target_voltage;
    std::function<void(const DcChargeLoopReq&)> dc_charge_loop_req;
    std::function<void(const DcMaximumLimits&)> dc_max_limits;
    std::function<void(const AcChargeLoopReq&)> ac_charge_loop_req;
    std::function<void(const std::string&)> evccid;
    std::function<void(const std::string&)> selected_protocol;

    Callbacks() = default;
};

} // namespace iso15118::session::feedback
