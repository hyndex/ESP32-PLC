#include <Arduino.h>
#include <math.h>
#include <cstring>
#include <algorithm>
#include "main.h"
#include "ipv6.h"
#include "tcp.h"
#include "cp_control.h"
#include "dc_can.h"
#include "evse_config.h"
#ifdef ESP_PLATFORM
#include "esp_system.h"
#include "esp_timer.h"
#endif

extern "C" {
#include "cbv2g/app_handshake/appHand_Datatypes.h"
#include "cbv2g/app_handshake/appHand_Decoder.h"
#include "cbv2g/app_handshake/appHand_Encoder.h"
#include "cbv2g/common/exi_bitstream.h"
#include "cbv2g/din/din_msgDefDatatypes.h"
#include "cbv2g/din/din_msgDefDecoder.h"
#include "cbv2g/din/din_msgDefEncoder.h"
#include "cbv2g/iso_2/iso2_msgDefDatatypes.h"
#include "cbv2g/iso_2/iso2_msgDefDecoder.h"
#include "cbv2g/iso_2/iso2_msgDefEncoder.h"
}

/* Todo: implement a retry strategy, to cover the situation that single packets are lost on the way. */

#define NEXT_TCP 0x06  // the next protocol is TCP

#define TCP_FLAG_FIN 0x01
#define TCP_FLAG_SYN 0x02
#define TCP_FLAG_RST 0x04
#define TCP_FLAG_PSH 0x08
#define TCP_FLAG_ACK 0x10

#define EXI_BUFFER_SIZE 1024
#define SESSIONID_LEN 8

enum class HlcProtocol : uint8_t {
    Din = 0,
    Iso2 = 1
};

uint8_t tcpHeaderLen;
#define TCP_PAYLOAD_LEN 200
uint8_t tcpPayloadLen;
uint8_t tcpPayload[TCP_PAYLOAD_LEN];


#define TCP_ACTIVITY_TIMER_START (5*33) /* 5 seconds */
uint16_t tcpActivityTimer;

#define TCP_TRANSMIT_PACKET_LEN 200
uint8_t TcpTransmitPacketLen;
uint8_t TcpTransmitPacket[TCP_TRANSMIT_PACKET_LEN];

#define TCPIP_TRANSMIT_PACKET_LEN 200
uint8_t TcpIpRequestLen;
uint8_t TcpIpRequest[TCPIP_TRANSMIT_PACKET_LEN];

#define TCP_STATE_CLOSED 0
#define TCP_STATE_SYN_ACK 1
#define TCP_STATE_ESTABLISHED 2
#define TCP_RECEIVE_WINDOW 1000 /* number of octets we are able to receive */

uint8_t tcpState = TCP_STATE_CLOSED;
uint32_t TcpSeqNr;
uint32_t TcpAckNr;

#define TCP_RX_DATA_LEN 1000
uint8_t tcp_rxdataLen=0;
uint8_t tcp_rxdata[TCP_RX_DATA_LEN];
uint32_t expectedTcpAckNr = 0;
bool tcpAwaitingAck = false;
uint8_t lastTcpPayload[TCP_PAYLOAD_LEN];
uint8_t lastTcpPayloadLen = 0;
uint32_t lastTcpTxTimestamp = 0;
uint8_t tcpRetransmitAttempts = 0;
unsigned long tcpLastActivity = 0;
const uint32_t TCP_RETRANSMIT_TIMEOUT_MS = 1000;
const uint8_t TCP_MAX_RETRANSMIT = 3;
const uint32_t TCP_IDLE_TIMEOUT_MS = 5000;

#define stateWaitForSupportedApplicationProtocolRequest 0
#define stateWaitForSessionSetupRequest 1
#define stateWaitForServiceDiscoveryRequest 2
#define stateWaitForServicePaymentSelectionRequest 3
#define stateWaitForContractAuthenticationRequest 4
#define stateWaitForChargeParameterDiscoveryRequest 5 
#define stateWaitForCableCheckRequest 6
#define stateWaitForPreChargeRequest 7
#define stateWaitForPowerDeliveryRequest 8
#define stateWaitForCurrentDemandRequest 9
#define stateWaitForSessionStopRequest 10

uint8_t fsmState = stateWaitForSupportedApplicationProtocolRequest;

static struct din_exiDocument dinDocEnc;
static struct din_exiDocument dinDocDec;
static struct appHand_exiDocument appHandDoc;
static exi_bitstream_t g_exi_encode_stream;
static exi_bitstream_t g_exi_decode_stream;
static uint8_t g_exi_tx_buffer[EXI_BUFFER_SIZE];
static int g_exi_err = 0;
static uint8_t sessionId[SESSIONID_LEN];
static uint8_t sessionIdLen = 0;
static struct iso2_exiDocument iso2DocEnc;
static struct iso2_exiDocument iso2DocDec;
static exi_bitstream_t g_iso2_encode_stream;
static exi_bitstream_t g_iso2_decode_stream;

using dinPhysicalValueType = din_PhysicalValueType;
using dinunitSymbolType = din_unitSymbolType;
using dinDC_EVSEStatusType = din_DC_EVSEStatusType;
using dinDC_EVSEStatusCodeType = din_DC_EVSEStatusCodeType;
using dinAC_EVSEStatusType = din_AC_EVSEStatusType;
using dinCurrentDemandReqType = din_CurrentDemandReqType;

#define init_dinPhysicalValueType init_din_PhysicalValueType
#define init_dinDC_EVSEStatusType init_din_DC_EVSEStatusType
#define init_dinAC_EVSEStatusType init_din_AC_EVSEStatusType
#define init_dinMeteringReceiptResType init_din_MeteringReceiptResType
#define init_dinSessionSetupResType init_din_SessionSetupResType
#define init_dinServiceDiscoveryResType init_din_ServiceDiscoveryResType
#define init_dinServicePaymentSelectionResType init_din_ServicePaymentSelectionResType
#define init_dinContractAuthenticationResType init_din_ContractAuthenticationResType
#define init_dinChargeParameterDiscoveryResType init_din_ChargeParameterDiscoveryResType
#define init_dinCableCheckResType init_din_CableCheckResType
#define init_dinPreChargeResType init_din_PreChargeResType
#define init_dinPowerDeliveryResType init_din_PowerDeliveryResType
#define init_dinEVSEStatusType init_din_EVSEStatusType
#define init_dinCurrentDemandResType init_din_CurrentDemandResType
#define init_dinSessionStopResType init_din_SessionStopResType
#define init_dinMessageHeaderType init_din_MessageHeaderType
#define init_dinBodyType init_din_BodyType

#define dinunitSymbolType_V din_unitSymbolType_V
#define dinunitSymbolType_A din_unitSymbolType_A
#define dinunitSymbolType_W din_unitSymbolType_W
#define dinunitSymbolType_Wh din_unitSymbolType_Wh

#define dinDC_EVSEStatusCodeType_EVSE_NotReady din_DC_EVSEStatusCodeType_EVSE_NotReady
#define dinDC_EVSEStatusCodeType_EVSE_Ready din_DC_EVSEStatusCodeType_EVSE_Ready
#define dinDC_EVSEStatusCodeType_EVSE_Shutdown din_DC_EVSEStatusCodeType_EVSE_Shutdown

#define dinEVSENotificationType_None din_EVSENotificationType_None
#define dinresponseCodeType_OK din_responseCodeType_OK
#define dinresponseCodeType_OK_NewSessionEstablished din_responseCodeType_OK_NewSessionEstablished
#define dinresponseCodeType_FAILED_PowerDeliveryNotApplied din_responseCodeType_FAILED_PowerDeliveryNotApplied
#define dinpaymentOptionType_ExternalPayment din_paymentOptionType_ExternalPayment
#define dinserviceCategoryType_EVCharging din_serviceCategoryType_EVCharging
#define dinEVSESupportedEnergyTransferType_DC_extended din_EVSESupportedEnergyTransferType_DC_extended
#define dinEVSEProcessingType_Finished din_EVSEProcessingType_Finished
#define dinisolationLevelType_Valid din_isolationLevelType_Valid

#define init_iso2SessionSetupResType init_iso2_SessionSetupResType
#define init_iso2ServiceDiscoveryResType init_iso2_ServiceDiscoveryResType
#define init_iso2PaymentServiceSelectionResType init_iso2_PaymentServiceSelectionResType
#define init_iso2DC_EVSEStatusType init_iso2_DC_EVSEStatusType
#define init_iso2PaymentDetailsResType init_iso2_PaymentDetailsResType
#define init_iso2AuthorizationResType init_iso2_AuthorizationResType
#define init_iso2ChargeParameterDiscoveryResType init_iso2_ChargeParameterDiscoveryResType
#define init_iso2CableCheckResType init_iso2_CableCheckResType
#define init_iso2PreChargeResType init_iso2_PreChargeResType
#define init_iso2PowerDeliveryResType init_iso2_PowerDeliveryResType
#define init_iso2CurrentDemandResType init_iso2_CurrentDemandResType
#define init_iso2MeteringReceiptResType init_iso2_MeteringReceiptResType
#define init_iso2SessionStopResType init_iso2_SessionStopResType
#define init_iso2DC_EVSEChargeParameterType init_iso2_DC_EVSEChargeParameterType
#define init_iso2PhysicalValueType init_iso2_PhysicalValueType

typedef void (*socket_send_cb_t)(const uint8_t *data, uint16_t len);

static socket_send_cb_t g_socketSendCb = nullptr;
static HlcProtocol g_hlc_protocol = HlcProtocol::Din;
static bool g_iso_expect_payment_details = false;
static bool g_iso_payment_details_done = false;
static constexpr uint8_t kDefaultSasTupleId = 1;
static iso2_paymentOptionType g_iso_selected_payment_option = iso2_paymentOptionType_ExternalPayment;

void tcp_register_socket_sender(socket_send_cb_t cb) {
    g_socketSendCb = cb;
}

