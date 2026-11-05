/* DriveMonitor - ATA/SATA SMART (IOCTL_ATA_PASS_THROUGH). GPL-2.0-or-later. See LICENSE. */
#include "smart_internal.h"
#include "csmi.h"

void FillAtaProtocolFromIdent(DRIVE_INFO* pInfo, const WORD* pIdent)
{
    WORD w76, w77;
    unsigned neg;
    BYTE maxGen = 0;
    if (!pInfo || !pIdent) return;
    w76 = pIdent[76];
    w77 = pIdent[77];

    /* Word 76: bit1=1.5, bit2=3.0, bit3=6.0 Gb/s supported. */
    if (w76 != 0 && w76 != 0xFFFF) {
        if (w76 & 0x0008)
            maxGen = 3;
        else if (w76 & 0x0004)
            maxGen = 2;
        else if (w76 & 0x0002)
            maxGen = 1;
    }
    pInfo->nSataMaxGen = maxGen;

    /* USB: word 77 is the dongle's negotiated speed, not the disk.
     * Use word 76 (supported) for the disk, prefix USB later. */
    neg = pInfo->bIsUSB ? 0 : (unsigned)((w77 >> 1) & 0x7);
    if (!pInfo->bIsUSB && neg >= 1 && neg <= 3) {
        pInfo->nSataCurGen = (BYTE)neg;
        if (maxGen > (BYTE)neg) {
            pInfo->bLinkDegraded = TRUE;
            const char* szCurStr = (neg == 2) ? TN("3 Гбит/с", "3 Gb/s") : TN("1.5 Гбит/с", "1.5 Gb/s");
            const char* szMaxStr = (maxGen == 3) ? TN("6 Гбит/с", "6 Gb/s") : TN("3 Гбит/с", "3 Gb/s");
            safe_snprintf(pInfo->szProtocol, "SATA %s (%s %s \xE2\x9A\xA0)",
                          szCurStr, TN("макс.", "max"), szMaxStr);
            return;
        }
    }

    if (neg == 3)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 6 Гбит/с", "SATA 6 Gb/s"));
    else if (neg == 2)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 3 Гбит/с", "SATA 3 Gb/s"));
    else if (neg == 1)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 1.5 Гбит/с", "SATA 1.5 Gb/s"));
    else if (maxGen == 3)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 6 Гбит/с", "SATA 6 Gb/s"));
    else if (maxGen == 2)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 3 Гбит/с", "SATA 3 Gb/s"));
    else if (maxGen == 1)
        safe_snprintf(pInfo->szProtocol, "%s", TN("SATA 1.5 Гбит/с", "SATA 1.5 Gb/s"));
    else if (w76 != 0 && w76 != 0xFFFF)
        safe_snprintf(pInfo->szProtocol, "SATA");
    else
        safe_snprintf(pInfo->szProtocol, "ATA");
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
    (void)nDrive;
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

BOOL ValidateAtaIdentifyData(const BYTE* pIdent512)
{
    if (!pIdent512) return FALSE;
    if (IsBufferAllZero(pIdent512, 64)) return FALSE;

    /* Reject all-0xFF buffer */
    BOOL bAllFF = TRUE;
    for (int i = 0; i < 64; i++) {
        if (pIdent512[i] != 0xFF) {
            bAllFF = FALSE;
            break;
        }
    }
    if (bAllFF) return FALSE;

    const WORD* pIdent = (const WORD*)pIdent512;

    /* Word 0: Bit 15 must be 0 for standard ATA HDD / SSD.
     * Bit 15 == 1 indicates ATAPI (optical/tape drive) or invalid/garbage RAM dump. */
    if ((pIdent[0] & 0x8000) != 0) return FALSE;

    /* Unpack and validate Model string (Words 27-46 = 40 bytes) */
    char szModelTemp[48];
    SwapATAString(szModelTemp, &pIdent[27], 20);

    /* Trim leading and trailing spaces */
    char* pStart = szModelTemp;
    while (*pStart == ' ') pStart++;
    char* pEnd = pStart + strlen(pStart);
    while (pEnd > pStart && (*(pEnd - 1) == ' ' || *(pEnd - 1) == '\r' || *(pEnd - 1) == '\n')) {
        pEnd--;
    }
    *pEnd = '\0';

    size_t nLen = strlen(pStart);
    if (nLen < 2) return FALSE;

    /* Model must consist strictly of printable ASCII characters (0x20..0x7E) */
    for (size_t i = 0; i < nLen; i++) {
        unsigned char c = (unsigned char)pStart[i];
        if (c < 0x20 || c > 0x7E) return FALSE;
    }

    /* Plausible capacity check: reject ridiculous sector counts > 100 PB (200 billion sectors) */
    unsigned __int64 qwSectors = 0;
    if (pIdent[83] & 0x0400) {
        qwSectors = IdentLba48(pIdent);
    }
    if (qwSectors == 0) {
        DWORD dwSectors28 = ((DWORD)pIdent[61] << 16) | pIdent[60];
        qwSectors = (unsigned __int64)dwSectors28;
    }
    if (qwSectors > 200000000000ULL) return FALSE;

    return TRUE;
}

BOOL ParseAtaIdentifyData(const BYTE* pIdent512, DRIVE_INFO* pInfo)
{
    if (!pIdent512 || !pInfo) return FALSE;
    if (!ValidateAtaIdentifyData(pIdent512)) return FALSE;

    const WORD* pIdent = (const WORD*)pIdent512;
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
    pInfo->bSelfTestSupported = ((pIdent[84] & 0x0002) || (pIdent[87] & 0x0002)) ? TRUE : FALSE;

    /* Word 217: Nominal Media Rotation Rate
     * 0x0001 = non-rotating (SSD), >= 0x0401 = RPM */
    pInfo->wRotationRate = pIdent[217];

    /* Word 76/77: SATA gen / negotiated speed -> szProtocol */
    pInfo->bCapAtaIdentify = TRUE;
    FillAtaProtocolFromIdent(pInfo, pIdent);

    return (pInfo->szModel[0] != '\0');
}

BOOL GetIdentifyData(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
    (void)nDrive;
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
    return ParseAtaIdentifyData((const BYTE*)pCop->bBuffer, pInfo);
}

BOOL GetSMARTAttributes(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    (void)nDrive;
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
    return FillSmartData(pInfo, pCop->bBuffer) > 0;
}

BOOL GetSMARTThresholds(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
    (void)nDrive;
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
    (void)nDrive;
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
    (void)nDrive;
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

BOOL ReadAtaLogExtNative(HANDLE hDrive, BYTE bLogAddr, WORD wPage,
                         void* pBuf, DWORD dwBufSize)
{
    if (!hDrive || !pBuf || dwBufSize < 512) return FALSE;
    if (!DriveAllowsAtaIoctl(hDrive)) return FALSE;

    /* 1. Try ATA PASS-THROUGH with 0x2F (READ LOG EXT) */
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4 + 512];
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED | ATA_FLAGS_DATA_IN | ATA_FLAGS_48BIT_COMMAND;
    pApt->DataTransferLength = 512;
    pApt->TimeOutValue       = 10;
    pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

    pApt->CurrentTaskFile[0]  = (BYTE)(wPage & 0xFF);        /* Features: page low */
    pApt->PreviousTaskFile[0] = (BYTE)((wPage >> 8) & 0xFF); /* Features: page high */
    pApt->CurrentTaskFile[1]  = 1;                           /* Sector count: 1 */
    pApt->PreviousTaskFile[1] = 0;
    pApt->CurrentTaskFile[2]  = bLogAddr;                    /* LBA Low: Log Address */
    pApt->PreviousTaskFile[2] = 0;
    pApt->CurrentTaskFile[3]  = 0;                           /* LBA Mid */
    pApt->PreviousTaskFile[3] = 0;
    pApt->CurrentTaskFile[4]  = 0;                           /* LBA High */
    pApt->PreviousTaskFile[4] = 0;
    pApt->CurrentTaskFile[5]  = 0x40;                        /* Device (LBA mode) */
    pApt->CurrentTaskFile[6]  = 0x2F;                        /* Command: READ LOG EXT */

    DWORD dwBytes = 0;
    if (DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL)) {
        memcpy(pBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, 512);
        return TRUE;
    }

    /* 2. Fallback to SMART READ LOG (0xB0 / 0xD5) for page 0 */
    if (wPage == 0) {
        ZeroMemory(buf, sizeof(buf));
        pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
        pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED | ATA_FLAGS_DATA_IN;
        pApt->DataTransferLength = 512;
        pApt->TimeOutValue       = 10;
        pApt->DataBufferOffset   = sizeof(MY_ATA_PASS_THROUGH_EX) + 4;

        pApt->CurrentTaskFile[0] = SMART_READ_LOG;
        pApt->CurrentTaskFile[1] = 1;
        pApt->CurrentTaskFile[2] = bLogAddr;
        pApt->CurrentTaskFile[3] = SMART_CYL_LOW;
        pApt->CurrentTaskFile[4] = SMART_CYL_HI;
        pApt->CurrentTaskFile[5] = 0xA0;
        pApt->CurrentTaskFile[6] = SMART_CMD;

        if (DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
                buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL)) {
            memcpy(pBuf, buf + sizeof(MY_ATA_PASS_THROUGH_EX) + 4, 512);
            return TRUE;
        }

        /* 3. Fallback to SMART_RCV_DRIVE_DATA */
        BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1];
        BYTE outBuf[sizeof(SENDCMDOUTPARAMS) - 1 + 512];
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
        pCip->bDriveNumber                = 0;

        if (DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
                pCip, sizeof(SENDCMDINPARAMS) - 1,
                outBuf, sizeof(outBuf), &dwBytes, NULL)) {
            SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
            memcpy(pBuf, pCop->bBuffer, 512);
            return TRUE;
        }
    }
    return FALSE;
}

