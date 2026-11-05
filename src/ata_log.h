#pragma once
/* DriveMonitor - ATA General Purpose Logging (GPL): SATA PHY (Log 11h) and Device Statistics (Log 04h).
 * GPL-2.0-or-later. See LICENSE. */

#include <windows.h>
#include <stdint.h>
#include "smart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SATA PHY Event IDs (SATA Log 11h, Serial ATA specification) */
#define SATA_PHY_EV_COMMAND_ICRC          0x0001
#define SATA_PHY_EV_R_ERR_DATA_FIS        0x0002
#define SATA_PHY_EV_R_ERR_D2H_DATA        0x0003
#define SATA_PHY_EV_R_ERR_H2D_DATA        0x0004
#define SATA_PHY_EV_R_ERR_NON_DATA_FIS    0x0005
#define SATA_PHY_EV_R_ERR_D2H_NON_DATA    0x0006
#define SATA_PHY_EV_R_ERR_H2D_NON_DATA    0x0007
#define SATA_PHY_EV_RETRIES_D2H_NON_DATA  0x0008
#define SATA_PHY_EV_PHYRDY_TRANSITIONS    0x0009  /* PHYRDY to !PHYRDY: link drop / contact / power dip */
#define SATA_PHY_EV_COMRESET              0x000A  /* FISes sent due to COMRESET */
#define SATA_PHY_EV_CRC_ERRORS_H2D        0x000B  /* CRC error within Host-to-Device FIS: cable signal issue */
#define SATA_PHY_EV_NON_CRC_ERRORS_H2D    0x000D  /* Non-CRC errors within H2D FIS (8b10b / framing) */

/* ATA Device Statistics Log Pages (ATA Log 04h, ATA-8 ACS specification) */
#define ATA_DEV_STAT_PAGE_SUPPORTED       0x00
#define ATA_DEV_STAT_PAGE_GENERAL         0x01
#define ATA_DEV_STAT_PAGE_FREEFALL        0x02
#define ATA_DEV_STAT_PAGE_ROTATING        0x03
#define ATA_DEV_STAT_PAGE_ERRORS          0x04
#define ATA_DEV_STAT_PAGE_TEMP            0x05
#define ATA_DEV_STAT_PAGE_TRANSPORT       0x06
#define ATA_DEV_STAT_PAGE_SSD             0x07

/* Bit masks for 64-bit Device Statistics descriptors */
#define ATA_DEV_STAT_BIT_SUPPORTED        0x8000000000000000ULL
#define ATA_DEV_STAT_BIT_VALID            0x4000000000000000ULL
#define ATA_DEV_STAT_BIT_NORMALIZED       0x2000000000000000ULL
#define ATA_DEV_STAT_VAL_MASK             0x0000FFFFFFFFFFFFULL

/* Parsing functions */
BOOL ParseSataPhyLog(const BYTE* pLog512, DRIVE_INFO* pInfo);
BOOL ParseDeviceStatisticsPage(BYTE bPage, const BYTE* pPage512, DRIVE_INFO* pInfo);

/* Seagate FARM Log (Field Accessible Reliability Metrics, Log A6h) */
#define ATA_LOG_SEAGATE_FARM              0xA6
#define SEAGATE_FARM_SIGNATURE            "FARMER"
#define SEAGATE_FARM_PAGE_HEADER_SEC      0
#define SEAGATE_FARM_PAGE_INFO_SEC        32
#define SEAGATE_FARM_PAGE_WORKLOAD_SEC    64
#define SEAGATE_FARM_PAGE_ERRORS_SEC      96
#define SEAGATE_FARM_PAGE_ENV_SEC         128

BOOL ParseSeagateFarmHeader(const BYTE* pSec512, DRIVE_INFO* pInfo);
BOOL ParseSeagateFarmDriveInfo(const BYTE* pSec512, DRIVE_INFO* pInfo);
BOOL ParseSeagateFarmWorkload(const BYTE* pSec512, DRIVE_INFO* pInfo);
BOOL ParseSeagateFarmErrorStats(const BYTE* pSec512, DRIVE_INFO* pInfo);
BOOL ParseSeagateFarmEnvironment(const BYTE* pSec512, DRIVE_INFO* pInfo);
void AcquireSeagateFarmLog(HANDLE hDrive, BOOL bIsSAT, DRIVE_INFO* pInfo);

/* Reading / retrieval functions */
BOOL ReadAtaLogExtNative(HANDLE hDrive, BYTE bLogAddr, WORD wPage, void* pBuf, DWORD dwBufSize);
BOOL ReadAtaLogExtSAT(HANDLE hDrive, BYTE bLogAddr, WORD wPage, void* pBuf, DWORD dwBufSize);
void AcquireAtaLogs(HANDLE hDrive, BOOL bIsUSB, DRIVE_INFO* pInfo);

#ifdef __cplusplus
}
#endif
