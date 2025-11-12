/*----------------------------------------------------------------------------
;    
; Project:   EVSE PLC tests
;
;
; Permission is hereby granted, free of charge, to any person obtaining a copy
; of this software and associated documentation files (the "Software"), to deal
; in the Software without restriction, including without limitation the rights
; to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
; copies of the Software, and to permit persons to whom the Software is
; furnished to do so, subject to the following conditions:
;
; The above copyright notice and this permission notice shall be included in
; all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
; IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
; FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
; AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
; LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
; OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
; THE SOFTWARE.
;-----------------------------------------------------------------------------*/

// Code snippets from Qualcomm's linux driver, open-plc-tools,
// and the majority from uhi22's pyPLC. see https://github.com/uhi22/pyPLC
//
// Information on setting up the SLAC communication can be found in ISO 15118-3:2016
//


#include <Arduino.h>
#include <SPI.h>
#include <ArduinoJson.h>
#include <vector>
#include <string>
#include <mbedtls/base64.h>
#include <ctype.h>
#include <string.h>

#if __has_include("freertos/FreeRTOS.h")
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#include "main.h"
#include "evse_config.h"
#include "ipv6.h"
#include "tcp.h"
#include "cp_control.h"
#include "dc_can.h"
#include "lwip_bridge.h"
#include "sdp_server.h"
#include "tcp_socket_server.h"
#include "pki_store.h"
#include "tls_server.h"
#include "tls_credentials.h"
#include "iso15118_dc.h"
#include "diag_auth.h"
#include "iso_watchdog.h"


uint8_t txbuffer[3164], rxbuffer[3164];
uint8_t modem_state;
uint8_t myMac[6]; // the MAC of the EVSE (derived from the ESP32's MAC).
uint8_t pevMac[6]; // the MAC of the PEV.
uint8_t myModemMac[6]; // our own modem's MAC (this is different from myMAC !). Unused.
uint8_t pevModemMac[6]; // the MAC of the PEV's modem (obtained with GetSwReq). Could this be used to identify the EV?
uint8_t pevRunId[8]; // pev RunId. Received from the PEV in the CM_SLAC_PARAM.REQ message.
uint16_t AvgACVar[58]; // Average AC Variable Field. (used in CM_ATTEN_PROFILE.IND)
uint8_t NMK[16]; // Network Key. Will be initialized with a random key on each session.
uint8_t NID[] = {1, 2, 3, 4, 5, 6, 7}; // a default network ID. MSB bits 6 and 7 need to be 0.
unsigned long SoundsTimer = 0;
unsigned long ModemSearchTimer = 0;
unsigned long AttenCharResponseTimer = 0;
unsigned long SlacMatchTimer = 0;
uint8_t ModemsFound = 0;
uint8_t ReceivedSounds = 0;
uint8_t ReceivedProfiles = 0;
uint8_t EVCCID[6];  // Mac address or ID from the PEV, used in V2G communication
uint8_t EVSOC = 0;  // State Of Charge of the EV, obtained from the 'ContractAuthenticationRequest' message

static void cli_poll(void);
static void cli_process_line(const String &line);
static bool cli_process_json(const String &line);
static bool cli_decode_base64(const String &src, std::string &out);
static bool cli_encode_base64(const std::string &src, std::string &out);

const uint8_t MAX_SUPPORTED_SOUND_COUNT = 20;
const uint8_t SLAC_MAX_RETRIES = 3;
const uint8_t ATTEN_CHAR_MAX_RETRIES = 3;
const uint16_t ATTEN_CHAR_IND_FRAME_LEN = 130;
const uint32_t ATTEN_CHAR_RESPONSE_TIMEOUT_MS = 500;
const uint32_t SLAC_MATCH_TIMEOUT_MS = 1000;
const uint32_t MIN_SOUND_WINDOW_MS = 200;
const uint32_t SOUND_TIMEOUT_UNIT_MS = 100;
const uint16_t EXPECTED_MATCH_MVF_LEN = 0x003E;
const uint8_t INVALID_FRAME_THRESHOLD = 3;

uint8_t requestedSoundCount = 10;
uint8_t negotiatedSoundCount = 10;
uint8_t negotiatedSoundTimeoutField = 0x06;
uint32_t currentSoundWindowMs = 600;
uint8_t slacRetryCounter = 0;
uint8_t attenCharRetryCounter = 0;
uint8_t invalidFrameCounter = 0;


void SPI_InterruptHandler() { // Interrupt handler is currently unused

    volatile uint16_t rx_data;

    // Write zero into the SPI_REG_INTR_ENABLE register
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(QCA7K_SPI_WRITE | QCA7K_SPI_INTERNAL | SPI_REG_INTR_ENABLE);
    SPI.transfer16(0);                  // write the value to the bus
    digitalWrite(PIN_QCA700X_CS, HIGH);
    // Read the Interrupt Cause register
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(QCA7K_SPI_READ | QCA7K_SPI_INTERNAL | SPI_REG_INTR_CAUSE);
    rx_data = SPI.transfer16(0x0000);   // read the reason of the interrrupt
    digitalWrite(PIN_QCA700X_CS, HIGH);
    // Write contents back to Interrupt Cause register
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(QCA7K_SPI_WRITE | QCA7K_SPI_INTERNAL | SPI_REG_INTR_CAUSE);
    SPI.transfer16(rx_data);   
    digitalWrite(PIN_QCA700X_CS, HIGH);

    


    // Re-enable Packet Available interrupt
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(QCA7K_SPI_WRITE | QCA7K_SPI_INTERNAL | SPI_REG_INTR_ENABLE);
    SPI.transfer16(SPI_INT_PKT_AVLBL);   
    digitalWrite(PIN_QCA700X_CS, HIGH);

}

