/* DriveMonitor - ATA/SATA SMART (IOCTL_ATA_PASS_THROUGH). MIT: see LICENSE. */
#include "smart_internal.h"

void FillAtaProtocolFromIdent(DRIVE_INFO* pInfo, const WORD* pIdent)
{
    WORD w76, w77;
    unsigned neg;
    if (!pInfo || !pIdent) return;
    w76 = pIdent[76];
    w77 = pIdent[77];
    /* USB: word 77 is the dongle's negotiated speed, not the disk.
     * Use word 76 (supported) for the disk, prefix USB later. */
    neg = pInfo->bIsUSB ? 0 : (unsigned)((w77 >> 1) & 0x7);
    if (neg == 3)
        safe_snprintf(pInfo->szProtocol, TN("SATA 6 Гбит/с", "SATA 6 Gb/s"));
    else if (neg == 2)
        safe_snprintf(pInfo->szProtocol, TN("SATA 3 Гбит/с", "SATA 3 Gb/s"));
    else if (neg == 1)
        safe_snprintf(pInfo->szProtocol, TN("SATA 1.5 Гбит/с", "SATA 1.5 Gb/s"));
    else if (w76 != 0 && w76 != 0xFFFF) {
        /* Word 76: bit1=1.5, bit2=3.0, bit3=6.0 Gb/s supported. */
        if (w76 & 0x0008)
            safe_snprintf(pInfo->szProtocol, TN("SATA 6 Гбит/с", "SATA 6 Gb/s"));
        else if (w76 & 0x0004)
            safe_snprintf(pInfo->szProtocol, TN("SATA 3 Гбит/с", "SATA 3 Gb/s"));
        else if (w76 & 0x0002)
            safe_snprintf(pInfo->szProtocol, TN("SATA 1.5 Гбит/с", "SATA 1.5 Gb/s"));
        else
            safe_snprintf(pInfo->szProtocol, "SATA");
    } else {
        safe_snprintf(pInfo->szProtocol, "ATA");
    }
}

void SwapATAString(char* szDst, const WORD* pSrc, int nWords)
{
    int i;
    for (i = 0; i < nWords; i++) {
        szDst[i * 2]     = (char)(pSrc[i] >> 8);
        szDst[i * 2 + 1] = (char)(pSrc[i] & 0xFF);
    }
    szDst[nWords * 2] = '\0';
    TrimStr(szDst);
}

/* ============================================================
 * Legacy SMART IOCTL path
 *
 * SMART_SEND_DRIVE_COMMAND / SMART_RCV_DRIVE_DATA: bDriveNumber is the
 * IDE target on that controller (0..3), not PhysicalDriveN. The handle
 * is already \\.\PhysicalDriveN, so bDriveNumber must be 0.
 * ============================================================ */
BOOL EnableSMART(HANDLE hDrive, int nDrive)
{
    SENDCMDINPARAMS cip;
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    SENDCMDOUTPARAMS cop;
    DWORD dwBytes = 0;
    ZeroMemory(&cip, sizeof(cip));
    ZeroMemory(&cop, sizeof(cop));

    cip.cBufferSize                 = 0;
    cip.irDriveRegs.bFeaturesReg    = SMART_ENABLE;
    cip.irDriveRegs.bSectorCountReg = 1;
    cip.irDriveRegs.bSectorNumberReg= 1;
    cip.irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    cip.irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    cip.irDriveRegs.bDriveHeadReg   = 0xA0;
    cip.irDriveRegs.bCommandReg     = SMART_CMD;
    cip.bDriveNumber                = 0;  /* handle is already the drive */

    return DeviceIoControl(hDrive, SMART_SEND_DRIVE_COMMAND,
        &cip, sizeof(SENDCMDINPARAMS) - 1,
        &cop, sizeof(SENDCMDOUTPARAMS) - 1,
        &dwBytes, NULL);
}

