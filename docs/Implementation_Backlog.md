# CCS2 DC Practical Implementation Backlog

This backlog translates the high-level integration plan into concrete, traceable work items. Each work item references the relevant modules in the repo and the external dependencies it touches.

| ID | Work Item | Description | Owners | Status |
|----|-----------|-------------|--------|--------|
| NET-01 | lwIP/QCA netif scaffold | Introduce a reusable interface (`lwip_bridge`) that will host the QCA700x ↔ lwIP glue code. Initially stubs, later wired to QCA SPI. | Firmware | ☑ |
| NET-02 | Enable ESP-IDF lwIP stack | Switch PlatformIO env to dual framework (`arduino, espidf`), configure FreeRTOS tick (1 kHz) / Arduino init shim, and verify baseline build. | Firmware | ☑ |
| NET-03 | UDP SDP over sockets | Re-implement SDP server on lwIP UDP socket; retire hand-crafted IPv6 frame replies once NET-01/02 ready. | Firmware | ☑ |
| NET-04 | TCP server via lwIP | Replace custom TCP parsing with lwIP sockets, bridging payloads into existing DIN HLC. | Firmware | ☑ |
| EXI-01 | Vendor libcbv2g | Add libcbv2g sources/headers under `lib/`, set up build flags, and compile sample encode/decode unit test. | Firmware | ☑ |
| EXI-02 | Migrate DIN EXI | Replace `projectExiConnector` usage with libcbv2g types in the DIN FSM. | Firmware | ☑ |
| TLS-01 | esp-tls integration | Add TLS server (15118) using esp-tls (MbedTLS) with placeholder certificates; handshake loopback test. | Firmware/Sec | ☑ |
| TLS-02 | SDP security matrix | Respond to SDP Security=0x00/0x10 with appropriate endpoints; route TLS requests to TLS server. | Firmware | ☑ |
| HLC-01 | Complete DIN DC states | Implement CableCheck → SessionStop using existing power HAL (cp_control/dc_can). | Firmware | ☐ |
| HLC-02 | Integrate ISO-2 over TLS | Extend HLC to handle ISO15118-2 semantics (PnC/TLS) using libcbv2g. *(Initial SAP/SessionSetup/ServiceDiscovery/PaymentServiceSelection scaffolding merged; remaining execution states pending.)* | Firmware | ☐ |
| HLC-03 | Integrate ISO-20 | Pull libiso15118, wire TLS1.3 connections, and map callbacks to EVSE HAL. | Firmware | ☐ |
| HAL-01 | Power HAL contract | Define/implement a consolidated EVSE power HAL consumed by DIN/ISO FSMs (contactors, DC setpoints, isolation). | Firmware/HW | ☐ |
| PKI-01 | Certificate storage | Define storage + APIs for EVSE certificate/key + root CAs (NVS/secure element). | Security | ☑ |

_Legend_: ☐ = pending, ☑ = complete.
