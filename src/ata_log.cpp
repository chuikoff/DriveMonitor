/* DriveMonitor - ATA General Purpose Logging (GPL): SATA PHY (Log 11h) and Device Statistics (Log 04h).
 * GPL-2.0-or-later. See LICENSE. */

#include "ata_log.h"
#include "smart_internal.h"
#include <string.h>
#include <ctype.h>

BOOL ParseSataPhyLog(const BYTE* pLog512, DRIVE_INFO* pInfo)
{
    if (!pLog512 || !pInfo) return FALSE;

    /* Reject empty/all-zero buffer */
    if (IsBufferAllZero(pLog512, 512)) return FALSE;

    /* Iterate through counters starting at offset 4 */
    size_t offset = 4;
    BOOL bFoundAny = FALSE;

    while (offset + 2 <= 510) {
        WORD rawID = (WORD)pLog512[offset] | (WORD)((WORD)pLog512[offset + 1] << 8);
        if (rawID == 0) {
            /* 0 indicates the end of counter entries */
            break;
        }

        WORD eventID = (WORD)(rawID & 0x0FFF);
        BYTE counterBits = (BYTE)((rawID >> 12) & 0x07);
        size_t counterLen = 0;
        switch (counterBits) {
        case 1: counterLen = 2; break; /* 16-bit uint */
        case 2: counterLen = 4; break; /* 32-bit uint */
        case 3: counterLen = 6; break; /* 48-bit uint */
        case 4: counterLen = 8; break; /* 64-bit uint */
        default:
            /* Unknown counter size, stop parsing */
            offset = 512;
            break;
        }

        if (counterLen == 0 || offset + 2 + counterLen > 512) {
            break;
        }

        /* Extract little-endian uint64 value */
        uint64_t val = 0;
        for (size_t b = 0; b < counterLen; b++) {
            val |= ((uint64_t)pLog512[offset + 2 + b]) << (b * 8);
        }

        bFoundAny = TRUE;

        /* Map standard SATA-IO PHY event counters */
        switch (eventID) {
        case SATA_PHY_EV_COMMAND_ICRC:
            pInfo->nPhyCommandIcrcErrors = val;
            break;
        case SATA_PHY_EV_R_ERR_DATA_FIS:
        case SATA_PHY_EV_R_ERR_D2H_DATA:
        case SATA_PHY_EV_R_ERR_H2D_DATA:
            pInfo->nPhyR_ErrDataFis += val;
            break;
        case SATA_PHY_EV_PHYRDY_TRANSITIONS:
            pInfo->nPhyTransitionsPhyRdy = val;
            break;
        case SATA_PHY_EV_COMRESET:
            pInfo->nPhyComresetCount = val;
            break;
        case SATA_PHY_EV_CRC_ERRORS_H2D:
            pInfo->nPhyCrcErrorsH2D = val;
            break;
        case SATA_PHY_EV_NON_CRC_ERRORS_H2D:
            pInfo->nPhyNonCrcErrorsH2D = val;
            break;
        default:
            break;
        }

        offset += 2 + counterLen;
    }

    if (bFoundAny) {
        pInfo->bSataPhyLogSupported = TRUE;
    }
    return bFoundAny;
}

