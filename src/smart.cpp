/* DriveMonitor - SMART orchestration (open, scan, names). MIT: see LICENSE. */
#include "smart_internal.h"

/* Final protocol string after type / SMART / NVMe ident are known. */
void FillDriveProtocol(DRIVE_INFO* pInfo)
{
    char hay[384];
    if (!pInfo) return;

    if (pInfo->bIsNVMe) {
        if (pInfo->bGotNVMeIdent) {
            FillNvmeProtocolFromIdent(pInfo);
        } else if (pInfo->szProtocol[0] == '\0') {
            safe_snprintf(pInfo->szProtocol, "NVMe");
        }
        PrefixUsbProtocol(pInfo);
        return;
    }

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        safe_snprintf(hay, "%s %s %s",
                  pInfo->szModel, pInfo->szBridgeVendor, pInfo->szBridgeProduct);
        if (IsRealtekNvmeUsbBridge(pInfo) ||
            pInfo->eUsbBridgeType == USB_BRIDGE_NVME_REALTEK ||
            strstr(hay, "RTL9210") || strstr(hay, "RTL921"))
            safe_snprintf(pInfo->szProtocol, "USB (RTL9210)");
        else
            safe_snprintf(pInfo->szProtocol, "USB");
        return;
    }

    if (!(pInfo->szProtocol[0] &&
          (strncmp(pInfo->szProtocol, "SATA", 4) == 0 ||
           strcmp(pInfo->szProtocol, "ATA") == 0))) {
        if (pInfo->eType == DRIVE_TYPE_HDD ||
            pInfo->eType == DRIVE_TYPE_SSD_SATA ||
            pInfo->eType == DRIVE_TYPE_M2_SATA)
            safe_snprintf(pInfo->szProtocol, "SATA");
        else if (pInfo->eType == DRIVE_TYPE_EMMC)
            safe_snprintf(pInfo->szProtocol, "eMMC");
        else if (pInfo->eType == DRIVE_TYPE_SD)
            safe_snprintf(pInfo->szProtocol, "SD");
        else if (pInfo->eType == DRIVE_TYPE_SCSI)
            safe_snprintf(pInfo->szProtocol, "SCSI");
        else if (pInfo->bIsUSB)
            safe_snprintf(pInfo->szProtocol, "USB");
        else if (pInfo->szProtocol[0] == '\0')
            safe_snprintf(pInfo->szProtocol, "ATA");
    }

    PrefixUsbProtocol(pInfo);
}

void TrimStr(char* sz)
{
    if (!sz || !sz[0]) return;
    int len = (int)strlen(sz);
    while (len > 0 && (sz[len - 1] == ' ' || sz[len - 1] == '\t' ||
                       sz[len - 1] == '\r' || sz[len - 1] == '\n')) {
        sz[--len] = '\0';
    }
    int start = 0;
    while (sz[start] == ' ' || sz[start] == '\t') start++;
    if (start > 0) {
        int j;
        for (j = 0; sz[start + j] != '\0'; j++)
            sz[j] = sz[start + j];
        sz[j] = '\0';
    }
}

/* Bounded copy of a STORAGE_DEVICE_DESCRIPTOR string.
 * Never reads past dwBytes; always NUL-terminates dst. */
