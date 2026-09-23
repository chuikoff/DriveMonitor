/* DriveMonitor - NVMe via IOCTL_STORAGE_QUERY_PROPERTY.
 * MIT: see LICENSE.
 * Never IOCTL_STORAGE_PROTOCOL_COMMAND — nvme.sys bugchecks on it.
 * Intel RST VMD (iaStorVD): IntelNvm SRB only. ATA/SCSI passthrough
 * and a 4096-byte log-page query bugcheck 0x139. */
#include "smart_internal.h"

static DWORD g_dwLastNvmeQueryErr;
static BOOL NvmeMiniportAdmin(HANDLE hDrive, DWORD cdw0, DWORD nsid, DWORD cdw10, BYTE* pOut, DWORD dwOut);
static BOOL NvmeIntelRstAdmin(HANDLE hDrive, DWORD opcode, DWORD nsid, DWORD cdw10, BYTE* pOut, DWORD dwOut);


static DWORD NvmeIdentVerDword(const BYTE* id, DWORD n)
{
    if (!id || n < (NVME_IDENT_VER_OFF + 4)) return 0;
    return (DWORD)id[NVME_IDENT_VER_OFF] |
           ((DWORD)id[NVME_IDENT_VER_OFF + 1] << 8) |
           ((DWORD)id[NVME_IDENT_VER_OFF + 2] << 16) |
           ((DWORD)id[NVME_IDENT_VER_OFF + 3] << 24);
}

/* Higher is better. VER present (~1000) beats a 72-byte SN/MN/FR stub. */
static DWORD NvmeIdentQuality(const BYTE* p, DWORD n)
{
    DWORD q = 0, ver;
    WORD wctemp;
    if (!p || n < 8) return 0;
    if (IsBufferAllZero(p, n > 16 ? 16 : (int)n)) return 0;
    if (n >= 72) q += 10;
    if (n >= 84) {
        ver = NvmeIdentVerDword(p, n);
        if (ver != 0) q += 1000;
        q += 10;
    }
    if (n >= (NVME_IDENT_CCTEMP_OFF + 2)) {
        wctemp = (WORD)p[NVME_IDENT_WCTEMP_OFF] |
                 ((WORD)p[NVME_IDENT_WCTEMP_OFF + 1] << 8);
        if (wctemp >= 274 && wctemp <= 400) q += 100;
        q += 10;
    }
    return q;
}

void FillNvmeTempThresholdsFromIdent(DRIVE_INFO* pInfo, DWORD nCopied)
{
    const BYTE* id;
    if (!pInfo) return;
    id = (const BYTE*)&pInfo->nvmeIdent;
    if (nCopied < (NVME_IDENT_CCTEMP_OFF + 2)) return;
    pInfo->wNVMeWarnTempThreshold =
        (WORD)id[NVME_IDENT_WCTEMP_OFF] | ((WORD)id[NVME_IDENT_WCTEMP_OFF + 1] << 8);
    pInfo->wNVMeCritTempThreshold =
        (WORD)id[NVME_IDENT_CCTEMP_OFF] | ((WORD)id[NVME_IDENT_CCTEMP_OFF + 1] << 8);
}

void FillNvmeProtocolFromIdent(DRIVE_INFO* pInfo)
{
    const BYTE* id;
    DWORD ver;
    unsigned maj, minr, ter;
    if (!pInfo) return;
    id = (const BYTE*)&pInfo->nvmeIdent;
    ver = NvmeIdentVerDword(id, 4096);
    maj  = (ver >> 16) & 0xFFFF;
    minr = (ver >> 8) & 0xFF;
    ter  = ver & 0xFF;
    /* Some adapters store MJR.MNR.TER as bytes 80,81,82 instead of LE DWORD. */
    if (maj == 0 && id[NVME_IDENT_VER_OFF] >= 1 && id[NVME_IDENT_VER_OFF] <= 2) {
        maj  = id[NVME_IDENT_VER_OFF];
        minr = id[NVME_IDENT_VER_OFF + 1];
        ter  = id[NVME_IDENT_VER_OFF + 2];
    }
    if (maj < 1 || maj > 2) {
        /* Keep a model-table fallback such as "NVMe 1.4.0". */
        if (pInfo->szProtocol[0] == '\0')
            safe_snprintf(pInfo->szProtocol, "NVMe");
        return;
    }
    safe_snprintf(pInfo->szProtocol, "NVMe %u.%u.%u", maj, minr, ter);
}

