# SLAC Implementation Assessment

## 1) Executive summary

**Implemented correctly (baseline works with some EVs):**

- HomePlug GreenPHY link and SPI framing to QCA700x, including burst read/write and basic modem reset.
- SLAC basic flow: responds to CM_SLAC_PARAM.REQ, runs MNBC sounds, collects attenuation profile, sends ATTEN_CHAR.IND, verifies ATTEN_CHAR.RSP, answers SLAC_MATCH.REQ, then probes with GET_SW to confirm both modems are on the private network.
- IPv6 essentials: manual parsing of Ethernet → IPv6 → UDP/TCP/ICMPv6; Neighbor Solicitation → Neighbor Advertisement reply; SDP (SECC Discovery Protocol) over UDP 15118; computing outgoing UDP/TCP checksums via IPv6 pseudo-header.
- TCP minimal server on port 15118 (SYN→SYN+ACK→ACK), data receive, and ACK handling for inbound data; builds own IPv6+TCP packets and checksums.
- HLC (DIN 70121) handshake partially implemented: Supports the SupportedApplicationProtocol → SessionSetup → ServiceDiscovery → ServicePaymentSelection → ContractAuthentication → ChargeParameterDiscovery path (responses now built via libcbv2g). It answers only when the EV offers DIN 70121 in SupportedApplicationProtocol.

**Missing or incomplete (blocks production readiness):**

- SLAC: Sparse timeouts/retries; several values are hardcoded; length/field checks are partial; small off-by-one risks in transmit lengths; no recovery paths on timeouts.
- IPv6: Rx checksum/length validation not performed; extension headers unsupported; Hop Limit values are inconsistent (e.g., 0x0A / 10 on UDP) and can break strict EV stacks; ND/DAD not fully covered.
- TCP: No retransmission, no timers, no FIN/RST handling, no MSS/SACK options, no congestion/flow control beyond a fixed window. Works with tolerant EVs, fails with strict stacks.
- HLC: DIN 70121 ends after ChargeParameterDiscovery; CableCheck, PreCharge, PowerDelivery, and stop/terminate sequences are not implemented. ISO 15118-2/-20 not implemented (no TLS, no certs, no SECC status model).

## 2) End-to-end flow (what the code does today)

**A) Power-up & modem bring-up**

SPI to QCA700x is initialized; signature & write-space checks gate the next step; modem reset helper exists. States: MODEM_POWERUP → MODEM_WRITESPACE → MODEM_CM_SET_KEY_REQ.

CM_SET_KEY.REQ sets a random NMK each session and a fixed NID (upper two bits zero). On SET_KEY.CNF it marks the modem configured.

**B) SLAC pairing**

On CM_SLAC_PARAM.REQ it stores PEV MAC and RunID, then sends SLAC_PARAM.CNF (10 sounds, timeout 6, respType 1).

On CM_START_ATTEN_CHAR.IND it starts the sounds timer and collects MNBC_SOUND count; on CM_ATTEN_PROFILE.IND it accumulates 58-group averages (once 10 sounds are received).

When the 600 ms sound timer expires, it sends ATTEN_CHAR.IND with the averages and switches state to await ATTEN_CHAR.RSP.

On good ATTEN_CHAR.RSP it flags successful SLAC and expects SLAC_MATCH.REQ, then sends SLAC_MATCH.CNF (echoing RunID, NID, NMK) and moves to GET_SW probing to confirm the private network. If two GET_SW.CNF are seen within ~1 s, the link is considered ready.

**C) IPv6/ND + SDP**

Link-local IPv6 is derived from the ESP32 Ethernet MAC using EUI-64 with U/L bit inversion (fe80::...ff:fe...).

Responds to Neighbor Solicitation with Neighbor Advertisement (Solicited + Override = 0x60). It uses Hop Limit 255 here (good).

On UDP 15118, it parses V2GTP. If it is SDP request 0x9000 with len=2, it checks security==0x10 (NoTLS) and transport==0x00 (TCP) and sends an SDP response with your link-local address, port 15118, security 0x10, transport 0x00. EVCC IP/port is latched for later TCP.

**D) TCP 15118 server**