BOOL GetSMARTErrorLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    BYTE page[512];
    ZeroMemory(page, sizeof(page));
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_ERROR, page, 512)) {
        memcpy(&pInfo->errorLog, page, sizeof(pInfo->errorLog));
        ApplyAtaErrorLog(pInfo, page, 512);
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTErrorLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    BYTE page[512];
    ZeroMemory(page, sizeof(page));
    ZeroMemory(&pInfo->errorLog, sizeof(pInfo->errorLog));

    if (ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_ERROR, page, 512)) {
        memcpy(&pInfo->errorLog, page, sizeof(pInfo->errorLog));
        ApplyAtaErrorLog(pInfo, page, 512);
        return TRUE;
    }
    return FALSE;
}


BOOL GetSMARTSelfTestLog(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadAtaLogExtNative(hDrive, SMART_LOG_COMP_SELF_TEST, 0,
                            (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog)) ||
        ReadSMARTLog(hDrive, nDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog)) ||
        ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        int idx = (pInfo->selfTestLog.bMostRecentTestIndex >= 1 && pInfo->selfTestLog.bMostRecentTestIndex <= 21)
                  ? (pInfo->selfTestLog.bMostRecentTestIndex - 1) : 0;
        pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[idx].bStatusByte;
        return TRUE;
    }
    return FALSE;
}