uint16_t qcaspi_read_register16(uint16_t reg) {
    uint16_t tx_data;
    uint16_t rx_data;

    tx_data = QCA7K_SPI_READ | QCA7K_SPI_INTERNAL | reg;
    
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(tx_data);                // send the command to read the internal register
    rx_data = SPI.transfer16(0x0000);       // read the data on the bus
    digitalWrite(PIN_QCA700X_CS, HIGH);

    return rx_data;
}

void qcaspi_write_register(uint16_t reg, uint16_t value) {
    uint16_t tx_data;

    tx_data = QCA7K_SPI_WRITE | QCA7K_SPI_INTERNAL | reg;

    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(tx_data);                // send the command to write the internal register
    SPI.transfer16(value);                  // write the value to the bus
    digitalWrite(PIN_QCA700X_CS, HIGH);

}

void qcaspi_write_burst(uint8_t *src, uint32_t len) {
    uint16_t total_len;
    uint8_t buf[10];

    buf[0] = 0xAA;
	  buf[1] = 0xAA;
	  buf[2] = 0xAA;
	  buf[3] = 0xAA;
	  buf[4] = (uint8_t)((len >> 0) & 0xFF);
	  buf[5] = (uint8_t)((len >> 8) & 0xFF);
	  buf[6] = 0;
	  buf[7] = 0;

    total_len = len + 10;
    // Write nr of bytes to write to SPI_REG_BFR_SIZE
    qcaspi_write_register(SPI_REG_BFR_SIZE, total_len);
    //Serial.printf("Write buffer bytes sent: %u\n", total_len);

//    Serial.print("[TX] ");
//    for(int x=0; x< len; x++) Serial.printf("%02x ",src[x]);
//    Serial.printf("\n");
    
    digitalWrite(PIN_QCA700X_CS, LOW);
    SPI.transfer16(QCA7K_SPI_WRITE | QCA7K_SPI_EXTERNAL);      // Write External
    SPI.transfer(buf, 8);     // Header
    SPI.transfer(src, len);   // Data
    SPI.transfer16(0x5555);   // Footer
    digitalWrite(PIN_QCA700X_CS, HIGH);
}

uint32_t qcaspi_read_burst(uint8_t *dst) {
    uint16_t available, rxbytes;

    available = qcaspi_read_register16(SPI_REG_RDBUF_BYTE_AVA);

    if (available && available <= QCA7K_BUFFER_SIZE) {    // prevent buffer overflow
        // Write nr of bytes to read to SPI_REG_BFR_SIZE
        qcaspi_write_register(SPI_REG_BFR_SIZE, available);
        
        digitalWrite(PIN_QCA700X_CS, LOW);
        SPI.transfer16(QCA7K_SPI_READ | QCA7K_SPI_EXTERNAL);
        SPI.transfer(dst, available);
        digitalWrite(PIN_QCA700X_CS, HIGH);

        return available;   // return nr of bytes in the rxbuffer
    }
    return 0;
}

void randomizeNmk() {
    // randomize the Network Membership Key (NMK)
    for (uint8_t i=0; i<16; i++) NMK[i] = random(256); // NMK
    for (uint8_t i=0; i<7; i++) {
        NID[i] = NMK[i];
    }
    NID[0] &= 0x3F; // ensure upper two bits are zero
}

void setNmkAt(uint16_t index) {
    // sets the Network Membership Key (NMK) at a certain position in the transmit buffer
    for (uint8_t i=0; i<16; i++) txbuffer[index+i] = NMK[i]; // NMK 
}

void setNidAt(uint16_t index) {
    // copies the network ID (NID, 7 bytes) into the wished position in the transmit buffer
    for (uint8_t i=0; i<7; i++) txbuffer[index+i] = NID[i];
}

void setMacAt(uint8_t *mac, uint16_t offset) {
    // at offset 0 in the ethernet frame, we have the destination MAC
    // at offset 6 in the ethernet frame, we have the source MAC
    for (uint8_t i=0; i<6; i++) txbuffer[offset+i]=mac[i];
}

void setRunId(uint16_t offset) {
    // at the given offset in the transmit buffer, fill the 8-bytes-RunId.
    for (uint8_t i=0; i<8; i++) txbuffer[offset+i]=pevRunId[i];
}

void setACVarField(uint16_t offset, uint8_t samples) {
    uint8_t divisor = samples ? samples : 1;
    for (uint8_t i=0; i<58; i++) {
        txbuffer[offset+i] = (uint8_t)(AvgACVar[i] / divisor);
    }
}    

uint16_t getManagementMessageType() {
    // calculates the MMTYPE (base value + lower two bits), see Table 11-2 of homeplug spec
    return rxbuffer[16]*256 + rxbuffer[15];
}

uint16_t getFrameType() {
    // returns the Ethernet Frame type
    // 88E1 = HomeplugAV 
    // 86DD = IPv6
    return rxbuffer[12]*256 + rxbuffer[13];
}



void ModemReset() {
    uint16_t reg16;
    Serial.printf("Reset QCA700X Modem. ");
    reg16 = qcaspi_read_register16(SPI_REG_SPI_CONFIG);
    reg16 = reg16 | SPI_INT_CPU_ON;     // Reset QCA700X
    qcaspi_write_register(SPI_REG_SPI_CONFIG, reg16);
}