/* memcpy Identify Controller without overreading a 4096-byte page. */
void CopyNvmeIdentBuf(DRIVE_INFO* pInfo, const BYTE* pBuf, DWORD nAvail)
{
    DWORD n;
    if (!pInfo || !pBuf || nAvail == 0) return;
    n = (DWORD)sizeof(NVME_IDENTIFY_CONTROLLER);
    if (n > nAvail) n = nAvail;
    if (n > 4096) n = 4096;
    memcpy(&pInfo->nvmeIdent, pBuf, n);
    if (n < (DWORD)sizeof(NVME_IDENTIFY_CONTROLLER))
        ZeroMemory((BYTE*)&pInfo->nvmeIdent + n,
                   sizeof(NVME_IDENTIFY_CONTROLLER) - n);
    FillNvmeProtocolFromIdent(pInfo);
    FillNvmeTempThresholdsFromIdent(pInfo, n);
}

/* One IOCTL into caller-owned q (reused). 1 = log copied, -1 = ident good
 * enough to stop, 0 = try the next property/NSID/length. */
static int NvmeProtocolTry(HANDLE h, CDI_NVME_QUERY_BUF* q,
    ULONG propertyId, ULONG dataType, ULONG requestValue, ULONG subValue,
    ULONG length, BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied,
    BYTE* bestIdent, DWORD* bestCopied, DWORD* bestQ)
{
    DWORD dwBytes = 0;
    BYTE* pData;
    const BYTE* src;
    DWORD avail, copyLen;
    ULONG off, reported;

    ZeroMemory(q, sizeof(*q));
    q->PropertyId = propertyId;
    q->QueryType  = 0;
    q->ProtocolSpecific.ProtocolType             = MY_ProtocolTypeNvme;
    q->ProtocolSpecific.DataType                 = dataType;
    q->ProtocolSpecific.ProtocolDataRequestValue = requestValue;
    q->ProtocolSpecific.ProtocolDataRequestSubValue = subValue;
    q->ProtocolSpecific.ProtocolDataOffset       = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    q->ProtocolSpecific.ProtocolDataLength       = length;

    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY,
            q, sizeof(*q), q, sizeof(*q), &dwBytes, NULL)) {
        g_dwLastNvmeQueryErr = GetLastError();
        return 0;
    }

    off = q->ProtocolSpecific.ProtocolDataOffset;
    if (off == 0) off = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    pData = (BYTE*)&q->ProtocolSpecific + off;
    if (pData < (BYTE*)q || pData >= (BYTE*)q + sizeof(*q))
        pData = q->Buffer;
    avail = (DWORD)(((BYTE*)q + sizeof(*q)) - pData);
    reported = q->ProtocolSpecific.ProtocolDataLength;
    copyLen = avail;
    if (reported > 0 && reported < copyLen) copyLen = reported;
    if (copyLen > dwOutLen) copyLen = dwOutLen;

    src = NULL;
    if (copyLen >= 8 && !IsBufferAllZero(pData, copyLen > 16 ? 16 : (int)copyLen)) {
        src = pData;
    } else if (!IsBufferAllZero(q->Buffer, 16)) {
        src = q->Buffer;
        copyLen = dwOutLen < 4096 ? dwOutLen : 4096;
        if (reported > 0 && reported < copyLen) copyLen = reported;
    }
    if (!src)
        return 0;

    if (dataType == MY_NVMeDataTypeIdentify) {
        DWORD qq = NvmeIdentQuality(src, copyLen);
        if (qq > *bestQ) {
            DWORD n = copyLen < 4096 ? copyLen : 4096;
            memcpy(bestIdent, src, n);
            *bestCopied = n;
            *bestQ = qq;
        }
        /* Some drivers ignore the ProtocolDataOffset they returned. */
        {
            DWORD nBuf = dwOutLen < 4096 ? dwOutLen : 4096;
            if (reported > 0 && reported < nBuf) nBuf = reported;
            qq = NvmeIdentQuality(q->Buffer, nBuf);
            if (qq > *bestQ) {
                memcpy(bestIdent, q->Buffer, nBuf);
                *bestCopied = nBuf;
                *bestQ = qq;
            }
        }
        return (*bestQ >= 1000) ? -1 : 0;
    }

    memcpy(pOut, src, copyLen);
    if (pdwCopied) *pdwCopied = copyLen;
    return 1;
}

