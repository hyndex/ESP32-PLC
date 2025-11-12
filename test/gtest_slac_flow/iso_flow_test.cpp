#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "Arduino.h"
#include "ipv6.h"
#include "main.h"
#include "tcp.h"

extern "C" {
void slac_test_reset_state(void);
void slac_test_set_tx_hook(void (*hook)(const uint8_t *, uint32_t));
void dc_stub_set_bus_voltage(float voltage);
void dc_stub_set_bus_current(float current);
void dc_stub_reset_measurements(void);
void tcp_test_override_evse_status(int status_code, int isolation_used);
void tcp_test_clear_evse_status_override(void);
}

extern "C" {
#include "cbv2g/common/exi_bitstream.h"
#include "cbv2g/din/din_msgDefDatatypes.h"
#include "cbv2g/din/din_msgDefDecoder.h"
}

namespace {

#ifndef DEMO_CHARGING_LOG_PATH
#define DEMO_CHARGING_LOG_PATH "../../temp/ccs32berta/doc/2023-07-04_demoChargingWorks.log"
#endif

constexpr const char kDemoLogPath[] = DEMO_CHARGING_LOG_PATH;
constexpr std::array<uint8_t, 6> kEvseMac{{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x01}};
constexpr std::array<uint8_t, 16> kEvIp{
    0xfe, 0x80, 0, 0, 0, 0, 0, 0, 0x12, 0x34, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef};

std::vector<std::vector<uint8_t>> g_tcp_frames;

void CaptureTcpPayload(const uint8_t *data, uint16_t len) {
    g_tcp_frames.emplace_back(data, data + len);
}

struct LogTrace {
    std::vector<std::vector<uint8_t>> requests;
    std::vector<std::vector<uint8_t>> responses;
};

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

std::optional<size_t> ExtractDeclaredSize(const std::string &line) {
    const std::string needle = "has";
    auto pos = line.find(needle);
    if (pos == std::string::npos) return std::nullopt;
    pos += needle.size();
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
        ++pos;
    }
    size_t end = pos;
    while (end < line.size() && std::isdigit(static_cast<unsigned char>(line[end]))) {
        ++end;
    }
    if (end == pos) return std::nullopt;
    size_t value = 0;
    try {
        value = static_cast<size_t>(std::stoul(line.substr(pos, end - pos)));
    } catch (...) {
        return std::nullopt;
    }
    return value;
}

std::optional<std::vector<uint8_t>> ParseFrameLine(const std::string &line) {
    auto declared = ExtractDeclaredSize(line);
    if (!declared) return std::nullopt;
    auto colon = line.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    std::vector<uint8_t> bytes;
    bytes.reserve(*declared);
    int current_nibble = -1;
    for (size_t i = colon + 1; i < line.size(); ++i) {
        int nibble = HexValue(line[i]);
        if (nibble < 0) continue;
        if (current_nibble < 0) {
            current_nibble = nibble;
        } else {
            bytes.push_back(static_cast<uint8_t>((current_nibble << 4) | nibble));
            current_nibble = -1;
        }
    }
    if (current_nibble >= 0 || bytes.size() != *declared) return std::nullopt;
    return bytes;
}

bool IsResponseTriggerLine(const std::string &line) {
    return line.find("In state") != std::string::npos &&
           line.find("received") != std::string::npos;
}

LogTrace LoadLogTrace(const std::string &path) {
    LogTrace trace;
    std::ifstream file(path);
    if (!file.is_open()) {
        ADD_FAILURE() << "Unable to open log file: " << path;
        return trace;
    }

    std::string line;
    bool expect_response = false;
    while (std::getline(file, line)) {
        if (line.find("tcpPayload has") != std::string::npos) {
            if (auto frame = ParseFrameLine(line)) {
                trace.requests.push_back(std::move(*frame));
            }
        } else if (IsResponseTriggerLine(line)) {
            expect_response = true;
        } else if (expect_response && line.find(" has ") != std::string::npos) {
            if (auto frame = ParseFrameLine(line)) {
                trace.responses.push_back(std::move(*frame));
            }
            expect_response = false;
        }
    }

    return trace;
}

std::string HexDump(const std::vector<uint8_t> &frame) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < frame.size(); ++i) {
        if (i) oss << ' ';
        oss << std::setw(2) << static_cast<unsigned int>(frame[i]);
    }
    return oss.str();
}

struct DecodedDinMessage {
    din_exiDocument doc;
    bool valid = false;
};

