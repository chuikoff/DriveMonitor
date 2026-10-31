/* DriveMonitor - Latency Probe / Surface Benchmark implementation. GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "latency_probe.h"
#include "safestr.h"
#include "lang.h"

LATENCY_BIN LatencyBinFromMs(double fLatencyMs, DWORD dwWin32Error)
{
    if (dwWin32Error != 0) {
        return LATENCY_BIN_ERROR;
    }
    if (fLatencyMs < 10.0) {
        return LATENCY_BIN_FAST;
    }
    if (fLatencyMs < 50.0) {
        return LATENCY_BIN_NORMAL;
    }
    if (fLatencyMs < 150.0) {
        return LATENCY_BIN_ACCEPTABLE;
    }
    if (fLatencyMs < 500.0) {
        return LATENCY_BIN_SLOW;
    }
    return LATENCY_BIN_CRITICAL;
}

const char* LatencyBinName(LATENCY_BIN bin)
{
    switch (bin) {
    case LATENCY_BIN_FAST:
        return TN("Быстро (<10 мс)", "Fast (<10 ms)");
    case LATENCY_BIN_NORMAL:
        return TN("Нормально (10–50 мс)", "Normal (10–50 ms)");
    case LATENCY_BIN_ACCEPTABLE:
        return TN("Приемлемо (50–150 мс)", "Acceptable (50–150 ms)");
    case LATENCY_BIN_SLOW:
        return TN("Медленно (150–500 мс)", "Slow (150–500 ms)");
    case LATENCY_BIN_CRITICAL:
        return TN("Критично (>500 мс)", "Critical (>500 ms)");
    case LATENCY_BIN_ERROR:
        return TN("Ошибка чтения", "Read error");
    default:
        return TN("Неизвестно", "Unknown");
    }
}

const char* LatencyBinRange(LATENCY_BIN bin)
{
    switch (bin) {
    case LATENCY_BIN_FAST:        return "< 10 ms";
    case LATENCY_BIN_NORMAL:      return "10–50 ms";
    case LATENCY_BIN_ACCEPTABLE:  return "50–150 ms";
    case LATENCY_BIN_SLOW:        return "150–500 ms";
    case LATENCY_BIN_CRITICAL:    return "> 500 ms";
    case LATENCY_BIN_ERROR:       return TN("Ошибка", "Error");
    default:                      return "?";
    }
}

COLORREF LatencyBinColor(LATENCY_BIN bin)
{
    switch (bin) {
    case LATENCY_BIN_FAST:
        return RGB(46, 204, 113);   /* Fresh green */
    case LATENCY_BIN_NORMAL:
        return RGB(52, 152, 219);   /* Sky blue */
    case LATENCY_BIN_ACCEPTABLE:
        return RGB(241, 196, 15);   /* Warm amber/yellow */
    case LATENCY_BIN_SLOW:
        return RGB(230, 126, 34);   /* Orange */
    case LATENCY_BIN_CRITICAL:
        return RGB(231, 76, 60);    /* Bright red */
    case LATENCY_BIN_ERROR:
        return RGB(142, 68, 173);   /* Deep purple / error */
    default:
        return RGB(180, 180, 180);
    }
}