BOOL GetSMARTSelfTestLogATAPassthrough(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    ZeroMemory(&pInfo->selfTestLog, sizeof(pInfo->selfTestLog));

    if (ReadAtaLogExtNative(hDrive, SMART_LOG_COMP_SELF_TEST, 0,
                            (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog)) ||
        ReadSMARTLogATAPassthrough(hDrive, SMART_LOG_COMP_SELF_TEST,
                     (BYTE*)&pInfo->selfTestLog, sizeof(pInfo->selfTestLog))) {
        pInfo->bGotSelfTestLog = TRUE;
        int idx = (pInfo->selfTestLog.bMostRecentTestIndex >= 1 && pInfo->selfTestLog.bMostRecentTestIndex <= 21)
                  ? (pInfo->selfTestLog.bMostRecentTestIndex - 1) : 0;
        pInfo->nSelfTestStatus = pInfo->selfTestLog.stEntries[idx].bStatusByte;
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

    return ParseAtaIdentifyData(data, pInfo);
}

/* ATA CHECK POWER MODE (0xE5).
 * Non-data command: asks drive controller for power state without spinning up the spindle.
 * Output in Sector Count register (CurrentTaskFile[1]):
 *   0x00: Standby mode (spindle stopped)
 *   0x40: NV Cache Standby (spindle stopped)
 *   0x41: NV Cache Active (spinning)
 *   0x80: Idle mode (spinning)
 *   0xFF: Active or Idle mode (spinning)
 * Returns sector count byte on success, or -1 on error/unsupported. */
int AtaCheckPowerMode(HANDLE hDrive)
{
    BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4];
    if (!DriveAllowsAtaIoctl(hDrive)) return -1;
    ZeroMemory(buf, sizeof(buf));

    MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
    pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
    pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED;
    pApt->DataTransferLength = 0;
    pApt->TimeOutValue       = 5;
    pApt->DataBufferOffset   = 0;

    pApt->CurrentTaskFile[0] = 0;     /* Features */
    pApt->CurrentTaskFile[1] = 0;     /* Sector count */
    pApt->CurrentTaskFile[2] = 0;     /* LBA Low */
    pApt->CurrentTaskFile[3] = 0;     /* LBA Mid */
    pApt->CurrentTaskFile[4] = 0;     /* LBA High */
    pApt->CurrentTaskFile[5] = 0xA0;  /* Device */
    pApt->CurrentTaskFile[6] = 0xE5;  /* Command: CHECK POWER MODE */

    DWORD dwBytes = 0;
    if (!DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
            buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL))
        return -1;

    /* Status register bit 0 is ERR (error) */
    if (pApt->CurrentTaskFile[6] & 0x01)
        return -1;

    return (int)(pApt->CurrentTaskFile[1]);
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