static void tcp_bufferPayload(const uint8_t *payload, uint16_t len, bool fromSocket);
void tcp_retransmitPendingPayload(void);
void tcp_tick(void);
void resetHlcSession(void);
static void setPhysicalValue(dinPhysicalValueType *value, dinunitSymbolType unit, int16_t magnitude, int8_t multiplier);
static void populateDcEvseStatus(dinDC_EVSEStatusType *status, dinDC_EVSEStatusCodeType code);
static void populateAcEvseStatus(dinAC_EVSEStatusType *status);
static bool handleMeteringReceipt(void);
static void iso_set_physical_value(iso2_PhysicalValueType *value, iso2_unitSymbolType unit, float magnitude);
static float iso_decode_physical_value(const iso2_PhysicalValueType &value);
static void iso_populate_dc_evse_status(iso2_DC_EVSEStatusType *status, iso2_DC_EVSEStatusCodeType code);
static iso2_DC_EVSEStatusCodeType iso_current_evse_status_code(void);
static void iso_set_evse_id(char *buffer, uint16_t &len, size_t maxLen);
static bool handle_iso_metering_receipt(void);
static void stop_evse_power_output(void);
static void iso_watchdog_start(uint8_t state);
static void iso_watchdog_clear(void);
static void iso_watchdog_check(void);
static bool decode_iso2_message(void);
static void prepare_iso2_message(void);
static bool send_iso2_message(void);

const int16_t EVSE_PRESENT_VOLTAGE = 400;
const int16_t EVSE_PRESENT_CURRENT = 0;
bool chargingActive = false;
static uint32_t g_iso_state_start_ms = 0;
static uint8_t g_iso_state_watchdog = 0;
static uint8_t g_iso_watchdog_retries = 0;
#ifndef ISO_STATE_TIMEOUT_MS
#define ISO_STATE_TIMEOUT_MS 4000UL
#endif
#ifndef ISO_STATE_WATCHDOG_MAX_RETRIES
#define ISO_STATE_WATCHDOG_MAX_RETRIES 3
#endif

static float decodePhysicalValue(const dinPhysicalValueType &value) {
    float scale = powf(10.0f, (float)value.Multiplier);
    return (float)value.Value * scale;
}

static void encodePhysicalValue(dinPhysicalValueType *dst, dinunitSymbolType unit, float val) {
    init_dinPhysicalValueType(dst);
    dst->Unit = unit;
    dst->Unit_isUsed = 1;
    dst->Multiplier = -1;
    dst->Value = (int16_t)lroundf(val * 10.0f);
}

static dinDC_EVSEStatusCodeType currentEvseStatusCode() {
    if (!cp_is_connected()) return dinDC_EVSEStatusCodeType_EVSE_NotReady;
    if (chargingActive && cp_contactor_feedback()) return dinDC_EVSEStatusCodeType_EVSE_Ready;
    if (!cp_contactor_feedback()) return dinDC_EVSEStatusCodeType_EVSE_Shutdown;
    return dinDC_EVSEStatusCodeType_EVSE_Ready;
}


void routeDecoderInputData(void) {
    if (tcp_rxdataLen <= V2GTP_HEADER_SIZE) {
        g_exi_err = -1;
        return;
    }
    uint8_t *payload = tcp_rxdata + V2GTP_HEADER_SIZE;
    size_t payloadLen = tcp_rxdataLen - V2GTP_HEADER_SIZE;
    if (fsmState == stateWaitForSupportedApplicationProtocolRequest) {
        exi_bitstream_init(&g_exi_decode_stream, payload, payloadLen, 0, nullptr);
        return;
    }
    if (g_hlc_protocol == HlcProtocol::Iso2) {
        exi_bitstream_init(&g_iso2_decode_stream, payload, payloadLen, 0, nullptr);
    } else {
        exi_bitstream_init(&g_exi_decode_stream, payload, payloadLen, 0, nullptr);
    }
}

static void setPhysicalValue(dinPhysicalValueType *value, dinunitSymbolType unit, int16_t magnitude, int8_t multiplier) {
    init_dinPhysicalValueType(value);
    value->Multiplier = multiplier;
    if (unit <= dinunitSymbolType_Wh) {
        value->Unit = unit;
        value->Unit_isUsed = 1;
    } else {
        value->Unit_isUsed = 0;
    }
    value->Value = magnitude;
}

static void populateDcEvseStatus(dinDC_EVSEStatusType *status, dinDC_EVSEStatusCodeType code) {
    init_dinDC_EVSEStatusType(status);
    status->EVSEIsolationStatus = dinisolationLevelType_Valid;
    status->EVSEIsolationStatus_isUsed = 1;
    status->EVSEStatusCode = code;
    status->NotificationMaxDelay = 0;
    status->EVSENotification = dinEVSENotificationType_None;
}

static void populateAcEvseStatus(dinAC_EVSEStatusType *status) {
    init_dinAC_EVSEStatusType(status);
    status->PowerSwitchClosed = cp_contactor_feedback();
    status->RCD = 0;
    status->NotificationMaxDelay = 0;
    status->EVSENotification = dinEVSENotificationType_None;
}

static void iso_set_physical_value(iso2_PhysicalValueType *value, iso2_unitSymbolType unit, float magnitude) {
    if (!value) return;
    init_iso2PhysicalValueType(value);
    value->Unit = unit;
    value->Multiplier = -1;
    value->Value = (int16_t)lroundf(magnitude * 10.0f);
}

static float iso_decode_physical_value(const iso2_PhysicalValueType &value) {
    float scale = powf(10.0f, (float)value.Multiplier);
    return (float)value.Value * scale;
}

static void iso_populate_dc_evse_status(iso2_DC_EVSEStatusType *status, iso2_DC_EVSEStatusCodeType code) {
    if (!status) return;
    init_iso2DC_EVSEStatusType(status);
    status->NotificationMaxDelay = 0;
    status->EVSENotification = iso2_EVSENotificationType_None;
    status->EVSEStatusCode = code;
    status->EVSEIsolationStatus = iso2_isolationLevelType_Valid;
    status->EVSEIsolationStatus_isUsed = 1;
}

static iso2_DC_EVSEStatusCodeType iso_current_evse_status_code(void) {
    if (!cp_is_connected()) return iso2_DC_EVSEStatusCodeType_EVSE_NotReady;
    if (!cp_contactor_feedback()) return iso2_DC_EVSEStatusCodeType_EVSE_Shutdown;
    return iso2_DC_EVSEStatusCodeType_EVSE_Ready;
}

static void iso_set_evse_id(char *buffer, uint16_t &len, size_t maxLen) {
    if (!buffer || maxLen == 0) {
        len = 0;
        return;
    }
    const char *evseId = EVSE_ID;
    size_t copyLen = strnlen(evseId, maxLen);
    memcpy(buffer, evseId, copyLen);
    len = copyLen;
}

static void stop_evse_power_output(void) {
    chargingActive = false;
    dc_set_targets(0.0f, 0.0f);
    dc_enable_output(false);
    cp_contactor_command(false);
}

static void iso_watchdog_start(uint8_t state) {
    if (g_hlc_protocol != HlcProtocol::Iso2) return;
    g_iso_state_watchdog = state;
    g_iso_state_start_ms = millis();
    g_iso_watchdog_retries = 0;
}

static void iso_watchdog_clear(void) {
    g_iso_state_watchdog = 0;
    g_iso_state_start_ms = 0;
    g_iso_watchdog_retries = 0;
}

static void iso_watchdog_check(void) {
    if (g_hlc_protocol != HlcProtocol::Iso2) return;
    if (g_iso_state_watchdog == 0) return;
    uint32_t now = millis();
    if ((int32_t)(now - g_iso_state_start_ms) > (int32_t)ISO_STATE_TIMEOUT_MS) {
        g_iso_watchdog_retries++;
        Serial.printf("[ISO-2] State %u watchdog timeout (retry %u/%u)\n",
                      g_iso_state_watchdog,
                      g_iso_watchdog_retries,
                      (unsigned)ISO_STATE_WATCHDOG_MAX_RETRIES);
        resetHlcSession();
        if (g_iso_watchdog_retries >= ISO_STATE_WATCHDOG_MAX_RETRIES) {
            Serial.println("[ISO-2] Watchdog retries exhausted, forcing transport reset");
            tcp_transport_reset();
            g_iso_watchdog_retries = 0;
        }
    }
}

static iso2_responseCodeType iso_process_power_delivery(iso2_chargeProgressType progress,
                                                        bool &contactorOk,
                                                        uint8_t &nextState) {
    contactorOk = true;
    if (progress == iso2_chargeProgressType_Start) {
        contactorOk = cp_contactor_command(true);
        if (contactorOk) {
            chargingActive = true;
            dc_enable_output(true);
        } else {
            stop_evse_power_output();
        }
        nextState = contactorOk ? stateWaitForCurrentDemandRequest : stateWaitForSessionStopRequest;
    } else if (progress == iso2_chargeProgressType_Stop) {
        stop_evse_power_output();
        nextState = stateWaitForSessionStopRequest;
    } else { // Renegotiate
        stop_evse_power_output();
        nextState = stateWaitForChargeParameterDiscoveryRequest;
    }
    return contactorOk ? iso2_responseCodeType_OK : iso2_responseCodeType_FAILED_PowerDeliveryNotApplied;
}

static bool decode_iso2_message() {
    exi_bitstream_reset(&g_iso2_decode_stream);
    g_exi_err = decode_iso2_exiDocument(&g_iso2_decode_stream, &iso2DocDec);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] ISO-2 decode failed (%d)\n", g_exi_err);
        return false;
    }
    return true;
}

