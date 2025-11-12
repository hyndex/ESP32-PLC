#include <Arduino.h>
#include <string.h>
#include "main.h"
#include "tcp.h"
#include "evse_config.h"
#include "tls_server.h"

const uint8_t broadcastIPv6[16] = { 0xff, 0x02, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
/* our link-local IPv6 address. Based on myMac, but with 0xFFFE in the middle, and bit 1 of MSB inverted */
uint8_t SeccIp[16]; 
uint8_t EvccIp[16];
uint16_t evccTcpPort; /* the TCP port number of the car */
uint8_t sourceIp[16];
uint16_t evccPort;
uint16_t seccPort;
uint16_t sourceport;
uint16_t destinationport;
uint16_t udplen;
uint16_t udpsum;
uint8_t NeighborsMac[6];
uint8_t NeighborsIp[16];
uint8_t DiscoveryReqSecurity;
uint8_t DiscoveryReqTransportProtocol;

static constexpr uint8_t kSdpSecurityTls = 0x00;
static constexpr uint8_t kSdpSecurityNoTls = 0x10;
static constexpr uint8_t kSdpTransportTcp = 0x00;

static uint8_t g_activeSdpSecurity = kSdpSecurityNoTls;
static uint8_t g_activeSdpTransport = kSdpTransportTcp;
static uint16_t g_activeSeccPort = TCP_PLAIN_PORT;



#define NEXT_UDP 0x11 /* next protocol is UDP */
#define NEXT_TCP 0x06  /* next protocol is TCP */
#define NEXT_ICMPv6 0x3a /* next protocol is ICMPv6 */

#define UDP_PAYLOAD_LEN 100
uint8_t udpPayload[UDP_PAYLOAD_LEN];
uint16_t udpPayloadLen;

#define V2G_FRAME_LEN 100
uint8_t v2gFrameLen;
uint8_t V2GFrame[V2G_FRAME_LEN];

#define UDP_RESPONSE_LEN 100
uint8_t UdpResponseLen;
uint8_t UdpResponse[UDP_RESPONSE_LEN];

#define IP_RESPONSE_LEN 100
uint8_t IpResponseLen;
uint8_t IpResponse[IP_RESPONSE_LEN];

#define PSEUDO_HEADER_LEN 40
uint8_t pseudoHeader[PSEUDO_HEADER_LEN];

static const uint16_t IPV6_HEADER_OFFSET = 14;
static const uint16_t IPV6_FIXED_HEADER_LEN = 40;
static const uint16_t IPV6_PAYLOAD_OFFSET = IPV6_HEADER_OFFSET + IPV6_FIXED_HEADER_LEN;

#ifndef PLC_TRACE_IPV6
#define PLC_TRACE_IPV6 0
#endif

static bool isIpv6ExtensionHeader(uint8_t nextHeader) {
    switch (nextHeader) {
        case 0:   // Hop-by-hop
        case 43:  // Routing
        case 44:  // Fragment
        case 50:  // ESP
        case 51:  // AH
        case 60:  // Destination options
        case 135: // Mobility
            return true;
        default:
            return false;
    }
}

static bool computeIpv6PayloadMetadata(uint16_t rxbytes, uint8_t *nextHeader, uint16_t *payloadOffset, uint16_t *payloadLength) {
    if (rxbytes < IPV6_PAYLOAD_OFFSET) {
        return false;
    }
    uint16_t ipv6PayloadLen = (rxbuffer[IPV6_HEADER_OFFSET + 4] << 8) | rxbuffer[IPV6_HEADER_OFFSET + 5];
    if (rxbytes < IPV6_PAYLOAD_OFFSET + ipv6PayloadLen) {
        return false;
    }

    uint16_t offset = IPV6_PAYLOAD_OFFSET;
    uint16_t consumed = 0;
    uint8_t currentHeader = rxbuffer[IPV6_HEADER_OFFSET + 6];

    while (isIpv6ExtensionHeader(currentHeader)) {
        if (offset + 2 > IPV6_PAYLOAD_OFFSET + ipv6PayloadLen) {
            return false;
        }
        uint8_t next = rxbuffer[offset];
        uint8_t hdrLenUnits = rxbuffer[offset + 1];
        uint16_t headerBytes = (uint16_t)(hdrLenUnits + 1) * 8;
        if (offset + headerBytes > IPV6_PAYLOAD_OFFSET + ipv6PayloadLen) {
            return false;
        }
        offset += headerBytes;
        consumed += headerBytes;
        currentHeader = next;
    }

    if (consumed > ipv6PayloadLen) {
        return false;
    }
    *payloadOffset = offset;
    *payloadLength = ipv6PayloadLen - consumed;
    *nextHeader = currentHeader;
    return true;
}

void setSeccIp() {
    // Create a link-local Ipv6 address based on myMac (the MAC of the ESP32).
    memset(SeccIp, 0, 16);
    SeccIp[0] = 0xfe;             // Link-local address
    SeccIp[1] = 0x80;
    // byte 2-7 are zero;               
    SeccIp[8] = myMac[0] ^ 2;     // invert bit 1 of MSB
    SeccIp[9] = myMac[1];
    SeccIp[10] = myMac[2];
    SeccIp[11] = 0xff;
    SeccIp[12] = 0xfe;
    SeccIp[13] = myMac[3];
    SeccIp[14] = myMac[4];
    SeccIp[15] = myMac[5];
}


static uint32_t foldChecksum(uint32_t sum) {
    while (sum > 0xFFFF) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return sum;
}

uint16_t calculateUdpAndTcpChecksumForIPv6(uint8_t *UdpOrTcpframe, uint16_t UdpOrTcpframeLen, const uint8_t *ipv6source, const uint8_t *ipv6dest, uint8_t nxt) {
    uint32_t sum = 0;
    uint16_t i;

    for (i = 0; i < 16; i += 2) {
        sum += (ipv6source[i] << 8) | ipv6source[i + 1];
        sum += (ipv6dest[i] << 8) | ipv6dest[i + 1];
        sum = foldChecksum(sum);
    }

    sum += (UdpOrTcpframeLen >> 16) & 0xFFFF;
    sum = foldChecksum(sum);
    sum += (UdpOrTcpframeLen & 0xFFFF);
    sum = foldChecksum(sum);

    sum += nxt; // three padding bytes are zero
    sum = foldChecksum(sum);

    for (i = 0; i + 1 < UdpOrTcpframeLen; i += 2) {
        sum += (UdpOrTcpframe[i] << 8) | UdpOrTcpframe[i + 1];
        sum = foldChecksum(sum);
    }
    if (UdpOrTcpframeLen & 1) {
        sum += (UdpOrTcpframe[UdpOrTcpframeLen - 1] << 8);
        sum = foldChecksum(sum);
    }

    sum = foldChecksum(sum);
    return (uint16_t)(~sum);
}

void packResponseIntoEthernet() {
    // packs the IP packet into an ethernet packet
    uint8_t i;
    uint16_t EthTxFrameLen;

    EthTxFrameLen = IpResponseLen + 6 + 6 + 2;  // Ethernet header needs 14 bytes:
                                                //  6 bytes destination MAC
                                                //  6 bytes source MAC
                                                //  2 bytes EtherType
    for (i=0; i<6; i++) {       // fill the destination MAC with the source MAC of the received package
        txbuffer[i] = rxbuffer[6+i];
    }    
    setMacAt(myMac,6); // bytes 6 to 11 are the source MAC
    txbuffer[12] = 0x86; // 86dd is IPv6
    txbuffer[13] = 0xdd;
    for (i=0; i<IpResponseLen; i++) {
        txbuffer[14+i] = IpResponse[i];
    }

    qcaspi_write_burst(txbuffer, EthTxFrameLen);    
}

void packResponseIntoIp(void) {
  // # embeds the (SDP) response into the lower-layer-protocol: IP, Ethernet
  uint8_t i;
  uint16_t plen;
  IpResponseLen = UdpResponseLen + 8 + 16 + 16; // # IP6 header needs 40 bytes:
                                              //  #   4 bytes traffic class, flow
                                              //  #   2 bytes destination port
                                              //  #   2 bytes length (incl checksum)
                                              //  #   2 bytes checksum
  if (IpResponseLen > IP_RESPONSE_LEN) {
      Serial.printf("Error: IPv6 response too large (%u bytes)\n", IpResponseLen);
      return;
  }
  IpResponse[0] = 0x60; // # traffic class, flow
  IpResponse[1] = 0; 
  IpResponse[2] = 0;
  IpResponse[3] = 0;
  plen = UdpResponseLen; // length of the payload. Without headers.
  IpResponse[4] = plen >> 8;
  IpResponse[5] = plen & 0xFF;
  IpResponse[6] = 0x11; // next level protocol, 0x11 = UDP in this case
  IpResponse[7] = 0xFF; // hop limit must be 255 on link-local control traffic
    for (i=0; i<16; i++) {
    IpResponse[8+i] = SeccIp[i]; // source IP address
    IpResponse[24+i] = EvccIp[i]; // destination IP address
  }
  for (i=0; i<UdpResponseLen; i++) {
    IpResponse[40+i] = UdpResponse[i];
  }            
  packResponseIntoEthernet();
}


void packResponseIntoUdp(void) {
    //# embeds the (SDP) request into the lower-layer-protocol: UDP
    //# Reference: wireshark trace of the ioniq car
    uint8_t i;
    uint16_t lenInclChecksum;
    uint16_t checksum;
    UdpResponseLen = v2gFrameLen + 8; // # UDP header needs 8 bytes:
                                        //           #   2 bytes source port
                                        //           #   2 bytes destination port
                                        //           #   2 bytes length (incl checksum)
                                        //           #   2 bytes checksum
    if (UdpResponseLen > UDP_RESPONSE_LEN) {
        Serial.printf("Error: UDP response too large (%u bytes)\n", UdpResponseLen);
        return;
    }
    UdpResponse[0] = 15118 >> 8;
    UdpResponse[1] = 15118  & 0xFF;
    UdpResponse[2] = evccPort >> 8;
    UdpResponse[3] = evccPort & 0xFF;
    
    lenInclChecksum = UdpResponseLen;
    UdpResponse[4] = lenInclChecksum >> 8;
    UdpResponse[5] = lenInclChecksum & 0xFF;
    // checksum will be calculated afterwards
    UdpResponse[6] = 0;
    UdpResponse[7] = 0;
    memcpy(UdpResponse+8, V2GFrame, v2gFrameLen);
    // The content of buffer is ready. We can calculate the checksum. see https://en.wikipedia.org/wiki/User_Datagram_Protocol
    checksum =calculateUdpAndTcpChecksumForIPv6(UdpResponse, UdpResponseLen, SeccIp, EvccIp, NEXT_UDP); 
    UdpResponse[6] = checksum >> 8;
    UdpResponse[7] = checksum & 0xFF;
    packResponseIntoIp();
}


uint16_t buildSdpResponseFrame(uint8_t *out, uint16_t maxLen) {
    const uint16_t payloadLen = 20;
    const uint16_t totalLen = payloadLen + V2GTP_HEADER_SIZE;
    if (maxLen < totalLen) {
        Serial.printf("Error: SDP payload does not fit in buffer (%u)\n", maxLen);
        return 0;
    }

    uint8_t *payload = out + V2GTP_HEADER_SIZE;
    memcpy(payload, SeccIp, 16);
    payload[16] = g_activeSeccPort >> 8;
    payload[17] = g_activeSeccPort & 0xff;
    payload[18] = g_activeSdpSecurity;
    payload[19] = g_activeSdpTransport;

    out[0] = 0x01;
    out[1] = 0xfe;
    out[2] = 0x90;
    out[3] = 0x01;
    out[4] = (payloadLen >> 24) & 0xff;
    out[5] = (payloadLen >> 16) & 0xff;
    out[6] = (payloadLen >> 8) & 0xff;
    out[7] = payloadLen & 0xff;
    return totalLen;
}

bool handleSdpRequestBuffer(const uint8_t *payload, uint16_t len, const uint8_t *srcIp, uint16_t srcPort) {
    if (len != 2) return false;
    DiscoveryReqSecurity = payload[0];
    DiscoveryReqTransportProtocol = payload[1];

    if (DiscoveryReqTransportProtocol != kSdpTransportTcp) {
        Serial.printf("DiscoveryReqTransportProtocol %u is not supported\n", DiscoveryReqTransportProtocol);
        return false;
    }

    const bool tlsRequested = (DiscoveryReqSecurity == kSdpSecurityTls);
    const bool plainRequested = (DiscoveryReqSecurity == kSdpSecurityNoTls);
    if (!tlsRequested && !plainRequested) {
        Serial.printf("DiscoveryReqSecurity %u is not supported\n", DiscoveryReqSecurity);
        return false;
    }

    if (tlsRequested && !tls_server_ready()) {
        Serial.println("SDP request asked for TLS but TLS server is not ready yet");
        return false;
    }

    g_activeSdpSecurity = tlsRequested ? kSdpSecurityTls : kSdpSecurityNoTls;
    g_activeSdpTransport = kSdpTransportTcp;
    g_activeSeccPort = tlsRequested ? TCP_TLS_PORT : TCP_PLAIN_PORT;
    seccPort = g_activeSeccPort;

    Serial.printf("Ok, SDP request accepted. Selected %s endpoint on port %u\n",
                  tlsRequested ? "TLS" : "TCP", g_activeSeccPort);
    memcpy(EvccIp, srcIp, 16);
    evccPort = srcPort;
    return true;
}

void sendSdpResponse() {
    uint16_t len = buildSdpResponseFrame(V2GFrame, sizeof(V2GFrame));
    if (!len) return;
    v2gFrameLen = len;
    packResponseIntoUdp();
}


void evaluateUdpPayload(uint16_t payloadOffset, uint16_t payloadLen) {
    uint16_t v2gptPayloadType;
    uint32_t v2gptPayloadLen;

    if (payloadLen < 8) {
        Serial.printf("Ignoring UDP frame: payload too short (%u)\n", payloadLen);
        return;
    }

    sourceport = (rxbuffer[payloadOffset] << 8) | rxbuffer[payloadOffset + 1];
    destinationport = (rxbuffer[payloadOffset + 2] << 8) | rxbuffer[payloadOffset + 3];
    udplen = (rxbuffer[payloadOffset + 4] << 8) | rxbuffer[payloadOffset + 5];
    udpsum = (rxbuffer[payloadOffset + 6] << 8) | rxbuffer[payloadOffset + 7];

    if (udplen > payloadLen) {
        Serial.printf("Ignoring UDP frame: length mismatch (%u > %u)\n", udplen, payloadLen);
        return;
    }

    if (udplen < 8) {
        Serial.printf("Ignoring UDP frame: invalid header length\n");
        return;
    }

    udpPayloadLen = udplen - 8;
    if (udpPayloadLen > UDP_PAYLOAD_LEN) {
        Serial.printf("Ignoring UDP payload: %u exceeds buffer\n", udpPayloadLen);
        return;
    }

    memcpy(udpPayload, rxbuffer + payloadOffset + 8, udpPayloadLen);

    if (destinationport == 15118) { // port for the SECC
        if ((udpPayloadLen >= 2) && (udpPayload[0] == 0x01) && (udpPayload[1] == 0xFE)) { //# protocol version 1 and inverted
            v2gptPayloadType = udpPayload[2]*256 + udpPayload[3];
            v2gptPayloadLen = (((uint32_t)udpPayload[4])<<24)  + 
                              (((uint32_t)udpPayload[5])<<16) +
                              (((uint32_t)udpPayload[6])<<8) +
                              udpPayload[7];
            if (v2gptPayloadType == 0x9000) {
                Serial.printf("it is a SDP request from the car to the charger\n");
                if (handleSdpRequestBuffer(udpPayload+8, v2gptPayloadLen, sourceIp, sourceport)) {
                    sendSdpResponse();
                } else {
                    Serial.printf("SDP request ignored\n");
                }
            } else {    
                Serial.printf("v2gptPayloadType %04x not supported\n", v2gptPayloadType);
            }                  
        }
    }
}

void evaluateNeighborSolicitation(void) {
    uint16_t checksum;
    uint8_t i;
    /* The neighbor discovery protocol is used by the charger to find out the
        relation between MAC and IP. */

    /* We could extract the necessary information from the NeighborSolicitation,
        means the chargers IP and MAC address. But this is not fully necessary:
        - The chargers MAC was already discovered in the SLAC. So we do not need to extract
        it here again. But if we have not done the SLAC, because the modems are already paired,
        then it makes sense to extract the chargers MAC from the Neighbor Solicitation message.
        - For the chargers IPv6, there are two possible cases:
            (A) The charger made the SDP without NeighborDiscovery. This works, if
                we use the pyPlc.py as charger. It does not care for NeighborDiscovery,
                because the SDP is implemented independent of the address resolution of 
                the operating system.
                In this case, we know the chargers IP already from the SDP.
            (B) The charger insists of doing NeighborSolitcitation in the middle of
                SDP. This behavior was observed on Alpitronics. Means, we have the
                following sequence:
                1. car sends SDP request
                2. charger sends NeighborSolicitation
                3. car sends NeighborAdvertisement
                4. charger sends SDP response
                In this case, we need to extract the chargers IP from the NeighborSolicitation,
                otherwise we have to chance to send the correct NeighborAdvertisement. 
                We can do this always, because this does not hurt for case A, address
                is (hopefully) not changing. */
    /* More general approach: In the network there may be more participants than only the charger,
        e.g. a notebook for sniffing. Eeach of it may send a NeighborSolicitation, and we should NOT use the addresses from the
        NeighborSolicitation as addresses of the charger. The chargers address is only determined
        by the SDP. */
        
    /* save the requesters IP. The requesters IP is the source IP on IPv6 level, at byte 22. */
    memcpy(NeighborsIp, rxbuffer+22, 16);
    /* save the requesters MAC. The requesters MAC is the source MAC on Eth level, at byte 6. */
    memcpy(NeighborsMac, rxbuffer+6, 6);
    
    /* send a NeighborAdvertisement as response. */
    // destination MAC = neighbors MAC
    setMacAt(NeighborsMac, 0); // bytes 0 to 5 are the destination MAC	
    // source MAC = my MAC
    setMacAt(myMac, 6); // bytes 6 to 11 are the source MAC
    // Ethertype 86DD
    txbuffer[12] = 0x86; // # 86dd is IPv6
    txbuffer[13] = 0xdd;
    txbuffer[14] = 0x60; // # traffic class, flow
    txbuffer[15] = 0; 
    txbuffer[16] = 0;
    txbuffer[17] = 0;
    // plen
    #define ICMP_LEN 32 /* bytes in the ICMPv6 */
    txbuffer[18] = 0;
    txbuffer[19] = ICMP_LEN;
    txbuffer[20] = NEXT_ICMPv6;
    txbuffer[21] = 0xff;
    // We are the EVSE. So the SeccIp is our own link-local IP address.
    memcpy(txbuffer+22, SeccIp, 16); // source IP address
    memcpy(txbuffer+38, NeighborsIp, 16); // destination IP address
    /* here starts the ICMPv6 */
    txbuffer[54] = 0x88; /* Neighbor Advertisement */
    txbuffer[55] = 0;	
    txbuffer[56] = 0; /* checksum (filled later) */	
    txbuffer[57] = 0;	

    /* Flags */
    txbuffer[58] = 0x60; /* Solicited, override */	
    txbuffer[59] = 0;
    txbuffer[60] = 0;
    txbuffer[61] = 0;

    memcpy(txbuffer+62, SeccIp, 16); /* The own IP address */
    txbuffer[78] = 2; /* Type 2, Link Layer Address */
    txbuffer[79] = 1; /* Length 1, means 8 byte (?) */
    memcpy(txbuffer+80, myMac, 6); /* The own Link Layer (MAC) address */

    checksum = calculateUdpAndTcpChecksumForIPv6(txbuffer+54, ICMP_LEN, SeccIp, NeighborsIp, NEXT_ICMPv6);
    txbuffer[56] = checksum >> 8;
    txbuffer[57] = checksum & 0xFF;
    
    Serial.printf("transmitting Neighbor Advertisement\n");
    /* Length of the NeighborAdvertisement = 86*/
    qcaspi_write_burst(txbuffer, 86);
}


void IPv6Manager(uint16_t rxbytes) {
    uint16_t payloadOffset;
    uint16_t payloadLen;
    uint8_t nextheader; 
    uint8_t icmpv6type; 

#if PLC_TRACE_IPV6
    Serial.printf("\n[RX] ");
    for (uint16_t x=0; x<rxbytes; x++) Serial.printf("%02x",rxbuffer[x]);
    Serial.printf("\n");
#endif

    if (!computeIpv6PayloadMetadata(rxbytes, &nextheader, &payloadOffset, &payloadLen)) {
        Serial.printf("Ignoring malformed IPv6 frame (len=%u)\n", rxbytes);
        return;
    }

    memcpy(sourceIp, rxbuffer+IPV6_HEADER_OFFSET+8, 16);

    if (nextheader == NEXT_UDP) {
        evaluateUdpPayload(payloadOffset, payloadLen);
    } else if (nextheader == 0x06) {
        Serial.printf("TCP received\n");
        evaluateTcpPacket(payloadOffset, payloadLen);
    } else if (nextheader == NEXT_ICMPv6) {
        Serial.printf("ICMPv6 received\n");
        if (payloadLen == 0) return;
        icmpv6type = rxbuffer[payloadOffset];
        if (icmpv6type == 0x87 && payloadOffset == IPV6_PAYLOAD_OFFSET) {
            Serial.printf("Neighbor Solicitation received\n");
            evaluateNeighborSolicitation();
        }
    }
}
