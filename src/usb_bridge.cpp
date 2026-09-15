/* DriveMonitor - USB VID/PID, SAT, NVMe-over-USB bridges. MIT: see LICENSE. */
#include "smart_internal.h"

void PrefixUsbProtocol(DRIVE_INFO* pInfo)
{
    char disk[64];
    const char* pre = "USB";
    char hay[384];
    if (!pInfo || !pInfo->bIsUSB || !pInfo->szProtocol[0])
        return;
    if (strncmp(pInfo->szProtocol, "USB", 3) == 0)
        return;
    lstrcpynA(disk, pInfo->szProtocol, (int)sizeof(disk));
    safe_snprintf(hay, "%s %s %s",
              pInfo->szModel, pInfo->szBridgeVendor, pInfo->szBridgeProduct);
    if (IsRealtekNvmeUsbBridge(pInfo) ||
        pInfo->eUsbBridgeType == USB_BRIDGE_NVME_REALTEK ||
        strstr(hay, "RTL9210") || strstr(hay, "RTL921"))
        pre = "USB RTL9210";
    else if (pInfo->bIsNVMe)
        pre = "USB";
    else
        pre = "USB SAT";
    safe_snprintf(pInfo->szProtocol, "%s · %s", pre, disk);
}

/* Realtek RTL9210 USB dual-mode enclosure (NVMe via 0xE4, SATA via SAT).
 * Native NvmeMini IOCTL on this USB handle crashes the process.
 * ScanDrives: SAT SMART first, then one-shot vendor 0xE4.
 * Never 0xE4 / NvmeMini from NVMeOverUSBTryAll. */
BOOL IsRealtekNvmeUsbBridge(const DRIVE_INFO* p)
{
    char hay[384];
    int i;
    if (!p) return FALSE;
    if (p->eUsbBridgeType == USB_BRIDGE_NVME_REALTEK)
        return TRUE;
    /* Any Realtek USB storage VID — SAT first, then 0xE4. */
    if (p->wUsbVid == 0x0BDA)
        return TRUE;
    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);
    for (i = 0; hay[i]; i++)
        hay[i] = (char)toupper((unsigned char)hay[i]);
    if (strstr(hay, "RTL9210") || strstr(hay, "RTL9211") ||
        strstr(hay, "RTL921"))
        return TRUE;
    if (strstr(hay, "REALTEK") && strstr(hay, "9210"))
        return TRUE;
    return FALSE;
}

/* ============================================================
 * USB/SCSI bridge identification
 * ============================================================ */
BOOL GetBridgeIdentity(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    ZeroMemory(outBuf, sizeof(outBuf));
    DWORD dwBytes = 0;

    pInfo->szBridgeVendor[0]  = '\0';
    pInfo->szBridgeProduct[0] = '\0';
    pInfo->wUsbVid = 0;
    pInfo->wUsbPid = 0;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    if (dwBytes < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    CopyDescStr(pInfo->szBridgeVendor, sizeof(pInfo->szBridgeVendor),
                outBuf, dwBytes, pDesc->VendorIdOffset);
    CopyDescStr(pInfo->szBridgeProduct, sizeof(pInfo->szBridgeProduct),
                outBuf, dwBytes, pDesc->ProductIdOffset);

    /* Try to extract VID/PID from the (already bounded) descriptor strings.
     * Some USB bridges include VID/PID in the vendor or product strings. */
    if (pInfo->szBridgeVendor[0])
        ParseVidPidFromHardwareId(pInfo->szBridgeVendor, &pInfo->wUsbVid, &pInfo->wUsbPid);
    if (pInfo->wUsbVid == 0 && pInfo->wUsbPid == 0 && pInfo->szBridgeProduct[0])
        ParseVidPidFromHardwareId(pInfo->szBridgeProduct, &pInfo->wUsbVid, &pInfo->wUsbPid);

    /* Also try SCSI INQUIRY to get bridge vendor/product for more accurate detection.
     * SCSI INQUIRY for USB bridge identification. */
    {
        CDI_SAT_PASSTHROUGH_BUF sptwb;
        DWORD dwInqBytes = 0;
        ZeroMemory(&sptwb, sizeof(sptwb));

        sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
        sptwb.spt.PathId             = 0;
        sptwb.spt.TargetId           = 0;
        sptwb.spt.Lun                = 0;
        sptwb.spt.CdbLength          = 6;
        sptwb.spt.SenseInfoLength    = 32;
        sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = 96;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
        sptwb.spt.TimeOutValue       = 10;

        /* Standard SCSI INQUIRY */
        sptwb.spt.Cdb[0] = 0x12;   /* INQUIRY */
        sptwb.spt.Cdb[4] = 96;     /* Allocation length */

        DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf) + 96;

        if (DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
                &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwInqBytes, NULL) &&
            sptwb.spt.ScsiStatus == 0) {
            /* Parse INQUIRY data: Vendor at offset 8 (8 bytes), Product at offset 16 (16 bytes) */
            BYTE* pInq = sptwb.DataBuf;
            if (pInq[0] != 0xFF && pInq[4] >= 0x1F) {
                /* Valid INQUIRY response */
                if (pInfo->szBridgeVendor[0] == '\0') {
                    memcpy(pInfo->szBridgeVendor, pInq + 8, 8);
                    pInfo->szBridgeVendor[8] = '\0';
                    TrimStr(pInfo->szBridgeVendor);
                }
                if (pInfo->szBridgeProduct[0] == '\0') {
                    memcpy(pInfo->szBridgeProduct, pInq + 16, 16);
                    pInfo->szBridgeProduct[16] = '\0';
                    TrimStr(pInfo->szBridgeProduct);
                }
            }
        }
    }

    return (pInfo->szBridgeVendor[0] != '\0' || pInfo->szBridgeProduct[0] != '\0');
}

/* ============================================================
 * USB VID/PID retrieval via SetupDi API
 * The VID/PID is extracted from the device's hardware ID string
 * in the Windows registry (SPDRP_HARDWAREID), which contains
 * entries like "USB\\VID_152D&PID_0583".
 * This is the most reliable way to identify USB bridge chips.
 * ============================================================ */

