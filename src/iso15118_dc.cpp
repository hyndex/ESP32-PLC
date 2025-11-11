#include "iso15118_dc.h"

#if ISO20_ENABLE

#include <Arduino.h>
#include <memory>

#include "cp_control.h"
#include "dc_can.h"
#include "evse_config.h"

#if defined(HAVE_LIBISO15118)
#include <iso15118/config.hpp>
#include <iso15118/d20/config.hpp>
#include <iso15118/message/common_types.hpp>
#include <iso15118/session/feedback.hpp>
#include <iso15118/tbd_controller.hpp>

using namespace iso15118;

namespace {

std::unique_ptr<TbdController> g_iso20_controller;
bool g_iso20_started = false;

config::TlsNegotiationStrategy translate_tls_strategy() {
    switch (ISO20_TLS_STRATEGY) {
    case 1:
        return config::TlsNegotiationStrategy::ENFORCE_TLS;
    case 2:
        return config::TlsNegotiationStrategy::ENFORCE_NO_TLS;
    default:
        return config::TlsNegotiationStrategy::ACCEPT_CLIENT_OFFER;
    }
}

d20::DcTransferLimits build_dc_limits_snapshot() {
    d20::DcTransferLimits limits;
    limits.max_voltage = static_cast<uint32_t>(EVSE_MAX_VOLTAGE);
    limits.min_voltage = 0;
    limits.max_current = static_cast<uint32_t>(EVSE_MAX_CURRENT);
    limits.max_power = static_cast<uint32_t>(EVSE_MAX_POWER_KW * 1000.0f);
    limits.ramp_rate = static_cast<uint32_t>(DC_V_RAMP_V_PER_S);
    return limits;
}

d20::EvseSetupConfig build_setup_config() {
    d20::EvseSetupConfig cfg;
    cfg.evse_id = EVSE_ID;
    cfg.supported_energy_services = {message_20::datatypes::ServiceCategory::DC};
    cfg.authorization_services = {
        message_20::datatypes::Authorization::ExternalPayment,
        message_20::datatypes::Authorization::PlugAndCharge,
    };
    cfg.supported_vas_services = {};
    cfg.enable_certificate_install_service = false;
    cfg.dc_limits = build_dc_limits_snapshot();
    cfg.ac_limits = {};
    cfg.powersupply_limits = cfg.dc_limits;
    return cfg;
}

session::feedback::Callbacks build_callbacks() {
    session::feedback::Callbacks cb;
    cb.signal = [](session::feedback::Signal sig) {
        switch (sig) {
        case session::feedback::Signal::START_CABLE_CHECK:
        case session::feedback::Signal::SETUP_FINISHED:
        case session::feedback::Signal::PRE_CHARGE_STARTED:
        case session::feedback::Signal::CHARGE_LOOP_STARTED:
            break;
        case session::feedback::Signal::DC_OPEN_CONTACTOR:
            cp_contactor_command(false);
            break;
        case session::feedback::Signal::DLINK_TERMINATE:
        case session::feedback::Signal::DLINK_ERROR:
            cp_contactor_command(false);
            break;
        default:
            break;
        }
    };
    cb.dc_pre_charge_target_voltage = [](float volts) {
        dc_set_targets(volts, dc_get_set_current());
    };
    cb.dc_charge_loop_req = [](const session::feedback::DcChargeLoopReq &req) {
        if (!req.has_targets) return;
        dc_set_targets(req.target_voltage, req.target_current);
        dc_enable_output(true);
    };
    cb.dc_max_limits = [](const session::feedback::DcMaximumLimits &limits) {
        d20::DcTransferLimits snapshot = build_dc_limits_snapshot();
        if (!std::isnan(limits.voltage)) snapshot.max_voltage = static_cast<uint32_t>(limits.voltage);
        if (!std::isnan(limits.current)) snapshot.max_current = static_cast<uint32_t>(limits.current);
        if (!std::isnan(limits.power)) snapshot.max_power = static_cast<uint32_t>(limits.power);
        if (g_iso20_controller) {
            g_iso20_controller->update_dc_limits(snapshot);
        }
    };
    return cb;
}

TbdConfig build_tbd_config() {
    TbdConfig cfg;
    cfg.interface_name = ISO20_INTERFACE_NAME;
    cfg.enable_sdp_server = (ISO20_SDP_ENABLE != 0);
    cfg.tls_negotiation_strategy = translate_tls_strategy();
    cfg.ssl.backend = config::CertificateBackend::CUSTOM;
    cfg.ssl.path_certificate_chain = "/spiffs/certs/evse_chain.pem";
    cfg.ssl.path_certificate_key = "/spiffs/certs/evse_key.pem";
    cfg.ssl.path_certificate_v2g_root = "/spiffs/certs/v2g_root.pem";
    cfg.ssl.path_certificate_mo_root = "/spiffs/certs/mo_root.pem";
    return cfg;
}

} // namespace

void iso20_init() {
    auto setup = build_setup_config();
    auto callbacks = build_callbacks();
    g_iso20_controller.reset(new TbdController(build_tbd_config(), callbacks, setup));
    d20::DcTransferLimits limits = build_dc_limits_snapshot();
    g_iso20_controller->update_dc_limits(limits);
    g_iso20_controller->update_powersupply_limits(limits);
}

void iso20_loop() {
    if (!g_iso20_controller) return;
    bool connected = cp_is_connected();
    if (connected && !g_iso20_started) {
        d20::ControlEvent evt;
        evt.type = d20::ControlEvent::Type::StartCharging;
        g_iso20_controller->send_control_event(evt);
        g_iso20_started = true;
    } else if (!connected && g_iso20_started) {
        d20::ControlEvent evt;
        evt.type = d20::ControlEvent::Type::StopCharging;
        g_iso20_controller->send_control_event(evt);
        g_iso20_started = false;
    }
    g_iso20_controller->loop();
}

#else

void iso20_init() {
    Serial.println("[ISO20] ENABLED but HAVE_LIBISO15118 missing; build library to activate ISO-20.");
}

void iso20_loop() {}

#endif // defined(HAVE_LIBISO15118)

#else

#include <Arduino.h>
void iso20_init() {
    Serial.println("[ISO20] Disabled (ISO20_ENABLE=0).");
}

void iso20_loop() {}

#endif