Minimal server: accepts SYN, replies SYN+ACK, then expects ACK and enters ESTABLISHED; no options, rcv_wnd=1000. For inbound data: calculates payload length, copies it to tcp_rxdata, ACKs immediately, then hands it to the V2GTP/EXI decoder. Outbound data: wraps EXI in V2GTP, sets PSH+ACK, computes checksum, and transmits.

**E) HLC (DIN 70121: partial)**

SupportedApplicationProtocolRequest: scans EV’s advertised namespaces; only reacts to DIN (":din:70121:"). Sends SAP Response selecting the EV’s SchemaID. (No ISO15118 path.)

SessionSetupReq → Res (assigns SessionID), ServiceDiscoveryReq → Res (Payment=External, EnergyTransferType=DC_extended, i.e., CCS2 DC), ServicePaymentSelectionReq → Res, ContractAuthenticationReq → Res (Finished), ChargeParameterDiscoveryReq → Res (reads EVRESSSOC). Stops here and sets state to wait for CableCheck, but CableCheck/PreCharge/PowerDelivery are not implemented yet.

## 3) Robustness gaps, edge cases, and error handling

**Critical correctness & safety**

Checksum helper writes past buffer on odd lengths.
calculateUdpAndTcpChecksumForIPv6() increments the length and writes a padding byte into the caller’s buffer when the length is odd:
evenFrameLen++; UdpOrTcpframe[evenFrameLen-1] = 0;
That is an out-of-bounds write for any odd-length buffer sized tightly to UdpOrTcpframeLen. You must not mutate caller memory here; pad virtually in the summation.

TCP lacks retransmission, timers, and FIN/RST handling.
Any dropped segment or delayed ACK stalls the session indefinitely. No keepalive, no idle timeout, no receive reassembly beyond single-packet payloads. Strict EV TCP stacks (or packet loss on PLC) will break.

UDP/TCP rx validation is minimal.
No IP header checksum (IPv6 has none, but upper-layer checks not validated), no bounds checks against rxbytes beyond a few spots, ignores IPv6 extension headers. A strict EV may send extension headers (e.g., Hop-by-Hop), which will shift offsets.

**SLAC edge cases**

Timing tolerance: you send SLAC_PARAM.CNF with sound count 10 and a 600ms “sound window”. Some EVs send >10 sounds or are slower; you continue averaging but trigger ATTEN_CHAR.IND exactly at 600ms, possibly before the EV is ready, or after it has transitioned. This can desynchronize strict stacks. Add adaptive timing or accept up to the EV’s soundCount from SLAC_PARAM.REQ.

Length confusion: In composeSlacMatchCnf() you set length bytes at offsets 21–22 to 0x56,0x00, while in SlacManager() you later check an incoming SLAC_MATCH.REQ for rxbuffer[21] == 0x3e. That asymmetry suggests a misinterpreted field or endianness problem and will fail with strict EVs. Align with the spec’s MVF length and verify both TX and RX use the same unit and endianness.

Off-by-one risk: composeAttenCharInd() clears 130 bytes, but you transmit 129 on timeout. Reconfirm the exact frame size including HomePlug headers so the modem does not drop it (many PLC stacks are strict about length).

Retry/back-off: If the EV never sends SLAC_MATCH.REQ, or ATTEN_CHAR.RSP is “failed”, your code falls back silently or restarts GET_SW. You need explicit error codes, recovery paths, and a maximum retry policy.

NMK/NID policy: You randomize NMK (good) but use a fixed NID. Some implementations derive NID from NMK; fixed NID may clash with other PLC networks on site. Consider deriving NID deterministically from NMK and/or session context.

**IPv6, SDP, and ND**

Hop Limit: For your UDP/SDP IP header you set Hop-Limit = 0x0A (10); for ND (ICMPv6) you use 255. Many stacks expect Hop-Limit 255 on link-local control traffic to protect against off-link spoofing. Use 255 consistently for ND and V2G link-local control.

Neighbor Discovery: You answer NS correctly, but you do not perform Duplicate Address Detection for your own link-local (some EVSE stacks do). While low-risk on a private PLC link, strict stacks can probe.