/* Parse "VID_xxxx&PID_xxxx" from a hardware ID string
 * (Definition — forward declared at top of file for use in IsNVMeDrive/GetBridgeIdentity) */
BOOL ParseVidPidFromHardwareId(const char* szHwId, WORD* pwVid, WORD* pwPid)
{
    const char* pVid = strstr(szHwId, "VID_");
    const char* pPid = strstr(szHwId, "PID_");
    if (!pVid || !pPid) return FALSE;

    /* Parse VID (4 hex digits after "VID_") */
    char szVid[5] = {0};
    int i;
    for (i = 0; i < 4; i++) {
        char c = pVid[4 + i];
        if (!c) return FALSE;
        szVid[i] = (char)toupper((unsigned char)c);
    }

    /* Parse PID (4 hex digits after "PID_") */
    char szPid[5] = {0};
    for (i = 0; i < 4; i++) {
        char c = pPid[4 + i];
        if (!c) return FALSE;
        szPid[i] = (char)toupper((unsigned char)c);
    }

    *pwVid = (WORD)strtol(szVid, NULL, 16);
    *pwPid = (WORD)strtol(szPid, NULL, 16);
    return (*pwVid != 0 || *pwPid != 0);
}

/* Get the device instance path for a PhysicalDrive handle.*/
static BOOL GetDevicePathFromHandle(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    /* Walk the device tree from a PhysicalDrive handle to find the
     * USB parent device and extract its VID/PID.  This is the same */
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    /* Enumerate all disk class devices and match by adapter/serial */
    HDEVINFO hDevInfo = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_DISK,
                                              NULL, NULL,
                                              DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
        return FALSE;

    SP_DEVICE_INTERFACE_DATA did;
    did.cbSize = sizeof(did);

    BOOL bFound = FALSE;
    DWORD dwIndex = 0;

    while (SetupDiEnumDeviceInterfaces(hDevInfo, NULL,
                                       &GUID_DEVINTERFACE_DISK, dwIndex, &did)) {
        dwIndex++;

        /* Get required buffer size */
        DWORD dwReqSize = 0;
        SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, NULL, 0, &dwReqSize, NULL);
        if (dwReqSize == 0) continue;

        SP_DEVICE_INTERFACE_DETAIL_DATA_A* pDetail =
            (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)HeapAlloc(
                GetProcessHeap(), HEAP_ZERO_MEMORY, dwReqSize);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);

        SP_DEVINFO_DATA dd;
        dd.cbSize = sizeof(dd);

        if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, pDetail,
                                              dwReqSize, &dwReqSize, &dd)) {
            /* Check if this device path matches our PhysicalDrive */
            const char* szDetailPath = pDetail->DevicePath;

            /* Try to open this device path and compare with our handle */
            HANDLE hTest = CreateFileA(szDetailPath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                NULL, OPEN_EXISTING, 0, NULL);

            if (hTest != INVALID_HANDLE_VALUE) {
                /* Compare by getting disk number */
                STORAGE_DEVICE_NUMBER sdn1, sdn2;
                DWORD dwRet1 = 0, dwRet2 = 0;
                BOOL bOk1 = DeviceIoControl(hTest, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &sdn1, sizeof(sdn1), &dwRet1, NULL);
                BOOL bOk2 = DeviceIoControl(hDrive, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                    NULL, 0, &sdn2, sizeof(sdn2), &dwRet2, NULL);

                if (bOk1 && bOk2 && sdn1.DeviceNumber == sdn2.DeviceNumber) {
                    /* Match found - now walk up to find USB parent */
                    DEVINST devInst = dd.DevInst;
                    DEVINST parentInst = 0;
                    WORD wVid = 0, wPid = 0;

                    /* Walk up the device tree looking for USB\VID_ */
                    int maxWalk = 20;
                    while (maxWalk-- > 0) {
                        CHAR szHwId[512] = {0};
                        if (SetupDiGetDeviceRegistryPropertyA(hDevInfo, &dd,
                                SPDRP_HARDWAREID, NULL,
                                (BYTE*)szHwId, sizeof(szHwId), NULL)) {
                            if (ParseVidPidFromHardwareId(szHwId, &wVid, &wPid)) {
                                bFound = TRUE;
                                if (pwVid) *pwVid = wVid;
                                if (pwPid) *pwPid = wPid;
                                break;
                            }
                        }

                        /* Try parent device */
                        if (CM_Get_Parent(&parentInst, devInst, 0) != CR_SUCCESS)
                            break;

                        /* Get hardware ID of parent */
                        ULONG ulSize = 0;
                        CM_Get_Device_IDA(parentInst, NULL, 0, 0);
                        CM_Get_Device_ID_Size(&ulSize, parentInst, 0);
                        ulSize += 2;
                        char* szParentId = (char*)HeapAlloc(
                            GetProcessHeap(), HEAP_ZERO_MEMORY, ulSize);
                        if (szParentId) {
                            if (CM_Get_Device_IDA(parentInst, szParentId, ulSize, 0) == CR_SUCCESS) {
                                if (ParseVidPidFromHardwareId(szParentId, &wVid, &wPid)) {
                                    bFound = TRUE;
                                    if (pwVid) *pwVid = wVid;
                                    if (pwPid) *pwPid = wPid;
                                    HeapFree(GetProcessHeap(), 0, szParentId);
                                    break;
                                }
                            }
                            HeapFree(GetProcessHeap(), 0, szParentId);
                        }

                        devInst = parentInst;
                    }
                }
                CloseHandle(hTest);
            }
        }

        HeapFree(GetProcessHeap(), 0, pDetail);
        if (bFound) break;
    }

    SetupDiDestroyDeviceInfoList(hDevInfo);
    return bFound;
}

/* Fallback: try reading VID/PID from the STORAGE_DEVICE_DESCRIPTOR's
 * VendorID offset, which may contain USB VID info for some bridge chips.
 * Also try parsing the hardware ID from the device's SCSI address. */
