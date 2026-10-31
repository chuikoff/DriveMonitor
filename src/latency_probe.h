/* DriveMonitor - Latency Probe / Surface Benchmark. GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef LATENCY_PROBE_H
#define LATENCY_PROBE_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LATENCY_SAMPLES_DEFAULT  128
#define LATENCY_SAMPLES_MAX      256
#define LATENCY_BLOCK_SIZE       65536    /* 64 KiB unbuffered sector-aligned read for express probe */
#define LATENCY_FULL_BLOCK_SIZE  1048576  /* 1 MiB unbuffered block for full sequential read benchmark */

/* Probe operating modes */
typedef enum _LATENCY_PROBE_MODE {
    LATENCY_MODE_EXPRESS = 0, /* Express 128-point distributed probe */
    LATENCY_MODE_FULL    = 1  /* Full 100% surface sequential read benchmark */
} LATENCY_PROBE_MODE;

/* Latency categorization bins */
typedef enum _LATENCY_BIN {
    LATENCY_BIN_FAST        = 0,  /* < 10 ms (Green) */
    LATENCY_BIN_NORMAL      = 1,  /* 10 .. 50 ms (Blue) */
    LATENCY_BIN_ACCEPTABLE  = 2,  /* 50 .. 150 ms (Yellow) */
    LATENCY_BIN_SLOW        = 3,  /* 150 .. 500 ms (Orange - LDPC soft-decision / multi-seek) */
    LATENCY_BIN_CRITICAL    = 4,  /* >= 500 ms (Red - severe retention loss / weak head) */
    LATENCY_BIN_ERROR       = 5,  /* Unreadable / I/O error (Purple/Crimson) */
    LATENCY_BIN_COUNT       = 6
} LATENCY_BIN;

/* Overall diagnostic verdict level */
typedef enum _LATENCY_VERDICT_LEVEL {
    LATENCY_VERDICT_EXCELLENT = 0, /* All fast, no degradation */
    LATENCY_VERDICT_GOOD      = 1, /* Normal response times */
    LATENCY_VERDICT_CAUTION   = 2, /* 150-500 ms blocks detected (LDPC active / seek lag) */
    LATENCY_VERDICT_WARNING   = 3, /* > 500 ms blocks detected (Retention loss / weak head) */
    LATENCY_VERDICT_BAD       = 4  /* Unreadable blocks / I/O errors */
} LATENCY_VERDICT_LEVEL;

/* Single sample point */
typedef struct _LATENCY_SAMPLE {
    uint64_t    nOffsetBytes;     /* Byte offset on physical drive */
    double      fLatencyMs;       /* Measured read latency in milliseconds */
    double      fSpeedMBs;        /* Transfer speed in MB/s */
    LATENCY_BIN bin;              /* Categorized bin */
    DWORD       dwWin32Error;     /* 0 on success, or Win32 GetLastError() */
} LATENCY_SAMPLE;

/* Aggregate result of latency probe */
typedef struct _LATENCY_PROBE_RESULT {
    LATENCY_PROBE_MODE    eMode;                /* Express sample vs Full surface */
    int                   nTotalSamples;
    int                   nSamplesTested;
    uint64_t              nBytesTested;         /* Total bytes actually processed */
    uint64_t              nTotalCapacityBytes;  /* Total physical drive capacity */
    double                fCurrentSpeedMBs;     /* Real-time transfer rate in MB/s */
    double                fAvgSpeedMBs;         /* Average transfer rate in MB/s */
    double                fMinSpeedMBs;         /* Minimum transfer rate in MB/s */
    double                fMaxSpeedMBs;         /* Maximum transfer rate in MB/s */
    double                fEtaSeconds;          /* Estimated remaining time in seconds */
    int                   nBins[LATENCY_BIN_COUNT];
    double                fMinMs;
    double                fMaxMs;
    double                fAvgMs;
    double                fTotalTimeSec;
    bool                  bAborted;
    LATENCY_VERDICT_LEVEL eVerdictLevel;
    char                  szVerdictBadge[32];   /* e.g. "ОТЛИЧНО", "ВНИМАНИЕ", "СБОЙ" */
    char                  szVerdictText[512];   /* Detailed physical diagnosis */
    LATENCY_SAMPLE        samples[LATENCY_SAMPLES_MAX];
} LATENCY_PROBE_RESULT;

/* Callback signature: return false to abort/cancel probe */
typedef bool (*LatencyProbeCallback)(
    int nCurrentSample,        /* 0-indexed */
    int nTotalSamples,
    const LATENCY_SAMPLE* pSample,
    void* pUserData
);

/* Bin classification and metadata */
LATENCY_BIN LatencyBinFromMs(double fLatencyMs, DWORD dwWin32Error);
const char* LatencyBinName(LATENCY_BIN bin);
const char* LatencyBinRange(LATENCY_BIN bin);
COLORREF    LatencyBinColor(LATENCY_BIN bin);

/* Verdict evaluation based on SSD vs HDD context */
void LatencyProbeAssessVerdict(LATENCY_PROBE_RESULT* pResult, bool bIsSsd);

/* Format human-readable summary report (bilingual according to UiLangIsEn()) */
void FormatLatencySummary(const LATENCY_PROBE_RESULT* pResult,
                          const char* pszModel,
                          const char* pszSerial,
                          uint64_t nCapacityBytes,
                          bool bIsSsd,
                          char* pszBuf,
                          size_t nBufSize);

/* Physical probe execution on \\.\PhysicalDriveN (unbuffered sector-aligned reads) */
bool RunLatencyProbeEx(int nDriveIndex,
                       uint64_t nTotalCapacityBytes,
                       LATENCY_PROBE_MODE eMode,
                       int nSampleCount,
                       LATENCY_PROBE_RESULT* pResult,
                       LatencyProbeCallback pCallback,
                       void* pUserData,
                       volatile bool* pbCancel);

bool RunLatencyProbe(int nDriveIndex,
                     uint64_t nTotalCapacityBytes,
                     int nSampleCount,
                     LATENCY_PROBE_RESULT* pResult,
                     LatencyProbeCallback pCallback,
                     void* pUserData,
                     volatile bool* pbCancel);

/* Format markdown report section for inclusion in drive reports */
void FormatLatencyMarkdown(const LATENCY_PROBE_RESULT* pResult,
                           char* pszBuf,
                           size_t nBufSize);

/* Cached results per PhysicalDrive index */
const LATENCY_PROBE_RESULT* GetDriveLatencyResult(int nDriveIndex);
void SetDriveLatencyResult(int nDriveIndex, const LATENCY_PROBE_RESULT* pResult);
void ClearDriveLatencyResults(void);

#ifdef __cplusplus
}
#endif

#endif /* LATENCY_PROBE_H */