static int TempAbsDiff(int a, int b)
{
    int d = a - b;
    return d < 0 ? -d : d;
}

/* Airflow 190 (and a 194 that uses the same scale): Value ≈ scale − °C. */
static int AtaTempInvertedScale(const SMART_ATTRIBUTE* a)
{
    int cur, val;
    if (!a) return 0;
    cur = (int)a->bRawValue[0];
    if (cur < 1 || cur > 125) return 0;
    val = (int)a->bAttrValue;
    if (TempAbsDiff(val, 100 - cur) <= 1) return 100;
    if (TempAbsDiff(val, 125 - cur) <= 1) return 125;
    return 0;
}

int AtaTempLifetimeMaxC(const SMART_ATTRIBUTE* a)
{
    int cur, best, worst, scale, peak, rawMax;
    if (!a) return -1;
    if (a->bAttrID != 0xBE && a->bAttrID != 0xC2) return -1;
    cur = (int)a->bRawValue[0];
    if (cur < 1 || cur > 125) return -1;
    best = -1;
    rawMax = (int)a->bRawValue[4];
    if (rawMax >= 1 && rawMax <= 125 && rawMax >= cur)
        best = rawMax;
    worst = (int)a->bWorstValue;
    scale = AtaTempInvertedScale(a);
    if (scale > 0 && worst >= 1 && worst <= 125 && worst < (int)a->bAttrValue) {
        peak = scale - worst;
        if (peak >= 1 && peak <= 125 && peak >= cur && peak > best)
            best = peak;
    } else if (scale == 0 && a->bAttrID == 0xC2 &&
               TempAbsDiff((int)a->bAttrValue, cur) <= 1 &&
               worst >= 1 && worst <= 125 && worst >= cur && worst > best) {
        /* Seagate 194 stores the current °C in Value and the lifetime max in Worst.
         * WD 194 values such as 129 are not Celsius and stay out. */
        best = worst;
    }
    return best;
}