void composeSetKey() {
    
    memset(txbuffer, 0x00, 60);  // clear buffer
    txbuffer[0]=0x00; // Destination MAC
    txbuffer[1]=0xB0;
    txbuffer[2]=0x52;
    txbuffer[3]=0x00;
    txbuffer[4]=0x00;
    txbuffer[5]=0x01;                
    setMacAt(myMac, 6);  // Source MAC         
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x01; // version
    txbuffer[15]=0x08; // CM_SET_KEY.REQ
    txbuffer[16]=0x60; 
    txbuffer[17]=0x00; // frag_index
    txbuffer[18]=0x00; // frag_seqnum
    txbuffer[19]=0x01; // 0 key info type
                     // 20-23 my nonce (0x00 in spec!)
                     // 24-27 your nonce
    txbuffer[28]=0x04; // 9 nw info pid
        
    txbuffer[29]=0x00; // 10 info prn
    txbuffer[30]=0x00; // 11
    txbuffer[31]=0x00; // 12 pmn
    txbuffer[32]=0x00; // 13 CCo capability
    setNidAt(33);    // 14-20 nid  7 bytes from 33 to 39
                     // Network ID to be associated with the key distributed herein.
                     // The 54 LSBs of this field contain the NID (refer to Section 3.4.3.1). The
                     // two MSBs shall be set to 0b00.
    txbuffer[40]=0x01; // NewEKS. Table A.8 01 is NMK.
    setNmkAt(41); 
}

void composeGetSwReq() {
		// GET_SW.REQ request
    memset(txbuffer, 0x00, 60);  // clear buffer
    txbuffer[0]=0xff;  // Destination MAC Broadcast
    txbuffer[1]=0xff;
    txbuffer[2]=0xff;
    txbuffer[3]=0xff;
    txbuffer[4]=0xff;
    txbuffer[5]=0xff;                
    setMacAt(myMac, 6);  // Source MAC         
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x00; // version
    txbuffer[15]=0x00; // GET_SW.REQ
    txbuffer[16]=0xA0;  
    txbuffer[17]=0x00; // Vendor OUI
    txbuffer[18]=0xB0;  
    txbuffer[19]=0x52;  
}

void composeSlacParamCnf() {

    memset(txbuffer, 0x00, 60);  // clear txbuffer
    setMacAt(pevMac, 0);  // Destination MAC
    setMacAt(myMac, 6);  // Source MAC
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x01; // version
    txbuffer[15]=0x65; // SLAC_PARAM.CNF
    txbuffer[16]=0x60; // 
    txbuffer[17]=0x00; // 2 bytes fragmentation information. 0000 means: unfragmented.
    txbuffer[18]=0x00; // 
    txbuffer[19]=0xff; // 19-24 sound target
    txbuffer[20]=0xff; 
    txbuffer[21]=0xff; 
    txbuffer[22]=0xff; 
    txbuffer[23]=0xff; 
    txbuffer[24]=0xff; 
    txbuffer[25]=negotiatedSoundCount; // negotiated sound count
    txbuffer[26]=negotiatedSoundTimeoutField; // timeout (EV requested or fallback)
    txbuffer[27]=0x01; // resptype
    setMacAt(pevMac, 28);  // forwarding_sta, same as PEV MAC, plus 2 bytes 00 00
    txbuffer[34]=0x00; // 
    txbuffer[35]=0x00; // 
    setRunId(36);  // 36 to 43 runid 8 bytes 
    // rest is 00
}

void composeAttenCharInd() {
    
    memset(txbuffer, 0x00, 130);  // clear txbuffer
    setMacAt(pevMac, 0);  // Destination MAC
    setMacAt(myMac, 6);  // Source MAC
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x01; // version
    txbuffer[15]=0x6E; // ATTEN_CHAR.IND
    txbuffer[16]=0x60;  
    txbuffer[17]=0x00; // 2 bytes fragmentation information. 0000 means: unfragmented.
    txbuffer[18]=0x00; // 
    txbuffer[19]=0x00; // apptype
    txbuffer[20]=0x00; // security
    setMacAt(pevMac, 21); // Mac address of the EV Host which initiates the SLAC process
    setRunId(27); // RunId 8 bytes 
    txbuffer[35]=0x00; // 35 - 51 source_id, 17 bytes 0x00 (defined in ISO15118-3 table A.4)
        
    txbuffer[52]=0x00; // 52 - 68 response_id, 17 bytes 0x00. (defined in ISO15118-3 table A.4)
    
    uint8_t reportedSounds = ReceivedSounds ? ReceivedSounds : ReceivedProfiles;
    if (reportedSounds > negotiatedSoundCount) reportedSounds = negotiatedSoundCount;
    uint8_t samplesForAverage = ReceivedProfiles ? ReceivedProfiles : (reportedSounds ? reportedSounds : 1);
    txbuffer[69]=reportedSounds; // Number of sounds reported back to the EV.
    txbuffer[70]=0x3A; // Number of groups = 58. (defined in ISO15118-3 table A.4)
    setACVarField(71, samplesForAverage); // 71 to 128: The group attenuation for the 58 announced groups.
 }


void transmitAttenCharInd(const char *reason) {
    composeAttenCharInd();
    qcaspi_write_burst(txbuffer, ATTEN_CHAR_IND_FRAME_LEN);
    modem_state = ATTEN_CHAR_IND;
    AttenCharResponseTimer = millis();
    if (attenCharRetryCounter < 255) attenCharRetryCounter++;
    Serial.printf("transmitting CM_ATTEN_CHAR.IND (%s) attempt %u/%u\n",
                  reason ? reason : "start",
                  attenCharRetryCounter,
                  ATTEN_CHAR_MAX_RETRIES);
}


