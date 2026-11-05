/* DriveMonitor - Drive Self-Test (DST) and Hardware Error Logs.
 * GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dst_engine.h"
#include "smart_internal.h"
#include "csmi.h"
#include "safestr.h"
#include "lang.h"

#define APPEND_FORMAT(buf, maxLen, pos, fmt, ...) \
    do { \
        if ((pos) < (maxLen)) { \
            int _w_ = snprintf((buf) + (pos), (size_t)((maxLen) - (pos)), fmt, ##__VA_ARGS__); \
            if (_w_ > 0) { \
                (pos) += _w_; \
                if ((pos) >= (maxLen)) (pos) = (maxLen) - 1; \
            } \
        } \
    } while (0)

BOOL DstStartSelfTest(HANDLE hDrive, DRIVE_INFO* pInfo, DST_TEST_TYPE eType, char* szErr, int nErrLen)
{
    if (szErr && nErrLen > 0) szErr[0] = '\0';
    if (!pInfo) {
        if (szErr) safe_snprintf_n(szErr, nErrLen, "%s", TN("Диск не выбран", "No drive selected"));
        return FALSE;
    }

    if (pInfo->bIsNVMe) {
        /* NVMe Device Self-Test: 1 = Short, 2 = Extended */
        if (!NvmeSendDeviceSelfTest(hDrive, (DWORD)eType)) {
            if (szErr) safe_snprintf_n(szErr, nErrLen, "%s", TN("Контроллер NVMe отклонил команду самотестирования (0x14)", "NVMe controller rejected self-test command (0x14)"));
            return FALSE;
        }
        pInfo->bSelfTestRunning = TRUE;
        return TRUE;
    } else {
        /* ATA SMART EXECUTE OFF-LINE IMMEDIATE (0xB0 / 0xD4) */
        BYTE bSub = (eType == DST_TEST_SHORT) ? 0x01 : (eType == DST_TEST_EXTENDED ? 0x02 : 0x03);
        if (!AtaSendSmartOfflineImmediate(hDrive, pInfo, bSub)) {
            if (szErr) safe_snprintf_n(szErr, nErrLen, "%s", TN("Накопитель ATA не принял команду самотестирования SMART (0xD4)", "ATA drive rejected SMART self-test command (0xD4)"));
            return FALSE;
        }
        pInfo->bSelfTestRunning = TRUE;
        return TRUE;
    }
}

BOOL DstAbortSelfTest(HANDLE hDrive, DRIVE_INFO* pInfo, char* szErr, int nErrLen)
{
    if (szErr && nErrLen > 0) szErr[0] = '\0';
    if (!pInfo) return FALSE;

    if (pInfo->bIsNVMe) {
        if (!NvmeSendDeviceSelfTest(hDrive, 0)) {
            if (szErr) safe_snprintf_n(szErr, nErrLen, "%s", TN("Не удалось прервать тест NVMe", "Failed to abort NVMe self-test"));
            return FALSE;
        }
    } else {
        if (!AtaSendSmartOfflineImmediate(hDrive, pInfo, 0x00)) {
            if (szErr) safe_snprintf_n(szErr, nErrLen, "%s", TN("Не удалось прервать тест SMART", "Failed to abort SMART self-test"));
            return FALSE;
        }
    }
    pInfo->bSelfTestRunning = FALSE;
    return TRUE;
}