static void prepare_iso2_message(void) {
    init_iso2_exiDocument(&iso2DocEnc);
    init_iso2_V2G_Message(&iso2DocEnc.V2G_Message);
    init_iso2_MessageHeaderType(&iso2DocEnc.V2G_Message.Header);
    init_iso2_BodyType(&iso2DocEnc.V2G_Message.Body);
    if (sessionIdLen > 0 && sessionIdLen <= SESSIONID_LEN) {
        memcpy(iso2DocEnc.V2G_Message.Header.SessionID.bytes, sessionId, sessionIdLen);
        iso2DocEnc.V2G_Message.Header.SessionID.bytesLen = sessionIdLen;
    } else {
        iso2DocEnc.V2G_Message.Header.SessionID.bytesLen = 0;
    }
}

static bool send_iso2_message(void) {
    exi_bitstream_init(&g_iso2_encode_stream, g_exi_tx_buffer, sizeof(g_exi_tx_buffer), 0, nullptr);
    g_exi_err = encode_iso2_exiDocument(&g_iso2_encode_stream, &iso2DocEnc);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] ISO-2 encode failed (%d)\n", g_exi_err);
        return false;
    }
    size_t exiLen = exi_bitstream_get_length(&g_iso2_encode_stream);
    if (exiLen > UINT16_MAX) {
        Serial.println("[EXI] ISO-2 frame too large");
        return false;
    }
    addV2GTPHeaderAndTransmit(g_exi_tx_buffer, static_cast<uint16_t>(exiLen));
    return true;
}


static bool decode_handshake_message() {
    exi_bitstream_reset(&g_exi_decode_stream);
    g_exi_err = decode_appHand_exiDocument(&g_exi_decode_stream, &appHandDoc);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] Handshake decode failed (%d)\n", g_exi_err);
        return false;
    }
    return true;
}

static bool decode_din_message() {
    exi_bitstream_reset(&g_exi_decode_stream);
    g_exi_err = decode_din_exiDocument(&g_exi_decode_stream, &dinDocDec);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] DIN decode failed (%d)\n", g_exi_err);
        return false;
    }
    return true;
}

static void prepare_din_message(void) {
    init_din_exiDocument(&dinDocEnc);
    init_din_V2G_Message(&dinDocEnc.V2G_Message);
    init_dinMessageHeaderType(&dinDocEnc.V2G_Message.Header);
    init_dinBodyType(&dinDocEnc.V2G_Message.Body);
    if (sessionIdLen > 0 && sessionIdLen <= SESSIONID_LEN) {
        memcpy(dinDocEnc.V2G_Message.Header.SessionID.bytes, sessionId, sessionIdLen);
        dinDocEnc.V2G_Message.Header.SessionID.bytesLen = sessionIdLen;
    } else {
        dinDocEnc.V2G_Message.Header.SessionID.bytesLen = 0;
    }
}

static bool send_din_message(void) {
    exi_bitstream_init(&g_exi_encode_stream, g_exi_tx_buffer, sizeof(g_exi_tx_buffer), 0, nullptr);
    g_exi_err = encode_din_exiDocument(&g_exi_encode_stream, &dinDocEnc);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] DIN encode failed (%d)\n", g_exi_err);
        return false;
    }
    size_t exiLen = exi_bitstream_get_length(&g_exi_encode_stream);
    if (exiLen > UINT16_MAX) {
        Serial.println("[EXI] Encoded DIN frame too large");
        return false;
    }
    addV2GTPHeaderAndTransmit(g_exi_tx_buffer, static_cast<uint16_t>(exiLen));
    return true;
}

static bool send_supported_app_protocol_response(uint8_t schemaId) {
    struct appHand_exiDocument resp;
    init_appHand_exiDocument(&resp);
    resp.supportedAppProtocolRes_isUsed = 1;
    init_appHand_supportedAppProtocolRes(&resp.supportedAppProtocolRes);
    resp.supportedAppProtocolRes.ResponseCode = appHand_responseCodeType_OK_SuccessfulNegotiation;
    resp.supportedAppProtocolRes.SchemaID = schemaId;
    resp.supportedAppProtocolRes.SchemaID_isUsed = 1;
    exi_bitstream_init(&g_exi_encode_stream, g_exi_tx_buffer, sizeof(g_exi_tx_buffer), 0, nullptr);
    g_exi_err = encode_appHand_exiDocument(&g_exi_encode_stream, &resp);
    if (g_exi_err != 0) {
        Serial.printf("[EXI] Handshake encode failed (%d)\n", g_exi_err);
        return false;
    }
    size_t exiLen = exi_bitstream_get_length(&g_exi_encode_stream);
    addV2GTPHeaderAndTransmit(g_exi_tx_buffer, static_cast<uint16_t>(exiLen));
    return true;
}

static bool handleMeteringReceipt(void) {
    if (!dinDocDec.V2G_Message.Body.MeteringReceiptReq_isUsed) {
        return false;
    }
    prepare_din_message();
    dinDocEnc.V2G_Message.Body.MeteringReceiptRes_isUsed = 1;
    init_dinMeteringReceiptResType(&dinDocEnc.V2G_Message.Body.MeteringReceiptRes);
    dinDocEnc.V2G_Message.Body.MeteringReceiptRes.ResponseCode = dinresponseCodeType_OK;
    populateAcEvseStatus(&dinDocEnc.V2G_Message.Body.MeteringReceiptRes.AC_EVSEStatus);
    send_din_message();
    return true;
}

static bool handle_iso_metering_receipt(void) {
    if (!iso2DocDec.V2G_Message.Body.MeteringReceiptReq_isUsed) {
        return false;
    }
    prepare_iso2_message();
    iso2DocEnc.V2G_Message.Body.MeteringReceiptRes_isUsed = 1;
    init_iso2MeteringReceiptResType(&iso2DocEnc.V2G_Message.Body.MeteringReceiptRes);
    auto &res = iso2DocEnc.V2G_Message.Body.MeteringReceiptRes;
    res.ResponseCode = iso2_responseCodeType_OK;
    res.AC_EVSEStatus_isUsed = 0;
    res.DC_EVSEStatus_isUsed = 1;
    iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
    send_iso2_message();
    return true;
}

void resetHlcSession(void) {
    fsmState = stateWaitForSupportedApplicationProtocolRequest;
    stop_evse_power_output();
    tcpAwaitingAck = false;
    lastTcpPayloadLen = 0;
    expectedTcpAckNr = 0;
    tcpRetransmitAttempts = 0;
    tcp_rxdataLen = 0;
    tcpLastActivity = 0;
    g_hlc_protocol = HlcProtocol::Din;
    g_iso_expect_payment_details = false;
    g_iso_payment_details_done = false;
    iso_watchdog_clear();
}

static void tcp_bufferPayload(const uint8_t *payload, uint16_t len, bool fromSocket) {
    if (!len) return;
    if ((tcp_rxdataLen + len) > TCP_RX_DATA_LEN) {
        Serial.printf("TCP RX buffer overflow (%u)\n", tcp_rxdataLen + len);
        tcp_rxdataLen = 0;
        resetHlcSession();
        return;
    }
    memcpy(tcp_rxdata + tcp_rxdataLen, payload, len);
    tcp_rxdataLen += len;

    while (tcp_rxdataLen >= V2GTP_HEADER_SIZE) {
        if (tcp_rxdata[0] != 0x01 || tcp_rxdata[1] != 0xFE) {
            Serial.printf("Invalid V2GTP header\n");
            tcp_rxdataLen = 0;
            resetHlcSession();
            return;
        }
        uint16_t payloadType = (tcp_rxdata[2] << 8) | tcp_rxdata[3];
        uint32_t payloadLen = ((uint32_t)tcp_rxdata[4] << 24) |
                              ((uint32_t)tcp_rxdata[5] << 16) |
                              ((uint32_t)tcp_rxdata[6] << 8) |
                              (uint32_t)tcp_rxdata[7];
        uint32_t frameLen = V2GTP_HEADER_SIZE + payloadLen;
        if (frameLen > TCP_RX_DATA_LEN) {
            Serial.printf("V2GTP frame too large (%u)\n", (unsigned int)frameLen);
            tcp_rxdataLen = 0;
            resetHlcSession();
            return;
        }
        if (tcp_rxdataLen < frameLen) {
            break; // wait for more data
        }
        if (payloadType != 0x8001) {
            Serial.printf("Unsupported V2GTP payload type 0x%04x\n", payloadType);
            tcp_rxdataLen = 0;
            resetHlcSession();
            return;
        }
        uint16_t frameLen16 = (uint16_t)frameLen;
        uint16_t remainderLen = tcp_rxdataLen - frameLen16;
        tcp_rxdataLen = frameLen16;
        decodeV2GTP();
        if (remainderLen) {
            memmove(tcp_rxdata, tcp_rxdata + frameLen16, remainderLen);
        }
        tcp_rxdataLen = remainderLen;
    }
}

void tcp_process_socket_payload(const uint8_t *payload, uint16_t len) {
    tcpLastActivity = millis();
    tcp_bufferPayload(payload, len, true);
}

void tcp_transport_reset(void) {
    resetHlcSession();
    tcpState = TCP_STATE_CLOSED;
}

void tcp_transport_connected(void) {
    tcpState = TCP_STATE_ESTABLISHED;
    tcpLastActivity = millis();
}

void tcp_retransmitPendingPayload(void) {
    if (!tcpAwaitingAck || lastTcpPayloadLen == 0) return;
    if ((lastTcpPayloadLen + 20) >= TCP_TRANSMIT_PACKET_LEN) return;
    tcpHeaderLen = 20;
    tcpPayloadLen = lastTcpPayloadLen;
    memcpy(tcpPayload, lastTcpPayload, lastTcpPayloadLen);
    memcpy(&TcpTransmitPacket[tcpHeaderLen], tcpPayload, tcpPayloadLen);
    tcp_prepareTcpHeader(TCP_FLAG_PSH | TCP_FLAG_ACK);
    tcp_packRequestIntoIp();
    lastTcpTxTimestamp = millis();
}

