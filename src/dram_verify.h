/* DriveMonitor - DRAM Buffer Integrity & Silent Data Corruption (Bit Flip) Test.
 * GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef DRAM_VERIFY_H
#define DRAM_VERIFY_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DRAM_BLOCK_SIZE           1048576   /* 1 MiB sector-aligned unbuffered I/O block */
#define DRAM_EXPRESS_BYTES        (32ULL  * 1024ULL * 1024ULL)  /* 32 MiB express test */
#define DRAM_STANDARD_BYTES       (128ULL * 1024ULL * 1024ULL)  /* 128 MiB standard test */
#define DRAM_STRESS_BYTES         (512ULL * 1024ULL * 1024ULL)  /* 512 MiB full DRAM cache stress */
#define DRAM_MAX_RECORDED_FLIPS   32

/* Test operating modes */
typedef enum _DRAM_VERIFY_MODE {
    DRAM_VERIFY_MODE_EXPRESS  = 0, /* 32 MiB quick active buffer test (~1-2s) */
    DRAM_VERIFY_MODE_STANDARD = 1, /* 128 MiB balanced stress test */
    DRAM_VERIFY_MODE_STRESS   = 2  /* 512 MiB full DRAM cache stress test */
} DRAM_VERIFY_MODE;

/* Test pattern types */
typedef enum _DRAM_PATTERN_TYPE {
    DRAM_PATTERN_PRBS         = 0, /* Deterministic Pseudo-Random stream (PRBS31 / Xorshift64) */
    DRAM_PATTERN_CHECKERBOARD = 1, /* Alternating bit checkerboard 0xAA / 0x55 (maximum bus crosstalk) */
    DRAM_PATTERN_WALKING_BITS = 2, /* Walking 1s and walking 0s (stuck-at bit line detection) */
    DRAM_PATTERN_SOLID_INV    = 3, /* 0x00000000 / 0xFFFFFFFF solid inversion (charge retention/leakage) */
    DRAM_PATTERN_COUNT        = 4
} DRAM_PATTERN_TYPE;

/* Current execution stage */
typedef enum _DRAM_VERIFY_STAGE {
    DRAM_STAGE_IDLE               = 0,
    DRAM_STAGE_PREPARING          = 1,
    DRAM_STAGE_WRITING            = 2,
    DRAM_STAGE_FLUSHING           = 3,
    DRAM_STAGE_READING_VERIFYING  = 4,
    DRAM_STAGE_COMPLETED          = 5,
    DRAM_STAGE_ABORTED            = 6,
    DRAM_STAGE_FAILED             = 7
} DRAM_VERIFY_STAGE;

/* Single detected bit flip sample */
typedef struct _DRAM_BITFLIP_SAMPLE {
    uint64_t nByteOffset;     /* Byte offset in file */
    uint8_t  bExpected;       /* Expected byte value */
    uint8_t  bActual;         /* Read byte value */
    uint8_t  bFlippedBits;    /* XOR difference: which bits flipped */
} DRAM_BITFLIP_SAMPLE;

/* Complete result of DRAM verification test */
typedef struct _DRAM_VERIFY_RESULT {
    DRAM_VERIFY_MODE    eMode;
    uint64_t            nBytesTested;
    uint64_t            nTotalBytesToTest;
    DRAM_VERIFY_STAGE   eStage;
    double              fWriteSpeedMBs;
    double              fReadSpeedMBs;
    double              fDurationSec;
    uint64_t            nCorruptedBytes;
    uint64_t            nTotalBitFlips;
    int                 nRecordedFlips;
    DRAM_BITFLIP_SAMPLE flips[DRAM_MAX_RECORDED_FLIPS];
    char                szTestedVolume[32];     /* e.g. "C:\" */
    char                szTempFilePath[MAX_PATH];
    char                szVerdictBadge[64];     /* "БУФЕР ИСПРАВЕН" / "СБОЙ БУФЕРА (BIT FLIP)" */
    char                szVerdictText[1024];    /* Physical diagnosis explanation */
    bool                bPassed;
    bool                bAborted;
    DWORD               dwWin32Error;
} DRAM_VERIFY_RESULT;

/* Progress callback signature: return false to abort/cancel */
typedef bool (*DramVerifyProgressCallback)(
    const DRAM_VERIFY_RESULT* pResult,
    void* pUserData
);

/* Fill buffer with deterministic stress test pattern */
void DramFillPattern(void* pBuffer, size_t nBytes, uint64_t nBlockOffset, DRAM_PATTERN_TYPE ePattern);

/* Count flipped bits in byte XOR mask */
static inline uint32_t DramCountFlippedBits(uint8_t bXor)
{
    return (uint32_t)__builtin_popcount((unsigned int)bXor);
}

/* Compare read buffer with expected buffer, record sample bit-flips */
size_t DramVerifyBuffer(const void* pActual,
                        const void* pExpected,
                        size_t nBytes,
                        uint64_t nBaseOffset,
                        DRAM_VERIFY_RESULT* pResult);

/* Assess diagnostic verdict based on test results */
void DramVerifyAssessVerdict(DRAM_VERIFY_RESULT* pResult);

/* Get pattern human-readable name */
const char* DramPatternName(DRAM_PATTERN_TYPE ePattern);

/* Format human-readable summary report */
void FormatDramVerifySummary(const DRAM_VERIFY_RESULT* pResult,
                             const char* pszModel,
                             const char* pszSerial,
                             char* pszBuf,
                             size_t nBufSize);

/* Format Markdown summary report for export */
void FormatDramVerifyMarkdown(const DRAM_VERIFY_RESULT* pResult,
                              char* pszBuf,
                              size_t nBufSize);

/* Run complete unbuffered Read-After-Write verification test on specified volume */
bool RunDramVerification(const char* pszVolumeRoot,
                         DRAM_VERIFY_MODE eMode,
                         DRAM_VERIFY_RESULT* pResult,
                         DramVerifyProgressCallback pCallback,
                         void* pUserData,
                         volatile bool* pbCancel);

/* Cache management for DRAM test results per drive */
void SetDriveDramVerifyResult(int nDriveIndex, const DRAM_VERIFY_RESULT* pResult);
const DRAM_VERIFY_RESULT* GetDriveDramVerifyResult(int nDriveIndex);
void ClearDriveDramVerifyResults(void);

#ifdef __cplusplus
}
#endif

#endif /* DRAM_VERIFY_H */