SDP parsing: Your evaluateUdpPayload enforces v2gptPayloadLen==2 (fine for RFC/ISO message), and only security=0x10, transport=0x00. EVs that request TLS (security==0x00) or UDP transport will not be serviced (by design). Consider advertising both TLS and No-TLS if you plan ISO 15118 PnC later.

**TCP**

No MSS: You commented out MSS options; without them, the peer may choose suboptimal MSS. For IPv6+PLC, negotiate a sane MSS (e.g., 1220).

Out-of-order data: You assume a whole EXI document fits one TCP segment (TCP_RX_DATA_LEN=1000). Fragmentation or Nagle/coalescing on either side will break your parser. Add a reassembly buffer and only decode when full V2GTP frame length has been accumulated.

Flags handling: You match exact flags==SYN and exact flags==ACK. Peers may set ECN or combine flags; parse by masking (e.g., (flags & TCP_FLAG_SYN)), not equality. Handle RST and FIN to cleanly close.

**HLC (DIN 70121)**

Protocol selection: You only respond when EV advertises a DIN namespace. If a car offers only ISO 15118 you do not respond (EV may abort). Add ISO path or send a negative SAP response to be explicit.

State coverage: After ChargeParameterDiscovery, you stop. For CCS2 DC you must implement CableCheck → PreCharge → PowerDelivery (Start) → (CurrentDemand loop) → Metering → PowerDelivery (Stop) → SessionStop, including timeouts (T_x) and ResponseCode/Processing.

## 4) Production-ready changes and patches

### 4.1 Fix the checksum helper (no OOB writes, handle odd lengths safely)

Problem: calculateUdpAndTcpChecksumForIPv6() writes a padding byte into the caller buffer on odd lengths. Fix: Never modify the input buffer; sum a virtual zero byte if len is odd.

Drop-in replacement for your function in ipv6.cpp (same signature, no external deps):

```cpp
// Safe: does not write into UdpOrTcpframe
uint16_t calculateUdpAndTcpChecksumForIPv6(uint8_t *UdpOrTcpframe,
                                           uint16_t UdpOrTcpframeLen,
                                           const uint8_t *ipv6source,
                                           const uint8_t *ipv6dest,
                                           uint8_t nxt) {
    uint32_t sum = 0;

    // IPv6 pseudo header: src(16) + dst(16) + len(4) + three zero bytes + nxt(1)
    for (int i = 0; i < 16; i += 2) {
        sum += (ipv6source[i] << 8) | ipv6source[i+1];
        sum += (ipv6dest[i]   << 8) | ipv6dest[i+1];
        if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    }
    // 32-bit length
    sum += (UdpOrTcpframeLen >> 16) & 0xFFFF;
    if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    sum += (UdpOrTcpframeLen & 0xFFFF);
    if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;

    // 3 zero bytes + nxt
    sum += nxt; // only low 8 bits used here
    if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;

    // Payload (16-bit one's complement sum)
    const uint8_t *p = UdpOrTcpframe;
    uint16_t i = 0;
    while (i + 1 < UdpOrTcpframeLen) {
        sum += (p[i] << 8) | p[i+1];
        if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
        i += 2;
    }
    if (UdpOrTcpframeLen & 1) { // odd length: pad last byte with zero
        sum += (p[i] << 8);
        if (sum > 0xFFFF) sum = (sum & 0xFFFF) + 1;
    }

    return (uint16_t)~sum;
}
```

### 4.2 Normalize Hop-Limit to 255 on link-local control

In packResponseIntoIp() (UDP/SDP) set IpResponse[7] = 0xFF instead of 0x0A. This matches your ICMPv6 NA and avoids strict EVs rejecting SDP packets due to unexpected Hop-Limit.

### 4.3 SLAC timing & length corrections

ATTEN_CHAR.IND size: Ensure you send the exact frame length. You memset(...,130) but later transmit 129 bytes in the timer path. Pick one and stick to it; I suggest 130 bytes (or the exact spec’d size of MME + headers) and keep it consistent in both code paths.

SLAC_MATCH length: Align the length field you set in composeSlacMatchCnf() with the check you do at rxbuffer[21] for SLAC_MATCH.REQ. If your intended field is MVFLength, use the same value and endianness everywhere; otherwise, remove the mismatched check.

