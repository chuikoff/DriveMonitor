/* DriveMonitor - profile resolver. GPL-2.0-or-later. See LICENSE.
 * Picks one profile for a drive. A row may carry a channel rule.
 * The finished grade is still computed by the health engine.
 *
 * Priority, highest first: model+firmware 100, exact model 98, family 90,
 * controller 80, vendor 70, generic ATA/NVMe 50. The comparator is the
 * score, then the specificity of the model and firmware key. The same
 * comparator selects the attribute tier. If the score and the specificity
 * are both equal, the identity match is ambiguous: the lexically first id
 * is recorded and the other id is the conflict. An attribute on a tier is
 * used only when every profile on that tier states the same semantic,
 * encoding, preference, and health-rule fields. If they disagree, or the attribute is
 * missing from some of them, lookup refuses. If none of them name it, the
 * next worse tier is tried. A lower-tier conflict does not mark the
 * identity ambiguous. Registration order is not a tie-break. A firmware
 * shape alone does not select a controller. Brand alone is not a controller.
 *
 * Builtin rows are this file. ProfileRegister adds a row without changing
 * the interpreter. drivedb.h is a separate matcher (DriveDbApply) and is
 * not reordered by this priority. */
#include "smart_internal.h"

#define PA(id, sem, enc) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      0, NULL, NULL, NULL, NULL, \
      0, 0, 0, 0, 0, 0, 0, 0 }
#define PN(id, sem, enc, ru, en) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      0, ru, en, NULL, NULL, \
      0, 0, 0, 0, 0, 0, 0, 0 }
#define PNA(id, sem, enc, ru, en, unless, aru, aen) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      (unsigned char)(unless), ru, en, aru, aen, \
      0, 0, 0, 0, 0, 0, 0, 0 }
#define PH(id, sem, enc, rule, slot, scope, obs, cau, bad, badHdd) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      0, NULL, NULL, NULL, NULL, \
      (unsigned char)(rule), (unsigned char)(slot), (unsigned char)(scope), \
      (unsigned short)(obs), (unsigned short)(cau), \
      (unsigned short)(bad), (unsigned short)(badHdd), 0 }
#define PHN(id, sem, enc, ru, en, rule, slot, scope, obs, cau, bad, badHdd) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      0, ru, en, NULL, NULL, \
      (unsigned char)(rule), (unsigned char)(slot), (unsigned char)(scope), \
      (unsigned short)(obs), (unsigned short)(cau), \
      (unsigned short)(bad), (unsigned short)(badHdd), 0 }
/* Same as PN, with an explicit preference among one semantic. */
#define PNY(id, sem, enc, ru, en, pref) \
    { (unsigned char)(id), (unsigned char)(sem), (unsigned char)(enc), \
      0, ru, en, NULL, NULL, \
      0, 0, 0, 0, 0, 0, 0, (unsigned char)(pref) }

typedef struct _DRIVE_PROFILE {
    const char* id;
    const char* extends;
    int kind;
    const char* model;
    const char* fw;
    int fwMode;
    DRIVE_VENDOR vendor;
    DRIVE_CONTROLLER ctl;
    int nvme;                 /* -1 any, 0 not NVMe, 1 NVMe only */
    int hdd;                  /* -1 any, 0 not HDD, 1 HDD only */
    const char* chip;
    const char* nvmeVer;
    const PROF_ATTR* attrs;
    int flags;
    const char* origin;
} DRIVE_PROFILE;

/* Threshold 0 is off. badHdd applies only on an HDD.
 * Pending 4 and realloc 10 are BAD on HDD and stay CAUTION on SSD.
 * 184 is observe from 1 and caution from 5, HDD only.
 * 187 is BAD from 1 on HDD. On a generic SSD it is not scored.
 * Crucial and Micron keep their own raw48 rule. 188 and CRC share the interface steps. */
static const PROF_ATTR kAtaGeneric[] = {
    PH(0x05, PROF_SEM_MEDIA, PROF_ENC_LO16, PROF_RULE_WORSE,
       PROF_SLOT_REALLOC, PROF_SCOPE_ANY, 0, 1, 0, 10),
    PA(0x09, PROF_SEM_NONE,      PROF_ENC_COUNTER),
    PH(0x0A, PROF_SEM_INTERFACE, PROF_ENC_COUNTER, PROF_RULE_WORSE,
       PROF_SLOT_NONE, PROF_SCOPE_HDD, 0, 3, 0, 0),
    PA(0x0C, PROF_SEM_NONE,      PROF_ENC_COUNTER),
    PH(0xB8, PROF_SEM_MEDIA, PROF_ENC_COUNTER, PROF_RULE_WORSE,
       PROF_SLOT_NONE, PROF_SCOPE_HDD, 1, 5, 0, 0),
    PH(0xBB, PROF_SEM_MEDIA, PROF_ENC_REPORTED, PROF_RULE_WORSE,
       PROF_SLOT_REPORTED, PROF_SCOPE_HDD, 0, 0, 1, 0),
    PH(0xBC, PROF_SEM_INTERFACE, PROF_ENC_LO16, PROF_RULE_WORSE,
       PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 10, 100, 0),
    PA(0xC2, PROF_SEM_THERMAL,   PROF_ENC_COUNTER),
    PH(0xC4, PROF_SEM_MEDIA, PROF_ENC_REMAP, PROF_RULE_WORSE,
       PROF_SLOT_REMAP, PROF_SCOPE_ANY, 0, 1, 0, 0),
    PH(0xC5, PROF_SEM_MEDIA, PROF_ENC_COUNTER, PROF_RULE_WORSE,
       PROF_SLOT_PENDING, PROF_SCOPE_ANY, 0, 1, 0, 4),
    PH(0xC6, PROF_SEM_MEDIA, PROF_ENC_COUNTER, PROF_RULE_WORSE,
       PROF_SLOT_UNCORR, PROF_SCOPE_ANY, 0, 0, 1, 0),
    PH(0xC7, PROF_SEM_INTERFACE, PROF_ENC_COUNTER, PROF_RULE_WORSE,
       PROF_SLOT_CRC, PROF_SCOPE_ANY, 0, 1, 100, 0),
    PA(0, 0, 0)
};