void composeSlacMatchCnf() {
    
    memset(txbuffer, 0x00, 109);  // clear txbuffer
    setMacAt(pevMac, 0);  // Destination MAC
    setMacAt(myMac, 6);  // Source MAC
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x01; // version
    txbuffer[15]=0x7D; // SLAC_MATCH.CNF
    txbuffer[16]=0x60; // 
    txbuffer[17]=0x00; // 2 bytes fragmentation information. 0000 means: unfragmented.
    txbuffer[18]=0x00; // 
    txbuffer[19]=0x00; // apptype
    txbuffer[20]=0x00; // security
    txbuffer[21]=(uint8_t)(EXPECTED_MATCH_MVF_LEN & 0xFF); // MVF length LSB
    txbuffer[22]=(uint8_t)(EXPECTED_MATCH_MVF_LEN >> 8);  
                          // 23 - 39: pev_id 17 bytes. All zero.
    setMacAt(pevMac, 40); // Pev Mac address
                          // 46 - 62: evse_id 17 bytes. All zero.
    setMacAt(myMac, 63);  // 63 - 68 evse_mac 
    setRunId(69);         // runid 8 bytes 69-76 run_id.
                          // 77 to 84 reserved 0
    setNidAt(85);         // 85-91 NID. We can nearly freely choose this, but the upper two bits need to be zero
                          // 92 reserved 0                                 
    setNmkAt(93);         // 93 to 108 NMK. We can freely choose this. Normally we should use a random number. 
}        

void composeFactoryDefaults() {

    memset(txbuffer, 0x00, 60);  // clear buffer
    txbuffer[0]=0x00; // Destination MAC
    txbuffer[1]=0xB0;
    txbuffer[2]=0x52;
    txbuffer[3]=0x00;
    txbuffer[4]=0x00;
    txbuffer[5]=0x01;                
    setMacAt(myMac, 6); // Source MAC         
    txbuffer[12]=0x88; // Protocol HomeplugAV
    txbuffer[13]=0xE1;
    txbuffer[14]=0x00; // version
    txbuffer[15]=0x7C; // Load modem Factory Defaults (same as holding GPIO3 low for 15 secs)
    txbuffer[16]=0xA0; 
    txbuffer[17]=0x00; 
    txbuffer[18]=0xB0; 
    txbuffer[19]=0x52; 
}

void handleSlacFailure(const char *reason) {
    Serial.printf("SLAC failure: %s (attempt %u/%u)\n",
                  reason ? reason : "unknown",
                  slacRetryCounter + 1,
                  SLAC_MAX_RETRIES);
    if (slacRetryCounter < SLAC_MAX_RETRIES) {
        slacRetryCounter++;
    }
    attenCharRetryCounter = 0;
    ReceivedSounds = 0;
    ReceivedProfiles = 0;
    AttenCharResponseTimer = 0;
    SlacMatchTimer = 0;
    modem_state = MODEM_CONFIGURED;
}

