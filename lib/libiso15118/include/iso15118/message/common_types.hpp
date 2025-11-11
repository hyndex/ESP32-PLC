#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace iso15118::message_20::datatypes {

enum class Authorization : uint8_t {
    ExternalPayment = 0,
    PlugAndCharge = 1,
};

enum class ServiceCategory : uint8_t {
    DC = 0,
    AC = 1,
    Internet = 2,
    Parking = 3,
};

enum class ControlMode : uint8_t {
    DynamicMode = 0,
    ScheduledMode = 1,
};

enum class MobilityNeedsMode : uint8_t {
    ChargingProfile = 0,
};

struct ServiceParameter {
    uint16_t id{0};
    std::string value;
};

struct ServiceParameterList {
    std::vector<ServiceParameter> parameters;
};

} // namespace iso15118::message_20::datatypes