static BOOL QueryNVMeProtocolOnHandle(HANDLE h, ULONG dataType, ULONG requestValue,
                                      BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied,
                                      DWORD identLen)
{
    /* Identify Controller is adapter-level. Device Identify is often a
     * 72-byte SN/MN/FR stub (VER at 80 = 0). Health log: Device first. */
    ULONG propertyIds[2];
    ULONG subValues[3];
    ULONG lengths[2];
    int nSub, nLen, ip, isv, il;
    BYTE bestIdent[4096];
    CDI_NVME_QUERY_BUF q;
    DWORD bestCopied = 0, bestQ = 0;

    if (dataType == MY_NVMeDataTypeIdentify) {
        propertyIds[0] = (ULONG)StorageAdapterProtocolSpecificProperty;
        propertyIds[1] = (ULONG)StorageDeviceProtocolSpecificProperty;
        subValues[0] = 0;
        subValues[1] = 1;
        nSub = 2;
        /* 4096 only for stornvme/secnvme. Other drivers allocate a
         * stack buffer from this length and bugcheck 0x139. */
        if (identLen < 512) identLen = 512;
        if (identLen > 4096) identLen = 4096;
        lengths[0] = identLen;
        nLen = 1;
    } else {
        propertyIds[0] = (ULONG)StorageDeviceProtocolSpecificProperty;
        propertyIds[1] = (ULONG)StorageAdapterProtocolSpecificProperty;
        subValues[0] = 0;
        subValues[1] = 0xFFFFFFFFu;
        subValues[2] = 1;
        nSub = 3;
        /* Log page 02h is 512 bytes. ProtocolDataLength 4096 makes
         * iaStorVD overrun a stack cookie (bugcheck 0x139). */
        lengths[0] = 512;
        nLen = 1;
    }

    ZeroMemory(bestIdent, sizeof(bestIdent));

    for (ip = 0; ip < 2; ip++) {
        for (isv = 0; isv < nSub; isv++) {
            for (il = 0; il < nLen; il++) {
                int r = NvmeProtocolTry(h, &q, propertyIds[ip], dataType,
                    requestValue, subValues[isv], lengths[il],
                    pOut, dwOutLen, pdwCopied,
                    bestIdent, &bestCopied, &bestQ);
                if (r > 0) return TRUE;
                if (r < 0) goto ident_done;
            }
        }
    }

ident_done:
    if (bestCopied >= 8) {
        DWORD n = bestCopied < dwOutLen ? bestCopied : dwOutLen;
        memcpy(pOut, bestIdent, n);
        if (pdwCopied) *pdwCopied = n;
        return TRUE;
    }
    return FALSE;
}