void LatencyProbeAssessVerdict(LATENCY_PROBE_RESULT* pResult, bool bIsSsd)
{
    if (!pResult) return;

    if (pResult->nBins[LATENCY_BIN_ERROR] > 0) {
        pResult->eVerdictLevel = LATENCY_VERDICT_BAD;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("СБОЙ ЧТЕНИЯ", "READ FAILURE"));
        if (bIsSsd) {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Обнаружены нечитаемые блоки (I/O ошибки). Страницы флеш-памяти повреждены или контроллер сообщил о неисправимой ошибке (ECC Uncorrectable).",
                   "Unreadable blocks detected (I/O errors). Flash memory pages damaged or controller reported uncorrectable ECC error."));
        } else {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Обнаружены нечитаемые сектора (I/O ошибки). Физическое повреждение магнитной поверхности диска (Bad Sectors).",
                   "Unreadable sectors detected (I/O errors). Physical surface damage on disk media (Bad Sectors)."));
        }
    }
    else if (pResult->nBins[LATENCY_BIN_CRITICAL] > 0) {
        pResult->eVerdictLevel = LATENCY_VERDICT_WARNING;
        if (bIsSsd) {
            safe_snprintf(pResult->szVerdictBadge, "%s", TN("УТЕЧКА ЗАРЯДА", "RETENTION LOSS"));
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Обнаружены блоки с критической задержкой (>500 мс). Признак потери заряда ячеек (Data Retention Loss) или деградации от чтения (Read Disturb). Контроллер тратит 500–2000 мс на циклы LDPC Soft Decision, что вызывает зависания системы при чистом SMART. Рекомендуется перезапись (Refresh) или резервное копирование.",
                   "Blocks with critical latency (>500 ms) detected. Indicates cell charge loss (Data Retention Loss) or Read Disturb. Controller spends 500–2000 ms per read on LDPC Soft Decision iterations, freezing apps while SMART stays clean. Data refresh or backup recommended."));
        } else {
            safe_snprintf(pResult->szVerdictBadge, "%s", TN("СЛАБЫЙ СЕКТОР", "WEAK SECTOR"));
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Обнаружены сектора с задержкой >500 мс (многократные попытки раскрутки сервометок). Признак слабеющей магнитной головки (Weak Head) или деградации магнитного слоя.",
                   "Sectors with latency >500 ms detected (multi-rotation servo retries). Indicates a weakening magnetic head or magnetic media surface degradation."));
        }
    }
    else if (pResult->nBins[LATENCY_BIN_SLOW] > 0) {
        pResult->eVerdictLevel = LATENCY_VERDICT_CAUTION;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ПРЕДУПРЕЖДЕНИЕ", "CAUTION"));
        if (bIsSsd) {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Зафиксированы блоки с задержкой 150–500 мс (активация LDPC коррекции). Ранняя стадия потери заряда ячеек памяти на старых данных.",
                   "Blocks with 150-500 ms latency detected (LDPC correction active). Early stage of cell charge leakage on cold data."));
        } else {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Зафиксированы сектора с задержкой 150–500 мс. Повышенное время позиционирования или ранний износ поверхности.",
                   "Sectors with 150-500 ms latency detected. Elevated seek time or early surface wear."));
        }
    }
    else if (bIsSsd && pResult->nBins[LATENCY_BIN_ACCEPTABLE] > 0) {
        pResult->eVerdictLevel = LATENCY_VERDICT_GOOD;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ХОРОШО", "GOOD"));
        safe_snprintf(pResult->szVerdictText, "%s",
            TN("Задержки чтения в пределах допустимого. Незначительные колебания времени отклика при обращении к NAND.",
               "Read latencies within acceptable range. Minor response time fluctuations during NAND access."));
    }
    else {
        pResult->eVerdictLevel = LATENCY_VERDICT_EXCELLENT;
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ОТЛИЧНО", "EXCELLENT"));
        if (bIsSsd) {
            if (pResult->eMode == LATENCY_MODE_FULL && pResult->fAvgSpeedMBs > 0.0) {
                safe_snprintf(pResult->szVerdictText, "%s (%.1f %s)",
                    TN("Отличная скорость линейного чтения без признаков потери заряда ячеек (Retention Loss)",
                       "Excellent sequential read speed with no signs of cell charge loss (Retention Loss)"),
                    pResult->fAvgSpeedMBs, TN("МБ/с", "MB/s"));
            } else {
                safe_snprintf(pResult->szVerdictText, "%s",
                    TN("Мгновенный отклик ячеек памяти по всей емкости. Признаков потери заряда (Retention Loss) или деградации не обнаружено.",
                       "Instant cell response across full capacity. No signs of charge loss (Retention Loss) or degradation detected."));
            }
        } else {
            if (pResult->eMode == LATENCY_MODE_FULL && pResult->fAvgSpeedMBs > 0.0) {
                safe_snprintf(pResult->szVerdictText, "%s (%.1f %s)",
                    TN("Стабильное линейное чтение по всей поверхности пластин",
                       "Stable sequential read across entire platter surface"),
                    pResult->fAvgSpeedMBs, TN("МБ/с", "MB/s"));
            } else {
                safe_snprintf(pResult->szVerdictText, "%s",
                    TN("Стабильное время доступа по всей поверхности диска. Механика и магнитный слой в отличном состоянии.",
                       "Stable seek times across entire disk surface. Mechanics and magnetic layer in excellent condition."));
            }
        }
    }
}

