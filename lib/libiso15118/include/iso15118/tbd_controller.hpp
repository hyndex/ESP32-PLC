#pragma once

#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

#include <iso15118/config.hpp>
#include <iso15118/d20/config.hpp>
#include <iso15118/d20/control_event.hpp>
#include <iso15118/d20/limits.hpp>
#include <iso15118/message/common_types.hpp>
#include <iso15118/session/feedback.hpp>

namespace iso15118 {

struct TbdConfig {
    config::SSLConfig ssl;
    std::string interface_name;
    config::TlsNegotiationStrategy tls_negotiation_strategy{config::TlsNegotiationStrategy::ACCEPT_CLIENT_OFFER};
    bool enable_sdp_server{true};
};

class TbdController {
public:
    TbdController(TbdConfig cfg, session::feedback::Callbacks callbacks, d20::EvseSetupConfig setup);

    void loop();
    void send_control_event(const d20::ControlEvent& evt);

    void update_authorization_services(const std::vector<message_20::datatypes::Authorization>& services,
                                       bool cert_install_service);
    void update_dc_limits(const d20::DcTransferLimits& limits);
    void update_powersupply_limits(const d20::DcTransferLimits& limits);
    void update_energy_modes(const std::vector<message_20::datatypes::ServiceCategory>& modes);
    void update_ac_limits(const d20::AcTransferLimits& limits);
    void update_supported_vas_services(const std::vector<uint16_t>& vas_services);

private:
    enum class State { Idle, CableCheck, PreCharge, ChargeLoop };

    void handle_state();

    TbdConfig config_;
    session::feedback::Callbacks callbacks_;
    d20::EvseSetupConfig setup_;

    d20::DcTransferLimits dc_limits_;
    d20::AcTransferLimits ac_limits_;
    d20::DcTransferLimits supply_limits_;

    std::queue<d20::ControlEvent> event_queue_;
    State state_{State::Idle};
};

} // namespace iso15118