BOOL AtaTempPeakTripped(const SMART_ATTRIBUTE* a, BYTE thresh)
{
    int scale, peak, trip;
    if (!a || thresh == 0) return FALSE;
    scale = AtaTempInvertedScale(a);
    if (scale == 0 || (int)thresh >= scale) return FALSE;
    peak = AtaTempLifetimeMaxC(a);
    if (peak < 1) return FALSE;
    trip = scale - (int)thresh;
    return trip >= 1 && peak >= trip;
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
 * pRawBuf is the raw 512-byte SMART READ DATA sector, before
 * FillSmartData compacts it. The compacted attribute struct is
 * the wrong buffer: its last byte is zeroed padding.
 *
 * Reject a short, empty, 0xFF, or torn sector, and a checksum
 * byte that is present but does not match. Byte 511 == 0 means
 * the drive or bridge left the checksum unimplemented (typical
 * of USB SAT). That sector stays valid when the table is intact.
 * ============================================================ */
BOOL ValidateSmartData(const BYTE* pRawBuf, int nBufLen)
{
    int nValidAttrs = 0;
    int i, j;
    unsigned sum;

    if (!pRawBuf || nBufLen < 512) return FALSE;
    if (IsBufferAllZero(pRawBuf, 512)) return FALSE;
    if (IsBufferAllFF(pRawBuf + 2, 30 * 12)) return FALSE;

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        if (pEntry[0] == 0) {
            /* Unused slots are all zeros. A payload behind ID 0 is torn. */
            for (j = 1; j < 12; j++) {
                if (pEntry[j] != 0) return FALSE;
            }
            continue;
        }
        nValidAttrs++;
    }
    if (nValidAttrs == 0) return FALSE;

    if (pRawBuf[511] != 0) {
        sum = 0;
        for (i = 0; i < 512; i++) sum += pRawBuf[i];
        if ((sum & 0xFF) != 0) return FALSE;
    }
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
    if (!pInfo) return 0;
    if (!ValidateSmartData(pRawBuf, READ_ATTRIBUTE_BUFFER_SIZE)) {
        ZeroMemory(&pInfo->attrData, sizeof(pInfo->attrData));
        pInfo->bCapSmartData = FALSE;
        pInfo->bSmartDataInvalid = TRUE;
        return 0;
    }
    pInfo->bSmartDataInvalid = FALSE;
    ZeroMemory(&pInfo->attrData, sizeof(SMART_ATTRIBUTE_DATA));
    pInfo->attrData.wRevisionNumber = (WORD)((WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8));

    for (i = 0; i < 30; i++) {
        const BYTE* pEntry = pRawBuf + 2 + i * 12;
        BYTE bID = pEntry[0];
        if (bID == 0) continue;

        /* ATA 12-byte attr: ID, flags[1..2], value, worst, raw[5..10], reserved[11]. */
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[nCount];
        pA->bAttrID      = bID;
        pA->wStatusFlags = (WORD)((WORD)pEntry[1] | ((WORD)pEntry[2] << 8));
        pA->bAttrValue   = pEntry[3];
        pA->bWorstValue  = pEntry[4];
        memcpy(pA->bRawValue, &pEntry[5], 6);
        pA->bReserved    = pEntry[11];
        nCount++;
    }
    if (nCount > 0)
        pInfo->bCapSmartData = TRUE;

    /* Offset 363: Self-test execution status byte */
    pInfo->nSelfTestStatus = pRawBuf[363];
    int stCode = (pRawBuf[363] >> 4) & 0x0F;
    if (stCode == 15) {
        pInfo->bSelfTestRunning = TRUE;
        pInfo->nSelfTestPercentRemaining = (pRawBuf[363] & 0x0F) * 10;
    } else {
        pInfo->bSelfTestRunning = FALSE;
        pInfo->nSelfTestPercentRemaining = 0;
    }
    pInfo->nShortSelfTestMinutes = pRawBuf[372];
    if (pRawBuf[373] != 255) {
        pInfo->nExtSelfTestMinutes = pRawBuf[373];
    } else {
        pInfo->nExtSelfTestMinutes = (int)((WORD)pRawBuf[375] | ((WORD)pRawBuf[376] << 8));
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
    pInfo->threshData.wRevisionNumber = (WORD)((WORD)pRawBuf[0] | ((WORD)pRawBuf[1] << 8));

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
    if (nCount > 0)
        pInfo->bCapSmartThresh = TRUE;
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
        /* FillSmartData already accepted the raw sector. */
        if (GetSMARTPredictFailure(hDrive, nDrive, &pInfo->bPredictFailure))
            pInfo->bGotReturnStatus = TRUE;
        else if (GetSMARTReturnStatusSAT(hDrive, &pInfo->bPredictFailure))
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
            AcquireAtaLogs(hDrive, FALSE, pInfo);
        }
    } else {
        /* No accepted attribute sector. A rejected packet keeps
         * bSmartDataInvalid so health is not scored from it. An IOCTL
         * failure leaves the flag clear: SMART was not returned. */
        pInfo->bSMART_Supported = FALSE;
        pInfo->nHealthPercent   = -1;
        if (pInfo->bSmartDataInvalid) {
            ZeroMemory(&pInfo->threshData, sizeof(pInfo->threshData));
            pInfo->bCapSmartThresh = FALSE;
        }
    }
    (void)bThresh;
    return bAttr;
}