static BOOL GetUSBVidPidFallback(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    /* Try to get VID/PID from the device descriptor's raw properties.
     * Some USB bridge chips expose VID/PID via the STORAGE_DEVICE_DESCRIPTOR. */
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    /* Try to parse VID/PID from the VendorId string.
     * Some USB-SATA bridges put "VID_xxxx&PID_xxxx" in the vendor ID. */
    if (pDesc->VendorIdOffset && pDesc->VendorIdOffset < dwBytes) {
        const char* pStr = (const char*)outBuf + pDesc->VendorIdOffset;
        if (ParseVidPidFromHardwareId(pStr, pwVid, pwPid))
            return TRUE;
    }

    /* Try from ProductId string */
    if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwBytes) {
        const char* pStr = (const char*)outBuf + pDesc->ProductIdOffset;
        if (ParseVidPidFromHardwareId(pStr, pwVid, pwPid))
            return TRUE;
    }

    return FALSE;
}

BOOL GetUSBVidPid(HANDLE hDrive, WORD* pwVid, WORD* pwPid)
{
    if (pwVid) *pwVid = 0;
    if (pwPid) *pwPid = 0;

    /* Method 1: SetupDi API — walk device tree to find USB parent (CDI primary method) */
    if (GetDevicePathFromHandle(hDrive, pwVid, pwPid))
        return TRUE;

    /* Method 2: descriptor strings (some bridges put VID_xxxx&PID_xxxx there). */
    if (GetUSBVidPidFallback(hDrive, pwVid, pwPid))
        return TRUE;

    return FALSE;
}

/* ============================================================
 * SAT (SCSI/ATA Translation) — for USB enclosures & SAS
 * ============================================================ */
static BOOL SATSendCommand12(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.CdbLength          = 12;
    sptwb.spt.SenseInfoLength    = 32;
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue       = 30;

    if (pDataBuf && dwDataLen > 0) {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = dwDataLen;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    } else {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_UNSPECIFIED;
        sptwb.spt.DataTransferLength = 0;
        sptwb.spt.DataBufferOffset   = 0;
    }

    sptwb.spt.Cdb[0] = SAT_ATA_PASSTHROUGH_12;
    sptwb.spt.Cdb[1] = bProtocol;
    sptwb.spt.Cdb[2] = (pDataBuf && dwDataLen > 0)
                         ? (SAT_FLAGS_CK_COND | SAT_FLAGS_TDIR_FROM_DEV | SAT_FLAGS_BYTE_BLOCK | SAT_FLAGS_TLEN_SECTOR_CNT)
                         : SAT_FLAGS_CK_COND;
    sptwb.spt.Cdb[3] = bFeatures;
    sptwb.spt.Cdb[4] = bSectorCnt;
    sptwb.spt.Cdb[5] = bLBALow;
    sptwb.spt.Cdb[6] = bCylLow;
    sptwb.spt.Cdb[7] = bCylHigh;
    sptwb.spt.Cdb[8] = 0xA0;
    sptwb.spt.Cdb[9] = bCommand;

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    if (pDataBuf && dwDataLen > 0) {
        dwInLen += dwDataLen;
    }

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL))
        return FALSE;

    if (sptwb.spt.ScsiStatus == 0x08 || sptwb.spt.ScsiStatus == 0x04)
        return FALSE;

    if (pDataBuf && dwDataLen > 0 && sptwb.spt.DataTransferLength > 0) {
        DWORD dwCopy = dwDataLen;
        if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
        if (dwCopy > 512) dwCopy = 512;
        memcpy(pDataBuf, sptwb.DataBuf, dwCopy);
    }

    return TRUE;
}

static BOOL SATSendCommand16(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.CdbLength          = 16;
    sptwb.spt.SenseInfoLength    = 32;
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue       = 30;

    if (pDataBuf && dwDataLen > 0) {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
        sptwb.spt.DataTransferLength = dwDataLen;
        sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    } else {
        sptwb.spt.DataIn             = SCSI_IOCTL_DATA_UNSPECIFIED;
        sptwb.spt.DataTransferLength = 0;
        sptwb.spt.DataBufferOffset   = 0;
    }

    /* CK_COND flag (0x20) for SAT commands.
     * Many USB-SATA bridge chips (especially JMicron) require this flag. */
    sptwb.spt.Cdb[0]  = SAT_ATA_PASSTHROUGH_16;
    sptwb.spt.Cdb[1]  = bProtocol;
    sptwb.spt.Cdb[2]  = (pDataBuf && dwDataLen > 0)
                          ? (SAT_FLAGS_CK_COND | SAT_FLAGS_TDIR_FROM_DEV | SAT_FLAGS_BYTE_BLOCK | SAT_FLAGS_TLEN_SECTOR_CNT)
                          : SAT_FLAGS_CK_COND;
    sptwb.spt.Cdb[3]  = 0;
    sptwb.spt.Cdb[4]  = bFeatures;
    sptwb.spt.Cdb[5]  = 0;
    sptwb.spt.Cdb[6]  = bSectorCnt;
    sptwb.spt.Cdb[7]  = 0;
    sptwb.spt.Cdb[8]  = bLBALow;
    sptwb.spt.Cdb[9]  = 0;
    sptwb.spt.Cdb[10] = bCylLow;
    sptwb.spt.Cdb[11] = 0;
    sptwb.spt.Cdb[12] = bCylHigh;
    sptwb.spt.Cdb[13] = 0xA0;  /* LBA mode */
    sptwb.spt.Cdb[14] = bCommand;
    sptwb.spt.Cdb[15] = 0;

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);
    if (pDataBuf && dwDataLen > 0) {
        dwInLen += dwDataLen;
    }

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL))
        return FALSE;

    /* When CK_COND is set, ScsiStatus may be 0x02 (CHECK CONDITION)
     * which is NORMAL per SAT spec — the ATA status is returned in sense data.
     * Only treat it as failure if ScsiStatus indicates a transport error. */
    if (sptwb.spt.ScsiStatus == 0x08 ||   /* BUSY */
        sptwb.spt.ScsiStatus == 0x04 ||   /* CONDITION MET (abnormal for SAT) */
        sptwb.spt.ScsiStatus == 0x18)     /* RESERVATION CONFLICT */
        return FALSE;

    /* Copy data from embedded buffer to caller's buffer */
    if (pDataBuf && dwDataLen > 0 && sptwb.spt.DataTransferLength > 0) {
        DWORD dwCopy = dwDataLen;
        if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
        if (dwCopy > 512) dwCopy = 512;
        memcpy(pDataBuf, sptwb.DataBuf, dwCopy);
    }

    return TRUE;
}