// Received SLAC messages from the PEV are handled here
void SlacManager(uint16_t rxbytes) {
    uint16_t reg16, mnt, x;

    mnt = getManagementMessageType();
  
  //  Serial.print("[RX] ");
  //  for (x=0; x<rxbytes; x++) Serial.printf("%02x ",rxbuffer[x]);
  //  Serial.printf("\n");

    if (mnt == (CM_SET_KEY + MMTYPE_CNF)) {
        Serial.printf("received SET_KEY.CNF\n");
        if (rxbuffer[19] == 0x01) {
            modem_state = MODEM_CONFIGURED;
            // copy MAC from the EVSE modem to myModemMac. This MAC is not used for communication.
            memcpy(myModemMac, rxbuffer+6, 6);
            Serial.printf("NMK set\n");
        } else Serial.printf("NMK -NOT- set\n");

    } else if (mnt == (CM_SLAC_PARAM + MMTYPE_REQ)) {
        Serial.printf("received CM_SLAC_PARAM.REQ\n");
        // We received a SLAC_PARAM request from the PEV. This is the initiation of a SLAC procedure.
        // We extract the pev MAC from it.
        memcpy(pevMac, rxbuffer+6, 6);
        // extract the RunId from the SlacParamReq, and store it for later use
        memcpy(pevRunId, rxbuffer+21, 8);
        if (rxbytes > 26) {
            requestedSoundCount = rxbuffer[25];
            negotiatedSoundCount = requestedSoundCount;
            if (negotiatedSoundCount == 0) negotiatedSoundCount = 1;
            if (negotiatedSoundCount > MAX_SUPPORTED_SOUND_COUNT) negotiatedSoundCount = MAX_SUPPORTED_SOUND_COUNT;
            negotiatedSoundTimeoutField = rxbuffer[26] ? rxbuffer[26] : 0x06;
        } else {
            negotiatedSoundCount = 10;
            negotiatedSoundTimeoutField = 0x06;
        }
        currentSoundWindowMs = (uint32_t)negotiatedSoundTimeoutField * SOUND_TIMEOUT_UNIT_MS;
        if (currentSoundWindowMs < MIN_SOUND_WINDOW_MS) currentSoundWindowMs = MIN_SOUND_WINDOW_MS;
        slacRetryCounter = 0;
        attenCharRetryCounter = 0;
        ReceivedSounds = 0;
        ReceivedProfiles = 0;
        Serial.printf("Negotiated %u sounds, timeout field %u (~%lums)\n",
                      negotiatedSoundCount,
                      negotiatedSoundTimeoutField,
                      (unsigned long)currentSoundWindowMs);
        // We are EVSE, we want to answer.
        composeSlacParamCnf();
        qcaspi_write_burst(txbuffer, 60); // Send data to modem
        modem_state = SLAC_PARAM_CNF;
        Serial.printf("transmitting CM_SLAC_PARAM.CNF\n");

    } else if (mnt == (CM_START_ATTEN_CHAR + MMTYPE_IND) && modem_state == SLAC_PARAM_CNF) {
        Serial.printf("received CM_START_ATTEN_CHAR.IND\n");
        SoundsTimer = millis(); // start timer
        memset(AvgACVar, 0x00, sizeof(AvgACVar)); // reset averages.
        ReceivedSounds = 0;
        ReceivedProfiles = 0;
        attenCharRetryCounter = 0;
        modem_state = MNBC_SOUND;

    } else if (mnt == (CM_MNBC_SOUND + MMTYPE_IND) && modem_state == MNBC_SOUND) { 
        Serial.printf("received CM_MNBC_SOUND.IND\n");
        if (ReceivedSounds < 255) ReceivedSounds++;

    } else if (mnt == (CM_ATTEN_PROFILE + MMTYPE_IND) && modem_state == MNBC_SOUND) { 
        Serial.printf("received CM_ATTEN_PROFILE.IND\n");
        if (rxbytes < 85) {
            Serial.printf("Invalid ATTEN_PROFILE length\n");
            return;
        }
        if (ReceivedProfiles < negotiatedSoundCount) {
            for (x=0; x<58; x++) AvgACVar[x] += rxbuffer[27+x];
            ReceivedProfiles++;
            if (ReceivedProfiles >= negotiatedSoundCount && negotiatedSoundCount > 0) {
                transmitAttenCharInd("sounds complete");
            }
        }

    } else if (mnt == (CM_ATTEN_CHAR + MMTYPE_RSP) && modem_state == ATTEN_CHAR_IND) { 
        Serial.printf("received CM_ATTEN_CHAR.RSP\n");
        // verify pevMac, RunID, and succesful Slac fields
        if (memcmp(pevMac, rxbuffer+21, 6) == 0 && memcmp(pevRunId, rxbuffer+27, 8) == 0 && rxbuffer[69] == 0) {
            Serial.printf("Successful SLAC process\n");
            modem_state = ATTEN_CHAR_RSP;
            SlacMatchTimer = millis();
            attenCharRetryCounter = 0;
        } else {
            Serial.printf("ATTEN_CHAR.RSP validation failed (status=0x%02x)\n", rxbuffer[69]);
            if (attenCharRetryCounter < ATTEN_CHAR_MAX_RETRIES) {
                transmitAttenCharInd("RSP mismatch");
            } else {
                handleSlacFailure("ATTEN_CHAR.RSP invalid");
            }
        }

    } else if (mnt == (CM_SLAC_MATCH + MMTYPE_REQ) && modem_state == ATTEN_CHAR_RSP) { 
        Serial.printf("received CM_SLAC_MATCH.REQ\n"); 
        // Verify pevMac, RunID and MVFLength fields
        uint16_t mvfLength = rxbuffer[21] + (rxbuffer[22] << 8);
        if (memcmp(pevMac, rxbuffer+40, 6) == 0 && memcmp(pevRunId, rxbuffer+69, 8) == 0 && mvfLength == EXPECTED_MATCH_MVF_LEN) {
            composeSlacMatchCnf();
            qcaspi_write_burst(txbuffer, 109); // Send data to modem
            Serial.printf("transmitting CM_SLAC_MATCH.CNF\n");
            modem_state = MODEM_GET_SW_REQ;
            attenCharRetryCounter = 0;
            SlacMatchTimer = 0;
        } else {
            handleSlacFailure("SLAC_MATCH verification failed");
        }

    } else if (mnt == (CM_GET_SW + MMTYPE_CNF) && modem_state == MODEM_WAIT_SW) { 
        // Both the local and Pev modem will send their software version.
        // check if the MAC of the modem is the same as our local modem.
        if (memcmp(rxbuffer+6, myModemMac, 6) != 0) { 
            // Store the Pev modem MAC, as long as it is not random, we can use it for identifying the EV (Autocharge / Plug N Charge)
            memcpy(pevModemMac, rxbuffer+6, 6);
        }
        Serial.printf("received GET_SW.CNF\n");
        ModemsFound++;
    }
}