void FormatLatencySummary(const LATENCY_PROBE_RESULT* pResult,
                          const char* pszModel,
                          const char* pszSerial,
                          uint64_t nCapacityBytes,
                          bool bIsSsd,
                          char* pszBuf,
                          size_t nBufSize)
{
    if (!pResult || !pszBuf || nBufSize == 0) return;

    double capGb = (double)nCapacityBytes / (1024.0 * 1024.0 * 1024.0);
    int tested = pResult->nSamplesTested;
    if (tested <= 0) tested = 1;

    int totalBlocks = 0;
    for (int b = 0; b < LATENCY_BIN_COUNT; b++) {
        totalBlocks += pResult->nBins[b];
    }
    if (totalBlocks <= 0) totalBlocks = tested;

    bool bFull = (pResult->eMode == LATENCY_MODE_FULL);

    safe_snprintf_n(pszBuf, nBufSize,
        "============================================================\r\n"
        "%s\r\n"
        "============================================================\r\n"
        "%s: %s\r\n"
        "%s: %s\r\n"
        "%s: %s\r\n"
        "%s: %.1f GB (%s)\r\n"
        "%s: %d\r\n"
        "%s: %.2f s\r\n"
        "%s: %.1f MB/s (Min: %.1f | Max: %.1f)\r\n"
        "\r\n"
        "%s:\r\n"
        "  Min: %.2f ms  |  Avg: %.2f ms  |  Max: %.2f ms\r\n"
        "\r\n"
        "%s:\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "  %-22s : %3d (%5.1f%%)\r\n"
        "\r\n"
        "------------------------------------------------------------\r\n"
        "%s: [%s]\r\n"
        "%s\r\n"
        "============================================================\r\n",
        bFull ? TN("ОТЧЁТ ПОЛНОГО ТЕСТИРОВАНИЯ СКОРОСТИ ЧТЕНИЯ (SURFACE BENCHMARK)",
                   "FULL SURFACE READ SPEED BENCHMARK REPORT")
              : TN("ОТЧЁТ ЭКСПРЕСС-ТЕСТИРОВАНИЯ ВРЕМЕНИ ОТКЛИКА (LATENCY PROBE)",
                   "EXPRESS LATENCY PROBE / SURFACE BENCHMARK REPORT"),
        TN("Режим теста", "Test mode"),
        bFull ? TN("Полный тест скорости (100% поверхности)", "Full read speed test (100% surface)")
              : TN("Экспресс-сканирование отклика (128 точек)", "Express latency probe (128 samples)"),
        TN("Модель", "Model"), pszModel ? pszModel : "Unknown",
        TN("Серийный номер", "Serial"), pszSerial ? pszSerial : "Unknown",
        TN("Объём", "Capacity"), capGb, bIsSsd ? "SSD" : "HDD",
        TN("Проверено блоков/секций", "Tested blocks/sections"), pResult->nSamplesTested,
        TN("Общее время", "Total duration"), pResult->fTotalTimeSec,
        TN("Скорость чтения", "Read speed"), pResult->fAvgSpeedMBs, pResult->fMinSpeedMBs, pResult->fMaxSpeedMBs,
        TN("ВРЕМЯ ОТКЛИКА", "LATENCY TIMINGS"),
        pResult->fMinMs, pResult->fAvgMs, pResult->fMaxMs,
        TN("РАСПРЕДЕЛЕНИЕ ЗАДЕРЖЕК", "LATENCY DISTRIBUTION"),
        LatencyBinName(LATENCY_BIN_FAST), pResult->nBins[LATENCY_BIN_FAST],
        (double)pResult->nBins[LATENCY_BIN_FAST] * 100.0 / (double)totalBlocks,
        LatencyBinName(LATENCY_BIN_NORMAL), pResult->nBins[LATENCY_BIN_NORMAL],
        (double)pResult->nBins[LATENCY_BIN_NORMAL] * 100.0 / (double)totalBlocks,
        LatencyBinName(LATENCY_BIN_ACCEPTABLE), pResult->nBins[LATENCY_BIN_ACCEPTABLE],
        (double)pResult->nBins[LATENCY_BIN_ACCEPTABLE] * 100.0 / (double)totalBlocks,
        LatencyBinName(LATENCY_BIN_SLOW), pResult->nBins[LATENCY_BIN_SLOW],
        (double)pResult->nBins[LATENCY_BIN_SLOW] * 100.0 / (double)totalBlocks,
        LatencyBinName(LATENCY_BIN_CRITICAL), pResult->nBins[LATENCY_BIN_CRITICAL],
        (double)pResult->nBins[LATENCY_BIN_CRITICAL] * 100.0 / (double)totalBlocks,
        LatencyBinName(LATENCY_BIN_ERROR), pResult->nBins[LATENCY_BIN_ERROR],
        (double)pResult->nBins[LATENCY_BIN_ERROR] * 100.0 / (double)totalBlocks,
        TN("ДИАГНОСТИЧЕСКОЕ ЗАКЛЮЧЕНИЕ", "DIAGNOSTIC VERDICT"),
        pResult->szVerdictBadge,
        pResult->szVerdictText
    );
}