void tcp_tick(void) {
    unsigned long now = millis();
    if (tcpState == TCP_STATE_ESTABLISHED) {
        if (tcpAwaitingAck && lastTcpPayloadLen > 0 &&
            (now - lastTcpTxTimestamp) > TCP_RETRANSMIT_TIMEOUT_MS) {
            if (tcpRetransmitAttempts < TCP_MAX_RETRANSMIT) {
                tcpRetransmitAttempts++;
                Serial.printf("TCP retransmit attempt %u\n", tcpRetransmitAttempts);
                tcp_retransmitPendingPayload();
            } else {
                Serial.printf("TCP retransmit limit reached\n");
                tcpState = TCP_STATE_CLOSED;
                tcpAwaitingAck = false;
                resetHlcSession();
            }
        }
        if (tcpLastActivity && (now - tcpLastActivity) > TCP_IDLE_TIMEOUT_MS) {
            Serial.printf("TCP idle timeout\n");
            tcpState = TCP_STATE_CLOSED;
            tcpAwaitingAck = false;
            resetHlcSession();
        }
    }
    iso_watchdog_check();
}


void tcp_transmit(void) {
  //showAsHex(tcpPayload, tcpPayloadLen, "tcp_transmit");
  if (tcpState == TCP_STATE_ESTABLISHED) {  
    //addToTrace("[TCP] sending data");
    tcpHeaderLen = 20; /* 20 bytes normal header, no options */
    if (tcpPayloadLen+tcpHeaderLen < TCP_TRANSMIT_PACKET_LEN) {    
      memcpy(&TcpTransmitPacket[tcpHeaderLen], tcpPayload, tcpPayloadLen);
      tcp_prepareTcpHeader(TCP_FLAG_PSH + TCP_FLAG_ACK); /* data packets are always sent with flags PUSH and ACK. */
      tcp_packRequestIntoIp();
      if (tcpPayloadLen > 0) {
        memcpy(lastTcpPayload, tcpPayload, tcpPayloadLen);
        lastTcpPayloadLen = tcpPayloadLen;
        expectedTcpAckNr = TcpSeqNr + tcpPayloadLen;
        tcpAwaitingAck = true;
        tcpRetransmitAttempts = 0;
        lastTcpTxTimestamp = millis();
      }
    } else {
      Serial.printf("Error: tcpPayload and header do not fit into TcpTransmitPacket.\n");
    }      
  }  
}


void addV2GTPHeaderAndTransmit(const uint8_t *exiBuffer, uint16_t exiBufferLen) {
    // takes the bytearray with exidata, and adds a header to it, according to the Vehicle-to-Grid-Transport-Protocol
    // V2GTP header has 8 bytes
    // 1 byte protocol version
    // 1 byte protocol version inverted
    // 2 bytes payload type
    // 4 byte payload length
    tcpPayload[0] = 0x01; // version
    tcpPayload[1] = 0xfe; // version inverted
    tcpPayload[2] = 0x80; // payload type. 0x8001 means "EXI data"
    tcpPayload[3] = 0x01; // 
    tcpPayload[4] = (uint8_t)(exiBufferLen >> 24); // length 4 byte.
    tcpPayload[5] = (uint8_t)(exiBufferLen >> 16);
    tcpPayload[6] = (uint8_t)(exiBufferLen >> 8);
    tcpPayload[7] = (uint8_t)exiBufferLen;
    if (exiBufferLen + 8 < TCP_PAYLOAD_LEN) {
        memcpy(tcpPayload+8, exiBuffer, exiBufferLen);
        tcpPayloadLen = 8 + exiBufferLen; /* 8 byte V2GTP header, plus the EXI data */
        //log_v("Step3 %d", tcpPayloadLen);
        //showAsHex(tcpPayload, tcpPayloadLen, "tcpPayload");
        if (g_socketSendCb) {
            g_socketSendCb(tcpPayload, tcpPayloadLen);
        } else {
            tcp_transmit();
        }
    } else {
        Serial.printf("Error: EXI does not fit into tcpPayload.\n");
    }
}