BOOL QueryNVMeProtocol(HANDLE hDrive, ULONG dataType, ULONG requestValue,
                              BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied)
{
    SCSI_ADDRESS addr;
    DWORD dwBytes = 0;
    HANDLE hScsi;
    char szScsi[32];
    BOOL okDrive;
    DWORD identLen;

    g_dwLastNvmeQueryErr = 0;
    /* Intel RST and USB bridges: no Microsoft protocol query.
     * RAID/VROC/virtual get a 512-byte query on this handle only.
     * \\.\ScsiN: is stornvme/secnvme — opening it on iaStor or MegaRAID
     * bugchecks 0x139. */
    if (!DriveAllowsNvmeProtocol(hDrive)) {
        g_dwLastNvmeQueryErr = ERROR_NOT_SUPPORTED;
        return FALSE;
    }
    identLen = DriveAllowsFullNvmeIdentify(hDrive) ? 4096 : 512;
    okDrive = QueryNVMeProtocolOnHandle(hDrive, dataType, requestValue,
                                        pOut, dwOutLen, pdwCopied, identLen);

    /* A 72-byte SN/MN/FR stub still counts as success. For Identify, keep
     * going to \\.\ScsiN: — that is where full VER/WCTEMP often live. */
    if (okDrive && (dataType != MY_NVMeDataTypeIdentify ||
                    NvmeIdentVerDword(pOut, pdwCopied ? *pdwCopied : 0) != 0))
        return TRUE;
    if (!DriveAllowsFullNvmeIdentify(hDrive))
        return okDrive;

    ZeroMemory(&addr, sizeof(addr));
    addr.Length = sizeof(addr);
    if (DeviceIoControl(hDrive, IOCTL_SCSI_GET_ADDRESS,
            NULL, 0, &addr, sizeof(addr), &dwBytes, NULL)) {
        safe_snprintf(szScsi, "\\\\.\\Scsi%d:", (int)addr.PortNumber);
        hScsi = CreateFileA(szScsi, GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL, NULL);
        if (hScsi != INVALID_HANDLE_VALUE) {
            BYTE scsiBuf[4096];
            DWORD nScsi = 0;
            BOOL okScsi;
            ZeroMemory(scsiBuf, sizeof(scsiBuf));
            okScsi = QueryNVMeProtocolOnHandle(hScsi, dataType, requestValue,
                                               scsiBuf, sizeof(scsiBuf), &nScsi,
                                               identLen);
            CloseHandle(hScsi);
            if (okScsi) {
                if (!okDrive ||
                    NvmeIdentQuality(scsiBuf, nScsi) >
                        NvmeIdentQuality(pOut, pdwCopied ? *pdwCopied : 0)) {
                    DWORD n = nScsi < dwOutLen ? nScsi : dwOutLen;
                    memcpy(pOut, scsiBuf, n);
                    if (pdwCopied) *pdwCopied = n;
                }
                return TRUE;
            }
        } else if (g_dwLastNvmeQueryErr == 0) {
            g_dwLastNvmeQueryErr = GetLastError();
        }
    } else if (g_dwLastNvmeQueryErr == 0) {
        g_dwLastNvmeQueryErr = GetLastError();
    }
    return okDrive;
}

/* ============================================================
 * NVMe paths
 * ============================================================ */
BOOL GetNVMeIdentifyController(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE ident[4096];
    DWORD nCopied = 0;
    ZeroMemory(ident, sizeof(ident));

    /* Native NvmeMini / protocol query on a USB handle crashes RTL9210. */
    if (GetStorageBusType(hDrive) == 7)
        return FALSE;

    /* VMD: do not send NvmeMini or a 4096-byte protocol identify. */
    if (DriveBehindIntelRst(hDrive)) {
        BYTE alt[4096];
        ZeroMemory(alt, sizeof(alt));
        if (!NvmeIntelRstAdmin(hDrive, 0x06, 0, 1, alt, 4096) ||
            NvmeIdentQuality(alt, 4096) < 10)
            return FALSE;
        memcpy(ident, alt, 4096);
        nCopied = 4096;
        goto ident_copied;
    }

    if (!QueryNVMeProtocol(hDrive, MY_NVMeDataTypeIdentify, 1,
                           ident, sizeof(ident), &nCopied) || nCopied < 72 ||
        NvmeIdentVerDword(ident, nCopied) == 0) {
        BYTE alt[4096];
        ZeroMemory(alt, sizeof(alt));
        if (NvmeMiniportAdmin(hDrive, 6, 0, 1, alt, 4096) ||
            NvmeIntelRstAdmin(hDrive, 0x06, 0, 1, alt, 4096)) {
            if (nCopied < 72 ||
                NvmeIdentQuality(alt, 4096) > NvmeIdentQuality(ident, nCopied)) {
                memcpy(ident, alt, 4096);
                nCopied = 4096;
            }
        } else if (nCopied < 72) {
            return FALSE;
        }
    }

ident_copied:
    /* Store identify controller data (never more than the 4096-byte page). */
    CopyNvmeIdentBuf(pInfo, ident, nCopied ? nCopied : 4096);
    pInfo->bGotNVMeIdent = TRUE;

    /* Extract key strings (NVMe uses direct ASCII, no byte-swap needed) */
    memcpy(pInfo->szSerial, ident + 4, 20);
    pInfo->szSerial[20] = '\0';
    TrimStr(pInfo->szSerial);

    memcpy(pInfo->szModel, ident + 24, 40);
    pInfo->szModel[40] = '\0';
    TrimStr(pInfo->szModel);

    memcpy(pInfo->szFirmware, ident + 64, 8);
    pInfo->szFirmware[8] = '\0';
    TrimStr(pInfo->szFirmware);

    return (pInfo->szModel[0] != '\0' || pInfo->szSerial[0] != '\0');
}

