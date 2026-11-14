# Project Shuzi (v8.4) — Deterministic Edge Enclave & Multi-Billion Vector Streaming Audit

**Project Shuzi (v8.4)** is a clean-room, deterministic state-machine and Memory-Mapped I/O (MMIO) execution architecture engineered for high-density data center telemetry, edge computing, automotive ECU protection, and zero-visibility Protected Health Information (PHI) encapsulation.

> **Notice:** This repository contains non-confidential performance benchmarks, cryptographic audit logs, and public adversarial verification harnesses. Proprietary C++ source modules and binary executables are withheld and available solely under a Mutual Non-Disclosure Agreement (MNDA).

---
## Core Technical Capabilities
• Dual-Layer Quarantine & Socket-Locked Sandbox: Isolates untrusted telemetry streams inside a SOCKET_LOCKED execution sandbox under a strict 512MB memory ceiling, neutralizing buffer bombs, arithmetic traps, null-byte injections, and timestamp replay attacks with zero memory leaks.
• Isolated 128MB MMIO Backplane & Zero-Visibility PHI Sandbox: Segregates sensitive telemetry and Personal Health Information (PHI) within a dedicated 128MB isolated MMIO space, enforcing AES-256 end-to-end cryptographic encapsulation prior to external bus routing.
• Reversible Soft-Brick & Hardware MUX Failover: Replaces destructive physical eFuse tripping with a Reversible Soft-Brick state-machine topology, isolating compromised OS layers via hardware MUX switching and permitting restoration only via cryptographic master token authentication (Module 320).
• Satellite-to-5G Edge Failover & Active MitM Defense: Detects satellite line occlusion and diverts critical telemetry to terrestrial 5G edge networks without packet loss, blocking Man-in-the-Middle (MitM) intercept attempts and 5,000 packets/sec CAN-bus flooding stress.

## 1. Benchmark Highlights (1B Single-Core & 4B Multi-Core Scaling)

All benchmarks were executed inside a `SOCKET_LOCKED` Dual-Layer Quarantine sandbox (`MEM_CAP: 512 MB` per instance) pinned to dedicated physical P-Cores (`taskset`) against a hostile ~80 GB dataset (`1,008,667,864` lines) containing 32KB line bombs, `NaN`/`Infinity`/`INT64_MAX` traps, 9.16M null bytes, 1ms state-thrashing bursts, and timestamp replay spoofing.

| Audit Tier | Topology | Total Vectors | Data Volume | Aggregate Throughput | Scaling / Retention | Peak RSS / Instance | Crashes |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **10M Smoke** | 1x P-Core (`CPU 2`) | 10,086,766 | ~0.8 GB | **663,776 VEC/S** | Baseline (10M) | **16.0 MB** (`16,044 KB`) | **0** |
| **1B Single** | 1x P-Core (`CPU 2`) | 1,008,667,864 | ~80 GB | **665,738 VEC/S** | **100.3%** linear retention | **16.0 MB** (`16,044 KB`) | **0** |
| **4B Parallel** | 4x P-Cores (`CPU 0,2,4,6`) | 4,034,671,456 | ~320 GB | **2,438,097 VEC/S** | **93.16%** (`3.73x` on 4 cores) | **~16.0 MB** (`15,972–16,108 KB`) | **0** |

---

## 2. Cryptographic Integrity & Determinism Seals (SHA-256)

Every execution across 1B single-core and 4B multi-core runs produced identical input/runtime digests and an invariant `STATE_CHAIN` hash, proving 100% bit-exact deterministic state transitions under Zero-Trust Fail-Closed quarantine:

* **Input Dataset (`PRE_SHA256`):** `6dd8a0602198b7f7963081989b9c6fddd2e38e8be58a78935ac7fb70393886df`
* **Runtime Verification (`RUN_SHA256`):** `6dd8a0602198b7f7963081989b9c6fddd2e38e8be58a78935ac7fb70393886df` (`MATCH_PRE: True`)
* **Deterministic `STATE_CHAIN` (All 5 Runs Identical):** `232d3fe1d555581c473e63c75c6d578e8fcba871701c6b98c8365b0cf8a65d30`
* **Output Audit Log Seals (`OUT_SHA256`):**
  * `SHUZI_V8_4_DUAL_AUDIT_20261114_235908.log` (1B Peak Run): `6f013ed183a7c620a8bb7e8f32e7bafe7bb38960151d6bb77cb8e588570a413d`
  * `SHUZI_V8_4_DUAL_AUDIT_20261114_222242.log` (1B Baseline): `69eaf1616fea1d2d0dcd5ae86e97f612eb16d76ccf4c50e90909a373ab183bbf`
  * `audit_pcore_0.out` (4B Parallel Core 0): `518729d03a6f544c8dc9fd721d3cfe2cb6f006d8cab10d7e452cfefc80bef798`
  * `audit_pcore_2.out` (4B Parallel Core 2): `59766befccb2b226cd7fc2aea878274b4be675c348fd820924ef71d67e444dbd`
  * `audit_pcore_4.out` (4B Parallel Core 4): `d8b25c461070140477324239ca70c52528269dd87142e713b69fe5ba102a6615`
  * `audit_pcore_6.out` (4B Parallel Core 6): `d02978a95bd91804c3ee3bc0f4eb86024ee86d5e9e7f0d9d7b2b834666aefbe1`