void decodeV2GTP(void) {

    uint16_t arrayLen, i;
    uint8_t strNamespace[50];
    uint8_t SchemaID, n;
    uint16_t NamespaceLen;


    routeDecoderInputData();
    bool decodeOk = false;
    if (fsmState == stateWaitForSupportedApplicationProtocolRequest) {
        decodeOk = decode_handshake_message();
    } else if (g_hlc_protocol == HlcProtocol::Iso2) {
        decodeOk = decode_iso2_message();
    } else {
        decodeOk = decode_din_message();
    }
    if (!decodeOk) {
        tcp_rxdataLen = 0;
        return;
    }
    tcp_rxdataLen = 0; /* mark the input data as "consumed" */

    if (fsmState == stateWaitForSupportedApplicationProtocolRequest) {

        // Check if we have received the correct message
        if (appHandDoc.supportedAppProtocolReq_isUsed) {
        
            Serial.printf("SupportedApplicationProtocolRequest\n");
            // process data when no errors occured during decoding
            if (g_exi_err == 0) {
                arrayLen = appHandDoc.supportedAppProtocolReq.AppProtocol.arrayLen;
                Serial.printf("The car supports %u schemas.\n", arrayLen);
            
                // check all schemas for DIN
                for(n=0; n<arrayLen; n++) {
                    memset(strNamespace, 0, sizeof(strNamespace));
                    NamespaceLen = appHandDoc.supportedAppProtocolReq.AppProtocol.array[n].ProtocolNamespace.charactersLen;
                    SchemaID = appHandDoc.supportedAppProtocolReq.AppProtocol.array[n].SchemaID;
                    for (i=0; i< NamespaceLen; i++) {
                        strNamespace[i] = appHandDoc.supportedAppProtocolReq.AppProtocol.array[n].ProtocolNamespace.characters[i];    
                    }
                    Serial.printf("strNameSpace %s SchemaID: %u\n", strNamespace, SchemaID);

                    if (strstr((const char*)strNamespace, ":din:70121:") != NULL) {
                        Serial.printf("Detected DIN\n");
                        g_hlc_protocol = HlcProtocol::Din;
                        if (send_supported_app_protocol_response(SchemaID)) {
                            fsmState = stateWaitForSessionSetupRequest;
                            iso_watchdog_start(stateWaitForSessionSetupRequest);
                        }
                    } else if (strstr((const char*)strNamespace, ":iso:15118:2") != NULL) {
                        Serial.printf("Detected ISO 15118-2\n");
                        g_hlc_protocol = HlcProtocol::Iso2;
                        if (send_supported_app_protocol_response(SchemaID)) {
                            fsmState = stateWaitForSessionSetupRequest;
                            iso_watchdog_start(stateWaitForSessionSetupRequest);
                        }
                    }
                }
            }
        }

    } else if (fsmState == stateWaitForSessionSetupRequest) {
        
        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.SessionSetupReq_isUsed) {
                Serial.printf("ISO SessionSetupReq\n");

                sessionIdLen = SESSIONID_LEN;
                for (i = 0; i < sessionIdLen; ++i) {
                    sessionId[i] = (uint8_t)random(256);
                }

                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.SessionSetupRes_isUsed = 1;
                init_iso2SessionSetupResType(&iso2DocEnc.V2G_Message.Body.SessionSetupRes);
                iso2DocEnc.V2G_Message.Body.SessionSetupRes.ResponseCode = iso2_responseCodeType_OK_NewSessionEstablished;
                const char *evseId = EVSE_ID;
                size_t idLen = strnlen(evseId, iso2_EVSEID_CHARACTER_SIZE - 1);
                memcpy(iso2DocEnc.V2G_Message.Body.SessionSetupRes.EVSEID.characters, evseId, idLen);
                iso2DocEnc.V2G_Message.Body.SessionSetupRes.EVSEID.charactersLen = idLen;
                iso2DocEnc.V2G_Message.Body.SessionSetupRes.EVSETimeStamp_isUsed = 0;
                send_iso2_message();
                fsmState = stateWaitForServiceDiscoveryRequest;
                iso_watchdog_start(stateWaitForServiceDiscoveryRequest);
                return;
            }
        }

        // Check if we have received the correct message
        if (dinDocDec.V2G_Message.Body.SessionSetupReq_isUsed) {

            Serial.printf("SessionSetupReqest\n");

            //n = dinDocDec.V2G_Message.Header.SessionID.bytesLen;
            //for (i=0; i< n; i++) {
            //    Serial.printf("%02x", dinDocDec.V2G_Message.Header.SessionID.bytes[i] );
            //}
            n = dinDocDec.V2G_Message.Body.SessionSetupReq.EVCCID.bytesLen;
            if (n>6) n=6;       // out of range check
            Serial.printf("EVCCID=");
            for (i=0; i<n; i++) {
                EVCCID[i]= dinDocDec.V2G_Message.Body.SessionSetupReq.EVCCID.bytes[i];
                Serial.printf("%02x", EVCCID[i] );
            }
            Serial.printf("\n");
            
            sessionIdLen = SESSIONID_LEN;
            for (i=0; i<sessionIdLen; i++) {
                sessionId[i] = (uint8_t)random(256);
            }

            // Now prepare the 'SessionSetupResponse' message to send back to the EV
            prepare_din_message();
            
            dinDocEnc.V2G_Message.Body.SessionSetupRes_isUsed = 1;
            init_dinSessionSetupResType(&dinDocEnc.V2G_Message.Body.SessionSetupRes);
            dinDocEnc.V2G_Message.Body.SessionSetupRes.ResponseCode = dinresponseCodeType_OK_NewSessionEstablished;
            const char *evseId = EVSE_ID;
            size_t idLen = strnlen(evseId, sizeof(dinDocEnc.V2G_Message.Body.SessionSetupRes.EVSEID.bytes));
            memcpy(dinDocEnc.V2G_Message.Body.SessionSetupRes.EVSEID.bytes, evseId, idLen);
            dinDocEnc.V2G_Message.Body.SessionSetupRes.EVSEID.bytesLen = idLen;

            // Send SessionSetupResponse to EV
            send_din_message();
            fsmState = stateWaitForServiceDiscoveryRequest;
        }    
        
    } else if (fsmState == stateWaitForServiceDiscoveryRequest) {


        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.ServiceDiscoveryReq_isUsed) {
                Serial.printf("ISO ServiceDiscoveryReqest\n");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.ServiceDiscoveryRes_isUsed = 1;
                init_iso2ServiceDiscoveryResType(&iso2DocEnc.V2G_Message.Body.ServiceDiscoveryRes);
                auto &res = iso2DocEnc.V2G_Message.Body.ServiceDiscoveryRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                res.PaymentOptionList.PaymentOption.arrayLen = 1;
                res.PaymentOptionList.PaymentOption.array[0] = iso2_paymentOptionType_ExternalPayment;
                res.ChargeService.ServiceID = 1;
                res.ChargeService.ServiceCategory = iso2_serviceCategoryType_EVCharging;
                res.ChargeService.FreeService = 0;
                res.ChargeService.ServiceName_isUsed = 0;
                res.ChargeService.ServiceScope_isUsed = 0;
                res.ChargeService.SupportedEnergyTransferMode.EnergyTransferMode.arrayLen = 1;
                res.ChargeService.SupportedEnergyTransferMode.EnergyTransferMode.array[0] = iso2_EnergyTransferModeType_DC_extended;
                res.ServiceList_isUsed = 0;
                const char *svc = EVSE_SERVICE_NAME;
                size_t svcLen = strnlen(svc, iso2_ServiceName_CHARACTER_SIZE - 1);
                if (svcLen) {
                    memcpy(res.ChargeService.ServiceName.characters, svc, svcLen);
                    res.ChargeService.ServiceName.charactersLen = svcLen;
                    res.ChargeService.ServiceName_isUsed = 1;
                }
                send_iso2_message();
                fsmState = stateWaitForServicePaymentSelectionRequest;
                iso_watchdog_start(stateWaitForServicePaymentSelectionRequest);
                return;
            }
        }

        // Check if we have received the correct message
        if (dinDocDec.V2G_Message.Body.ServiceDiscoveryReq_isUsed) {

            Serial.printf("ServiceDiscoveryReqest\n");
            n = dinDocDec.V2G_Message.Header.SessionID.bytesLen;
            Serial.printf("SessionID:");
            for (i=0; i<n; i++) Serial.printf("%02x", dinDocDec.V2G_Message.Header.SessionID.bytes[i] );
            Serial.printf("\n");
            
            // Now prepare the 'ServiceDiscoveryResponse' message to send back to the EV
            prepare_din_message();
            
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes_isUsed = 1;
            init_dinServiceDiscoveryResType(&dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes);
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ResponseCode = dinresponseCodeType_OK;
            /* the mandatory fields in the ISO are PaymentOptionList and ChargeService.
            But in the DIN, this is different, we find PaymentOptions, ChargeService and optional ServiceList */
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.PaymentOptions.PaymentOption.array[0] = dinpaymentOptionType_ExternalPayment; /* EVSE handles the payment */
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.PaymentOptions.PaymentOption.arrayLen = 1; /* just one single payment option in the table */
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceID = 1; /* todo: not clear what this means  */
            //dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceName
            //dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceName_isUsed
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceCategory = dinserviceCategoryType_EVCharging;
            //dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceScope
            //dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.ServiceTag.ServiceScope_isUsed
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.FreeService = 0; /* what ever this means. Just from example. */
            /* dinEVSESupportedEnergyTransferType, e.g.
            dinEVSESupportedEnergyTransferType_DC_combo_core or
            dinEVSESupportedEnergyTransferType_DC_core or
            dinEVSESupportedEnergyTransferType_DC_extended
            dinEVSESupportedEnergyTransferType_AC_single_phase_core.
            DC_extended means "extended pins of an IEC 62196-3 Configuration FF connector", which is
            the normal CCS connector https://en.wikipedia.org/wiki/IEC_62196#FF) */
            dinDocEnc.V2G_Message.Body.ServiceDiscoveryRes.ChargeService.EnergyTransferType = dinEVSESupportedEnergyTransferType_DC_extended;
            
            // Send ServiceDiscoveryResponse to EV
            send_din_message();
            fsmState = stateWaitForServicePaymentSelectionRequest;

        }    
 
    } else if (fsmState == stateWaitForServicePaymentSelectionRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.PaymentServiceSelectionReq_isUsed) {
                Serial.printf("ISO PaymentServiceSelectionReqest\n");
                auto selected = iso2DocDec.V2G_Message.Body.PaymentServiceSelectionReq.SelectedPaymentOption;
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PaymentServiceSelectionRes_isUsed = 1;
                init_iso2PaymentServiceSelectionResType(&iso2DocEnc.V2G_Message.Body.PaymentServiceSelectionRes);
                bool supported = (selected == iso2_paymentOptionType_ExternalPayment ||
                                  selected == iso2_paymentOptionType_Contract);
                iso2DocEnc.V2G_Message.Body.PaymentServiceSelectionRes.ResponseCode =
                    supported ? iso2_responseCodeType_OK : iso2_responseCodeType_FAILED_ServiceSelectionInvalid;
                send_iso2_message();
                if (!supported) {
                    Serial.println("[ISO-2] Unsupported payment option, terminating session");
                    resetHlcSession();
                    return;
                }
                g_iso_selected_payment_option = selected;
                g_iso_expect_payment_details = (selected == iso2_paymentOptionType_Contract);
                g_iso_payment_details_done = !g_iso_expect_payment_details;
                fsmState = stateWaitForContractAuthenticationRequest;
                iso_watchdog_start(stateWaitForContractAuthenticationRequest);
                return;
            }
        }

        // Check if we have received the correct message
        if (dinDocDec.V2G_Message.Body.ServicePaymentSelectionReq_isUsed) {

            Serial.printf("ServicePaymentSelectionReqest\n");

            if (dinDocDec.V2G_Message.Body.ServicePaymentSelectionReq.SelectedPaymentOption == dinpaymentOptionType_ExternalPayment) {
                Serial.printf("OK. External Payment Selected\n");

                // Now prepare the 'ServicePaymentSelectionResponse' message to send back to the EV
                prepare_din_message();
                 
                dinDocEnc.V2G_Message.Body.ServicePaymentSelectionRes_isUsed = 1;
                init_dinServicePaymentSelectionResType(&dinDocEnc.V2G_Message.Body.ServicePaymentSelectionRes);

                dinDocEnc.V2G_Message.Body.ServicePaymentSelectionRes.ResponseCode = dinresponseCodeType_OK;
                
                // Send response to EV
                send_din_message();
                fsmState = stateWaitForContractAuthenticationRequest;
            }
        }
    } else if (fsmState == stateWaitForContractAuthenticationRequest) {
        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.PaymentDetailsReq_isUsed) {
                Serial.println("[ISO-2] PaymentDetailsReq");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PaymentDetailsRes_isUsed = 1;
                init_iso2PaymentDetailsResType(&iso2DocEnc.V2G_Message.Body.PaymentDetailsRes);
                auto &res = iso2DocEnc.V2G_Message.Body.PaymentDetailsRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                uint16_t challengeLen =
                    (uint16_t)std::min<size_t>(iso2_genChallengeType_BYTES_SIZE, static_cast<size_t>(16));
                res.GenChallenge.bytesLen = challengeLen;
                for (uint16_t i = 0; i < challengeLen; ++i) {
#ifdef ESP_PLATFORM
                    uint32_t r = esp_random();
#else
                    uint32_t r = (uint32_t)random(0, 0x7FFFFFFF);
#endif
                    res.GenChallenge.bytes[i] = (uint8_t)(r & 0xFF);
                }
#ifdef ESP_PLATFORM
                res.EVSETimeStamp = esp_timer_get_time() / 1000;
#else
                res.EVSETimeStamp = millis();
#endif
                send_iso2_message();
                g_iso_payment_details_done = true;
                g_iso_expect_payment_details = false;
                return;
            }
            if (iso2DocDec.V2G_Message.Body.AuthorizationReq_isUsed) {
                Serial.println("[ISO-2] AuthorizationReq");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.AuthorizationRes_isUsed = 1;
                init_iso2AuthorizationResType(&iso2DocEnc.V2G_Message.Body.AuthorizationRes);
                auto &res = iso2DocEnc.V2G_Message.Body.AuthorizationRes;
                if (g_iso_expect_payment_details && !g_iso_payment_details_done) {
                    res.ResponseCode = iso2_responseCodeType_FAILED_SequenceError;
                    res.EVSEProcessing = iso2_EVSEProcessingType_Finished;
                    send_iso2_message();
                    Serial.println("[ISO-2] Authorization before PaymentDetails; sequence error");
                    return;
                }
                res.ResponseCode = iso2_responseCodeType_OK;
                res.EVSEProcessing = iso2_EVSEProcessingType_Finished;
                send_iso2_message();
                fsmState = stateWaitForChargeParameterDiscoveryRequest;
                iso_watchdog_start(stateWaitForChargeParameterDiscoveryRequest);
                return;
            }
        }

        // Check if we have received the correct message
        if (dinDocDec.V2G_Message.Body.ContractAuthenticationReq_isUsed) {

            Serial.printf("ContractAuthenticationRequest\n");

            // Now prepare the 'ContractAuthenticationResponse' message to send back to the EV
            prepare_din_message();
                        
            dinDocEnc.V2G_Message.Body.ContractAuthenticationRes_isUsed = 1;
            // Set Authorisation immediately to 'Finished'.
            dinDocEnc.V2G_Message.Body.ContractAuthenticationRes.EVSEProcessing = dinEVSEProcessingType_Finished;
            init_dinContractAuthenticationResType(&dinDocEnc.V2G_Message.Body.ContractAuthenticationRes);
            
            // Send response to EV
            send_din_message();
            fsmState = stateWaitForChargeParameterDiscoveryRequest;
        }    

    } else if (fsmState == stateWaitForChargeParameterDiscoveryRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.ChargeParameterDiscoveryReq_isUsed) {
                Serial.println("[ISO-2] ChargeParameterDiscoveryReq");
                if (iso2DocDec.V2G_Message.Body.ChargeParameterDiscoveryReq.DC_EVChargeParameter_isUsed) {
                    EVSOC = iso2DocDec.V2G_Message.Body.ChargeParameterDiscoveryReq.DC_EVChargeParameter.DC_EVStatus.EVRESSSOC;
                }
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.ChargeParameterDiscoveryRes_isUsed = 1;
                init_iso2ChargeParameterDiscoveryResType(&iso2DocEnc.V2G_Message.Body.ChargeParameterDiscoveryRes);
                auto &res = iso2DocEnc.V2G_Message.Body.ChargeParameterDiscoveryRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                res.EVSEProcessing = iso2_EVSEProcessingType_Finished;
                res.SAScheduleList_isUsed = 0;
                res.SASchedules_isUsed = 0;
                res.AC_EVSEChargeParameter_isUsed = 0;
                res.EVSEChargeParameter_isUsed = 0;
                res.DC_EVSEChargeParameter_isUsed = 1;
                init_iso2DC_EVSEChargeParameterType(&res.DC_EVSEChargeParameter);
                iso_populate_dc_evse_status(&res.DC_EVSEChargeParameter.DC_EVSEStatus, iso_current_evse_status_code());
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEMaximumVoltageLimit,
                                       iso2_unitSymbolType_V, EVSE_MAX_VOLTAGE);
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEMinimumVoltageLimit,
                                       iso2_unitSymbolType_V, 0.0f);
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEMaximumCurrentLimit,
                                       iso2_unitSymbolType_A, EVSE_MAX_CURRENT);
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEMinimumCurrentLimit,
                                       iso2_unitSymbolType_A, 0.0f);
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEMaximumPowerLimit,
                                       iso2_unitSymbolType_W, EVSE_MAX_POWER_KW * 1000.0f);
                iso_set_physical_value(&res.DC_EVSEChargeParameter.EVSEPeakCurrentRipple,
                                       iso2_unitSymbolType_A, 2.0f);
                res.DC_EVSEChargeParameter.EVSECurrentRegulationTolerance_isUsed = 0;
                res.DC_EVSEChargeParameter.EVSEEnergyToBeDelivered_isUsed = 0;
                send_iso2_message();
                fsmState = stateWaitForCableCheckRequest;
                iso_watchdog_start(stateWaitForCableCheckRequest);
                return;
            }
        }

        // Check if we have received the correct message
        if (dinDocDec.V2G_Message.Body.ChargeParameterDiscoveryReq_isUsed) {

            Serial.printf("ChargeParameterDiscoveryRequest\n");

            // Read the SOC from the EVRESSOC data
            EVSOC = dinDocDec.V2G_Message.Body.ChargeParameterDiscoveryReq.DC_EVChargeParameter.DC_EVStatus.EVRESSSOC;

            Serial.printf("Current SoC %d%%\n", EVSOC);

            // Now prepare the 'ChargeParameterDiscoveryResponse' message to send back to the EV
            prepare_din_message();
            
            dinDocEnc.V2G_Message.Body.ChargeParameterDiscoveryRes_isUsed = 1;   
            init_dinChargeParameterDiscoveryResType(&dinDocEnc.V2G_Message.Body.ChargeParameterDiscoveryRes);
            
            // Send response to EV
            send_din_message();
            fsmState = stateWaitForCableCheckRequest;

        }    

    } else if (fsmState == stateWaitForCableCheckRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.CableCheckReq_isUsed) {
                Serial.println("[ISO-2] CableCheckReq");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.CableCheckRes_isUsed = 1;
                init_iso2CableCheckResType(&iso2DocEnc.V2G_Message.Body.CableCheckRes);
                auto &res = iso2DocEnc.V2G_Message.Body.CableCheckRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                res.EVSEProcessing = iso2_EVSEProcessingType_Finished;
                send_iso2_message();
                fsmState = stateWaitForPreChargeRequest;
                iso_watchdog_start(stateWaitForPreChargeRequest);
                return;
            }
        }

        if (dinDocDec.V2G_Message.Body.CableCheckReq_isUsed) {
            prepare_din_message();
            dinDocEnc.V2G_Message.Body.CableCheckRes_isUsed = 1;
            init_dinCableCheckResType(&dinDocEnc.V2G_Message.Body.CableCheckRes);
            dinDocEnc.V2G_Message.Body.CableCheckRes.ResponseCode = dinresponseCodeType_OK;
            populateDcEvseStatus(&dinDocEnc.V2G_Message.Body.CableCheckRes.DC_EVSEStatus, currentEvseStatusCode());
            dinDocEnc.V2G_Message.Body.CableCheckRes.EVSEProcessing = dinEVSEProcessingType_Finished;

            send_din_message();
            fsmState = stateWaitForPreChargeRequest;
        }

    } else if (fsmState == stateWaitForPreChargeRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.PreChargeReq_isUsed) {
                Serial.println("[ISO-2] PreChargeReq");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PreChargeRes_isUsed = 1;
                init_iso2PreChargeResType(&iso2DocEnc.V2G_Message.Body.PreChargeRes);
                auto &res = iso2DocEnc.V2G_Message.Body.PreChargeRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                iso_set_physical_value(&res.EVSEPresentVoltage, iso2_unitSymbolType_V, dc_get_bus_voltage());
                send_iso2_message();
                fsmState = stateWaitForPowerDeliveryRequest;
                iso_watchdog_start(stateWaitForPowerDeliveryRequest);
                return;
            }
        }

        if (dinDocDec.V2G_Message.Body.PreChargeReq_isUsed) {
            prepare_din_message();
            dinDocEnc.V2G_Message.Body.PreChargeRes_isUsed = 1;
            init_dinPreChargeResType(&dinDocEnc.V2G_Message.Body.PreChargeRes);
            dinDocEnc.V2G_Message.Body.PreChargeRes.ResponseCode = dinresponseCodeType_OK;
            populateDcEvseStatus(&dinDocEnc.V2G_Message.Body.PreChargeRes.DC_EVSEStatus, currentEvseStatusCode());
            setPhysicalValue(&dinDocEnc.V2G_Message.Body.PreChargeRes.EVSEPresentVoltage, dinunitSymbolType_V, EVSE_PRESENT_VOLTAGE, 0);

            send_din_message();
            fsmState = stateWaitForPowerDeliveryRequest;
        }

    } else if (fsmState == stateWaitForPowerDeliveryRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
                auto progress = iso2DocDec.V2G_Message.Body.PowerDeliveryReq.ChargeProgress;
                uint8_t nextState = stateWaitForSessionStopRequest;
                bool contactorOk = true;
                iso2_responseCodeType resp = iso_process_power_delivery(progress, contactorOk, nextState);
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
                init_iso2PowerDeliveryResType(&iso2DocEnc.V2G_Message.Body.PowerDeliveryRes);
                auto &res = iso2DocEnc.V2G_Message.Body.PowerDeliveryRes;
                res.ResponseCode = resp;
                res.AC_EVSEStatus_isUsed = 0;
                res.EVSEStatus_isUsed = 0;
                res.DC_EVSEStatus_isUsed = 1;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                send_iso2_message();
                fsmState = nextState;
                iso_watchdog_start(nextState);
                return;
            }
        }

        if (dinDocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
            bool ready = dinDocDec.V2G_Message.Body.PowerDeliveryReq.ReadyToChargeState != 0;
            bool contactorOk = true;
            if (ready) {
                contactorOk = cp_contactor_command(true);
                if (contactorOk) {
                    chargingActive = true;
                    dc_enable_output(true);
                } else {
                    chargingActive = false;
                    dc_enable_output(false);
                }
            } else {
                chargingActive = false;
                dc_enable_output(false);
                cp_contactor_command(false);
            }

            prepare_din_message();
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
            init_dinPowerDeliveryResType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes);
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes.ResponseCode = contactorOk ? dinresponseCodeType_OK
                                                                                   : dinresponseCodeType_FAILED_PowerDeliveryNotApplied;
            init_dinEVSEStatusType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes.EVSEStatus);

            send_din_message();

            fsmState = (ready && contactorOk) ? stateWaitForCurrentDemandRequest : stateWaitForSessionStopRequest;
        }

    } else if (fsmState == stateWaitForCurrentDemandRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (iso2DocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
                auto progress = iso2DocDec.V2G_Message.Body.PowerDeliveryReq.ChargeProgress;
                uint8_t nextState = stateWaitForSessionStopRequest;
                bool contactorOk = true;
                iso2_responseCodeType resp = iso_process_power_delivery(progress, contactorOk, nextState);
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
                init_iso2PowerDeliveryResType(&iso2DocEnc.V2G_Message.Body.PowerDeliveryRes);
                auto &res = iso2DocEnc.V2G_Message.Body.PowerDeliveryRes;
                res.ResponseCode = resp;
                res.AC_EVSEStatus_isUsed = 0;
                res.EVSEStatus_isUsed = 0;
                res.DC_EVSEStatus_isUsed = 1;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                send_iso2_message();
                fsmState = nextState;
                return;
            } else if (iso2DocDec.V2G_Message.Body.CurrentDemandReq_isUsed) {
                const auto &req = iso2DocDec.V2G_Message.Body.CurrentDemandReq;
                float targetVoltage = iso_decode_physical_value(req.EVTargetVoltage);
                float targetCurrent = iso_decode_physical_value(req.EVTargetCurrent);
                if (targetVoltage < 0) targetVoltage = 0;
                if (targetCurrent < 0) targetCurrent = 0;
                dc_set_targets(targetVoltage, targetCurrent);
                chargingActive = true;

                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.CurrentDemandRes_isUsed = 1;
                init_iso2CurrentDemandResType(&iso2DocEnc.V2G_Message.Body.CurrentDemandRes);
                auto &res = iso2DocEnc.V2G_Message.Body.CurrentDemandRes;
                res.ResponseCode = iso2_responseCodeType_OK;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                float presentVoltage = dc_get_bus_voltage();
                float presentCurrent = dc_get_bus_current();
                iso_set_physical_value(&res.EVSEPresentVoltage, iso2_unitSymbolType_V, presentVoltage);
                iso_set_physical_value(&res.EVSEPresentCurrent, iso2_unitSymbolType_A, presentCurrent);
                res.EVSECurrentLimitAchieved = (targetCurrent >= EVSE_MAX_CURRENT);
                res.EVSEVoltageLimitAchieved = (targetVoltage >= EVSE_MAX_VOLTAGE);
                res.EVSEPowerLimitAchieved =
                    ((targetVoltage * targetCurrent) >= (EVSE_MAX_POWER_KW * 1000.0f));
                iso_set_physical_value(&res.EVSEMaximumVoltageLimit, iso2_unitSymbolType_V, EVSE_MAX_VOLTAGE);
                res.EVSEMaximumVoltageLimit_isUsed = 1;
                iso_set_physical_value(&res.EVSEMaximumCurrentLimit, iso2_unitSymbolType_A, EVSE_MAX_CURRENT);
                res.EVSEMaximumCurrentLimit_isUsed = 1;
                iso_set_physical_value(&res.EVSEMaximumPowerLimit, iso2_unitSymbolType_W,
                                       EVSE_MAX_POWER_KW * 1000.0f);
                res.EVSEMaximumPowerLimit_isUsed = 1;
                iso_set_evse_id(res.EVSEID.characters, res.EVSEID.charactersLen, iso2_EVSEID_CHARACTER_SIZE);
                res.SAScheduleTupleID = kDefaultSasTupleId;
                res.MeterInfo_isUsed = 0;
                res.ReceiptRequired_isUsed = 0;
                send_iso2_message();
                iso_watchdog_start(stateWaitForCurrentDemandRequest);
                return;
            } else if (handle_iso_metering_receipt()) {
                // stay in current demand
            }
        }

        if (dinDocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
            bool ready = dinDocDec.V2G_Message.Body.PowerDeliveryReq.ReadyToChargeState != 0;
            bool contactorOk = true;
            if (ready) {
                contactorOk = cp_contactor_command(true);
                if (contactorOk) {
                    chargingActive = true;
                    dc_enable_output(true);
                } else {
                    chargingActive = false;
                    dc_enable_output(false);
                }
            } else {
                chargingActive = false;
                dc_enable_output(false);
                cp_contactor_command(false);
            }
            prepare_din_message();
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
            init_dinPowerDeliveryResType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes);
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes.ResponseCode = contactorOk ? dinresponseCodeType_OK
                                                                                   : dinresponseCodeType_FAILED_PowerDeliveryNotApplied;
            init_dinEVSEStatusType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes.EVSEStatus);
            send_din_message();

            if (!(ready && contactorOk)) {
                fsmState = stateWaitForSessionStopRequest;
            }

        } else if (dinDocDec.V2G_Message.Body.CurrentDemandReq_isUsed) {
            const dinCurrentDemandReqType &req = dinDocDec.V2G_Message.Body.CurrentDemandReq;
            float targetVoltage = decodePhysicalValue(req.EVTargetVoltage);
            float targetCurrent = decodePhysicalValue(req.EVTargetCurrent);
            if (targetVoltage < 0) targetVoltage = 0;
            if (targetCurrent < 0) targetCurrent = 0;
            dc_set_targets(targetVoltage, targetCurrent);

            if (req.ChargingComplete) {
                chargingActive = false;
                dc_enable_output(false);
                cp_contactor_command(false);
            }

            prepare_din_message();
            dinDocEnc.V2G_Message.Body.CurrentDemandRes_isUsed = 1;
            init_dinCurrentDemandResType(&dinDocEnc.V2G_Message.Body.CurrentDemandRes);
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.ResponseCode = dinresponseCodeType_OK;
            populateDcEvseStatus(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.DC_EVSEStatus,
                                 currentEvseStatusCode());
            encodePhysicalValue(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEPresentVoltage, dinunitSymbolType_V, dc_get_bus_voltage());
            encodePhysicalValue(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEPresentCurrent, dinunitSymbolType_A,
                                chargingActive ? dc_get_bus_current() : 0.0f);
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSECurrentLimitAchieved = 0;
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEVoltageLimitAchieved = 0;
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEPowerLimitAchieved = 0;
            encodePhysicalValue(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumVoltageLimit, dinunitSymbolType_V, EVSE_MAX_VOLTAGE);
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumVoltageLimit_isUsed = 1;
            encodePhysicalValue(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumCurrentLimit, dinunitSymbolType_A, EVSE_MAX_CURRENT);
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumCurrentLimit_isUsed = 1;
            encodePhysicalValue(&dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumPowerLimit, dinunitSymbolType_W, EVSE_MAX_POWER_KW * 1000.0f);
            dinDocEnc.V2G_Message.Body.CurrentDemandRes.EVSEMaximumPowerLimit_isUsed = 1;

            send_din_message();

        } else if (handleMeteringReceipt()) {
            // stay in current demand state
        }

    } else if (fsmState == stateWaitForSessionStopRequest) {

        if (g_hlc_protocol == HlcProtocol::Iso2) {
            if (handle_iso_metering_receipt()) {
                // wait for SessionStopReq
            } else if (iso2DocDec.V2G_Message.Body.SessionStopReq_isUsed) {
                Serial.println("[ISO-2] SessionStopReq");
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.SessionStopRes_isUsed = 1;
                init_iso2SessionStopResType(&iso2DocEnc.V2G_Message.Body.SessionStopRes);
                iso2DocEnc.V2G_Message.Body.SessionStopRes.ResponseCode = iso2_responseCodeType_OK;
                send_iso2_message();
                stop_evse_power_output();
                fsmState = stateWaitForSupportedApplicationProtocolRequest;
                return;
            } else if (iso2DocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
                auto progress = iso2DocDec.V2G_Message.Body.PowerDeliveryReq.ChargeProgress;
                uint8_t nextState = stateWaitForSessionStopRequest;
                bool contactorOk = true;
                iso2_responseCodeType resp = iso_process_power_delivery(progress, contactorOk, nextState);
                prepare_iso2_message();
                iso2DocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
                init_iso2PowerDeliveryResType(&iso2DocEnc.V2G_Message.Body.PowerDeliveryRes);
                auto &res = iso2DocEnc.V2G_Message.Body.PowerDeliveryRes;
                res.ResponseCode = resp;
                res.AC_EVSEStatus_isUsed = 0;
                res.EVSEStatus_isUsed = 0;
                res.DC_EVSEStatus_isUsed = 1;
                iso_populate_dc_evse_status(&res.DC_EVSEStatus, iso_current_evse_status_code());
                send_iso2_message();
                fsmState = nextState;
                return;
            }
        }

        if (handleMeteringReceipt()) {
            // wait for SessionStopReq
        } else if (dinDocDec.V2G_Message.Body.SessionStopReq_isUsed) {
            prepare_din_message();
            dinDocEnc.V2G_Message.Body.SessionStopRes_isUsed = 1;
            init_dinSessionStopResType(&dinDocEnc.V2G_Message.Body.SessionStopRes);
            dinDocEnc.V2G_Message.Body.SessionStopRes.ResponseCode = dinresponseCodeType_OK;

            send_din_message();
            chargingActive = false;
            fsmState = stateWaitForSupportedApplicationProtocolRequest;
        } else if (dinDocDec.V2G_Message.Body.PowerDeliveryReq_isUsed) {
            // Treat unexpected PowerDelivery during stop as start/stop handshake.
            bool ready = dinDocDec.V2G_Message.Body.PowerDeliveryReq.ReadyToChargeState != 0;
            prepare_din_message();
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes_isUsed = 1;
            init_dinPowerDeliveryResType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes);
            dinDocEnc.V2G_Message.Body.PowerDeliveryRes.ResponseCode = dinresponseCodeType_OK;
            init_dinEVSEStatusType(&dinDocEnc.V2G_Message.Body.PowerDeliveryRes.EVSEStatus);
            send_din_message();
            chargingActive = ready;
            if (ready) {
                fsmState = stateWaitForCurrentDemandRequest;
            }
        }

    }
    
}