Adaptive sound handling: Read soundCount from CM_SLAC_PARAM.REQ (the EV’s request) and accept min(count, maxSupported). Extend your sound window accordingly; do not hard-cut at 600 ms if the EV requested more sounds. On timeout, include a meaningful error path (retry with back-off, limited attempts).

### 4.4 TCP robustness (minimal changes that matter)

Parse flags by mask, not equality, so SYN|ECE|CWR or ACK|PSH still match. Replace checks like if (flags == TCP_FLAG_SYN) with (flags & TCP_FLAG_SYN). Handle RST by closing state and resetting SLAC/HLC. Handle FIN with a graceful close.

Add MSS option on SYN+ACK (e.g., 1220) and set tcpHeaderLen=24 when options present. Your code already has a stub commented out; re-enable and lengthen header accordingly.

Implement a simple RTO and retransmit:

- Store the last outbound segment (seq, len).
- Start a timer when sending; on timeout without ACK advance, retransmit up to N tries, then abort HLC and fall back to SDP.
- Add idle timeout (e.g., 5–10 s) to reset the HLC session if the EV stops talking.
- Use the existing tcpActivityTimer (unused now) to track liveness.

Reassembly buffer: Buffer incoming TCP data until a complete V2GTP frame is available (8-byte header + payloadLen). Only then call the EXI decoder. This fixes fragmentation/coalescing issues seen with strict stacks.

### 4.5 V2GTP framing & HLC state machine

Validate V2GTP header before decoding: version==0x01, ~version==inverted, proper payloadType (0x8001 for EXI), and that payloadLen fits your receive buffer. Today you skip these checks and trust the parser. Add them in routeDecoderInputData()/decodeV2GTP().

Complete DIN 70121 states (needed for CCS2 DC):

- CableCheck: respond with EVSE status (proximity/pilot measurement results).
- PreCharge: honor EV requested precharge voltage/current and return EVSE voltage feedback.
- PowerDelivery: implement Start/Stop; on Start enter the CurrentDemand loop.
- CurrentDemand loop: respond with EVSE limits and present values, loop at ~100–300 ms; respect Processing and ResponseCode rules.
- MeteringReceipt, SessionStop: send/validate as needed.

Your FSM currently stops at stateWaitForCableCheckRequest.

Timers/Response windows: Implement the DIN T_x timers (e.g., response timeout per message type) to support fast EVs (tight timing) and slow/strict EVs (need retries and patient windows). Use a single scheduler (tick at 10–20 ms) rather than ad-hoc millis() checks scattered in multiple places.

### 4.6 IPv6 receive validation & defensive parsing

Before using rxbuffer contents, ensure rxbytes covers Ethernet (14) + IPv6 (40) + upper-layer header + payload you plan to read. Guard every index (54, 58, 66, etc.). Today the code assumes standard layout and crashes on any extension header.

Either reject IPv6 extension headers explicitly with a clean error path (log + ignore + keep alive), or implement a small walker for the extension header chain to find TCP/UDP/ICMPv6 reliably.

### 4.7 Miscellaneous correctness

SDP hop-limit to 255 (already covered).

SessionId: you pick a fixed SessionID 01-02-03-04; that is fine for bring-up but some EVs expect random. Consider random but consistent for the session.

Logging: printing entire frames on every packet (IPv6Manager) is useful for bring-up but will disturb tight timing on slow cores. Gate it behind #if DEBUG or a verbose flag.

## 5) Test matrix (fast / strict / slow EV behavior)

Use the matrix below after applying the fixes:

| Stage | Fast EV expectations | Strict EV expectations | Slow EV expectations | What to verify in your stack |
| --- | --- | --- | --- | --- |
| SLAC sounds | Accept 10–20 sounds within 300–800 ms | Exact field lengths, MVF length, RunID matches, NID/NMK consistency | Continue collecting beyond 10; do not early-timeout | Adaptive sound window, consistent sizes, retries |
| ND/SDP | Respond <100 ms; Hop-Limit=255 | Reject packets with wrong HL; validate content strictly | Tolerate resend SDP/NS | HL=255, correct SDP payload and checksum |
| TCP handshake | MSS negotiated; window ok | Respect flags; handle ECN; no spurious data | Retransmit lost SYN+ACK | Masked flag checks; MSS option; RTO |
| SAP | DIN picked only when offered | Negative response if ISO only | Wait for retry | V2GTP header checks and schema selection |
| DIN messages | Responses within T_x windows | Field/value semantics correct | Retries tolerated | Timers, ResponseCode/Processing |
| CableCheck/PreCharge | Low latency | Accurate status | No premature timeout | Implemented & timed |
| PowerDelivery + loop | Stable loop 100–300 ms | Proper limits & feedback | Gap tolerance | Loop cadence, meter data |
| Close/Stop | Clean FIN or RST handling | Traceable state transitions | Idle timeout cleanup | FIN/RST + session timeouts |

## 6) Notes for CCS2 DC adaptation

You already advertise DC_extended in ServiceDiscoveryRes, which is appropriate for CCS2 DC (IEC 62196-3 Configuration FF). The key additional work for CCS2 DC production:

- Complete DIN 70121 DC charging sequence: CableCheck, PreCharge, PowerDelivery loop, Stop/SessionStop (see 4.5).
- Hardware integration: Provide hooks for CP/PP measurement, contactor drive, HV measurements, isolation monitoring, and mapping to EVSEStatus/limits in your DIN responses. (Your code currently stubs these out in EXI messages.)
- Optional ISO 15118: If you plan PnC/TLS, extend SupportedApplicationProtocol handling to accept ISO 15118 namespaces and add a TLS/TCP path on 15118 with certificate chain, contract cert validation, and EVSE status model. For CCS2 DC today many vehicles still support DIN, but ISO support increases the fleet coverage.
- Error handling & user messaging: Map failures (SLAC fail, TCP fail, HLC timeout, EVSE interlock errors) to user-visible status/LEDs and log codes.

## 7) Smaller but worthwhile cleanups

- In sendSdpResponse() you have a `// ToDo: Check lenSdp against buffer size!`. Do that check against both V2G_FRAME_LEN and UDP_PAYLOAD_LEN.
- In evaluateTcpPacket(), guard all reads with rxbytes and the computed TCP header length, not hardcoded indices. Also ignore packets not matching your EvccIp and port 15118 to avoid reacting to stray traffic.
- In Timer20ms, you reset the modem on any “Invalid data!”—add a few soft errors before hard resets to reduce flapping on noisy links.

## 8) What is already good

- Manual IPv6 + UDP/TCP crafting with pseudo-header checksums is solid for an embedded stack; with the checksum fix and header walkers you will be in good shape.
- SLAC coverage is close—most observed interop issues are timing and length strictness. Your averaging of 58 groups is exactly what many EVs expect.
- DIN 70121 handshake path is clear and easy to extend to the remaining states; libcbv2g now shields you from EXI details.

## 9) Concrete TODO list (production-ready)

1. Fix checksum function (no OOB write). (4.1)
2. Set Hop-Limit=255 for SDP/UDP. (4.2)
3. SLAC: unify lengths; adaptive sound window; correct MVF length handling; explicit retry/fault states. (4.3)
4. TCP: mask flags, add MSS option, implement RTO/retransmit + idle timeout; add FIN/RST handling; add V2GTP reassembly buffer. (4.4)
5. V2GTP validation before EXI decode. (4.5)
6. Complete DIN sequence: CableCheck, PreCharge, PowerDelivery loop, Stop. (4.5)
7. IPv6 parser hardening: length guards and (minimal) extension-header walker or explicit rejection. (4.6)
8. Logging / timing: gate hex dumps, centralize timers. (4.7)

Quick references to where in your repo:

- SLAC core: `SlacManager`, composers, timers, state machine in `Timer20ms`.
- IPv6/ND/SDP: `IPv6Manager`, `evaluateNeighborSolicitation`, `evaluateUdpPayload`, `sendSdpResponse`, `calculateUdpAndTcpChecksumForIPv6`.
- TCP/HLC: `evaluateTcpPacket`, `tcp_*`, and `decodeV2GTP` (DIN 70121 flow).
- Interfaces/defs: `ipv6.h`, `tcp.h`, pins/constants in `main.h`, PlatformIO settings in `platformio.ini`.
