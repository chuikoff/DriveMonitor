/* DriveMonitor - Counterfeit and Relabeled Drive Detector. GPL-2.0-or-later. See LICENSE. */
#include "counterfeit_detect.h"
#include "smart_internal.h"
#include "safestr.h"
#include "lang.h"
#include <ctype.h>
#include <string.h>

static BOOL IsWdModel(const char* szModelU)
{
    if (!szModelU || !szModelU[0]) return FALSE;
    return (strstr(szModelU, "WDC") != NULL ||
            strstr(szModelU, "WD ") != NULL ||
            strstr(szModelU, "WESTERN") != NULL ||
            strncmp(szModelU, "WD", 2) == 0);
}

static BOOL IsWdConsumer5400Model(const char* szModelU)
{
    if (!IsWdModel(szModelU)) return FALSE;
    /* PURX = WD Purple 5400 RPM surveillance
     * EZRZ / EZAZ = WD Blue 5400 RPM desktop
     * EFRX / EFZX = WD Red 5400 RPM NAS (Red Pro is FFBX 7200 RPM)
     * EARX / EZRX / EARS = WD Green 5400 RPM */
    if (strstr(szModelU, "PURX") || strstr(szModelU, "EZRZ") ||
        strstr(szModelU, "EZAZ") || strstr(szModelU, "EFRX") ||
        strstr(szModelU, "EARX") || strstr(szModelU, "EZRX") ||
        strstr(szModelU, "EARS"))
        return TRUE;
    return FALSE;
}

static BOOL IsWdConsumerModel(const char* szModelU)
{
    if (!IsWdModel(szModelU)) return FALSE;
    if (IsWdConsumer5400Model(szModelU)) return TRUE;
    if (strstr(szModelU, "PURZ") || strstr(szModelU, "EZEX"))
        return TRUE;
    return FALSE;
}

static BOOL IsSeagateFirmwarePattern(const char* fwU)
{
    if (!fwU || !fwU[0]) return FALSE;
    /* Seagate firmware families: 4 chars typically, or common Seagate prefixes */
    if (strncmp(fwU, "SN0", 3) == 0 || strncmp(fwU, "SC6", 3) == 0 ||
        strncmp(fwU, "CC4", 3) == 0 || strncmp(fwU, "CC3", 3) == 0 ||
        strncmp(fwU, "CC2", 3) == 0 || strncmp(fwU, "CC1", 3) == 0 ||
        strncmp(fwU, "CC5", 3) == 0 || strncmp(fwU, "CC6", 3) == 0 ||
        strncmp(fwU, "GX4", 3) == 0 || strncmp(fwU, "TN0", 3) == 0 ||
        strncmp(fwU, "AN0", 3) == 0 || strncmp(fwU, "SD0", 3) == 0 ||
        strncmp(fwU, "LC1", 3) == 0 || strncmp(fwU, "EB0", 3) == 0 ||
        strncmp(fwU, "DE1", 3) == 0 || strncmp(fwU, "JC4", 3) == 0 ||
        strncmp(fwU, "SC4", 3) == 0 || strncmp(fwU, "SC5", 3) == 0)
        return TRUE;
    return FALSE;
}

static BOOL IsHgstFirmwarePattern(const char* fwU)
{
    if (!fwU || !fwU[0]) return FALSE;
    /* HGST / Hitachi microcode: 00.02C*, 00.02A*, JA0OA*, A180, A7J0, AH01, MA40 */
    if (strncmp(fwU, "00.02C", 6) == 0 || strncmp(fwU, "00.02A", 6) == 0 ||
        strncmp(fwU, "JA0OA", 5) == 0 || strncmp(fwU, "A180", 4) == 0 ||
        strncmp(fwU, "A7J0", 4) == 0 || strncmp(fwU, "AH01", 4) == 0 ||
        strncmp(fwU, "MA40", 4) == 0)
        return TRUE;
    return FALSE;
}

static BOOL IsWdFirmwarePattern(const char* fwU)
{
    if (!fwU || strlen(fwU) != 8) return FALSE;
    /* Format: 80.00A80, 82.00A82, 01.01A01, 51.00A51, etc. */
    if (isdigit((unsigned char)fwU[0]) && isdigit((unsigned char)fwU[1]) &&
        fwU[2] == '.' &&
        isdigit((unsigned char)fwU[3]) && isdigit((unsigned char)fwU[4]) &&
        isalpha((unsigned char)fwU[5]) &&
        isdigit((unsigned char)fwU[6]) && isdigit((unsigned char)fwU[7]))
        return TRUE;
    return FALSE;
}

