/* DriveMonitor - TRIM and Volume Capacity Monitor. GPL-2.0-or-later. See LICENSE.
 *
 * Monitors SSD TRIM status and filesystem volume capacity:
 *  - Checks hardware/driver TRIM support (StorageDeviceTrimProperty);
 *  - Checks Windows OS global delete notification (DisableDeleteNotify in registry);
 *  - Enumerates logical drive volumes (C:, D:, etc.) mapped to the physical drive;
 *  - Detects filesystems lacking TRIM support (FAT32, exFAT);
 *  - Detects critical volume space exhaustion (< 7% and < 5% free space)
 *    which causes FTL block depletion, write amplification spike, and Protect Mode lock.
 */

#ifndef VOLUME_TRIM_H_INCLUDED
#define VOLUME_TRIM_H_INCLUDED

#include <windows.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MAX_DRIVE_VOLUMES 8

typedef struct _VOLUME_INFO_ENTRY {
    char szLetter[4];             /* e.g. "C:" */
    char szFileSystem[16];        /* e.g. "NTFS", "FAT32", "exFAT", "ReFS" */
    unsigned __int64 qwTotalBytes;
    unsigned __int64 qwFreeBytes;
    float fFreePercent;           /* 0.0 to 100.0 */
    BOOL bTrimSupportedFs;        /* TRUE for NTFS / ReFS, FALSE for FAT32 / exFAT */
} VOLUME_INFO_ENTRY;

typedef struct _DRIVE_FS_TRIM_INFO {
    BOOL bQueried;
    BOOL bDeviceTrimSupported;    /* Device / driver level: StorageDeviceTrimProperty TrimEnabled */
    BOOL bOsTrimEnabled;          /* OS level: DisableDeleteNotify == 0 */
    BOOL bHasNonTrimVolume;       /* One or more volumes formatted as FAT32 / exFAT on SSD */
    BOOL bCriticallyLowSpace;     /* One or more volumes have < 7% free space on SSD */
    BOOL bPanicLowSpace;          /* One or more volumes have < 5% free space on SSD */
    int  nVolumeCount;
    VOLUME_INFO_ENTRY volumes[MAX_DRIVE_VOLUMES];
    float fMinFreePercent;        /* Minimum free % across all volumes (-1.0 if no volumes) */
    char szTrimSummary[96];       /* Human-readable status summary */
} DRIVE_FS_TRIM_INFO;

struct _DRIVE_INFO;

/* Query volume partitions, filesystem types, free space, and TRIM state for drives */
void QueryDriveVolumesAndTrim(struct _DRIVE_INFO* pDrives, int nNumDrives);

/* Query whether Windows global delete notification (TRIM) is enabled */
BOOL QueryOsTrimEnabled(void);

/* Check if a filesystem name supports TRIM (NTFS and ReFS support TRIM in Windows) */
BOOL IsFileSystemTrimSupported(const char* szFsName);

/* Query whether a physical drive handle supports TRIM */
BOOL QueryDeviceTrimProperty(HANDLE hDrive);

/* Helper to evaluate TRIM health impact */
void EvaluateDriveTrimAndCapacity(struct _DRIVE_INFO* pInfo);

#ifdef __cplusplus
}
#endif

#endif /* VOLUME_TRIM_H_INCLUDED */
