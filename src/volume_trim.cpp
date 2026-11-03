/* DriveMonitor - TRIM and Volume Capacity Monitor. GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "volume_trim.h"
#include "smart.h"
#include "safestr.h"
#include "lang.h"

BOOL IsFileSystemTrimSupported(const char* szFsName)
{
    if (!szFsName || !szFsName[0]) return FALSE;
    if (_stricmp(szFsName, "NTFS") == 0) return TRUE;
    if (_stricmp(szFsName, "ReFS") == 0) return TRUE;
    return FALSE;
}

BOOL QueryOsTrimEnabled(void)
{
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                      0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        DWORD dwVal = 0;
        DWORD dwSize = sizeof(dwVal);
        LONG lRes = RegQueryValueExA(hKey, "DisableDeleteNotify", NULL, NULL, (LPBYTE)&dwVal, &dwSize);
        RegCloseKey(hKey);
        if (lRes == ERROR_SUCCESS) {
            /* Bit 0 is NTFS DisableDeleteNotify (1 = disabled, 0 = enabled) */
            if ((dwVal & 1) != 0) {
                return FALSE;
            }
        }
    }
    /* By default in modern Windows, TRIM is enabled */
    return TRUE;
}

BOOL QueryDeviceTrimProperty(HANDLE hDrive)
{
    if (!hDrive || hDrive == INVALID_HANDLE_VALUE) return FALSE;

    STORAGE_PROPERTY_QUERY q;
    ZeroMemory(&q, sizeof(q));
    q.PropertyId = (STORAGE_PROPERTY_ID)StorageDeviceTrimProperty;
    q.QueryType = PropertyStandardQuery;

    DEVICE_TRIM_DESCRIPTOR desc;
    ZeroMemory(&desc, sizeof(desc));
    DWORD dwRet = 0;

    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof(q),
                         &desc, sizeof(desc), &dwRet, NULL)) {
        return (desc.TrimEnabled != 0);
    }
    return FALSE;
}

void EvaluateDriveTrimAndCapacity(DRIVE_INFO* pInfo)
{
    if (!pInfo) return;

    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
        safe_snprintf(pInfo->trimInfo.szTrimSummary, "%s",
            TN("HDD (TRIM не требуется)", "HDD (TRIM not applicable)"));
        return;
    }

    /* Compute flags across volumes */
    pInfo->trimInfo.bHasNonTrimVolume = FALSE;
    pInfo->trimInfo.bCriticallyLowSpace = FALSE;
    pInfo->trimInfo.bPanicLowSpace = FALSE;
    pInfo->trimInfo.fMinFreePercent = -1.0f;

    for (int i = 0; i < pInfo->trimInfo.nVolumeCount; i++) {
        const VOLUME_INFO_ENTRY* v = &pInfo->trimInfo.volumes[i];
        if (v->qwTotalBytes > 0 && !v->bTrimSupportedFs) {
            pInfo->trimInfo.bHasNonTrimVolume = TRUE;
        }
        if (v->fFreePercent >= 0.0f) {
            if (pInfo->trimInfo.fMinFreePercent < 0.0f || v->fFreePercent < pInfo->trimInfo.fMinFreePercent) {
                pInfo->trimInfo.fMinFreePercent = v->fFreePercent;
            }
            if (v->fFreePercent < 7.0f) pInfo->trimInfo.bCriticallyLowSpace = TRUE;
            if (v->fFreePercent < 5.0f) pInfo->trimInfo.bPanicLowSpace = TRUE;
        }
    }

    /* For SSD / NVMe */
    if (!pInfo->trimInfo.bOsTrimEnabled) {
        safe_snprintf(pInfo->trimInfo.szTrimSummary, "%s",
            TN("ОТКЛЮЧЕН в Windows (DisableDeleteNotify)", "DISABLED in Windows (DisableDeleteNotify)"));
    } else if (pInfo->trimInfo.bHasNonTrimVolume) {
        safe_snprintf(pInfo->trimInfo.szTrimSummary, "%s",
            TN("Частично (разделы FAT32/exFAT без TRIM)", "Partial (FAT32/exFAT without TRIM)"));
    } else if (!pInfo->trimInfo.bDeviceTrimSupported && !pInfo->bIsNVMe) {
        safe_snprintf(pInfo->trimInfo.szTrimSummary, "%s",
            TN("Не поддерживается мостом/накопителем", "Not supported by bridge/device"));
    } else {
        safe_snprintf(pInfo->trimInfo.szTrimSummary, "%s",
            TN("Активен (NTFS/ReFS)", "Active (NTFS/ReFS)"));
    }
}