DecodedDinMessage DecodeDinResponse(const std::vector<uint8_t> &frame) {
    DecodedDinMessage msg{};
    if (frame.size() < V2GTP_HEADER_SIZE) {
        ADD_FAILURE() << "Frame too small for V2GTP header";
        return msg;
    }
    uint32_t payloadLen = (frame[4] << 24) | (frame[5] << 16) | (frame[6] << 8) | frame[7];
    if (payloadLen != frame.size() - V2GTP_HEADER_SIZE) {
        ADD_FAILURE() << "Invalid V2GTP length";
        return msg;
    }
    init_din_exiDocument(&msg.doc);
    exi_bitstream_t stream;
    exi_bitstream_init(&stream, const_cast<uint8_t *>(frame.data()) + V2GTP_HEADER_SIZE,
                       payloadLen, 0, nullptr);
    int err = decode_din_exiDocument(&stream, &msg.doc);
    if (err != 0) {
        msg.valid = false;
        return msg;
    }
    msg.valid = true;
    return msg;
}

std::string HexString(const uint8_t *data, size_t len) {
    if (!data || !len) return "";
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        if (i) oss << ' ';
        oss << std::setw(2) << static_cast<unsigned int>(data[i]);
    }
    return oss.str();
}

std::string PrintableString(const uint8_t *data, size_t len) {
    std::string result;
    result.reserve(len);
    for (size_t i = 0; i < len; ++i) {
        char c = static_cast<char>(data[i]);
        result.push_back(std::isprint(static_cast<unsigned char>(c)) ? c : '.');
    }
    return result;
}

double DecodePhysicalValue(const din_PhysicalValueType &value) {
    double scale = std::pow(10.0, static_cast<double>(value.Multiplier));
    return static_cast<double>(value.Value) * scale;
}

std::string DescribePhysicalValueDetailed(const din_PhysicalValueType &value) {
    std::ostringstream oss;
    oss << DecodePhysicalValue(value) << " (raw=" << value.Value
        << ", mult=" << static_cast<int>(value.Multiplier)
        << ", unitUsed=" << value.Unit_isUsed << ")";
    return oss.str();
}

std::string DescribeDcEvseStatus(const din_DC_EVSEStatusType &status) {
    std::ostringstream oss;
    oss << "status(code=" << static_cast<int>(status.EVSEStatusCode)
        << ", isolationUsed=" << status.EVSEIsolationStatus_isUsed
        << ", notification=" << static_cast<int>(status.EVSENotification) << ")";
    return oss.str();
}