// Task
// 
// called every 20ms
//
void Timer20ms(void * parameter) {

    uint16_t reg16, rxbytes, mnt, x;
    uint16_t FrameType;
    
    while(1)  // infinite loop
    {
        cp_tick();
        if (!cp_is_connected()) {
            if (cp_is_contactor_commanded()) cp_contactor_command(false);
            if (dc_is_enabled()) dc_enable_output(false);
        }
        dc_can_tick();
        lwip_bridge_poll();

        switch(modem_state) {
          
            case MODEM_POWERUP:
                Serial.printf("Searching for local modem.. ");
                reg16 = qcaspi_read_register16(SPI_REG_SIGNATURE);
                if (reg16 == QCASPI_GOOD_SIGNATURE) {
                    Serial.printf("QCA700X modem found\n");
                    modem_state = MODEM_WRITESPACE;
                }    
                break;

            case MODEM_WRITESPACE:
                reg16 = qcaspi_read_register16(SPI_REG_WRBUF_SPC_AVA);
                if (reg16 == QCA7K_BUFFER_SIZE) {
                    Serial.printf("QCA700X write space ok\n"); 
                    modem_state = MODEM_CM_SET_KEY_REQ;
                }  
                break;

            case MODEM_CM_SET_KEY_REQ:
                randomizeNmk();       // randomize Nmk, so we start with a new key.
                composeSetKey();      // set up buffer with CM_SET_KEY.REQ request data
                qcaspi_write_burst(txbuffer, 60);    // write minimal 60 bytes according to an4_rev5.pdf
                Serial.printf("transmitting SET_KEY.REQ, to configure the EVSE modem with random NMK\n"); 
                modem_state = MODEM_CM_SET_KEY_CNF;
                break;

            case MODEM_GET_SW_REQ:
                composeGetSwReq();
                qcaspi_write_burst(txbuffer, 60); // Send data to modem
                Serial.printf("Modem Search..\n");
                ModemsFound = 0; 
                ModemSearchTimer = millis();        // start timer
                modem_state = MODEM_WAIT_SW;
                break;

            default:
                // poll modem for data
                reg16 = qcaspi_read_burst(rxbuffer);

                while (reg16) {
                    // we received data, read the length of the first packet.
                    rxbytes = rxbuffer[8] + (rxbuffer[9] << 8);
                    
                    // check if the header exists and a minimum of 60 bytes are available
                    if (rxbuffer[4] == 0xaa && rxbuffer[5] == 0xaa && rxbuffer[6] == 0xaa && rxbuffer[7] == 0xaa && rxbytes >= 60) {
                        invalidFrameCounter = 0;
                        // now remove the header, and footer.
                        memcpy(rxbuffer, rxbuffer+12, reg16-14);
                        //Serial.printf("available: %u rxbuffer bytes: %u\n",reg16, rxbytes);
                    
                        FrameType = getFrameType();
                        if (FrameType == FRAME_HOMEPLUG) SlacManager(rxbytes);
                        else if (FrameType == FRAME_IPV6) {
                            lwip_bridge_on_frame(rxbuffer, rxbytes);
                            IPv6Manager(rxbytes);
                        }

                        // there might be more data still in the buffer. Check if there is another packet.
                        if ((int16_t)reg16-rxbytes-14 >= 74) {
                            reg16 = reg16-rxbytes-14;
                            // move data forward.
                            memcpy(rxbuffer, rxbuffer+2+rxbytes, reg16);
                        } else reg16 = 0;
                      
                    } else {
                        invalidFrameCounter++;
                        Serial.printf("Invalid data! (%u/%u)\n", invalidFrameCounter, INVALID_FRAME_THRESHOLD);
                        if (invalidFrameCounter >= INVALID_FRAME_THRESHOLD) {
                            Serial.printf("Resetting modem due to repeated invalid frames\n");
                            ModemReset();
                            modem_state = MODEM_POWERUP;
                            invalidFrameCounter = 0;
                        }
                    }  
                }
                break;
        }

        // Did the Sound timer expire or did we receive enough samples?
        if (modem_state == MNBC_SOUND) {
            bool soundsComplete = (negotiatedSoundCount > 0) && (ReceivedProfiles >= negotiatedSoundCount);
            bool timerExpired = (SoundsTimer + currentSoundWindowMs) < millis();
            if (soundsComplete || timerExpired) {
                transmitAttenCharInd(soundsComplete ? "sounds complete" : "timeout");
            }
        }

        if (modem_state == ATTEN_CHAR_IND && (AttenCharResponseTimer + ATTEN_CHAR_RESPONSE_TIMEOUT_MS) < millis()) {
            if (attenCharRetryCounter < ATTEN_CHAR_MAX_RETRIES) {
                transmitAttenCharInd("waiting for RSP");
            } else {
                handleSlacFailure("ATTEN_CHAR.RSP timeout");
            }
        }

        if (modem_state == ATTEN_CHAR_RSP && (SlacMatchTimer + SLAC_MATCH_TIMEOUT_MS) < millis()) {
            handleSlacFailure("SLAC_MATCH timeout");
        }

        if (modem_state == MODEM_WAIT_SW && (ModemSearchTimer + 1000) < millis() ) {
            Serial.printf("MODEM timer expired. ");
            if (ModemsFound >= 2) {
                Serial.printf("Found %u modems. Private network between EVSE and PEV established\n", ModemsFound); 
                
                Serial.printf("PEV MAC: ");
                for(x=0; x<6 ;x++) Serial.printf("%02x", pevMac[x]);
                Serial.printf(" PEV modem MAC: ");
                for(x=0; x<6 ;x++) Serial.printf("%02x", pevModemMac[x]);
                Serial.printf("\n");

                modem_state = MODEM_LINK_READY;
            } else {
                Serial.printf("(re)transmitting MODEM_GET_SW.REQ\n");
                modem_state = MODEM_GET_SW_REQ;
            } 
        }


        tcp_tick();

        // Pause the task for 20ms
        vTaskDelay(20 / portTICK_PERIOD_MS);

    } // while(1)
}    


static String g_cli_buffer;

static void split_tokens(const String &line, std::vector<String> &tokens) {
    int idx = 0;
    while (idx < line.length()) {
        while (idx < line.length() && isspace((int)line[idx])) idx++;
        if (idx >= line.length()) break;
        int start = idx;
        while (idx < line.length() && !isspace((int)line[idx])) idx++;
        tokens.push_back(line.substring(start, idx));
    }
}