/* ============================================================
 * SMART EXECUTE OFF-LINE IMMEDIATE (Command 0xB0, Features 0xD4)
 * Subcommands (in LBA Low):
 *   0x01: Short Self-test routine
 *   0x02: Extended Self-test routine
 *   0x03: Conveyance Self-test routine
 *   0x00: Abort Self-test routine
 * ============================================================ */
BOOL AtaSendSmartOfflineImmediate(HANDLE hDrive, const DRIVE_INFO* pInfo, BYTE bSubcommand)
{
    if (hDrive == INVALID_HANDLE_VALUE || !hDrive) return FALSE;

    /* 1. If CSMI RAID member */
    if (pInfo && pInfo->eAccessMethod == SMART_ACCESS_CSMI) {
        SCSI_ADDRESS addr;
        HANDLE hScsi = OpenScsiAdapterFromDrive(hDrive, &addr);
        if (hScsi != INVALID_HANDLE_VALUE) {
            BYTE bPhy = (BYTE)addr.TargetId;
            BOOL ok = CsmiSendStpCommand(hScsi, bPhy, SMART_EXECUTE_OFFLINE, 0, bSubcommand, SMART_CYL_LOW, SMART_CYL_HI, SMART_CMD, NULL, 0, FALSE);
            CloseHandle(hScsi);
            return ok;
        }
    }

    /* 2. ATA Pass-Through */
    if (DriveAllowsAtaIoctl(hDrive)) {
        BYTE buf[sizeof(MY_ATA_PASS_THROUGH_EX) + 4];
        ZeroMemory(buf, sizeof(buf));
        MY_ATA_PASS_THROUGH_EX* pApt = (MY_ATA_PASS_THROUGH_EX*)buf;
        pApt->Length             = sizeof(MY_ATA_PASS_THROUGH_EX);
        pApt->AtaFlags           = ATA_FLAGS_DRDY_REQUIRED;
        pApt->DataTransferLength = 0;
        pApt->TimeOutValue       = 10;
        pApt->DataBufferOffset   = 0;

        pApt->CurrentTaskFile[0] = SMART_EXECUTE_OFFLINE; /* Features: 0xD4 */
        pApt->CurrentTaskFile[1] = 0;                     /* Sector count */
        pApt->CurrentTaskFile[2] = bSubcommand;          /* LBA Low */
        pApt->CurrentTaskFile[3] = SMART_CYL_LOW;        /* 0x4F */
        pApt->CurrentTaskFile[4] = SMART_CYL_HI;         /* 0xC2 */
        pApt->CurrentTaskFile[5] = 0xA0;                 /* Device */
        pApt->CurrentTaskFile[6] = SMART_CMD;            /* 0xB0 */

        DWORD dwBytes = 0;
        if (DeviceIoControl(hDrive, IOCTL_ATA_PASS_THROUGH,
                buf, sizeof(buf), buf, sizeof(buf), &dwBytes, NULL)) {
            return TRUE;
        }
    }

    /* 3. SAT16 Pass-Through (SCSI 85h) */
    if (DriveAllowsScsiPassthrough(hDrive)) {
        BYTE cdb[16];
        ZeroMemory(cdb, sizeof(cdb));
        cdb[0]  = 0x85;        /* ATA PASS-THROUGH (16) */
        cdb[1]  = (3 << 1);    /* Non-data protocol */
        cdb[4]  = SMART_EXECUTE_OFFLINE; /* 0xD4 */
        cdb[8]  = bSubcommand; /* LBA Low */
        cdb[10] = SMART_CYL_LOW; /* 0x4F */
        cdb[12] = SMART_CYL_HI;  /* 0xC2 */
        cdb[14] = SMART_CMD;     /* 0xB0 */
        if (ScsiSendGenericCdb(hDrive, cdb, 16, NULL, 0, FALSE)) {
            return TRUE;
        }
    }

    /* 4. Legacy SMART IOCTL: SMART_SEND_DRIVE_COMMAND */
    SENDCMDINPARAMS in;
    BYTE outBuf[sizeof(SENDCMDOUTPARAMS) + 16];
    ZeroMemory(&in, sizeof(in));
    ZeroMemory(outBuf, sizeof(outBuf));
    in.cBufferSize = 0;
    in.bDriveNumber = (BYTE)(pInfo ? pInfo->nDriveIndex : 0);
    in.irDriveRegs.bFeaturesReg     = SMART_EXECUTE_OFFLINE;
    in.irDriveRegs.bSectorNumberReg = bSubcommand;
    in.irDriveRegs.bCylLowReg       = SMART_CYL_LOW;
    in.irDriveRegs.bCylHighReg      = SMART_CYL_HI;
    in.irDriveRegs.bDriveHeadReg    = 0xA0;
    in.irDriveRegs.bCommandReg      = SMART_CMD;

    DWORD dwBytes = 0;
    if (DeviceIoControl(hDrive, SMART_SEND_DRIVE_COMMAND, &in, sizeof(in), outBuf, sizeof(outBuf), &dwBytes, NULL)) {
        return TRUE;
    }

    /* 5. Legacy USB Bridges (Cypress, Sunplus, Prolific) */
    if (pInfo && pInfo->bIsUSB) {
        if (pInfo->eAccessMethod == SMART_ACCESS_CYPRESS) {
            BYTE cdb[16];
            ZeroMemory(cdb, sizeof(cdb));
            cdb[0] = 0x24; cdb[1] = 0x24; cdb[3] = 0xFF;
            cdb[4] = SMART_EXECUTE_OFFLINE;
            cdb[6] = bSubcommand;
            cdb[7] = SMART_CYL_LOW;
            cdb[8] = SMART_CYL_HI;
            cdb[9] = 0xA0;
            cdb[10] = SMART_CMD;
            return ScsiSendGenericCdb(hDrive, cdb, 16, NULL, 0, FALSE);
        } else if (pInfo->eAccessMethod == SMART_ACCESS_SUNPLUS) {
            BYTE cdb[12];
            ZeroMemory(cdb, sizeof(cdb));
            cdb[0] = 0xF8; cdb[1] = 0x22;
            cdb[3] = SMART_EXECUTE_OFFLINE;
            cdb[5] = bSubcommand;
            cdb[6] = SMART_CYL_LOW;
            cdb[7] = SMART_CYL_HI;
            cdb[8] = 0xA0;
            cdb[9] = SMART_CMD;
            return ScsiSendGenericCdb(hDrive, cdb, 12, NULL, 0, FALSE);
        } else if (pInfo->eAccessMethod == SMART_ACCESS_PROLIFIC) {
            BYTE cdb[16];
            ZeroMemory(cdb, sizeof(cdb));
            cdb[0] = 0xD8; cdb[1] = 0x06; cdb[2] = 0x7B;
            cdb[5] = SMART_EXECUTE_OFFLINE;
            cdb[7] = bSubcommand;
            cdb[8] = SMART_CYL_LOW;
            cdb[9] = SMART_CYL_HI;
            cdb[10] = 0xA0;
            cdb[11] = SMART_CMD;
            return ScsiSendGenericCdb(hDrive, cdb, 16, NULL, 0, FALSE);
        }
    }

    return FALSE;
}
