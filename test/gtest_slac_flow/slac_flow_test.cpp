#include <gtest/gtest.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <vector>
#include <cstring>

#include "main.h"

extern "C" {
void slac_test_set_tx_hook(void (*hook)(const uint8_t *, uint32_t));
void slac_test_reset_state(void);
}

void SlacManager(uint16_t rxbytes);
void slac_test_set_millis(unsigned long value);
extern uint8_t txbuffer[];
extern uint8_t rxbuffer[];
extern uint8_t modem_state;
extern uint8_t myMac[];
extern uint8_t pevMac[];
extern uint8_t pevRunId[];
extern uint8_t NMK[];
extern uint8_t NID[];
extern uint8_t negotiatedSoundCount;
extern uint8_t negotiatedSoundTimeoutField;
extern uint32_t currentSoundWindowMs;
extern uint32_t currentMatchWindowMs;

namespace {

using Frame = std::vector<uint8_t>;

std::vector<Frame> g_captured_frames;

void CaptureTx(const uint8_t *data, uint32_t len) {
    g_captured_frames.emplace_back(data, data + len);
}

constexpr std::array<uint8_t, 6> kEvseMac{{0x70, 0xB3, 0xD5, 0x00, 0x00, 0x01}};
constexpr std::array<uint8_t, 6> kPevMac{{0xFE, 0xED, 0xBE, 0xEF, 0xAF, 0xFE}};
constexpr uint8_t kSoundCount = 3;
constexpr uint8_t kSoundTimeoutField = 0x08;
constexpr std::array<uint8_t, 8> kRunId{{0x10, 0x11, 0x12, 0x13, kSoundCount, kSoundTimeoutField, 0x16, 0x17}};
constexpr std::array<uint8_t, 16> kLogNmK{{0x77, 0x77, 0x73, 0x7F, 0x77, 0x77, 0x77, 0x77,
                                           0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77}};
constexpr std::array<uint8_t, 7> kLogNid{{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07}};
constexpr size_t kSlacParamLen = 60;
constexpr size_t kSlacMatchLen = 109;
constexpr size_t kAttenCharLen = 130;

Frame base_frame(uint8_t mmtype_low, uint8_t mmtype_high, size_t size) {
    Frame frame(size, 0);
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin());
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 6);
    frame[12] = 0x88;
    frame[13] = 0xE1;
    frame[14] = 0x01;
    frame[15] = mmtype_low;
    frame[16] = mmtype_high;
    frame[17] = 0x00;
    frame[18] = 0x00;
    frame[19] = 0x00;
    frame[20] = 0x00;
    return frame;
}

Frame make_slac_param_req(uint8_t sound_count, uint8_t timeout_field) {
    Frame frame = base_frame(0x64, 0x60, 60);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 21);
    frame[25] = sound_count;
    frame[26] = timeout_field;
    return frame;
}

Frame make_start_atten_char(uint8_t sound_count, uint8_t timeout_field) {
    Frame frame = base_frame(0x6A, 0x60, 60);
    frame[21] = sound_count;
    frame[22] = timeout_field;
    frame[23] = 0x01;
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 24);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 30);
    return frame;
}

Frame make_mnbc_sound(uint8_t remaining) {
    Frame frame = base_frame(0x76, 0x60, 71);
    frame[38] = remaining;
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 39);
    for (size_t i = 55; i < 71; ++i) frame[i] = 0xFF;
    return frame;
}

Frame make_atten_profile(uint8_t base_value) {
    Frame frame = base_frame(0x86, 0x60, 90);
    for (size_t i = 0; i < 58; ++i) {
        frame[27 + i] = base_value + i;
    }
    return frame;
}

Frame make_atten_char_rsp() {
    Frame frame = base_frame(0x6F, 0x60, 70);
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 21);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 27);
    frame[69] = 0x00;
    return frame;
}

Frame make_slac_match_req() {
    Frame frame = base_frame(0x7C, 0x60, 109);
    frame[21] = 0x3E;
    frame[22] = 0x00;
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 40);
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin() + 63);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 69);
    return frame;
}

Frame expected_slac_param_cnf() {
    Frame frame(kSlacParamLen, 0);
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin());
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin() + 6);
    frame[12] = 0x88;
    frame[13] = 0xE1;
    frame[14] = 0x01;
    frame[15] = 0x65;
    frame[16] = 0x60;
    std::fill(frame.begin() + 19, frame.begin() + 25, 0xFF);
    frame[25] = kSoundCount;
    frame[26] = kSoundTimeoutField;
    frame[27] = 0x01;
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 28);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 36);
    return frame;
}