static BOOL SATSendCommand(HANDLE hDrive, BYTE bFeatures, BYTE bSectorCnt,
    BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand, BYTE bProtocol,
    BYTE* pDataBuf, DWORD dwDataLen, SMART_ACCESS_METHOD* pMethod)
{
    if (SATSendCommand16(hDrive, bFeatures, bSectorCnt, bLBALow,
                         bCylLow, bCylHigh, bCommand, bProtocol, pDataBuf, dwDataLen)) {
        if (pMethod) *pMethod = SMART_ACCESS_SAT16;
        return TRUE;
    }
    if (SATSendCommand12(hDrive, bFeatures, bSectorCnt, bLBALow,
                         bCylLow, bCylHigh, bCommand, bProtocol, pDataBuf, dwDataLen)) {
        if (pMethod) *pMethod = SMART_ACCESS_SAT12;
        return TRUE;
    }
    return FALSE;
}

BOOL EnableSMARTSAT(HANDLE hDrive)
{
    SMART_ACCESS_METHOD m = SMART_ACCESS_NONE;
    return SATSendCommand(hDrive, SMART_ENABLE, 1, 1,
        SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
        SAT_PROTO_NON_DATA, NULL, 0, &m);
}

BOOL GetIdentifyDataSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[IDENTIFY_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    if (!SATSendCommand(hDrive, 0, 1, 0, 0, 0,
            ID_CMD, SAT_PROTO_PIO_IN, data, IDENTIFY_BUFFER_SIZE, &method))
        return FALSE;

    if (IsBufferAllZero(data, 64)) return FALSE;

    WORD* pIdent = (WORD*)data;
    SwapATAString(pInfo->szSerial,   &pIdent[10], 10);
    SwapATAString(pInfo->szFirmware, &pIdent[23], 4);
    SwapATAString(pInfo->szModel,    &pIdent[27], 20);

    DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = IdentLba48(pIdent);
    }
    if (qwSectors == 0) qwSectors = (unsigned __int64)dwSectors28;
    if (qwSectors > 0)
        pInfo->dwCapacityMB = (DWORD)(qwSectors * 512 / (1024 * 1024));

    pInfo->bSMART_Supported = TRUE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;
    pInfo->bIsUSB           = TRUE;
    pInfo->eAccessMethod    = method;
    pInfo->wRotationRate    = pIdent[217];
    FillAtaProtocolFromIdent(pInfo, pIdent);
    return TRUE;
}

BOOL GetSMARTAttributesSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_ATTRIBUTE_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    EnableSMARTSAT(hDrive);

    if (!SATSendCommand(hDrive, SMART_READ_DATA, 1, 0x00,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, READ_ATTRIBUTE_BUFFER_SIZE, &method)) {
        pInfo->dwErrSat16 = GetLastError();
        pInfo->dwErrSat12 = pInfo->dwErrSat16;
        return FALSE;
    }

    if (IsBufferAllZero(data + 2, 30) || IsBufferAllFF(data + 2, 30))
        return FALSE;

    if (FillSmartData(pInfo, data) <= 0)
        return FALSE;
    if (pInfo->eAccessMethod == SMART_ACCESS_NONE) pInfo->eAccessMethod = method;
    return TRUE;
}

BOOL GetSMARTThresholdsSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_THRESHOLD_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;

    if (!SATSendCommand(hDrive, SMART_READ_THRESHOLDS, 1, 0x00,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, READ_THRESHOLD_BUFFER_SIZE, &method))
        return FALSE;

    FillSmartThreshold(pInfo, data);
    return TRUE;
}

/* ============================================================
 * SCSI LOG SENSE
 * ============================================================ */
#define SCSI_LOG_SENSE_CMD          0x4D
#define SCSI_LOGPAGE_TEMPERATURE    0x0D
#define SCSI_LOGPAGE_INFO_EXCEPTIONS 0x2F
#define SCSI_ASC_FAILURE_PREDICTED  0x5D

static BOOL SCSILogSense(HANDLE hDrive, BYTE bPageCode, BYTE* pOut, DWORD dwOutLen, DWORD* pdwLastError)
{
    CDI_SAT_PASSTHROUGH_BUF sptwb;
    DWORD dwBytes = 0;
    ZeroMemory(&sptwb, sizeof(sptwb));

    sptwb.spt.Length              = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.CdbLength           = 10;
    sptwb.spt.SenseInfoLength     = 32;
    sptwb.spt.SenseInfoOffset     = offsetof(CDI_SAT_PASSTHROUGH_BUF, SenseBuf);
    sptwb.spt.TimeOutValue        = 10;
    sptwb.spt.DataIn              = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength  = dwOutLen;
    sptwb.spt.DataBufferOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf);

    sptwb.spt.Cdb[0] = SCSI_LOG_SENSE_CMD;
    sptwb.spt.Cdb[1] = 0x00;
    sptwb.spt.Cdb[2] = (BYTE)(0x40 | (bPageCode & 0x3F));
    sptwb.spt.Cdb[3] = 0x00;
    sptwb.spt.Cdb[7] = (BYTE)((dwOutLen >> 8) & 0xFF);
    sptwb.spt.Cdb[8] = (BYTE)(dwOutLen & 0xFF);

    DWORD dwInLen = offsetof(CDI_SAT_PASSTHROUGH_BUF, DataBuf) + dwOutLen;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, dwInLen, &sptwb, sizeof(sptwb), &dwBytes, NULL)) {
        if (pdwLastError) *pdwLastError = GetLastError();
        return FALSE;
    }
    if (sptwb.spt.ScsiStatus != 0) {
        if (pdwLastError) *pdwLastError = (DWORD)0x10000 + sptwb.spt.ScsiStatus;
        return FALSE;
    }

    /* Copy data from embedded buffer */
    DWORD dwCopy = dwOutLen;
    if (dwCopy > sizeof(sptwb.DataBuf)) dwCopy = sizeof(sptwb.DataBuf);
    if (dwCopy > sptwb.spt.DataTransferLength) dwCopy = sptwb.spt.DataTransferLength;
    memcpy(pOut, sptwb.DataBuf, dwCopy);

    if (pdwLastError) *pdwLastError = 0;
    return TRUE;
}