bool RunLatencyProbeEx(int nDriveIndex,
                       uint64_t nTotalCapacityBytes,
                       LATENCY_PROBE_MODE eMode,
                       int nSampleCount,
                       LATENCY_PROBE_RESULT* pResult,
                       LatencyProbeCallback pCallback,
                       void* pUserData,
                       volatile bool* pbCancel)
{
    if (!pResult) return false;

    if (nSampleCount <= 0) nSampleCount = LATENCY_SAMPLES_DEFAULT;
    if (nSampleCount > LATENCY_SAMPLES_MAX) nSampleCount = LATENCY_SAMPLES_MAX;

    memset(pResult, 0, sizeof(*pResult));
    pResult->eMode = eMode;
    pResult->nTotalSamples = (eMode == LATENCY_MODE_FULL) ? LATENCY_SAMPLES_DEFAULT : nSampleCount;
    pResult->fMinMs = 9999999.0;
    pResult->fMaxMs = 0.0;
    pResult->fMinSpeedMBs = 9999999.0;
    pResult->fMaxSpeedMBs = 0.0;

    wchar_t szPath[64];
    _snwprintf(szPath, sizeof(szPath)/sizeof(szPath[0]), L"\\\\.\\PhysicalDrive%d", nDriveIndex);

    DWORD accessFlags = FILE_FLAG_NO_BUFFERING | ((eMode == LATENCY_MODE_FULL) ? FILE_FLAG_SEQUENTIAL_SCAN : FILE_FLAG_RANDOM_ACCESS);

    /* Open device with unbuffered read access */
    HANDLE hDev = CreateFileW(szPath,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              NULL,
                              OPEN_EXISTING,
                              accessFlags,
                              NULL);

    if (hDev == INVALID_HANDLE_VALUE) {
        DWORD dwErr = GetLastError();
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ОШИБКА ДОСТУПА", "ACCESS ERROR"));
        if (dwErr == ERROR_ACCESS_DENIED) {
            safe_snprintf(pResult->szVerdictText, "%s",
                TN("Отказано в доступе к физическому диску. Запустите DriveMonitor с правами Администратора.",
                   "Access denied to physical drive. Please run DriveMonitor as Administrator."));
        } else {
            safe_snprintf(pResult->szVerdictText, "%s (%lu)",
                TN("Не удалось открыть физический диск", "Failed to open physical drive"),
                (unsigned long)dwErr);
        }
        return false;
    }

    /* Query capacity if not provided */
    if (nTotalCapacityBytes == 0) {
        DISK_GEOMETRY_EX geom;
        DWORD dwRet = 0;
        if (DeviceIoControl(hDev, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, NULL, 0,
                            &geom, sizeof(geom), &dwRet, NULL)) {
            nTotalCapacityBytes = (uint64_t)geom.DiskSize.QuadPart;
        }
    }
    pResult->nTotalCapacityBytes = nTotalCapacityBytes;

    DWORD blockSize = (eMode == LATENCY_MODE_FULL) ? (DWORD)LATENCY_FULL_BLOCK_SIZE : (DWORD)LATENCY_BLOCK_SIZE;

    if (nTotalCapacityBytes < (uint64_t)blockSize) {
        CloseHandle(hDev);
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("ОШИБКА ЕМКОСТИ", "CAPACITY ERROR"));
        safe_snprintf(pResult->szVerdictText, "%s",
            TN("Не удалось определить размер диска для тестирования.",
               "Could not determine disk capacity for latency probe."));
        return false;
    }

    /* Allocate sector-aligned buffer */
    void* pBuffer = VirtualAlloc(NULL, blockSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pBuffer) {
        CloseHandle(hDev);
        safe_snprintf(pResult->szVerdictBadge, "%s", TN("НЕТ ПАМЯТИ", "OUT OF MEMORY"));
        safe_snprintf(pResult->szVerdictText, "%s",
            TN("Не удалось выделить выровненный буфер памяти.", "Failed to allocate aligned memory buffer."));
        return false;
    }

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    LARGE_INTEGER tStartTotal, tEndTotal;
    QueryPerformanceCounter(&tStartTotal);

    double sumMs = 0.0;

    if (eMode == LATENCY_MODE_FULL) {
        /* Full sequential surface read benchmark */
        uint64_t curOffset = 0;
        int nMapCells = LATENCY_SAMPLES_DEFAULT;
        uint64_t bytesPerCell = (nTotalCapacityBytes > (uint64_t)nMapCells)
                              ? (nTotalCapacityBytes / (uint64_t)nMapCells) : (uint64_t)blockSize;
        if (bytesPerCell < (uint64_t)blockSize) bytesPerCell = (uint64_t)blockSize;
        bytesPerCell = (bytesPerCell / (uint64_t)blockSize) * (uint64_t)blockSize;
        if (bytesPerCell == 0) bytesPerCell = (uint64_t)blockSize;

        for (int c = 0; c < nMapCells; c++) {
            pResult->samples[c].nOffsetBytes = (uint64_t)c * bytesPerCell;
            pResult->samples[c].bin = LATENCY_BIN_FAST;
        }

        LARGE_INTEGER tLastCallback = tStartTotal;

        while (curOffset < nTotalCapacityBytes) {
            if (pbCancel && *pbCancel) {
                pResult->bAborted = true;
                break;
            }

            DWORD toRead = blockSize;
            if (curOffset + (uint64_t)toRead > nTotalCapacityBytes) {
                toRead = (DWORD)(nTotalCapacityBytes - curOffset);
            }
            if (toRead % 512 != 0) {
                toRead = ((toRead + 511) / 512) * 512;
            }

            LARGE_INTEGER liOffset;
            liOffset.QuadPart = (LONGLONG)curOffset;
            SetFilePointerEx(hDev, liOffset, NULL, FILE_BEGIN);

            LARGE_INTEGER t1, t2;
            DWORD dwRead = 0;
            QueryPerformanceCounter(&t1);
            BOOL bOk = ReadFile(hDev, pBuffer, toRead, &dwRead, NULL);
            QueryPerformanceCounter(&t2);

            DWORD dwErr = 0;
            double blockMs = 0.0;
            LATENCY_BIN bin = LATENCY_BIN_FAST;

            if (!bOk || dwRead == 0) {
                dwErr = bOk ? ERROR_READ_FAULT : GetLastError();
                bin = LATENCY_BIN_ERROR;
            } else {
                blockMs = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
                if (blockMs < 0.001) blockMs = 0.001;
                bin = LatencyBinFromMs(blockMs, 0);
            }

            double blockSpeedMBs = (blockMs > 0.0 && dwRead > 0)
                                 ? (((double)dwRead / (1024.0 * 1024.0)) / (blockMs / 1000.0)) : 0.0;
            pResult->fCurrentSpeedMBs = blockSpeedMBs;
            if (blockSpeedMBs > pResult->fMaxSpeedMBs) pResult->fMaxSpeedMBs = blockSpeedMBs;
            if (blockSpeedMBs > 0.0 && blockSpeedMBs < pResult->fMinSpeedMBs) pResult->fMinSpeedMBs = blockSpeedMBs;

            pResult->nBins[bin]++;
            if (bin != LATENCY_BIN_ERROR) {
                if (blockMs < pResult->fMinMs) pResult->fMinMs = blockMs;
                if (blockMs > pResult->fMaxMs) pResult->fMaxMs = blockMs;
                sumMs += blockMs;
            }

            pResult->nBytesTested += dwRead;
            curOffset += (uint64_t)toRead;

            int curCell = (int)(curOffset / bytesPerCell);
            if (curCell >= nMapCells) curCell = nMapCells - 1;

            LATENCY_SAMPLE* pCell = &pResult->samples[curCell];
            if (bin > pCell->bin || pCell->fLatencyMs == 0.0) pCell->bin = bin;
            pCell->fLatencyMs = blockMs;
            pCell->fSpeedMBs = blockSpeedMBs;
            pCell->nOffsetBytes = curOffset;
            pCell->dwWin32Error = dwErr;
            pResult->nSamplesTested = curCell + 1;

            LARGE_INTEGER tNow;
            QueryPerformanceCounter(&tNow);
            double elapsed = (double)(tNow.QuadPart - tStartTotal.QuadPart) / (double)freq.QuadPart;
            if (elapsed < 0.001) elapsed = 0.001;
            pResult->fTotalTimeSec = elapsed;
            pResult->fAvgSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / elapsed;

            if (pResult->fAvgSpeedMBs > 0.1 && nTotalCapacityBytes > pResult->nBytesTested) {
                double remBytes = (double)(nTotalCapacityBytes - pResult->nBytesTested);
                pResult->fEtaSeconds = remBytes / (pResult->fAvgSpeedMBs * 1024.0 * 1024.0);
            } else {
                pResult->fEtaSeconds = 0.0;
            }

            double msSinceCallback = (double)(tNow.QuadPart - tLastCallback.QuadPart) * 1000.0 / (double)freq.QuadPart;
            if (msSinceCallback >= 40.0 || curOffset >= nTotalCapacityBytes || (pbCancel && *pbCancel)) {
                tLastCallback = tNow;
                if (pCallback) {
                    if (!pCallback(curCell, nMapCells, pCell, pUserData)) {
                        pResult->bAborted = true;
                        break;
                    }
                }
            }
        }
    } else {
        /* Express sample mode (128 distributed points) */
        uint64_t maxSafeOffset = (nTotalCapacityBytes > (uint64_t)LATENCY_BLOCK_SIZE)
                               ? (nTotalCapacityBytes - (uint64_t)LATENCY_BLOCK_SIZE) : 0;

        for (int i = 0; i < nSampleCount; i++) {
            if (pbCancel && *pbCancel) {
                pResult->bAborted = true;
                break;
            }

            uint64_t rawOffset = (nSampleCount > 1)
                               ? ((maxSafeOffset * (uint64_t)i) / (uint64_t)(nSampleCount - 1))
                               : 0;

            uint64_t alignedOffset = (rawOffset / (uint64_t)LATENCY_BLOCK_SIZE) * (uint64_t)LATENCY_BLOCK_SIZE;
            if (alignedOffset + (uint64_t)LATENCY_BLOCK_SIZE > nTotalCapacityBytes &&
                alignedOffset >= (uint64_t)LATENCY_BLOCK_SIZE) {
                alignedOffset -= (uint64_t)LATENCY_BLOCK_SIZE;
            }

            LARGE_INTEGER liOffset;
            liOffset.QuadPart = (LONGLONG)alignedOffset;

            LATENCY_SAMPLE sample;
            sample.nOffsetBytes = alignedOffset;
            sample.fSpeedMBs = 0.0;

            if (!SetFilePointerEx(hDev, liOffset, NULL, FILE_BEGIN)) {
                sample.dwWin32Error = GetLastError();
                sample.fLatencyMs = 0.0;
                sample.bin = LATENCY_BIN_ERROR;
            } else {
                LARGE_INTEGER t1, t2;
                DWORD dwRead = 0;
                QueryPerformanceCounter(&t1);
                BOOL bOk = ReadFile(hDev, pBuffer, (DWORD)LATENCY_BLOCK_SIZE, &dwRead, NULL);
                QueryPerformanceCounter(&t2);

                if (!bOk || dwRead != (DWORD)LATENCY_BLOCK_SIZE) {
                    sample.dwWin32Error = bOk ? ERROR_READ_FAULT : GetLastError();
                    sample.fLatencyMs = 0.0;
                    sample.bin = LATENCY_BIN_ERROR;
                } else {
                    sample.dwWin32Error = 0;
                    sample.fLatencyMs = (double)(t2.QuadPart - t1.QuadPart) * 1000.0 / (double)freq.QuadPart;
                    if (sample.fLatencyMs < 0.001) sample.fLatencyMs = 0.001;
                    sample.bin = LatencyBinFromMs(sample.fLatencyMs, 0);
                    sample.fSpeedMBs = ((double)LATENCY_BLOCK_SIZE / (1024.0 * 1024.0)) / (sample.fLatencyMs / 1000.0);
                    if (sample.fSpeedMBs > pResult->fMaxSpeedMBs) pResult->fMaxSpeedMBs = sample.fSpeedMBs;
                    if (pResult->fMinSpeedMBs > 999999.0 || sample.fSpeedMBs < pResult->fMinSpeedMBs) pResult->fMinSpeedMBs = sample.fSpeedMBs;
                }
            }

            pResult->samples[i] = sample;
            pResult->nBins[sample.bin]++;
            pResult->nSamplesTested++;
            pResult->nBytesTested += (uint64_t)LATENCY_BLOCK_SIZE;

            if (sample.bin != LATENCY_BIN_ERROR) {
                if (sample.fLatencyMs < pResult->fMinMs) pResult->fMinMs = sample.fLatencyMs;
                if (sample.fLatencyMs > pResult->fMaxMs) pResult->fMaxMs = sample.fLatencyMs;
                sumMs += sample.fLatencyMs;
            }

            if (pCallback) {
                if (!pCallback(i, nSampleCount, &sample, pUserData)) {
                    pResult->bAborted = true;
                    break;
                }
            }
        }
    }

    QueryPerformanceCounter(&tEndTotal);
    pResult->fTotalTimeSec = (double)(tEndTotal.QuadPart - tStartTotal.QuadPart) / (double)freq.QuadPart;
    if (pResult->fTotalTimeSec > 0.001) {
        pResult->fAvgSpeedMBs = ((double)pResult->nBytesTested / (1024.0 * 1024.0)) / pResult->fTotalTimeSec;
    }

    VirtualFree(pBuffer, 0, MEM_RELEASE);
    CloseHandle(hDev);

    int totalBlocks = 0;
    for (int b = 0; b < LATENCY_BIN_COUNT; b++) {
        totalBlocks += pResult->nBins[b];
    }
    int validSamples = totalBlocks - pResult->nBins[LATENCY_BIN_ERROR];
    if (validSamples > 0) {
        pResult->fAvgMs = sumMs / (double)validSamples;
    } else {
        pResult->fMinMs = 0.0;
        pResult->fAvgMs = 0.0;
    }
    if (pResult->fMinSpeedMBs > 999999.0) pResult->fMinSpeedMBs = 0.0;

    return true;
}

