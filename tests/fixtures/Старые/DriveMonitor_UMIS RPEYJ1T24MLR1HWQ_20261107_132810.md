# DriveMonitor — drive report

**Version:** 2.0 beta 12 (build 63)  
**Date:** 2026-11-07 13:28:10

## Drive

| Field | Value |
| --- | --- |
| Model | UMIS RPEYJ1T24MLR1HWQ |
| Brand | — |
| Controller | — |
| Serial | SS1D71512X1RC4152159 |
| Firmware | 1.3Q06B1 |
| Capacity / type | 953.9 GB · NVMe SSD |
| Temperature | 22 °C (normal) |
| Power-on | 180 days 8 hours |
| Power cycles | 463 |
| Protocol | USB NVMe · NVMe 1.4.0 |
| USB bridge | Realtek RTL9210B-CG |

## Assessment

```
Drive: 🟢 GOOD
Assessment: 🟢 GOOD

Adapter: SAT yes · SMART no · thresholds no · NVMe yes

No critical problems found.
Media, wear, interface and temperature are OK.

Life remaining: 99%
Power-on: 180 days 8 hours
```

## Expert notes

```
Drive: 🟢 GOOD
Assessment: 🟢 GOOD

Adapter: SAT yes · SMART no · thresholds no · NVMe yes

Summary: GOOD
Assessment confidence: 87%

Reasons:
  • model only partly identified
  • SMART fully read
  • vendor decoder incomplete

Main evidence:
  ✓ Critical warning: none
  ✓ Available spare: 100%
  ✓ Media errors: 0
  • Reported life remaining: 99%   (NAND wear, not health)

Context (not a penalty):
  Temperature: 22 °C · normal (warn 80 °C, crit 83 °C). Sensor 1: 22 °C. Sensor 2: 29 °C. Time at high temperature: warning 0 min, critical 0 min.
  Power-on: 180 days 8 hours. Power-on hours are not a failure sign.
  Power cycles: 463. Context, not a penalty.
  Unsafe shutdowns: 57. A power log, not a penalty.

Evidence matrix:
  Evidence                    Value                                               Effect          
  SMART overall               PASS                                                +               
  Available spare             100%                                                +               
  Media errors                0                                                   +               
  Life remaining              99%                                                 +               
  Temperature                 22 °C                                               0               
  Power-on                    180 days 8 hours                                    0               
  Controller                  —                                                   −confidence     
  Profile                     nvme-generic (50)                                   0               

Verdict:
  GOOD because:
    3 strong positive signs
    0 critical failures
    0 signs of media degradation
Percentage used is NAND wear. It is not mixed with spare or media errors. Percentage used, spare, the critical warning, media errors, and temperature are not folded into one health percent.
```

## SMART / NVMe

| ID | Attribute | Value | Worst | Thresh | RAW | Decoded | Status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 (01h) | Critical warning | 0 (none) | n/a | n/a | 0 | — | OK |
| 2 (02h) | Temperature | 22 C (warn 80, crit 83) | n/a | 80 C | 295 | — | OK |
| -- | Warning threshold | 80 C | n/a | n/a | 353 | — | OK |
| -- | Critical threshold | 83 C | n/a | n/a | 356 | — | OK |
| 3 (03h) | Available spare | 100 % | n/a | 10 % | 100 | — | OK |
| 4 (04h) | Spare threshold | 10 % | n/a | n/a | 10 | — | OK |
| 5 (05h) | Percentage used | 1 % | n/a | n/a | 1 | — | OK |
| 6 (06h) | Data units read | 13.9 TB | n/a | n/a | 27136408 | — | OK |
| 7 (07h) | Data units written | 9.9 TB | n/a | n/a | 19261400 | — | OK |
| 8 (08h) | Host read commands | 240822863 | n/a | n/a | 240822863 | — | OK |
| 9 (09h) | Host write commands | 268319015 | n/a | n/a | 268319015 | — | OK |
| 10 (0Ah) | Controller busy time | 9226 мин | n/a | n/a | 9226 | — | OK |
| 11 (0Bh) | Power cycles | 463 | n/a | n/a | 463 | — | OK |
| 12 (0Ch) | Power-on | 180 days 8 hours | n/a | n/a | 4328 | — | OK |
| 13 (0Dh) | Unsafe shutdowns | 57 | n/a | n/a | 57 | — | power log |
| 14 (0Eh) | Media errors | 0 | n/a | n/a | 0 | — | OK |
| 15 (0Fh) | Error log entries | 0 | n/a | n/a | 0 | — | OK |
| -- | Time above warning temp | 0 мин | n/a | n/a | 0 | — | OK |
| -- | Time above critical temp | 0 мин | n/a | n/a | 0 | — | OK |
| T1 | Temperature sensor 1 | 22 C | n/a | n/a | 295 | — | OK |
| T2 | Temperature sensor 2 | 29 C | n/a | n/a | 302 | — | OK |
| -- | Thermal Mgmt T1 переходов | 23 | n/a | n/a | 23 | — | OK |

## Windows Storage Events (Event Log, 30 days)

- **Event 153 (IO Retries / таймауты)**: 0
- **Event 129 (Controller Resets / сбросы шины)**: 1
- **Event 51 (Paging Errors / ошибки страничного обмена)**: 0
- **Event 7/154 (Bad Blocks / аппаратные сбои блоков)**: 0
- **Последнее событие / Latest event**: 2026-10-27 13:58:20 (Event 129 (Controller reset \Device\RaidPort0))

## TRIM Status & Logical Volumes

- **Windows TRIM (DisableDeleteNotify)**: Enabled (0)
- **Drive TRIM Support**: Supported

### Logical Volumes

| Volume | File System | Volume TRIM | Free | Total | Free % |
| --- | --- | --- | --- | --- | --- |
| G: | NTFS | Yes | 51.8 GB | 198.9 GB | 26.0% |
| H: | NTFS | Yes | 588.3 GB | 753.4 GB | 78.1% |