BOOL ParseDeviceStatisticsPage(BYTE bPage, const BYTE* pPage512, DRIVE_INFO* pInfo)
{
    if (!pPage512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pPage512, 512)) return FALSE;

    /* Verify page number at byte 2 */
    if (pPage512[2] != bPage) return FALSE;

    BOOL bFound = FALSE;

    switch (bPage) {
    case ATA_DEV_STAT_PAGE_GENERAL: { /* Page 01h */
        /* Logical sectors written: offset 0x18 */
        uint64_t wWritten = ReadLE64(pPage512 + 0x18);
        if (wWritten & ATA_DEV_STAT_BIT_SUPPORTED) {
            pInfo->qwDevStatSectorsWritten = wWritten & ATA_DEV_STAT_VAL_MASK;
            bFound = TRUE;
        }
        /* Logical sectors read: offset 0x28 */
        uint64_t wRead = ReadLE64(pPage512 + 0x28);
        if (wRead & ATA_DEV_STAT_BIT_SUPPORTED) {
            pInfo->qwDevStatSectorsRead = wRead & ATA_DEV_STAT_VAL_MASK;
            bFound = TRUE;
        }
        break;
    }
    case ATA_DEV_STAT_PAGE_ERRORS: { /* Page 04h */
        /* Number of reported uncorrectable errors: offset 0x08 */
        uint64_t wErr = ReadLE64(pPage512 + 0x08);
        if (wErr & ATA_DEV_STAT_BIT_SUPPORTED) {
            pInfo->qwDevStatUncorrectable = wErr & ATA_DEV_STAT_VAL_MASK;
            bFound = TRUE;
        }
        break;
    }
    case ATA_DEV_STAT_PAGE_TEMP: { /* Page 05h */
        /* Highest lifetime temperature: offset 0x20 */
        uint64_t wMax = ReadLE64(pPage512 + 0x20);
        if ((wMax & ATA_DEV_STAT_BIT_SUPPORTED) && (wMax & ATA_DEV_STAT_BIT_VALID)) {
            int tMax = (int)(char)(wMax & 0xFF);
            if (tMax > 0 && tMax < 125) {
                pInfo->nDevStatLifetimeMaxTempC = tMax;
                if (tMax > pInfo->nTempMaxC) {
                    pInfo->nTempMaxC = tMax;
                }
                bFound = TRUE;
            }
        }
        /* Lowest lifetime temperature: offset 0x28 */
        uint64_t wMin = ReadLE64(pPage512 + 0x28);
        if ((wMin & ATA_DEV_STAT_BIT_SUPPORTED) && (wMin & ATA_DEV_STAT_BIT_VALID)) {
            int tMin = (int)(char)(wMin & 0xFF);
            if (tMin > -40 && tMin < 100) {
                pInfo->nDevStatLifetimeMinTempC = tMin;
                if (pInfo->nTempMinC < 0 || tMin < pInfo->nTempMinC) {
                    pInfo->nTempMinC = tMin;
                }
                bFound = TRUE;
            }
        }
        break;
    }
    case ATA_DEV_STAT_PAGE_SSD: { /* Page 07h */
        /* Percentage Used Endurance Indicator: offset 0x08 */
        uint64_t wEndur = ReadLE64(pPage512 + 0x08);
        if ((wEndur & ATA_DEV_STAT_BIT_SUPPORTED) && (wEndur & ATA_DEV_STAT_BIT_VALID)) {
            int pct = (int)(wEndur & 0x7F);
            if (pct >= 0 && pct <= 100) {
                pInfo->nDevStatPercentUsed = pct;
                if (pInfo->nEndurancePercent < 0) {
                    pInfo->nEndurancePercent = 100 - pct;
                }
                bFound = TRUE;
            }
        }
        break;
    }
    default:
        break;
    }

    if (bFound) {
        pInfo->bDevStatSupported = TRUE;
    }
    return bFound;
}

BOOL ParseSeagateFarmHeader(const BYTE* pSec512, DRIVE_INFO* pInfo)
{
    if (!pSec512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pSec512, 512)) return FALSE;

    /* Verify signature "FARMER" */
    if (memcmp(pSec512, SEAGATE_FARM_SIGNATURE, 6) != 0) {
        return FALSE;
    }

    pInfo->bFarmLogSupported = TRUE;
    pInfo->dwFarmMajorVersion = (DWORD)ReadLE64(pSec512 + 8);
    pInfo->dwFarmMinorVersion = (DWORD)ReadLE64(pSec512 + 16);
    return TRUE;
}