Frame expected_atten_char_ind() {
    Frame frame(kAttenCharLen, 0);
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin());
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin() + 6);
    frame[12] = 0x88;
    frame[13] = 0xE1;
    frame[14] = 0x01;
    frame[15] = 0x6E;
    frame[16] = 0x60;
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 21);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 27);
    frame[69] = kSoundCount;
    frame[70] = 0x3A;
    for (int i = 0; i < 58; ++i) {
        frame[71 + i] = static_cast<uint8_t>(11 + i);
    }
    return frame;
}

Frame expected_slac_match_cnf() {
    Frame frame(kSlacMatchLen, 0);
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin());
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin() + 6);
    frame[12] = 0x88;
    frame[13] = 0xE1;
    frame[14] = 0x01;
    frame[15] = 0x7D;
    frame[16] = 0x60;
    frame[21] = 0x3E;
    frame[22] = 0x00;
    std::copy(kPevMac.begin(), kPevMac.end(), frame.begin() + 40);
    std::copy(kEvseMac.begin(), kEvseMac.end(), frame.begin() + 63);
    std::copy(kRunId.begin(), kRunId.end(), frame.begin() + 69);
    std::copy(kLogNid.begin(), kLogNid.end(), frame.begin() + 85);
    std::copy(kLogNmK.begin(), kLogNmK.end(), frame.begin() + 93);
    return frame;
}

void ExpectFrameEq(const Frame &expected, const Frame &actual, const char *label) {
    ASSERT_EQ(expected.size(), actual.size()) << label << " length mismatch";
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(expected[i], actual[i]) << label << " byte " << i;
    }
}

void feed_frame(const Frame &frame) {
    std::memcpy(rxbuffer, frame.data(), frame.size());
    SlacManager(static_cast<uint16_t>(frame.size()));
}

class SlacFlowTest : public ::testing::Test {
protected:
    void SetUp() override {
        g_captured_frames.clear();
        slac_test_reset_state();
        slac_test_set_tx_hook(&CaptureTx);
        std::copy(kEvseMac.begin(), kEvseMac.end(), myMac);
        std::copy(kLogNmK.begin(), kLogNmK.end(), NMK);
        std::copy(kLogNid.begin(), kLogNid.end(), NID);
        slac_test_set_millis(0);
    }
};

} // namespace

TEST_F(SlacFlowTest, ReplaysRecordedSequence) {
    auto slac_param_req = make_slac_param_req(kSoundCount, kSoundTimeoutField);
    feed_frame(slac_param_req);
    ASSERT_EQ(modem_state, SLAC_PARAM_CNF);
    EXPECT_EQ(0, std::memcmp(pevMac, kPevMac.data(), kPevMac.size()));
    EXPECT_EQ(0, std::memcmp(pevRunId, kRunId.data(), kRunId.size()));
    ASSERT_EQ(g_captured_frames.size(), 1u);
    ExpectFrameEq(expected_slac_param_cnf(), g_captured_frames[0], "SLAC_PARAM.CNF");
    g_captured_frames.clear();

    auto start_atten = make_start_atten_char(kSoundCount, kSoundTimeoutField);
    feed_frame(start_atten);
    ASSERT_EQ(modem_state, MNBC_SOUND);
    EXPECT_EQ(negotiatedSoundCount, kSoundCount);
    EXPECT_EQ(negotiatedSoundTimeoutField, kSoundTimeoutField);

    for (int i = 0; i < kSoundCount; ++i) {
        feed_frame(make_mnbc_sound(static_cast<uint8_t>(kSoundCount - 1 - i)));
        feed_frame(make_atten_profile(static_cast<uint8_t>(10 + i)));
    }
    ASSERT_EQ(g_captured_frames.size(), 1u);
    ExpectFrameEq(expected_atten_char_ind(), g_captured_frames[0], "ATTEN_CHAR.IND");
    g_captured_frames.clear();

    feed_frame(make_atten_char_rsp());
    ASSERT_EQ(modem_state, ATTEN_CHAR_RSP);
    EXPECT_GT(currentMatchWindowMs, currentSoundWindowMs);

    feed_frame(make_slac_match_req());
    ASSERT_EQ(modem_state, MODEM_GET_SW_REQ);
    ASSERT_EQ(g_captured_frames.size(), 1u);
    ExpectFrameEq(expected_slac_match_cnf(), g_captured_frames[0], "SLAC_MATCH.CNF");
}
