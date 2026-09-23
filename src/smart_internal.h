/* DriveMonitor - SMART internal types and cross-module API. MIT: see LICENSE. */
#pragma once
#ifndef SMART_INTERNAL_H
#define SMART_INTERNAL_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <limits.h>
#include <stdarg.h>
#include <setupapi.h>
#include <devguid.h>
#include <cfgmgr32.h>
#include "smart.h"
#include "safestr.h"
#include "lang.h"

#ifndef CR_SUCCESS
#define CR_SUCCESS 0
#endif

#ifndef StorageDeviceProtocolSpecificProperty
#define StorageDeviceProtocolSpecificProperty ((STORAGE_PROPERTY_ID)49)
#endif
#ifndef StorageAdapterProtocolSpecificProperty
#define StorageAdapterProtocolSpecificProperty ((STORAGE_PROPERTY_ID)50)
#endif

#ifndef ATA_FLAGS_DRDY_REQUIRED
#define ATA_FLAGS_DRDY_REQUIRED  (1 << 0)
#define ATA_FLAGS_DATA_IN        (1 << 1)
#define ATA_FLAGS_DATA_OUT       (1 << 2)
#define ATA_FLAGS_48BIT_COMMAND  (1 << 3)
#define ATA_FLAGS_USE_DMA        (1 << 4)
#define ATA_FLAGS_NO_MULTIPLE    (1 << 5)
#endif

#define SAT_SENSEBUF_OFFSET  (sizeof(SCSI_PASS_THROUGH_DIRECT) + sizeof(ULONG))

/* Windows STORAGE_PROTOCOL_TYPE / NVMe DataType. Enum so the compiler
 * sees a single typed set of names instead of scattered #defines. */
enum {
    MY_ProtocolTypeUnknown = 0,
    MY_ProtocolTypeScsi    = 1,
    MY_ProtocolTypeAta     = 2,
    MY_ProtocolTypeNvme    = 3,
    MY_ProtocolTypeSd      = 4,
    MY_ProtocolTypeUfs     = 5
};

enum {
    MY_NVMeDataTypeUnknown  = 0,
    MY_NVMeDataTypeIdentify = 1,
    MY_NVMeDataTypeLogPage  = 2,
    MY_NVMeDataTypeFeature  = 3
};

enum {
    MY_AtaDataTypeUnknown         = 0,
    MY_AtaDataTypeIdentify        = 1,
    MY_AtaDataTypeSmartData       = 2,
    MY_AtaDataTypeSmartThresholds = 3
};

enum {
    NVME_LOG_PAGE_ERROR_INFO         = 0x01,
    NVME_LOG_PAGE_HEALTH_INFO        = 0x02,
    NVME_LOG_PAGE_FIRMWARE_SLOT_INFO = 0x03
};

/* NVMe Identify Controller spec offsets. Named NVME_IDENTIFY_CONTROLLER
 * fields after byte 75 are misaligned (OACS is spec 256, WCTEMP 266) —
 * always read the raw Identify copy. */
enum NvmeIdentOff {
    NVME_IDENT_VER_OFF    = 80,
    NVME_IDENT_WCTEMP_OFF = 266,
    NVME_IDENT_CCTEMP_OFF = 268
};

#pragma pack(push, 1)

typedef struct _SAT_PASSTHROUGH_BUF {
    SCSI_PASS_THROUGH_DIRECT sptd;
    ULONG    Filler;
    BYTE     SenseBuf[32];
} SAT_PASSTHROUGH_BUF;

typedef struct _CDI_SAT_PASSTHROUGH_BUF {
    SCSI_PASS_THROUGH spt;
    ULONG  Filler;
    BYTE   SenseBuf[32];
    BYTE   DataBuf[512];
} CDI_SAT_PASSTHROUGH_BUF;

typedef struct _CDI_SAT_PASSTHROUGH_BUF_4K {
    SCSI_PASS_THROUGH spt;
    ULONG  Filler;
    BYTE   SenseBuf[32];
    BYTE   DataBuf[4096];
} CDI_SAT_PASSTHROUGH_BUF_4K;

typedef struct _MY_ATA_PASS_THROUGH_EX {
    USHORT    Length;
    USHORT    AtaFlags;
    UCHAR     PathId;
    UCHAR     TargetId;
    UCHAR     Lun;
    UCHAR     ReservedAsUchar;
    ULONG     DataTransferLength;
    ULONG     TimeOutValue;
    ULONG     ReservedAsUlong;
    ULONG_PTR DataBufferOffset;
    UCHAR     PreviousTaskFile[8];
    UCHAR     CurrentTaskFile[8];
} MY_ATA_PASS_THROUGH_EX;

typedef struct _MY_ATA_PASS_THROUGH_BUF {
    MY_ATA_PASS_THROUGH_EX apt;
    ULONG  Filler;
    BYTE   DataBuf[512];
} MY_ATA_PASS_THROUGH_BUF;

typedef struct _MY_STORAGE_PROTOCOL_SPECIFIC_DATA {
    ULONG ProtocolType;
    ULONG DataType;
    ULONG ProtocolDataRequestValue;
    ULONG ProtocolDataRequestSubValue;
    ULONG ProtocolDataOffset;
    ULONG ProtocolDataLength;
    ULONG FixedProtocolReturnData;
    ULONG Reserved[3];
} MY_STORAGE_PROTOCOL_SPECIFIC_DATA;

typedef struct _MY_STORAGE_PROTOCOL_QUERY {
    ULONG PropertyId;
    ULONG QueryType;
    MY_STORAGE_PROTOCOL_SPECIFIC_DATA ProtocolSpecific;
} MY_STORAGE_PROTOCOL_QUERY;