static BOOL ModelLooksSamsung(const char* szModelU)
{
    if (!szModelU || !szModelU[0]) return FALSE;
    if (strstr(szModelU, "SAMSUNG") || strstr(szModelU, "MZ-") ||
        (szModelU[0] == 'M' && szModelU[1] == 'Z' && szModelU[2] >= 'A' && szModelU[2] <= 'Z') ||
        strstr(szModelU, "870 EVO") || strstr(szModelU, "860 EVO") ||
        strstr(szModelU, "850 EVO") || strstr(szModelU, "870 QVO") ||
        strstr(szModelU, "860 QVO") || strstr(szModelU, "980 PRO") ||
        strstr(szModelU, "970 EVO") || strstr(szModelU, "990 PRO") ||
        strstr(szModelU, "PM981") || strstr(szModelU, "PM9A1") ||
        strstr(szModelU, "SM961"))
        return TRUE;
    return FALSE;
}

void DetectCounterfeitMismatch(DRIVE_INFO* pInfo)
{
    if (!pInfo) return;
    pInfo->bCounterfeitSuspected = FALSE;
    pInfo->nAnomalyType = ANOMALY_NONE;
    pInfo->szCounterfeitReason[0] = '\0';
    pInfo->szCounterfeitReasonEn[0] = '\0';

    /* Generic MPTool / All-zeros placeholder serial check */
    if (pInfo->szSerial[0]) {
        size_t sLen = strlen(pInfo->szSerial);
        size_t zeroCount = 0;
        for (size_t i = 0; i < sLen; i++) {
            if (pInfo->szSerial[i] == '0') zeroCount++;
        }
        if (sLen >= 6 && zeroCount == sLen) {
            pInfo->bCounterfeitSuspected = TRUE;
            pInfo->nAnomalyType = ANOMALY_REFLASHED_REPAIRED;
            safe_snprintf(pInfo->szCounterfeitReason,
                "Недостоверный серийный номер: серийный номер (%s) занулён (признак сброса или сервисной перепрошивки)",
                pInfo->szSerial);
            safe_snprintf(pInfo->szCounterfeitReasonEn,
                "Invalid serial number: drive serial (%s) is all zeros (wiped or flashed firmware signature for recovery)",
                pInfo->szSerial);
            return;
        }

        if (strncmp(pInfo->szSerial, "AA00000000", 10) == 0 ||
            strncmp(pInfo->szSerial, "aa00000000", 10) == 0 ||
            strncmp(pInfo->szSerial, "0123456789", 10) == 0 ||
            strncmp(pInfo->szSerial, "1234567890", 10) == 0) {
            pInfo->bCounterfeitSuspected = TRUE;
            pInfo->nAnomalyType = ANOMALY_REFLASHED_REPAIRED;
            safe_snprintf(pInfo->szCounterfeitReason,
                "Признак сервисной перепрошивки/восстановления: серийный номер (%s) является шаблоном MPTool (диск мог быть перепрошит после сбоя)",
                pInfo->szSerial);
            safe_snprintf(pInfo->szCounterfeitReasonEn,
                "Reflashed/repaired drive signature: serial number (%s) is a default MPTool flasher template (may have been recovered after firmware crash)",
                pInfo->szSerial);
            return;
        }
    }

    if (pInfo->eVendor == VENDOR_UTANIA) return;

    char szModelU[64];
    char szFwU[32];
    ToUpperCopy(szModelU, sizeof(szModelU), pInfo->szModel);
    ToUpperCopy(szFwU, sizeof(szFwU), pInfo->szFirmware);

    /* 1. NVMe PCI Vendor ID Mismatch */
    if (pInfo->bIsNVMe || pInfo->bGotNVMeIdent) {
        USHORT vid = NvmeIdentVid(pInfo);
        if (vid != 0) {
            if (ModelLooksSamsung(szModelU) || pInfo->eVendor == VENDOR_SAMSUNG) {
                if (vid != 0x144D) {
                    pInfo->bCounterfeitSuspected = TRUE;
                    pInfo->nAnomalyType = ANOMALY_COUNTERFEIT;
                    safe_snprintf(pInfo->szCounterfeitReason,
                        "Поддельный Samsung NVMe: аппаратный PCI VID (0x%04X) не принадлежит Samsung (0x144D)",
                        vid);
                    safe_snprintf(pInfo->szCounterfeitReasonEn,
                        "Counterfeit Samsung NVMe: hardware PCI VID (0x%04X) does not belong to Samsung (0x144D)",
                        vid);
                    return;
                }
            } else if (strstr(szModelU, "WD_BLACK") || strstr(szModelU, "SN850") ||
                       strstr(szModelU, "SN770") || strstr(szModelU, "SN570") ||
                       strstr(szModelU, "SN580") ||
                       (IsWdModel(szModelU) && strstr(szModelU, "WDS"))) {
                if (vid != 0x15B7 && vid != 0x1B96 && vid != 0x1B4B) {
                    if (vid == 0x10EC || vid == 0x126F || vid == 0x1E4B ||
                        vid == 0x1E49 || vid == 0x1DDC || vid == 0x1987) {
                        pInfo->bCounterfeitSuspected = TRUE;
                        pInfo->nAnomalyType = ANOMALY_COUNTERFEIT;
                        safe_snprintf(pInfo->szCounterfeitReason,
                            "Поддельный WD NVMe: аппаратный PCI VID (0x%04X) не принадлежит WD/SanDisk",
                            vid);
                        safe_snprintf(pInfo->szCounterfeitReasonEn,
                            "Counterfeit WD NVMe: hardware PCI VID (0x%04X) does not belong to Western Digital/SanDisk",
                            vid);
                        return;
                    }
                }
            } else if (strstr(szModelU, "CRUCIAL") || strstr(szModelU, "CT1000P") ||
                       strstr(szModelU, "CT2000P") || strstr(szModelU, "CT500P")) {
                if (vid != 0x1344 && vid != 0xC0A9 && vid != 0x1987 && vid != 0x1D97 && vid != 0x126F) {
                    if (vid == 0x10EC || vid == 0x1E4B || vid == 0x1E49 || vid == 0x1DDC) {
                        pInfo->bCounterfeitSuspected = TRUE;
                        pInfo->nAnomalyType = ANOMALY_COUNTERFEIT;
                        safe_snprintf(pInfo->szCounterfeitReason,
                            "Поддельный Crucial NVMe: аппаратный PCI VID (0x%04X) не принадлежит Crucial/Micron",
                            vid);
                        safe_snprintf(pInfo->szCounterfeitReasonEn,
                            "Counterfeit Crucial NVMe: hardware PCI VID (0x%04X) does not belong to Crucial/Micron",
                            vid);
                        return;
                    }
                }
            } else if (strstr(szModelU, "INTEL SSD") || strstr(szModelU, "SOLIDIGM")) {
                if (vid != 0x8086 && vid != 0x025E && vid != 0x126F) {
                    pInfo->bCounterfeitSuspected = TRUE;
                    pInfo->nAnomalyType = ANOMALY_COUNTERFEIT;
                    safe_snprintf(pInfo->szCounterfeitReason,
                        "Поддельный Intel NVMe: аппаратный PCI VID (0x%04X) не принадлежит Intel/Solidigm",
                        vid);
                    safe_snprintf(pInfo->szCounterfeitReasonEn,
                        "Counterfeit Intel NVMe: hardware PCI VID (0x%04X) does not belong to Intel/Solidigm",
                        vid);
                    return;
                }
            }
        }
    }

    /* 2. SATA SSD Controller / Firmware Contradictions */
    if (!pInfo->bIsNVMe && (pInfo->eType == DRIVE_TYPE_SSD_SATA || pInfo->eType == DRIVE_TYPE_M2_SATA ||
        (pInfo->wRotationRate == 0x0001 && !DriveIsHdd(pInfo)))) {
        if (ModelLooksSamsung(szModelU) || pInfo->eVendor == VENDOR_SAMSUNG) {
            if (pInfo->eController == CONTROLLER_REALTEK || pInfo->eController == CONTROLLER_MAXIO ||
                pInfo->eController == CONTROLLER_YEESTOR || pInfo->eController == CONTROLLER_SMI) {
                pInfo->bCounterfeitSuspected = TRUE;
                safe_snprintf(pInfo->szCounterfeitReason,
                    "Поддельный Samsung SSD: контроллер (%s) не является оригинальным Samsung",
                    DriveControllerLabel(pInfo));
                safe_snprintf(pInfo->szCounterfeitReasonEn,
                    "Counterfeit Samsung SSD: controller (%s) is not an original Samsung MCU",
                    DriveControllerLabel(pInfo));
                return;
            }
            if (szFwU[0]) {
                if (strncmp(szFwU, "U0", 2) == 0 || strncmp(szFwU, "VC0", 3) == 0 ||
                    strncmp(szFwU, "SN1", 3) == 0 || strncmp(szFwU, "EH", 2) == 0 ||
                    strncmp(szFwU, "W0", 2) == 0 || strncmp(szFwU, "S0", 2) == 0 ||
                    strncmp(szFwU, "SBF", 3) == 0 || strstr(szFwU, "2258") || strstr(szFwU, "2259")) {
                    pInfo->bCounterfeitSuspected = TRUE;
                    pInfo->nAnomalyType = ANOMALY_COUNTERFEIT;
                    safe_snprintf(pInfo->szCounterfeitReason,
                        "Поддельный Samsung SSD: прошивка (%s) принадлежит стороннему контроллеру (Realtek/SMI)",
                        pInfo->szFirmware);
                    safe_snprintf(pInfo->szCounterfeitReasonEn,
                        "Counterfeit Samsung SSD: firmware (%s) belongs to a third-party controller (Realtek/SMI)",
                        pInfo->szFirmware);
                    return;
                }
            }
        }
    }

    /* 3. Seagate FARM Telemetry Discrepancy (wiped / refurbished drive detection) */
    if (pInfo->bFarmLogSupported && pInfo->qwFarmPowerOnHours > 0) {
        if (pInfo->dwPowerOnHours >= 0 &&
            pInfo->qwFarmPowerOnHours > (unsigned __int64)pInfo->dwPowerOnHours + 100) {
            pInfo->bCounterfeitSuspected = TRUE;
            pInfo->nAnomalyType = ANOMALY_SMART_WIPED;
            safe_snprintf(pInfo->szCounterfeitReason,
                "Сброшенный SMART (б/у диск): реальная наработка по аппаратному FARM-логу составляет %llu ч, а в SMART заявлено всего %lu ч (счётчики сброшены перед перепродажей)",
                (unsigned long long)pInfo->qwFarmPowerOnHours, (unsigned long)pInfo->dwPowerOnHours);
            safe_snprintf(pInfo->szCounterfeitReasonEn,
                "Wiped SMART (refurbished drive): hardware FARM log records %llu hours, but SMART claims only %lu hours (runtime counters reset before resale)",
                (unsigned long long)pInfo->qwFarmPowerOnHours, (unsigned long)pInfo->dwPowerOnHours);
            return;
        }

        if (pInfo->qwFarmPowerCycleCount > 0 && pInfo->dwPowerCycleCount > 0 &&
            pInfo->qwFarmPowerCycleCount > (unsigned __int64)pInfo->dwPowerCycleCount + 50 &&
            pInfo->dwPowerCycleCount <= 5) {
            pInfo->bCounterfeitSuspected = TRUE;
            pInfo->nAnomalyType = ANOMALY_SMART_WIPED;
            safe_snprintf(pInfo->szCounterfeitReason,
                "Сброшенный SMART: циклов включения по FARM-логу %llu, а в SMART всего %lu (признак сброса б/у диска)",
                (unsigned long long)pInfo->qwFarmPowerCycleCount, (unsigned long)pInfo->dwPowerCycleCount);
            safe_snprintf(pInfo->szCounterfeitReasonEn,
                "Wiped SMART: FARM log records %llu power cycles, but SMART claims only %lu (wiped used drive signature)",
                (unsigned long long)pInfo->qwFarmPowerCycleCount, (unsigned long)pInfo->dwPowerCycleCount);
            return;
        }
    }

    /* 4. HDD Vendor / Model / Firmware Contradictions */
    if (DriveIsHdd(pInfo) || pInfo->eType == DRIVE_TYPE_HDD || pInfo->wRotationRate >= 0x0401) {
        if (IsWdModel(szModelU)) {
            if (IsSeagateFirmwarePattern(szFwU)) {
                pInfo->bCounterfeitSuspected = TRUE;
                pInfo->nAnomalyType = ANOMALY_RELABELED;
                safe_snprintf(pInfo->szCounterfeitReason,
                    "Несоответствие платформы: модель заявлена как WD (%s), но прошивка (%s) — Seagate",
                    pInfo->szModel, pInfo->szFirmware);
                safe_snprintf(pInfo->szCounterfeitReasonEn,
                    "Platform mismatch: model claims WD (%s), but firmware (%s) is Seagate microcode",
                    pInfo->szModel, pInfo->szFirmware);
                return;
            }
            if (IsWdConsumerModel(szModelU) && IsHgstFirmwarePattern(szFwU)) {
                pInfo->bCounterfeitSuspected = TRUE;
                pInfo->nAnomalyType = ANOMALY_RELABELED;
                safe_snprintf(pInfo->szCounterfeitReason,
                    "Ремаркированный накопитель: потребительская модель WD (%s) имеет прошивку серверного HGST (%s)",
                    pInfo->szModel, pInfo->szFirmware);
                safe_snprintf(pInfo->szCounterfeitReasonEn,
                    "Relabeled drive: consumer WD model (%s) has server HGST firmware (%s)",
                    pInfo->szModel, pInfo->szFirmware);
                return;
            }
            if (IsWdConsumer5400Model(szModelU) && pInfo->wRotationRate >= 7000) {
                pInfo->bCounterfeitSuspected = TRUE;
                pInfo->nAnomalyType = ANOMALY_RELABELED;
                safe_snprintf(pInfo->szCounterfeitReason,
                    "Физическое несоответствие: модель %s (5400 RPM) имеет мотор %u RPM (признак серверного диска)",
                    pInfo->szModel, (unsigned)pInfo->wRotationRate);
                safe_snprintf(pInfo->szCounterfeitReasonEn,
                    "Physical mismatch: 5400 RPM model %s physically spins at %u RPM (server drive signature)",
                    pInfo->szModel, (unsigned)pInfo->wRotationRate);
                return;
            }
        } else if (ModelLooksSeagate(szModelU)) {
            if (IsWdFirmwarePattern(szFwU) || IsHgstFirmwarePattern(szFwU)) {
                pInfo->bCounterfeitSuspected = TRUE;
                pInfo->nAnomalyType = ANOMALY_RELABELED;
                safe_snprintf(pInfo->szCounterfeitReason,
                    "Несоответствие платформы: модель заявлена как Seagate (%s), но прошивка (%s) — WD/HGST",
                    pInfo->szModel, pInfo->szFirmware);
                safe_snprintf(pInfo->szCounterfeitReasonEn,
                    "Platform mismatch: model claims Seagate (%s), but firmware (%s) is WD/HGST microcode",
                    pInfo->szModel, pInfo->szFirmware);
                return;
            }
        }
    }
}

