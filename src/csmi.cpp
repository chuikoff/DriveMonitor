/* DriveMonitor - Common Storage Management Interface (CSMI) & Intel RST RAID.
 * GPL-2.0-or-later. See LICENSE. */
#include "csmi.h"
#include "smart_internal.h"

BOOL CsmiProbeController(int nScsiPort, HANDLE* phScsi)
{
    if (phScsi) *phScsi = INVALID_HANDLE_VALUE;
    if (nScsiPort < 0 || nScsiPort > 31) return FALSE;

    char szPath[32];
    safe_snprintf(szPath, "\\\\.\\Scsi%d:", nScsiPort);
    HANDLE h = CreateFileA(szPath, GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    CSMI_SAS_DRIVER_INFO_BUFFER buf;
    DWORD dwRet = 0;

    /* 1. Try CC_CSMI_SAS_GET_DRIVER_INFO with "CSMI_SAS" */
    ZeroMemory(&buf, sizeof(buf));
    buf.IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(buf.IoctlHeader.Signature, CSMI_SAS_SIGNATURE, 8);
    buf.IoctlHeader.Timeout = 5;
    buf.IoctlHeader.ControlCode = CC_CSMI_SAS_GET_DRIVER_INFO;
    buf.IoctlHeader.Length = (ULONG)sizeof(CSMI_SAS_DRIVER_INFO);

    if (DeviceIoControl(h, IOCTL_SCSI_MINIPORT, &buf, (DWORD)sizeof(buf),
                        &buf, (DWORD)sizeof(buf), &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (phScsi) *phScsi = h;
        else CloseHandle(h);
        return TRUE;
    }

    /* 2. Try with "CSMIALL " signature */
    ZeroMemory(&buf, sizeof(buf));
    buf.IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(buf.IoctlHeader.Signature, CSMI_ALL_SIGNATURE, 8);
    buf.IoctlHeader.Timeout = 5;
    buf.IoctlHeader.ControlCode = CC_CSMI_SAS_GET_DRIVER_INFO;
    buf.IoctlHeader.Length = (ULONG)sizeof(CSMI_SAS_DRIVER_INFO);

    if (DeviceIoControl(h, IOCTL_SCSI_MINIPORT, &buf, (DWORD)sizeof(buf),
                        &buf, (DWORD)sizeof(buf), &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (phScsi) *phScsi = h;
        else CloseHandle(h);
        return TRUE;
    }

    /* 3. Try 32-bit control code CC_CSMI_SAS_GET_DRIVER_INFO_32 */
    ZeroMemory(&buf, sizeof(buf));
    buf.IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(buf.IoctlHeader.Signature, CSMI_SAS_SIGNATURE, 8);
    buf.IoctlHeader.Timeout = 5;
    buf.IoctlHeader.ControlCode = CC_CSMI_SAS_GET_DRIVER_INFO_32;
    buf.IoctlHeader.Length = (ULONG)sizeof(CSMI_SAS_DRIVER_INFO);

    if (DeviceIoControl(h, IOCTL_SCSI_MINIPORT, &buf, (DWORD)sizeof(buf),
                        &buf, (DWORD)sizeof(buf), &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (phScsi) *phScsi = h;
        else CloseHandle(h);
        return TRUE;
    }

    CloseHandle(h);
    return FALSE;
}

static void InitCsmiStpBuffer(CSMI_SAS_STP_PASSTHRU_BUFFER* pBuf, DWORD bufSize,
                              ULONG ccode, const char* sig,
                              BYTE bPhy, BYTE bFeatures, BYTE bSectorCnt,
                              BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand,
                              BOOL bDataIn)
{
    ZeroMemory(pBuf, sizeof(CSMI_SAS_STP_PASSTHRU_BUFFER));
    pBuf->IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(pBuf->IoctlHeader.Signature, sig, 8);
    pBuf->IoctlHeader.Timeout = 10;
    pBuf->IoctlHeader.ControlCode = ccode;
    pBuf->IoctlHeader.Length = bufSize - (DWORD)sizeof(SRB_IO_CONTROL);

    pBuf->Parameters.bPhyIdentifier = bPhy;
    pBuf->Parameters.bPortIdentifier = 0;
    pBuf->Parameters.bConnectionRate = 0;

    /* Standard Register Host to Device FIS (FIS 27h) */
    pBuf->Parameters.bCommandFIS[0]  = 0x27; /* FIS Type 27h */
    pBuf->Parameters.bCommandFIS[1]  = 0x80; /* C=1 (command) */
    pBuf->Parameters.bCommandFIS[2]  = bCommand;
    pBuf->Parameters.bCommandFIS[3]  = bFeatures;
    pBuf->Parameters.bCommandFIS[4]  = bLBALow;
    pBuf->Parameters.bCommandFIS[5]  = bCylLow;
    pBuf->Parameters.bCommandFIS[6]  = bCylHigh;
    pBuf->Parameters.bCommandFIS[7]  = 0xA0; /* Device/Head */
    pBuf->Parameters.bCommandFIS[12] = bSectorCnt;

    if (bDataIn) {
        pBuf->Parameters.uFlags = CSMI_SAS_STP_READ | CSMI_SAS_STP_PIO;
        pBuf->Parameters.uDataLength = 512;
    } else {
        pBuf->Parameters.uFlags = CSMI_SAS_STP_UNSPECIFIED;
        pBuf->Parameters.uDataLength = 0;
    }
}

BOOL CsmiSendStpCommand(HANDLE hScsi, BYTE bPhy, BYTE bFeatures, BYTE bSectorCnt,
                        BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand,
                        BYTE* pDataBuf, DWORD dwDataLen, BOOL bDataIn)
{
    if (hScsi == INVALID_HANDLE_VALUE || !hScsi) return FALSE;
    if (bDataIn && (!pDataBuf || dwDataLen < 512)) return FALSE;

    CSMI_SAS_STP_PASSTHRU_BUFFER buf;
    DWORD dwRet = 0;
    DWORD bufSize = (DWORD)sizeof(CSMI_SAS_STP_PASSTHRU_BUFFER);
    if (!bDataIn) {
        bufSize = (DWORD)(sizeof(buf) - sizeof(buf.bDataBuffer));
    }

    /* 1. Try standard CC_CSMI_SAS_STP_PASSTHRU (25) */
    InitCsmiStpBuffer(&buf, bufSize, CC_CSMI_SAS_STP_PASSTHRU, CSMI_SAS_SIGNATURE,
                      bPhy, bFeatures, bSectorCnt, bLBALow, bCylLow, bCylHigh, bCommand, bDataIn);
    if (DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, &buf, bufSize, &buf, bufSize, &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (bDataIn && pDataBuf) {
            memcpy(pDataBuf, buf.bDataBuffer, 512);
        }
        return TRUE;
    }

    /* 2. Try 32-bit control code CC_CSMI_SAS_STP_PASSTHRU_32 (0xCC770019) */
    InitCsmiStpBuffer(&buf, bufSize, CC_CSMI_SAS_STP_PASSTHRU_32, CSMI_SAS_SIGNATURE,
                      bPhy, bFeatures, bSectorCnt, bLBALow, bCylLow, bCylHigh, bCommand, bDataIn);
    if (DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, &buf, bufSize, &buf, bufSize, &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (bDataIn && pDataBuf) {
            memcpy(pDataBuf, buf.bDataBuffer, 512);
        }
        return TRUE;
    }

    return FALSE;
}

BOOL CsmiGetIdentifyData(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo)
{
    BYTE data[IDENTIFY_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!CsmiSendStpCommand(hScsi, bPhy, 0, 1, 0, 0, 0, ID_CMD, data, (DWORD)sizeof(data), TRUE))
        return FALSE;

    if (ParseAtaIdentifyData(data, pInfo)) {
        pInfo->bSMART_Supported = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_CSMI;
        return TRUE;
    }
    return FALSE;
}

BOOL CsmiGetSMARTAttributes(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo)
{
    BYTE data[READ_ATTRIBUTE_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    /* Enable SMART */
    CsmiSendStpCommand(hScsi, bPhy, SMART_ENABLE, 1, 0, SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD, NULL, 0, FALSE);

    /* Read SMART data */
    if (!CsmiSendStpCommand(hScsi, bPhy, SMART_READ_DATA, 1, 0, SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD, data, (DWORD)sizeof(data), TRUE))
        return FALSE;

    if (FillSmartData(pInfo, data) <= 0)
        return FALSE;

    pInfo->eAccessMethod = SMART_ACCESS_CSMI;
    return TRUE;
}

BOOL CsmiGetSMARTThresholds(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo)
{
    BYTE data[READ_THRESHOLD_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!CsmiSendStpCommand(hScsi, bPhy, SMART_READ_THRESHOLDS, 1, 0, SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD, data, (DWORD)sizeof(data), TRUE))
        return FALSE;

    FillSmartThreshold(pInfo, data);
    return TRUE;
}

BOOL CsmiGetSMARTReturnStatus(HANDLE hScsi, BYTE bPhy, BOOL* pbFail)
{
    if (!pbFail) return FALSE;
    *pbFail = FALSE;

    CSMI_SAS_STP_PASSTHRU_BUFFER buf;
    DWORD dwRet = 0;
    DWORD bufSize = (DWORD)(sizeof(buf) - sizeof(buf.bDataBuffer));

    ZeroMemory(&buf, sizeof(buf));
    buf.IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(buf.IoctlHeader.Signature, CSMI_SAS_SIGNATURE, 8);
    buf.IoctlHeader.Timeout = 10;
    buf.IoctlHeader.ControlCode = CC_CSMI_SAS_STP_PASSTHRU;
    buf.IoctlHeader.Length = bufSize - (DWORD)sizeof(SRB_IO_CONTROL);

    buf.Parameters.bPhyIdentifier = bPhy;
    buf.Parameters.bCommandFIS[0]  = 0x27;
    buf.Parameters.bCommandFIS[1]  = 0x80;
    buf.Parameters.bCommandFIS[2]  = SMART_CMD;
    buf.Parameters.bCommandFIS[3]  = SMART_RETURN_STATUS;
    buf.Parameters.bCommandFIS[5]  = SMART_CYL_LOW;
    buf.Parameters.bCommandFIS[6]  = SMART_CYL_HI;
    buf.Parameters.bCommandFIS[7]  = 0xA0;

    if (DeviceIoControl(hScsi, IOCTL_SCSI_MINIPORT, &buf, bufSize, &buf, bufSize, &dwRet, NULL) &&
        buf.IoctlHeader.ReturnCode == 0) {
        if (buf.Status.bStatusFIS[5] == SMART_CYL_LOW && buf.Status.bStatusFIS[6] == SMART_CYL_HI) {
            *pbFail = FALSE;
            return TRUE;
        }
        if (buf.Status.bStatusFIS[5] == 0xF4 && buf.Status.bStatusFIS[6] == 0x2C) {
            *pbFail = TRUE;
            return TRUE;
        }
    }
    return FALSE;
}

int CsmiScanRaidDrives(DRIVE_INFO* pDrives, int nMaxDrives, int nCurrentCount)
{
    if (!pDrives || nCurrentCount >= nMaxDrives) return nCurrentCount;

    for (int port = 0; port < 16 && nCurrentCount < nMaxDrives; port++) {
        HANDLE hScsi = INVALID_HANDLE_VALUE;
        if (!CsmiProbeController(port, &hScsi)) continue;

        /* Probe Phys 0 to 7 on this controller */
        for (BYTE phy = 0; phy < 8 && nCurrentCount < nMaxDrives; phy++) {
            DRIVE_INFO tempInfo;
            ZeroMemory(&tempInfo, sizeof(tempInfo));

            if (!CsmiGetIdentifyData(hScsi, phy, &tempInfo)) continue;
            if (tempInfo.szModel[0] == '\0') continue;

            /* Check if this drive serial is already in pDrives */
            BOOL bDuplicate = FALSE;
            for (int d = 0; d < nCurrentCount; d++) {
                if (tempInfo.szSerial[0] && pDrives[d].szSerial[0] &&
                    strcmp(tempInfo.szSerial, pDrives[d].szSerial) == 0) {
                    bDuplicate = TRUE;
                    break;
                }
            }
            if (bDuplicate) continue;

            /* Found a distinct physical SATA drive behind RAID! */
            DRIVE_INFO* pNew = &pDrives[nCurrentCount];
            *pNew = tempInfo;
            pNew->nDriveIndex = nCurrentCount;
            pNew->nScsiPort = port;
            pNew->eType = DetectDriveType(INVALID_HANDLE_VALUE, pNew);
            pNew->bSMART_Supported = TRUE;
            pNew->eAccessMethod = SMART_ACCESS_CSMI;

            /* Retrieve SMART telemetry */
            CsmiGetSMARTAttributes(hScsi, phy, pNew);
            CsmiGetSMARTThresholds(hScsi, phy, pNew);

            ExtractTemperatureFromATA(pNew);
            ExtractCommonATACounters(pNew);
            ExtractSSDIndicators(pNew);
            IdentifyDriveParts(pNew);
            AssessDriveHealth(pNew);

            safe_snprintf(pNew->szProtocol, "RAID · CSMI (Port %d, Phy %d)", port, (int)phy);

            nCurrentCount++;
        }
        CloseHandle(hScsi);
    }

    return nCurrentCount;
}