BOOL GetSMARTViaLogSense(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE buf[64];
    BOOL bAny = FALSE;
    DWORD dwErr = 0;

    if (SCSILogSense(hDrive, SCSI_LOGPAGE_INFO_EXCEPTIONS, buf, sizeof(buf), &dwErr)) {
        if (buf[0] == SCSI_LOGPAGE_INFO_EXCEPTIONS) {
            if (buf[7] >= 1) {
                BYTE bASC = buf[8];
                if (bASC == SCSI_ASC_FAILURE_PREDICTED) {
                    pInfo->bPredictFailure = TRUE;
                    /* Do NOT set nHealthPercent = 0 here!
                     * Health % and health status are separate.
                     * Predictive failure affects STATUS (Warning),
                     * not the health PERCENTAGE. HDD Sentinel still
                     * shows the calculated health % even when
                     * predictive failure is detected. */
                }
                pInfo->bGotReturnStatus = TRUE;
                bAny = TRUE;
            }
        }
    }
    pInfo->dwErrLogSense = dwErr;

    if (SCSILogSense(hDrive, SCSI_LOGPAGE_TEMPERATURE, buf, sizeof(buf), &dwErr)) {
        if (buf[0] == SCSI_LOGPAGE_TEMPERATURE && buf[7] >= 2) {
            BYTE bTemp = buf[9];
            if (bTemp != 0xFF && bTemp > 0 && bTemp <= 150) {
                pInfo->nTemperatureC = (int)bTemp;
                bAny = TRUE;
            }
        }
    }
    if (pInfo->dwErrLogSense == 0) pInfo->dwErrLogSense = dwErr;

    if (bAny) pInfo->eAccessMethod = SMART_ACCESS_SAT16;
    return bAny;
}

/* USB descriptor-only path (last resort) */
BOOL GetIdentifyDataUSB(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (!GetDeviceDescriptor(hDrive, pInfo)) return FALSE;
    GetCapacityFromGeometry(hDrive, pInfo);

    pInfo->bSMART_Supported = FALSE;
    pInfo->bSMART_Enabled   = FALSE;
    /* Only mark USB when the bus really is USB. Native NVMe is a SCSI
     * device to Windows; treating it as USB sent it down SCSI Log Sense
     * instead of the NVMe Health Log. */
    if (GetStorageBusType(hDrive) == 7) {
        pInfo->bIsUSB = TRUE;
        pInfo->eType  = DRIVE_TYPE_USB;
    }
    pInfo->nHealthPercent   = -1;
    pInfo->eAccessMethod    = SMART_ACCESS_STORAGE_QUERY;
    return (pInfo->szModel[0] != '\0');
}


static BOOL ReadSMARTLogSAT(HANDLE hDrive, BYTE bLogAddr,
                            BYTE* pOutBuf, DWORD dwBufSize)
{
    SMART_ACCESS_METHOD method = SMART_ACCESS_NONE;
    BYTE data[512];
    ZeroMemory(data, sizeof(data));

    if (!SATSendCommand(hDrive, SMART_READ_LOG, 1, bLogAddr,
            SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD,
            SAT_PROTO_PIO_IN, data, 512, &method))
        return FALSE;

    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, data, dwCopy);
    return TRUE;
}

