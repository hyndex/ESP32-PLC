#pragma once

#include <string>
#include <vector>

#include <iso15118/d20/limits.hpp>
#include <iso15118/message/common_types.hpp>

namespace iso15118::d20 {

struct EvseSetupConfig {
    std::string evse_id;
    std::vector<message_20::datatypes::ServiceCategory> supported_energy_services;
    std::vector<message_20::datatypes::Authorization> authorization_services;
    std::vector<uint16_t> supported_vas_services;
    bool enable_certificate_install_service{false};
    DcTransferLimits dc_limits;
    AcTransferLimits ac_limits;
    DcTransferLimits powersupply_limits;
    struct ControlMobilityPair {
        message_20::datatypes::ControlMode control;
        message_20::datatypes::MobilityNeedsMode mobility;
    };
    std::vector<ControlMobilityPair> control_mobility_modes;
};

struct PauseContext {
    bool dummy{false};
};

} // namespace iso15118::d20