BOOL ParseSeagateFarmDriveInfo(const BYTE* pSec512, DRIVE_INFO* pInfo)
{
    if (!pSec512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pSec512, 512)) return FALSE;

    /* Verify pageNumber == 1 */
    uint64_t pageNumber = ReadLE64(pSec512);
    if (pageNumber != 1) return FALSE;

    /* Word 19 (offset 152): Power-On Hours */
    pInfo->qwFarmPowerOnHours = ReadLE64(pSec512 + 152);
    /* Word 20 (offset 160): Spindle Power-On Hours */
    pInfo->qwFarmSpindlePowerOnHours = ReadLE64(pSec512 + 160);
    /* Word 21 (offset 168): Head Flight Hours */
    pInfo->qwFarmHeadFlightHours = ReadLE64(pSec512 + 168);
    /* Word 23 (offset 184): Power Cycle Count */
    pInfo->qwFarmPowerCycleCount = ReadLE64(pSec512 + 184);

    /* Word 45 (offset 360): Date of Assembly ASCII YYWW */
    if (pSec512[360] != 0) {
        char rawDate[5];
        memcpy(rawDate, pSec512 + 360, 4);
        rawDate[4] = '\0';
        if (isdigit((unsigned char)rawDate[0]) && isdigit((unsigned char)rawDate[1]) &&
            isdigit((unsigned char)rawDate[2]) && isdigit((unsigned char)rawDate[3])) {
            safe_snprintf(pInfo->szFarmAssemblyDate, "%s", rawDate);
        }
    }
    return TRUE;
}

BOOL ParseSeagateFarmWorkload(const BYTE* pSec512, DRIVE_INFO* pInfo)
{
    if (!pSec512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pSec512, 512)) return FALSE;

    uint64_t pageNumber = ReadLE64(pSec512);
    if (pageNumber != 2) return FALSE;

    /* Word 8 (offset 64): Logical Sectors Written */
    pInfo->qwFarmSectorsWritten = ReadLE64(pSec512 + 64);
    /* Word 9 (offset 72): Logical Sectors Read */
    pInfo->qwFarmSectorsRead = ReadLE64(pSec512 + 72);
    return TRUE;
}

BOOL ParseSeagateFarmErrorStats(const BYTE* pSec512, DRIVE_INFO* pInfo)
{
    if (!pSec512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pSec512, 512)) return FALSE;

    uint64_t pageNumber = ReadLE64(pSec512);
    if (pageNumber != 3) return FALSE;

    /* Word 4 (offset 32): Number of Reallocated Sectors */
    pInfo->qwFarmReallocatedSectors = ReadLE64(pSec512 + 32);
    return TRUE;
}

BOOL ParseSeagateFarmEnvironment(const BYTE* pSec512, DRIVE_INFO* pInfo)
{
    if (!pSec512 || !pInfo) return FALSE;
    if (IsBufferAllZero(pSec512, 512)) return FALSE;

    uint64_t pageNumber = ReadLE64(pSec512);
    if (pageNumber != 4) return FALSE;

    /* Word 3 (offset 24): Highest Temperature */
    uint64_t maxT = ReadLE64(pSec512 + 24);
    if (maxT > 0 && maxT < 125) {
        pInfo->nFarmLifetimeMaxTempC = (int)maxT;
    }
    /* Word 4 (offset 32): Lowest Temperature */
    uint64_t minT = ReadLE64(pSec512 + 32);
    if (minT > 0 && minT < 100) {
        pInfo->nFarmLifetimeMinTempC = (int)minT;
    }
    return TRUE;
}