BOOL DstPollProgress(HANDLE hDrive, DRIVE_INFO* pInfo, DST_PROGRESS* pProgress)
{
    if (!pProgress) return FALSE;
    ZeroMemory(pProgress, sizeof(DST_PROGRESS));
    if (!pInfo) return FALSE;

    if (pInfo->bIsNVMe) {
        NVME_SELF_TEST_LOG log;
        ZeroMemory(&log, sizeof(log));
        DWORD dwCopied = 0;
        if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, 0x06,
                              (BYTE*)&log, (DWORD)sizeof(log), &dwCopied) && dwCopied >= 4) {
            pInfo->bGotNvmeSelfTestLog = TRUE;
            pInfo->nvmeSelfTestLog = log;

            if (log.CurrentOperation != 0) {
                pProgress->bRunning = TRUE;
                pProgress->nPercentComplete = (int)(log.CurrentCompletion & 0x7F);
                pProgress->eStatusCode = SELFTEST_ST_IN_PROGRESS;
                safe_snprintf(pProgress->szStatusText, TN("Выполняется проверка NVMe (%d%%)...", "NVMe self-test running (%d%%)..."), pProgress->nPercentComplete);
                return TRUE;
            } else {
                pProgress->bRunning = FALSE;
                pProgress->nPercentComplete = 100;
                BYTE res = (BYTE)(log.Results[0].Status & 0x0F);
                if (res == 0) {
                    pProgress->eStatusCode = SELFTEST_ST_OK;
                    safe_snprintf(pProgress->szStatusText, "%s", TN("Тест успешно завершён (ошибок нет)", "Test completed successfully (no error)"));
                } else if (res == 1) {
                    pProgress->eStatusCode = SELFTEST_ST_ABORTED;
                    safe_snprintf(pProgress->szStatusText, "%s", TN("Тест прерван хостом", "Test aborted by host"));
                } else if (res == 2) {
                    pProgress->eStatusCode = SELFTEST_ST_INTERRUPTED;
                    safe_snprintf(pProgress->szStatusText, "%s", TN("Тест прерван сбросом шины", "Test interrupted by reset"));
                } else {
                    pProgress->eStatusCode = SELFTEST_ST_FAILED_READ;
                    pProgress->nFailingLBA = (uint64_t)log.Results[0].FailingLBA;
                    safe_snprintf(pProgress->szStatusText, TN("Обнаружен сбой (код %d, LBA 0x%llX)", "Test failed (code %d, LBA 0x%llX)"), (int)res, (unsigned long long)log.Results[0].FailingLBA);
                }
                return TRUE;
            }
        }
        return FALSE;
    } else {
        /* ATA: read attribute sector or self-test log */
        if (GetSMARTSelfTestLog(hDrive, pInfo->nDriveIndex, pInfo) ||
            GetSMARTSelfTestLogATAPassthrough(hDrive, pInfo) ||
            GetSMARTSelfTestLogSAT(hDrive, pInfo)) {
            int idx = (pInfo->selfTestLog.bMostRecentTestIndex >= 1 && pInfo->selfTestLog.bMostRecentTestIndex <= 21)
                      ? (int)(pInfo->selfTestLog.bMostRecentTestIndex - 1) : 0;
            BYTE bSt = pInfo->selfTestLog.stEntries[idx].bStatusByte;
            int progress = 0;
            pProgress->eStatusCode = DecodeSelfTestStatus(bSt, &progress);
            if (pProgress->eStatusCode == SELFTEST_ST_IN_PROGRESS) {
                pProgress->bRunning = TRUE;
                pProgress->nPercentComplete = progress;
                safe_snprintf(pProgress->szStatusText, TN("Выполняется самопроверка SMART (%d%%)...", "SMART self-test in progress (%d%%)..."), progress);
            } else {
                pProgress->bRunning = FALSE;
                pProgress->nPercentComplete = 100;
                char desc[128];
                GetSelfTestStatusDesc(bSt, (unsigned long)pInfo->selfTestLog.stEntries[idx].dwFailingLBA, desc, (int)sizeof(desc));
                safe_snprintf(pProgress->szStatusText, "%s", desc);
            }
            return TRUE;
        }
        return FALSE;
    }
}

BOOL DstQueryAtaLogs(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (!pInfo) return FALSE;
    BOOL bLog1 = GetSMARTErrorLog(hDrive, pInfo->nDriveIndex, pInfo) ||
                 GetSMARTErrorLogATAPassthrough(hDrive, pInfo) ||
                 GetSMARTErrorLogSAT(hDrive, pInfo);
    BOOL bLog6 = GetSMARTSelfTestLog(hDrive, pInfo->nDriveIndex, pInfo) ||
                 GetSMARTSelfTestLogATAPassthrough(hDrive, pInfo) ||
                 GetSMARTSelfTestLogSAT(hDrive, pInfo);
    return bLog1 || bLog6;
}