void QueryDriveVolumesAndTrim(DRIVE_INFO* pDrives, int nNumDrives)
{
    if (!pDrives || nNumDrives <= 0) return;

    BOOL bOsTrim = QueryOsTrimEnabled();

    for (int i = 0; i < nNumDrives; i++) {
        pDrives[i].trimInfo.bQueried = TRUE;
        pDrives[i].trimInfo.bOsTrimEnabled = bOsTrim;
        pDrives[i].trimInfo.bDeviceTrimSupported = FALSE;
        pDrives[i].trimInfo.bHasNonTrimVolume = FALSE;
        pDrives[i].trimInfo.bCriticallyLowSpace = FALSE;
        pDrives[i].trimInfo.bPanicLowSpace = FALSE;
        pDrives[i].trimInfo.nVolumeCount = 0;
        pDrives[i].trimInfo.fMinFreePercent = -1.0f;
        pDrives[i].trimInfo.szTrimSummary[0] = '\0';

        /* Query Device Trim Property on physical drive */
        char szPath[64];
        safe_snprintf(szPath, "\\\\.\\PhysicalDrive%d", pDrives[i].nDriveIndex);
        HANDLE hDrive = CreateFileA(szPath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    NULL, OPEN_EXISTING, 0, NULL);
        if (hDrive != INVALID_HANDLE_VALUE) {
            pDrives[i].trimInfo.bDeviceTrimSupported = QueryDeviceTrimProperty(hDrive);
            CloseHandle(hDrive);
        } else if (pDrives[i].bIsNVMe && !pDrives[i].bIsUSB) {
            /* NVMe spec includes Deallocate in Dataset Management */
            pDrives[i].trimInfo.bDeviceTrimSupported = TRUE;
        }
    }

    char letters[256];
    letters[0] = '\0';
    if (!GetLogicalDriveStringsA((DWORD)sizeof(letters), letters)) {
        for (int i = 0; i < nNumDrives; i++) {
            EvaluateDriveTrimAndCapacity(&pDrives[i]);
        }
        return;
    }

    for (const char* p = letters; *p; p += strlen(p) + 1) {
        char vol[16];
        safe_snprintf(vol, "\\\\.\\%c:", p[0]);

        HANDLE hVol = CreateFileA(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  NULL, OPEN_EXISTING, 0, NULL);
        if (hVol == INVALID_HANDLE_VALUE) continue;

        BYTE extBuf[1024];
        ZeroMemory(extBuf, sizeof(extBuf));
        DWORD ret = 0;
        BOOL bGotExtents = DeviceIoControl(hVol, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                                           NULL, 0, extBuf, sizeof(extBuf), &ret, NULL);

        STORAGE_DEVICE_NUMBER sdn;
        ZeroMemory(&sdn, sizeof(sdn));
        BOOL bGotDevNum = FALSE;
        if (!bGotExtents) {
            bGotDevNum = DeviceIoControl(hVol, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                                         NULL, 0, &sdn, sizeof(sdn), &ret, NULL);
        }
        CloseHandle(hVol);

        if (!bGotExtents && !bGotDevNum) continue;

        /* Get filesystem type and free space */
        char root[8];
        safe_snprintf(root, "%c:\\", p[0]);

        char fsName[64];
        fsName[0] = '\0';
        GetVolumeInformationA(root, NULL, 0, NULL, NULL, NULL, fsName, sizeof(fsName));

        ULARGE_INTEGER freeCaller, totalBytes, totalFree;
        ZeroMemory(&freeCaller, sizeof(freeCaller));
        ZeroMemory(&totalBytes, sizeof(totalBytes));
        ZeroMemory(&totalFree, sizeof(totalFree));
        BOOL bGotSpace = GetDiskFreeSpaceExA(root, &freeCaller, &totalBytes, &totalFree);

        float fFreePct = -1.0f;
        if (bGotSpace && totalBytes.QuadPart > 0) {
            fFreePct = (float)((double)totalFree.QuadPart / (double)totalBytes.QuadPart * 100.0);
        }

        BOOL bTrimFs = IsFileSystemTrimSupported(fsName);

        /* Map this volume to matching physical drives */
        for (int d = 0; d < nNumDrives; d++) {
            BOOL bMatched = FALSE;
            if (bGotExtents) {
                VOLUME_DISK_EXTENTS* pExt = (VOLUME_DISK_EXTENTS*)extBuf;
                for (DWORD e = 0; e < pExt->NumberOfDiskExtents; e++) {
                    if ((int)pExt->Extents[e].DiskNumber == pDrives[d].nDriveIndex) {
                        bMatched = TRUE;
                        break;
                    }
                }
            } else if (bGotDevNum) {
                if ((int)sdn.DeviceNumber == pDrives[d].nDriveIndex) {
                    bMatched = TRUE;
                }
            }

            if (bMatched) {
                DRIVE_FS_TRIM_INFO* pTrim = &pDrives[d].trimInfo;
                if (pTrim->nVolumeCount < MAX_DRIVE_VOLUMES) {
                    VOLUME_INFO_ENTRY* pEntry = &pTrim->volumes[pTrim->nVolumeCount++];
                    safe_snprintf(pEntry->szLetter, "%c:", p[0]);
                    safe_snprintf(pEntry->szFileSystem, "%s", fsName);
                    pEntry->qwTotalBytes = totalBytes.QuadPart;
                    pEntry->qwFreeBytes = totalFree.QuadPart;
                    pEntry->fFreePercent = fFreePct;
                    pEntry->bTrimSupportedFs = bTrimFs;

                    if (totalBytes.QuadPart > 0 && !bTrimFs && pDrives[d].eType != DRIVE_TYPE_HDD) {
                        pTrim->bHasNonTrimVolume = TRUE;
                    }

                    if (fFreePct >= 0.0f) {
                        if (pTrim->fMinFreePercent < 0.0f || fFreePct < pTrim->fMinFreePercent) {
                            pTrim->fMinFreePercent = fFreePct;
                        }
                        if (pDrives[d].eType != DRIVE_TYPE_HDD) {
                            if (fFreePct < 7.0f) pTrim->bCriticallyLowSpace = TRUE;
                            if (fFreePct < 5.0f) pTrim->bPanicLowSpace = TRUE;
                        }
                    }
                }
            }
        }
    }

    for (int i = 0; i < nNumDrives; i++) {
        EvaluateDriveTrimAndCapacity(&pDrives[i]);
    }
}