static bool cli_decode_base64(const String &src, std::string &out) {
    size_t in_len = src.length();
    if (in_len == 0) {
        out.clear();
        return true;
    }
    size_t estimate = ((in_len * 3) / 4) + 4;
    std::vector<unsigned char> buffer(estimate);
    size_t actual = 0;
    int ret = mbedtls_base64_decode(buffer.data(), buffer.size(), &actual,
                                    reinterpret_cast<const unsigned char *>(src.c_str()), in_len);
    if (ret != 0) return false;
    out.assign(reinterpret_cast<const char *>(buffer.data()), actual);
    return true;
}

static bool cli_encode_base64(const std::string &src, std::string &out) {
    size_t estimate = ((src.size() + 2) / 3) * 4 + 4;
    std::vector<unsigned char> buffer(estimate);
    size_t actual = 0;
    int ret = mbedtls_base64_encode(buffer.data(), buffer.size(), &actual,
                                    reinterpret_cast<const unsigned char *>(src.data()), src.size());
    if (ret != 0) return false;
    out.assign(reinterpret_cast<const char *>(buffer.data()), actual);
    return true;
}

static bool cli_process_json(const String &line) {
    StaticJsonDocument<768> doc;
    DeserializationError err = deserializeJson(doc, line);
    if (err) {
        return false;
    }
    const char *type = doc["type"] | "";
    if (!strcmp(type, "diag")) {
        const char *op = doc["op"] | "";
        StaticJsonDocument<256> res;
        res["type"] = "diag.res";
        res["op"] = op;
        auto emit = [&]() {
            serializeJson(res, Serial);
            Serial.print('\n');
        };
        if (!strcmp(op, "auth")) {
            const char *token = doc["token"] | "";
            if (diag_auth_attempt(token, millis())) {
                res["ok"] = true;
                emit();
            } else {
                res["ok"] = false;
                res["error"] = "auth_failed";
                emit();
            }
            return true;
        }
        res["ok"] = false;
        res["error"] = "unknown_op";
        emit();
        return true;
    }
    if (strcmp(type, "pki") != 0) {
        return false;
    }
    if (diag_auth_required()) {
        const char *token = doc["auth_token"] | "";
        if (!diag_auth_attempt(token, millis())) {
            StaticJsonDocument<256> res;
            res["type"] = "pki.res";
            res["ok"] = false;
            res["error"] = "auth_required";
            serializeJson(res, Serial);
            Serial.print('\n');
            return true;
        }
    }
    const char *op = doc["op"] | "";
    const char *target = doc["target"] | "";
    StaticJsonDocument<768> res;
    res["type"] = "pki.res";
    res["op"] = op;
    res["target"] = target;
    auto emit = [&]() {
        serializeJson(res, Serial);
        Serial.print('\n');
    };
    if (!op[0] || !target[0]) {
        res["ok"] = false;
        res["error"] = "invalid_params";
        emit();
        return true;
    }
    String targetStr(target);
    if (!strcmp(op, "set")) {
        const char *payload = doc["data_b64"] | "";
        if (!payload[0]) {
            res["ok"] = false;
            res["error"] = "missing_data";
            emit();
            return true;
        }
        std::string decoded;
        if (!cli_decode_base64(String(payload), decoded)) {
            res["ok"] = false;
            res["error"] = "bad_base64";
            emit();
            return true;
        }
        bool ok = false;
        if (targetStr.equalsIgnoreCase("cert")) ok = pki_store_set_server_cert(decoded);
        else if (targetStr.equalsIgnoreCase("key")) ok = pki_store_set_server_key(decoded);
        else if (targetStr.equalsIgnoreCase("ca")) ok = pki_store_set_root_ca(decoded);
        else {
            res["ok"] = false;
            res["error"] = "unknown_target";
            emit();
            return true;
        }
        if (ok) {
            tls_credentials_reload();
            res["ok"] = true;
            res["bytes"] = static_cast<uint32_t>(decoded.size());
        } else {
            res["ok"] = false;
            res["error"] = "store_failed";
        }
        emit();
        return true;
    }
    if (!strcmp(op, "get")) {
        std::string value;
        bool ok = false;
        if (targetStr.equalsIgnoreCase("cert")) ok = pki_store_get_server_cert(value);
        else if (targetStr.equalsIgnoreCase("key")) ok = pki_store_get_server_key(value);
        else if (targetStr.equalsIgnoreCase("ca")) ok = pki_store_get_root_ca(value);
        else {
            res["ok"] = false;
            res["error"] = "unknown_target";
            emit();
            return true;
        }
        if (!ok) {
            res["ok"] = false;
            res["error"] = "no_data";
            emit();
            return true;
        }
        std::string encoded;
        if (!cli_encode_base64(value, encoded)) {
            res["ok"] = false;
            res["error"] = "encode_failed";
            emit();
            return true;
        }
        res["ok"] = true;
        res["data_b64"] = encoded.c_str();
        emit();
        return true;
    }
    res["ok"] = false;
    res["error"] = "unknown_op";
    emit();
    return true;
}