BOOL DstQueryNvmeLogs(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (!pInfo || !pInfo->bIsNVMe) return FALSE;

    /* 1. Error Information Log (01h) */
    BYTE errBuf[512];
    ZeroMemory(errBuf, sizeof(errBuf));
    DWORD dwCopied = 0;
    if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, 0x01, errBuf, (DWORD)sizeof(errBuf), &dwCopied) &&
        dwCopied >= sizeof(NVME_ERROR_LOG_ENTRY)) {
        DWORD nEntries = dwCopied / (DWORD)sizeof(NVME_ERROR_LOG_ENTRY);
        if (nEntries > 8) nEntries = 8;
        pInfo->bGotNvmeErrorLog = TRUE;
        pInfo->dwNvmeErrorLogEntries = nEntries;
        memcpy(pInfo->nvmeErrorLog, errBuf, nEntries * sizeof(NVME_ERROR_LOG_ENTRY));
    }

    /* 2. Device Self-test Log (06h) */
    NVME_SELF_TEST_LOG selfLog;
    ZeroMemory(&selfLog, sizeof(selfLog));
    dwCopied = 0;
    if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, 0x06, (BYTE*)&selfLog, (DWORD)sizeof(selfLog), &dwCopied) &&
        dwCopied >= 4) {
        pInfo->bGotNvmeSelfTestLog = TRUE;
        pInfo->nvmeSelfTestLog = selfLog;
    }

    return pInfo->bGotNvmeErrorLog || pInfo->bGotNvmeSelfTestLog;
}

