#include <Arduino.h>
#include <unity.h>

extern "C" {
#include "cbv2g/common/exi_bitstream.h"
#include "cbv2g/app_handshake/appHand_Datatypes.h"
#include "cbv2g/app_handshake/appHand_Encoder.h"
#include "cbv2g/app_handshake/appHand_Decoder.h"
#include "cbv2g/din/din_msgDefDatatypes.h"
#include "cbv2g/din/din_msgDefEncoder.h"
#include "cbv2g/din/din_msgDefDecoder.h"
}

static void test_apphand_encode_decode(void) {
    struct appHand_exiDocument doc;
    init_appHand_exiDocument(&doc);
    doc.supportedAppProtocolReq_isUsed = 1;
    auto &req = doc.supportedAppProtocolReq;
    req.AppProtocol.arrayLen = 1;
    auto &entry = req.AppProtocol.array[0];
    entry.SchemaID = 5;
    entry.VersionNumberMajor = 2;
    entry.VersionNumberMinor = 1;
    entry.Priority = 100;
    const char *ns = "urn:iso:15118:2:2013:MsgDef";
    size_t nsLen = strlen(ns);
    entry.ProtocolNamespace.charactersLen = nsLen;
    memcpy(entry.ProtocolNamespace.characters, ns, nsLen);

    uint8_t buffer[256];
    exi_bitstream_t stream;
    exi_bitstream_init(&stream, buffer, sizeof(buffer), 0, nullptr);
    TEST_ASSERT_EQUAL_INT(0, encode_appHand_exiDocument(&stream, &doc));
    size_t len = exi_bitstream_get_length(&stream);

    struct appHand_exiDocument decoded;
    init_appHand_exiDocument(&decoded);
    exi_bitstream_t dec_stream;
    exi_bitstream_init(&dec_stream, buffer, len, 0, nullptr);
    TEST_ASSERT_EQUAL_INT(0, decode_appHand_exiDocument(&dec_stream, &decoded));

    TEST_ASSERT_TRUE(decoded.supportedAppProtocolReq_isUsed);
    TEST_ASSERT_EQUAL_UINT(1, decoded.supportedAppProtocolReq.AppProtocol.arrayLen);
    const auto &decodedEntry = decoded.supportedAppProtocolReq.AppProtocol.array[0];
    TEST_ASSERT_EQUAL_UINT8(5, decodedEntry.SchemaID);
    TEST_ASSERT_EQUAL_UINT16(100, decodedEntry.Priority);
    TEST_ASSERT_EQUAL_STRING_LEN(ns,
                                 decodedEntry.ProtocolNamespace.characters,
                                 decodedEntry.ProtocolNamespace.charactersLen);
}

static void test_din_session_setup_roundtrip(void) {
    struct din_exiDocument doc;
    init_din_exiDocument(&doc);
    init_din_V2G_Message(&doc.V2G_Message);
    init_din_MessageHeaderType(&doc.V2G_Message.Header);
    init_din_BodyType(&doc.V2G_Message.Body);
    doc.V2G_Message.Body.SessionSetupRes_isUsed = 1;
    init_din_SessionSetupResType(&doc.V2G_Message.Body.SessionSetupRes);
    auto &res = doc.V2G_Message.Body.SessionSetupRes;
    res.ResponseCode = din_responseCodeType_OK_NewSessionEstablished;
    const char *evseId = "DE*UNIT*EVSE*0001";
    size_t idLen = strlen(evseId);
    res.EVSEID.bytesLen = idLen;
    memcpy(res.EVSEID.bytes, evseId, idLen);

    uint8_t buffer[512];
    exi_bitstream_t stream;
    exi_bitstream_init(&stream, buffer, sizeof(buffer), 0, nullptr);
    TEST_ASSERT_EQUAL_INT(0, encode_din_exiDocument(&stream, &doc));
    size_t len = exi_bitstream_get_length(&stream);

    struct din_exiDocument decoded;
    init_din_exiDocument(&decoded);
    exi_bitstream_t dec_stream;
    exi_bitstream_init(&dec_stream, buffer, len, 0, nullptr);
    TEST_ASSERT_EQUAL_INT(0, decode_din_exiDocument(&dec_stream, &decoded));
    TEST_ASSERT_TRUE(decoded.V2G_Message.Body.SessionSetupRes_isUsed);
    const auto &decodedRes = decoded.V2G_Message.Body.SessionSetupRes;
    TEST_ASSERT_EQUAL(din_responseCodeType_OK_NewSessionEstablished, decodedRes.ResponseCode);
    TEST_ASSERT_EQUAL_UINT(idLen, decodedRes.EVSEID.bytesLen);
    TEST_ASSERT_EQUAL_MEMORY(evseId, decodedRes.EVSEID.bytes, idLen);
}

void setUp() {}
void tearDown() {}

void setup() {
    UNITY_BEGIN();
    RUN_TEST(test_apphand_encode_decode);
    RUN_TEST(test_din_session_setup_roundtrip);
    UNITY_END();
}

void loop() {}

#ifdef ESP_PLATFORM
extern "C" void app_main() {
    initArduino();
    setup();
    while (true) {
        loop();
        vTaskDelay(1);
    }
}
#endif