std::string DescribeDinMessage(const din_exiDocument &doc) {
    std::ostringstream oss;
    const auto &header = doc.V2G_Message.Header;
    if (header.SessionID.bytesLen) {
        oss << "SessionID=" << HexString(reinterpret_cast<const uint8_t *>(header.SessionID.bytes),
                                         header.SessionID.bytesLen)
            << "; ";
    }
    const auto &body = doc.V2G_Message.Body;
    if (body.SessionSetupRes_isUsed) {
        const auto &res = body.SessionSetupRes;
        oss << "SessionSetupRes rc=" << static_cast<int>(res.ResponseCode);
        if (res.EVSEID.bytesLen) {
            oss << " EVSEID=\"" << PrintableString(res.EVSEID.bytes, res.EVSEID.bytesLen) << "\"";
        }
        return oss.str();
    }
    if (body.ServiceDiscoveryRes_isUsed) {
        const auto &res = body.ServiceDiscoveryRes;
        oss << "ServiceDiscoveryRes rc=" << static_cast<int>(res.ResponseCode)
            << " paymentOpts=" << res.PaymentOptions.PaymentOption.arrayLen;
        return oss.str();
    }
    if (body.ServicePaymentSelectionRes_isUsed) {
        oss << "ServicePaymentSelectionRes rc="
            << static_cast<int>(body.ServicePaymentSelectionRes.ResponseCode);
        return oss.str();
    }
    if (body.ContractAuthenticationRes_isUsed) {
        oss << "ContractAuthenticationRes rc="
            << static_cast<int>(body.ContractAuthenticationRes.ResponseCode);
        return oss.str();
    }
    if (body.ChargeParameterDiscoveryRes_isUsed) {
        const auto &res = body.ChargeParameterDiscoveryRes;
        oss << "ChargeParameterDiscoveryRes rc=" << static_cast<int>(res.ResponseCode)
            << " processing=" << static_cast<int>(res.EVSEProcessing);
        if (res.DC_EVSEChargeParameter_isUsed) {
            const auto &dc = res.DC_EVSEChargeParameter;
            oss << " Vmax=" << DescribePhysicalValueDetailed(dc.EVSEMaximumVoltageLimit)
                << " Imax=" << DescribePhysicalValueDetailed(dc.EVSEMaximumCurrentLimit)
                << " Vmin=" << DescribePhysicalValueDetailed(dc.EVSEMinimumVoltageLimit)
                << " Imin=" << DescribePhysicalValueDetailed(dc.EVSEMinimumCurrentLimit)
                << " powerUsed=" << dc.EVSEMaximumPowerLimit_isUsed;
            if (dc.EVSEMaximumPowerLimit_isUsed) {
                oss << " Pmax=" << DescribePhysicalValueDetailed(dc.EVSEMaximumPowerLimit);
            }
            oss << " Peak=" << DescribePhysicalValueDetailed(dc.EVSEPeakCurrentRipple);
            if (dc.EVSECurrentRegulationTolerance_isUsed) {
                oss << " Tol=" << DescribePhysicalValueDetailed(dc.EVSECurrentRegulationTolerance);
            }
            oss << " tolUsed=" << dc.EVSECurrentRegulationTolerance_isUsed
                << " energyUsed=" << dc.EVSEEnergyToBeDelivered_isUsed
                << " " << DescribeDcEvseStatus(dc.DC_EVSEStatus);
        }
        return oss.str();
    }
    if (body.CableCheckRes_isUsed) {
        const auto &res = body.CableCheckRes;
        oss << "CableCheckRes rc=" << static_cast<int>(res.ResponseCode)
            << " processing=" << static_cast<int>(res.EVSEProcessing) << " "
            << DescribeDcEvseStatus(res.DC_EVSEStatus);
        return oss.str();
    }
    if (body.PreChargeRes_isUsed) {
        const auto &res = body.PreChargeRes;
        oss << "PreChargeRes rc=" << static_cast<int>(res.ResponseCode)
            << " EVSEPresentVoltage=" << DescribePhysicalValueDetailed(res.EVSEPresentVoltage)
            << " " << DescribeDcEvseStatus(res.DC_EVSEStatus);
        return oss.str();
    }
    if (body.PowerDeliveryRes_isUsed) {
        oss << "PowerDeliveryRes rc=" << static_cast<int>(body.PowerDeliveryRes.ResponseCode);
        if (body.PowerDeliveryRes.DC_EVSEStatus_isUsed) {
            oss << " DC-" << DescribeDcEvseStatus(body.PowerDeliveryRes.DC_EVSEStatus);
        }
        if (body.PowerDeliveryRes.EVSEStatus_isUsed) {
            oss << " EVSEStatusUsed";
        }
        return oss.str();
    }
    if (body.CurrentDemandRes_isUsed) {
        const auto &res = body.CurrentDemandRes;
        oss << "CurrentDemandRes rc=" << static_cast<int>(res.ResponseCode)
            << " EVSEPresentVoltage=" << DescribePhysicalValueDetailed(res.EVSEPresentVoltage)
            << " EVSEPresentCurrent=" << DescribePhysicalValueDetailed(res.EVSEPresentCurrent)
            << " limits(V=" << res.EVSEVoltageLimitAchieved
            << ",I=" << res.EVSECurrentLimitAchieved
            << ",P=" << res.EVSEPowerLimitAchieved << ")";
        return oss.str();
    }
    if (body.SessionStopRes_isUsed) {
        oss << "SessionStopRes rc=" << static_cast<int>(body.SessionStopRes.ResponseCode);
        return oss.str();
    }
    oss << "Unknown DIN body";
    return oss.str();
}

void ConfigureStubFromExpected(const din_exiDocument &doc) {
    const auto &body = doc.V2G_Message.Body;
    if (body.PreChargeRes_isUsed) {
        float voltage = static_cast<float>(DecodePhysicalValue(body.PreChargeRes.EVSEPresentVoltage));
        dc_stub_set_bus_voltage(voltage);
        return;
    }
    if (body.CurrentDemandRes_isUsed) {
        float voltage = static_cast<float>(DecodePhysicalValue(body.CurrentDemandRes.EVSEPresentVoltage));
        float current = static_cast<float>(DecodePhysicalValue(body.CurrentDemandRes.EVSEPresentCurrent));
        dc_stub_set_bus_voltage(voltage);
        dc_stub_set_bus_current(current);
    }
}