static void cli_process_line(const String &line) {
    std::vector<String> tokens;
    split_tokens(line, tokens);
    if (tokens.empty()) return;
    if (line.startsWith("{")) {
        if (cli_process_json(line)) return;
    }
    if (tokens[0].equalsIgnoreCase("diag")) {
        if (tokens.size() >= 3 && tokens[1].equalsIgnoreCase("auth")) {
        if (diag_auth_attempt(tokens[2].c_str(), millis())) {
            Serial.println("[DIAG] Auth accepted");
        } else {
            diag_auth_revoke();
            Serial.println("[DIAG] Auth failed");
            }
        } else {
            Serial.println("[DIAG] Usage: diag auth <token>");
        }
        return;
    }
    if (!tokens[0].equalsIgnoreCase("pki")) {
        Serial.println("[CLI] Unknown command");
        return;
    }
    if (diag_auth_required() && !diag_auth_is_valid(millis())) {
        Serial.println("[PKI] Authorization required (diag auth <token>)");
        return;
    }
    if (tokens.size() >= 4 && tokens[1].equalsIgnoreCase("set")) {
        String target = tokens[2];
        String payload = tokens[3];
        std::string decoded;
        if (!cli_decode_base64(payload, decoded)) {
            Serial.println("[PKI] Base64 decode failed");
            return;
        }
        bool ok = false;
        if (target.equalsIgnoreCase("cert")) {
            ok = pki_store_set_server_cert(decoded);
        } else if (target.equalsIgnoreCase("key")) {
            ok = pki_store_set_server_key(decoded);
        } else if (target.equalsIgnoreCase("ca")) {
            ok = pki_store_set_root_ca(decoded);
        } else {
            Serial.println("[PKI] Unknown target (use cert|key|ca)");
            return;
        }
        if (ok) {
            tls_credentials_reload();
            Serial.printf("[PKI] Stored %s (%u bytes). Restart TLS sessions to apply.\n",
                          target.c_str(), (unsigned)decoded.size());
        } else {
            Serial.println("[PKI] Failed to store data");
        }
        return;
    }
    if (tokens.size() >= 3 && tokens[1].equalsIgnoreCase("get")) {
        String target = tokens[2];
        std::string value;
        bool ok = false;
        if (target.equalsIgnoreCase("cert")) ok = pki_store_get_server_cert(value);
        else if (target.equalsIgnoreCase("key")) ok = pki_store_get_server_key(value);
        else if (target.equalsIgnoreCase("ca")) ok = pki_store_get_root_ca(value);
        else {
            Serial.println("[PKI] Unknown target (use cert|key|ca)");
            return;
        }
        if (!ok) {
            Serial.println("[PKI] No data stored for target");
            return;
        }
        std::string encoded;
        if (!cli_encode_base64(value, encoded)) {
            Serial.println("[PKI] Base64 encode failed");
            return;
        }
        Serial.printf("[PKI] %s=%s\n", target.c_str(), encoded.c_str());
        return;
    }
    Serial.println("[PKI] Usage: pki set <cert|key|ca> <base64>, pki get <cert|key|ca>");
}

static void cli_poll(void) {
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n') {
            String line = g_cli_buffer;
            g_cli_buffer = "";
            line.trim();
            if (line.length()) {
                cli_process_line(line);
            }
        } else if (c != '\r') {
            if (g_cli_buffer.length() < 1024) {
                g_cli_buffer += c;
            }
        }
    }
}

#ifndef APP_NO_MAIN
void setup() {

    pinMode(PIN_QCA700X_CS, OUTPUT);           // SPI_CS QCA7005 
    pinMode(PIN_QCA700X_INT, INPUT);           // SPI_INT QCA7005 
    pinMode(SPI_SCK, OUTPUT);     
    pinMode(SPI_MISO, INPUT);     
    pinMode(SPI_MOSI, OUTPUT);     

    digitalWrite(PIN_QCA700X_CS, HIGH); 

    // configure SPI connection to QCA modem
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, PIN_QCA700X_CS);
    // SPI mode is MODE3 (Idle = HIGH, clock in on rising edge), we use a 10Mhz SPI clock
    SPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE3));
    //attachInterrupt(digitalPinToInterrupt(PIN_QCA700X_INT), SPI_InterruptHandler, RISING);

    Serial.begin();
    Serial.printf("\npowerup\n");
    diag_auth_init(DIAG_AUTH_TOKEN, DIAG_AUTH_WINDOW_MS);
    iso_watchdog_configure(ISO_STATE_TIMEOUT_MS, ISO_STATE_WATCHDOG_MAX_RETRIES);

    // Create Task 20ms Timer
    xTaskCreate(
        Timer20ms,      // Function that should be called
        "Timer20ms",    // Name of the task (for debugging)
        3072,           // Stack size (bytes)                              
        NULL,           // Parameter to pass
        1,              // Task priority
        NULL            // Task handle
    );

    
    esp_read_mac(myMac, ESP_MAC_ETH); // select the Ethernet MAC     
    setSeccIp();  // use myMac to create link-local IPv6 address.

    cp_init();
    dc_can_init();
    lwip_bridge_init();
    if (!pki_store_init()) {
        Serial.println("[PKI] Failed to initialize PKI store, using embedded credentials");
    }
#ifdef ESP_PLATFORM
    sdp_server_start();
    tcp_socket_server_start();
    tls_server_start();
#endif
    iso20_init();

    modem_state = MODEM_POWERUP;
   
}

void loop() {

  // Serial.printf("Total heap: %u\n", ESP.getHeapSize());
  //  Serial.printf("Free heap: %u\n", ESP.getFreeHeap());
  //  Serial.printf("Flash Size: %u\n", ESP.getFlashChipSize());
  //  Serial.printf("Total PSRAM: %u\n", ESP.getPsramSize());
  //  Serial.printf("Free PSRAM: %u\n", ESP.getFreePsram());

    cli_poll();
    iso20_loop();
    delay(1000);
}

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
#endif // UNIT_TEST