void tcp_packRequestIntoEthernet(void) {
    //# packs the IP packet into an ethernet packet
    uint16_t i;
    uint16_t length;        
    
    length = TcpIpRequestLen + 6 + 6 + 2; // # Ethernet header needs 14 bytes:
                                                    // #  6 bytes destination MAC
                                                    // #  6 bytes source MAC
                                                    // #  2 bytes EtherType
    //# fill the destination MAC with the MAC of the charger
    setMacAt(pevMac, 0);
    setMacAt(myMac, 6); // bytes 6 to 11 are the source MAC
    txbuffer[12] = 0x86; // # 86dd is IPv6
    txbuffer[13] = 0xdd;
    memcpy(txbuffer+14, TcpIpRequest, length);
    
    //Serial.print("[TX] ");
    //for(int x=0; x<length; x++) Serial.printf("%02x",txbuffer[x]);
    //Serial.printf("\n\n");

    qcaspi_write_burst(txbuffer, length);
}

void tcp_packRequestIntoIp(void) {
    // # embeds the TCP into the lower-layer-protocol: IP, Ethernet
    uint8_t i;
    uint16_t plen;
    TcpIpRequestLen = TcpTransmitPacketLen + 8 + 16 + 16; // # IP6 header needs 40 bytes:
                                                //  #   4 bytes traffic class, flow
                                                //  #   2 bytes destination port
                                                //  #   2 bytes length (incl checksum)
                                                //  #   2 bytes checksum
    TcpIpRequest[0] = 0x60; // traffic class, flow
    TcpIpRequest[1] = 0x00; 
    TcpIpRequest[2] = 0x00;
    TcpIpRequest[3] = 0x00;
    plen = TcpTransmitPacketLen; // length of the payload. Without headers.
    TcpIpRequest[4] = plen >> 8;
    TcpIpRequest[5] = plen & 0xFF;
    TcpIpRequest[6] = NEXT_TCP; // next level protocol, 0x06 = TCP in this case
    TcpIpRequest[7] = 0x40; // hop limit
    //
    // We are the EVSE. So the PevIp is our own link-local IP address.
    for (i=0; i<16; i++) {
        TcpIpRequest[8+i] = SeccIp[i]; // source IP address
    }            
    for (i=0; i<16; i++) {
        TcpIpRequest[24+i] = EvccIp[i]; // destination IP address
    }
    for (i=0; i<TcpTransmitPacketLen; i++) {
        TcpIpRequest[40+i] = TcpTransmitPacket[i];
    }
    //showAsHex(TcpIpRequest, TcpIpRequestLen, "TcpIpRequest");
    tcp_packRequestIntoEthernet();
}