void AcquireSeagateFarmLog(HANDLE hDrive, BOOL bIsSAT, DRIVE_INFO* pInfo)
{
    if (!hDrive || !pInfo) return;

    /* Check if drive is likely a Seagate HDD */
    BOOL bLikelySeagate = (pInfo->eVendor == VENDOR_SEAGATE ||
                           pInfo->eController == CONTROLLER_SEAGATE ||
                           (pInfo->szModel[0] == 'S' && pInfo->szModel[1] == 'T') ||
                           strstr(pInfo->szModel, "Seagate") != NULL);
    if (!bLikelySeagate) return;

    BYTE secBuf[512];

    /* Sector 0: FARM Header */
    ZeroMemory(secBuf, sizeof(secBuf));
    BOOL ok0 = bIsSAT ? ReadAtaLogExtSAT(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_HEADER_SEC, secBuf, sizeof(secBuf))
                      : ReadAtaLogExtNative(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_HEADER_SEC, secBuf, sizeof(secBuf));
    if (!ok0) return;
    if (!ParseSeagateFarmHeader(secBuf, pInfo)) return;

    /* Sector 32: Page 1 Drive Info */
    ZeroMemory(secBuf, sizeof(secBuf));
    BOOL ok1 = bIsSAT ? ReadAtaLogExtSAT(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_INFO_SEC, secBuf, sizeof(secBuf))
                      : ReadAtaLogExtNative(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_INFO_SEC, secBuf, sizeof(secBuf));
    if (ok1) ParseSeagateFarmDriveInfo(secBuf, pInfo);

    /* Sector 64: Page 2 Workload */
    ZeroMemory(secBuf, sizeof(secBuf));
    BOOL ok2 = bIsSAT ? ReadAtaLogExtSAT(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_WORKLOAD_SEC, secBuf, sizeof(secBuf))
                      : ReadAtaLogExtNative(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_WORKLOAD_SEC, secBuf, sizeof(secBuf));
    if (ok2) ParseSeagateFarmWorkload(secBuf, pInfo);

    /* Sector 96: Page 3 Error Statistics */
    ZeroMemory(secBuf, sizeof(secBuf));
    BOOL ok3 = bIsSAT ? ReadAtaLogExtSAT(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_ERRORS_SEC, secBuf, sizeof(secBuf))
                      : ReadAtaLogExtNative(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_ERRORS_SEC, secBuf, sizeof(secBuf));
    if (ok3) ParseSeagateFarmErrorStats(secBuf, pInfo);

    /* Sector 128: Page 4 Environment */
    ZeroMemory(secBuf, sizeof(secBuf));
    BOOL ok4 = bIsSAT ? ReadAtaLogExtSAT(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_ENV_SEC, secBuf, sizeof(secBuf))
                      : ReadAtaLogExtNative(hDrive, ATA_LOG_SEAGATE_FARM, SEAGATE_FARM_PAGE_ENV_SEC, secBuf, sizeof(secBuf));
    if (ok4) ParseSeagateFarmEnvironment(secBuf, pInfo);
}