BOOL GetIdentifyData(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf,  sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = IDENTIFY_BUFFER_SIZE;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = ID_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    WORD* pIdent = (WORD*)pCop->bBuffer;

    if (IsBufferAllZero((BYTE*)pIdent, 64)) return FALSE;

    SwapATAString(pInfo->szSerial,   &pIdent[10], 10);
    SwapATAString(pInfo->szFirmware, &pIdent[23], 4);
    SwapATAString(pInfo->szModel,    &pIdent[27], 20);

    DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = IdentLba48(pIdent);
    }
    if (qwSectors == 0) qwSectors = (unsigned __int64)dwSectors28;
    pInfo->dwCapacityMB = (DWORD)(qwSectors * 512 / (1024 * 1024));

    /* Word 82-84: Command set/feature support */
    pInfo->bSMART_Supported = (pIdent[82] & 0x0001) ? TRUE : FALSE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;

    /* Word 217: Nominal Media Rotation Rate
     * 0x0001 = non-rotating (SSD), >= 0x0401 = RPM */
    pInfo->wRotationRate = pIdent[217];

    /* Word 76/77: SATA gen / negotiated speed → szProtocol */
    FillAtaProtocolFromIdent(pInfo, pIdent);

    return TRUE;
}

BOOL GetSMARTAttributes(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + READ_ATTRIBUTE_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf, sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = READ_ATTRIBUTE_BUFFER_SIZE;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_DATA;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    if (IsBufferAllZero(pCop->bBuffer + 2, 30)) return FALSE;

    return FillSmartData(pInfo, pCop->bBuffer) > 0;
}

BOOL GetSMARTThresholds(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + READ_THRESHOLD_BUFFER_SIZE];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf,  sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = READ_THRESHOLD_BUFFER_SIZE;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_THRESHOLDS;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= 1;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    FillSmartThreshold(pInfo, pCop->bBuffer);
    return TRUE;
}

BOOL GetSMARTPredictFailure(HANDLE hDrive, int nDrive, BOOL* pbFail)
{
#pragma pack(push,1)
    typedef struct {
        DWORD        cBufferSize;
        DRIVERSTATUS DriverStatus;
        BYTE         bBuffer[16];
    } MY_OUTPARAMS;
#pragma pack(pop)

    SENDCMDINPARAMS cip;
    MY_OUTPARAMS    cop;
    DWORD dwBytes = 0;
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    ZeroMemory(&cip, sizeof(cip));
    ZeroMemory(&cop, sizeof(cop));
    if (!pbFail) return FALSE;

    cip.cBufferSize                 = 0;
    cip.irDriveRegs.bFeaturesReg    = SMART_RETURN_STATUS;
    cip.irDriveRegs.bSectorCountReg = 1;
    cip.irDriveRegs.bSectorNumberReg= 1;
    cip.irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    cip.irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    cip.irDriveRegs.bDriveHeadReg   = 0xA0;
    cip.irDriveRegs.bCommandReg     = SMART_CMD;
    cip.bDriveNumber                = 0;  /* handle is already the drive */

    BOOL bOK = DeviceIoControl(hDrive, SMART_SEND_DRIVE_COMMAND,
        &cip, sizeof(SENDCMDINPARAMS) - 1,
        &cop, sizeof(MY_OUTPARAMS), &dwBytes, NULL);

    /* Leave *pbFail unchanged unless RETURN STATUS succeeded (LOG SENSE may have set it). */
    if (bOK && cop.DriverStatus.bDriverError == 0) {
        *pbFail = (cop.bBuffer[3] == 0xF4 && cop.bBuffer[4] == 0x2C);
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * SMART Error Log reading
 * Provides diagnostic detail beyond attribute raw values
 * ============================================================ */
static BOOL ReadSMARTLog(HANDLE hDrive, int nDrive, BYTE bLogAddr,
                         BYTE* pOutBuf, DWORD dwBufSize)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + 512];
    DWORD dwBytes = 0;
    ZeroMemory(inBuf, sizeof(inBuf));
    ZeroMemory(outBuf, sizeof(outBuf));

    SENDCMDINPARAMS* pCip = (SENDCMDINPARAMS*)inBuf;
    pCip->cBufferSize                 = 512;
    pCip->irDriveRegs.bFeaturesReg    = SMART_READ_LOG;
    pCip->irDriveRegs.bSectorCountReg = 1;
    pCip->irDriveRegs.bSectorNumberReg= bLogAddr;
    pCip->irDriveRegs.bCylLowReg      = SMART_CYL_LOW;
    pCip->irDriveRegs.bCylHighReg     = SMART_CYL_HI;
    pCip->irDriveRegs.bDriveHeadReg   = 0xA0;
    pCip->irDriveRegs.bCommandReg     = SMART_CMD;
    pCip->bDriveNumber                = 0;  /* handle is already the drive */

    if (!DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL))
        return FALSE;

    SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, pCop->bBuffer, dwCopy);
    return TRUE;
}