---

## 3. Repository Structure & Verification Harnesses

* `run_verify_v5.py` — Deterministic 1-Billion row (~80 GB) hostile chaos dataset generator & single-core audit launcher.
* `run_multicore_4p.sh` — 4x physical P-Core (`CPU 0, 2, 4, 6`) Shared-Nothing parallel scaling harness (`4.03B` aggregate vectors).
* `logs/` — Raw cryptographic audit logs (`SHUZI_V8_4_DUAL_AUDIT_*.log`, `audit_pcore_*.out`).
* **Zero Security Escalation:** All security metrics (`ESC`, `VULN`, `SILENT`, `DMG`, `ECU_BRICK`, `MOD_320`) remain `0` across all **4,034,671,456** aggregate vectors (`EMS_LOCK: 0`, `MED_PACKET_TX: 0`, `100% SYNCED: True`).

---

## 4. Repository Documentation & Reports

* **1B & 10M Dual-Layer Quarantine Audit Logs (`v8.4`):** Raw SHA-256 verified black-box audit summaries for 10M precision and 1B trojan chaos workloads (`REPORTS/`).
* **Cryptographic Integrity Manifest:** Standard SHA-256 checksums for verifying all raw log files (`sha256sum -c SHA256SUMS.txt`).
* **Enterprise Procurement & IP Summary (`PROCUREMENT_SUMMARY.md`):** Patent status, clean-room IP governance, risk mitigation ROI, and HIL evaluation requirements.
* **Commercial Use Cases & Integration Scenarios (`USE_CASES.md`):** Real-world operational workflows across emergency response, automotive security, and offline edge settlement.

---

## 5. Open Black-Box CSV Challenge & Enterprise Protocol

Project Shuzi (v8.4) provides a public black-box testing track for 6-column ASCII CSV telemetry payloads (`timestamp_ms,sensor_value,frequency_hz,signal_dbm,state_tag,alert_flag`) without requiring an NDA.

### Track A: Public Open Black-Box CSV Challenge (No NDA Required)
* **Payload Size Limit:** Maximum **50 GB – 80 GB** per uncompressed CSV submission (sufficient for up to ~1B vectors).
* **Anything Goes:** Feel free to inject `NaN`, `±Infinity`, `Int64` overflows, null bytes (`0x00`), ANSI escape sequences, unclosed quotes, 32KB+ buffer-probing lines, column explosions, timestamp reversals, or 1ms state-machine thrashing.
* **What You Get Back:** Once processed through our isolated black-box enclave, you will receive a full telemetry report including throughput, worst-case latency, RSS memory delta, and SHA-256 cryptographic integrity seals.

#### Expected 6-Column CSV Schema & Baseline Examples

```csv
timestamp_ms,sensor_value,frequency_hz,signal_dbm,state_tag,alert_flag
1731500001000,1.2,210.0,-65.0,STANDBY,0
1731500002000,8.1,850.0,-20.0,DISTRESS,1
1731500003000,0.5,210.0,-80.0,PRANK,0
```

### Track B: Enterprise Technical Due Diligence & Commercial Licensing

For verified corporate and semiconductor partners:

1. **Mutual NDA (MNDA):** Bilateral non-disclosure agreement.
2. **Dedicated HIL Evaluation:** Hardware-level validation on target silicon platforms.
3. **Commercial Licensing & Full IP Transfer:** Transfer of source code and master tokens upon settlement.

---

## 6. Open Challenge Submission & Corporate Inquiries

To submit custom CSV payloads for the **Public Black-Box Challenge (No NDA)**, or to request enterprise technical briefings and patent documentation under MNDA, please use our intake channel:
### **[Access Secure Enterprise Intake Channel](https://docs.google.com/forms/d/e/1FAIpQLScdm_8xb3I5zMYN7EsDksdNFwrgrdi9eIfVo3IcS2RM3S5QYw/viewform?usp=publish-editor)**