BOOL GetSMARTErrorLogSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLogSAT(hDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLogSAT(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLogSAT(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * USB bridge chip detection
 * CDI uses VID/PID matching to select the correct command type
 * for each bridge chip family.
 * ============================================================ */
USB_BRIDGE_TYPE DetectUsbBridgeType(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    WORD vid = pInfo->wUsbVid;
    WORD pid = pInfo->wUsbPid;

    if (vid == 0 || pid == 0) {
        GetUSBVidPid(hDrive, &pInfo->wUsbVid, &pInfo->wUsbPid);
        vid = pInfo->wUsbVid;
        pid = pInfo->wUsbPid;
    }

    /* JMicron SATA bridges */
    if (vid == 0x152D) {
        if (pid == 0x0583 || pid == 0x0586 || pid == 0x058C || pid == 0x058F)
            return USB_BRIDGE_NVME_JMICRON;  /* NVMe bridges */
        if (pid == 0x0578 || pid == 0x0565 || pid == 0x0562 || pid == 0x0566 ||
            pid == 0x0579 || pid == 0x0577 || pid == 0x2566 || pid == 0x0571)
            return USB_BRIDGE_JMICRON;       /* SATA bridges */
        /* Default JMicron → try JMicron SAT first */
        return USB_BRIDGE_JMICRON;
    }

    /* ASMedia bridges */
    if (vid == 0x174C) {
        if (pid == 0x2362 || pid == 0x2364)
            return USB_BRIDGE_NVME_ASMEDIA;
        if (pid == 0x1352 || pid == 0x1351)
            return USB_BRIDGE_ASM1352R;
        return USB_BRIDGE_SAT;  /* Other ASMedia → standard SAT */
    }

    /* Realtek bridges (RTL9210/B dual-mode and other USB-SATA) */
    if (vid == 0x0BDA) {
        if ((pid & 0xFF00) == 0x9200 || pid == 0x9210 || pid == 0x9211 ||
            pid == 0x9220 || pid == 0x9221)
            return USB_BRIDGE_NVME_REALTEK;
        return USB_BRIDGE_SAT;
    }

    /* Sunplus bridges */
    if (vid == 0x04FC) return USB_BRIDGE_SUNPLUS;
    if (vid == 0x04E8 && pid == 0x5100) return USB_BRIDGE_SUNPLUS; /* Samsung USB */

    /* Cypress/I-O Data bridges */
    if (vid == 0x04B4) return USB_BRIDGE_CYPRESS;
    if (vid == 0x04BB) return USB_BRIDGE_IO_DATA;  /* I-O Data */

    /* Prolific bridges */
    if (vid == 0x067B) return USB_BRIDGE_PROLIFIC;

    /* Logitec bridges */
    if (vid == 0x0789) return USB_BRIDGE_LOGITEC;

    /* VIA Labs NVMe bridges */
    if (vid == 0x2109) {
        if (pid == 0x0900 || pid == 0x0901 || pid == 0x0902)
            return USB_BRIDGE_NVME_VLI;
        return USB_BRIDGE_SAT;
    }

    /* Initio bridges */
    if (vid == 0x13FD) return USB_BRIDGE_SAT;

    /* Western Digital bridges */
    if (vid == 0x1058) return USB_BRIDGE_SAT;

    /* Seagate bridges */
    if (vid == 0x0BC2) return USB_BRIDGE_SAT;

    /* FMA NL6221 NVMe bridge */
    if (vid == 0x0BDA) {
        /* Already handled above for Realtek NVMe bridges, but
         * additional PIDs for FMA-branded NVMe bridges */
        if (pid == 0x9220 || pid == 0x9221)
            return USB_BRIDGE_NVME_FMA;
        return USB_BRIDGE_NVME_REALTEK;
    }

    /* Default: try standard SAT */
    return USB_BRIDGE_SAT;
}

/* ============================================================
 * NVMe-over-USB: JMicron JMS583/586 (CDI-style)
 * JMicron NVMe bridges use vendor-specific SCSI commands
 * with opcode 0xA1 and "NVMe" signature in the data buffer.
 * ============================================================ */

/* Heap SPT buffer for Realtek 0xE4 (Identify 4096 / Health 512).
 * Stack 4K SPT was OK for a single probe; keep it off the stack anyway. */
static CDI_SAT_PASSTHROUGH_BUF_4K* AllocSptBuf4K(void)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* p =
        (CDI_SAT_PASSTHROUGH_BUF_4K*)VirtualAlloc(
            NULL, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K),
            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (p)
        ZeroMemory(p, sizeof(*p));
    return p;
}

static void FreeSptBuf4K(CDI_SAT_PASSTHROUGH_BUF_4K* p)
{
    if (p)
        VirtualFree(p, 0, MEM_RELEASE);
}

BOOL NVMeIdentifyJMicron(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Send NVMe Identify command request */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;   /* NVMe PASS THROUGH */
    sptwb.spt.Cdb[1] = 0x80;   /* ADMIN command */
    sptwb.spt.Cdb[4] = 0x02;   /* Identify */

    /* NVMe signature and Identify Controller command */
    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';
    sptwb.DataBuf[8] = 0x06;   /* NVMe Identify opcode */
    sptwb.DataBuf[0x30] = 0x01; /* CNS = 1 (Controller) */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read NVMe Identify response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;   /* NVMe PASS THROUGH */
    sptwb.spt.Cdb[1] = 0x82;   /* ADMIN + DMA-IN */
    sptwb.spt.Cdb[4] = 0x10;   /* Transfer length */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Extract model/serial/firmware from NVMe Identify data.
     * NVMe uses direct ASCII (no byte-swap needed), unlike ATA.
     * memcpy for NVMe strings. */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        /* NVMe Identify Controller: Serial at offset 4 (20 bytes),
         * Model at offset 24 (40 bytes), Firmware at offset 64 (8 bytes) */
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);
    }

    return (pInfo->szModel[0] != '\0');
}

BOOL NVMeHealthLogJMicron(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Request NVMe Health Log */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;
    sptwb.spt.Cdb[1] = 0x80;   /* ADMIN */
    sptwb.spt.Cdb[4] = 0x02;   /* Get Log Page */

    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';
    sptwb.DataBuf[8] = 0x02;   /* NVMe Get Log Page opcode */
    sptwb.DataBuf[0x30] = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read Health Log response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 12;

    sptwb.spt.Cdb[0] = 0xA1;
    sptwb.spt.Cdb[1] = 0x82;   /* ADMIN + DMA-IN */
    sptwb.spt.Cdb[4] = 0x02;   /* Transfer length for 512 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: ASMedia ASM2362 (CDI-style)
 * ASMedia uses vendor-specific opcode 0xE6 for Identify
 * and 0xE7 for Get Log Page.
 * ============================================================ */
BOOL NVMeIdentifyASMedia(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0] = 0xE6;   /* ASMedia NVMe Pass-Through */
    sptwb.spt.Cdb[1] = 0x06;   /* Identify */
    sptwb.spt.Cdb[3] = 0x01;   /* CNS = 1 */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* NVMe Identify uses direct ASCII, no byte-swap needed.
     * Uses memcpy for NVMe strings. */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);
    }

    return (pInfo->szModel[0] != '\0');
}

BOOL NVMeHealthLogASMedia(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0] = 0xE7;   /* ASMedia NVMe Get Log */
    sptwb.spt.Cdb[1] = 0x02;   /* Log Page 02h (Health) */
    sptwb.spt.Cdb[3] = NVME_LOG_PAGE_HEALTH_INFO;

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: Realtek RTL9210 (CDI-style)
 * Realtek uses vendor-specific opcode 0xE4 for Read and
 * 0xE5 for Write, with subcommands in CDB[3].
 * ============================================================ */
BOOL NVMeIdentifyRealtek(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* psptwb;
    DWORD dwReturned = 0;
    DWORD length;
    BYTE* pIdentBuf;
    BOOL bOk;

    psptwb = AllocSptBuf4K();
    if (!psptwb)
        return FALSE;

    psptwb->spt.Length             = sizeof(SCSI_PASS_THROUGH);
    psptwb->spt.PathId             = 0;
    psptwb->spt.TargetId           = 0;
    psptwb->spt.Lun                = 0;
    psptwb->spt.SenseInfoLength    = 32;
    psptwb->spt.DataIn             = SCSI_IOCTL_DATA_IN;
    psptwb->spt.DataTransferLength = 4096;
    psptwb->spt.TimeOutValue       = 8;
    psptwb->spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    psptwb->spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    psptwb->spt.CdbLength          = 16;

    psptwb->spt.Cdb[0] = 0xE4;   /* Realtek NVMe Read — never 0xE5 (write) */
    psptwb->spt.Cdb[1] = (BYTE)(4096);         /* Transfer length low */
    psptwb->spt.Cdb[2] = (BYTE)(4096 >> 8);    /* Transfer length high */
    psptwb->spt.Cdb[3] = 0x06;   /* Identify */
    psptwb->spt.Cdb[4] = 0x01;   /* CNS = 1 */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            psptwb, length, psptwb, length, &dwReturned, NULL)) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    /* NVMe Identify uses direct ASCII, no byte-swap needed.
     * CopyNvmeIdentBuf never copies more than 4096 into nvmeIdent. */
    pIdentBuf = psptwb->DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);
    }

    bOk = (pInfo->szModel[0] != '\0');
    FreeSptBuf4K(psptwb);
    return bOk;
}