static const PROF_ATTR kPhison[] = {
    PN(0xA0, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Внезапные выключения", "Unsafe shutdowns"),
    /* Remaining spare is not a defect count. */
    PHN(0xA1, PROF_SEM_MEDIA, PROF_ENC_COUNTER,
        "Резервные блоки (осталось)", "Spare blocks remaining",
        PROF_RULE_OFF, PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 0, 0, 0),
    PN(0xA3, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Начальные плохие блоки", "Initial bad blocks"),
    PN(0xA4, PROF_SEM_ERASE,      PROF_ENC_COUNTER, "Всего стираний", "Total erase count"),
    PN(0xA5, PROF_SEM_ERASE,      PROF_ENC_MAX,     "Макс. стираний", "Max erase count"),
    PN(0xA6, PROF_SEM_ERASE,      PROF_ENC_MIN,     "Мин. стираний", "Min erase count"),
    PN(0xA7, PROF_SEM_ERASE,      PROF_ENC_AVG,     "Среднее стираний", "Average erase count"),
    PN(0xA8, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Макс. стираний по спецификации", "Specified max erase count"),
    PN(0xA9, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT, "Заявленный остаток ресурса", "Reported life remaining"),
    PN(0xAF, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Сбой защиты питания", "Power-loss protection fail"),
    PN(0xB0, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки стирания (худший кристалл)", "Erase fails (worst die)"),
    PN(0xB1, PROF_SEM_WEAR,       PROF_ENC_COUNTER, "Всего циклов выравнивания износа", "Wear-leveling count"),
    PN(0xB2, PROF_SEM_MEDIA,      PROF_ENC_COUNTER, "Резервные блоки (использовано)", "Used spare blocks"),
    PN(0xB5, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки программирования (всего)", "Program fail count"),
    PN(0xB6, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки стирания (всего)", "Erase fail count"),
    PN(0xC0, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Аварийные отключения питания", "Unsafe power-off events"),
    PN(0xC3, PROF_SEM_NONE,       PROF_ENC_NONE,    "ECC (вендор)", "ECC (vendor)"),
    /* 245 is a NAND counter when 165 is present. Otherwise the name is max erase. */
    PNA(0xF5, PROF_SEM_NONE,      PROF_ENC_COUNTER,
        "Счётчик NAND", "NAND counter", 0xA5, "Макс. стираний", "Max erase count"),
    PN(0xE7, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT, "Остаток ресурса SSD", "SSD life remaining"),
    PN(0xE8, PROF_SEM_WEAR,       PROF_ENC_PERCENT, "Резервное пространство", "Available reserved space"),
    PN(0xE9, PROF_SEM_NAND_WRITE, PROF_ENC_GIB,     "Записи NAND (ГиБ)", "NAND writes (GiB)"),
    PN(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_SCALE,   "Записано хостом (ГБ)", "Host written (GB)"),
    PN(0xF2, PROF_SEM_HOST_READ,  PROF_ENC_SCALE,   "Прочитано хостом (ГБ)", "Host read (GB)"),
    PN(0xF3, PROF_SEM_HOST_WRITE, PROF_ENC_SCALE,   "Записано хостом (ГБ)", "Host written (GB)"),
    PA(0, 0, 0)
};

static const PROF_ATTR kSamsungSsd[] = {
    /* Value is remaining life when 231 is absent. RAW is not the percent.
     * Preference 0 loses to 231. */
    PN(0xB1, PROF_SEM_ENDURANCE,  PROF_ENC_VALUE, "Выравнивание износа", "Wear leveling"),
    PN(0xB3, PROF_SEM_MEDIA,      PROF_ENC_COUNTER, "Использовано резервных блоков", "Used reserved blocks"),
    PA(0xB5, PROF_SEM_NONE,       PROF_ENC_COUNTER),
    PN(0xB6, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки стирания", "Erase fail count"),
    /* 183 is not a confirmed defect count on Samsung SSD. */
    PH(0xB7, PROF_SEM_MEDIA, PROF_ENC_COUNTER, PROF_RULE_OFF,
       PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 0, 0, 0),
    PHN(0xBB, PROF_SEM_MEDIA, PROF_ENC_COUNTER,
        "Неисправимые ошибки", "Uncorrectable errors",
        PROF_RULE_WORSE, PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 0, 1, 0),
    /* Higher preference than 177. RAW is an erase count, not the percent. */
    PNY(0xE7, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "Остаток ресурса SSD", "SSD life remaining", 1),
    PN(0xEB, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Защита от потери питания", "Power-loss protection"),
    PN(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_LBA,     "Записано хостом (ГБ)", "Host written (GB)"),
    PN(0xF2, PROF_SEM_HOST_READ,  PROF_ENC_LBA,     "Прочитано хостом (ГБ)", "Host read (GB)"),
    PN(0xF5, PROF_SEM_ERASE,      PROF_ENC_MIN,     "Мин. стираний (Samsung)", "Min erase count (Samsung)"),
    PN(0xF6, PROF_SEM_ERASE,      PROF_ENC_MAX,     "Макс. стираний (Samsung)", "Max erase count (Samsung)"),
    PN(0xF7, PROF_SEM_ERASE,      PROF_ENC_AVG,     "Среднее стираний (Samsung)", "Average erase count (Samsung)"),
    PN(0xF8, PROF_SEM_WEAR,       PROF_ENC_COUNTER, "Выравнивание износа (Samsung)", "Wear leveling (Samsung)"),
    PA(0, 0, 0)
};

/* SM2258/SM2259 map. a400-smi225 is Kingston A400 plus the 7-character
 * firmware. kpss-smi225 is model KPSS120G2 plus that same shape
 * (W0714A0). The shape alone does not select this table, and the model
 * with another firmware (HT3618C1) does not either.
 * Apacer AS340/AS350 keep their own life counter (231). */
static const PROF_ATTR kSmi225[] = {
    PN(0xA0, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Неисправимые ошибки", "Uncorrectable errors"),
    PN(0xA1, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Годные резервные блоки", "Valid spare blocks"),
    PN(0xA3, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Начальные плохие блоки", "Initial bad blocks"),
    PN(0xA4, PROF_SEM_ERASE,      PROF_ENC_COUNTER, "Всего стираний", "Total erase count"),
    PN(0xA5, PROF_SEM_ERASE,      PROF_ENC_MAX,     "Макс. стираний", "Max erase count"),
    PN(0xA6, PROF_SEM_ERASE,      PROF_ENC_MIN,     "Мин. стираний", "Min erase count"),
    PN(0xA7, PROF_SEM_ERASE,      PROF_ENC_AVG,     "Среднее стираний", "Average erase count"),
    PN(0xA8, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Макс. стираний по спецификации", "Specified max erase count"),
    PN(0xA9, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT, "Остаток ресурса", "Life remaining"),
    PN(0xAF, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки программирования (чип)", "Program fail count (chip)"),
    PN(0xB0, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки стирания (чип)", "Erase fail count (chip)"),
    PN(0xB1, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Выравнивание износа", "Wear leveling"),
    PN(0xB2, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Плохие блоки (runtime)", "Runtime invalid blocks"),
    PN(0xB5, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки программирования (всего)", "Program fail count (total)"),
    PN(0xB6, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки стирания (всего)", "Erase fail count (total)"),
    PN(0xC3, PROF_SEM_NONE,       PROF_ENC_COUNTER, "ECC контроллера", "Controller ECC"),
    PN(0xE8, PROF_SEM_NONE,       PROF_ENC_PERCENT, "Резервное пространство", "Available reserved space"),
    PN(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_BLOCK32, "Записано хостом", "Host written"),
    PN(0xF2, PROF_SEM_HOST_READ,  PROF_ENC_BLOCK32, "Прочитано хостом", "Host read"),
    PN(0xF5, PROF_SEM_NAND_WRITE, PROF_ENC_BLOCK32, "Записи NAND", "NAND writes"),
    PA(0, 0, 0)
};

/* KingSpec P3-* with SN firmware. 231 Value is life remaining.
 * 169 is a bad-block pair, not endurance. 241/242 are already GiB. */
static const PROF_ATTR kMaxioSata[] = {
    PN(0xA7, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Режим защиты", "Protect mode"),
    PN(0xA8, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Ошибки SATA PHY", "SATA PHY errors"),
    PN(0xA9, PROF_SEM_NONE,       PROF_ENC_PAIR,    "Плохие блоки", "Bad blocks"),
    PN(0xAD, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Стирания", "Erase count"),
    PN(0xAF, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Плохие кластеры", "Bad clusters"),
    PN(0xB4, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Резервные блоки", "Spare blocks left"),
    /* Value is life remaining. RAW is percent used, not the percent left. */
    PN(0xE7, PROF_SEM_ENDURANCE,  PROF_ENC_VALUE,   "Остаток ресурса SSD", "SSD life remaining"),
    PN(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_GIB,     "Записано хостом (ГиБ)", "Host written (GiB)"),
    PN(0xF2, PROF_SEM_HOST_READ,  PROF_ENC_GIB,     "Прочитано хостом (ГиБ)", "Host read (GiB)"),
    PA(0, 0, 0)
};

static const PROF_ATTR kIntel[] = {
    PN(0xAA, PROF_SEM_WEAR,       PROF_ENC_PERCENT, "Доступное резервное пространство", "Available reserved space"),
    PN(0xE1, PROF_SEM_HOST_WRITE, PROF_ENC_BLOCK32, "Записи хоста", "Host writes"),
    PN(0xE2, PROF_SEM_NONE,       PROF_ENC_COUNTER, "Таймер нагрузки", "Timed workload"),
    PN(0xE8, PROF_SEM_WEAR,       PROF_ENC_PERCENT, "Доступное резервное пространство", "Available reserved space"),
    PN(0xE9, PROF_SEM_WEAR,       PROF_ENC_PERCENT, "Индикатор износа", "Media wear-out indicator"),
    PA(0, 0, 0)
};

static const PROF_ATTR kMicron[] = {
    PN(0xAD, PROF_SEM_ERASE,      PROF_ENC_AVG,     "Среднее стираний", "Average erase count"),
    /* Normalized Value is remaining life. RAW is not the percent. */
    PN(0xCA, PROF_SEM_ENDURANCE,  PROF_ENC_VALUE,   "Остаток ресурса %", "Percent lifetime remaining"),
    PN(0xF6, PROF_SEM_HOST_WRITE, PROF_ENC_LBA,     "Записано секторов хоста", "Host sectors written"),
    PA(0, 0, 0)
};

/* smartmontools: Crucial/Micron client SSDs, attribute 187 is raw48
 * Reported_Uncorrect (the default ATA meaning; the -v line is commented
 * because it repeats that default). A confirmed count is caution from 1.
 * Samsung and HDD 187 stay bad from 1. A generic SSD still does not score it. */
static const PROF_ATTR kCrucialMicron[] = {
    PHN(0xBB, PROF_SEM_MEDIA, PROF_ENC_COUNTER,
        "Неисправимые ошибки", "Uncorrectable errors",
        PROF_RULE_WORSE, PROF_SLOT_NONE, PROF_SCOPE_SSD, 0, 1, 0, 0),
    PA(0, 0, 0)
};

static const PROF_ATTR kSeagateHdd[] = {
    PN(0x01, PROF_SEM_NONE,      PROF_ENC_COUNTER, "Частота ошибок чтения (RAW вендора)", "Read Error Rate (vendor RAW)"),
    PHN(0xBB, PROF_SEM_MEDIA, PROF_ENC_REPORTED,
        "Неисправимые ошибки", "Uncorrectable errors",
        PROF_RULE_WORSE, PROF_SLOT_REPORTED, PROF_SCOPE_ANY, 0, 0, 1, 0),
    PN(0xC3, PROF_SEM_NONE,      PROF_ENC_COUNTER, "Восстановлено ECC (чтение)", "Hardware ECC recovered"),
    PN(0xF0, PROF_SEM_NONE,      PROF_ENC_COUNTER, "Часы полёта головок", "Head flying hours"),
    PA(0, 0, 0)
};

static const PROF_ATTR kToshibaHdd[] = {
    PHN(0xC7, PROF_SEM_INTERFACE, PROF_ENC_COUNTER,
        "Ошибки CRC", "CRC errors",
        PROF_RULE_WORSE, PROF_SLOT_CRC, PROF_SCOPE_ANY, 0, 1, 100, 0),
    PA(0, 0, 0)
};

/* nvme: -1 any, 0 ATA, 1 NVMe. hdd: -1 any, 0 not HDD, 1 HDD.
 * flags and origin are last. NULL origin means builtin. */
static const DRIVE_PROFILE kProfiles[] = {
    { "ata-generic", NULL, PROF_KIND_GENERIC, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_UNKNOWN, 0, -1, NULL, NULL, kAtaGeneric, 0, NULL },
    { "nvme-generic", NULL, PROF_KIND_GENERIC, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_UNKNOWN, 1, 0, NULL, NULL, NULL, 0, NULL },

    { "phison", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, -1, 0, NULL, NULL, kPhison,
      PROF_F_PREFER_AGREEING, NULL },
    { "samsung-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, NULL, NULL, kSamsungSsd,
      0, NULL },
    /* NVMe life is Percentage Used. kSamsungSsd stays on ATA. */
    { "samsung-nvme", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 1, 0, NULL, NULL, NULL, 0, NULL },
    { "intel-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_INTEL, -1, 0, NULL, NULL, kIntel, 0, NULL },
    { "micron-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_MICRON, -1, 0, NULL, NULL, kMicron, 0, NULL },
    /* Matches only after PCI VID or a chip token already set SMI. */
    { "smi", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0, NULL, NULL, NULL, 0, NULL },
    /* Controller is already Silicon Motion and the firmware has the
     * 7-character shape. The shape alone, with an unknown controller, matches nothing. */
    { "smi-s225-fw", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_SMI225,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, NULL, NULL, kSmi225, 0, NULL },
    { "a400-smi225", NULL, PROF_KIND_EXACT, "A400", NULL, PROF_FW_SMI225,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0,
      "Silicon Motion SM2258/SM2259", NULL, kSmi225, 0, NULL },
    /* KingPrice KPSS120G2 is sold with more than one controller.
     * Only this model plus the SM2258 firmware shape takes kSmi225. */
    { "kpss-smi225", NULL, PROF_KIND_EXACT, "KPSS120G2", NULL, PROF_FW_SMI225,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0,
      "Silicon Motion SM2258/SM2259", NULL, kSmi225, 0, NULL },
    { "smi-ve", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_VE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0,
      "Silicon Motion SM2258", NULL, NULL, 0, NULL },
    { "samsung-870-svt", "samsung-ssd", PROF_KIND_EXACT, "870", "SVT", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL, 0, NULL },
    { "samsung-mznln", "samsung-ssd", PROF_KIND_EXACT, "MZNLN", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, NULL, NULL, NULL, 0, NULL },
    { "p3-maxio-sn", NULL, PROF_KIND_EXACT, "P3-", "SN", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_MAXIO, 0, 0, "Maxio", NULL, kMaxioSata,
      0, NULL },
    { "maxio-sata", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_MAXIO, 0, 0, NULL, NULL, NULL,
      0, NULL },

    { "crucial-micron", NULL, PROF_KIND_VENDOR, NULL, NULL, PROF_FW_NONE,
      VENDOR_MICRON, CONTROLLER_UNKNOWN, 0, 0, NULL, NULL, kCrucialMicron, 0, NULL },
    { "seagate-hdd", NULL, PROF_KIND_VENDOR, NULL, NULL, PROF_FW_NONE,
      VENDOR_SEAGATE, CONTROLLER_UNKNOWN, 0, 1, NULL, NULL, kSeagateHdd, 0, NULL },
    { "toshiba-hdd", NULL, PROF_KIND_VENDOR, NULL, NULL, PROF_FW_NONE,
      VENDOR_TOSHIBA, CONTROLLER_UNKNOWN, 0, 1, NULL, NULL, kToshibaHdd, 0, NULL },

    { "samsung-870", "samsung-ssd", PROF_KIND_FAMILY, "870", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, NULL, NULL, NULL, 0, NULL },
    /* IDENTIFY often omits the brand and keeps CT…MX200…. The vendor row
     * still covers Crucial_ and Micron_ models that are not MX200. */
    { "mx200", NULL, PROF_KIND_FAMILY, "MX200", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_UNKNOWN, 0, 0, NULL, NULL, kCrucialMicron, 0, NULL },

    { "micron-2400", NULL, PROF_KIND_EXACT, "MICRON_2400", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 1, 0, "Silicon Motion SM2269XT", "1.4.0", NULL, 0, NULL },
    { "micron-2400-mtfd", NULL, PROF_KIND_EXACT, "2400_MTFD", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 1, 0, "Silicon Motion SM2269XT", "1.4.0", NULL, 0, NULL },
    { "sx8200pnp", NULL, PROF_KIND_EXACT, "SX8200PNP", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL, 0, NULL },
    { "sx8200-pro", NULL, PROF_KIND_EXACT, "SX8200 PRO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL, 0, NULL },
    { "sx8200pro", NULL, PROF_KIND_EXACT, "SX8200PRO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL, 0, NULL },
    { "gammix-s11", NULL, PROF_KIND_EXACT, "GAMMIX S11", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL, 0, NULL },
    { "xpg-s11", NULL, PROF_KIND_EXACT, "XPG S11", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL, 0, NULL },
    { "su800", NULL, PROF_KIND_EXACT, "SU800", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258/SM2259", NULL, NULL, 0, NULL },
    { "as340", NULL, PROF_KIND_EXACT, "AS340", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258", NULL, NULL, 0, NULL },
    { "as350", NULL, PROF_KIND_EXACT, "AS350", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258", NULL, NULL, 0, NULL },
    { "samsung-870-evo", "samsung-ssd", PROF_KIND_EXACT, "870 EVO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL, 0, NULL },
    { "samsung-870evo", "samsung-ssd", PROF_KIND_EXACT, "870EVO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL, 0, NULL },
    { "r5sl", "phison", PROF_KIND_EXACT, "R5SL", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "gammix-s70", NULL, PROF_KIND_EXACT, "GAMMIX S70", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_INNOGRIT, 1, 0, "Innogrit IG5236", "1.4.0", NULL, 0, NULL },
    { "p210-u11", "phison", PROF_KIND_EXACT, "P210", "U11", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "burst-u11", "phison", PROF_KIND_EXACT, "BURST", "U11", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "burst-u10", "phison", PROF_KIND_EXACT, "BURST", "U10", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3110-S10", NULL, NULL, 0, NULL },
    { "burst-hps", "phison", PROF_KIND_EXACT, "BURST", "HPS", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, NULL, NULL, NULL, 0, NULL },
    { "gstfs-sbfm", "phison", PROF_KIND_EXACT, "GSTFS", "SBFM", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "a400-sbfk61", "phison", PROF_KIND_EXACT, "A400", "SBFK61", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3110-S10", NULL, NULL, 0, NULL },
    { "a400-sbfk62", "phison", PROF_KIND_EXACT, "A400", "SBFK62", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "a400-sbfk", "phison", PROF_KIND_EXACT, "A400", "SBFK", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0,
      "Phison PS3110-S10 / PS3111-S11", NULL, NULL, 0, NULL },
    { "a400-sbfm", "phison", PROF_KIND_EXACT, "A400", "SBFM", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL, 0, NULL },
    { "m450", "phison", PROF_KIND_EXACT, "M450", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, -1, 0, "Phison PS5019-E19T", NULL, NULL, 0, NULL }
};

enum { PROF_EXTRA_MAX = 8, PROF_TOP_MAX = 64 };

static DRIVE_PROFILE g_extra[PROF_EXTRA_MAX];
static int g_nExtra = 0;

static int BuiltinCount(void)
{
    return (int)(sizeof(kProfiles) / sizeof(kProfiles[0]));
}

static int ProfileCount(void)
{
    return BuiltinCount() + g_nExtra;
}

static const DRIVE_PROFILE* ProfileAt(int i)
{
    if (i < BuiltinCount()) return &kProfiles[i];
    return &g_extra[i - BuiltinCount()];
}

static int FwHits(const DRIVE_PROFILE* pr, const char* fwU)
{
    if (pr->fwMode == PROF_FW_SMI225)
        return FirmwareIsSmi225x(fwU);
    if (pr->fwMode == PROF_FW_VE)
        return fwU[0] == 'V' && fwU[1] == 'E' &&
               fwU[2] >= '0' && fwU[2] <= '9';
    if (pr->fwMode == PROF_FW_SUBSTR)
        return pr->fw && fwU[0] && strstr(fwU, pr->fw) != NULL;
    return 1;
}

static int HasFwKey(const DRIVE_PROFILE* pr)
{
    return pr->fwMode == PROF_FW_SMI225 || pr->fwMode == PROF_FW_VE ||
           pr->fwMode == PROF_FW_SUBSTR;
}

static int TransportOk(const DRIVE_PROFILE* pr, const DRIVE_INFO* p)
{
    if (pr->nvme == 1 && !p->bIsNVMe) return 0;
    if (pr->nvme == 0 && p->bIsNVMe) return 0;
    if (pr->hdd == 1 && !DriveIsHdd(p)) return 0;
    if (pr->hdd == 0 && DriveIsHdd(p)) return 0;
    return 1;
}

static int ScoreProfile(const DRIVE_PROFILE* pr, const DRIVE_INFO* p,
                        const char* modelU, const char* fwU)
{
    int modelHit, fwHit;
    if (!TransportOk(pr, p)) return 0;
    modelHit = pr->model && pr->model[0] && strstr(modelU, pr->model) != NULL;
    fwHit = HasFwKey(pr) && FwHits(pr, fwU);

    if (pr->model && pr->model[0] && !modelHit) return 0;
    if (HasFwKey(pr) && !fwHit) return 0;
    /* A firmware shape confirms a model row, or a controller already set
     * by PCI VID, a model, or a chip token. The shape alone matches nothing. */
    if (HasFwKey(pr) && !(pr->model && pr->model[0]) &&
        p->eController != pr->ctl)
        return 0;
    if (pr->vendor != VENDOR_UNKNOWN && p->eVendor != pr->vendor) return 0;

    switch (pr->kind) {
    case PROF_KIND_EXACT:
        if (!modelHit) return 0;
        return fwHit ? PROF_SCORE_EXACT_FW : PROF_SCORE_EXACT_MODEL;
    case PROF_KIND_FAMILY:
        if (!modelHit) return 0;
        return PROF_SCORE_FAMILY;
    case PROF_KIND_CONTROLLER:
        if (!modelHit && !fwHit && p->eController != pr->ctl)
            return 0;
        if (pr->ctl == CONTROLLER_UNKNOWN) return 0;
        return PROF_SCORE_CONTROLLER;
    case PROF_KIND_VENDOR:
        if (pr->vendor == VENDOR_UNKNOWN || p->eVendor != pr->vendor)
            return 0;
        return PROF_SCORE_VENDOR;
    case PROF_KIND_GENERIC:
        return PROF_SCORE_GENERIC;
    default:
        return 0;
    }
}

static int Specificity(const DRIVE_PROFILE* pr)
{
    int n = 0;
    if (pr->model) n += (int)strlen(pr->model);
    if (pr->fw) n += (int)strlen(pr->fw);
    if (pr->fwMode == PROF_FW_SMI225) n += 7;
    if (pr->fwMode == PROF_FW_VE) n += 3;
    return n;
}

/* Positive when A beats B. Zero when the score and the specificity match. */
static int MatchCmp(int scoreA, int specA, int scoreB, int specB)
{
    if (scoreA != scoreB)
        return (scoreA > scoreB) ? 1 : -1;
    if (specA != specB)
        return (specA > specB) ? 1 : -1;
    return 0;
}

static const DRIVE_PROFILE* ProfileById(const char* id)
{
    int i;
    if (!id || !id[0]) return NULL;
    for (i = 0; i < ProfileCount(); i++) {
        if (strcmp(ProfileAt(i)->id, id) == 0)
            return ProfileAt(i);
    }
    return NULL;
}

static const DRIVE_PROFILE* GenericFor(const DRIVE_INFO* p)
{
    return ProfileById((p && p->bIsNVMe) ? "nvme-generic" : "ata-generic");
}

static const PROF_ATTR* FindProfAttr(const PROF_ATTR* tab, BYTE id)
{
    int i;
    if (!tab || id == 0) return NULL;
    for (i = 0; tab[i].id != 0; i++) {
        if (tab[i].id == id) return &tab[i];
    }
    return NULL;
}

static const PROF_ATTR* AttrOn(const DRIVE_PROFILE* pr, BYTE id)
{
    int guard = 0;
    while (pr && guard++ < 8) {
        const PROF_ATTR* a = FindProfAttr(pr->attrs, id);
        if (a) return a;
        pr = pr->extends ? ProfileById(pr->extends) : NULL;
    }
    return NULL;
}

static int FlagsOf(const DRIVE_PROFILE* pr)
{
    int f = 0;
    int guard = 0;
    while (pr && guard++ < 8) {
        f |= pr->flags;
        pr = pr->extends ? ProfileById(pr->extends) : NULL;
    }
    return f;
}

/* Best non-generic tier strictly below (ceilScore, ceilSpec).
 * *nTies is the full count. The array holds at most maxTop of them.
 * *first is the lexically smallest id. */
static void FillTier(const DRIVE_INFO* p, const DRIVE_PROFILE** top, int maxTop,
                     int ceilScore, int ceilSpec,
                     int* nTies, int* nStored, const DRIVE_PROFILE** first,
                     const DRIVE_PROFILE** second, int* scoreOut, int* specOut)
{
    char modelU[48], fwU[16];
    int bestScore = 0, bestSpec = -1;
    int found = 0;
    int i, pass;

    *nTies = 0;
    *nStored = 0;
    if (first) *first = NULL;
    if (second) *second = NULL;
    *scoreOut = 0;
    *specOut = 0;
    if (!p) return;
    ToUpperCopy(modelU, sizeof(modelU), p->szModel);
    ToUpperCopy(fwU, sizeof(fwU), p->szFirmware);

    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < ProfileCount(); i++) {
            const DRIVE_PROFILE* pr = ProfileAt(i);
            int score, spec;
            if (pr->kind == PROF_KIND_GENERIC) continue;
            score = ScoreProfile(pr, p, modelU, fwU);
            if (score <= 0) continue;
            spec = Specificity(pr);
            if (MatchCmp(score, spec, ceilScore, ceilSpec) >= 0) continue;
            if (pass == 0) {
                if (!found || MatchCmp(score, spec, bestScore, bestSpec) > 0) {
                    bestScore = score;
                    bestSpec = spec;
                    found = 1;
                }
                continue;
            }
            if (!found || MatchCmp(score, spec, bestScore, bestSpec) != 0)
                continue;
            (*nTies)++;
            if (*nStored < maxTop)
                top[(*nStored)++] = pr;
            if (!first) continue;
            if (!*first || strcmp(pr->id, (*first)->id) < 0) {
                if (second) *second = *first;
                *first = pr;
            } else if (second && (!*second || strcmp(pr->id, (*second)->id) < 0)) {
                *second = pr;
            }
        }
    }
    if (!found) return;
    *scoreOut = bestScore;
    *specOut = bestSpec;
}

/* Identity tier. 101 sits above PROF_SCORE_EXACT_FW. */
static void FillTop(const DRIVE_INFO* p, const DRIVE_PROFILE** top, int maxTop,
                    int* nTies, int* nStored, const DRIVE_PROFILE** first,
                    const DRIVE_PROFILE** second, int* scoreOut)
{
    int spec = 0;
    FillTier(p, top, maxTop, 101, 0, nTies, nStored, first, second,
             scoreOut, &spec);
    (void)spec;
}

static int TieLists(const DRIVE_PROFILE** top, int nStored, BYTE id)
{
    int i;
    for (i = 0; i < nStored; i++) {
        if (AttrOn(top[i], id)) return 1;
    }
    return 0;
}

static const PROF_ATTR* AgreedAttr(const DRIVE_PROFILE** top, int nStored,
                                   int nTies, BYTE id)
{
    const PROF_ATTR* first = NULL;
    int i;
    if (nTies <= 0 || nTies != nStored) return NULL;
    for (i = 0; i < nStored; i++) {
        const PROF_ATTR* a = AttrOn(top[i], id);
        if (!a) return NULL;
        if (!first) first = a;
        else if (a->sem != first->sem || a->enc != first->enc ||
                 a->pref != first->pref ||
                 a->rule != first->rule || a->slot != first->slot ||
                 a->scope != first->scope || a->observe != first->observe ||
                 a->caution != first->caution || a->bad != first->bad ||
                 a->badHdd != first->badHdd)
            return NULL;
    }
    return first;
}

enum { CONF_NONE = 0, CONF_FOUND = 1, CONF_REFUSE = 2 };

/* Walk tiers from the best match downward. Each tier uses MatchCmp, the
 * same comparator as FillTop. Equal score and equal specificity use the
 * conflict policy: every profile must agree, or the attribute is refused
 * when any of them lists it. A tier that names nobody yields to the next
 * worse tier. The walk does not change the identity winner. */
static int ConfirmLocal(const DRIVE_INFO* p, BYTE id, const PROF_ATTR** out)
{
    const DRIVE_PROFILE* top[PROF_TOP_MAX];
    int ceilScore = 101;
    int ceilSpec = 0;
    int guard;
    if (out) *out = NULL;
    if (!p || id == 0) return CONF_NONE;
    for (guard = 0; guard < PROF_TOP_MAX; guard++) {
        int nTies = 0, nStored = 0, score = 0, spec = 0;
        const PROF_ATTR* a;
        FillTier(p, top, PROF_TOP_MAX, ceilScore, ceilSpec,
                 &nTies, &nStored, NULL, NULL, &score, &spec);
        if (nTies <= 0) return CONF_NONE;
        if (nStored != nTies) return CONF_REFUSE;
        a = AgreedAttr(top, nStored, nTies, id);
        if (a) {
            if (out) *out = a;
            return CONF_FOUND;
        }
        if (TieLists(top, nStored, id))
            return CONF_REFUSE;
        ceilScore = score;
        ceilSpec = spec;
    }
    return CONF_NONE;
}

static void ApplyIdentity(DRIVE_INFO* p, const DRIVE_PROFILE* winner, int score,
                          int nTies, const DRIVE_PROFILE** top, int nStored,
                          const DRIVE_PROFILE* second)
{
    int i;
    p->nProfileScore = score > 0 ? score : PROF_SCORE_GENERIC;
    lstrcpynA(p->szProfileId, winner->id, (int)sizeof(p->szProfileId));
    lstrcpynA(p->szProfileOrigin,
              (winner->origin && winner->origin[0]) ? winner->origin : "builtin",
              (int)sizeof(p->szProfileOrigin));
    p->bProfileAmbiguous = (nTies > 1) ? TRUE : FALSE;
    if (nTies > 1 && second)
        lstrcpynA(p->szProfileConflict, second->id,
                  (int)sizeof(p->szProfileConflict));

    if (nTies <= 1) {
        if (p->nProfileScore >= PROF_SCORE_CONTROLLER &&
            winner->ctl != CONTROLLER_UNKNOWN)
            p->eController = winner->ctl;
        if (winner->chip && winner->chip[0])
            safe_snprintf(p->szControllerChip, "%s", winner->chip);
        if (winner->nvmeVer && p->bIsNVMe &&
            (p->szProtocol[0] == '\0' || strcmp(p->szProtocol, "NVMe") == 0))
            safe_snprintf(p->szProtocol, "NVMe %s", winner->nvmeVer);
        p->nProfileFlags = FlagsOf(winner);
        return;
    }

    if (nStored == nTies && nStored > 0 &&
        p->nProfileScore >= PROF_SCORE_CONTROLLER) {
        DRIVE_CONTROLLER ctl = top[0]->ctl;
        int same = 1;
        for (i = 1; i < nStored; i++) {
            if (top[i]->ctl != ctl) same = 0;
        }
        if (same && ctl != CONTROLLER_UNKNOWN)
            p->eController = ctl;
    }
    if (nStored == nTies && nStored > 0) {
        const char* chip = NULL;
        int same = 1;
        for (i = 0; i < nStored; i++) {
            const char* c = NULL;
            const DRIVE_PROFILE* pr = top[i];
            int guard = 0;
            while (pr && guard++ < 8) {
                if (pr->chip && pr->chip[0]) { c = pr->chip; break; }
                pr = pr->extends ? ProfileById(pr->extends) : NULL;
            }
            if (!c) { same = 0; break; }
            if (!chip) chip = c;
            else if (strcmp(chip, c) != 0) { same = 0; break; }
        }
        if (same && chip)
            safe_snprintf(p->szControllerChip, "%s", chip);
        p->nProfileFlags = ~0;
        for (i = 0; i < nStored; i++)
            p->nProfileFlags &= FlagsOf(top[i]);
    }
}

void ResolveDriveProfile(DRIVE_INFO* p)
{
    const DRIVE_PROFILE* top[PROF_TOP_MAX];
    const DRIVE_PROFILE* first = NULL;
    const DRIVE_PROFILE* second = NULL;
    const DRIVE_PROFILE* winner;
    int nTies = 0, nStored = 0, score = 0;

    if (!p) return;
    p->nProfileScore = 0;
    p->szProfileId[0] = '\0';
    p->bProfileAmbiguous = FALSE;
    p->szProfileConflict[0] = '\0';
    p->szProfileOrigin[0] = '\0';
    p->nProfileFlags = 0;

    FillTop(p, top, PROF_TOP_MAX, &nTies, &nStored, &first, &second, &score);
    winner = first ? first : GenericFor(p);
    if (!winner) {
        DriveDbApply(p);
        return;
    }
    if (!first)
        score = PROF_SCORE_GENERIC;
    ApplyIdentity(p, winner, score, first ? nTies : 0, top, nStored, second);
    DriveDbApply(p);
}

int DriveProfileScore(const DRIVE_INFO* p)
{
    if (!p) return 0;
    return p->nProfileScore;
}

BOOL ProfileRegister(const PROFILE_DEF* d)
{
    DRIVE_PROFILE* pr;
    if (!d || !d->id || !d->id[0]) return FALSE;
    if (g_nExtra >= PROF_EXTRA_MAX) return FALSE;
    if (ProfileById(d->id)) return FALSE;
    pr = &g_extra[g_nExtra];
    memset(pr, 0, sizeof(*pr));
    pr->id = d->id;
    pr->extends = d->extends;
    pr->kind = d->kind;
    pr->model = d->model;
    pr->fw = d->fw;
    pr->fwMode = d->fwMode;
    pr->vendor = d->vendor;
    pr->ctl = d->ctl;
    pr->nvme = d->nvme;
    pr->hdd = d->hdd;
    pr->chip = d->chip;
    pr->nvmeVer = d->nvmeVer;
    pr->attrs = d->attrs;
    pr->flags = d->flags;
    pr->origin = (d->source && d->source[0]) ? d->source : "registered";
    g_nExtra++;
    return TRUE;
}

void ProfileClearRegistered(void)
{
    g_nExtra = 0;
}

BOOL ProfileListsAttr(const DRIVE_INFO* p, BYTE id)
{
    const DRIVE_PROFILE* g;
    const PROF_ATTR* a = NULL;
    int c = ConfirmLocal(p, id, &a);
    if (c == CONF_REFUSE || c == CONF_FOUND) return TRUE;
    g = GenericFor(p);
    return g && FindProfAttr(g->attrs, id) != NULL;
}

BOOL ProfileLookup(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    const PROF_ATTR* bestA = NULL;
    int dsem = PROF_SEM_NONE, denc = PROF_ENC_NONE;
    int c;
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (!p || id == 0) return FALSE;

    c = ConfirmLocal(p, id, &bestA);
    if (c == CONF_REFUSE) return FALSE;
    if (c == CONF_FOUND && bestA) {
        if (sem) *sem = (int)bestA->sem;
        if (enc) *enc = (int)bestA->enc;
        return TRUE;
    }
    if (DriveDbLookup(p, id, &dsem, &denc)) {
        if (sem) *sem = dsem;
        if (enc) *enc = denc;
        return TRUE;
    }
    {
        const DRIVE_PROFILE* g = GenericFor(p);
        bestA = g ? FindProfAttr(g->attrs, id) : NULL;
    }
    if (!bestA) return FALSE;
    if (sem) *sem = (int)bestA->sem;
    if (enc) *enc = (int)bestA->enc;
    return TRUE;
}

int ProfileAttrPref(const DRIVE_INFO* p, BYTE id)
{
    const PROF_ATTR* a = NULL;
    int c;
    if (!p || id == 0) return 0;
    c = ConfirmLocal(p, id, &a);
    if (c == CONF_REFUSE) return 0;
    if (c == CONF_FOUND && a) return (int)a->pref;
    /* A drivedb row has no preference. */
    if (DriveDbLookup(p, id, NULL, NULL)) return 0;
    {
        const DRIVE_PROFILE* g = GenericFor(p);
        a = g ? FindProfAttr(g->attrs, id) : NULL;
    }
    if (!a) return 0;
    return (int)a->pref;
}

BOOL ProfileAttrLabel(const DRIVE_INFO* p, BYTE id,
                      int* sem, int* enc,
                      const char** nameRu, const char** nameEn,
                      unsigned char* altUnless,
                      const char** altRu, const char** altEn)
{
    const PROF_ATTR* a = NULL;
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (nameRu) *nameRu = NULL;
    if (nameEn) *nameEn = NULL;
    if (altUnless) *altUnless = 0;
    if (altRu) *altRu = NULL;
    if (altEn) *altEn = NULL;
    if (ConfirmLocal(p, id, &a) != CONF_FOUND || !a) return FALSE;
    if (!a->nameRu && !a->nameEn) return FALSE;
    if (sem) *sem = (int)a->sem;
    if (enc) *enc = (int)a->enc;
    if (nameRu) *nameRu = a->nameRu;
    if (nameEn) *nameEn = a->nameEn;
    if (altUnless) *altUnless = a->altUnless;
    if (altRu) *altRu = a->altRu;
    if (altEn) *altEn = a->altEn;
    return TRUE;
}

int ProfileWriteVolume(int sem, int enc)
{
    if (sem != PROF_SEM_HOST_WRITE && sem != PROF_SEM_NAND_WRITE)
        return 0;
    return enc == PROF_ENC_GIB || enc == PROF_ENC_BLOCK32 ||
           enc == PROF_ENC_SCALE || enc == PROF_ENC_LBA;
}

int ProfileEraseCount(int sem, int enc)
{
    if (sem != PROF_SEM_ERASE) return 0;
    /* COUNTER means "this is an erase count". It does not name the
     * average. AVG, MAX, and MIN do. */
    return enc == PROF_ENC_COUNTER || enc == PROF_ENC_MAXAVG ||
           enc == PROF_ENC_AVG || enc == PROF_ENC_MAX || enc == PROF_ENC_MIN;
}

static int StatusRank(int e)
{
    switch (e) {
    case HEALTH_STATUS_GOOD:     return 1;
    case HEALTH_STATUS_OBSERVE:  return 2;
    case HEALTH_STATUS_CAUTION:  return 3;
    case HEALTH_STATUS_BAD:      return 4;
    case HEALTH_STATUS_WARNING:  return 4;
    case HEALTH_STATUS_CRITICAL: return 5;
    default:                     return 0;
    }
}

static void NormalizeHealth(PROF_HEALTH* h)
{
    if (!h || h->rule == PROF_RULE_OFF) return;
    if (h->sem != PROF_SEM_MEDIA && h->sem != PROF_SEM_INTERFACE) {
        h->rule = PROF_RULE_OFF;
        return;
    }
    if (h->rule == PROF_RULE_DEFAULT) {
        h->rule = PROF_RULE_WORSE;
        if (h->observe == 0 && h->caution == 0 && h->bad == 0 && h->badHdd == 0)
            h->caution = 1;
    }
}

static void FillHealth(PROF_HEALTH* o, const PROF_ATTR* a, int fallback)
{
    memset(o, 0, sizeof(*o));
    o->sem = (int)a->sem;
    o->enc = (int)a->enc;
    o->rule = (int)a->rule;
    o->slot = (int)a->slot;
    o->scope = (int)a->scope;
    o->observe = (int)a->observe;
    o->caution = (int)a->caution;
    o->bad = (int)a->bad;
    o->badHdd = (int)a->badHdd;
    o->fallback = fallback;
    NormalizeHealth(o);
}

BOOL ProfileHealthRule(const DRIVE_INFO* p, BYTE id, PROF_HEALTH* out)
{
    const PROF_ATTR* a = NULL;
    int c, sem = PROF_SEM_NONE, enc = PROF_ENC_NONE;
    if (out) memset(out, 0, sizeof(*out));
    if (!p || !out || id == 0) return FALSE;
    c = ConfirmLocal(p, id, &a);
    if (c == CONF_REFUSE) return FALSE;
    if (c == CONF_FOUND && a) {
        FillHealth(out, a, 0);
        return TRUE;
    }
    {
        const DRIVE_PROFILE* g = GenericFor(p);
        a = g ? FindProfAttr(g->attrs, id) : NULL;
    }
    if (a) {
        FillHealth(out, a, 0);
        return TRUE;
    }
    if (!DriveDbLookup(p, id, &sem, &enc)) return FALSE;
    out->sem = sem;
    out->enc = enc;
    /* The name is a hint, not a confirmed counter layout. A packed RAW
     * (raw24/raw32, raw16(raw16)) is not one number, so it is not graded.
     * A plain counter stops at Risk on the row: count >= 1 is observe,
     * never caution, and it does not enter the media channel. */
    if (enc == PROF_ENC_PACKED || enc == PROF_ENC_PAIR || enc == PROF_ENC_MAXAVG) {
        out->rule = PROF_RULE_OFF;
        return TRUE;
    }
    out->rule = PROF_RULE_WORSE;
    out->observe = 1;
    /* Name only. The row can say Risk. The media channel, which the
     * headline copies, stays on confirmed rules. */
    out->fallback = 1;
    if (DriveDbAttrIsCrc(p, id) && enc == PROF_ENC_COUNTER) {
        out->sem = PROF_SEM_INTERFACE;
        out->slot = PROF_SLOT_CRC;
    }
    NormalizeHealth(out);
    return TRUE;
}

int ProfileCountRank(int count, int observe, int caution, int bad,
                     int badHdd, int isHdd)
{
    int r = HEALTH_STATUS_GOOD;
    int b = bad;
    if (count < 0) return HEALTH_STATUS_UNKNOWN;
    if (isHdd && badHdd > 0 && (b == 0 || badHdd < b))
        b = badHdd;
    if (observe > 0 && count >= observe)
        r = HEALTH_STATUS_OBSERVE;
    if (caution > 0 && count >= caution &&
        StatusRank(HEALTH_STATUS_CAUTION) > StatusRank(r))
        r = HEALTH_STATUS_CAUTION;
    if (b > 0 && count >= b && StatusRank(HEALTH_STATUS_BAD) > StatusRank(r))
        r = HEALTH_STATUS_BAD;
    return r;
}

int ProfileDecodeRaw(int enc, unsigned __int64 raw, unsigned __int64* out)
{
    if (!out) return 0;
    *out = 0;
    switch (enc) {
    case PROF_ENC_PERCENT:
        if (raw > 100ULL) return 0;
        *out = raw;
        return 1;
    case PROF_ENC_COUNTER:
    case PROF_ENC_AVG:
    case PROF_ENC_MAX:
    case PROF_ENC_MIN:
        *out = raw;
        return 1;
    case PROF_ENC_LO16:
        *out = raw & 0xFFFFULL;
        return 1;
    case PROF_ENC_GIB:
        if (raw == 0 || raw > 4000000ULL) return 0;
        *out = raw;
        return 1;
    case PROF_ENC_BLOCK32:
        if (raw == 0) return 0;
        if (raw > (4000000ULL * 1024ULL) / 32ULL) return 0;
        *out = (raw * 32ULL) / 1024ULL;
        return 1;
    case PROF_ENC_LBA:
        if (raw > (0xFFFFFFFFFFFFFFFFULL / 512ULL)) return 0;
        *out = raw;
        return 1;
    case PROF_ENC_MAXAVG:
    case PROF_ENC_PAIR:
        if (raw > 0xFFFFFFFFULL) return 0;
        *out = raw;
        return 1;
    case PROF_ENC_VALUE:
    case PROF_ENC_PACKED:
    case PROF_ENC_SCALE:
    case PROF_ENC_NONE:
    default:
        return 0;
    }
}

int ProfileRawDelta(unsigned __int64 prev, unsigned __int64 now, long long* delta)
{
    unsigned __int64 d;
    if (!delta) return 0;
    *delta = 0;
    if (now < prev) return 0;
    d = now - prev;
    if (d > 0x7FFFFFFFFFFFFFFFULL) return 0;
    *delta = (long long)d;
    return 1;
}

void FormatProfileDiagnostic(const DRIVE_INFO* p, char* buf, int nBuf)
{
    if (!buf || nBuf <= 0) return;
    buf[0] = '\0';
    if (!p) return;
    safe_snprintf_n(buf, nBuf,
        "profile=%s score=%d ambiguous=%d conflict=%s origin=%s flags=%d",
        p->szProfileId[0] ? p->szProfileId : "-",
        p->nProfileScore,
        p->bProfileAmbiguous ? 1 : 0,
        p->szProfileConflict[0] ? p->szProfileConflict : "-",
        p->szProfileOrigin[0] ? p->szProfileOrigin : "-",
        p->nProfileFlags);
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

BOOL DriveIsHdd(const DRIVE_INFO* p)
{
    return p && p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe;
}

BOOL FirmwareIsSmi225x(const char* fw)
{
    int i;
    if (!fw) return FALSE;
    for (i = 0; i < 7; i++) {
        if (!fw[i]) return FALSE;
    }
    if (fw[7] != '\0') return FALSE;
    if (!isalpha((unsigned char)fw[0]) || !isalpha((unsigned char)fw[5]))
        return FALSE;
    if (!isdigit((unsigned char)fw[1]) || !isdigit((unsigned char)fw[2]) ||
        !isdigit((unsigned char)fw[3]) || !isdigit((unsigned char)fw[4]) ||
        !isdigit((unsigned char)fw[6]))
        return FALSE;
    if (strncmp(fw, "U11", 3) == 0 || strncmp(fw, "U10", 3) == 0 ||
        strncmp(fw, "T07", 3) == 0)
        return FALSE;
    if (fw[0] == 'S' && fw[1] == '1' && fw[2] == '1' &&
        fw[3] >= '0' && fw[3] <= '9')
        return FALSE;
    return TRUE;
}