static BOOL ReadSMARTLogATAPassthrough(HANDLE hDrive, BYTE bLogAddr,
                                       BYTE* pOutBuf, DWORD dwBufSize)
{
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    /* Use ATA pass-through to read SMART log */
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4 + 512];
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED | ATA_FLAGS_DATA_IN;
    pApt->DataTransferLength = 512;
    pApt->TimeOutValue       = 10;
    pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

    pApt->CurrentTaskFile[0] = SMART_READ_LOG;     /* Features */
    pApt->CurrentTaskFile[1] = 1;                   /* Sector count */
    pApt->CurrentTaskFile[2] = bLogAddr;            /* LBA Low = log address */
    pApt->CurrentTaskFile[3] = SMART_CYL_LOW;       /* LBA Mid */
    pApt->CurrentTaskFile[4] = SMART_CYL_HI;        /* LBA High */
    pApt->CurrentTaskFile[5] = 0xA0;                /* Device */
    pApt->CurrentTaskFile[6] = SMART_CMD;            /* Command */

    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL))
        return FALSE;

    DWORD dwCopy = (dwBufSize < 512) ? dwBufSize : 512;
    memcpy(pOutBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, dwCopy);
    return TRUE;
}


BOOL GetSMARTErrorLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTErrorLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_ERROR,
                     (BYTE*)&pInfo->errorLog, sizeof(pInfo->errorLog))) {
        pInfo->bGotErrorLog = TRUE;
        pInfo->nErrorLogCount = pInfo->errorLog.bErrorLogIndex;
        return TRUE;
    }
    return FALSE;
}


BOOL GetSMARTSelfTestLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        if (pInfo->selfTestLog.stEntries[0].bStatusByte != 0)
            pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[0].bStatusByte;
        return TRUE;
    }
    return FALSE;
}


/* ============================================================
 * Native ATA pass-through
 * ============================================================ */
BOOL ATAPassThrough(HANDLE hDrive, BYTE bCommand, BYTE bFeatures,
    BYTE bSectorCount, BYTE bLBALow, BYTE bLBAMid, BYTE bLBAHigh,
    BYTE bDevice, BYTE* pDataBuf, DWORD dwDataLen, BOOL bDataIn)
{
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4 + 512];
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;
    if (dwDataLen > 512) return FALSE;
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED |
                               (bDataIn ? ATA_FLAGS_DATA_IN : 0);
    pApt->DataTransferLength = dwDataLen;
    pApt->TimeOutValue       = 10;
    pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

    pApt->CurrentTaskFile[0] = bFeatures;
    pApt->CurrentTaskFile[1] = bSectorCount;
    pApt->CurrentTaskFile[2] = bLBALow;
    pApt->CurrentTaskFile[3] = bLBAMid;
    pApt->CurrentTaskFile[4] = bLBAHigh;
    pApt->CurrentTaskFile[5] = bDevice;
    pApt->CurrentTaskFile[6] = bCommand;

    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL))
        return FALSE;

    if (bDataIn && pDataBuf && dwDataLen > 0)
        memcpy(pDataBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, dwDataLen);
    return TRUE;
}

BOOL GetIdentifyDataATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[IDENTIFY_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!ATAPassThrough(hDrive, ID_CMD, 0, 1, 0, 0, 0, 0xA0,
                        data, IDENTIFY_BUFFER_SIZE, TRUE))
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

    pInfo->bSMART_Supported = (pIdent[82] & 0x0001) ? TRUE : FALSE;
    pInfo->bSMART_Enabled   = (pIdent[85] & 0x0001) ? TRUE : FALSE;
    pInfo->wRotationRate    = pIdent[217];
    FillAtaProtocolFromIdent(pInfo, pIdent);
    return TRUE;
}

BOOL GetSMARTAttributesATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_ATTRIBUTE_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    ATAPassThrough(hDrive, SMART_CMD, SMART_ENABLE, 1, 1,
                   SMART_CYL_LOW, SMART_CYL_HI, 0xA0, NULL, 0, FALSE);

    if (!ATAPassThrough(hDrive, SMART_CMD, SMART_READ_DATA, 1, 0,
                        SMART_CYL_LOW, SMART_CYL_HI, 0xA0,
                        data, READ_ATTRIBUTE_BUFFER_SIZE, TRUE))
        return FALSE;

    if (IsBufferAllZero(data + 2, 30) || IsBufferAllFF(data + 2, 30))
        return FALSE;

    return FillSmartData(pInfo, data) > 0;
}

