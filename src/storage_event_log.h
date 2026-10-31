/* DriveMonitor - Windows Storage Event Log Monitor. GPL-2.0-or-later. See LICENSE.
 *
 * Scans the Windows Event Log (System channel) for storage subsystem warnings and errors:
 *  - Event 153 (Source: disk): IO operation at LBA was retried due to timeout.
 *  - Event 129 (Source: storahci/stornvme/UASPStor/iaStorA/iaStorVD/nvme): Reset to device (\Device\RaidPortN) was issued.
 *  - Event 51  (Source: disk): Error detected during a paging operation.
 *  - Event 7   (Source: disk): Device has a bad block.
 *  - Event 154 (Source: disk): IO operation failed because the drive is corrupted or inaccessible.
 */

#ifndef STORAGE_EVENT_LOG_H_INCLUDED
#define STORAGE_EVENT_LOG_H_INCLUDED

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Statistics on storage-related Windows Event Log records (e.g. within last 30 days) */
typedef struct _STORAGE_EVENT_STATS {
    BOOL bQueried;
    int  nEvent153Count;        /* IO retries: Event ID 153 (Source: disk) */
    int  nEvent129Count;        /* Bus/Controller resets: Event ID 129 (Source: storahci/stornvme/UASPStor/etc.) */
    int  nEvent51Count;         /* Paging errors: Event ID 51 (Source: disk) */
    int  nEventHwErrCount;      /* Bad block / hardware errors: Event ID 7 or 154 (Source: disk) */
    char szLastEventTime[32];   /* Date & time of the most recent storage event (e.g. "2026-10-28 11:56:02") */
    char szLastEventDesc[128];  /* Short human-readable summary of the most recent event */
} STORAGE_EVENT_STATS;

struct _DRIVE_INFO;

/* Scan the Windows Event Log (System channel) for storage subsystem warnings/errors
 * occurring within nDays (default: 30 days) and attribute them to the drives in pDrives. */
void QueryStorageEventLogs(struct _DRIVE_INFO* pDrives, int nNumDrives, int nDays);

/* Parse an XML event string from winevt and update drive statistics.
 * Exposed for unit testing without requiring live Event Log access. */
BOOL ParseStorageEventXml(const wchar_t* pwszXml, struct _DRIVE_INFO* pDrives, int nNumDrives);

/* Clear / reset event stats structure */
void ClearStorageEventStats(STORAGE_EVENT_STATS* pStats);

#ifdef __cplusplus
}
#endif

#endif /* STORAGE_EVENT_LOG_H_INCLUDED */