int DstFormatSelfTestHistory(const DRIVE_INFO* pInfo, char* szOut, int nOutLen)
{
    if (!szOut || nOutLen <= 0) return 0;
    szOut[0] = '\0';
    if (!pInfo) return 0;

    int pos = 0;
    APPEND_FORMAT(szOut, nOutLen, pos,
        "=== %s ===\r\n",
        TN("ЖУРНАЛ САМОПРОВЕРОК НАКОПИТЕЛЯ (SELF-TEST LOG)", "DRIVE SELF-TEST LOG"));

    if (pInfo->bIsNVMe) {
        if (!pInfo->bGotNvmeSelfTestLog) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Журнал самотестирования NVMe не поддерживается данным накопителем.",
                             "NVMe self-test log is not supported by this drive."));
            return pos;
        }

        int nValid = 0;
        for (int i = 0; i < 20; i++) {
            const NVME_SELF_TEST_RESULT* r = &pInfo->nvmeSelfTestLog.Results[i];
            if (r->Status != 0 || r->PowerOnHours != 0) nValid++;
        }

        if (nValid == 0) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n%s\r\n",
                TN("Журнал самопроверок NVMe пуст (тесты ещё не запускались на этом накопителе).",
                   "NVMe self-test log is empty (no tests have been run on this drive)."),
                TN("Для запуска диагностики выберите 'Короткий тест DST' или 'Расширенный тест' и нажмите 'Запустить'.",
                   "To run diagnostics, select 'Short DST' or 'Extended Test' and click 'Start'."));
            return pos;
        }

        APPEND_FORMAT(szOut, nOutLen, pos,
            "%-4s %-20s %-32s %-10s %-16s\r\n",
            "#", TN("Тип теста", "Test Type"), TN("Результат", "Result"),
            TN("POH (ч)", "POH (h)"), TN("LBA ошибки", "Failing LBA"));
        APPEND_FORMAT(szOut, nOutLen, pos,
            "--------------------------------------------------------------------------------\r\n");

        int nRow = 1;
        for (int i = 0; i < 20; i++) {
            const NVME_SELF_TEST_RESULT* r = &pInfo->nvmeSelfTestLog.Results[i];
            BYTE code = (BYTE)(r->Status & 0x0F);
            BYTE type = (BYTE)((r->Status >> 4) & 0x0F);
            if (r->Status == 0 && r->PowerOnHours == 0) continue;

            const char* szType = (type == 1) ? TN("Короткий DST", "Short DST") :
                                 ((type == 2) ? TN("Расширенный DST", "Extended DST") : TN("Вендорский", "Vendor"));
            const char* szRes = (code == 0) ? TN("Успешно: без ошибок", "Completed OK") :
                                ((code == 1) ? TN("Прерван хостом", "Aborted by Host") :
                                ((code == 2) ? TN("Прерван сбросом", "Interrupted by Reset") :
                                ((code == 3) ? TN("Фатальная ошибка", "Fatal Error") :
                                ((code == 7) ? TN("Ошибка чтения LBA", "Read Error (LBA)") : TN("Ошибка теста", "Failed")))));

            char szLba[32];
            if (code >= 3 && r->FailingLBA != 0) safe_snprintf(szLba, "0x%llX", (unsigned long long)r->FailingLBA);
            else safe_snprintf(szLba, "—");

            APPEND_FORMAT(szOut, nOutLen, pos,
                "%-4d %-20s %-32s %-10llu %-16s\r\n",
                nRow++, szType, szRes, (unsigned long long)r->PowerOnHours, szLba);
        }
    } else {
        if (!pInfo->bGotSelfTestLog) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Журнал самотестирования SMART не поддерживается накопителем или контроллером.",
                             "SMART self-test log is not supported by drive or controller."));
            return pos;
        }

        int nValid = 0;
        for (int i = 0; i < 21; i++) {
            const SMART_SELF_TEST_DESCRIPTOR* e = &pInfo->selfTestLog.stEntries[i];
            if (e->bStatusByte != 0 || e->wLifeTimestamp != 0 || e->bTestNumber != 0)
                nValid++;
        }

        if (nValid == 0) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n%s\r\n",
                TN("Журнал самопроверок SMART пуст (тесты ещё не запускались на этом накопителе).",
                   "SMART self-test log is empty (no tests have been run on this drive)."),
                TN("Для запуска диагностики выберите 'Короткий тест DST' или 'Расширенный тест' и нажмите 'Запустить'.",
                   "To run diagnostics, select 'Short DST' or 'Extended Test' and click 'Start'."));
            return pos;
        }

        APPEND_FORMAT(szOut, nOutLen, pos,
            "%-4s %-20s %-32s %-10s %-16s\r\n",
            "#", TN("Тип теста", "Test Type"), TN("Результат", "Result"),
            TN("POH (ч)", "POH (h)"), TN("LBA ошибки", "Failing LBA"));
        APPEND_FORMAT(szOut, nOutLen, pos,
            "--------------------------------------------------------------------------------\r\n");

        int nRow = 1;
        int startIdx = (pInfo->selfTestLog.bMostRecentTestIndex >= 1 && pInfo->selfTestLog.bMostRecentTestIndex <= 21)
                       ? (int)pInfo->selfTestLog.bMostRecentTestIndex : 0;

        for (int step = 0; step < 21; step++) {
            int i = (startIdx > 0) ? ((20 - step + startIdx) % 21) : step;
            const SMART_SELF_TEST_DESCRIPTOR* e = &pInfo->selfTestLog.stEntries[i];
            if (e->bStatusByte == 0 && e->wLifeTimestamp == 0 && e->bTestNumber == 0)
                continue;

            int progress = 0;
            SELFTEST_STATUS_CODE sc = DecodeSelfTestStatus(e->bStatusByte, &progress);
            const char* szBadge = GetSelfTestStatusBadge(e->bStatusByte);

            const char* szTestType;
            switch (e->bTestNumber & 0x7F) {
            case 0x01: szTestType = TN("Короткий DST", "Short DST"); break;
            case 0x02: szTestType = TN("Расширенный DST", "Extended DST"); break;
            case 0x03: szTestType = TN("Конвейерный", "Conveyance"); break;
            case 0x04: szTestType = TN("Выборочный", "Selective"); break;
            default:   szTestType = TN("SMART тест", "SMART Test"); break;
            }

            char szLba[32];
            if (sc >= SELFTEST_ST_FAILED_UNKNOWN && sc <= SELFTEST_ST_FAILED_SHOCK && e->dwFailingLBA != 0) {
                safe_snprintf(szLba, "0x%08X", (unsigned)e->dwFailingLBA);
            } else {
                safe_snprintf(szLba, "—");
            }

            APPEND_FORMAT(szOut, nOutLen, pos,
                "%-4d %-20s %-32s %-10u %-16s\r\n",
                nRow++, szTestType, szBadge, (unsigned)e->wLifeTimestamp, szLba);
        }
    }
    return pos;
}