const char* GetCounterfeitReason(const DRIVE_INFO* pInfo)
{
    if (!pInfo || !pInfo->bCounterfeitSuspected) return "";
    return UiLangIsEn() ? pInfo->szCounterfeitReasonEn : pInfo->szCounterfeitReason;
}

const char* GetCounterfeitBadge(const DRIVE_INFO* pInfo)
{
    if (!pInfo || !pInfo->bCounterfeitSuspected) return "";
    switch (pInfo->nAnomalyType) {
    case ANOMALY_REFLASHED_REPAIRED:
        return TN("⚠️ [ПЕРЕПРОШИТ / MPTOOL]", "⚠️ [REFLASHED / MPTOOL]");
    case ANOMALY_SMART_WIPED:
        return TN("⚠️ [СБРОШЕН SMART]", "⚠️ [WIPED SMART]");
    case ANOMALY_RELABELED:
        return TN("⚠️ [РЕМАРК]", "⚠️ [RELABELED]");
    case ANOMALY_COUNTERFEIT:
        return TN("⚠️ [ПОДДЕЛКА]", "⚠️ [COUNTERFEIT]");
    default:
        break;
    }
    return TN("⚠️ [ПОДДЕЛКА / РЕМАРК]", "⚠️ [FAKE / RELABELED]");
}
