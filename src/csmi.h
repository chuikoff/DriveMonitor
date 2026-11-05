#pragma once
/* DriveMonitor - Common Storage Management Interface (CSMI) & Intel RST RAID.
 * GPL-2.0-or-later. See LICENSE. */

#include <windows.h>
#include <ntddscsi.h>
#include <stdint.h>
#include "smart.h"

#ifdef __cplusplus
extern "C" {
#endif

/* CSMI Control Codes (matching smartmontools / csmisas.h / SNIA CSMI spec) */
#define CC_CSMI_SAS_GET_DRIVER_INFO    1
#define CC_CSMI_SAS_GET_CNTLR_CONFIG   2
#define CC_CSMI_SAS_GET_CNTLR_STATUS   3
#define CC_CSMI_SAS_GET_PHY_INFO       20
#define CC_CSMI_SAS_STP_PASSTHRU       25
#define CC_CSMI_SAS_GET_SATA_SIGNATURE 26

/* 32-bit CC fallback codes used by some Intel RST / LSI drivers */
#define CC_CSMI_SAS_GET_DRIVER_INFO_32  0xCC770001
#define CC_CSMI_SAS_GET_CNTLR_CONFIG_32 0xCC770002
#define CC_CSMI_SAS_GET_PHY_INFO_32     0xCC770014
#define CC_CSMI_SAS_STP_PASSTHRU_32     0xCC770019

#define CSMI_SAS_SIGNATURE             "CSMI_SAS"
#define CSMI_ALL_SIGNATURE             "CSMIALL "

/* STP Passthrough flags */
#define CSMI_SAS_STP_READ              0x00000001
#define CSMI_SAS_STP_WRITE             0x00000002
#define CSMI_SAS_STP_UNSPECIFIED       0x00000000
#define CSMI_SAS_STP_PIO               0x00000010
#define CSMI_SAS_STP_DMA               0x00000020

#pragma pack(push, 1)

typedef struct _CSMI_SAS_STP_PASSTHRU {
    BYTE  bPhyIdentifier;
    BYTE  bPortIdentifier;
    BYTE  bConnectionRate;
    BYTE  bReserved;
    BYTE  bDestinationSASAddress[8];
    BYTE  bReserved2[4];
    BYTE  bCommandFIS[20];
    ULONG uFlags;
    ULONG uDataLength;
} CSMI_SAS_STP_PASSTHRU;

typedef struct _CSMI_SAS_STP_PASSTHRU_STATUS {
    BYTE  bConnectionStatus;
    BYTE  bReserved[3];
    BYTE  bStatusFIS[20];
    ULONG uSCR[16];
    ULONG uDataBytes;
} CSMI_SAS_STP_PASSTHRU_STATUS;

typedef struct _CSMI_SAS_STP_PASSTHRU_BUFFER {
    SRB_IO_CONTROL               IoctlHeader;
    CSMI_SAS_STP_PASSTHRU        Parameters;
    CSMI_SAS_STP_PASSTHRU_STATUS Status;
    BYTE                         bDataBuffer[512];
} CSMI_SAS_STP_PASSTHRU_BUFFER;

typedef struct _CSMI_SAS_DRIVER_INFO {
    BYTE  szName[32];
    BYTE  szDescription[64];
    WORD  usMajorRevision;
    WORD  usMinorRevision;
} CSMI_SAS_DRIVER_INFO;

typedef struct _CSMI_SAS_DRIVER_INFO_BUFFER {
    SRB_IO_CONTROL       IoctlHeader;
    CSMI_SAS_DRIVER_INFO Information;
} CSMI_SAS_DRIVER_INFO_BUFFER;

#pragma pack(pop)

/* CSMI controller and SATA drive operations */
BOOL CsmiProbeController(int nScsiPort, HANDLE* phScsi);
BOOL CsmiSendStpCommand(HANDLE hScsi, BYTE bPhy, BYTE bFeatures, BYTE bSectorCnt,
                        BYTE bLBALow, BYTE bCylLow, BYTE bCylHigh, BYTE bCommand,
                        BYTE* pDataBuf, DWORD dwDataLen, BOOL bDataIn);
BOOL CsmiGetIdentifyData(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo);
BOOL CsmiGetSMARTAttributes(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo);
BOOL CsmiGetSMARTThresholds(HANDLE hScsi, BYTE bPhy, DRIVE_INFO* pInfo);
BOOL CsmiGetSMARTReturnStatus(HANDLE hScsi, BYTE bPhy, BOOL* pbFail);

/* Scan and discover hidden physical SATA member drives behind Intel RST / CSMI RAID */
int  CsmiScanRaidDrives(DRIVE_INFO* pDrives, int nMaxDrives, int nCurrentCount);

#ifdef __cplusplus
}
#endif