void CopyDescStr(char* dst, size_t dstSize,
                        const BYTE* buf, DWORD dwBytes, DWORD offset)
{
    size_t nMax, i;
    const char* src;

    if (!dst || dstSize == 0)
        return;
    dst[0] = '\0';
    if (!buf || !offset || offset >= dwBytes)
        return;

    nMax = (size_t)(dwBytes - offset);
    if (nMax > dstSize - 1)
        nMax = dstSize - 1;

    src = (const char*)(buf + offset);
    for (i = 0; i < nMax && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
    TrimStr(dst);
}


DWORD GetRawValue(const BYTE* pRaw)
{
    if (!pRaw) return 0;
    return ((DWORD)pRaw[3] << 24) |
           ((DWORD)pRaw[2] << 16) |
           ((DWORD)pRaw[1] <<  8) |
            (DWORD)pRaw[0];
}

unsigned __int64 GetRawValue48(const BYTE* pRaw)
{
    if (!pRaw) return 0;
    /* BYTE promotes to int (32-bit). Widen first: << 32/40 on int is UB. */
    return ((unsigned __int64)pRaw[0])
         | (((unsigned __int64)pRaw[1]) <<  8)
         | (((unsigned __int64)pRaw[2]) << 16)
         | (((unsigned __int64)pRaw[3]) << 24)
         | (((unsigned __int64)pRaw[4]) << 32)
         | (((unsigned __int64)pRaw[5]) << 40);
}

/* Seagate 187 is a 32-bit counter. Hitachi/HGST (and similar firmware)
 * leave the low 16 bits at 0 and pack other fields above — that is not
 * millions of uncorrectable errors. */
int DecodeReportedUncorrect(const BYTE* pRaw, DRIVE_VENDOR vendor)
{
    unsigned __int64 q;
    DWORD lo32;
    WORD lo16;
    if (!pRaw) return -1;
    q = GetRawValue48(pRaw);
    lo32 = GetRawValue(pRaw);
    lo16 = (WORD)(lo32 & 0xFFFFu);
    if (vendor != VENDOR_SEAGATE && lo16 == 0 && q != 0)
        return -1;
    if (lo32 > (DWORD)INT_MAX) return INT_MAX;
    return (int)lo32;
}

/* Seagate 240 (and a copy some firmware writes into 196) is msec24hour32:
 * low 32 bits = hours, upper 16 bits = milliseconds. A plain event count
 * keeps those upper bytes at 0. Matching a small integer (both raw == 5)
 * is not this format. */
BOOL RemapRawIsFlyingHours(const DRIVE_INFO* pInfo, const BYTE* pRaw)
{
    const SMART_ATTRIBUTE* fly;
    unsigned __int64 v;
    if (!pInfo || !pRaw) return FALSE;
    v = GetRawValue48(pRaw);
    if (v == 0 || (v >> 32) == 0) return FALSE;
    fly = FindAttr(pInfo, 0xF0);
    if (!fly) return FALSE;
    return GetRawValue48(fly->bRawValue) == v;
}

int DecodeRemapEvents(const DRIVE_INFO* pInfo, const BYTE* pRaw)
{
    unsigned __int64 v;
    if (!pRaw) return -1;
    if (RemapRawIsFlyingHours(pInfo, pRaw)) return -1;
    v = GetRawValue48(pRaw);
    /* Upper bytes set, or low 32 above a signed counter: not an event count.
     * Clamping to INT_MAX made a flying-hours field look like 2147483647 events. */
    if (v > (unsigned __int64)INT_MAX) return -1;
    return (int)v;
}

DWORD SeagateRateOps(const BYTE* pRaw)
{
    if (!pRaw) return 0;
    return ((DWORD)pRaw[3] << 24) | ((DWORD)pRaw[2] << 16) |
           ((DWORD)pRaw[1] <<  8) |  (DWORD)pRaw[0];
}

unsigned SeagateRateErrs(const BYTE* pRaw)
{
    if (!pRaw) return 0;
    return (unsigned)pRaw[4] | ((unsigned)pRaw[5] << 8);
}

/* ============================================================
 * SMART attribute name table
 * Comprehensive coverage of standard + vendor-specific attributes
 * ============================================================ */
static const char* VendorSpecificAttrName(BYTE bID);

static const ATTR_NAME g_AttrNames[] = {
    /* ---- Standard ATA SMART attributes ---- */
    { 0x01, "Частота ошибок чтения",             ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x02, "Производительность",                ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x03, "Время раскрутки",                   ATTR_CRIT_NONE,      INTERP_DURATION     },
    { 0x04, "Циклы старт/стоп",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x05, "Переназначенные сектора",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0x06, "Запас канала чтения",               ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x07, "Частота ошибок позиционирования",   ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x08, "Скорость позиционирования",         ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x09, "Часы наработки",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0A, "Повторы раскрутки",                 ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0x0B, "Повторы калибровки",                ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0C, "Циклы включения",                   ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x0D, "Частота программных ошибок чтения", ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0x0E, "G-Sense",                          ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0x0F, "Повторы парковки",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x10, "Часы полёта головок",               ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0x11, "Повторы калибровки (альт.)",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* ---- SATA/ATA additional standard ---- */
    { 0x16, "Уровень гелия",                     ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0x17, "Гелий (нижний порог)",              ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x18, "Гелий (верхний порог)",             ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0x19, "Счётчик состояния гелия",           ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0x1A, "Остаток ресурса",                   ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0x1B, "Остаток выносливости",              ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0x1C, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },

    /* ---- SSD vendor-specific attributes (multiple vendors) ---- */
    { 0xA0, "Внезапные выключения",              ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA1, "Атрибут A1h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA2, "Худший резервный блок",             ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xA3, "Начальные плохие блоки",            ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA4, "Всего стираний",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA5, "Макс. стираний",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA6, "Мин. стираний",                     ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA7, "Среднее стираний",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA8, "Макс. стираний по спецификации",    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xA9, "Заявленный остаток ресурса",         ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0xAA, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xAB, "Ошибки программирования",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xAC, "Ошибки стирания",                   ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xAD, "Выравнивание износа",               ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xAE, "Внезапная потеря питания",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xAF, "Сбой защиты питания",               ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB0, "Атрибут B0h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB1, "Атрибут B1h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB2, "Атрибут B2h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB3, "Использовано резервных блоков (всего)", ATTR_CRIT_ADVISORY, INTERP_COUNTER32 },
    { 0xB4, "Свободные резервные блоки",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xB5, "Ошибки программирования (всего)",   ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB6, "Ошибки стирания (всего)",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB7, "Ошибки понижения SATA",             ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xB8, "Ошибка End-to-End",                 ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xB9, "Стабильность головок",              ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xBA, "Детектор вибрации",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xBB, "Неисправимые ошибки ECC",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xBC, "Таймауты команд",                   ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xBD, "Записи на большой высоте",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xBE, "Температура воздуха",               ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xBF, "G-Sense",                          ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xC0, "Парковки при выключении",           ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC1, "Циклы парковки",                    ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC2, "Температура",                       ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xC3, "Восстановлено ECC",                 ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xC4, "События переназначения",            ATTR_CRIT_CRITICAL,  INTERP_EVENT_COUNT  },
    { 0xC5, "Нестабильные сектора",              ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xC6, "Неисправимые сектора",              ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xC7, "Ошибки CRC UltraDMA",               ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xC8, "Частота ошибок записи",             ATTR_CRIT_CRITICAL,  INTERP_RATE         },
    { 0xC9, "Частота программных ошибок чтения", ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xCA, "Ошибки DAM",                        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xCB, "Отмена Run-Out",                    ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xCC, "Программная коррекция ECC",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xCD, "Тепловые выбросы",                  ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xCE, "Высота полёта",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xCF, "Ток раскрутки",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD0, "Жужжание шпинделя",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD1, "Offline-позиционирование",          ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD2, "Вибрация при записи",               ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD3, "Вибрация при записи (альт.)",       ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD4, "Удар при записи",                   ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD5, "Защита от падения",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xD6, "События свободного падения",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xDC, "Смещение диска",                    ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xDD, "G-Sense (альт.)",                   ATTR_CRIT_NONE,      INTERP_RATE         },
    { 0xDE, "Часы под нагрузкой",                ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xDF, "Повторы парковки",                  ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE0, "Трение парковки",                   ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE1, "Циклы парковки (альт.)",            ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE2, "Время парковки",                    ATTR_CRIT_NONE,      INTERP_DURATION     },
    { 0xE3, "Усиление крутящего момента",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE4, "Циклы парковки при выключении",     ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xE5, "Амплитуда GMR",                     ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE6, "Амплитуда GMR (альт.)",             ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xE7, "Остаток ресурса SSD / температура", ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },
    { 0xE8, "Резервное пространство",            ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xE9, "Записи NAND (ГБ) / износ",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEA, "Среднее стираний / записи",         ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEB, "Хорошие блоки / ресурс NAND",       ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xEC, "Ошибки записи",                     ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xED, "Ошибки CRC",                        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },
    { 0xEE, "Стабильность PMR",                  ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xEF, "Ошибки SATA PHY",                   ATTR_CRIT_ADVISORY,  INTERP_COUNTER32    },

    /* ---- Samsung-specific ---- */
    { 0xF0, "Часы полёта головок (альт.)",       ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF1, "Записано LBA",                      ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF2, "Прочитано LBA",                     ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF3, "Записано LBA (расш.)",              ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF4, "Прочитано LBA (расш.)",             ATTR_CRIT_NONE,      INTERP_COUNTER48    },
    { 0xF5, "Атрибут F5h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF6, "Атрибут F6h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF7, "Атрибут F7h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF8, "Атрибут F8h (vendor-specific)",      ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xF9, "Записи NAND (ГиБ)",                 ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xFA, "Повторы ошибок чтения",             ATTR_CRIT_ADVISORY,  INTERP_RATE         },
    { 0xFB, "Остаток запасных блоков",           ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },
    { 0xFC, "Новые плохие блоки NAND",           ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },
    { 0xFD, "Межповерхностные дефекты",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },
    { 0xFE, "Защита от падения",                 ATTR_CRIT_NONE,      INTERP_NORMAL       },
    { 0xFF, "Атрибут производителя",             ATTR_CRIT_NONE,      INTERP_NORMAL       },

    /* ---- Intel SSD specific ---- */
    { 0xB8, "Обнаружение ошибок End-to-End",     ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- Micron/Crucial SSD specific ---- */
    { 0xBB, "Неисправимые ошибки",               ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- Additional vendor-specific SSD attributes ---- */
    { 0xA0, "Температура (производитель)",       ATTR_CRIT_NONE,      INTERP_TEMPERATURE  },
    { 0xA9, "Заявленный остаток ресурса",         ATTR_CRIT_ADVISORY,  INTERP_LIFE_PCT     },

    /* ---- Kingston SSD specific ---- */
    { 0xB6, "Ошибки стирания (Kingston)",        ATTR_CRIT_CRITICAL,  INTERP_COUNTER32    },

    /* ---- WDC/HGST specific ---- */
    { 0x18, "Уровень гелия (WDC)",               ATTR_CRIT_ADVISORY,  INTERP_NORMAL       },

    /* ---- Seagate specific ---- */
    { 0x02, "Циклы старт/стоп (Seagate)",        ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* ---- SK Hynix specific ---- */
    { 0xB4, "Ошибки SATA PHY (SK Hynix)",        ATTR_CRIT_ADVISORY,  INTERP_COUNTER32   },

    /* ---- ADATA specific ---- */
    { 0xAD, "Среднее стираний (ADATA)",          ATTR_CRIT_NONE,      INTERP_COUNTER32    },

    /* Terminator */
    { 0x00, NULL,                                ATTR_CRIT_NONE,      INTERP_NORMAL       }
};

/* Vendor overlay: only IDs whose name differs from generic g_AttrNames.
 * Terminated by id 0. HGST/Hitachi share WD names (generic already matches).
 * Kioxia uses the Toshiba/Phison SATA-SSD overlay. Solidigm is VENDOR_INTEL. */
typedef struct _ATTR_OVERLAY {
    BYTE        id;
    const char* name;
    const char* nameEn;
} ATTR_OVERLAY;

static const ATTR_OVERLAY g_OvSeagateHdd[] = {
    { 0x01, "Частота ошибок чтения (RAW вендора)", "Read Error Rate (vendor RAW)" },
    { 0xBB, "Неисправимые ошибки", "Uncorrectable errors" },
    { 0xC3, "Восстановлено ECC (чтение)", "Hardware ECC recovered" },
    { 0xF0, "Часы полёта головок", "Head flying hours" },
    { 0x00, NULL, NULL }
};

static const ATTR_OVERLAY g_OvSamsungSsd[] = {
    { 0xB1, "Выравнивание износа", "Wear leveling" },
    { 0xB3, "Использовано резервных блоков", "Used reserved blocks" },
    { 0xB5, "Ошибки программирования", "Program fail count" },
    { 0xB6, "Ошибки стирания", "Erase fail count" },
    { 0xB7, "Плохие блоки (runtime)", "Runtime bad blocks" },
    { 0xBB, "Неисправимые ошибки", "Uncorrectable errors" },
    { 0xEB, "Защита от потери питания", "Power-loss protection" },
    { 0xF5, "Мин. стираний (Samsung)", "Min erase count (Samsung)" },
    { 0xF6, "Макс. стираний (Samsung)", "Max erase count (Samsung)" },
    { 0xF7, "Среднее стираний (Samsung)", "Average erase count (Samsung)" },
    { 0xF8, "Выравнивание износа (Samsung)", "Wear leveling (Samsung)" },
    { 0x00, NULL, NULL }
};

static const ATTR_OVERLAY g_OvPhisonSsd[] = {
    { 0xE7, "Остаток ресурса SSD", "SSD life remaining" },
    { 0xE9, "Записи NAND (ГиБ)", "NAND writes (GiB)" },
    { 0x00, NULL, NULL }
};

static const ATTR_OVERLAY g_OvToshibaHdd[] = {
    { 0xC7, "Ошибки CRC", "CRC errors" },
    { 0x00, NULL, NULL }
};

static const ATTR_OVERLAY g_OvMicronSsd[] = {
    { 0xCA, "Остаток ресурса %", "Percent lifetime remaining" },
    { 0xF6, "Записано секторов хоста", "Host sectors written" },
    { 0xAD, "Среднее стираний", "Average erase count" },
    { 0x00, NULL, NULL }
};

static const ATTR_OVERLAY g_OvIntelSsd[] = {
    { 0xAA, "Доступное резервное пространство", "Available reserved space" },
    { 0xE1, "Записи хоста", "Host writes" },
    { 0xE2, "Таймер нагрузки", "Timed workload" },
    { 0xE8, "Доступное резервное пространство", "Available reserved space" },
    { 0xE9, "Индикатор износа", "Media wear-out indicator" },
    { 0x00, NULL, NULL }
};

static const char* LookupOverlay(const ATTR_OVERLAY* tab, BYTE bID)
{
    int i;
    if (!tab) return NULL;
    for (i = 0; tab[i].id != 0; i++) {
        if (tab[i].id == bID)
            return (UiLangIsEn() && tab[i].nameEn) ? tab[i].nameEn : tab[i].name;
    }
    return NULL;
}

static const ATTR_OVERLAY* OverlayFor(const DRIVE_INFO* p)
{
    DRIVE_TYPE t;
    DRIVE_CONTROLLER c;
    BOOL ssd, hdd;
    if (!p) return NULL;
    t = p->eType;
    c = p->eController;
    ssd = (t == DRIVE_TYPE_SSD_SATA || t == DRIVE_TYPE_M2_SATA);
    hdd = (t == DRIVE_TYPE_HDD);
    if (t == DRIVE_TYPE_NVME)
        return NULL;

    /* Overlays key off controller, not brand (Kingston A400 is Phison). */
    switch (c) {
    case CONTROLLER_PHISON:
        return g_OvPhisonSsd;
    case CONTROLLER_SAMSUNG:
        return hdd ? NULL : g_OvSamsungSsd;
    case CONTROLLER_INTEL:
        return g_OvIntelSsd;
    case CONTROLLER_MICRON:
        return g_OvMicronSsd;
    case CONTROLLER_SEAGATE:
        return ssd ? NULL : g_OvSeagateHdd;
    case CONTROLLER_TOSHIBA:
        return ssd ? NULL : g_OvToshibaHdd;
    default:
        break;
    }
    /* Seagate HDD brand + HDD type keeps the Seagate overlay even if the
     * MCU was left UNKNOWN. Toshiba HDD CRC overlay similarly. */
    if (hdd && p->eVendor == VENDOR_SEAGATE)
        return g_OvSeagateHdd;
    if (hdd && p->eVendor == VENDOR_TOSHIBA)
        return g_OvToshibaHdd;
    return NULL;
}

static char g_szVendorAttrName[256][48];
static UI_LANG g_vendorAttrLang = (UI_LANG)(-1);

static void InitVendorAttrNames(void)
{
    int i;
    if (g_vendorAttrLang == UiLang()) return;
    g_vendorAttrLang = UiLang();
    for (i = 0; i < 256; i++) {
        (void)_snprintf(g_szVendorAttrName[i], 48, Tr(STR_ATTR_VENDOR), i);
        g_szVendorAttrName[i][47] = '\0';
    }
}

static const char* VendorSpecificAttrName(BYTE bID)
{
    InitVendorAttrNames();
    return g_szVendorAttrName[bID];
}

BOOL IsPhisonFamily(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    if (p->eController == CONTROLLER_PHISON)
        return TRUE;
    if (p->szFirmware[0] && strncmp(p->szFirmware, "HPS", 3) == 0)
        return TRUE;
    return FALSE;
}

static int HostGiBPlausible(unsigned __int64 host, unsigned __int64 nandGb,
                            DWORD poh, unsigned __int64 capGb)
{
    if (host == 0) return 0;
    if (nandGb > 0) {
        /* Host writes cannot exceed NAND written (WAF >= 1). */
        if (host > nandGb) return 0;
        if (nandGb > host * 50ULL) return 0;
        return 1;
    }
    if (poh >= 24 && host > (unsigned __int64)poh * 20ULL)
        return 0;
    if (capGb > 0 && host > capGb * 500ULL)
        return 0;
    return 1;
}

unsigned __int64 ScaleAtaHostGiB(const DRIVE_INFO* pInfo, unsigned __int64 raw)
{
    unsigned __int64 asGb, as32mb, asLba, capGb, nandGb;
    int erase;
    DWORD poh;
    if (!pInfo || raw == 0) return 0;
    asGb = raw;
    as32mb = (raw * 32ULL) / 1024ULL;
    asLba = raw / (1024ULL * 1024ULL * 2ULL);
    capGb = (unsigned __int64)(pInfo->dwCapacityMB / 1024u);
    erase = pInfo->nSSDAvgEraseCount;
    if (erase < 0) erase = pInfo->nSSDMaxEraseCount;
    nandGb = (capGb > 0 && erase > 0) ? (unsigned __int64)erase * capGb : 0;
    poh = pInfo->dwPowerOnHours;
    if (HostGiBPlausible(asGb, nandGb, poh, capGb))
        return asGb;
    /* 32 MiB units need a NAND figure. Without it a modest LBA count
     * (Samsung PM871 F1) passes the POH check as tens of thousands of GB. */
    if (nandGb > 0 && HostGiBPlausible(as32mb, nandGb, poh, capGb))
        return as32mb;
    return asLba;
}

static BOOL IsAtaSsdTypeInfo(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    return p->eType == DRIVE_TYPE_SSD_SATA || p->eType == DRIVE_TYPE_M2_SATA;
}

BOOL DriveIsHdd(const DRIVE_INFO* p)
{
    return p && p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe;
}

BOOL DriveTreatsC0AsPowerLoss(const DRIVE_INFO* pInfo)
{
    if (!pInfo) return FALSE;
    if (IsPhisonFamily(pInfo)) return TRUE;
    if (IsAtaSsdTypeInfo(pInfo)) return TRUE;
    return FALSE;
}

BOOL IsShockSensorAttr(BYTE bID)
{
    return bID == 0x0E || bID == 0xBF || bID == 0xDD;
}

/* Vendor-SSD IDs whose RAW packing is not ATA-standard. BE/BF and F1-F4 stay out. */
static BOOL IsVendorSpecificId(BYTE bID)
{
    if (bID == 0xBE || bID == 0xBF || bID == 0xBC || bID == 0xBD)
        return FALSE;
    if (bID >= 0xA0 && bID <= 0xBB)
        return TRUE;
    if (bID >= 0xE7 && bID <= 0xF0)
        return TRUE;
    if (bID >= 0xF5 && bID <= 0xF8)
        return TRUE;
    return FALSE;
}

static RAW_ENC EncFromInterp(ATTR_INTERP e)
{
    switch (e) {
    case INTERP_LIFE_PCT:    return RAW_ENC_PERCENT;
    case INTERP_TEMPERATURE: return RAW_ENC_TEMP_C;
    case INTERP_COUNTER32:   return RAW_ENC_COUNTER32;
    case INTERP_COUNTER48:   return RAW_ENC_COUNTER48;
    case INTERP_RATE:        return RAW_ENC_EVENTS;
    case INTERP_DURATION:    return RAW_ENC_COUNTER32;
    case INTERP_EVENT_COUNT: return RAW_ENC_EVENTS;
    case INTERP_NORMAL:
    default:                 return RAW_ENC_COUNTER32;
    }
}

static void ApplyPhisonSsdDecode(BYTE bID, ATTR_DECODE* out)
{
    switch (bID) {
    case 0xA0:
        out->szName = TN("Внезапные выключения", "Unsafe shutdowns");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA1:
        out->szName = TN("Резервные блоки (осталось)", "Spare blocks remaining");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xA3:
        out->szName = TN("Начальные плохие блоки", "Initial bad blocks");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA4:
        out->szName = TN("Всего стираний", "Total erase count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA5:
        out->szName = TN("Макс. стираний", "Max erase count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA6:
        out->szName = TN("Мин. стираний", "Min erase count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA7:
        out->szName = TN("Среднее стираний", "Average erase count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA8:
        out->szName = TN("Макс. стираний по спецификации", "Specified max erase count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 75;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xA9:
        out->szName = TN("Заявленный остаток ресурса", "Reported life remaining");
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xAF:
        out->szName = TN("Сбой защиты питания", "Power-loss protection fail");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xC0:
        out->szName = TN("Аварийные отключения питания", "Unsafe power-off events");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB0:
        out->szName = TN("Ошибки стирания (худший кристалл)", "Erase fails (worst die)");
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB1:
        out->szName = TN("Всего циклов выравнивания износа", "Wear-leveling count");
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xB2:
        out->szName = TN("Резервные блоки (использовано)", "Used spare blocks");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 70;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xB5:
        out->szName = TN("Ошибки программирования (всего)", "Program fail count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xB6:
        out->szName = TN("Ошибки стирания (всего)", "Erase fail count");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_CRITICAL;
        break;
    case 0xC3:
        out->szName = TN("ECC (вендор)", "ECC (vendor)");
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 50;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xE7:
        out->szName = TN("Остаток ресурса SSD", "SSD life remaining");
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xE8:
        out->szName = TN("Резервное пространство", "Available reserved space");
        out->eEnc = RAW_ENC_PERCENT;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 80;
        out->eCrit = ATTR_CRIT_ADVISORY;
        break;
    case 0xE9:
        out->szName = TN("Записи NAND (ГиБ)", "NAND writes (GiB)");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 75;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF1:
        out->szName = TN("Записано хостом (ГБ)", "Host written (GB)");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF2:
        out->szName = TN("Прочитано хостом (ГБ)", "Host read (GB)");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eState = ATTR_DECODE_KNOWN;
        out->nSemanticConfidence = 85;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    case 0xF5:
    case 0xF6:
    case 0xF7:
        out->szName = VendorSpecificAttrName(bID);
        out->eEnc = RAW_ENC_UNKNOWN;
        out->eState = ATTR_DECODE_UNKNOWN;
        out->nSemanticConfidence = 0;
        out->eCrit = ATTR_CRIT_NONE;
        break;
    default:
        break;
    }
}

static const char* AttrNameEn(BYTE id)
{
    switch (id) {
    case 0x01: return "Read Error Rate";
    case 0x02: return "Throughput Performance";
    case 0x03: return "Spin-Up Time";
    case 0x04: return "Start/Stop Count";
    case 0x05: return "Reallocated Sectors";
    case 0x06: return "Read Channel Margin";
    case 0x07: return "Seek Error Rate";
    case 0x08: return "Seek Time Performance";
    case 0x09: return "Power-On Hours";
    case 0x0A: return "Spin Retry Count";
    case 0x0B: return "Calibration Retry Count";
    case 0x0C: return "Power Cycle Count";
    case 0x0D: return "Soft Read Error Rate";
    case 0x0E: return "G-Sense";
    case 0x10: return "Head Flying Hours";
    case 0x16: return "Helium Level";
    case 0xAA: return "Available Reserved Space";
    case 0xAB: return "Program Fail Count";
    case 0xAC: return "Erase Fail Count";
    case 0xAD: return "Wear Leveling Count";
    case 0xAE: return "Unexpected Power Loss";
    case 0xAF: return "Power-Loss Protection Fail";
    case 0xB1: return "Wear Leveling";
    case 0xB3: return "Used Reserved Blocks";
    case 0xB5: return "Program Fail Count (total)";
    case 0xB6: return "Erase Fail Count (total)";
    case 0xB7: return "SATA Downshift Errors";
    case 0xB8: return "End-to-End Error";
    case 0xBB: return "Reported Uncorrectable";
    case 0xBC: return "Command Timeout";
    case 0xBD: return "High Fly Writes";
    case 0xBE: return "Airflow Temperature";
    case 0xBF: return "G-Sense";
    case 0xC0: return "Power-Off Retracts";
    case 0xC1: return "Load/Unload Cycles";
    case 0xC2: return "Temperature";
    case 0xC3: return "Hardware ECC Recovered";
    case 0xC4: return "Reallocation Events";
    case 0xC5: return "Current Pending Sectors";
    case 0xC6: return "Uncorrectable Sectors";
    case 0xC7: return "UltraDMA CRC Errors";
    case 0xC8: return "Write Error Rate";
    case 0xC9: return "Soft Read Error Rate";
    case 0xCA: return "Data Address Mark Errors";
    case 0xE7: return "SSD Life Remaining";
    case 0xE8: return "Available Reserved Space";
    case 0xE9: return "NAND Writes (GiB)";
    case 0xF0: return "Head Flying Hours";
    case 0xF1: return "Host Written (GB)";
    case 0xF2: return "Host Read (GB)";
    case 0xA0: return "Unsafe Shutdowns";
    case 0xA3: return "Initial Bad Blocks";
    case 0xA4: return "Total Erase Count";
    case 0xA5: return "Max Erase Count";
    case 0xA6: return "Min Erase Count";
    case 0xA7: return "Average Erase Count";
    case 0xA8: return "Specified Max Erase Count";
    case 0xA9: return "Reported Life Remaining";
    default:   return NULL;
    }
}

void GetAttrDecode(BYTE bID, const DRIVE_INFO* pInfo, ATTR_DECODE* out)
{
    int i;
    const char* ovl;
    BOOL bOverlayTrustsEnc;

    if (!out) return;
    InitVendorAttrNames();
    out->szName = VendorSpecificAttrName(bID);
    out->eEnc = RAW_ENC_UNKNOWN;
    out->eState = ATTR_DECODE_UNKNOWN;
    out->nSemanticConfidence = 0;
    out->eCrit = ATTR_CRIT_NONE;

    i = 0;
    while (g_AttrNames[i].szName != NULL) {
        if (g_AttrNames[i].bID == bID) {
            out->szName = g_AttrNames[i].szName;
            if (UiLangIsEn()) {
                const char* en = AttrNameEn(bID);
                if (en) out->szName = en;
            }
            out->eCrit = g_AttrNames[i].eCritLevel;
            out->eEnc = EncFromInterp(g_AttrNames[i].eInterp);
            out->nSemanticConfidence = 60;
            break;
        }
        i++;
    }

    /* Well-known ATA encodings (name already from table). */
    switch (bID) {
    case 0x05: out->eEnc = RAW_ENC_SECTORS_LO16; break;
    case 0x09: out->eEnc = RAW_ENC_HOURS; break;
    case 0x04:
    case 0x0C: out->eEnc = RAW_ENC_CYCLES; break;
    case 0xC2:
    case 0xBE: out->eEnc = RAW_ENC_TEMP_C; break;
    case 0xC4: out->eEnc = RAW_ENC_EVENTS; break;
    case 0xBC: out->eEnc = RAW_ENC_SECTORS_LO16; break;
    case 0xF1:
    case 0xF2:
    case 0xF3:
    case 0xF4: out->eEnc = RAW_ENC_LBA; break;
    default: break;
    }

    ovl = LookupOverlay(OverlayFor(pInfo), bID);
    if (ovl)
        out->szName = ovl;

    /* HDD must not inherit SSD NAND/endurance names if the same ID exists. */
    if (DriveIsHdd(pInfo)) {
        switch (bID) {
        case 0x1A:
        case 0x1B:
        case 0xA9:
        case 0xE7:
        case 0xE9:
        case 0xEB:
        case 0xF9:
        case 0xFC:
            out->szName = VendorSpecificAttrName(bID);
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            out->nSemanticConfidence = 20;
            break;
        default:
            break;
        }
    }

    /* 195: ATA Hardware ECC Recovered on HDD (plate read correction).
     * Not NAND. On SSD the same ID is vendor-specific. */
    if (bID == 0xC3 && pInfo && !IsPhisonFamily(pInfo)) {
        if (DriveIsHdd(pInfo)) {
            if (pInfo->eVendor == VENDOR_SEAGATE)
                out->szName = "Восстановлено ECC (чтение)";
            else
                out->szName = "Восстановлено ECC";
            out->eEnc = RAW_ENC_COUNTER32;
            out->eCrit = ATTR_CRIT_NONE;
            out->nSemanticConfidence = (pInfo->eVendor == VENDOR_SEAGATE) ? 90 : 70;
        } else {
            out->szName = TN("ECC (вендор)", "ECC (vendor)");
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            out->nSemanticConfidence = 30;
        }
    }

    /* Overlay on Samsung/Intel/Micron is a trusted profile for that ID.
     * Phison overlay is names-only for E7/E9; encoding comes from ApplyPhison. */
    bOverlayTrustsEnc = FALSE;
    if (ovl && pInfo) {
        switch (pInfo->eController) {
        case CONTROLLER_SAMSUNG:
        case CONTROLLER_INTEL:
        case CONTROLLER_MICRON:
            bOverlayTrustsEnc = TRUE;
            break;
        default:
            break;
        }
    }

    if (IsVendorSpecificId(bID) && !bOverlayTrustsEnc) {
        BOOL bSsdLike = TRUE; /* no pInfo: do not claim a universal encoding */
        if (pInfo) {
            bSsdLike = (pInfo->eType == DRIVE_TYPE_SSD_SATA ||
                        pInfo->eType == DRIVE_TYPE_M2_SATA ||
                        pInfo->eType == DRIVE_TYPE_NVME ||
                        IsPhisonFamily(pInfo));
        }
        if (bSsdLike) {
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            /* Keep the name. Profile (Phison) may restore KNOWN below. */
            if (!ovl && out->nSemanticConfidence > 30)
                out->nSemanticConfidence = 30;
        } else if (bID == 0xB5 || bID == 0xB6 || bID == 0xAB || bID == 0xAC) {
            /* SSD names on an HDD — packing is not a program-fail count. */
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            out->nSemanticConfidence = 40;
        }
    }

    if (IsPhisonFamily(pInfo))
        ApplyPhisonSsdDecode(bID, out);

    /* SSD C0 is emergency/unsafe power-off count, not HDD head-park. */
    if (bID == 0xC0 && DriveTreatsC0AsPowerLoss(pInfo)) {
        out->szName = TN("Аварийные отключения питания", "Unsafe power-off events");
        out->eEnc = RAW_ENC_COUNTER32;
        out->eCrit = ATTR_CRIT_NONE;
        if (out->nSemanticConfidence < 80)
            out->nSemanticConfidence = 80;
    }

    /* 187: Hitachi-style packing is not a sector-error counter. */
    if (bID == 0xBB && pInfo) {
        const SMART_ATTRIBUTE* a187 = FindAttr(pInfo, 0xBB);
        if (a187 && DecodeReportedUncorrect(a187->bRawValue, pInfo->eVendor) < 0) {
            out->eEnc = RAW_ENC_UNKNOWN;
            out->eCrit = ATTR_CRIT_NONE;
            if (out->nSemanticConfidence > 30)
                out->nSemanticConfidence = 30;
        }
    }

    out->eState = (out->eEnc == RAW_ENC_UNKNOWN)
                  ? ATTR_DECODE_UNKNOWN
                  : ATTR_DECODE_KNOWN;
}

const char* GetAttrNameEx(BYTE bID, const DRIVE_INFO* pInfo)
{
    ATTR_DECODE d;
    GetAttrDecode(bID, pInfo, &d);
    return d.szName ? d.szName : VendorSpecificAttrName(bID);
}

const char* GetDriveTypeName(DRIVE_TYPE eType)
{
    switch (eType) {
    case DRIVE_TYPE_HDD:      return "HDD";
    case DRIVE_TYPE_SSD_SATA: return "SSD SATA";
    case DRIVE_TYPE_NVME:     return "NVMe SSD";
    case DRIVE_TYPE_USB:      return "USB";
    case DRIVE_TYPE_M2_SATA:  return "M.2 SATA";
    case DRIVE_TYPE_EMMC:     return "eMMC";
    case DRIVE_TYPE_SD:       return "SD";
    case DRIVE_TYPE_SCSI:     return "SAS";
    default:                  return Tr(STR_UNKNOWN_TYPE);
    }
}

const char* GetHealthStatusName(DRIVE_HEALTH_STATUS eStatus)
{
    switch (eStatus) {
    case HEALTH_STATUS_GOOD:     return Tr(STR_HS_GOOD);
    case HEALTH_STATUS_OBSERVE:  return Tr(STR_HS_WATCH);
    case HEALTH_STATUS_CAUTION:  return Tr(STR_HS_ATTN);
    case HEALTH_STATUS_BAD:      return Tr(STR_HS_BAD);
    case HEALTH_STATUS_WARNING:  return Tr(STR_HS_BAD);
    case HEALTH_STATUS_CRITICAL: return Tr(STR_HS_CRIT);
    default:                     return Tr(STR_HS_UNK);
    }
}

const char* GetHealthStatusNameShort(DRIVE_HEALTH_STATUS eStatus)
{
    switch (eStatus) {
    case HEALTH_STATUS_CAUTION:  return Tr(STR_HS_ATTN_SHORT);
    case HEALTH_STATUS_CRITICAL: return Tr(STR_HS_CRIT_SHORT);
    default:                     return GetHealthStatusName(eStatus);
    }
}

const char* GetDiskStatusName(const DRIVE_INFO* p)
{
    if (!p)
        return Tr(STR_HS_UNK);
    if (p->eDiskStatus != HEALTH_STATUS_UNKNOWN)
        return GetHealthStatusName(p->eDiskStatus);
    if (p->bIsUSB && p->bSMART_Supported)
        return Tr(STR_NO_BRIDGE_STATUS);
    if (p->bIsUSB)
        return Tr(STR_NO_SMART);
    return Tr(STR_HS_UNK);
}

const char* GetVendorName(DRIVE_VENDOR eVendor)
{
    switch (eVendor) {
    case VENDOR_SAMSUNG:       return "Samsung";
    case VENDOR_WDC:           return "Western Digital";
    case VENDOR_SEAGATE:       return "Seagate";
    case VENDOR_TOSHIBA:       return "Toshiba";
    case VENDOR_HITACHI:       return "HGST";
    case VENDOR_INTEL:         return "Intel";
    case VENDOR_MICRON:        return "Micron";
    case VENDOR_KINGSTON:      return "Kingston";
    case VENDOR_SANDISK:       return "SanDisk";
    case VENDOR_SKHYNIX:       return "SK Hynix";
    case VENDOR_KIOXIA:        return "Kioxia";
    case VENDOR_ADATA:         return "ADATA";
    case VENDOR_PNY:           return "PNY";
    case VENDOR_CORSAIR:       return "Corsair";
    case VENDOR_LEXAR:         return "Lexar";
    case VENDOR_SILICON_POWER: return "Silicon Power";
    case VENDOR_TEAMGROUP:     return "TeamGroup";
    case VENDOR_GOODRAM:       return "GOODRAM";
    case VENDOR_PLEXTOR:       return "Plextor";
    case VENDOR_OCZ:           return "OCZ";
    case VENDOR_UTANIA:        return "Utania";
    case VENDOR_PATRIOT:       return "Patriot";
    case VENDOR_MSI:           return "MSI";
    case VENDOR_RADEON:        return "Radeon";
    case VENDOR_OTHER:         return "Other";
    default:                   return "Unknown";
    }
}

const char* GetControllerName(DRIVE_CONTROLLER eController)
{
    switch (eController) {
    case CONTROLLER_PHISON:   return "Phison";
    case CONTROLLER_SMI:      return "Silicon Motion";
    case CONTROLLER_SAMSUNG:  return "Samsung";
    case CONTROLLER_MARVELL:  return "Marvell";
    case CONTROLLER_INTEL:    return "Intel";
    case CONTROLLER_MICRON:   return "Micron";
    case CONTROLLER_HYNIX:    return "SK Hynix";
    case CONTROLLER_KIOXIA:   return "Kioxia";
    case CONTROLLER_SANDISK:  return "SanDisk";
    case CONTROLLER_REALTEK:  return "Realtek";
    case CONTROLLER_SEAGATE:  return "Seagate";
    case CONTROLLER_WD:       return "Western Digital";
    case CONTROLLER_TOSHIBA:  return "Toshiba";
    case CONTROLLER_HITACHI:  return "HGST";
    case CONTROLLER_AMAZON:   return "Amazon";
    case CONTROLLER_MAXIO:    return "Maxio";
    case CONTROLLER_INNOGRIT: return "Innogrit";
    default:                  return "—";
    }
}

const char* DriveControllerLabel(const DRIVE_INFO* p)
{
    if (p && p->szControllerChip[0])
        return p->szControllerChip;
    return GetControllerName(p ? p->eController : CONTROLLER_UNKNOWN);
}

/* ============================================================
 * Drive vendor detection from model name
 * Identifies vendors for attribute interpretation
 * ============================================================ */
static BOOL ModelLooksSeagate(const char* szUpper)
{
    const char* p;
    if (!szUpper || !szUpper[0])
        return FALSE;
    if (strstr(szUpper, "SEAGATE") || strstr(szUpper, "BARRACUDA") ||
        strstr(szUpper, "FIRECUDA") || strstr(szUpper, "IRONWOLF") ||
        strstr(szUpper, "SKYHAWK") || strstr(szUpper, "EXOS"))
        return TRUE;
    p = szUpper;
    while ((p = strstr(p, "ST")) != NULL) {
        if ((p == szUpper || !isalnum((unsigned char)p[-1])) &&
            p[2] >= '0' && p[2] <= '9')
            return TRUE;
        p++;
    }
    return FALSE;
}

DRIVE_VENDOR DetectDriveVendor(const char* szModel)
{
    if (!szModel || !szModel[0]) return VENDOR_UNKNOWN;

    /* Convert to uppercase for case-insensitive matching */
    char szUpper[42];
    int i;
    for (i = 0; i < 41 && szModel[i]; i++)
        szUpper[i] = (char)toupper((unsigned char)szModel[i]);
    szUpper[i] = '\0';

    /* MZ-76E500, MZNLN128, MZVLB256 — OEM codes start with MZ + letter. */
    if (strstr(szUpper, "SAMSUNG") || strstr(szUpper, "MZ-") ||
        (szUpper[0] == 'M' && szUpper[1] == 'Z' &&
         szUpper[2] >= 'A' && szUpper[2] <= 'Z') ||
        strstr(szUpper, "PM9") || strstr(szUpper, "SM9") ||
        (strstr(szUpper, "SSD ") && strstr(szUpper, "EVO")) ||
        (strstr(szUpper, "SSD ") && strstr(szUpper, "PRO")))
        return VENDOR_SAMSUNG;

    if (strstr(szUpper, "WDC") || strstr(szUpper, "WD ") ||
        strstr(szUpper, "WESTERN") || strstr(szUpper, "BLUE") ||
        strstr(szUpper, "BLACK") || strstr(szUpper, "GREEN") ||
        strstr(szUpper, "RED ") || strstr(szUpper, "PURPLE") ||
        strstr(szUpper, "GOLD") || strstr(szUpper, "WD20") ||
        strstr(szUpper, "WD30") || strstr(szUpper, "WD40") ||
        strstr(szUpper, "WD50") || strstr(szUpper, "WD60") ||
        strstr(szUpper, "WD80") || strstr(szUpper, "WD10"))
        return VENDOR_WDC;

    if (strstr(szUpper, "UTANIA") || strstr(szUpper, "MR102") ||
        (szUpper[0] == 'O' && szUpper[1] == 'O' && szUpper[2] == 'S' &&
         szUpper[3] >= '0' && szUpper[3] <= '9'))
        return VENDOR_UTANIA;

    if (ModelLooksSeagate(szUpper))
        return VENDOR_SEAGATE;

    if (strstr(szUpper, "TOSHIBA") || strstr(szUpper, "MK") ||
        strstr(szUpper, "DT01") || strstr(szUpper, "DT02") ||
        strstr(szUpper, "MQ01") || strstr(szUpper, "MQ02") ||
        strstr(szUpper, "MG"))
        return VENDOR_TOSHIBA;

    if (strstr(szUpper, "HITACHI") || strstr(szUpper, "HGST") ||
        strstr(szUpper, "HUA") || strstr(szUpper, "HDT") ||
        strstr(szUpper, "HDP") || strstr(szUpper, "HCS") ||
        strstr(szUpper, "IC25") || strstr(szUpper, "IC35") ||
        strstr(szUpper, "HTS") ||
        strstr(szUpper, "HMS") || strstr(szUpper, "HUH"))
        return VENDOR_HITACHI;

    if (strstr(szUpper, "INTEL") || strstr(szUpper, "SSDSC") ||
        strstr(szUpper, "SSDPED") || strstr(szUpper, "SSDPE") ||
        strstr(szUpper, "SOLIDIGM"))
        return VENDOR_INTEL;

    if (strstr(szUpper, "MICRON") || strstr(szUpper, "CRUCIAL") ||
        strstr(szUpper, "CT") || strstr(szUpper, "MX") ||
        strstr(szUpper, "M4-") || strstr(szUpper, "MTF"))
        return VENDOR_MICRON;

    if (strstr(szUpper, "KINGSTON") || strstr(szUpper, "SA400") ||
        strstr(szUpper, "SA600") || strstr(szUpper, "SV300") ||
        strstr(szUpper, "SHFS") || strstr(szUpper, "SHSS") ||
        strstr(szUpper, "SKC"))
        return VENDOR_KINGSTON;

    if (strstr(szUpper, "SANDISK") || strstr(szUpper, "SDSS") ||
        strstr(szUpper, "SD8S"))
        return VENDOR_SANDISK;

    if (strstr(szUpper, "SKHYNIX") || strstr(szUpper, "HFS") ||
        strstr(szUpper, "HFM") || strstr(szUpper, "BC7") ||
        strstr(szUpper, "BC5") || strstr(szUpper, "PC7") ||
        strstr(szUpper, "PC5"))
        return VENDOR_SKHYNIX;

    if (strstr(szUpper, "KIOXIA") || strstr(szUpper, "EXCERIA") ||
        strstr(szUpper, "KXG") || strstr(szUpper, "BG"))
        return VENDOR_KIOXIA;

    if (strstr(szUpper, "ADATA") || strstr(szUpper, "SX8") ||
        strstr(szUpper, "SP9") || strstr(szUpper, "SU8") ||
        strstr(szUpper, "IM2P"))
        return VENDOR_ADATA;

    if (strstr(szUpper, "PNY") || strstr(szUpper, "CS"))
        return VENDOR_PNY;

    if (strstr(szUpper, "CORSAIR") || strstr(szUpper, "CSSD") ||
        strstr(szUpper, "FORCE"))
        return VENDOR_CORSAIR;

    if (strstr(szUpper, "LEXAR") || strstr(szUpper, "NM"))
        return VENDOR_LEXAR;

    if (strstr(szUpper, "SILICON POWER") || strstr(szUpper, "SPCC"))
        return VENDOR_SILICON_POWER;

    if (strstr(szUpper, "TEAMGROUP") || strstr(szUpper, "TEAM ") ||
        strstr(szUpper, "TM8") || strstr(szUpper, "T-FORCE") ||
        strstr(szUpper, "TFORCE"))
        return VENDOR_TEAMGROUP;

    if (strstr(szUpper, "GOODRAM") || strstr(szUpper, "IRDM"))
        return VENDOR_GOODRAM;

    if (strstr(szUpper, "PLEXTOR") || strstr(szUpper, "PX-"))
        return VENDOR_PLEXTOR;

    if (strstr(szUpper, "OCZ") || strstr(szUpper, "VERTEX") ||
        strstr(szUpper, "AGILITY"))
        return VENDOR_OCZ;

    if (strstr(szUpper, "PATRIOT") || strstr(szUpper, "P210") ||
        strstr(szUpper, "P300") || strstr(szUpper, "P400"))
        return VENDOR_PATRIOT;

    if (strstr(szUpper, "MSI") || strstr(szUpper, "SPATIUM") ||
        strstr(szUpper, "M450") || strstr(szUpper, "M390") ||
        strstr(szUpper, "M480"))
        return VENDOR_MSI;

    if (strstr(szUpper, "RADEON") || strstr(szUpper, "R5SL") ||
        strstr(szUpper, "R3SL") || strstr(szUpper, "R7SL"))
        return VENDOR_RADEON;

    /* If model has SSD keyword but vendor unknown */
    if (strstr(szUpper, "SSD") || strstr(szUpper, "NVME"))
        return VENDOR_OTHER;

    return VENDOR_UNKNOWN;
}

void ToUpperCopy(char* dst, int nDst, const char* src)
{
    int i;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (i = 0; i < nDst - 1 && src[i]; i++)
        dst[i] = (char)toupper((unsigned char)src[i]);
    dst[i] = '\0';
}

BOOL HasSmartAttr(const DRIVE_INFO* p, BYTE id)
{
    int i;
    if (!p || id == 0) return FALSE;
    for (i = 0; i < 30; i++) {
        if (p->attrData.stAttributes[i].bAttrID == id)
            return TRUE;
    }
    return FALSE;
}

static int CountSmartAttrRange(const DRIVE_INFO* p, BYTE lo, BYTE hi)
{
    int n = 0;
    unsigned id;
    for (id = (unsigned)lo; id <= (unsigned)hi; id++) {
        if (HasSmartAttr(p, (BYTE)id)) n++;
    }
    return n;
}

/* NVMe Identify Controller bytes 0-1 are PCI VID (spec). Named fields of
 * NVME_IDENTIFY_CONTROLLER skip this; read the raw copy. Do not invent a VID. */
static USHORT NvmeIdentVid(const DRIVE_INFO* p)
{
    const BYTE* id;
    if (!p || !p->bGotNVMeIdent) return 0;
    id = (const BYTE*)&p->nvmeIdent;
    return (USHORT)id[0] | ((USHORT)id[1] << 8);
}

static DRIVE_CONTROLLER ControllerFromNvmeVid(USHORT vid)
{
    switch (vid) {
    case 0x144D: return CONTROLLER_SAMSUNG;
    case 0x1987:
    case 0x1D97:
    case 0x1C2C: return CONTROLLER_PHISON;
    case 0x126F: return CONTROLLER_SMI;
    case 0x1B4B: return CONTROLLER_MARVELL;   /* PCI SIG: Marvell */
    case 0x1C5C: return CONTROLLER_HYNIX;
    case 0x8086: return CONTROLLER_INTEL;
    case 0x025E: return CONTROLLER_INTEL;    /* Solidigm (ex-Intel NAND) */
    case 0x1E0F:
    case 0x1179: return CONTROLLER_KIOXIA;   /* 1179 = Toshiba/Kioxia PCI */
    case 0x15B7: return CONTROLLER_SANDISK;
    case 0x1B96: return CONTROLLER_WD;
    case 0x10EC: return CONTROLLER_REALTEK;
    case 0x1BB1: return CONTROLLER_SEAGATE;
    case 0x1D0F: return CONTROLLER_AMAZON;
    case 0x1344:
    case 0xC0A9: return CONTROLLER_MICRON;
    case 0x1E4B: return CONTROLLER_MAXIO;
    case 0x1DDC: return CONTROLLER_INNOGRIT;
    /* 0x1CC1 is ADATA's OEM PCI VID, not a controller. SX8200 etc. are SMI
     * in the model table below — same as Hard Disk Sentinel. */
    default:     return CONTROLLER_UNKNOWN;
    }
}

static BOOL ModelHasChipToken(const char* u, const char* tok)
{
    const char* p;
    if (!u || !tok || !tok[0]) return FALSE;
    for (p = u; (p = strstr(p, tok)) != NULL; p++) {
        if (p != u && isalnum((unsigned char)p[-1]))
            continue;
        return TRUE;
    }
    return FALSE;
}

static DRIVE_CONTROLLER DetectDriveController(const DRIVE_INFO* p)
{
    USHORT vid;
    char szModelU[48], szFwU[16];
    int nPhison;
    BOOL hdd, ssd;

    if (!p) return CONTROLLER_UNKNOWN;

    hdd = (p->eType == DRIVE_TYPE_HDD);
    ssd = (p->eType == DRIVE_TYPE_SSD_SATA ||
           p->eType == DRIVE_TYPE_M2_SATA ||
           p->eType == DRIVE_TYPE_NVME ||
           p->bIsNVMe);

    ToUpperCopy(szFwU, sizeof(szFwU), p->szFirmware);
    ToUpperCopy(szModelU, sizeof(szModelU), p->szModel);

    /* 1. NVMe PCI/identify VID — highest confidence. */
    if (p->bIsNVMe || p->bGotNVMeIdent) {
        vid = NvmeIdentVid(p);
        if (vid) {
            DRIVE_CONTROLLER c = ControllerFromNvmeVid(vid);
            if (c != CONTROLLER_UNKNOWN)
                return c;
        }
    }

    /* USB dongle/bridge with no disk SMART: model is the chip (ASM225), not the SSD. */
    if (p->bIsUSB && !p->bSMART_Supported && !p->bIsNVMe)
        return CONTROLLER_UNKNOWN;

    /* 2. Firmware / model strings (work even before SMART). */
    if (strncmp(szFwU, "HPS", 3) == 0 ||
        strncmp(szFwU, "SBFK", 4) == 0 ||
        strncmp(szFwU, "SBFM", 4) == 0 ||
        strncmp(szFwU, "SAFM", 4) == 0 ||
        strncmp(szFwU, "SBFB", 4) == 0 ||
        strncmp(szFwU, "ECFM", 4) == 0 ||
        strncmp(szFwU, "E8FM", 4) == 0 ||
        strncmp(szFwU, "E7FM", 4) == 0 ||
        strncmp(szFwU, "S9FM", 4) == 0 ||
        strncmp(szFwU, "S8FM", 4) == 0 ||
        strncmp(szFwU, "U11", 3) == 0 ||
        strncmp(szFwU, "U10", 3) == 0 ||
        strncmp(szFwU, "T07", 3) == 0 ||
        strncmp(szFwU, "EJFM", 4) == 0)
        return CONTROLLER_PHISON;

    /* Token match: "ASM225" must not count as SM225. */
    if (ModelHasChipToken(szModelU, "SM226") || ModelHasChipToken(szModelU, "SM225") ||
        ModelHasChipToken(szModelU, "SM232") || ModelHasChipToken(szModelU, "SM250") ||
        strstr(szFwU, "SM226") || strstr(szFwU, "SM225") ||
        strstr(szFwU, "SM232"))
        return CONTROLLER_SMI;

    /* ADATA/XPG on Silicon Motion (HDS: SX8200PNP → SM2262EN).
     * PCI VID is ADATA 1CC1, so model/firmware is the fingerprint. */
    if (strstr(szModelU, "SX8200") || strstr(szModelU, "SX8100") ||
        strstr(szModelU, "SX7000") || strstr(szModelU, "GAMMIX S11") ||
        strstr(szFwU, "SANA"))
        return CONTROLLER_SMI;

    if (strstr(szModelU, "MAP120") || strstr(szModelU, "MAP160") ||
        strstr(szModelU, "MAP100") || strstr(szFwU, "MAP16") ||
        strstr(szFwU, "MAP12"))
        return CONTROLLER_MAXIO;

    if (strstr(szModelU, "RTS576") || strstr(szModelU, "RTS577") ||
        strstr(szFwU, "RTS576") || strstr(szFwU, "RTL57"))
        return CONTROLLER_REALTEK;

    if (strstr(szModelU, "IG521") || strstr(szModelU, "IG523") ||
        strstr(szModelU, "INNOGRIT"))
        return CONTROLLER_INNOGRIT;

    /* 3. SATA SMART fingerprint. 0xCA is not Micron-only (Samsung has it). */
    nPhison = CountSmartAttrRange(p, 0xA0, 0xA9);
    if (HasSmartAttr(p, 0xA9) || nPhison >= 3)
        return CONTROLLER_PHISON;
    if (HasSmartAttr(p, 0xE7) && HasSmartAttr(p, 0xE9))
        return CONTROLLER_PHISON;

    if (p->eVendor == VENDOR_SAMSUNG && !hdd)
        return CONTROLLER_SAMSUNG;

    if (HasSmartAttr(p, 0xE1) &&
        (p->eVendor == VENDOR_INTEL || p->eVendor == VENDOR_UNKNOWN))
        return CONTROLLER_INTEL;

    if (HasSmartAttr(p, 0xCA) && p->eVendor == VENDOR_MICRON)
        return CONTROLLER_MICRON;

    /* Kingston A400 and similar: consumer SATA is Phison. */
    if (!hdd && p->eVendor == VENDOR_KINGSTON &&
        (HasSmartAttr(p, 0xE7) || HasSmartAttr(p, 0xE9) ||
         HasSmartAttr(p, 0xA9) || p->eType == DRIVE_TYPE_SSD_SATA ||
         p->eType == DRIVE_TYPE_M2_SATA))
        return CONTROLLER_PHISON;

    /* 4. HDD MCU follows brand. In-house SSD silicon too. */
    if (hdd) {
        switch (p->eVendor) {
        case VENDOR_SEAGATE: return CONTROLLER_SEAGATE;
        case VENDOR_WDC:     return CONTROLLER_WD;
        case VENDOR_TOSHIBA: return CONTROLLER_TOSHIBA;
        case VENDOR_HITACHI: return CONTROLLER_HITACHI;
        case VENDOR_SAMSUNG: return CONTROLLER_SAMSUNG;
        default: break;
        }
    } else if (ssd || !hdd) {
        /* Rebrands (Kingston/ADATA/PNY/…) stay UNKNOWN unless fingerprinted. */
        switch (p->eVendor) {
        case VENDOR_SAMSUNG: return CONTROLLER_SAMSUNG;
        case VENDOR_INTEL:   return CONTROLLER_INTEL;
        case VENDOR_MICRON:  return CONTROLLER_MICRON;
        case VENDOR_SKHYNIX: return CONTROLLER_HYNIX;
        case VENDOR_SANDISK: return CONTROLLER_SANDISK;
        case VENDOR_KIOXIA:  return CONTROLLER_KIOXIA;
        default: break;
        }
    }

    return CONTROLLER_UNKNOWN;
}

/* Model/firmware → controller silicon. OEM PCI VID is the brand, not the ASIC. */
static void ApplySsdPartIds(DRIVE_INFO* p)
{
    static const struct {
        const char* model;   /* substring of uppercase model */
        const char* fw;      /* substring of uppercase firmware, NULL = any */
        DRIVE_CONTROLLER ctl;
        const char* chip;
        const char* nvmeVer; /* "1.4.0" or NULL; used only if Identify VER is 0 */
    } kTab[] = {
        { "MICRON_2400", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2269XT", "1.4.0" },
        { "2400_MTFD", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2269XT", "1.4.0" },
        { "SX8200PNP", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0" },
        { "SX8200 PRO", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0" },
        { "SX8200PRO", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0" },
        { "GAMMIX S11", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0" },
        { "XPG S11", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0" },
        { "SU800", NULL, CONTROLLER_SMI,
          "Silicon Motion SM2258/SM2259", NULL },
        { "GAMMIX S70", NULL, CONTROLLER_INNOGRIT,
          "Innogrit IG5236", "1.4.0" },
        { "P210", "U11", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "BURST", "U11", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "BURST", "U10", CONTROLLER_PHISON,
          "Phison PS3110-S10", NULL },
        { "", "U11", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "", "U10", CONTROLLER_PHISON,
          "Phison PS3110-S10", NULL },
        { "SA400", "SBFK61", CONTROLLER_PHISON,
          "Phison PS3110-S10", NULL },
        { "", "SBFK61", CONTROLLER_PHISON,
          "Phison PS3110-S10", NULL },
        { "", "SBFK62", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "", "SBFK", CONTROLLER_PHISON,
          "Phison PS3110-S10 / PS3111-S11", NULL },
        { "", "SBFM", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "", "S9FM", CONTROLLER_PHISON,
          "Phison PS3109-S9", NULL },
        { "", "T07", CONTROLLER_PHISON,
          "Phison PS3111-S11", NULL },
        { "", "EJFM", CONTROLLER_PHISON,
          "Phison PS5016-E16", "1.3.0" },
    };
    char modelU[48], fwU[16];
    unsigned i;
    if (!p || p->eType == DRIVE_TYPE_HDD)
        return;
    ToUpperCopy(modelU, sizeof(modelU), p->szModel);
    ToUpperCopy(fwU, sizeof(fwU), p->szFirmware);
    for (i = 0; i < sizeof(kTab) / sizeof(kTab[0]); i++) {
        if (kTab[i].model[0] && !strstr(modelU, kTab[i].model))
            continue;
        if (kTab[i].fw && !strstr(fwU, kTab[i].fw))
            continue;
        if (kTab[i].ctl != CONTROLLER_UNKNOWN)
            p->eController = kTab[i].ctl;
        if (kTab[i].chip)
            safe_snprintf(p->szControllerChip, "%s", kTab[i].chip);
        if (kTab[i].nvmeVer && p->bIsNVMe &&
            (p->szProtocol[0] == '\0' || strcmp(p->szProtocol, "NVMe") == 0))
            safe_snprintf(p->szProtocol, "NVMe %s", kTab[i].nvmeVer);
        return;
    }
}

void IdentifyDriveParts(DRIVE_INFO* pInfo)
{
    if (!pInfo) return;
    pInfo->szControllerChip[0] = '\0';
    pInfo->eVendor     = DetectDriveVendor(pInfo->szModel);
    if (pInfo->eVendor == VENDOR_UNKNOWN && pInfo->szFirmware[0]) {
        char szFw[16];
        ToUpperCopy(szFw, sizeof(szFw), pInfo->szFirmware);
        if (strncmp(szFw, "OOS", 3) == 0)
            pInfo->eVendor = VENDOR_UTANIA;
    }
    /* USB dock + old IDENTIFY without word 217 still has HDD SMART. */
    if (pInfo->eType == DRIVE_TYPE_USB && !pInfo->bIsNVMe &&
        pInfo->wRotationRate != 0x0001 &&
        (pInfo->wRotationRate >= 0x0401 ||
         (HasSmartAttr(pInfo, 0xC1) && HasSmartAttr(pInfo, 0x07))))
        pInfo->eType = DRIVE_TYPE_HDD;
    pInfo->eController = DetectDriveController(pInfo);
    ApplySsdPartIds(pInfo);
}

/* ============================================================
 * Device open
 * ============================================================ */
BOOL OpenDrive(int nDrive, HANDLE* phDrive)
{
    char szPath[32];
    safe_snprintf(szPath, "\\\\.\\PhysicalDrive%d", nDrive);

    *phDrive = CreateFileA(szPath,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (*phDrive == INVALID_HANDLE_VALUE) {
        *phDrive = CreateFileA(szPath, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }
    if (*phDrive == INVALID_HANDLE_VALUE) {
        *phDrive = CreateFileA(szPath, 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, 0, NULL);
    }
    return (*phDrive != INVALID_HANDLE_VALUE);
}

BOOL OpenDriveReadOnly(int nDrive, HANDLE* phDrive)
{
    char szPath[32];
    safe_snprintf(szPath, "\\\\.\\PhysicalDrive%d", nDrive);
    *phDrive = CreateFileA(szPath, GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);
    return (*phDrive != INVALID_HANDLE_VALUE);
}

static BOOL GetDiskNumber(HANDLE h, DWORD* pNum)
{
    STORAGE_DEVICE_NUMBER sdn;
    DWORD ret = 0;
    ZeroMemory(&sdn, sizeof(sdn));
    if (!DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER,
                         NULL, 0, &sdn, sizeof(sdn), &ret, NULL))
        return FALSE;
    if (pNum) *pNum = sdn.DeviceNumber;
    return TRUE;
}

static BOOL DiskNumberIsSystem(DWORD nDisk)
{
    char win[MAX_PATH];
    char vol[8];
    HANDLE h;
    DWORD num = (DWORD)-1;
    win[0] = '\0';
    if (!GetWindowsDirectoryA(win, MAX_PATH) || !win[0])
        return FALSE;
    safe_snprintf(vol, "\\\\.\\%c:", win[0]);
    h = CreateFileA(vol, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return FALSE;
    GetDiskNumber(h, &num);
    CloseHandle(h);
    return num == nDisk;
}

static BOOL FindDiskDevInst(DWORD nDisk, DEVINST* pInst)
{
    HDEVINFO hDevInfo;
    SP_DEVICE_INTERFACE_DATA did;
    DWORD idx = 0;
    BOOL found = FALSE;

    if (!pInst) return FALSE;
    *pInst = 0;
    hDevInfo = SetupDiGetClassDevsA(&GUID_DEVINTERFACE_DISK, NULL, NULL,
                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hDevInfo == INVALID_HANDLE_VALUE)
        return FALSE;
    did.cbSize = sizeof(did);
    while (!found && SetupDiEnumDeviceInterfaces(hDevInfo, NULL,
                                                 &GUID_DEVINTERFACE_DISK, idx, &did)) {
        DWORD req = 0;
        SP_DEVINFO_DATA dd;
        SP_DEVICE_INTERFACE_DETAIL_DATA_A* pDetail;
        idx++;
        SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, NULL, 0, &req, NULL);
        if (req == 0) continue;
        pDetail = (SP_DEVICE_INTERFACE_DETAIL_DATA_A*)HeapAlloc(
            GetProcessHeap(), HEAP_ZERO_MEMORY, req);
        if (!pDetail) continue;
        pDetail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_A);
        dd.cbSize = sizeof(dd);
        if (SetupDiGetDeviceInterfaceDetailA(hDevInfo, &did, pDetail, req, &req, &dd)) {
            HANDLE hTest = CreateFileA(pDetail->DevicePath, 0,
                FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
            if (hTest != INVALID_HANDLE_VALUE) {
                DWORD num = (DWORD)-1;
                if (GetDiskNumber(hTest, &num) && num == nDisk) {
                    *pInst = dd.DevInst;
                    found = TRUE;
                }
                CloseHandle(hTest);
            }
        }
        HeapFree(GetProcessHeap(), 0, pDetail);
    }
    SetupDiDestroyDeviceInfoList(hDevInfo);
    return found;
}

static BOOL BufHasI(const char* buf, ULONG len, const char* tok)
{
    ULONG i, j;
    size_t tlen;
    if (!buf || !tok) return FALSE;
    tlen = strlen(tok);
    if (tlen == 0 || len < tlen) return FALSE;
    for (i = 0; i + (ULONG)tlen <= len; i++) {
        for (j = 0; j < (ULONG)tlen; j++) {
            char a = buf[i + j];
            char b = tok[j];
            if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'z') b = (char)(b - 'a' + 'A');
            if (a != b) break;
        }
        if (j == (ULONG)tlen) return TRUE;
    }
    return FALSE;
}

static int StackRank(DRIVE_STACK_KIND k)
{
    switch (k) {
    case DRIVE_STACK_INTEL_RST: return 6;
    case DRIVE_STACK_RAID:      return 5;
    case DRIVE_STACK_VIRT:      return 4;
    case DRIVE_STACK_NVME:      return 3;
    case DRIVE_STACK_USB:       return 2;
    case DRIVE_STACK_ATA:       return 1;
    default:                    return 0;
    }
}

/* One devnode. Service and hardware id only — a product string
 * "NVMe Phison" must not classify the disk as the NVMe driver. */
static DRIVE_STACK_KIND KindFromText(const char* buf, ULONG len,
                                     BOOL* pMini, BOOL* pFullIdent)
{
    if (BufHasI(buf, len, "iaStor") ||
        BufHasI(buf, len, "DEV_A77F") ||
        BufHasI(buf, len, "DEV_09AB") ||
        BufHasI(buf, len, "RST VMD") ||
        BufHasI(buf, len, "Intel RST"))
        return DRIVE_STACK_INTEL_RST;
    /* VROC, AMD-RAID, MegaRAID, LSI/Broadcom, HighPoint, Marvell, NVIDIA.
     * ATA and SCSI passthrough bugcheck these. */
    if (BufHasI(buf, len, "iaVROC") || BufHasI(buf, len, "VROC") ||
        BufHasI(buf, len, "rcraid") || BufHasI(buf, len, "AMD-RAID") ||
        BufHasI(buf, len, "megasas") || BufHasI(buf, len, "MegaRAID") ||
        BufHasI(buf, len, "mpt3sas") || BufHasI(buf, len, "mpt2sas") ||
        BufHasI(buf, len, "lsi_sas") || BufHasI(buf, len, "smartpqi") ||
        BufHasI(buf, len, "hpcisss") || BufHasI(buf, len, "arcsas") ||
        BufHasI(buf, len, "nvraid") || BufHasI(buf, len, "SiSRaid") ||
        BufHasI(buf, len, "mvraid") || BufHasI(buf, len, "mvsas") ||
        BufHasI(buf, len, "vsmraid") || BufHasI(buf, len, "iaRNVMe"))
        return DRIVE_STACK_RAID;
    if (BufHasI(buf, len, "storvsc") || BufHasI(buf, len, "vhdmp") ||
        BufHasI(buf, len, "spaceport"))
        return DRIVE_STACK_VIRT;
    if (BufHasI(buf, len, "stornvme")) {
        if (pMini) *pMini = TRUE;
        if (pFullIdent) *pFullIdent = TRUE;
        return DRIVE_STACK_NVME;
    }
    if (BufHasI(buf, len, "secnvme")) {
        if (pFullIdent) *pFullIdent = TRUE;
        return DRIVE_STACK_NVME;
    }
    if (BufHasI(buf, len, "usbstor") || BufHasI(buf, len, "uaspstor") ||
        BufHasI(buf, len, "uasp"))
        return DRIVE_STACK_USB;
    if (BufHasI(buf, len, "storahci") || BufHasI(buf, len, "msahci") ||
        BufHasI(buf, len, "atapi") || BufHasI(buf, len, "pciide") ||
        BufHasI(buf, len, "intelide") || BufHasI(buf, len, "amdsata") ||
        BufHasI(buf, len, "viaide") || BufHasI(buf, len, "aliide") ||
        BufHasI(buf, len, "cmdide"))
        return DRIVE_STACK_ATA;
    return DRIVE_STACK_UNKNOWN;
}

static void NoteDevNode(DEVINST inst, DRIVE_STACK_KIND* pBest,
                        BOOL* pMini, BOOL* pFullIdent)
{
    char buf[1024];
    ULONG len, prop;
    static const ULONG kProps[] = {
        CM_DRP_SERVICE, CM_DRP_HARDWAREID, CM_DRP_DEVICEDESC, CM_DRP_FRIENDLYNAME
    };
    for (prop = 0; prop < sizeof(kProps) / sizeof(kProps[0]); prop++) {
        DRIVE_STACK_KIND k;
        len = sizeof(buf);
        ZeroMemory(buf, sizeof(buf));
        if (CM_Get_DevNode_Registry_PropertyA(inst, kProps[prop], NULL,
                buf, &len, 0) != CR_SUCCESS)
            continue;
        if (len > sizeof(buf)) len = sizeof(buf);
        k = KindFromText(buf, len, pMini, pFullIdent);
        if (StackRank(k) > StackRank(*pBest))
            *pBest = k;
    }
}

static int s_stackNum = -2;
static DRIVE_STACK_KIND s_stackKind = DRIVE_STACK_UNKNOWN;
static BOOL s_stackMini = FALSE;
static BOOL s_stackFullIdent = FALSE;

DRIVE_STACK_KIND DriveStackKind(HANDLE hDrive)
{
    DWORD nDisk = (DWORD)-1;
    DEVINST inst = 0;
    DRIVE_STACK_KIND best = DRIVE_STACK_UNKNOWN;
    BOOL mini = FALSE, full = FALSE;
    BYTE bus;
    int walk;

    if (!GetDiskNumber(hDrive, &nDisk))
        return DRIVE_STACK_UNKNOWN;
    if ((int)nDisk == s_stackNum)
        return s_stackKind;
    if (FindDiskDevInst(nDisk, &inst)) {
        for (walk = 0; walk < 16 && inst; walk++) {
            DEVINST parent = 0;
            NoteDevNode(inst, &best, &mini, &full);
            if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS)
                break;
            inst = parent;
        }
    }
    bus = GetStorageBusType(hDrive);
    if (best == DRIVE_STACK_UNKNOWN) {
        if (bus == 7)
            best = DRIVE_STACK_USB;
        else if (bus == 17)
            best = DRIVE_STACK_NVME;
        else if (bus == 8)
            best = DRIVE_STACK_RAID;
        else if (bus == 14 || bus == 15 || bus == 16)
            best = DRIVE_STACK_VIRT;
        else if (bus == 11 || bus == 3 || bus == 2)
            best = DRIVE_STACK_ATA;
    }
    s_stackNum = (int)nDisk;
    s_stackKind = best;
    s_stackMini = mini;
    s_stackFullIdent = full;
    return s_stackKind;
}

BOOL DriveBehindIntelRst(HANDLE hDrive)
{
    return DriveStackKind(hDrive) == DRIVE_STACK_INTEL_RST;
}

BOOL DriveAllowsAtaIoctl(HANDLE hDrive)
{
    return DriveStackKind(hDrive) == DRIVE_STACK_ATA;
}

BOOL DriveAllowsScsiPassthrough(HANDLE hDrive)
{
    return DriveStackKind(hDrive) == DRIVE_STACK_USB;
}

BOOL DriveAllowsNvmeProtocol(HANDLE hDrive)
{
    DRIVE_STACK_KIND k = DriveStackKind(hDrive);
    return k != DRIVE_STACK_INTEL_RST && k != DRIVE_STACK_USB;
}

BOOL DriveAllowsNvmeMini(HANDLE hDrive)
{
    if (DriveStackKind(hDrive) != DRIVE_STACK_NVME)
        return FALSE;
    return s_stackMini;
}

BOOL DriveAllowsFullNvmeIdentify(HANDLE hDrive)
{
    if (DriveStackKind(hDrive) != DRIVE_STACK_NVME)
        return FALSE;
    return s_stackFullIdent;
}

static BOOL DismountDiskVolumes(DWORD nDisk)
{
    char letters[256];
    char* p;
    int nOk = 0;
    letters[0] = '\0';
    if (!GetLogicalDriveStringsA((DWORD)sizeof(letters), letters))
        return FALSE;
    for (p = letters; *p; p += strlen(p) + 1) {
        char vol[8];
        HANDLE h;
        DWORD num = (DWORD)-1;
        int tries;
        safe_snprintf(vol, "\\\\.\\%c:", p[0]);
        h = CreateFileA(vol, GENERIC_READ | GENERIC_WRITE,
                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            h = CreateFileA(vol, GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE)
            continue;
        if (!GetDiskNumber(h, &num) || num != nDisk) {
            CloseHandle(h);
            continue;
        }
        for (tries = 0; tries < 8; tries++) {
            DWORD dummy = 0;
            if (DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &dummy, NULL))
                break;
            Sleep(250);
        }
        {
            DWORD dummy = 0;
            DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &dummy, NULL);
        }
        CloseHandle(h);
        nOk++;
    }
    return nOk > 0;
}

static BOOL TryEjectDevInst(DEVINST inst)
{
    if (CM_Request_Device_EjectA(inst, NULL, NULL, 0, 0) == CR_SUCCESS)
        return TRUE;
    return FALSE;
}

BOOL SafeEjectPhysicalDrive(int nDrive, char* szErr, int nErrLen)
{
    HANDLE hDisk;
    DWORD nDisk = (DWORD)-1;
    DEVINST inst = 0;
    int walk;
    BOOL ejected = FALSE;

    if (szErr && nErrLen > 0) szErr[0] = '\0';
    if (!OpenDrive(nDrive, &hDisk)) {
        if (szErr) safe_snprintf_n(szErr, nErrLen, "Не удалось открыть диск.");
        return FALSE;
    }
    if (!GetDiskNumber(hDisk, &nDisk)) {
        CloseHandle(hDisk);
        if (szErr) safe_snprintf_n(szErr, nErrLen, "Не удалось определить номер диска.");
        return FALSE;
    }
    if (DiskNumberIsSystem(nDisk)) {
        CloseHandle(hDisk);
        if (szErr) safe_snprintf_n(szErr, nErrLen,
            "Это системный диск Windows. Его нельзя извлечь.");
        return FALSE;
    }
    DismountDiskVolumes(nDisk);
    {
        DWORD dummy = 0;
        DeviceIoControl(hDisk, IOCTL_STORAGE_EJECT_MEDIA,
                        NULL, 0, NULL, 0, &dummy, NULL);
    }
    CloseHandle(hDisk);

    if (!FindDiskDevInst(nDisk, &inst) || inst == 0) {
        if (szErr) safe_snprintf_n(szErr, nErrLen,
            "Тома размонтированы, но устройство в дереве PnP не найдено.");
        return FALSE;
    }
    for (walk = 0; walk < 16 && inst; walk++) {
        DEVINST parent = 0;
        if (TryEjectDevInst(inst)) {
            ejected = TRUE;
            break;
        }
        if (CM_Get_Parent(&parent, inst, 0) != CR_SUCCESS)
            break;
        inst = parent;
    }
    if (!ejected) {
        if (szErr) safe_snprintf_n(szErr, nErrLen,
            "Windows не отключил устройство. Закройте файлы на диске и повторите.");
        return FALSE;
    }
    return TRUE;
}

BYTE GetStorageBusType(HANDLE hDrive)
{
    STORAGE_PROPERTY_QUERY spq;
    BYTE outBuf[1024];
    DWORD dwBytes = 0;

    /* Device descriptor BusType is the drive itself (17 = NVMe).
     * The old code read byte 8 of STORAGE_ADAPTER_DESCRIPTOR, which is
     * MaximumTransferLength, not BusType (offset 24). That made native
     * NVMe look like SCSI/USB and skip the NVMe SMART path. */
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;
    ZeroMemory(outBuf, sizeof(outBuf));
    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                        &spq, sizeof(spq), outBuf, sizeof(outBuf),
                        &dwBytes, NULL) &&
        dwBytes >= offsetof(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(BYTE)) {
        STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;
        return (BYTE)pDesc->BusType;
    }

    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageAdapterProperty;
    spq.QueryType  = PropertyStandardQuery;
    ZeroMemory(outBuf, sizeof(outBuf));
    dwBytes = 0;
    if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                        &spq, sizeof(spq), outBuf, sizeof(outBuf),
                        &dwBytes, NULL) && dwBytes >= 25)
        return outBuf[24];

    return 0;
}

BOOL IsUSBDrive(HANDLE hDrive)
{
    return (GetStorageBusType(hDrive) == 7);
}

BOOL IsEMMCDrive(HANDLE hDrive)
{
    BYTE bus = GetStorageBusType(hDrive);
    return (bus == 12 || bus == 13);
}

BOOL IsNVMeDrive(HANDLE hDrive)
{
    BYTE bBusType = GetStorageBusType(hDrive);
    if (bBusType == 17) return TRUE;

    /* Bus 8 = RAID (Intel RST/VMD often owns NVMe). Still try a safe
     * Health Info Log query — if it works, this is NVMe. */
    {
        BYTE health[sizeof(NVME_HEALTH_INFO_LOG)];
        DWORD nCopied = 0;
        ZeroMemory(health, sizeof(health));
        if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeLogPage, NVME_LOG_PAGE_HEALTH_INFO,
                              health, sizeof(health), &nCopied))
            return TRUE;
    }
    {
        BYTE ident[256];
        DWORD nCopied = 0;
        ZeroMemory(ident, sizeof(ident));
        if (QueryNVMeProtocol(hDrive, MY_NVMeDataTypeIdentify, 1,
                              ident, sizeof(ident), &nCopied))
            return TRUE;
    }

    /* For USB drives (bus type 7), check if this might be NVMe-over-USB.
     * product name contains "NVMe", OR the bridge
     * chip is a known NVMe-over-USB bridge (JMicron JMS583/586, ASMedia
     * ASM2362, Realtek RTL9210, VLI VL716/VL717).
     *
     * Many USB-NVMe enclosures DON'T include "NVMe" in their product string,
     * so we also check the bridge chip VID/PID. */
    if (bBusType == 7) {
        /* First check product name for "NVMe" keyword */
        STORAGE_PROPERTY_QUERY spqDev;
        ZeroMemory(&spqDev, sizeof(spqDev));
        spqDev.PropertyId = StorageDeviceProperty;
        spqDev.QueryType  = PropertyStandardQuery;

        BYTE devBuf[1024];
        ZeroMemory(devBuf, sizeof(devBuf));
        DWORD dwDevBytes = 0;

        if (DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                            &spqDev, sizeof(spqDev), devBuf, sizeof(devBuf),
                            &dwDevBytes, NULL) && dwDevBytes > 32) {
            STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)devBuf;

            /* Check product name for NVMe keyword */
            if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pStr = (const char*)devBuf + pDesc->ProductIdOffset;
                if (strstr(pStr, "NVMe") || strstr(pStr, "NVME") ||
                    strstr(pStr, "nvme"))
                    return TRUE;
            }

            /* Check VID/PID for known NVMe-over-USB bridges.
             * This is critical: many USB-NVMe enclosures don't include
             * "NVMe" in their product string, so VID/PID matching is
             * the primary detection method. */
            WORD wVid = 0, wPid = 0;
            DRIVE_INFO tempInfo;
            ZeroMemory(&tempInfo, sizeof(tempInfo));

            /* Try extracting VID/PID from device descriptor strings */
            if (pDesc->VendorIdOffset && pDesc->VendorIdOffset < dwDevBytes) {
                const char* pVidStr = (const char*)devBuf + pDesc->VendorIdOffset;
                ParseVidPidFromHardwareId(pVidStr, &wVid, &wPid);
            }
            if (wVid == 0 && wPid == 0 &&
                pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pPidStr = (const char*)devBuf + pDesc->ProductIdOffset;
                ParseVidPidFromHardwareId(pPidStr, &wVid, &wPid);
            }

            /* If we got VID/PID from descriptor strings, check for NVMe bridges */
            if (wVid != 0 || wPid != 0) {
                /* JMicron NVMe: VID 152D, PID 0583/0586/058C/058F */
                if (wVid == 0x152D && (wPid == 0x0583 || wPid == 0x0586 ||
                                       wPid == 0x058C || wPid == 0x058F))
                    return TRUE;

                /* ASMedia NVMe: VID 174C, PID 2362/2364 */
                if (wVid == 0x174C && (wPid == 0x2362 || wPid == 0x2364))
                    return TRUE;

                /* Realtek NVMe: VID 0BDA, PID 9210/9220/9221 */
                if (wVid == 0x0BDA && (wPid == 0x9210 || wPid == 0x9220 || wPid == 0x9221))
                    return TRUE;

                /* VLI NVMe: VID 2109, PID 0900/0901/0902 */
                if (wVid == 0x2109 && (wPid == 0x0900 || wPid == 0x0901 || wPid == 0x0902))
                    return TRUE;
            }

            /* Also check product name for known NVMe bridge model numbers */
            if (pDesc->ProductIdOffset && pDesc->ProductIdOffset < dwDevBytes) {
                const char* pProd = (const char*)devBuf + pDesc->ProductIdOffset;
                char szUpper[65];
                int k;
                for (k = 0; k < 64 && pProd[k]; k++)
                    szUpper[k] = (char)toupper((unsigned char)pProd[k]);
                szUpper[k] = '\0';

                if (strstr(szUpper, "JMS583") || strstr(szUpper, "JMS586") ||
                    strstr(szUpper, "ASM2362") || strstr(szUpper, "RTL9210") ||
                    strstr(szUpper, "VL716") || strstr(szUpper, "VL717") ||
                    strstr(szUpper, "NL6221"))
                    return TRUE;
            }

            /* Vendor names like "JMicron" also appear on SATA bridges, so a
             * vendor-only match is not enough to claim NVMe. Product-id
             * checks above already cover the known NVMe bridge chips. */
        }
    }
    return FALSE;
}

/* ============================================================
 * Generic STORAGE_DEVICE_DESCRIPTOR reader
 * ============================================================ */
BOOL GetDeviceDescriptor(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    STORAGE_PROPERTY_QUERY spq;
    ZeroMemory(&spq, sizeof(spq));
    spq.PropertyId = StorageDeviceProperty;
    spq.QueryType  = PropertyStandardQuery;

    BYTE outBuf[1024];
    ZeroMemory(outBuf, sizeof(outBuf));
    DWORD dwBytes = 0;

    if (!DeviceIoControl(hDrive, IOCTL_STORAGE_QUERY_PROPERTY,
                         &spq, sizeof(spq), outBuf, sizeof(outBuf),
                         &dwBytes, NULL))
        return FALSE;

    if (dwBytes < sizeof(STORAGE_DEVICE_DESCRIPTOR))
        return FALSE;

    STORAGE_DEVICE_DESCRIPTOR* pDesc = (STORAGE_DEVICE_DESCRIPTOR*)outBuf;

    if (pInfo->szModel[0] == '\0')
        CopyDescStr(pInfo->szModel, sizeof(pInfo->szModel),
                    outBuf, dwBytes, pDesc->ProductIdOffset);
    if (pInfo->szSerial[0] == '\0')
        CopyDescStr(pInfo->szSerial, sizeof(pInfo->szSerial),
                    outBuf, dwBytes, pDesc->SerialNumberOffset);
    if (pInfo->szFirmware[0] == '\0')
        CopyDescStr(pInfo->szFirmware, sizeof(pInfo->szFirmware),
                    outBuf, dwBytes, pDesc->ProductRevisionOffset);
    return TRUE;
}

BOOL GetCapacityFromGeometry(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    DISK_GEOMETRY_EX geo;
    ZeroMemory(&geo, sizeof(geo));
    DWORD dwBytes = 0;
    if (DeviceIoControl(hDrive, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                        NULL, 0, &geo, sizeof(geo), &dwBytes, NULL)) {
        DWORD dwCapMB = (DWORD)(geo.DiskSize.QuadPart / (1024 * 1024));
        if (dwCapMB > pInfo->dwCapacityMB) pInfo->dwCapacityMB = dwCapMB;
        return TRUE;
    }
    return FALSE;
}

/* ============================================================
 * Drive type detection
 * ============================================================ */
DRIVE_TYPE DetectDriveType(HANDLE hDrive, DRIVE_INFO* pInfo)
{
    if (pInfo->bIsNVMe) return DRIVE_TYPE_NVME;

    BYTE bus = GetStorageBusType(hDrive);
    if (bus == 12 || bus == 13) return DRIVE_TYPE_EMMC;
    if (bus == 10) return DRIVE_TYPE_SCSI;
    /* bus 11 = SATA: HDD vs SSD decided by rotation rate below */
    if (bus == 7  && !pInfo->bSMART_Supported) return DRIVE_TYPE_USB;

    /* Use stored rotation rate from IDENTIFY if available */
    if (pInfo->wRotationRate == 0x0001) {
        return DRIVE_TYPE_SSD_SATA;
    }
    if (pInfo->wRotationRate >= 0x0401) {
        return DRIVE_TYPE_HDD;
    }

    /* Try IDENTIFY to extract rotation rate.
     * iaStorVD bugchecks 0x139 on these IOCTLs. */
    if (DriveAllowsAtaIoctl(hDrive)) {
    BYTE ident[IDENTIFY_BUFFER_SIZE];
    BOOL bGotIdent = FALSE;
    ZeroMemory(ident, sizeof(ident));

    BYTE inBuf[sizeof(SENDCMDINPARAMS) - 1 + IDENTIFY_BUFFER_SIZE];
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

    if (DeviceIoControl(hDrive, SMART_RCV_DRIVE_DATA,
            pCip, sizeof(SENDCMDINPARAMS) - 1,
            outBuf, sizeof(outBuf), &dwBytes, NULL)) {
        SENDCMDOUTPARAMS* pCop = (SENDCMDOUTPARAMS*)outBuf;
        memcpy(ident, pCop->bBuffer, IDENTIFY_BUFFER_SIZE);
        bGotIdent = !IsBufferAllZero(ident, 64);
    }
    if (!bGotIdent) {
        if (ATAPassThrough(hDrive, ID_CMD, 0, 1, 0, 0, 0, 0xA0,
                           ident, IDENTIFY_BUFFER_SIZE, TRUE))
            bGotIdent = !IsBufferAllZero(ident, 64);
    }

    if (bGotIdent) {
        WORD* p = (WORD*)ident;
        WORD wRot = p[217];
        pInfo->wRotationRate = wRot;
        WORD wForm = p[168];
        if (wRot == 0x0001) {
            if (wForm == 0x0003 || wForm == 0x0005) return DRIVE_TYPE_M2_SATA;
            return DRIVE_TYPE_SSD_SATA;
        }
        if (wRot >= 0x0401) return DRIVE_TYPE_HDD;
    }
    }

    /* Heuristic from model name */
    const char* m = pInfo->szModel;
    if (m[0]) {
        if (strstr(m, "SSD") || strstr(m, "Solid") || strstr(m, "SOLID") ||
            strstr(m, "FLASH") || strstr(m, "Flash") || strstr(m, "flash") ||
            strstr(m, "MX") || strstr(m, "860") || strstr(m, "870") ||
            strstr(m, "BX500") || strstr(m, "EVO") || strstr(m, "PRO"))
            return DRIVE_TYPE_SSD_SATA;
    }

    if (bus == 7) {
        /* Old HDDs often omit IDENTIFY word 217. Don't call them "USB". */
        if (m[0]) {
            char u[48];
            int i;
            for (i = 0; i < 47 && m[i]; i++)
                u[i] = (char)toupper((unsigned char)m[i]);
            u[i] = '\0';
            if (!(strstr(u, "SSD") || strstr(u, "NVME") || strstr(u, "WDS"))) {
                if (u[0] == 'S' && u[1] == 'T' && u[2] >= '0' && u[2] <= '9')
                    return DRIVE_TYPE_HDD;
                if (strstr(u, "BEVS") || strstr(u, "BEVT") || strstr(u, "BPVT") ||
                    strstr(u, "LPCX") || strstr(u, "EZBX") || strstr(u, "EZRX"))
                    return DRIVE_TYPE_HDD;
            }
        }
        return DRIVE_TYPE_USB;
    }
    return DRIVE_TYPE_HDD;
}

int ScanDrives(DRIVE_INFO* pDrives, int nMaxDrives)
{
    int nFound = 0;
    int nDrive;
    const int nScanLimit = 32;

    for (nDrive = 0; nDrive < nScanLimit && nFound < nMaxDrives; nDrive++) {
        HANDLE hDrive;
        if (!OpenDrive(nDrive, &hDrive)) continue;

        DRIVE_INFO* pInfo = &pDrives[nFound];
        ZeroMemory(pInfo, sizeof(DRIVE_INFO));
        pInfo->nDriveIndex    = nDrive;
        pInfo->nTemperatureC  = -1;
        pInfo->nTempMaxC      = -1;
        pInfo->nTempMinC      = -1;
        pInfo->nTempWarnC     = -1;
        pInfo->nTempCritC     = -1;
        pInfo->nHealthPercent = -1;
        pInfo->nConfidence    = 0;
        pInfo->nEndurancePercent = -1;
        pInfo->eReliability   = HEALTH_STATUS_UNKNOWN;
        pInfo->eInterface     = HEALTH_STATUS_UNKNOWN;
        pInfo->eTempStatus    = HEALTH_STATUS_UNKNOWN;
        pInfo->nReallocated   = -1;
        pInfo->nPendingSectors = -1;
        pInfo->nUncorrectable = -1;
        pInfo->nRemapEvents   = -1;
        pInfo->nCrcErrors     = -1;
        pInfo->eTempBand      = TEMP_BAND_UNKNOWN;
        pInfo->eNormQuality   = NORM_QUALITY_UNKNOWN;
        pInfo->eRawQuality    = NORM_QUALITY_UNKNOWN;
        pInfo->bThresholdViolation = FALSE;
        pInfo->bPrefailNow = FALSE;
        pInfo->bPrefailPast = FALSE;
        pInfo->bUsageFailed = FALSE;
        pInfo->szEvidence[0]  = '\0';
        pInfo->eType          = DRIVE_TYPE_UNKNOWN;
        pInfo->eAccessMethod  = SMART_ACCESS_NONE;
        pInfo->eHealthStatus  = HEALTH_STATUS_UNKNOWN;
        pInfo->eVendor        = VENDOR_UNKNOWN;
        pInfo->eController    = CONTROLLER_UNKNOWN;
        pInfo->nSSDLifeLeft   = -1;
        pInfo->nSSDTotalWritesGB = -1;
        pInfo->nSSDAvgEraseCount = -1;
        pInfo->nSSDMaxEraseCount = -1;
        pInfo->nSSDMinEraseCount = -1;
        pInfo->nSSDWearLevelingCount = -1;
        {
            int ts;
            for (ts = 0; ts < 8; ts++) pInfo->nTempSensor[ts] = -1;
        }

        BYTE busType = GetStorageBusType(hDrive);

        /* Intel RST VMD (iaStorVD, DEV_A77F): ATA/SCSI passthrough and a
         * 4096-byte NVMe protocol query bugcheck 0x139. IntelNvm only. */
        if (DriveBehindIntelRst(hDrive)) {
            char modelU[80];
            int mi;
            GetNVMeInfo(hDrive, pInfo);
            if (pInfo->szModel[0] == '\0') GetDeviceDescriptor(hDrive, pInfo);
            if (pInfo->dwCapacityMB == 0)  GetCapacityFromGeometry(hDrive, pInfo);
            for (mi = 0; mi < 79 && pInfo->szModel[mi]; mi++)
                modelU[mi] = (char)toupper((unsigned char)pInfo->szModel[mi]);
            modelU[mi] = '\0';
            if (!pInfo->bIsNVMe &&
                (busType == 17 || strstr(modelU, "NVME"))) {
                pInfo->bIsNVMe = TRUE;
                pInfo->eType = DRIVE_TYPE_NVME;
            }
            if (!pInfo->bIsNVMe) {
                pInfo->bSMART_Supported = FALSE;
                pInfo->eType = DRIVE_TYPE_UNKNOWN;
            }
            AssessDriveHealth(pInfo);
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);
            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* --------------------- NVMe (not USB bridges) --------------------- */
        if (IsNVMeDrive(hDrive) && busType != 7) {
            GetNVMeInfo(hDrive, pInfo);

            if (pInfo->szModel[0] == '\0') GetDeviceDescriptor(hDrive, pInfo);
            if (pInfo->dwCapacityMB == 0)  GetCapacityFromGeometry(hDrive, pInfo);

            pInfo->eType               = DRIVE_TYPE_NVME;
            pInfo->bIsNVMe             = TRUE;
            AssessDriveHealth(pInfo);
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);

            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* --------------------- eMMC / SD --------------------- */
        if (busType == 12 || busType == 13) {
            GetDeviceDescriptor(hDrive, pInfo);
            GetCapacityFromGeometry(hDrive, pInfo);
            pInfo->eType = (busType == 13) ? DRIVE_TYPE_SD : DRIVE_TYPE_EMMC;
            pInfo->bSMART_Supported = FALSE;
            pInfo->nHealthPercent   = -1;
            pInfo->eHealthStatus    = HEALTH_STATUS_UNKNOWN;
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);
            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* RAID, VROC, Storage Spaces, Hyper-V, or an unknown stack
         * that is not inbox AHCI and not USB. Descriptor only.
         * ATA/SCSI passthrough bugchecks these drivers (0x139 and kin). */
        if (!DriveAllowsAtaIoctl(hDrive) &&
            !DriveAllowsScsiPassthrough(hDrive)) {
            GetDeviceDescriptor(hDrive, pInfo);
            GetCapacityFromGeometry(hDrive, pInfo);
            pInfo->bSMART_Supported = FALSE;
            if (busType == 17) {
                pInfo->bIsNVMe = TRUE;
                pInfo->eType = DRIVE_TYPE_NVME;
            } else if (busType == 10) {
                pInfo->eType = DRIVE_TYPE_SCSI;
            } else {
                pInfo->eType = DRIVE_TYPE_UNKNOWN;
            }
            AssessDriveHealth(pInfo);
            IdentifyDriveParts(pInfo);
            FillDriveProtocol(pInfo);
            CloseHandle(hDrive);
            nFound++;
            continue;
        }

        /* --------------------- ATA / USB / SAS --------------------- */
        /* ATA identify only on inbox AHCI. USB starts at SAT. */
        BOOL bIdentOK = FALSE;

        if (DriveAllowsAtaIoctl(hDrive)) {
            if (GetIdentifyDataATAPassthrough(hDrive, pInfo)) bIdentOK = TRUE;
            if (!bIdentOK && GetIdentifyData(hDrive, nDrive, pInfo)) bIdentOK = TRUE;
        }
        if (!bIdentOK && DriveAllowsScsiPassthrough(hDrive)) {
            /* Name the stick from the descriptor before any SAT CDB.
             * JetFlash / SanDisk UFD then never see ATA-over-SCSI. */
            if (busType == 7) {
                pInfo->bIsUSB = TRUE;
                if (pInfo->szModel[0] == '\0')
                    GetDeviceDescriptor(hDrive, pInfo);
                if (IsLikelyUsbFlashDrive(pInfo)) {
                    bIdentOK = (pInfo->szModel[0] != '\0');
                    pInfo->bSMART_Supported = FALSE;
                }
            }
            if (!bIdentOK && GetIdentifyDataSAT(hDrive, pInfo)) {
                bIdentOK = TRUE;
                if (busType == 7) pInfo->bIsUSB = TRUE;
            }
        }
        if (!bIdentOK) {
            bIdentOK = GetIdentifyDataUSB(hDrive, pInfo);
            if (!bIdentOK) {
                CloseHandle(hDrive);
                continue;
            }
        }

        if (pInfo->szModel[0] == '\0' || pInfo->szSerial[0] == '\0')
            GetDeviceDescriptor(hDrive, pInfo);
        if (pInfo->dwCapacityMB == 0)
            GetCapacityFromGeometry(hDrive, pInfo);

        if (busType == 7 && !pInfo->bIsUSB) pInfo->bIsUSB = TRUE;
        pInfo->eType = DetectDriveType(hDrive, pInfo);

        IdentifyDriveParts(pInfo);

        if (pInfo->bIsUSB) {
            GetBridgeIdentity(hDrive, pInfo);
            GetUSBVidPid(hDrive, &pInfo->wUsbVid, &pInfo->wUsbPid);
            pInfo->eUsbBridgeType = DetectUsbBridgeType(hDrive, pInfo);

            /* If no VID/PID from SetupDi, try heuristic detection from
             * the bridge vendor/product strings in STORAGE_DEVICE_DESCRIPTOR.
             * uses this as a fallback. */
            if (pInfo->wUsbVid == 0 && pInfo->wUsbPid == 0) {
                /* Try to detect NVMe bridge from product name */
                char szUpper[17];
                int k;
                for (k = 0; k < 16 && pInfo->szBridgeProduct[k]; k++)
                    szUpper[k] = (char)toupper((unsigned char)pInfo->szBridgeProduct[k]);
                szUpper[k] = '\0';

                char szUpperV[9];
                for (k = 0; k < 8 && pInfo->szBridgeVendor[k]; k++)
                    szUpperV[k] = (char)toupper((unsigned char)pInfo->szBridgeVendor[k]);
                szUpperV[k] = '\0';

                /* JMicron NVMe bridges often show "JMS583" in product ID */
                if (strstr(szUpper, "JMS583") || strstr(szUpper, "JMS586") ||
                    strstr(szUpper, "0583")   || strstr(szUpper, "0586"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_JMICRON;

                /* ASMedia NVMe bridges show "ASM2362" in product ID */
                else if (strstr(szUpper, "ASM2362") || strstr(szUpper, "2362"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_ASMEDIA;

                /* Realtek NVMe bridges show "RTL9210" in product ID */
                else if (strstr(szUpper, "RTL9210") || strstr(szUpper, "9210"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_REALTEK;

                /* VLI NVMe bridges show "VL716" or "VL717" */
                else if (strstr(szUpper, "VL716") || strstr(szUpper, "VL717"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_VLI;

                /* Also check the T10 vendor ID for known NVMe bridge makers */
                else if (strstr(szUpperV, "JMICRON"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_JMICRON;
                else if (strstr(szUpperV, "ASMEDIA"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_ASMEDIA;
                else if (strstr(szUpperV, "REALTEK"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_REALTEK;
                else if (strstr(szUpperV, "VLI"))
                    pInfo->eUsbBridgeType = USB_BRIDGE_NVME_VLI;
            }
        }

        /* SMART data — multi-path acquisition */
        /* Skip SAT / vendor NVMe-over-USB retries on likely USB flash.
         * Still list the stick with model/capacity. Do not send Realtek 0xE4
         * or SAT to SanDisk/Kingston UFD on every refresh. */
        if (pInfo->bIsUSB && !pInfo->bIsNVMe && IsLikelyUsbFlashDrive(pInfo)) {
            pInfo->bSMART_Supported = FALSE;
        }
        else if (pInfo->bIsUSB && !pInfo->bIsNVMe) {
            /* ---- USB drive SMART acquisition ---- */
            USB_BRIDGE_TYPE bridge = pInfo->eUsbBridgeType;

            if (IsRealtekNvmeUsbBridge(pInfo) ||
                bridge == USB_BRIDGE_NVME_REALTEK ||
                bridge == USB_BRIDGE_NVME_FMA) {
                /* RTL9210B dual-mode (SATA via SAT, NVMe via 0xE4).
                 * SAT first: this enclosure often holds a SATA SSD;
                 * 0xE4 on SATA is useless. One-shot 0xE4 only if SAT
                 * SMART failed. Stay USB: no native NVMe IOCTL on this
                 * handle. Do NOT TryAll, GetNVMeHealthLogEx,
                 * or IOCTL_STORAGE_PROTOCOL_COMMAND. */
                BOOL got = FALSE;

                /* 1) SATA behind RTL9210: AcquireATASMART is SAT-first on USB.
                 *    Earlier GetIdentifyDataSAT may already have the model;
                 *    still fetch SMART attributes. */
                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                    /* ExtractTemperature/counters/SSD indicators:
                     * already inside AcquireATASMART. */
                }

                /* 2) NVMe behind RTL9210: one-shot vendor 0xE4 */
                if (!got) {
                    if (NVMeIdentifyRealtek(hDrive, pInfo) &&
                        NVMeHealthLogRealtek(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_JMICRON) {
                /* JMS583/586 dual-mode (SATA via SAT, NVMe via vendor).
                 * SAT first like RTL9210: enclosure may hold a SATA SSD.
                 * One-shot JMicron identify+health only if SAT SMART failed.
                 * Stay USB: no native NVMe IOCTL on this handle.
                 * Do NOT TryAll, GetNVMeHealthLogEx, or NvmeMini. */
                BOOL got = FALSE;

                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                }

                if (!got) {
                    if (NVMeIdentifyJMicron(hDrive, pInfo) &&
                        NVMeHealthLogJMicron(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_ASMEDIA) {
                /* ASM2362 dual-mode (SATA via SAT, NVMe via vendor).
                 * SAT first like RTL9210. One-shot ASMedia identify+health
                 * only if SAT SMART failed. Stay USB. Do NOT TryAll,
                 * GetNVMeHealthLogEx, or NvmeMini. */
                BOOL got = FALSE;

                if (AcquireATASMART(hDrive, nDrive, pInfo, TRUE) &&
                    pInfo->attrData.stAttributes[0].bAttrID != 0) {
                    got = TRUE;
                    pInfo->bSMART_Supported = TRUE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->bIsUSB = TRUE;
                    pInfo->eType = DetectDriveType(hDrive, pInfo);
                }

                if (!got) {
                    if (NVMeIdentifyASMedia(hDrive, pInfo) &&
                        NVMeHealthLogASMedia(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->bIsUSB = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                        pInfo->bSMART_Supported = TRUE;
                        got = TRUE;
                    }
                }

                if (!got) {
                    pInfo->bSMART_Supported = FALSE;
                    pInfo->bIsNVMe = FALSE;
                    pInfo->eType = DRIVE_TYPE_USB;
                }
            }
            else if (bridge == USB_BRIDGE_NVME_VLI) {
                /* NVMe via VLI bridge (VL716/VL717) */
                if (NVMeIdentifyVLI(hDrive, pInfo)) {
                    if (NVMeHealthLogVLI(hDrive, pInfo)) {
                        ExtractNVMeExtendedInfo(pInfo);
                        pInfo->bIsNVMe = TRUE;
                        pInfo->eType = DRIVE_TYPE_NVME;
                    }
                }
            }

            /* No NVMeOverUSBTryAll here. An unidentified bridge used to
             * receive every vendor CDB in turn; that bugchecks UASP and
             * USBSTOR on flash sticks and unknown docks. Known bridges
             * already had their one command above. SAT is the fallback. */

            /* If not NVMe-over-USB, or NVMe detection failed, try SATA SMART via SAT.
             * Realtek RTL9210/FMA and JMicron/ASMedia NVMe: SAT + one vendor
             * passthrough already handled above; no TryAll, GetNVMeHealthLogEx,
             * or second AcquireATASMART. */
            if (!pInfo->bIsNVMe &&
                !IsRealtekNvmeUsbBridge(pInfo) &&
                bridge != USB_BRIDGE_NVME_REALTEK &&
                bridge != USB_BRIDGE_NVME_FMA &&
                bridge != USB_BRIDGE_NVME_JMICRON &&
                bridge != USB_BRIDGE_NVME_ASMEDIA) {
                if (pInfo->bSMART_Supported) {
                    AcquireATASMART(hDrive, nDrive, pInfo, TRUE);
                } else {
                    /* Try SAT regardless of IDENTIFY's SMART supported bit */
                    if (GetSMARTAttributesSAT(hDrive, pInfo)) {
                        GetSMARTThresholdsSAT(hDrive, pInfo);
                        pInfo->bSMART_Supported = TRUE;
                        ExtractTemperatureFromATA(pInfo);
                        ExtractCommonATACounters(pInfo);
                        ExtractSSDIndicators(pInfo);
                        GetSMARTErrorLogSAT(hDrive, pInfo);
                        GetSMARTSelfTestLogSAT(hDrive, pInfo);
                    } else if (GetSMARTViaStorageProtocol(hDrive, pInfo)) {
                        pInfo->bSMART_Supported = TRUE;
                        ExtractTemperatureFromATA(pInfo);
                        ExtractCommonATACounters(pInfo);
                        ExtractSSDIndicators(pInfo);
                    } else if (GetNVMeHealthLogEx(hDrive, pInfo)) {
                        /* Fallback: detect if this is actually NVMe via USB.
                         * GetNVMeHealthLogEx already refuses bus type 7. */
                        pInfo->bIsNVMe = TRUE;
                        pInfo->eType   = DRIVE_TYPE_NVME;
                        ExtractNVMeExtendedInfo(pInfo);
                        if (pInfo->szModel[0] == '\0' || pInfo->szSerial[0] == '\0')
                            GetNVMeIdentifyController(hDrive, pInfo);
                    } else if (GetSMARTViaLogSense(hDrive, pInfo)) {
                        pInfo->bSMART_Supported = TRUE;
                    }
                }
            }
        }
        else if (pInfo->bSMART_Supported) {
            AcquireATASMART(hDrive, nDrive, pInfo, TRUE);
        }

        AssessDriveHealth(pInfo);
        IdentifyDriveParts(pInfo);
        FillDriveProtocol(pInfo);

        CloseHandle(hDrive);
        nFound++;
    }

    return nFound;
}

/* ============================================================
 * Formatting
 * ============================================================ */
void FormatSize(DWORD dwMB, char* szBuf, int nBufLen)
{
    if (dwMB >= 1024 * 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f TB", (double)dwMB / (1024.0 * 1024.0));
    else if (dwMB >= 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f GB", (double)dwMB / 1024.0);
    else
        safe_snprintf_n(szBuf, nBufLen, "%u MB", (unsigned)dwMB);
}