bool RunLatencyProbe(int nDriveIndex,
                     uint64_t nTotalCapacityBytes,
                     int nSampleCount,
                     LATENCY_PROBE_RESULT* pResult,
                     LatencyProbeCallback pCallback,
                     void* pUserData,
                     volatile bool* pbCancel)
{
    return RunLatencyProbeEx(nDriveIndex, nTotalCapacityBytes, LATENCY_MODE_EXPRESS,
                             nSampleCount, pResult, pCallback, pUserData, pbCancel);
}

void FormatLatencyMarkdown(const LATENCY_PROBE_RESULT* pResult,
                           char* pszBuf,
                           size_t nBufSize)
{
    if (!pszBuf || nBufSize == 0) return;
    pszBuf[0] = '\0';

    if (!pResult || pResult->nSamplesTested <= 0) {
        safe_snprintf_n(pszBuf, nBufSize, "%s",
            TN("_Экспресс-сканирование времени отклика для этого накопителя ещё не проводилось._\r\n",
               "_Express latency probe has not been run for this drive yet._\r\n"));
        return;
    }

    size_t len = 0;
    int tested = pResult->nSamplesTested;
    int totalBlocks = 0;
    for (int b = 0; b < LATENCY_BIN_COUNT; b++) {
        totalBlocks += pResult->nBins[b];
    }
    if (totalBlocks <= 0) totalBlocks = (tested > 0) ? tested : 1;
    bool bFull = (pResult->eMode == LATENCY_MODE_FULL);

    char szHdr[1024];
    if (bFull) {
        double doneGb = (double)pResult->nBytesTested / (1024.0 * 1024.0 * 1024.0);
        double totalGb = (double)pResult->nTotalCapacityBytes / (1024.0 * 1024.0 * 1024.0);
        double pct = (totalGb > 0.0) ? (doneGb * 100.0 / totalGb) : 100.0;
        safe_snprintf(szHdr,
            TN("- **Режим теста:** Полный тест скорости чтения поверхности (100%% емкости)\r\n"
               "- **Вердикт:** [%s]\r\n"
               "- **Диагностика:** %s\r\n"
               "- **Проверено данных:** %.1f ГБ / %.1f ГБ (%.1f%%, блоками по %d КиБ, прямое небуферизованное чтение)\r\n"
               "- **Скорость чтения:** Средняя: %.1f МБ/с  |  Мин: %.1f МБ/с  |  Макс: %.1f МБ/с\r\n"
               "- **Время выполнения:** %.2f с\r\n"
               "- **Задержки:** Мин: %.2f мс  |  Среднее: %.2f мс  |  Макс: %.2f мс\r\n\r\n",
               "- **Test mode:** Full surface read benchmark (100%% capacity)\r\n"
               "- **Verdict:** [%s]\r\n"
               "- **Diagnosis:** %s\r\n"
               "- **Data tested:** %.1f GB / %.1f GB (%.1f%%, %d KiB blocks, unbuffered physical I/O)\r\n"
               "- **Read speed:** Avg: %.1f MB/s  |  Min: %.1f MB/s  |  Max: %.1f MB/s\r\n"
               "- **Duration:** %.2f s\r\n"
               "- **Latency timings:** Min: %.2f ms  |  Avg: %.2f ms  |  Max: %.2f ms\r\n\r\n"),
            pResult->szVerdictBadge,
            pResult->szVerdictText,
            doneGb, totalGb, pct,
            LATENCY_FULL_BLOCK_SIZE / 1024,
            pResult->fAvgSpeedMBs, pResult->fMinSpeedMBs, pResult->fMaxSpeedMBs,
            pResult->fTotalTimeSec,
            pResult->fMinMs, pResult->fAvgMs, pResult->fMaxMs);
    } else {
        safe_snprintf(szHdr,
            TN("- **Режим теста:** Экспресс-сканирование времени отклика (128 контрольных точек)\r\n"
               "- **Вердикт:** [%s]\r\n"
               "- **Диагностика:** %s\r\n"
               "- **Проверено блоков:** %d (по %d КиБ, прямое небуферизованное чтение)\r\n"
               "- **Время выполнения:** %.2f с\r\n"
               "- **Скорость чтения:** Средняя: %.1f МБ/с  |  Мин: %.1f МБ/с  |  Макс: %.1f МБ/с\r\n"
               "- **Задержки:** Мин: %.2f мс  |  Среднее: %.2f мс  |  Макс: %.2f мс\r\n\r\n",
               "- **Test mode:** Express latency probe (128 sample points)\r\n"
               "- **Verdict:** [%s]\r\n"
               "- **Diagnosis:** %s\r\n"
               "- **Tested blocks:** %d (%d KiB blocks, unbuffered physical I/O)\r\n"
               "- **Duration:** %.2f s\r\n"
               "- **Read speed:** Avg: %.1f MB/s  |  Min: %.1f MB/s  |  Max: %.1f MB/s\r\n"
               "- **Latency timings:** Min: %.2f ms  |  Avg: %.2f ms  |  Max: %.2f ms\r\n\r\n"),
            pResult->szVerdictBadge,
            pResult->szVerdictText,
            tested,
            LATENCY_BLOCK_SIZE / 1024,
            pResult->fTotalTimeSec,
            pResult->fAvgSpeedMBs, pResult->fMinSpeedMBs, pResult->fMaxSpeedMBs,
            pResult->fMinMs, pResult->fAvgMs, pResult->fMaxMs);
    }

    safe_snprintf_n(pszBuf, nBufSize, "%s", szHdr);
    len = strlen(pszBuf);

    const char* szTblHdr = TN(
        "| Диапазон задержки | Количество секторов | Доля |\r\n"
        "| --- | ---: | ---: |\r\n",
        "| Latency Range | Sector Count | Share |\r\n"
        "| --- | ---: | ---: |\r\n");

    if (len + strlen(szTblHdr) < nBufSize) {
        strcat(pszBuf, szTblHdr);
        len += strlen(szTblHdr);
    }

    for (int b = 0; b < LATENCY_BIN_COUNT; b++) {
        char szRow[128];
        double pct = (double)pResult->nBins[b] * 100.0 / (double)totalBlocks;
        safe_snprintf(szRow, "| %s | %d | %.1f%% |\r\n",
                      LatencyBinName((LATENCY_BIN)b),
                      pResult->nBins[b], pct);
        if (len + strlen(szRow) < nBufSize) {
            strcat(pszBuf, szRow);
            len += strlen(szRow);
        }
    }
}

#define MAX_CACHED_DRIVES 64
static LATENCY_PROBE_RESULT g_CachedResults[MAX_CACHED_DRIVES];
static bool                 g_bCachedValid[MAX_CACHED_DRIVES];

const LATENCY_PROBE_RESULT* GetDriveLatencyResult(int nDriveIndex)
{
    if (nDriveIndex >= 0 && nDriveIndex < MAX_CACHED_DRIVES && g_bCachedValid[nDriveIndex]) {
        return &g_CachedResults[nDriveIndex];
    }
    return NULL;
}

void SetDriveLatencyResult(int nDriveIndex, const LATENCY_PROBE_RESULT* pResult)
{
    if (nDriveIndex >= 0 && nDriveIndex < MAX_CACHED_DRIVES && pResult) {
        g_CachedResults[nDriveIndex] = *pResult;
        g_bCachedValid[nDriveIndex] = true;
    }
}

void ClearDriveLatencyResults(void)
{
    memset(g_CachedResults, 0, sizeof(g_CachedResults));
    memset(g_bCachedValid, 0, sizeof(g_bCachedValid));
}