BOOL GetNVMeHealthLog(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE health[sizeof(NVME_HEALTH_INFO_LOG)];
    DWORD nCopied = 0;
    ZeroMemory(health, sizeof(health));

    if (!QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, NVME_LOG_PAGE_HEALTH_INFO,
                           health, sizeof(health), &nCopied)) {
        pInfo->dwErrNvmeProtocol = g_dwLastNvmeQueryErr ? g_dwLastNvmeQueryErr : GetLastError();
        return FALSE;
    }

    memcpy(&pInfo->nvmeHealth, health,
           nCopied < sizeof(NVME_HEALTH_INFO_LOG) ? nCopied : sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bIsNVMe          = TRUE;
    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled   = TRUE;
    pInfo->eAccessMethod    = SMART_ACCESS_NVME_PROTOCOL;

    return TRUE;
}

BOOL GetNVMeHealthLogFallback(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    /* QueryNVMeProtocol already tries adapter vs device and several NSIDs. */
    return GetNVMeHealthLog(hDrive, pInfo);
}

#ifndef NVME_PASS_THROUGH_SRB_IO_CODE
#define NVME_STORPORT_DRIVER 0xE000
#define NVME_PASS_THROUGH_SRB_IO_CODE CTL_CODE(NVME_STORPORT_DRIVER, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif
#ifndef IOCTL_INTEL_NVME_PASS_THROUGH
#define IOCTL_INTEL_NVME_PASS_THROUGH CTL_CODE(0xf000, 0xA02, METHOD_BUFFERED, FILE_ANY_ACCESS)
#endif

#pragma pack(push, 1)
typedef struct _MY_SRB_IO_CONTROL {
    ULONG HeaderLength;
    UCHAR Signature[8];
    ULONG Timeout;
    ULONG ControlCode;
    ULONG ReturnCode;
    ULONG Length;
} MY_SRB_IO_CONTROL;

typedef struct _MY_NVME_PT {
    MY_SRB_IO_CONTROL SrbIoCtrl;
    DWORD VendorSpecific[6];
    DWORD NVMeCmd[16];
    DWORD CplEntry[4];
    DWORD Direction;
    DWORD QueueId;
    DWORD DataBufferLen;
    DWORD MetaDataLen;
    DWORD ReturnBufferLen;
    UCHAR DataBuffer[4096];
} MY_NVME_PT;

typedef struct _MY_INTEL_NVME_PT {
    MY_SRB_IO_CONTROL SRB;
    BYTE  Version;
    BYTE  PathId;
    BYTE  TargetId;
    BYTE  Lun;
    DWORD NVMeCmd[16];
    DWORD CplEntry[4];
    DWORD QueueId;
    DWORD ParamBufLen;
    DWORD ReturnBufferLen;
    BYTE  Rsvd[0x28];
    BYTE  DataBuffer[0x1000];
} MY_INTEL_NVME_PT;
#pragma pack(pop)

static HANDLE OpenScsiAdapterFromDrive(HANDLE hDrive, SCSI_ADDRESS* pAddr)
{
    SCSI_ADDRESS addr;
    DWORD dw = 0;
    char sz[32];
    HANDLE hScsi;
    ZeroMemory(&addr, sizeof(addr));
    addr.Length = sizeof(addr);
    if (!DeviceIoControl(hDrive, IOCTL_SCSI_GET_ADDRESS,
            NULL, 0, &addr, sizeof(addr), &dw, NULL))
        return INVALID_HANDLE_VALUE;
    if (pAddr) *pAddr = addr;
    safe_snprintf(sz, "\\\\.\\Scsi%d:", (int)addr.PortNumber);
    hScsi = CreateFileA(sz, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, NULL);
    return hScsi;
}