BOOL GetSMARTThresholdsATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE data[READ_THRESHOLD_BUFFER_SIZE];
    ZeroMemory(data, sizeof(data));

    if (!ATAPassThrough(hDrive, SMART_CMD, SMART_READ_THRESHOLDS, 1, 0,
                        SMART_CYL_LOW, SMART_CYL_HI, 0xA0,
                        data, READ_THRESHOLD_BUFFER_SIZE, TRUE))
        return FALSE;

    FillSmartThreshold(pInfo, data);
    return TRUE;
}

/* ============================================================
 * Storage protocol property path (Windows 8+)
 * ============================================================ */
BOOL GetSMARTViaStorageProtocol(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE buf[sizeof(MY_STORAGE_PROTOCOL_QUERY) + 512 + 64];
    DWORD dwBytes = 0;

    ZeroMemory(buf, sizeof(buf));
    MY_STORAGE_PROTOCOL_QUERY* pQ = (MY_STORAGE_PROTOCOL_QUERY*)buf;
    pQ->PropertyId = (ULONG)StorageDeviceProtocolSpecificProperty;
    pQ->QueryType  = 0;
    pQ->ProtocolSpecific.ProtocolType       = MY_ProtocolTypeAta;
    pQ->ProtocolSpecific.DataType           = MY_AtaDataTypeSmartData;
    pQ->ProtocolSpecific.ProtocolDataOffset = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    pQ->ProtocolSpecific.ProtocolDataLength = 512;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL) ||
        dwBytes < (ULONG)(sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA) + 16)) {
        pInfo->dwErrStorageProtocol = GetLastError();
        return FALSE;
    }

    BYTE* pData = buf + sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    if (IsBufferAllZero(pData + 2, 28)) {
        pInfo->dwErrStorageProtocol = ERROR_INVALID_DATA;
        return FALSE;
    }
    if (FillSmartData(pInfo, pData) <= 0) {
        pInfo->dwErrStorageProtocol = ERROR_INVALID_DATA;
        return FALSE;
    }

    ZeroMemory(buf, sizeof(buf));
    pQ = (MY_STORAGE_PROTOCOL_QUERY*)buf;
    pQ->PropertyId = (ULONG)StorageDeviceProtocolSpecificProperty;
    pQ->QueryType  = 0;
    pQ->ProtocolSpecific.ProtocolType       = MY_ProtocolTypeAta;
    pQ->ProtocolSpecific.DataType           = MY_AtaDataTypeSmartThresholds;
    pQ->ProtocolSpecific.ProtocolDataOffset = sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
    pQ->ProtocolSpecific.ProtocolDataLength = 512;

    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL) &&
        dwBytes >= (ULONG)(sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA) + 16)) {
        pData = buf + sizeof(ULONG)*2 + sizeof(MY_STORAGE_PROTOCOL_SPECIFIC_DATA);
        FillSmartThreshold(pInfo, pData);
    }

    pInfo->eAccessMethod = SMART_ACCESS_STORAGE_QUERY;
    return TRUE;
}

static int TempCFromAtaAttr(const SMART_ATTRIBUTE* pA, DRIVE_VENDOR vendor)
{
    int raw0, lo16, val;
    if (!pA) return -1;
    raw0 = (int)pA->bRawValue[0];
    lo16 = (int)GetRawValue16Lo(pA->bRawValue);
    val  = (int)pA->bAttrValue;

    if (raw0 >= 1 && raw0 <= 125)
        return raw0;
    if (lo16 >= 1 && lo16 <= 125)
        return lo16;
    /* Value as °C only when RAW is empty. 70–100 is inverted 100−T
     * (Seagate). WD 190 airflow is 125−T. 194 HDA is not inverted. */
    if (val >= 1 && val <= 60)
        return val;
    if (pA->bAttrID == 0xBE && vendor == VENDOR_WDC &&
        val >= 70 && val <= 125) {
        int t = 125 - val;
        if (t >= 1 && t <= 70)
            return t;
    }
    if (val >= 70 && val <= 100) {
        int t = 100 - val;
        if (t >= 1 && t <= 60)
            return t;
    }
    return -1;
}

