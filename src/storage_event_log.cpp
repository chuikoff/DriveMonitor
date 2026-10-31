/* DriveMonitor - Windows Storage Event Log Monitor. GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winevt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "storage_event_log.h"
#include "smart.h"
#include "safestr.h"

void ClearStorageEventStats(STORAGE_EVENT_STATS* pStats)
{
    if (!pStats) return;
    ZeroMemory(pStats, sizeof(STORAGE_EVENT_STATS));
}

static const wchar_t* FindCaseInsensitiveW(const wchar_t* haystack, const wchar_t* needle)
{
    if (!haystack || !needle) return NULL;
    size_t needleLen = wcslen(needle);
    if (needleLen == 0) return haystack;

    for (const wchar_t* h = haystack; *h; h++) {
        if (_wcsnicmp(h, needle, needleLen) == 0)
            return h;
    }
    return NULL;
}

static BOOL MatchDeviceTarget(const wchar_t* pwszXml, const wchar_t* wszTarget)
{
    if (!pwszXml || !wszTarget || !wszTarget[0]) return FALSE;
    size_t targetLen = wcslen(wszTarget);
    const wchar_t* p = pwszXml;
    while ((p = FindCaseInsensitiveW(p, wszTarget)) != NULL) {
        wchar_t next = p[targetLen];
        /* Suffix must not be an alphanumeric digit (e.g. DR2 vs DR28) */
        if (next == L'\0' || next == L'<' || next == L'\\' || next == L'/' || next == L' ' || next == L'\r' || next == L'\n' || next == L'"' || next == L'\'') {
            return TRUE;
        }
        p += targetLen;
    }
    return FALSE;
}

BOOL ParseStorageEventXml(const wchar_t* pwszXml, DRIVE_INFO* pDrives, int nNumDrives)
{
    if (!pwszXml || !pDrives || nNumDrives <= 0) return FALSE;

    /* 1. Extract Provider Name */
    wchar_t wszProvider[64];
    wszProvider[0] = L'\0';
    const wchar_t* pProv = FindCaseInsensitiveW(pwszXml, L"<Provider Name=\"");
    size_t prefixLen = 16;
    if (!pProv) {
        pProv = FindCaseInsensitiveW(pwszXml, L"<Provider Name=\'");
        prefixLen = 16;
    }
    if (pProv) {
        pProv += prefixLen;
        size_t len = 0;
        while (*pProv && *pProv != L'\"' && *pProv != L'\'' && len + 1 < sizeof(wszProvider)/sizeof(wszProvider[0])) {
            wszProvider[len++] = *pProv++;
        }
        wszProvider[len] = L'\0';
    }

    /* 2. Extract EventID */
    int nEventId = 0;
    const wchar_t* pEvtId = FindCaseInsensitiveW(pwszXml, L"<EventID");
    if (pEvtId) {
        const wchar_t* pClose = wcschr(pEvtId, L'>');
        if (pClose) {
            nEventId = (int)wcstol(pClose + 1, NULL, 10);
        }
    }
    if (nEventId == 0) return FALSE;

    /* 3. Extract TimeCreated SystemTime */
    char szTime[32];
    szTime[0] = '\0';
    const wchar_t* pTime = FindCaseInsensitiveW(pwszXml, L"SystemTime=\"");
    prefixLen = 12;
    if (!pTime) {
        pTime = FindCaseInsensitiveW(pwszXml, L"SystemTime=\'");
        prefixLen = 12;
    }
    if (pTime) {
        pTime += prefixLen;
        char rawIso[40];
        size_t len = 0;
        while (*pTime && *pTime != L'\"' && *pTime != L'\'' && len + 1 < sizeof(rawIso)) {
            rawIso[len++] = (char)*pTime++;
        }
        rawIso[len] = '\0';
        /* Convert "2026-10-28T08:56:02.123456Z" -> "2026-10-28 08:56:02" */
        if (len >= 19) {
            rawIso[10] = ' ';
            rawIso[19] = '\0';
            safe_snprintf(szTime, "%s", rawIso);
        }
    }

    /* 4. Match to target drive(s) */
    BOOL bMatched = FALSE;

    /* If event is from 'disk' driver (153, 51, 7, 154), look for \Device\HarddiskX */
    const wchar_t* pHarddisk = FindCaseInsensitiveW(pwszXml, L"\\Device\\Harddisk");
    if (pHarddisk) {
        pHarddisk += 16; /* length of "\\Device\\Harddisk" */
        wchar_t* pEnd = NULL;
        long nDisk = wcstol(pHarddisk, &pEnd, 10);
        if (pEnd != pHarddisk && nDisk >= 0) {
            for (int i = 0; i < nNumDrives; i++) {
                if (pDrives[i].nDriveIndex == (int)nDisk) {
                    if (pDrives[i].szDeviceTarget[0] != '\0') {
                        wchar_t wszTarget[64];
                        MultiByteToWideChar(CP_UTF8, 0, pDrives[i].szDeviceTarget, -1, wszTarget, 64);
                        if (!MatchDeviceTarget(pwszXml, wszTarget)) {
                            /* If the XML specifies a specific \DR instance or this is a removable/USB drive,
                             * it belongs to a different device instance; do not attribute to this drive. */
                            if (FindCaseInsensitiveW(pHarddisk, L"\\DR") || pDrives[i].bIsUSB) {
                                continue;
                            }
                        }
                    }
                    DRIVE_INFO* pMatch = &pDrives[i];
                    bMatched = TRUE;

                    if (nEventId == 153) {
                        pMatch->eventStats.nEvent153Count++;
                        if (pMatch->eventStats.szLastEventDesc[0] == '\0') {
                            safe_snprintf(pMatch->eventStats.szLastEventDesc, "Event 153 (IO retry timeout)");
                        }
                    } else if (nEventId == 51) {
                        pMatch->eventStats.nEvent51Count++;
                        if (pMatch->eventStats.szLastEventDesc[0] == '\0') {
                            safe_snprintf(pMatch->eventStats.szLastEventDesc, "Event 51 (Paging operation error)");
                        }
                    } else if (nEventId == 7) {
                        pMatch->eventStats.nEventHwErrCount++;
                        if (pMatch->eventStats.szLastEventDesc[0] == '\0') {
                            safe_snprintf(pMatch->eventStats.szLastEventDesc, "Event 7 (Bad block detected)");
                        }
                    } else if (nEventId == 154) {
                        pMatch->eventStats.nEventHwErrCount++;
                        if (pMatch->eventStats.szLastEventDesc[0] == '\0') {
                            safe_snprintf(pMatch->eventStats.szLastEventDesc, "Event 154 (Drive inaccessible/corrupt)");
                        }
                    }

                    if (pMatch->eventStats.szLastEventTime[0] == '\0' && szTime[0] != '\0') {
                        safe_snprintf(pMatch->eventStats.szLastEventTime, "%s", szTime);
                    }
                }
            }
            return bMatched;
        }
    }

    /* If event is 129 (bus/controller reset from storahci/stornvme/UASPStor/etc.), look for \Device\RaidPortX */
    const wchar_t* pRaidPort = FindCaseInsensitiveW(pwszXml, L"\\Device\\RaidPort");
    if (pRaidPort) {
        pRaidPort += 16; /* length of "\\Device\\RaidPort" */
        wchar_t* pEnd = NULL;
        long nPort = wcstol(pRaidPort, &pEnd, 10);
        if (pEnd != pRaidPort && nPort >= 0) {
            for (int i = 0; i < nNumDrives; i++) {
                if (pDrives[i].nScsiPort == (int)nPort) {
                    DRIVE_INFO* pMatch = &pDrives[i];
                    bMatched = TRUE;

                    pMatch->eventStats.nEvent129Count++;
                    if (pMatch->eventStats.szLastEventDesc[0] == '\0') {
                        safe_snprintf(pMatch->eventStats.szLastEventDesc, "Event 129 (Controller reset \\Device\\RaidPort%d)", (int)nPort);
                    }
                    if (pMatch->eventStats.szLastEventTime[0] == '\0' && szTime[0] != '\0') {
                        safe_snprintf(pMatch->eventStats.szLastEventTime, "%s", szTime);
                    }
                }
            }
            return bMatched;
        }
    }

    return FALSE;
}