static BOOL BufferHasData(const BYTE* p, int n)
{
    int i;
    DWORD sum = 0;
    for (i = 0; i < n; i++) sum += p[i];
    return sum != 0;
}

static BOOL NvmeMiniportAdmin(HANDLE hDrive, DWORD cdw0, DWORD nsid, DWORD cdw10,
                              BYTE* pOut, DWORD dwOut)
{
    SCSI_ADDRESS addr;
    HANDLE hScsi;
    /* "NvmeMini" is stornvme's SRB. iaStor, MegaRAID, Samsung secnvme
     * and AMD-RAID bugcheck or corrupt the stack on this signature. */
    if (!DriveAllowsNvmeMini(hDrive))
        return FALSE;
    hScsi = OpenScsiAdapterFromDrive(hDrive, &addr);
    MY_NVME_PT* pt;
    DWORD dwRet = 0;
    BOOL ok;
    if (hScsi == INVALID_HANDLE_VALUE) return FALSE;
    pt = (MY_NVME_PT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MY_NVME_PT));
    if (!pt) { CloseHandle(hScsi); return FALSE; }
    pt->SrbIoCtrl.HeaderLength = sizeof(MY_SRB_IO_CONTROL);
    memcpy(pt->SrbIoCtrl.Signature, "NvmeMini", 8);
    pt->SrbIoCtrl.Timeout = 40;
    pt->SrbIoCtrl.ControlCode = NVME_PASS_THROUGH_SRB_IO_CODE;
    pt->SrbIoCtrl.Length = sizeof(MY_NVME_PT) - sizeof(MY_SRB_IO_CONTROL);
    pt->Direction = 2;
    pt->DataBufferLen = 4096;
    pt->ReturnBufferLen = sizeof(MY_NVME_PT);
    pt->NVMeCmd[0] = cdw0;
    pt->NVMeCmd[1] = nsid;
    pt->NVMeCmd[10] = cdw10;
    ok = DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, pt, sizeof(*pt), pt, sizeof(*pt), &dwRet, NULL);
    CloseHandle(hScsi);
    if (!ok) {
        g_dwLastNvmeQueryErr = GetLastError();
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (!BufferHasData(pt->DataBuffer, 64)) {
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (dwOut > sizeof(pt->DataBuffer)) dwOut = (DWORD)sizeof(pt->DataBuffer);
    memcpy(pOut, pt->DataBuffer, dwOut);
    HeapFree(GetProcessHeap(), 0, pt);
    return TRUE;
}

static BOOL NvmeIntelRstAdmin(HANDLE hDrive, DWORD opcode, DWORD nsid, DWORD cdw10,
                              BYTE* pOut, DWORD dwOut)
{
    SCSI_ADDRESS addr;
    HANDLE hScsi;
    MY_INTEL_NVME_PT* pt;
    DWORD dwRet = 0;
    BOOL ok;
    if (!DriveBehindIntelRst(hDrive))
        return FALSE;
    hScsi = OpenScsiAdapterFromDrive(hDrive, &addr);
    if (hScsi == INVALID_HANDLE_VALUE) return FALSE;
    pt = (MY_INTEL_NVME_PT*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(MY_INTEL_NVME_PT));
    if (!pt) { CloseHandle(hScsi); return FALSE; }
    pt->SRB.HeaderLength = sizeof(MY_SRB_IO_CONTROL);
    memcpy(pt->SRB.Signature, "IntelNvm", 8);
    pt->SRB.Timeout = 10;
    pt->SRB.ControlCode = IOCTL_INTEL_NVME_PASS_THROUGH;
    pt->SRB.Length = sizeof(MY_INTEL_NVME_PT) - sizeof(MY_SRB_IO_CONTROL);
    pt->Version = 1;
    pt->PathId = addr.PathId;
    /* TargetId and Lun stay 0. CrystalDiskInfo's IntelNvm SRB does the
     * same; a non-zero TargetId is not what iaStorVD expects.
     * ParamBufLen is payload + SRB (0xA4), not the payload alone. */
    pt->NVMeCmd[0] = opcode;
    pt->NVMeCmd[1] = nsid;
    pt->NVMeCmd[10] = cdw10;
    pt->ParamBufLen = (DWORD)(sizeof(MY_INTEL_NVME_PT) - 0x1000);
    pt->ReturnBufferLen = 0x1000;
    ok = DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, pt, sizeof(*pt), pt, sizeof(*pt), &dwRet, NULL);
    CloseHandle(hScsi);
    if (!ok) {
        g_dwLastNvmeQueryErr = GetLastError();
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (!BufferHasData(pt->DataBuffer, 64)) {
        HeapFree(GetProcessHeap(), 0, pt);
        return FALSE;
    }
    if (dwOut > sizeof(pt->DataBuffer)) dwOut = (DWORD)sizeof(pt->DataBuffer);
    memcpy(pOut, pt->DataBuffer, dwOut);
    HeapFree(GetProcessHeap(), 0, pt);
    return TRUE;
}

static BOOL FillNvmeHealthFromBuf(DRIVE_INFO* pInfo, BYTE* health, DWORD n)
{
    memcpy(&pInfo->nvmeHealth, health,
           n < sizeof(NVME_HEALTH_INFO_LOG) ? n : sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bIsNVMe = TRUE;
    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PROTOCOL;
    return TRUE;
}

BOOL GetNVMeHealthLogEx(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE health[512];
    /* Native NvmeMini IOCTL on a USB handle crashes RTL9210 (and is
     * meaningless on USB SAT). Never run this path on bus type 7. */
    if (GetStorageBusType(hDrive) == 7)
        return FALSE;
    /* Do not call IOCTL_STORAGE_PROTOCOL_COMMAND — it bugchecked nvme.sys.
     * VMD does not implement that query either; IntelNvm only. */
    if (DriveBehindIntelRst(hDrive)) {
        ZeroMemory(health, sizeof(health));
        if (!NvmeIntelRstAdmin(hDrive, 0x02, 0xFFFFFFFFu, 0x007f0002,
                               health, sizeof(health)))
            return FALSE;
        return FillNvmeHealthFromBuf(pInfo, health, sizeof(health));
    }
    if (GetNVMeHealthLog(hDrive, pInfo)) return TRUE;
    if (GetNVMeHealthLogFallback(hDrive, pInfo)) return TRUE;
    ZeroMemory(health, sizeof(health));
    if (NvmeMiniportAdmin(hDrive, 2, 0xFFFFFFFFu, 0x007f0002, health, sizeof(health)))
        return FillNvmeHealthFromBuf(pInfo, health, sizeof(health));
    if (NvmeIntelRstAdmin(hDrive, 0x02, 0xFFFFFFFFu, 0x007f0002, health, sizeof(health)))
        return FillNvmeHealthFromBuf(pInfo, health, sizeof(health));
    return FALSE;
}

/* ============================================================
 * NVMe data extraction helpers
 * Extracts all 128-bit NVMe counters as 64-bit
 * ============================================================ */
void ExtractNVMeExtendedInfo(DRIVE_INFO* pInfo)
{
    NVME_HEALTH_INFO_LOG* pLog = &pInfo->nvmeHealth;

    /* Temperature */
    WORD wTempK = ReadLE16(pLog->CompositeTemperature);
    if (wTempK > 273 && wTempK < 400)
        pInfo->nTemperatureC = (int)wTempK - 273;

    /* NVMe temperature sensors */
    int i;
    for (i = 0; i < 8; i++) {
        WORD wSensorK = pLog->TempSensor[i];
        if (wSensorK > 273 && wSensorK < 400)
            pInfo->nTempSensor[i] = (int)wSensorK - 273;
        else
            pInfo->nTempSensor[i] = -1;
    }

    /* 128-bit counters → use lower 64 bits (sufficient for practical use) */
    pInfo->qwNVMeDataUnitsRead      = ReadLE64(pLog->DataUnitsRead);
    pInfo->qwNVMeDataUnitsWritten   = ReadLE64(pLog->DataUnitsWritten);
    pInfo->qwNVMeHostReads          = ReadLE64(pLog->HostReadCommands);
    pInfo->qwNVMeHostWrites         = ReadLE64(pLog->HostWriteCommands);
    pInfo->qwNVMeControllerBusyTime = ReadLE64(pLog->ControllerBusyTime);
    pInfo->qwNVMePowerOnHours       = ReadLE64(pLog->PowerOnHours);
    pInfo->qwNVMeUnsafeShutdowns    = ReadLE64(pLog->UnsafeShutdowns);
    pInfo->qwNVMeMediaErrors        = ReadLE64(pLog->MediaErrors);

    /* Convenience 32-bit fields for backwards compat */
    pInfo->dwPowerOnHours    = (DWORD)pInfo->qwNVMePowerOnHours;
    pInfo->dwPowerCycleCount = (DWORD)ReadLE64(pLog->PowerCycles);
}

BOOL GetNVMeInfo(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BOOL bIdent = GetNVMeIdentifyController(hDrive, pInfo);
    if (!bIdent) GetDeviceDescriptor(hDrive, pInfo);
    GetCapacityFromGeometry(hDrive, pInfo);

    if (GetNVMeHealthLogEx(hDrive, pInfo)) {
        ExtractNVMeExtendedInfo(pInfo);
        /* Log page 0xCA via the Microsoft protocol query. iaStorVD
         * does not implement it; do not send it. */
        if (!DriveBehindIntelRst(hDrive))
            TryNvmeLifetimeTemp(hDrive, pInfo);
    } else if (!DriveBehindIntelRst(hDrive)) {
        GetSMARTViaLogSense(hDrive, pInfo);
    }

    /* A VMD disk that did not answer IntelNvm stays non-NVMe so the
     * caller does not pretend SMART was read. */
    if (bIdent || pInfo->bSMART_Supported || !DriveBehindIntelRst(hDrive)) {
        pInfo->eType   = DRIVE_TYPE_NVME;
        pInfo->bIsNVMe = TRUE;
    }

    IdentifyDriveParts(pInfo);

    return (pInfo->szModel[0] != '\0' || pInfo->bSMART_Supported);
}

int NvmeIdentifyTempC(USHORT kelvin)
{
    if (kelvin < 274 || kelvin > 400)
        return -1;
    return (int)kelvin - 273;
}

void TryNvmeLifetimeTemp(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE log[512];
    DWORD nCopied = 0;
    int i, cur, hi, lo, nHit, warnC, critC;
    BOOL curSeen;
    int vals[24];
    if (!pInfo || GetStorageBusType(hDrive) == 7)
        return;
    ZeroMemory(log, sizeof(log));
    /* Vendor unique additional SMART (Samsung/Intel 0xCA). Not spec Health 02h. */
    if (!QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, 0xCA,
                           log, sizeof(log), &nCopied) || nCopied < 16)
        return;
    nHit = 0;
    cur = pInfo->nTemperatureC;
    warnC = NvmeIdentifyTempC(pInfo->wNVMeWarnTempThreshold);
    critC = NvmeIdentifyTempC(pInfo->wNVMeCritTempThreshold);
    for (i = 0; i + 1 < 64 && i + 1 < (int)nCopied && nHit < 24; i += 2) {
        unsigned k = (unsigned)log[i] | ((unsigned)log[i + 1] << 8);
        int c;
        if (k < 274 || k > 400)
            continue;
        c = (int)k - 273;
        if ((warnC > 0 && c == warnC) || (critC > 0 && c == critC))
            continue;
        vals[nHit++] = c;
    }
    if (nHit < 2 || cur <= 0)
        return;
    curSeen = FALSE;
    hi = vals[0];
    lo = vals[0];
    for (i = 0; i < nHit; i++) {
        if (vals[i] >= cur - 1 && vals[i] <= cur + 1)
            curSeen = TRUE;
        if (vals[i] > hi) hi = vals[i];
        if (vals[i] < lo) lo = vals[i];
    }
    if (!curSeen)
        return;
    if (hi > cur && hi != warnC && hi != critC)
        pInfo->nTempMaxC = hi;
    if (lo < cur && lo != warnC && lo != critC)
        pInfo->nTempMinC = lo;
}