BOOL NVMeHealthLogRealtek(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K* psptwb;
    DWORD dwReturned = 0;
    DWORD length;
    DWORD nCopy;

    psptwb = AllocSptBuf4K();
    if (!psptwb)
        return FALSE;

    psptwb->spt.Length             = sizeof(SCSI_PASS_THROUGH);
    psptwb->spt.PathId             = 0;
    psptwb->spt.TargetId           = 0;
    psptwb->spt.Lun                = 0;
    psptwb->spt.SenseInfoLength    = 32;
    psptwb->spt.DataIn             = SCSI_IOCTL_DATA_IN;
    psptwb->spt.DataTransferLength = 512;
    psptwb->spt.TimeOutValue       = 8;
    psptwb->spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    psptwb->spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    psptwb->spt.CdbLength          = 16;

    psptwb->spt.Cdb[0] = 0xE4;   /* Realtek NVMe Read — never 0xE5 (write) */
    psptwb->spt.Cdb[1] = (BYTE)(512);          /* Transfer length low */
    psptwb->spt.Cdb[2] = (BYTE)(512 >> 8);     /* Transfer length high */
    psptwb->spt.Cdb[3] = 0x02;   /* Get Log Page */
    psptwb->spt.Cdb[4] = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            psptwb, length, psptwb, length, &dwReturned, NULL)) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    if (IsBufferAllZero(psptwb->DataBuf, sizeof(NVME_HEALTH_INFO_LOG))) {
        FreeSptBuf4K(psptwb);
        return FALSE;
    }

    nCopy = sizeof(NVME_HEALTH_INFO_LOG);
    if (nCopy > 512)
        nCopy = 512;
    memcpy(&pInfo->nvmeHealth, psptwb->DataBuf, nCopy);
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    FreeSptBuf4K(psptwb);
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: VLI VL716/VL717 (CDI-style)
 * VLI NVMe bridges use vendor-specific SCSI commands.
 * VL716 uses opcode 0xC0 for NVMe passthrough with a
 * proprietary command structure similar to JMicron.
 * ============================================================ */
BOOL NVMeIdentifyVLI(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Send NVMe Identify command request */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x01;   /* Admin command */
    sptwb.spt.Cdb[2]  = 0x06;   /* Identify */
    sptwb.spt.Cdb[3]  = 0x01;   /* CNS = 1 (Controller) */
    sptwb.spt.Cdb[6]  = 0x00;   /* Namespace = 0 */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length 512*2 */

    /* NVMe signature */
    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read NVMe Identify response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 4096;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x02;   /* DMA-IN */
    sptwb.spt.Cdb[10] = 0x20;   /* Transfer length for 4096 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 4096;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* NVMe Identify uses direct ASCII, no byte-swap needed */
    BYTE* pIdentBuf = sptwb.DataBuf;
    if (!IsBufferAllZero(pIdentBuf, 64)) {
        memcpy(pInfo->szSerial, pIdentBuf + 4, 20);
        pInfo->szSerial[20] = '\0';
        TrimStr(pInfo->szSerial);

        memcpy(pInfo->szModel, pIdentBuf + 24, 40);
        pInfo->szModel[40] = '\0';
        TrimStr(pInfo->szModel);

        memcpy(pInfo->szFirmware, pIdentBuf + 64, 8);
        pInfo->szFirmware[8] = '\0';
        TrimStr(pInfo->szFirmware);

        pInfo->bGotNVMeIdent = TRUE;
        CopyNvmeIdentBuf(pInfo, pIdentBuf, 4096);
    }

    return (pInfo->szModel[0] != '\0');
}

BOOL NVMeHealthLogVLI(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    CDI_SAT_PASSTHROUGH_BUF_4K sptwb;
    DWORD dwReturned = 0;
    DWORD length;

    /* Step 1: Request NVMe Health Log */
    ZeroMemory(&sptwb, sizeof(sptwb));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_OUT;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x01;   /* Admin command */
    sptwb.spt.Cdb[2]  = 0x02;   /* Get Log Page */
    sptwb.spt.Cdb[3]  = NVME_LOG_PAGE_HEALTH_INFO; /* Log page 02h */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length */

    sptwb.DataBuf[0] = 'N'; sptwb.DataBuf[1] = 'V';
    sptwb.DataBuf[2] = 'M'; sptwb.DataBuf[3] = 'E';

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    /* Step 2: Read Health Log response */
    ZeroMemory(&sptwb, sizeof(CDI_SAT_PASSTHROUGH_BUF_4K));
    sptwb.spt.Length             = sizeof(SCSI_PASS_THROUGH);
    sptwb.spt.PathId             = 0;
    sptwb.spt.TargetId           = 0;
    sptwb.spt.Lun                = 0;
    sptwb.spt.SenseInfoLength    = 24;
    sptwb.spt.DataIn             = SCSI_IOCTL_DATA_IN;
    sptwb.spt.DataTransferLength = 512;
    sptwb.spt.TimeOutValue       = 10;
    sptwb.spt.DataBufferOffset   = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf);
    sptwb.spt.SenseInfoOffset    = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, SenseBuf);
    sptwb.spt.CdbLength          = 16;

    sptwb.spt.Cdb[0]  = 0xC0;   /* VLI NVMe Pass-Through */
    sptwb.spt.Cdb[1]  = 0x02;   /* DMA-IN */
    sptwb.spt.Cdb[10] = 0x02;   /* Transfer length for 512 bytes */

    length = offsetof(CDI_SAT_PASSTHROUGH_BUF_4K, DataBuf) + 512;

    if (!DeviceIoControl(hDrive, IOCTL_SCSI_PASS_THROUGH,
            &sptwb, length, &sptwb, length, &dwReturned, NULL))
        return FALSE;

    if (IsBufferAllZero(sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG)))
        return FALSE;

    memcpy(&pInfo->nvmeHealth, sptwb.DataBuf, sizeof(NVME_HEALTH_INFO_LOG));
    pInfo->bSMART_Supported = TRUE;
    pInfo->bIsNVMe = TRUE;
    pInfo->eAccessMethod = SMART_ACCESS_NVME_PASSTHROUGH;
    return TRUE;
}