void AcquireAtaLogs(HANDLE hDrive, BOOL bIsUSB, DRIVE_INFO* pInfo)
{
    if (!hDrive || !pInfo) return;

    /* Initialize Log 11h (SATA PHY) fields */
    pInfo->bSataPhyLogSupported   = FALSE;
    pInfo->nPhyCommandIcrcErrors  = 0;
    pInfo->nPhyR_ErrDataFis       = 0;
    pInfo->nPhyTransitionsPhyRdy  = 0;
    pInfo->nPhyComresetCount      = 0;
    pInfo->nPhyCrcErrorsH2D       = 0;
    pInfo->nPhyNonCrcErrorsH2D    = 0;

    /* Initialize Log 04h (Device Statistics) fields */
    pInfo->bDevStatSupported      = FALSE;
    pInfo->nDevStatLifetimeMaxTempC = -1;
    pInfo->nDevStatLifetimeMinTempC = -1;
    pInfo->qwDevStatSectorsWritten = 0;
    pInfo->qwDevStatSectorsRead    = 0;
    pInfo->qwDevStatUncorrectable  = 0;
    pInfo->nDevStatPercentUsed     = -1;

    /* Initialize Seagate FARM fields */
    pInfo->bFarmLogSupported         = FALSE;
    pInfo->dwFarmMajorVersion        = 0;
    pInfo->dwFarmMinorVersion        = 0;
    pInfo->qwFarmPowerOnHours        = 0;
    pInfo->qwFarmSpindlePowerOnHours = 0;
    pInfo->qwFarmHeadFlightHours     = 0;
    pInfo->qwFarmPowerCycleCount     = 0;
    pInfo->qwFarmSectorsWritten      = 0;
    pInfo->qwFarmSectorsRead         = 0;
    pInfo->qwFarmReallocatedSectors  = 0;
    pInfo->nFarmLifetimeMaxTempC     = -1;
    pInfo->nFarmLifetimeMinTempC     = -1;
    pInfo->szFarmAssemblyDate[0]     = '\0';

    BYTE logBuf[512];

    /* 1. Read SATA PHY Event Counters (Log 11h, page 0) */
    ZeroMemory(logBuf, sizeof(logBuf));
    BOOL okPhy = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x11, 0, logBuf, sizeof(logBuf))
                        : ReadAtaLogExtNative(hDrive, 0x11, 0, logBuf, sizeof(logBuf));
    if (okPhy) {
        ParseSataPhyLog(logBuf, pInfo);
    }

    /* 2. Read ATA Device Statistics Log (Log 04h) */
    /* Check Directory / Supported Pages list at Page 00h */
    ZeroMemory(logBuf, sizeof(logBuf));
    BOOL okDir = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x04, 0x00, logBuf, sizeof(logBuf))
                        : ReadAtaLogExtNative(hDrive, 0x04, 0x00, logBuf, sizeof(logBuf));
    if (okDir && logBuf[2] == 0x00) {
        BYTE nEntries = logBuf[3];
        if (nEntries > 64) nEntries = 64;

        BOOL bHasGeneral = FALSE;
        BOOL bHasErrors  = FALSE;
        BOOL bHasTemp    = FALSE;
        BOOL bHasSSD     = FALSE;

        for (BYTE i = 0; i < nEntries && (8 + i) < 512; i++) {
            BYTE pg = logBuf[8 + i];
            if (pg == ATA_DEV_STAT_PAGE_GENERAL) bHasGeneral = TRUE;
            else if (pg == ATA_DEV_STAT_PAGE_ERRORS) bHasErrors = TRUE;
            else if (pg == ATA_DEV_STAT_PAGE_TEMP) bHasTemp = TRUE;
            else if (pg == ATA_DEV_STAT_PAGE_SSD) bHasSSD = TRUE;
        }

        if (bHasTemp) {
            ZeroMemory(logBuf, sizeof(logBuf));
            BOOL ok = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x04, ATA_DEV_STAT_PAGE_TEMP, logBuf, sizeof(logBuf))
                             : ReadAtaLogExtNative(hDrive, 0x04, ATA_DEV_STAT_PAGE_TEMP, logBuf, sizeof(logBuf));
            if (ok) ParseDeviceStatisticsPage(ATA_DEV_STAT_PAGE_TEMP, logBuf, pInfo);
        }

        if (bHasGeneral) {
            ZeroMemory(logBuf, sizeof(logBuf));
            BOOL ok = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x04, ATA_DEV_STAT_PAGE_GENERAL, logBuf, sizeof(logBuf))
                             : ReadAtaLogExtNative(hDrive, 0x04, ATA_DEV_STAT_PAGE_GENERAL, logBuf, sizeof(logBuf));
            if (ok) ParseDeviceStatisticsPage(ATA_DEV_STAT_PAGE_GENERAL, logBuf, pInfo);
        }

        if (bHasErrors) {
            ZeroMemory(logBuf, sizeof(logBuf));
            BOOL ok = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x04, ATA_DEV_STAT_PAGE_ERRORS, logBuf, sizeof(logBuf))
                             : ReadAtaLogExtNative(hDrive, 0x04, ATA_DEV_STAT_PAGE_ERRORS, logBuf, sizeof(logBuf));
            if (ok) ParseDeviceStatisticsPage(ATA_DEV_STAT_PAGE_ERRORS, logBuf, pInfo);
        }

        if (bHasSSD || pInfo->eType == DRIVE_TYPE_SSD_SATA) {
            ZeroMemory(logBuf, sizeof(logBuf));
            BOOL ok = bIsUSB ? ReadAtaLogExtSAT(hDrive, 0x04, ATA_DEV_STAT_PAGE_SSD, logBuf, sizeof(logBuf))
                             : ReadAtaLogExtNative(hDrive, 0x04, ATA_DEV_STAT_PAGE_SSD, logBuf, sizeof(logBuf));
            if (ok) ParseDeviceStatisticsPage(ATA_DEV_STAT_PAGE_SSD, logBuf, pInfo);
        }
    }

    /* 3. Query Seagate FARM Log (Log A6h) if supported */
    AcquireSeagateFarmLog(hDrive, bIsUSB, pInfo);
}
