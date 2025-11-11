# ⚡ ESP32‑PLC CCS2 DC Stack

ESP32‑S3 firmware for CCS2 DC fast charging.  
It speaks HomePlug GreenPHY via a QCA7005 modem, runs lwIP + esp‑tls, executes DIN 70121 + ISO 15118‑2 over libcbv2g, and hosts ISO 15118‑20 DC using an embedded libiso15118 controller.

> Target: `esp32-s3-devkitc-1` (PlatformIO) – the project only contains ESP32‑compatible code paths.

---

## 🧱 Platform Overview

```
╔════════════════════════════════════════════════════════════════════╗
║                    ESP32-S3 (Arduino + ESP-IDF)                    ║
║                                                                    ║
║  ┌──────────────┐    ┌───────────────┐    ┌────────────────────┐   ║
║  │ QCA7005 SPI  │───▶│ lwIP Netif    │───▶│ UDP/TCP Sockets     │   ║
║  │ bridge       │    │ (EtherType)   │    │ (SDP + HLC)         │   ║
║  └──────────────┘    └───────────────┘    └─────────┬──────────┘   ║
║                                               ┌─────▼─────┐        ║
║                                               │ esp-tls   │        ║
║                                               └─────┬─────┘        ║
║                     ┌──────────────┐                │              ║
║                     │ libcbv2g     │<───────────────┘              ║
║                     │ (DIN/ISO-2)  │                               ║
║                     └─────┬────────┘                               ║
║                           │                                        ║
║           ┌───────────────▼──────────────┐                          ║
║           │ Embedded libiso15118 (ISO-20)│                          ║
║           └─────┬────────────────┬───────┘                          ║
║                 │                │                                  ║
║        ┌────────▼───────┐  ┌─────▼────────┐                         ║
║        │ cp_control     │  │ dc_can (MCP) │                         ║
║        │ (CP/PWM/IO)    │  │ setpoints,   │                         ║
║        └────────────────┘  │ telemetry    │                         ║
║                            └──────────────┘                         ║
╚════════════════════════════════════════════════════════════════════╝
```

---

## ✅ Feature Matrix

| Subsystem                     | Status | Notes |
|------------------------------|--------|-------|
| QCA7005 SLAC (ISO 15118‑3)   | ✅     | Adaptive timers, full CM_* flow |
| SDP / IPv6 (lwIP)            | ✅     | UDP/15118 sockets, dual endpoint (TLS + plain) |
| TCP transport                | ✅     | lwIP sockets + legacy fallback |
| TLS (esp‑tls)                | ✅     | TLS 1.2 for ISO‑2, TLS endpoint advertised via SDP |
| DIN 70121 HLC                | ✅     | CableCheck → SessionStop complete via libcbv2g |
| ISO 15118‑2 DC               | ✅     | Full PaymentDetails → SessionStop path, watchdog w/ retries |
| ISO 15118‑20 DC              | ⚙️     | Embedded libiso15118 controller enabled (TLS 1.3 plumbing next) |
| PKI store / provisioning     | ✅     | Preferences-backed + token-protected CLI/JSON |
| Diagnostics / security       | ✅     | `diag auth` tokens gate PKI endpoints |

---

## 🧰 Build & Flash

```bash
# 1) Install PlatformIO (via pip or VS Code extension)
pip install platformio

# 2) Pull submodules / vendor libs (already committed under lib/)

# 3) Build firmware
cd ESP32-PLC
python3 -m platformio run

# 4) Flash (adjust port)
python3 -m platformio run -t upload -t monitor
```

The only PlatformIO environment is `esp32-s3-devkitc-1` and it already sets:
```
framework = arduino, espidf
build_flags = -DHAVE_LIBISO15118=1 -DISO20_ENABLE=1 ...
```

---

## ⚙️ Configuration Cheat Sheet

All user‑tunable knobs live in `include/evse_config.h`. Highlights:

| Section | Macros | Description |
|---------|--------|-------------|
| Control Pilot | `CP_PWM_PIN`, `CP_ADC_PIN`, threshold constants | Map PWM/ADC pins, CP state thresholds, sample depth |
| Contactor IO | `CONTACTOR_*` macros | Coil/aux pins and polarity |
| CAN / DC modules | `CAN_*`, `DC_*` | MCP2515 pinout, ramp rates, number of modules |
| Network | `TCP_PLAIN_PORT`, `TCP_TLS_PORT` | HLC plain/TLS port numbers |
| ISO‑20 | `ISO20_ENABLE`, `ISO20_INTERFACE_NAME`, `ISO20_TLS_STRATEGY`, `ISO20_SDP_ENABLE` | Toggle libiso15118, interface name (default `plc0`), TLS policy (0 accept, 1 force, 2 no‑TLS) |
| Diagnostics | `DIAG_AUTH_TOKEN`, `DIAG_AUTH_WINDOW_MS` | Token required before PKI read/write operations |

Override any macro via PlatformIO `build_flags` (e.g., add `-DDIAG_AUTH_TOKEN=\"supersecret\"`).

---

## 🔐 Certificates & PKI

1. **ISO‑20 runtime** expects PEM files at:
   ```
   /spiffs/certs/evse_chain.pem
   /spiffs/certs/evse_key.pem
   /spiffs/certs/v2g_root.pem
   /spiffs/certs/mo_root.pem
   ```
   Sample placeholders live under `certs/iso20/`. Copy your real certs before flashing or mount SPIFFS at runtime.

2. **PKI store** (esp‑tls) is Preferences-backed. Use either:
   - UART CLI:
     ```text
     diag auth changeme
     pki set cert <base64>
     pki get cert
     ```
   - JSON RPC over the diagnostics channel:
     ```json
     {"type":"diag","op":"auth","token":"changeme"}
     {"type":"pki","op":"set","target":"cert","data_b64":"..."}
     ```
   Credentials hot‑reload automatically for new TLS sessions.

---

## 🕹️ Runtime Tips

- **Serial CLI** (`USB CDC @ 115200`):
  - `diag auth <token>` – authenticate for PKI ops.
  - `pki set|get <cert|key|ca> <base64>` – manage TLS material.
- **Status logging**: HLC state changes show up via `Serial` with CP state, ISO watchdog activity, and DIN/ISO transitions.
- **Watchdog tuning**: `ISO_STATE_TIMEOUT_MS` and `ISO_STATE_WATCHDOG_MAX_RETRIES` (in `src/tcp.cpp`) control ISO‑2 timeout behavior for automated negative tests.

---

## 🧪 Validation Checklist

1. **Transport bring-up**
   - Observe `MODEM_POWERUP → MODEM_LINK_READY` via log.
   - Run SLAC pairing (SET_KEY, SLAC_PARAM, SOUNDS, ATTEN_CHAR).
2. **SDP / IPv6**
   - Verify UDP/15118 responses for both Security=0x00 (TLS) and 0x10 (plain).
3. **DIN / ISO‑2**
   - Drive a full session (SAP → SessionStop); monitor state logs and ensure watchdog does not trigger.
4. **ISO‑20 smoke**
   - With `ISO20_ENABLE=1`, connect CP to B/C. You should see `[ISO20]` messages indicating start/stop events, confirming ControlEvents are dispatched.
5. **PKI access control**
   - Attempt a `pki get cert` without `diag auth` → expect rejection. Authenticate and retry.

### Running Automated Tests

```bash
python3 -m platformio test -e esp32-s3-devkitc-1
```

This exercises the on-target Unity suite (`test/test_diag_iso`) covering:

- Diagnostic auth token flow (success, expiry, revoke).
- ISO state watchdog timeouts/fatal retries.

Use it after any change to high-level state handling.

---

## 📚 Further Reading

- `docs/CCS2_DC_Integration_Plan.md` – phased plan + architecture decisions.
- `docs/Implementation_Backlog.md` – work items mapping to CCS2 milestones.
- `Tasks.md` – dated log of implemented steps for traceability.

---

Happy charging! 🚗⚡