void tcp_prepareTcpHeader(uint8_t tcpFlag) {
    uint8_t i;
    uint16_t checksum;

    // # TCP header needs at least 24 bytes:
    // 2 bytes source port
    // 2 bytes destination port
    // 4 bytes sequence number
    // 4 bytes ack number
    // 4 bytes DO/RES/Flags/Windowsize
    // 2 bytes checksum
    // 2 bytes urgentPointer
    // n*4 bytes options/fill (empty for the ACK frame and payload frames)
    TcpTransmitPacket[0] = (uint8_t)(seccPort >> 8); /* source port */
    TcpTransmitPacket[1] = (uint8_t)(seccPort);
    TcpTransmitPacket[2] = (uint8_t)(evccTcpPort >> 8); /* destination port */
    TcpTransmitPacket[3] = (uint8_t)(evccTcpPort);

    TcpTransmitPacket[4] = (uint8_t)(TcpSeqNr>>24); /* sequence number */
    TcpTransmitPacket[5] = (uint8_t)(TcpSeqNr>>16);
    TcpTransmitPacket[6] = (uint8_t)(TcpSeqNr>>8);
    TcpTransmitPacket[7] = (uint8_t)(TcpSeqNr);

    TcpTransmitPacket[8] = (uint8_t)(TcpAckNr>>24); /* ack number */
    TcpTransmitPacket[9] = (uint8_t)(TcpAckNr>>16);
    TcpTransmitPacket[10] = (uint8_t)(TcpAckNr>>8);
    TcpTransmitPacket[11] = (uint8_t)(TcpAckNr);
    TcpTransmitPacketLen = tcpHeaderLen + tcpPayloadLen; 
    TcpTransmitPacket[12] = (tcpHeaderLen/4) << 4; /* 70 High-nibble: DataOffset in 4-byte-steps. Low-nibble: Reserved=0. */

    TcpTransmitPacket[13] = tcpFlag; 
    TcpTransmitPacket[14] = (uint8_t)(TCP_RECEIVE_WINDOW>>8);
    TcpTransmitPacket[15] = (uint8_t)(TCP_RECEIVE_WINDOW);

    // checksum will be calculated afterwards
    TcpTransmitPacket[16] = 0;
    TcpTransmitPacket[17] = 0;

    TcpTransmitPacket[18] = 0; /* 16 bit urgentPointer. Always zero in our case. */
    TcpTransmitPacket[19] = 0;

    if (tcpHeaderLen > 20) {
        TcpTransmitPacket[20] = 0x02; // MSS option
        TcpTransmitPacket[21] = 0x04;
        TcpTransmitPacket[22] = 0x05;
        TcpTransmitPacket[23] = 0xa0;
    }
    

    checksum = calculateUdpAndTcpChecksumForIPv6(TcpTransmitPacket, TcpTransmitPacketLen, SeccIp, EvccIp, NEXT_TCP); 
    TcpTransmitPacket[16] = (uint8_t)(checksum >> 8);
    TcpTransmitPacket[17] = (uint8_t)(checksum);

    //Serial.printf("Source:%u Dest:%u Seqnr:%08x Acknr:%08x\n", seccPort, evccTcpPort, TcpSeqNr, TcpAckNr);  
}