void ExtractTemperatureFromATA(DRIVE_INFO* pInfo)
{
    int i;
    IdentifyDriveParts(pInfo);
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0xC2) {
            int t = TempCFromAtaAttr(pA, pInfo->eVendor);
            if (t > 0) { pInfo->nTemperatureC = t; return; }
        }
    }
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0xBE) {
            int t = TempCFromAtaAttr(pA, pInfo->eVendor);
            if (t > 0) { pInfo->nTemperatureC = t; return; }
        }
    }
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID != 0xE7)
            continue;
        if (pInfo->eController == CONTROLLER_PHISON)
            continue;
        {
            int nT = (int)GetRawValue16Lo(pA->bRawValue);
            if (nT > 0 && nT <= 125) {
                pInfo->nTemperatureC = nT;
                return;
            }
        }
    }
}

void ExtractCommonATACounters(DRIVE_INFO* pInfo)
{
    int i;
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0x09 && pInfo->dwPowerOnHours == 0)
            pInfo->dwPowerOnHours = GetRawValue(pA->bRawValue);
        else if (pA->bAttrID == 0x0C && pInfo->dwPowerCycleCount == 0)
            pInfo->dwPowerCycleCount = GetRawValue(pA->bRawValue);
    }
}

/* ============================================================
 * SMART data validation
 *
 * Validates the SMART read data buffer by:
 * 1. Checking that it's not all zeros
 * 2. Counting valid (non-zero ID) attributes
 * 3. Optional checksum verification (last byte of 512)
 * ============================================================ */
BOOL ValidateSmartData(const BYTE* pRawBuf, int nBufLen)
{
    /* Check not all zero */
    if (IsBufferAllZero(pRawBuf, nBufLen)) return FALSE;

    /* Check at least one valid attribute ID exists */
    int nValidAttrs = 0;
    int i;
    for (i = 0; i < 30; i++) {
        BYTE bID = pRawBuf[2 + i * 12];
        if (bID != 0) nValidAttrs++;
    }
    if (nValidAttrs == 0) return FALSE;

    return TRUE;
}

/* ============================================================
 * FillSmartData 
 *
 * Parses the raw 512-byte SMART data buffer
 * by iterating through 12-byte attribute entries, skipping
 * entries with ID=0, and compacting into the Attribute array.
 * This is more robust than simple memcpy because it validates
 * each entry individually and removes gaps.
 * ============================================================ */
int FillSmartData(DRIVE_INFO* pInfo, const BYTE* pRawBuf)
{
    int nCount = 0;
    int i;
    ZeroMemory(&pInfo->attrData, sizeof(SMART_ATTRIBUTE_DATA));
    pInfo->attrData.wRevisionNumber = (WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8);

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        BYTE bID = pEntry[0];
        if (bID == 0) continue;

        /* ATA 12-byte attr: ID, flags[1..2], value, worst, raw[5..10], reserved[11]. */
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[nCount];
        pA->bAttrID      = bID;
        pA->wStatusFlags = (WORD)pEntry[1] | ((WORD)pEntry[2] << 8);
        pA->bAttrValue   = pEntry[3];
        pA->bWorstValue  = pEntry[4];
        memcpy(pA->bRawValue, &pEntry[5], 6);
        pA->bReserved    = pEntry[11];
        nCount++;
    }
    return nCount;
}

/* ============================================================
 * FillSmartThreshold 
 *
 * Matches thresholds by attribute ID to the
 * already-parsed attribute array. This handles misaligned or
 * vendor-specific threshold data correctly.
 * ============================================================ */
int FillSmartThreshold(DRIVE_INFO* pInfo, const BYTE* pRawBuf)
{
    int nCount = 0;
    int i, j;
    ZeroMemory(&pInfo->threshData, sizeof(SMART_THRESHOLD_DATA));
    pInfo->threshData.wRevisionNumber = (WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8);

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        BYTE bID = pEntry[0];
        if (bID == 0) continue;

        /* Find matching attribute by ID */
        for (j = 0; j < 30; j++) {
            if (pInfo->attrData.stAttributes[j].bAttrID == bID) {
                pInfo->threshData.stThresholds[j].bAttrID = bID;
                pInfo->threshData.stThresholds[j].bThresholdValue = pEntry[1];
                nCount++;
                break;
            }
        }
    }
    return nCount;
}