void QueryStorageEventLogs(DRIVE_INFO* pDrives, int nNumDrives, int nDays)
{
    if (!pDrives || nNumDrives <= 0) return;
    if (nDays <= 0) nDays = 30;

    for (int i = 0; i < nNumDrives; i++) {
        pDrives[i].eventStats.bQueried = TRUE;
    }

    unsigned long long ms = (unsigned long long)nDays * 24ULL * 3600ULL * 1000ULL;
    wchar_t wszQuery[512];
    _snwprintf(wszQuery, sizeof(wszQuery)/sizeof(wszQuery[0]),
        L"*[System[(Level=1 or Level=2 or Level=3) and "
        L"TimeCreated[timediff(@SystemTime) <= %I64u] and "
        L"((Provider[@Name='disk'] and (EventID=153 or EventID=51 or EventID=7 or EventID=154)) or "
        L"((Provider[@Name='storahci' or @Name='stornvme' or @Name='UASPStor' or @Name='iaStorA' or @Name='iaStorVD' or @Name='nvme']) and EventID=129))"
        L"]]",
        ms);

    EVT_HANDLE hResults = EvtQuery(NULL, L"System", wszQuery,
                                   EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!hResults) {
        return;
    }

    const DWORD BATCH_SIZE = 32;
    EVT_HANDLE hEvents[32];
    DWORD dwReturned = 0;
    DWORD dwTotalProcessed = 0;
    const DWORD MAX_EVENTS_TO_PROCESS = 500;

    DWORD dwBufferSize = 8192;
    wchar_t* pXmlBuffer = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, dwBufferSize);

    while (dwTotalProcessed < MAX_EVENTS_TO_PROCESS &&
           EvtNext(hResults, BATCH_SIZE, hEvents, 2000, 0, &dwReturned)) {
        for (DWORD i = 0; i < dwReturned; i++) {
            DWORD dwBufferUsed = 0;
            DWORD dwPropertyCount = 0;

            if (pXmlBuffer) {
                if (!EvtRender(NULL, hEvents[i], EvtRenderEventXml, dwBufferSize,
                               pXmlBuffer, &dwBufferUsed, &dwPropertyCount)) {
                    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && dwBufferUsed > dwBufferSize) {
                        wchar_t* pNew = (wchar_t*)HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                              pXmlBuffer, dwBufferUsed + 1024);
                        if (pNew) {
                            pXmlBuffer = pNew;
                            dwBufferSize = dwBufferUsed + 1024;
                            EvtRender(NULL, hEvents[i], EvtRenderEventXml, dwBufferSize,
                                      pXmlBuffer, &dwBufferUsed, &dwPropertyCount);
                        }
                    }
                }

                ParseStorageEventXml(pXmlBuffer, pDrives, nNumDrives);
            }
            EvtClose(hEvents[i]);
            dwTotalProcessed++;
        }
    }

    if (pXmlBuffer) {
        HeapFree(GetProcessHeap(), 0, pXmlBuffer);
    }
    EvtClose(hResults);
}
