
extern uint16_t evccPort;
extern uint16_t seccPort;
extern uint16_t evccTcpPort;
extern uint8_t SeccIp[]; 
extern uint8_t EvccIp[];

void setSeccIp();
void IPv6Manager(uint16_t rxbytes); 
uint16_t calculateUdpAndTcpChecksumForIPv6(uint8_t *UdpOrTcpframe, uint16_t UdpOrTcpframeLen, const uint8_t *ipv6source, const uint8_t *ipv6dest, uint8_t nxt);
uint16_t buildSdpResponseFrame(uint8_t *out, uint16_t maxLen);
bool handleSdpRequestBuffer(const uint8_t *payload, uint16_t len, const uint8_t *srcIp, uint16_t srcPort);
void sendSdpResponse(void);
