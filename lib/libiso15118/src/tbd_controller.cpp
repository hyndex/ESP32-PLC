#include "iso15118/tbd_controller.hpp"

namespace iso15118 {

TbdController::TbdController(TbdConfig cfg, session::feedback::Callbacks callbacks, d20::EvseSetupConfig setup)
    : config_(std::move(cfg)),
      callbacks_(std::move(callbacks)),
      setup_(std::move(setup)),
      dc_limits_(setup_.dc_limits),
      ac_limits_(setup_.ac_limits),
      supply_limits_(setup_.powersupply_limits) {
}

void TbdController::send_control_event(const d20::ControlEvent& evt) {
    event_queue_.push(evt);
}

void TbdController::update_authorization_services(const std::vector<message_20::datatypes::Authorization>& services,
                                                  bool cert_install_service) {
    (void)cert_install_service;
    setup_.authorization_services = services;
}

void TbdController::update_dc_limits(const d20::DcTransferLimits& limits) {
    dc_limits_ = limits;
    if (callbacks_.dc_max_limits) {
        session::feedback::DcMaximumLimits msg;
        msg.voltage = static_cast<float>(limits.max_voltage);
        msg.current = static_cast<float>(limits.max_current);
        msg.power = static_cast<float>(limits.max_power);
        callbacks_.dc_max_limits(msg);
    }
}

void TbdController::update_powersupply_limits(const d20::DcTransferLimits& limits) {
    supply_limits_ = limits;
}

void TbdController::update_energy_modes(const std::vector<message_20::datatypes::ServiceCategory>& modes) {
    setup_.supported_energy_services = modes;
}

void TbdController::update_ac_limits(const d20::AcTransferLimits& limits) {
    ac_limits_ = limits;
}

void TbdController::update_supported_vas_services(const std::vector<uint16_t>& vas_services) {
    setup_.supported_vas_services = vas_services;
}

void TbdController::handle_state() {
    switch (state_) {
    case State::Idle:
        break;
    case State::CableCheck:
        if (callbacks_.signal) callbacks_.signal(session::feedback::Signal::START_CABLE_CHECK);
        state_ = State::PreCharge;
        break;
    case State::PreCharge:
        if (callbacks_.signal) callbacks_.signal(session::feedback::Signal::PRE_CHARGE_STARTED);
        if (callbacks_.dc_pre_charge_target_voltage) {
            callbacks_.dc_pre_charge_target_voltage(static_cast<float>(supply_limits_.max_voltage));
        }
        state_ = State::ChargeLoop;
        break;
    case State::ChargeLoop:
        if (callbacks_.signal) callbacks_.signal(session::feedback::Signal::CHARGE_LOOP_STARTED);
        if (callbacks_.dc_charge_loop_req) {
            session::feedback::DcChargeLoopReq req;
            req.has_targets = true;
            req.target_voltage = static_cast<float>(dc_limits_.max_voltage);
            req.target_current = static_cast<float>(dc_limits_.max_current);
            callbacks_.dc_charge_loop_req(req);
        }
        // remain in charge loop until stop command
        break;
    }
}

void TbdController::loop() {
    while (!event_queue_.empty()) {
        const auto evt = event_queue_.front();
        event_queue_.pop();
        if (evt.type == d20::ControlEvent::Type::StartCharging) {
            state_ = State::CableCheck;
        } else if (evt.type == d20::ControlEvent::Type::StopCharging) {
            state_ = State::Idle;
            if (callbacks_.signal) callbacks_.signal(session::feedback::Signal::CHARGE_LOOP_FINISHED);
            if (callbacks_.signal) callbacks_.signal(session::feedback::Signal::DC_OPEN_CONTACTOR);
        }
    }
    handle_state();
}

} // namespace iso15118
