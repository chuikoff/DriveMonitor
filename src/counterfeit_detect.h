/* DriveMonitor - Counterfeit and Relabeled Drive Detector. GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef COUNTERFEIT_DETECT_H
#define COUNTERFEIT_DETECT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

struct _DRIVE_INFO;
typedef struct _DRIVE_INFO DRIVE_INFO;

/* Checks the drive for hardware/microcode contradictions:
 * - WD model with Seagate or HGST firmware signature
 * - WD 5400 RPM consumer model physically spinning at 7200 RPM
 * - Seagate model with WD or HGST firmware signature
 * - Samsung NVMe SSD with non-Samsung PCI Vendor ID
 * - Samsung SATA SSD with foreign controller / mismatched firmware
 * - WD/SanDisk NVMe SSD with foreign PCI Vendor ID
 * - Crucial/Micron NVMe SSD with foreign PCI Vendor ID
 * - Intel/Solidigm NVMe SSD with foreign PCI Vendor ID
 * Sets pInfo->bCounterfeitSuspected, pInfo->szCounterfeitReason and szCounterfeitReasonEn. */
void DetectCounterfeitMismatch(struct _DRIVE_INFO* pInfo);

/* Returns localized reason string (RU/EN) according to current UI language */
const char* GetCounterfeitReason(const struct _DRIVE_INFO* pInfo);

/* Returns localized badge tag (e.g. "⚠️ [ПЕРЕПРОШИТ / MPTOOL]", "⚠️ [ПОДДЕЛКА]") */
const char* GetCounterfeitBadge(const struct _DRIVE_INFO* pInfo);

#ifdef __cplusplus
}
#endif

#endif /* COUNTERFEIT_DETECT_H */
