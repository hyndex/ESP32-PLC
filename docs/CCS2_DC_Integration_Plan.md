# CCS2 DC Stack – Production Integration Plan

This document captures the end-to-end plan for evolving the ESP32‑S3 DC‑PLC firmware into a production-grade CCS2 stack that supports DIN 70121, ISO 15118‑2, and ISO 15118‑20 using libcbv2g (EXI/V2GTP) and libiso15118 (optional for -20).

---

## 0. Current Baseline

* **SLAC**: Implemented in `src/main.cpp` (`SlacManager()` + 20 ms task): SET_KEY → SLAC_PARAM → MNBC sound → ATTEN_CHAR → SLAC_MATCH → GET_SW.
* **IPv6 / SDP / ND**: Manual IPv6 parsing in `src/ipv6.cpp`; SDP response w/ security=0x10 only; NA crafted explicitly.
* **TCP/HLC**: Hand-written TCP stack + `projectExiConnector` EXI codec; DIN flow implemented up to ChargeParameterDiscovery.

---

## 1. Libraries & Components to Integrate

| Component | Purpose | Reference |
|-----------|---------|-----------|
| **libcbv2g** | Unified EXI/V2GTP encoder/decoder for DIN, ISO‑2, ISO‑20 | <https://github.com/EVerest/libcbv2g> |
| **libiso15118** (optional) | ISO 15118‑20/-2 FSMs + TLS hooks | <https://github.com/EVerest/libiso15118> |
| **lwIP (ESP-IDF)** | Replace custom TCP/UDP stack; provide sockets/TLS | ESP-IDF (dual framework) |
| **MbedTLS** (ESP-IDF) or **wolfSSL** | TLS 1.2/1.3 for ISO‑2 PnC & ISO‑20 (mandatory TLS 1.3) | [Espressif Docs](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html) / [wolfSSL component](https://components.espressif.com/components/wolfssl/wolfssl/versions/5.8.2~1/readme) |
| **libslac** (optional) | ISO 15118‑3 SLAC helper if needed | <https://github.com/EVerest/libslac> |
| **Reference PKI tooling** | Dev/test certificates (-2/-20) | <https://github.com/EcoG-io/iso15118> |

---

## 2. Target Architecture (DC PLC Only)

```
QCA700x SPI HAL ─► SLAC FSM ─► Ethernet frames
                         │
                  lwIP netif (IPv6)
                         │
                 UDP 15118 (SDP), ICMPv6 ND
                         │
           TCP 15118 or TLS(TCP) 15118
                         │
               V2GTP + EXI (libcbv2g)
                         │
     DIN-70121 FSM + ISO-15118 (-2/-20) FSMs
                         │
         EVSE Power HAL (CP/PP, contactors, DC module)
```

---

## 3. Phase Plan

### Phase A – Transport Migration *(Status: ✅ Completed 2025-11-12 – lwIP netif, UDP/TCP sockets, esp‑tls server & SDP dual-endpoint support)*

1. **QCA700x netif**: wrap SPI driver as lwIP netif (eth input/output).
2. **ND/SDP via sockets**: use lwIP IPv6 + UDP socket on 15118; keep fallback NA handler.
3. **TCP via lwIP**: retire `evaluateTcpPacket`; run TCP server (15118).
4. **DIN continuity**: keep current FSM but route through sockets until EXI swap.

### Phase B – TLS Enablement *(Status: 🚧 In progress – TLS listener + PKI store complete, ISO‑2 DC execution implemented; ISO‑20 pending)*

5. **Add TLS stack**: integrate MbedTLS (esp-tls) or wolfSSL; open TLS server (15118).
6. **SDP security matrix**: respond with both Security=0x10 (no TLS) and 0x00 (TLS) endpoints.
7. **PKI**: import EVSE certs + root CAs; use EcoG scripts for dev.
   *Current status*: ESP-TLS server uses a Preferences-backed store accessible via the new serial CLI (`pki set|get <cert|key|ca>` using Base64 payloads) **and** a JSON RPC endpoint (`{"type":"pki","op":"set|get"}`) so certs/keys/roots can be rotated without reflashing. Credentials are reloaded on the fly for subsequent TLS sessions.

### Phase C – HLC Expansion

8. **Swap EXI codec** *(Status: ✅ DIN path now uses libcbv2g)*: replace `projectExiConnector` with libcbv2g.
9. **ISO‑2** *(Status: ✅)*: extend the in-house FSM (PaymentDetails → SessionStop) to reuse the ESP32 HAL and esp‑tls transport; fall back to libiso15118 only if future maintenance requires it.
10. **ISO‑20 DC**: integrate libiso15118 (TLS 1.3 mandatory) with EVSE HAL callbacks.

---

## 3b. Reference Alignment Check (EVerest vs ESP32 HAL)

To ensure the libcbv2g/HAL wiring mirrors a production-grade stack, we compared our implementation with the upstream **EvseV2G** module from EVerest (`temp/everest-core/modules/EVSE/EvseV2G`). Key takeaways:

- **HAL responsibilities match**: our contactor + DC module hooks (`cp_contactor_command` / `cp_contactor_feedback` in `src/cp_control.cpp:217` and `dc_set_targets` / `dc_enable_output` / telemetry helpers in `src/dc_can.cpp:252-323`) provide the same signals consumed inside `ISO15118_chargerImpl::handle_set_charging_parameters` and the phase callbacks in `temp/everest-core/modules/EVSE/EvseV2G/charger/ISO15118_chargerImpl.cpp`. The ESP32 HAL now exposes voltage/current feedback plus contactor state so the ISO‑2/DC execution phase can reuse the exact semantics.
- **libcbv2g usage is identical**: our DIN + ISO bitstreams (`src/tcp.cpp` @ helpers `prepare_din_message` / `prepare_iso2_message`) follow the same init/encode pattern as `iso_server.cpp:2200-2305`, ensuring message layout parity with the reference implementation. Any message we add simply mirrors the `init_iso2_*` / `encode_iso2_exiDocument` flow that EvseV2G uses.
- **State progression & timers**: we validated that the FSM sequencing (`stateWaitFor*` in `src/tcp.cpp`) lines up with the `handle_iso_*` order in `iso_server.cpp:2200-2470`. The remaining TODOs (PaymentDetails → SessionStop) therefore have a direct blueprint and only need the HAL glue already confirmed above.

This audit confirms that the ESP32 HAL plus libcbv2g stack can host the same ISO‑2/DC logic as the reference implementation; the remaining work is feature completion, not architectural changes.

---

## 3c. ISO‑20 Wiring Progress

* **Embedded libiso15118 port**: Vendored the upstream headers under `lib/libiso15118/include/iso15118/` and added an ESP32-friendly controller (`lib/libiso15118/src/tbd_controller.cpp`). The port keeps the upstream namespaces/types but replaces the Linux-only poll/openssl pieces with a lightweight state machine that triggers the same callbacks EvseV2G relies on in `temp/everest-core/modules/EVSE/EvseV2G/iso_server.cpp`.
* **Configuration hooks**: `include/evse_config.h` now exposes `ISO20_ENABLE`, `ISO20_INTERFACE_NAME`, `ISO20_TLS_STRATEGY`, and `ISO20_SDP_ENABLE`, mirroring the knobs defined in `temp/libiso15118/include/iso15118/config.hpp`. Provisioning dev certificates only requires copying PEM files into `certs/iso20/` (paths match the ones consumed in `src/iso15118_dc.cpp`).
* **HAL parity confirmation**: The embedded controller forwards `session::feedback::Signal` events (`START_CABLE_CHECK`, `PRE_CHARGE_STARTED`, `DC_OPEN_CONTACTOR`, …) into the same cp_control/dc_can helpers that DIN/ISO‑2 already use. This keeps the Maxwell CAN telemetry and contactor logic identical to the EvseV2G reference module.
* **Runtime integration**: `ISO20_ENABLE` is on by default, `HAVE_LIBISO15118` informs the build, and `iso20_loop()` now tracks CP connectivity to send embedded `d20::ControlEvent` start/stop signals so the ISO‑20 loop runs alongside DIN/ISO‑2 on real hardware.

---

## 4. SDP & Session Selection

* Accept both TLS (0x00) and no-TLS (0x10) in SDP requests.
* Advertise two endpoints: plain TCP (DIN or ISO‑2 EIM) and TLS (ISO‑2 PnC / ISO‑20).
* Map sessions using `(pevMac, pevRunId, EvccIp, port)` to prevent crosstalk.

---

## 5. DIN Execution Phase Completion

For your home-grown DIN/ISO‑2 path:

1. **CableCheck**: insulation measurement, ResponseCode + EVSEProcessing fields.
2. **PreCharge**: precharge contactor, EVSEPresentVoltage feedback.
3. **PowerDelivery(Start)**: close main contactors, start DC regulation.
4. **CurrentDemand loop**: periodic EV target ↔ EVSE feedback; enforce limits, detect faults.
5. **PowerDelivery(Stop)**: safe shutdown, open contactors.
6. **MeteringReceipt**: optional energy reporting.
7. **SessionStop**: release resources, FIN connection.

_Progress update_: DIN and ISO‑2 now share the same libcbv2g encoder/decoder, and the ISO branch drives PaymentDetails → Authorization → ChargeParameterDiscovery → CableCheck → PreCharge → PowerDelivery → CurrentDemand → MeteringReceipt → SessionStop using the cp_control/dc_can HAL and esp‑tls transport. Per-message watchdogs (ISO_STATE_TIMEOUT_MS + retry counters in `src/tcp.cpp`) emit deterministic logs for automated negative tests and can tear down the transport path when retries are exhausted.

Add per-state timers & retry policy; propagate EVSEStatus/EVEStatus codes.

---

## 6. TLS Policy

* **DIN / ISO‑2**: TLS optional (required for Plug&Charge). Support TLS 1.2 suites.
* **ISO‑20**: TLS 1.3 **mandatory**; enable TLS_AES_* suites or CHACHA20_POLY1305.
* Store certificates securely (NVS/HSM); verify V2G-PKI chains. The PKI JSON/CLI endpoints now require a diagnostic auth token (`diag auth <token>`; see `DIAG_AUTH_TOKEN` in `evse_config.h`) before reads/writes, matching the production hardening requirement for TLS material.

---

## 7. Power HAL Requirements

Implement a deterministic HAL for both DIN and libiso15118:

```c
bool cp_get_state(evse_state_t* out);
bool iso_monitor_ok(void);
bool contactor_precharge(bool on);
bool contactor_main(bool on);
bool measure_dc(float* volts, float* amps);
bool set_dc_targets(float v, float i);
bool weld_detection_ok(void);
bool meter_read(float* energy_kwh);
```

Include watchdogs, fault propagation, and safe fallback.

---

## 8. Hardening & Cleanups

* Fix IPv6 checksum padding (no buffer overwrite).
* Increase hop limit to ≥64.
* Enlarge TCP/UDP buffers to ≥1280 bytes (IPv6 minimum MTU).
* Replace per-frame logging with leveled logging to avoid timing issues.
* Add retry/timeouts for all SLAC & HLC states.

---

## 9. PlatformIO Updates

* Use dual framework (`arduino, espidf`) to access lwIP + esp-tls.
* Enable IPv6 + TLS 1.3 in sdkconfig (via `menuconfig`).
* Vendor libcbv2g (and optional libiso15118) under `lib/`.

---

## 10. Testing & Certification

* **Unit/comp tests**: EXI round-trips, SDP matrix, CP/HAL tests.
* **Interoperability**: RISE‑V2G/Josev, commercial EV simulators.
* **Soak**: 24 h connect/disconnect cycles.
* **Security**: TLS handshake variants, bad cert chains, replay attempts.

---

## 11. Execution Checklist

| Week | Tasks |
|------|-------|
| 1‑2  | lwIP netif, sockets, buffer fixes, logging gating |
| 3‑4  | libcbv2g integration, complete DIN DC states |
| 5‑6  | TLS stack, SDP security matrix, ISO‑2 TLS path, PKI storage |
| 7+   | libiso15118 for ISO‑20, TLS 1.3 tuning, EV simulator interop |

---

## 12. References

* libcbv2g: <https://github.com/EVerest/libcbv2g>
* libiso15118: <https://github.com/EVerest/libiso15118>
* libslac: <https://github.com/EVerest/libslac>
* EcoG ISO15118 (PKI scripts): <https://github.com/EcoG-io/iso15118>
* ESP-IDF MbedTLS: <https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/mbedtls.html>
* wolfSSL ESP component: <https://components.espressif.com/components/wolfssl/wolfssl/versions/5.8.2~1/readme>
* ISO15118-20 summary: <https://www.switch-ev.com/blog/new-features-and-timeline-for-iso15118-20>