void tcp_sendFirstAck(void) {
    Serial.printf("[TCP] sending first ACK\n");
    tcpHeaderLen = 24;
    tcpPayloadLen = 0;
    tcp_prepareTcpHeader(TCP_FLAG_ACK | TCP_FLAG_SYN);	
    tcp_packRequestIntoIp();
}

void tcp_sendAck(void) {
   Serial.printf("[TCP] sending ACK\n");
   tcpHeaderLen = 20; /* 20 bytes normal header, no options */
   tcpPayloadLen = 0;   
   tcp_prepareTcpHeader(TCP_FLAG_ACK);	
   tcp_packRequestIntoIp();
}


void evaluateTcpPacket(uint16_t payloadOffset, uint16_t ipPayloadLen) {
    uint8_t flags;
    uint32_t remoteSeqNr;
    uint32_t remoteAckNr;
    uint16_t SourcePort, DestinationPort, hdrLen, tmpPayloadLen;
    const uint8_t *tcp = rxbuffer + payloadOffset;
        
    if (ipPayloadLen < 20) {
        Serial.printf("[TCP] payload too short (%u). Drop.\n", ipPayloadLen);
        return;
    }

    hdrLen = (tcp[12]>>4) * 4; /* header length in byte */
    if (hdrLen < 20 || hdrLen > ipPayloadLen) {
        Serial.printf("[TCP] invalid header length %u (payload %u)\n", hdrLen, ipPayloadLen);
        return;
    }
    tmpPayloadLen = ipPayloadLen - hdrLen;

    SourcePort = (tcp[0] << 8) | tcp[1];
    DestinationPort = (tcp[2] << 8) | tcp[3];
    if (DestinationPort != 15118) {
        Serial.printf("[TCP] wrong port.\n");
        return; /* wrong port */
    }
    tcpLastActivity = millis();
    remoteSeqNr = 
            (((uint32_t)tcp[4])<<24) +
            (((uint32_t)tcp[5])<<16) +
            (((uint32_t)tcp[6])<<8) +
            (((uint32_t)tcp[7]));
    remoteAckNr = 
            (((uint32_t)tcp[8])<<24) +
            (((uint32_t)tcp[9])<<16) +
            (((uint32_t)tcp[10])<<8) +
            (((uint32_t)tcp[11]));
    flags = tcp[13];
    if (flags & TCP_FLAG_RST) {
        Serial.printf("TCP RST received\n");
        tcpState = TCP_STATE_CLOSED;
        tcpAwaitingAck = false;
        resetHlcSession();
        return;
    }

    if ((flags & TCP_FLAG_SYN) && !(flags & TCP_FLAG_ACK)) { /* connection setup request */
        if (tcpState == TCP_STATE_CLOSED) {
            evccTcpPort = SourcePort; // update the evccTcpPort to the new TCP port
            TcpSeqNr = 0x01020304; // We start with a 'random' sequence nr
            TcpAckNr = remoteSeqNr+1; // The ACK number of our next transmit packet is one more than the received seq number.
            tcpState = TCP_STATE_SYN_ACK;
            tcp_sendFirstAck();
        }
        return;
    }    
    if ((flags & TCP_FLAG_ACK) && (tcpState == TCP_STATE_SYN_ACK)) {
        if (remoteAckNr == (TcpSeqNr + 1) ) {
            Serial.printf("-------------- TCP connection established ---------------\n\n");
            tcpState = TCP_STATE_ESTABLISHED;
            tcpLastActivity = millis();
        }
        return;
    }
    /* It is no connection setup. We can have the following situations here: */
    if (tcpState != TCP_STATE_ESTABLISHED) {
        /* received something while the connection is closed. Just ignore it. */
        Serial.printf("[TCP] ignore, not connected.\n");
        return;    
    } 

    // It can be an ACK, or a data package, or a combination of both. We treat the ACK and the data independent from each other,
    // to treat each combination. 
   if (tmpPayloadLen > 0) {
        if (tmpPayloadLen >= TCP_RX_DATA_LEN) {
            Serial.printf("TCP payload too large (%u)\n", tmpPayloadLen);
            resetHlcSession();
            return;
        }
        /* This is a data transfer packet. */
        TcpAckNr = remoteSeqNr + tmpPayloadLen; // ACK references end of payload
        TcpSeqNr = remoteAckNr;
        tcp_sendAck();  // Send Ack, then process data
        tcp_bufferPayload(tcp + hdrLen, tmpPayloadLen, false);
    }

   if (flags & TCP_FLAG_ACK) {
       Serial.printf("This was an ACK\n\n");
       //nTcpPacketsReceived+=1000;
       TcpSeqNr = remoteAckNr; /* The sequence number of our next transmit packet is given by the received ACK number. */
       if (tcpAwaitingAck && remoteAckNr >= expectedTcpAckNr) {
           tcpAwaitingAck = false;
           lastTcpPayloadLen = 0;
           tcpRetransmitAttempts = 0;
       }
   }

   if (flags & TCP_FLAG_FIN) {
       TcpAckNr = remoteSeqNr + tmpPayloadLen + 1;
       tcp_sendAck();
       tcpState = TCP_STATE_CLOSED;
       tcpAwaitingAck = false;
       resetHlcSession();
       return;
   }
}