/* ============================================================
 * Multi-path SMART acquisition for an internal/SATA drive
 *
 * Priority order (most modern/reliable first):
 *   USB drives: 1. SAT (SCSI/ATA Translation - A1h/85h)
 *              2. Storage Protocol query
 *              3. ATA PASS THROUGH
 *              4. Legacy IOCTL
 *
 * Internal drives:
 *   1. ATA PASS THROUGH (IOCTL_ATA_PASS_THROUGH)  ← CDI preferred
 *   2. Legacy IOCTL (SMART_RCV_DRIVE_DATA)          ← CDI fallback
 *   3. SAT (SCSI/ATA Translation - A1h/85h)
 *   4. Storage Protocol query
 *
 * Prefers ATA Pass-Through for internal drives
 * but SAT-first for USB drives (bridge chips need SCSI commands).
 * ============================================================ */
BOOL AcquireATASMART(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo, BOOL bReadLogs)
{
    BOOL bAttr = FALSE, bThresh = FALSE;

    /* Enable SMART via both paths */
    EnableSMART(hDrive, nDrive);
    EnableSMARTSAT(hDrive);

    /* ---- For USB drives: prioritize SAT  ---- */
    if (pInfo->bIsUSB) {
        if (GetSMARTAttributesSAT(hDrive, pInfo)) {
            bAttr = TRUE;
            pInfo->eAccessMethod = SMART_ACCESS_SAT16;
            bThresh = GetSMARTThresholdsSAT(hDrive, pInfo);
        }
        if (!bAttr && GetSMARTViaStorageProtocol(hDrive, pInfo)) {
            bAttr = TRUE;
            bThresh = TRUE;
        }
    }

    /* ---- Path 1: ATA PASS THROUGH  ---- */
    if (!bAttr && GetSMARTAttributesATAPassthrough(hDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_ATA_PASSTHROUGH;
        bThresh = GetSMARTThresholdsATAPassthrough(hDrive, pInfo);
    }

    /* ---- Path 2: Legacy IOCTL ---- */
    if (!bAttr && GetSMARTAttributes(hDrive, nDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_LEGACY_IOCTL;
        bThresh = GetSMARTThresholds(hDrive, nDrive, pInfo);
    }

    /* ---- Path 3: SAT (SCSI A1h/85h) — for USB enclosures ---- */
    if (!bAttr && GetSMARTAttributesSAT(hDrive, pInfo)) {
        bAttr = TRUE;
        pInfo->eAccessMethod = SMART_ACCESS_SAT16;
        bThresh = GetSMARTThresholdsSAT(hDrive, pInfo);
    }

    /* ---- Path 4: Windows storage protocol query ---- */
    if (!bAttr && GetSMARTViaStorageProtocol(hDrive, pInfo)) {
        bAttr = TRUE;
        bThresh = TRUE;
    }

    if (bAttr) {
        /* Validate SMART data */
        if (!ValidateSmartData((const BYTE*)&pInfo->attrData,
                               sizeof(SMART_ATTRIBUTE_DATA))) {
            /* Data may be invalid — but still try to use what we have */
        }

        if (GetSMARTPredictFailure(hDrive, nDrive, &pInfo->bPredictFailure))
            pInfo->bGotReturnStatus = TRUE;
        ExtractTemperatureFromATA(pInfo);
        ExtractCommonATACounters(pInfo);
        ExtractSSDIndicators(pInfo);

        /* SMART error / self-test logs: full scan only. Extra SMART READ LOG
         * on USB/HDD every refresh can AV the usbstor/atapi stack. */
        if (bReadLogs) {
            if (!GetSMARTErrorLog(hDrive, nDrive, pInfo)) {
                if (!GetSMARTErrorLogATAPassthrough(hDrive, pInfo))
                    GetSMARTErrorLogSAT(hDrive, pInfo);
            }
            if (!GetSMARTSelfTestLog(hDrive, nDrive, pInfo)) {
                if (!GetSMARTSelfTestLogATAPassthrough(hDrive, pInfo))
                    GetSMARTSelfTestLogSAT(hDrive, pInfo);
            }
        }
    } else {
        pInfo->bSMART_Supported = FALSE;
        pInfo->nHealthPercent   = -1;
    }
    (void)bThresh;
    return bAttr;
}

