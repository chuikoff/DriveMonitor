#pragma once
/* DriveMonitor - Drive Self-Test (DST) and Hardware Error Logs.
 * GPL-2.0-or-later. See LICENSE. */

#include <windows.h>
#include <stdint.h>
#include "smart.h"

typedef enum _DST_TEST_TYPE {
    DST_TEST_ABORT      = 0,
    DST_TEST_SHORT      = 1,
    DST_TEST_EXTENDED   = 2,
    DST_TEST_CONVEYANCE = 3
} DST_TEST_TYPE;

typedef struct _DST_PROGRESS {
    BOOL                 bRunning;
    int                  nPercentComplete; /* 0..100 */
    SELFTEST_STATUS_CODE eStatusCode;
    char                 szStatusText[160];
    uint64_t             nFailingLBA;
    int                  nEstimatedMinutesLeft;
} DST_PROGRESS;

/* Start or Abort Drive Self-Test on a physical drive */
BOOL DstStartSelfTest(HANDLE hDrive, DRIVE_INFO* pInfo, DST_TEST_TYPE eType, char* szErr, int nErrLen);
BOOL DstAbortSelfTest(HANDLE hDrive, DRIVE_INFO* pInfo, char* szErr, int nErrLen);

/* Poll current self-test progress */
BOOL DstPollProgress(HANDLE hDrive, DRIVE_INFO* pInfo, DST_PROGRESS* pProgress);

/* Query and parse self-test and hardware error logs */
BOOL DstQueryAtaLogs(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL DstQueryNvmeLogs(HANDLE hDrive, DRIVE_INFO* pInfo);

/* Format log histories for UI display and Markdown export */
int  DstFormatSelfTestHistory(const DRIVE_INFO* pInfo, char* szOut, int nOutLen);
int  DstFormatErrorLogHistory(const DRIVE_INFO* pInfo, char* szOut, int nOutLen);

/* UI Dialog (implemented in mainwnd.cpp) */
void ShowSelfTestDialog(HWND hParent, int nDriveIndex);
