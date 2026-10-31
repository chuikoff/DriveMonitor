/* DriveMonitor - DRAM Buffer Integrity & Silent Data Corruption (Bit Flip) Test.
 * GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "dram_verify.h"
#include "safestr.h"
#include "lang.h"
#include "utf8ui.h"

static DRAM_VERIFY_RESULT s_DramCache[32];
static bool s_DramCacheValid[32];

void ClearDriveDramVerifyResults(void)
{
    memset(s_DramCache, 0, sizeof(s_DramCache));
    memset(s_DramCacheValid, 0, sizeof(s_DramCacheValid));
}

void SetDriveDramVerifyResult(int nDriveIndex, const DRAM_VERIFY_RESULT* pResult)
{
    if (nDriveIndex < 0 || nDriveIndex >= 32 || !pResult) return;
    s_DramCache[nDriveIndex] = *pResult;
    s_DramCacheValid[nDriveIndex] = true;
}

const DRAM_VERIFY_RESULT* GetDriveDramVerifyResult(int nDriveIndex)
{
    if (nDriveIndex < 0 || nDriveIndex >= 32 || !s_DramCacheValid[nDriveIndex])
        return NULL;
    return &s_DramCache[nDriveIndex];
}

const char* DramPatternName(DRAM_PATTERN_TYPE ePattern)
{
    switch (ePattern) {
    case DRAM_PATTERN_PRBS:
        return TN("Псевдослучайный PRBS (высокая энтропия)", "Pseudo-Random PRBS (high entropy)");
    case DRAM_PATTERN_CHECKERBOARD:
        return TN("Шахматный 0xAA/0x55 (макс. наводки шины)", "Checkerboard 0xAA/0x55 (max crosstalk)");
    case DRAM_PATTERN_WALKING_BITS:
        return TN("Бегущая единица/ноль (линии данных)", "Walking 1s/0s (data lines)");
    case DRAM_PATTERN_SOLID_INV:
        return TN("Сплошная инверсия 0x00/0xFF (утечка заряда)", "Solid Inversion 0x00/0xFF (charge leakage)");
    default:
        return TN("Неизвестный паттерн", "Unknown pattern");
    }
}

void DramFillPattern(void* pBuffer, size_t nBytes, uint64_t nBlockOffset, DRAM_PATTERN_TYPE ePattern)
{
    if (!pBuffer || nBytes == 0) return;

    switch (ePattern) {
    case DRAM_PATTERN_PRBS:
    {
        uint64_t* pWords = (uint64_t*)pBuffer;
        size_t nWords = nBytes / sizeof(uint64_t);
        for (size_t i = 0; i < nWords; i++) {
            uint64_t x = 0x9E3779B97F4A7C15ULL ^ (nBlockOffset + (uint64_t)(i * sizeof(uint64_t)));
            x ^= x >> 12;
            x ^= x << 25;
            x ^= x >> 27;
            x *= 0x2545F4914F6CDD1DULL;
            pWords[i] = x;
        }
        break;
    }

    case DRAM_PATTERN_CHECKERBOARD:
    {
        uint64_t* pWords = (uint64_t*)pBuffer;
        size_t nWords = nBytes / sizeof(uint64_t);
        for (size_t i = 0; i < nWords; i++) {
            pWords[i] = ((i & 1) == 0) ? 0xAAAAAAAAAAAAAAAAULL : 0x5555555555555555ULL;
        }
        break;
    }

    case DRAM_PATTERN_WALKING_BITS:
    {
        static const uint8_t kWalk[16] = {
            0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80,
            0xFE, 0xFD, 0xFB, 0xF7, 0xEF, 0xDF, 0xBF, 0x7F
        };
        uint8_t* pBytes = (uint8_t*)pBuffer;
        for (size_t i = 0; i < nBytes; i++) {
            pBytes[i] = kWalk[(nBlockOffset + i) % 16];
        }
        break;
    }

    case DRAM_PATTERN_SOLID_INV:
    {
        uint8_t* pBytes = (uint8_t*)pBuffer;
        size_t nPages = nBytes / 4096;
        for (size_t p = 0; p < nPages; p++) {
            uint64_t pageIdx = (nBlockOffset / 4096) + p;
            uint8_t val = ((pageIdx & 1) == 0) ? 0x00 : 0xFF;
            memset(pBytes + p * 4096, val, 4096);
        }
        break;
    }

    default:
        memset(pBuffer, 0x5A, nBytes);
        break;
    }
}

size_t DramVerifyBuffer(const void* pActual,
                        const void* pExpected,
                        size_t nBytes,
                        uint64_t nBaseOffset,
                        DRAM_VERIFY_RESULT* pResult)
{
    if (memcmp(pActual, pExpected, nBytes) == 0) {
        return 0;
    }

    const uint8_t* pA = (const uint8_t*)pActual;
    const uint8_t* pE = (const uint8_t*)pExpected;
    size_t nCorrupted = 0;

    for (size_t i = 0; i < nBytes; i++) {
        if (pA[i] != pE[i]) {
            nCorrupted++;
            uint8_t bXor = (uint8_t)(pA[i] ^ pE[i]);
            uint32_t flips = DramCountFlippedBits(bXor);
            if (pResult) {
                pResult->nCorruptedBytes++;
                pResult->nTotalBitFlips += flips;
                if (pResult->nRecordedFlips < DRAM_MAX_RECORDED_FLIPS) {
                    DRAM_BITFLIP_SAMPLE* s = &pResult->flips[pResult->nRecordedFlips++];
                    s->nByteOffset = nBaseOffset + (uint64_t)i;
                    s->bExpected = pE[i];
                    s->bActual = pA[i];
                    s->bFlippedBits = bXor;
                }
            }
        }
    }
    return nCorrupted;
}

void DramVerifyAssessVerdict(DRAM_VERIFY_RESULT* pResult)
{
    if (!pResult) return;

    if (pResult->bAborted) {
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ПРЕРВАНО", "ABORTED"));
        safe_snprintf(pResult->szVerdictText, "%s",
            TN("Тестирование буфера DRAM было остановлено пользователем до завершения.",
               "DRAM buffer test was aborted by user before completion."));
        pResult->bPassed = false;
        return;
    }

    if (pResult->dwWin32Error != 0) {
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ОШИБКА ДОСТУПА", "I/O ERROR"));
        if (pResult->dwWin32Error == ERROR_ACCESS_DENIED) {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Отказано в доступе при прямом тестировании (требуются права Администратора).",
                   "Access denied during unbuffered verification (Administrator rights required)."));
        } else if (pResult->dwWin32Error == ERROR_DISK_FULL) {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Недостаточно свободного места на выбранном томе для проведения теста.",
                   "Insufficient free disk space on selected volume for the test."));
        } else {
            safe_snprintf(pResult->szVerdictText, "%s (Win32: %lu)",
                TN("Ошибка ввода-вывода при тестировании файла",
                   "I/O error during file verification test"),
                (unsigned long)pResult->dwWin32Error);
        }
        pResult->bPassed = false;
        return;
    }

    if (pResult->nCorruptedBytes == 0) {
        pResult->bPassed = true;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("БУФЕР ИСПРАВЕН", "BUFFER HEALTHY"));
        safe_snprintf(pResult->szVerdictText, "%s",
            TN("✓ Все проверочные блоки успешно записаны и сверены сквозным методом (Read-After-Write) без единой ошибки.\r\n"
               "Сбоев оперативной памяти DRAM, битовых инверсий (Bit Flips) и паразитных наводок на шине не обнаружено.\r\n"
               "Контроллер накопителя и буферная память работают абсолютно стабильно.",
               "✓ All test blocks successfully written and verified end-to-end (Read-After-Write) with zero errors.\r\n"
               "No DRAM memory faults, bit flips, or bus crosstalk detected.\r\n"
               "Drive controller and write buffer are completely healthy."));
    } else {
        pResult->bPassed = false;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("СБОЙ БУФЕРА (BIT FLIP)", "BUFFER FAILURE (BIT FLIP)"));
        safe_snprintf(pResult->szVerdictText,
            TN("⚠️ КРИТИЧЕСКИЙ ДЕФЕКТ: Обнаружено скрытое искажение данных (Silent Data Corruption / Bit Flips)!\r\n"
               "Зафиксировано %llu искажённых байт (%llu битовых ошибок) при прямой записи и чтении стресс-паттернов.\r\n"
               "Микросхема буфера DRAM или контроллер накопителя повреждены (деградация памяти, перегрев) и портят данные на лету.\r\n"
               "В SMART это не отражается, но файлы на диске тихо разрушаются. Накопитель подлежит немедленной замене!",
               "⚠️ CRITICAL DEFECT: Silent Data Corruption / Bit Flips detected!\r\n"
               "Recorded %llu corrupted bytes (%llu bit flips) during direct Read-After-Write stress verification.\r\n"
               "DRAM buffer chip or controller memory path is damaged (thermal degradation, bad cells) and corrupts data in flight.\r\n"
               "SMART does not reflect this error, but files are quietly destroyed. Drive must be replaced immediately!"),
            (unsigned long long)pResult->nCorruptedBytes,
            (unsigned long long)pResult->nTotalBitFlips);
    }
}

void FormatDramVerifySummary(const DRAM_VERIFY_RESULT* pResult,
                             const char* pszModel,
                             const char* pszSerial,
                             char* pszBuf,
                             size_t nBufSize)
{
    if (!pResult || !pszBuf || nBufSize == 0) return;

    double mbTested = (double)pResult->nBytesTested / (1024.0 * 1024.0);
    const char* szModeStr = (pResult->eMode == DRAM_VERIFY_MODE_EXPRESS)  ? TN("Экспресс (32 МБ)", "Express (32 MB)") :
                            (pResult->eMode == DRAM_VERIFY_MODE_STANDARD) ? TN("Стандарт (128 МБ)", "Standard (128 MB)") :
                                                                           TN("Стресс (512 МБ)", "Stress (512 MB)");

    char szFlipsList[2048] = "";
    if (pResult->nRecordedFlips > 0) {
        safe_snprintf(szFlipsList, "%s\r\n", TN("  Список зафиксированных битовых сбоев:", "  Recorded bit flips list:"));
        for (int i = 0; i < pResult->nRecordedFlips && i < DRAM_MAX_RECORDED_FLIPS; i++) {
            char line[128];
            const DRAM_BITFLIP_SAMPLE* s = &pResult->flips[i];
            safe_snprintf(line, "    [%02d] Смещение: 0x%llX | Ожидалось: 0x%02X | Прочитано: 0x%02X | Инверсия: 0x%02X\r\n",
                i + 1, (unsigned long long)s->nByteOffset, s->bExpected, s->bActual, s->bFlippedBits);
            size_t curLen = strlen(szFlipsList);
            if (curLen + strlen(line) < sizeof(szFlipsList) - 1) {
                strcat(szFlipsList, line);
            }
        }
    }

    safe_snprintf_n(pszBuf, nBufSize,
        "============================================================\r\n"
        "%s\r\n"
        "============================================================\r\n"
        "%s: %s\r\n"
        "%s: %s\r\n"
        "%s: %s\r\n"
        "%s: %s\r\n"
        "%s: %.1f MB\r\n"
        "%s: %.2f s\r\n"
        "%s: %.1f MB/s\r\n"
        "%s: %.1f MB/s\r\n"
        "%s: %llu\r\n"
        "%s: %llu\r\n"
        "%s"
        "------------------------------------------------------------\r\n"
        "%s: [%s]\r\n"
        "%s\r\n"
        "============================================================\r\n",
        TN("ОТЧЁТ ТЕСТИРОВАНИЯ БУФЕРА ПАМЯТИ DRAM (READ-AFTER-WRITE)",
           "DRAM BUFFER INTEGRITY & BIT FLIP VERIFICATION REPORT"),
        TN("Модель накопителя", "Drive model"), pszModel ? pszModel : "Unknown",
        TN("Серийный номер", "Serial number"), pszSerial ? pszSerial : "Unknown",
        TN("Режим теста", "Test mode"), szModeStr,
        TN("Проверенный том", "Tested volume"), pResult->szTestedVolume[0] ? pResult->szTestedVolume : "-",
        TN("Объём проверенных данных", "Volume of data tested"), mbTested,
        TN("Общее время теста", "Total duration"), pResult->fDurationSec,
        TN("Скорость прямой записи (Write-Through)", "Direct write speed (Write-Through)"), pResult->fWriteSpeedMBs,
        TN("Скорость прямого чтения (No-Buffering)", "Direct read speed (No-Buffering)"), pResult->fReadSpeedMBs,
        TN("Искажённых байт (Corrupted bytes)", "Corrupted bytes count"), (unsigned long long)pResult->nCorruptedBytes,
        TN("Всего битовых инверсий (Bit flips)", "Total bit flips count"), (unsigned long long)pResult->nTotalBitFlips,
        szFlipsList,
        TN("ДИАГНОСТИЧЕСКИЙ ВЕРДИКТ", "DIAGNOSTIC VERDICT"),
        pResult->szVerdictBadge,
        pResult->szVerdictText
    );
}

void FormatDramVerifyMarkdown(const DRAM_VERIFY_RESULT* pResult,
                              char* pszBuf,
                              size_t nBufSize)
{
    if (!pszBuf || nBufSize == 0) return;

    if (!pResult || pResult->nBytesTested == 0) {
        safe_snprintf_n(pszBuf, nBufSize, "%s",
            TN("## Экспресс-тест буфера DRAM (Read-After-Write)\r\n\r\n_Тестирование буфера DRAM не проводилось._\r\n",
               "## DRAM Buffer Integrity Test (Read-After-Write)\r\n\r\n_DRAM buffer verification test was not run._\r\n"));
        return;
    }

    double mbTested = (double)pResult->nBytesTested / (1024.0 * 1024.0);
    const char* szModeStr = (pResult->eMode == DRAM_VERIFY_MODE_EXPRESS)  ? TN("Экспресс (32 МБ)", "Express (32 MB)") :
                            (pResult->eMode == DRAM_VERIFY_MODE_STANDARD) ? TN("Стандарт (128 МБ)", "Standard (128 MB)") :
                                                                           TN("Стресс (512 МБ)", "Stress (512 MB)");

    char szFlipsTable[2048] = "";
    if (pResult->nRecordedFlips > 0) {
        safe_snprintf(szFlipsTable, "%s",
            TN("\r\n### Зафиксированные битовые ошибки\r\n\r\n| № | Смещение | Ожидалось | Прочитано | Битовая маска XOR |\r\n| :--- | :--- | :--- | :--- | :--- |\r\n",
               "\r\n### Recorded Bit Flip Errors\r\n\r\n| # | Offset | Expected | Actual | XOR Bitmask |\r\n| :--- | :--- | :--- | :--- | :--- |\r\n"));
        for (int i = 0; i < pResult->nRecordedFlips && i < DRAM_MAX_RECORDED_FLIPS; i++) {
            char row[128];
            const DRAM_BITFLIP_SAMPLE* s = &pResult->flips[i];
            safe_snprintf(row, "| %d | `0x%llX` | `0x%02X` | `0x%02X` | `0x%02X` |\r\n",
                i + 1, (unsigned long long)s->nByteOffset, s->bExpected, s->bActual, s->bFlippedBits);
            size_t curLen = strlen(szFlipsTable);
            if (curLen + strlen(row) < sizeof(szFlipsTable) - 1) {
                strcat(szFlipsTable, row);
            }
        }
    }

    safe_snprintf_n(pszBuf, nBufSize,
        TN("## Экспресс-тест буфера DRAM (Read-After-Write)\r\n\r\n"
           "- **Режим теста:** %s\r\n"
           "- **Проверенный том:** `%s`\r\n"
           "- **Проверено данных:** %.1f МБ\r\n"
           "- **Скорость прямой записи (Write-Through):** %.1f МБ/с\r\n"
           "- **Скорость прямого чтения (No-Buffering):** %.1f МБ/с\r\n"
           "- **Общее время:** %.2f сек\r\n"
           "- **Искажённых байт:** %llu\r\n"
           "- **Битовых инверсий (Bit Flips):** %llu\r\n"
           "- **Вердикт:** **`[%s]`**\r\n\r\n"
           "> %s\r\n%s",
           "## DRAM Buffer Integrity Test (Read-After-Write)\r\n\r\n"
           "- **Test Mode:** %s\r\n"
           "- **Tested Volume:** `%s`\r\n"
           "- **Data Tested:** %.1f MB\r\n"
           "- **Direct Write Speed (Write-Through):** %.1f MB/s\r\n"
           "- **Direct Read Speed (No-Buffering):** %.1f MB/s\r\n"
           "- **Duration:** %.2f s\r\n"
           "- **Corrupted Bytes:** %llu\r\n"
           "- **Bit Flips:** %llu\r\n"
           "- **Verdict:** **`[%s]`**\r\n\r\n"
           "> %s\r\n%s"),
        szModeStr,
        pResult->szTestedVolume[0] ? pResult->szTestedVolume : "-",
        mbTested,
        pResult->fWriteSpeedMBs,
        pResult->fReadSpeedMBs,
        pResult->fDurationSec,
        (unsigned long long)pResult->nCorruptedBytes,
        (unsigned long long)pResult->nTotalBitFlips,
        pResult->szVerdictBadge,
        pResult->szVerdictText,
        szFlipsTable
    );
}

bool RunDramVerification(const char* pszVolumeRoot,
                         DRAM_VERIFY_MODE eMode,
                         DRAM_VERIFY_RESULT* pResult,
                         DramVerifyProgressCallback pCallback,
                         void* pUserData,
                         volatile bool* pbCancel)
{
    if (!pResult) return false;

    memset(pResult, 0, sizeof(*pResult));
    pResult->eMode = eMode;
    pResult->eStage = DRAM_STAGE_PREPARING;

    uint64_t nTotalBytes = (eMode == DRAM_VERIFY_MODE_EXPRESS)  ? DRAM_EXPRESS_BYTES :
                           (eMode == DRAM_VERIFY_MODE_STANDARD) ? DRAM_STANDARD_BYTES :
                                                                 DRAM_STRESS_BYTES;
    pResult->nTotalBytesToTest = nTotalBytes;

    char szVol[32] = "C:\\";
    if (pszVolumeRoot && pszVolumeRoot[0]) {
        safe_snprintf(szVol, "%s", pszVolumeRoot);
        size_t len = strlen(szVol);
        if (len > 0 && szVol[len - 1] != '\\') {
            safe_snprintf(szVol, "%s\\", pszVolumeRoot);
        }
    }
    safe_snprintf(pResult->szTestedVolume, "%s", szVol);

    /* 1. Verify free disk space */
    ULARGE_INTEGER ulFreeBytes, ulTotalBytes, ulTotalFree;
    WCHAR wzVol[32];
    U8ToW(szVol, wzVol, 32);
    if (GetDiskFreeSpaceExW(wzVol, &ulFreeBytes, &ulTotalBytes, &ulTotalFree)) {
        uint64_t nRequired = nTotalBytes + (200ULL * 1024ULL * 1024ULL); /* test size + 200MB safety buffer */
        if (ulFreeBytes.QuadPart < nRequired) {
            pResult->eStage = DRAM_STAGE_FAILED;
            pResult->dwWin32Error = ERROR_DISK_FULL;
            DramVerifyAssessVerdict(pResult);
            if (pCallback) pCallback(pResult, pUserData);
            return false;
        }
    }

    /* 2. Form temporary test file path */
    DWORD dwPid = GetCurrentProcessId();
    DWORD dwTick = GetTickCount();
    safe_snprintf(pResult->szTempFilePath, "%s.dm_dram_test_%lu_%lu.tmp", szVol, dwPid, dwTick);

    WCHAR wzTempPath[MAX_PATH];
    U8ToW(pResult->szTempFilePath, wzTempPath, MAX_PATH);

    /* 3. Allocate page-aligned memory buffers for unbuffered I/O */
    uint8_t* pWriteBuf = (uint8_t*)VirtualAlloc(NULL, DRAM_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* pReadBuf  = (uint8_t*)VirtualAlloc(NULL, DRAM_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    uint8_t* pExpBuf   = (uint8_t*)VirtualAlloc(NULL, DRAM_BLOCK_SIZE, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);

    if (!pWriteBuf || !pReadBuf || !pExpBuf) {
        if (pWriteBuf) VirtualFree(pWriteBuf, 0, MEM_RELEASE);
        if (pReadBuf)  VirtualFree(pReadBuf, 0, MEM_RELEASE);
        if (pExpBuf)   VirtualFree(pExpBuf, 0, MEM_RELEASE);
        pResult->eStage = DRAM_STAGE_FAILED;
        pResult->dwWin32Error = ERROR_NOT_ENOUGH_MEMORY;
        DramVerifyAssessVerdict(pResult);
        if (pCallback) pCallback(pResult, pUserData);
        return false;
    }

    LARGE_INTEGER freq, tStart, tNow;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&tStart);

    /* 4. Open file with FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH to bypass host RAM cache */
    HANDLE hFile = CreateFileW(wzTempPath,
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               NULL,
                               CREATE_ALWAYS,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                               NULL);

    if (hFile == INVALID_HANDLE_VALUE) {
        /* Fallback: try user Temp directory if root volume access was restricted */
        WCHAR wzWinTemp[MAX_PATH];
        if (GetTempPathW(MAX_PATH, wzWinTemp)) {
            safe_snprintf(pResult->szTempFilePath, "%s\\.dm_dram_test_%lu_%lu.tmp", szVol, dwPid, dwTick);
            U8ToW(pResult->szTempFilePath, wzTempPath, MAX_PATH);
            hFile = CreateFileW(wzTempPath,
                                GENERIC_READ | GENERIC_WRITE,
                                0,
                                NULL,
                                CREATE_ALWAYS,
                                FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
                                NULL);
        }
    }

    if (hFile == INVALID_HANDLE_VALUE) {
        pResult->eStage = DRAM_STAGE_FAILED;
        pResult->dwWin32Error = GetLastError();
        VirtualFree(pWriteBuf, 0, MEM_RELEASE);
        VirtualFree(pReadBuf, 0, MEM_RELEASE);
        VirtualFree(pExpBuf, 0, MEM_RELEASE);
        DramVerifyAssessVerdict(pResult);
        if (pCallback) pCallback(pResult, pUserData);
        return false;
    }

    /* 5. Phase 1: Write stress patterns */
    pResult->eStage = DRAM_STAGE_WRITING;
    size_t nTotalBlocks = (size_t)(nTotalBytes / DRAM_BLOCK_SIZE);
    size_t nBlocksPerPattern = nTotalBlocks / 4;
    if (nBlocksPerPattern == 0) nBlocksPerPattern = 1;

    LARGE_INTEGER tWriteStart, tWriteEnd;
    QueryPerformanceCounter(&tWriteStart);

    for (size_t b = 0; b < nTotalBlocks; b++) {
        if (pbCancel && *pbCancel) {
            pResult->bAborted = true;
            break;
        }

        uint64_t nBlockOffset = (uint64_t)b * DRAM_BLOCK_SIZE;
        DRAM_PATTERN_TYPE ePattern = (DRAM_PATTERN_TYPE)(b / nBlocksPerPattern);
        if (ePattern >= DRAM_PATTERN_COUNT) ePattern = DRAM_PATTERN_SOLID_INV;

        DramFillPattern(pWriteBuf, DRAM_BLOCK_SIZE, nBlockOffset, ePattern);

        DWORD dwWritten = 0;
        if (!WriteFile(hFile, pWriteBuf, (DWORD)DRAM_BLOCK_SIZE, &dwWritten, NULL) || dwWritten != DRAM_BLOCK_SIZE) {
            pResult->dwWin32Error = GetLastError();
            pResult->eStage = DRAM_STAGE_FAILED;
            break;
        }

        pResult->nBytesTested += dwWritten;

        QueryPerformanceCounter(&tNow);
        double el = (double)(tNow.QuadPart - tWriteStart.QuadPart) / (double)freq.QuadPart;
        if (el > 0.05) {
            pResult->fWriteSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / el;
        }

        if (pCallback && !pCallback(pResult, pUserData)) {
            pResult->bAborted = true;
            break;
        }
    }

    QueryPerformanceCounter(&tWriteEnd);
    double writeDur = (double)(tWriteEnd.QuadPart - tWriteStart.QuadPart) / (double)freq.QuadPart;
    if (writeDur > 0.001) {
        pResult->fWriteSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / writeDur;
    }

    /* Flush controller hardware cache buffers and close write handle */
    pResult->eStage = DRAM_STAGE_FLUSHING;
    if (pCallback) pCallback(pResult, pUserData);
    FlushFileBuffers(hFile);
    CloseHandle(hFile);
    hFile = INVALID_HANDLE_VALUE;

    /* If aborted or write failed, clean up */
    if (pResult->bAborted || pResult->eStage == DRAM_STAGE_FAILED) {
        DeleteFileW(wzTempPath);
        VirtualFree(pWriteBuf, 0, MEM_RELEASE);
        VirtualFree(pReadBuf, 0, MEM_RELEASE);
        VirtualFree(pExpBuf, 0, MEM_RELEASE);
        DramVerifyAssessVerdict(pResult);
        if (pCallback) pCallback(pResult, pUserData);
        return false;
    }

    /* 6. Phase 2: Read-After-Write Verification (Unbuffered) */
    pResult->eStage = DRAM_STAGE_READING_VERIFYING;
    pResult->nBytesTested = 0;

    HANDLE hRead = CreateFileW(wzTempPath,
                               GENERIC_READ,
                               FILE_SHARE_READ,
                               NULL,
                               OPEN_EXISTING,
                               FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
                               NULL);

    if (hRead == INVALID_HANDLE_VALUE) {
        pResult->dwWin32Error = GetLastError();
        pResult->eStage = DRAM_STAGE_FAILED;
        DeleteFileW(wzTempPath);
        VirtualFree(pWriteBuf, 0, MEM_RELEASE);
        VirtualFree(pReadBuf, 0, MEM_RELEASE);
        VirtualFree(pExpBuf, 0, MEM_RELEASE);
        DramVerifyAssessVerdict(pResult);
        if (pCallback) pCallback(pResult, pUserData);
        return false;
    }

    LARGE_INTEGER tReadStart, tReadEnd;
    QueryPerformanceCounter(&tReadStart);

    for (size_t b = 0; b < nTotalBlocks; b++) {
        if (pbCancel && *pbCancel) {
            pResult->bAborted = true;
            break;
        }

        DWORD dwRead = 0;
        if (!ReadFile(hRead, pReadBuf, (DWORD)DRAM_BLOCK_SIZE, &dwRead, NULL) || dwRead != DRAM_BLOCK_SIZE) {
            pResult->dwWin32Error = GetLastError();
            pResult->eStage = DRAM_STAGE_FAILED;
            break;
        }

        uint64_t nBlockOffset = (uint64_t)b * DRAM_BLOCK_SIZE;
        DRAM_PATTERN_TYPE ePattern = (DRAM_PATTERN_TYPE)(b / nBlocksPerPattern);
        if (ePattern >= DRAM_PATTERN_COUNT) ePattern = DRAM_PATTERN_SOLID_INV;

        DramFillPattern(pExpBuf, DRAM_BLOCK_SIZE, nBlockOffset, ePattern);

        /* Byte-by-byte compare and bit flip accounting */
        DramVerifyBuffer(pReadBuf, pExpBuf, DRAM_BLOCK_SIZE, nBlockOffset, pResult);

        pResult->nBytesTested += dwRead;

        QueryPerformanceCounter(&tNow);
        double el = (double)(tNow.QuadPart - tReadStart.QuadPart) / (double)freq.QuadPart;
        if (el > 0.05) {
            pResult->fReadSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / el;
        }

        if (pCallback && !pCallback(pResult, pUserData)) {
            pResult->bAborted = true;
            break;
        }
    }

    QueryPerformanceCounter(&tReadEnd);
    double readDur = (double)(tReadEnd.QuadPart - tReadStart.QuadPart) / (double)freq.QuadPart;
    if (readDur > 0.001) {
        pResult->fReadSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / readDur;
    }

    CloseHandle(hRead);
    DeleteFileW(wzTempPath);

    VirtualFree(pWriteBuf, 0, MEM_RELEASE);
    VirtualFree(pReadBuf, 0, MEM_RELEASE);
    VirtualFree(pExpBuf, 0, MEM_RELEASE);

    QueryPerformanceCounter(&tNow);
    pResult->fDurationSec = (double)(tNow.QuadPart - tStart.QuadPart) / (double)freq.QuadPart;

    if (!pResult->bAborted && pResult->eStage != DRAM_STAGE_FAILED) {
        pResult->eStage = DRAM_STAGE_COMPLETED;
    }

    DramVerifyAssessVerdict(pResult);
    if (pCallback) pCallback(pResult, pUserData);

    return pResult->bPassed;
}