typedef struct _CDI_NVME_QUERY_BUF {
    ULONG PropertyId;
    ULONG QueryType;
    MY_STORAGE_PROTOCOL_SPECIFIC_DATA ProtocolSpecific;
    BYTE  Buffer[4096];
} CDI_NVME_QUERY_BUF;

#pragma pack(pop)

static inline BOOL IsBufferAllZero(const BYTE* p, int nLen)
{
    int i;
    if (!p || nLen <= 0) return TRUE;
    for (i = 0; i < nLen; i++)
        if (p[i] != 0x00) return FALSE;
    return TRUE;
}

static inline BOOL IsBufferAllFF(const BYTE* p, int nLen)
{
    int i;
    if (!p || nLen <= 0) return TRUE;
    for (i = 0; i < nLen; i++)
        if (p[i] != 0xFF) return FALSE;
    return TRUE;
}

static inline WORD ReadLE16(const BYTE* p)
{
    return (WORD)p[0] | ((WORD)p[1] << 8);
}

static inline DWORD ReadLE32(const BYTE* p)
{
    return (DWORD)p[0] | ((DWORD)p[1] << 8) |
           ((DWORD)p[2] << 16) | ((DWORD)p[3] << 24);
}

static inline unsigned __int64 ReadLE64(const BYTE* p)
{
    unsigned __int64 v = 0;
    int i;
    if (!p) return 0;
    for (i = 7; i >= 0; i--)
        v = (v << 8) | (unsigned __int64)p[i];
    return v;
}

/* IDENTIFY words 100..103: 48-bit LBA. Each WORD is widened before << 32/48. */
static inline unsigned __int64 IdentLba48(const WORD* pIdent)
{
    if (!pIdent) return 0;
    return ((unsigned __int64)pIdent[100])
         | (((unsigned __int64)pIdent[101]) << 16)
         | (((unsigned __int64)pIdent[102]) << 32)
         | (((unsigned __int64)pIdent[103]) << 48);
}

static inline WORD GetRawValue16Lo(const BYTE* pRaw)
{
    return ((WORD)pRaw[1] << 8) | (WORD)pRaw[0];
}

static inline const SMART_ATTRIBUTE* FindAttr(const DRIVE_INFO* pInfo, BYTE id)
{
    int i;
    if (!pInfo) return NULL;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID == id)
            return &pInfo->attrData.stAttributes[i];
    }
    return NULL;
}

void ToUpperCopy(char* dst, int nDst, const char* src);
BOOL HasSmartAttr(const DRIVE_INFO* p, BYTE id);
BOOL DriveIsHdd(const DRIVE_INFO* p);

void TrimStr(char* sz);
void CopyDescStr(char* dst, size_t dstSize,
                 const BYTE* buf, DWORD dwBytes, DWORD offset);
void FillDriveProtocol(DRIVE_INFO* pInfo);
BOOL GetCapacityFromGeometry(HANDLE hDrive, DRIVE_INFO* pInfo);

void FillAtaProtocolFromIdent(DRIVE_INFO* pInfo, const WORD* pIdent);
void SwapATAString(char* szDst, const WORD* pSrc, int nWords);
int  FillSmartData(DRIVE_INFO* pInfo, const BYTE* pRawBuf);
int  FillSmartThreshold(DRIVE_INFO* pInfo, const BYTE* pRawBuf);
BOOL ValidateSmartData(const BYTE* pRawBuf, int nBufLen);
BOOL AcquireATASMART(HANDLE hDrive, int nDrive, DRIVE_INFO* pInfo, BOOL bReadLogs);
void ExtractTemperatureFromATA(DRIVE_INFO* pInfo);
void ExtractCommonATACounters(DRIVE_INFO* pInfo);
BOOL ATAPassThrough(HANDLE hDrive, BYTE bCommand, BYTE bFeatures,
    BYTE bSectorCount, BYTE bLBALow, BYTE bLBAMid, BYTE bLBAHigh,
    BYTE bDevice, BYTE* pDataBuf, DWORD dwDataLen, BOOL bDataIn);

void FillNvmeProtocolFromIdent(DRIVE_INFO* pInfo);
void FillNvmeTempThresholdsFromIdent(DRIVE_INFO* pInfo, DWORD nCopied);
void CopyNvmeIdentBuf(DRIVE_INFO* pInfo, const BYTE* pBuf, DWORD nAvail);
BOOL QueryNVMeProtocol(HANDLE hDrive, ULONG dataType, ULONG requestValue,
                       BYTE* pOut, DWORD dwOutLen, DWORD* pdwCopied);
void ExtractNVMeExtendedInfo(DRIVE_INFO* pInfo);
BOOL GetNVMeInfo(HANDLE hDrive, DRIVE_INFO* pInfo);
void TryNvmeLifetimeTemp(HANDLE hDrive, DRIVE_INFO* pInfo);

void PrefixUsbProtocol(DRIVE_INFO* pInfo);
BOOL IsRealtekNvmeUsbBridge(const DRIVE_INFO* p);
BOOL ParseVidPidFromHardwareId(const char* szHwId, WORD* pwVid, WORD* pwPid);
BOOL NVMeIdentifyJMicron(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeHealthLogJMicron(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeIdentifyASMedia(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeHealthLogASMedia(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeIdentifyRealtek(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeHealthLogRealtek(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeIdentifyVLI(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeHealthLogVLI(HANDLE hDrive, DRIVE_INFO* pInfo);
BOOL NVMeOverUSBTryAll(HANDLE hDrive, DRIVE_INFO* pInfo);

#endif /* SMART_INTERNAL_H */