int DstFormatErrorLogHistory(const DRIVE_INFO* pInfo, char* szOut, int nOutLen)
{
    if (!szOut || nOutLen <= 0) return 0;
    szOut[0] = '\0';
    if (!pInfo) return 0;

    int pos = 0;
    APPEND_FORMAT(szOut, nOutLen, pos,
        "\r\n=== %s ===\r\n",
        TN("ЖУРНАЛ АППАРАТНЫХ ОШИБОК (ERROR LOG)", "HARDWARE ERROR LOG"));

    if (pInfo->bIsNVMe) {
        if (!pInfo->bGotNvmeErrorLog) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Журнал аппаратных ошибок NVMe не поддерживается контроллером.",
                             "NVMe error log is not supported by controller."));
            return pos;
        }

        DWORD nRealErrors = 0;
        for (DWORD i = 0; i < pInfo->dwNvmeErrorLogEntries; i++) {
            if (pInfo->nvmeErrorLog[i].ErrorCount != 0) nRealErrors++;
        }

        if (nRealErrors == 0) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Аппаратных ошибок контроллера NVMe не зафиксировано (журнал чист).",
                             "No NVMe controller hardware errors recorded (log is clean)."));
            return pos;
        }

        APPEND_FORMAT(szOut, nOutLen, pos,
            "%-6s %-12s %-12s %-18s %-16s\r\n",
            "Error#", "SQID/CMDID", "StatusField", "Location (Byte/Bit)", "Failing LBA");
        APPEND_FORMAT(szOut, nOutLen, pos,
            "--------------------------------------------------------------------------------\r\n");
        for (DWORD i = 0; i < pInfo->dwNvmeErrorLogEntries; i++) {
            const NVME_ERROR_LOG_ENTRY* e = &pInfo->nvmeErrorLog[i];
            if (e->ErrorCount == 0) continue;

            APPEND_FORMAT(szOut, nOutLen, pos,
                "%-6llu %u/%u        0x%04X       Byte %-4u Bit %-4u  0x%llX\r\n",
                (unsigned long long)e->ErrorCount,
                (unsigned)e->SQID, (unsigned)e->CMDID,
                (unsigned)e->StatusField,
                (unsigned)(e->ParameterErrorLocation & 0x0FFF),
                (unsigned)((e->ParameterErrorLocation >> 12) & 0x07),
                (unsigned long long)e->LBA);
        }
    } else {
        if (!pInfo->bGotErrorLog) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Журнал аппаратных ошибок интерфейса ATA не поддерживается.",
                             "ATA hardware error log is not supported."));
            return pos;
        }
        if (pInfo->nErrorLogCount == 0) {
            APPEND_FORMAT(szOut, nOutLen, pos,
                "%s\r\n", TN("Аппаратных ошибок интерфейса ATA не зафиксировано (журнал чист).",
                             "No ATA hardware errors recorded (log is clean)."));
            return pos;
        }

        APPEND_FORMAT(szOut, nOutLen, pos,
            "%s %d.\r\n", TN("Суммарное количество ошибок ATA по журналу:", "Total ATA error log count:"), pInfo->nErrorLogCount);
        APPEND_FORMAT(szOut, nOutLen, pos,
            "%-4s %-10s %-10s %-16s %-16s\r\n",
            "#", "ErrorReg", "StatusReg", "LBA (Low/Mid/Hi)", "Timestamp");
        APPEND_FORMAT(szOut, nOutLen, pos,
            "--------------------------------------------------------------------------------\r\n");
        for (int i = 0; i < 5; i++) {
            const SMART_ERROR_DESCRIPTOR* d = &pInfo->errorLog.stErrors[i];
            if (d->bErrorRegister == 0 && d->bStatus == 0) continue;

            DWORD dwLba = (DWORD)d->bLBA_Low | ((DWORD)d->bLBA_Mid << 8) | ((DWORD)d->bLBA_High << 16);
            WORD ts = (WORD)((WORD)d->bTimestamp[0] | ((WORD)d->bTimestamp[1] << 8));

            APPEND_FORMAT(szOut, nOutLen, pos,
                "%-4d 0x%02X       0x%02X       0x%08X         %u ms\r\n",
                i + 1, (unsigned)d->bErrorRegister, (unsigned)d->bStatus, (unsigned)dwLba, (unsigned)ts);
        }
    }
    return pos;
}