void ConfigureStatusOverride(const din_exiDocument &doc) {
    const din_DC_EVSEStatusType *status = nullptr;
    const auto &body = doc.V2G_Message.Body;
    if (body.ChargeParameterDiscoveryRes_isUsed &&
        body.ChargeParameterDiscoveryRes.DC_EVSEChargeParameter_isUsed) {
        status = &body.ChargeParameterDiscoveryRes.DC_EVSEChargeParameter.DC_EVSEStatus;
    } else if (body.CableCheckRes_isUsed) {
        status = &body.CableCheckRes.DC_EVSEStatus;
    } else if (body.PreChargeRes_isUsed) {
        status = &body.PreChargeRes.DC_EVSEStatus;
    } else if (body.CurrentDemandRes_isUsed) {
        status = &body.CurrentDemandRes.DC_EVSEStatus;
    } else if (body.PowerDeliveryRes_isUsed && body.PowerDeliveryRes.DC_EVSEStatus_isUsed) {
        status = &body.PowerDeliveryRes.DC_EVSEStatus;
    } else {
        status = nullptr;
    }

    if (status) {
        tcp_test_override_evse_status(static_cast<int>(status->EVSEStatusCode),
                                      status->EVSEIsolationStatus_isUsed ? 1 : 0);
    } else {
        tcp_test_clear_evse_status_override();
    }
}

void ExpectFrameEq(const std::vector<uint8_t> &expected, const std::vector<uint8_t> &actual,
                   size_t index) {
    ASSERT_EQ(expected.size(), actual.size()) << "Frame " << index << " length mismatch\nExpected: "
                                              << HexDump(expected) << "\nActual  : "
                                              << HexDump(actual);
    for (size_t i = 0; i < expected.size(); ++i) {
        ASSERT_EQ(expected[i], actual[i]) << "Frame " << index << " differs at byte " << i
                                          << "\nExpected: " << HexDump(expected)
                                          << "\nActual  : " << HexDump(actual);
    }
}

void SendFrame(const std::vector<uint8_t> &frame) {
    tcp_process_socket_payload(frame.data(), static_cast<uint16_t>(frame.size()));
}

std::vector<uint8_t> PopSingleResponse() {
    EXPECT_FALSE(g_tcp_frames.empty());
    auto frame = g_tcp_frames.front();
    g_tcp_frames.erase(g_tcp_frames.begin());
    return frame;
}

class DinEndToEndTest : public ::testing::Test {
protected:
    void SetUp() override {
        slac_test_reset_state();
        tcp_transport_reset();
        tcp_register_socket_sender(&CaptureTcpPayload);
        g_tcp_frames.clear();
        slac_test_set_millis(0);
        tcp_transport_connected();
        std::copy(kEvseMac.begin(), kEvseMac.end(), myMac);
        setSeccIp();
        memcpy(EvccIp, kEvIp.data(), kEvIp.size());
        dc_stub_reset_measurements();
        tcp_test_clear_evse_status_override();
    }

    void TearDown() override {
        tcp_register_socket_sender(nullptr);
    }
};

}  // namespace

TEST_F(DinEndToEndTest, ReplayDemoLogProducesRecordedResponses) {
    const LogTrace trace = LoadLogTrace(kDemoLogPath);
    ASSERT_FALSE(trace.requests.empty()) << "No requests parsed from " << kDemoLogPath;
    ASSERT_EQ(trace.requests.size(), trace.responses.size())
        << "Log files must contain equal numbers of requests and responses";

    for (size_t i = 0; i < trace.requests.size(); ++i) {
        const auto &expectedFrame = trace.responses[i];
        auto expectedDoc = DecodeDinResponse(expectedFrame);
        if (expectedDoc.valid) {
            ConfigureStubFromExpected(expectedDoc.doc);
            ConfigureStatusOverride(expectedDoc.doc);
        } else {
            tcp_test_clear_evse_status_override();
        }
        ASSERT_TRUE(g_tcp_frames.empty()) << "Unexpected queued response before request " << i;
        SendFrame(trace.requests[i]);
        ASSERT_FALSE(g_tcp_frames.empty()) << "Missing response for request index " << i;
        auto actual = PopSingleResponse();
        auto actualDoc = DecodeDinResponse(actual);
        if (expectedDoc.valid && actualDoc.valid) {
            SCOPED_TRACE("Frame " + std::to_string(i) + "\nExpected: " +
                         DescribeDinMessage(expectedDoc.doc) + "\nActual: " +
                         DescribeDinMessage(actualDoc.doc));
            ExpectFrameEq(expectedFrame, actual, i);
        } else {
            ExpectFrameEq(expectedFrame, actual, i);
        }
    }

    EXPECT_TRUE(g_tcp_frames.empty());
}