/* ============================================================
 * NVMe-over-USB: FMA NL6221 (CDI-style)
 * FMA NL6221 uses Realtek-compatible commands with slight
 * variations. We try the Realtek protocol first, then
 * fallback to a generic NVMe-over-USB approach.
 * ============================================================ */

/* ============================================================
 * Generic NVMe-over-USB bridge detection and SMART reading
 * Tries all known bridge protocols in sequence
 * when the bridge type is unknown or auto-detect fails.
 * ============================================================ */
BOOL NVMeOverUSBTryAll(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    /* Try known NVMe-over-USB bridge protocols. Never shotgun JMicron /
     * ASMedia / VLI / native NvmeMini / 0xE4 at a known Realtek RTL9210.
     * Realtek 0xE4 is a one-shot in ScanDrives only. */

    if (IsRealtekNvmeUsbBridge(pInfo))
        return FALSE;

    /* 1. JMicron JMS583/586 */
    if (NVMeIdentifyJMicron(hDrive, pInfo)) {
        if (NVMeHealthLogJMicron(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 2. ASMedia ASM2362 */
    if (NVMeIdentifyASMedia(hDrive, pInfo)) {
        if (NVMeHealthLogASMedia(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 3. Realtek RTL9210 — 0xE4 only from ScanDrives, never here. */

    /* 4. VLI VL716/VL717 */
    if (NVMeIdentifyVLI(hDrive, pInfo)) {
        if (NVMeHealthLogVLI(hDrive, pInfo)) {
            return TRUE;
        }
    }

    /* 5. Native NVMe protocol query. GetNVMeIdentifyController /
     * GetNVMeHealthLogEx refuse USB (bus type 7), so this is a no-op
     * on USB handles. Thunderbolt/internal NVMe is not USB. */
    if (GetNVMeIdentifyController(hDrive, pInfo)) {
        if (GetNVMeHealthLogEx(hDrive, pInfo)) {
            return TRUE;
        }
    }

    return FALSE;
}

/* Heuristic: consumer USB flash almost never exposes SMART. Known HDD/SSD
 * enclosure bridges (RTL9210, JMicron, ASMedia, Realtek NVMe, …) are NOT flash. */
BOOL UsbBridgeLooksLikeEnclosure(const DRIVE_INFO* p)
{
    char hay[384];
    if (!p) return FALSE;
    /* USB VID of known enclosure chips — even when INQUIRY is the disk. */
    if (p->wUsbVid == 0x0BDA || p->wUsbVid == 0x152D || p->wUsbVid == 0x174C ||
        p->wUsbVid == 0x2109 || p->wUsbVid == 0x13FD || p->wUsbVid == 0x1058 ||
        p->wUsbVid == 0x0BC2)
        return TRUE;
    switch (p->eUsbBridgeType) {
    case USB_BRIDGE_NVME_JMICRON:
    case USB_BRIDGE_NVME_ASMEDIA:
    case USB_BRIDGE_NVME_REALTEK:
    case USB_BRIDGE_NVME_VLI:
    case USB_BRIDGE_NVME_FMA:
    case USB_BRIDGE_ASM1352R:
    case USB_BRIDGE_JMICRON:
    case USB_BRIDGE_SUNPLUS:
    case USB_BRIDGE_CYPRESS:
    case USB_BRIDGE_IO_DATA:
    case USB_BRIDGE_LOGITEC:
    case USB_BRIDGE_PROLIFIC:
        return TRUE;
    default:
        break;
    }
    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);
    if (strstr(hay, "RTL9210") || strstr(hay, "RTL921") ||
        strstr(hay, "Realtek") || strstr(hay, "REALTEK") ||
        strstr(hay, "JMS") || strstr(hay, "JMicron") || strstr(hay, "JMICRON") ||
        strstr(hay, "ASMedia") || strstr(hay, "ASMEDIA") ||
        strstr(hay, "ASM1") || strstr(hay, "ASM2") ||
        strstr(hay, "VL71") || strstr(hay, "VL716") || strstr(hay, "VL717"))
        return TRUE;
    return FALSE;
}

BOOL IsLikelyUsbFlashDrive(const DRIVE_INFO* p)
{
    char hay[384];
    if (!p || !p->bIsUSB || p->bIsNVMe) return FALSE;
    if (UsbBridgeLooksLikeEnclosure(p)) return FALSE;

    safe_snprintf(hay, "%s %s %s",
              p->szModel, p->szBridgeVendor, p->szBridgeProduct);

    /* SATA/NVMe SSD in an enclosure (Kingston A400, etc.) is NOT a stick. */
    if (strstr(hay, "A400") || strstr(hay, "SA400") ||
        strstr(hay, "SSD") || strstr(hay, "NVMe") || strstr(hay, "NVME"))
        return FALSE;

    /* Brand alone is not enough: Kingston/SanDisk also make SATA SSDs.
     * Match flash product names only. */
    if (strstr(hay, "Cruzer") || strstr(hay, "DataTraveler") ||
        strstr(hay, "JetFlash") || strstr(hay, "Flash Drive") ||
        strstr(hay, "USB DISK") || strstr(hay, "USB Flash") ||
        strstr(hay, "3.2Gen") || strstr(hay, "Ultra Fit") ||
        strstr(hay, "UFD") != NULL)
        return TRUE;
    return FALSE;
}

