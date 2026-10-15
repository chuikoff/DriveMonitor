/* DriveMonitor - smartmontools drive database. GPL-2.0-or-later. See LICENSE.
 * Data: third_party/smartmontools/drivedb.h
 *   Copyright (C) 2003-11 Philip Williams, Bruce Allen
 *   Copyright (C) 2008-26 Christian Franke
 *   SPDX-License-Identifier: GPL-2.0-or-later
 *   VERSION: 8.0  https://www.smartmontools.org
 * POSIX extended regexes match the whole model and, when present, firmware.
 * Among rows that match, the highest feature score wins. An equal score
 * keeps the earlier row, as smartmontools keeps the first match. That
 * other family is recorded and shown. An attribute both rows name with
 * a different semantic or encoding is not applied. This is not the
 * local-profile rule, which refuses a tier when score and specificity
 * are both equal. A row whose family names a different controller is
 * skipped when this program already knows the controller. -d device-type
 * options are not sent. USB bridge rows are not probes. */
#include "smart_internal.h"

#include <regex>
#include <vector>
#include <stdlib.h>
#include <string.h>

struct drive_settings {
    const char* modelfamily;
    const char* modelregexp;
    const char* firmwareregexp;
    const char* warningmsg;
    const char* presets;
};

static const drive_settings kDriveDb[] = {
#include "../third_party/smartmontools/drivedb.h"
#ifdef DM_TESTS
    /* Test rows only. DriveMonitor.exe is built without DM_TESTS.
     * drivedb_test.exe links a second object compiled with -DDM_TESTS.
     * Not an upstream row. Two exact matches of ZZTIEPROBE, so a tie can
     * be tested without editing drivedb.h. A real disk is not named that.
     * The earlier row stays the label. 231 disagrees and is dropped.
     * 5 agrees and stays. 232 exists only on the later row and is not
     * imported. */
    { "Tie Probe Earlier",
      "ZZTIEPROBE",
      "", "",
      "-v 231,raw48,Lifetime_Left "
      "-v 5,raw48,Reallocated_Sector_Ct "
    },
    { "Tie Probe Later",
      "ZZTIEPROBE",
      "", "",
      "-v 231,raw48,Total_Erase_Count "
      "-v 5,raw48,Reallocated_Sector_Ct "
      "-v 232,raw48,Lifetime_Left "
    },
    /* A packed RAW must not be scored as one count, and a plain name
     * hint must not become Caution from 1. */
    { "Packed Name Probe",
      "ZZPACKPROBE",
      "", "",
      "-v 160,raw24/raw32,Uncorrectable_Error_Cnt "
      "-v 161,raw16(raw16),Reallocated_Block_Ct "
      "-v 170,raw48,Uncorrectable_ECC_Cnt "
      "-v 218,raw24/raw32,CRC_Error_Count "
    },
    /* A name longer than the 43-character store must still reach the
     * HDD/SSD flag after the comma. */
    { "Long Name Probe",
      "ZZLONGNAME",
      "", "",
      "-v 160,raw48,ABCDEFGHIJ_ABCDEFGHIJ_ABCDEFGHIJ_ABCDEFGHIJX,HDD "
      "-v 161,raw48,ABCDEFGHIJ_ABCDEFGHIJ_ABCDEFGHIJ_ABCDEFGHIJX,SSD "
      "-v 162,raw48,Short_Name,HDD "
    },
    /* Equal score, empty family. 231 disagrees and is dropped. The
     * lecture must say so: there is no family name to print. */
    { "",
      "ZZTIEANON",
      "", "",
      "-v 231,raw48,Lifetime_Left "
      "-v 5,raw48,Reallocated_Sector_Ct "
    },
    { "Anon Tie Later",
      "ZZTIEANON",
      "", "",
      "-v 231,raw48,Total_Erase_Count "
      "-v 5,raw48,Reallocated_Sector_Ct "
    },
    { "Anon Tie Earlier",
      "ZZTIENONE",
      "", "",
      "-v 231,raw48,Lifetime_Left "
      "-v 5,raw48,Reallocated_Sector_Ct "
    },
    { "",
      "ZZTIENONE",
      "", "",
      "-v 231,raw48,Total_Erase_Count "
      "-v 5,raw48,Reallocated_Sector_Ct "
    },
#endif
};

enum { DRIVEDB_N = (int)(sizeof(kDriveDb) / sizeof(kDriveDb[0])) };

struct DbRx {
    bool tried;
    bool modelOk;
    bool fwAny;
    bool fwOk;
    std::regex model;
    std::regex fw;
};

static std::vector<DbRx> gRx;
static INIT_ONCE gDbOnce = INIT_ONCE_STATIC_INIT;

static void TrimCopy(char* dst, int cap, const char* src)
{
    int n, i;
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    n = 0;
    while (src[n] && n < cap - 1) {
        dst[n] = src[n];
        n++;
    }
    dst[n] = '\0';
    i = n;
    while (i > 0 && (dst[i - 1] == ' ' || dst[i - 1] == '\t')) {
        dst[i - 1] = '\0';
        i--;
    }
}

static bool EntrySkipped(const drive_settings* e)
{
    if (!e->modelregexp || !e->modelregexp[0]) return true;
    if (strcmp(e->modelregexp, "-") == 0) return true;
    if (e->modelfamily && e->modelfamily[0] == '$') return true;
    if (e->modelfamily && strncmp(e->modelfamily, "USB:", 4) == 0) return true;
    return false;
}

static bool CompilePat(std::regex* out, const char* pat)
{
    try {
        out->assign(pat, std::regex::extended | std::regex::nosubs);
        return true;
    } catch (...) {
        return false;
    }
}

/* One compiler. Other callers wait until the table is published.
 * FALSE leaves the once unmarked, so a later call tries again.
 * A throw must not escape: InitOnce is a C callback. */
static BOOL CALLBACK CompileDbOnce(PINIT_ONCE once, PVOID param, PVOID* ctx)
{
    int i;
    (void)once;
    (void)param;
    (void)ctx;
    try {
        gRx.resize((size_t)DRIVEDB_N);
        for (i = 0; i < DRIVEDB_N; i++) {
            DbRx* rx = &gRx[(size_t)i];
            const drive_settings* e = &kDriveDb[i];
            rx->tried = true;
            rx->modelOk = false;
            rx->fwAny = true;
            rx->fwOk = false;
            if (EntrySkipped(e)) continue;
            rx->modelOk = CompilePat(&rx->model, e->modelregexp);
            if (e->firmwareregexp && e->firmwareregexp[0]) {
                rx->fwAny = false;
                rx->fwOk = CompilePat(&rx->fw, e->firmwareregexp);
            }
        }
    } catch (...) {
        return FALSE;
    }
    return TRUE;
}

/* TRUE when every row has been compiled, including rows that failed
 * their own pattern and stay unmatched. */
static bool CompileDb(void)
{
    return InitOnceExecuteOnce(&gDbOnce, CompileDbOnce, NULL, NULL) != FALSE;
}

static bool MatchEntry(int i, const char* model, const char* fw)
{
    const DbRx* rx;
    if (i < 0 || i >= DRIVEDB_N) return false;
    rx = &gRx[(size_t)i];
    if (!rx->modelOk || !model || !model[0]) return false;
    try {
        if (!std::regex_match(model, rx->model)) return false;
        if (rx->fwAny) return true;
        if (!rx->fwOk || !fw) return false;
        return std::regex_match(fw, rx->fw);
    } catch (...) {
        return false;
    }
}

static void LowerCopy(char* dst, int cap, const char* src)
{
    int i;
    if (!dst || cap <= 0) return;
    dst[0] = '\0';
    if (!src) return;
    for (i = 0; src[i] && i < cap - 1; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        dst[i] = c;
    }
    dst[i] = '\0';
}

static const struct { const char* needle; int ctl; } kFamilyCtl[] = {
    { "phison",         CONTROLLER_PHISON },
    { "silicon motion", CONTROLLER_SMI },
    { "sandforce",      -1 },
    { "samsung",        CONTROLLER_SAMSUNG },
    { "marvell",        CONTROLLER_MARVELL },
    { "intel",          CONTROLLER_INTEL },
    { "micron",         CONTROLLER_MICRON },
    { "crucial",        CONTROLLER_MICRON },
    { "hynix",          CONTROLLER_HYNIX },
    { "kioxia",         CONTROLLER_KIOXIA },
    { "sandisk",        CONTROLLER_SANDISK },
    { "realtek",        CONTROLLER_REALTEK },
    { "seagate",        CONTROLLER_SEAGATE },
    { "western digital", CONTROLLER_WD },
    { "toshiba",        CONTROLLER_TOSHIBA },
    { "hitachi",        CONTROLLER_HITACHI },
    { "hgst",           CONTROLLER_HITACHI },
    { "maxio",          CONTROLLER_MAXIO },
    { "innogrit",       CONTROLLER_INNOGRIT }
};

static void FamilyControllerBits(DRIVE_CONTROLLER ctl, const char* family,
                                 bool* ours, bool* other)
{
    char low[96];
    unsigned i;
    *ours = false;
    *other = false;
    if (ctl == CONTROLLER_UNKNOWN) return;
    LowerCopy(low, (int)sizeof(low), family);
    if (!low[0]) return;
    for (i = 0; i < sizeof(kFamilyCtl) / sizeof(kFamilyCtl[0]); i++) {
        if (!strstr(low, kFamilyCtl[i].needle)) continue;
        if (kFamilyCtl[i].ctl == (int)ctl) *ours = true;
        else *other = true;
    }
}

/* Family text that names some other silicon is not applied on top of a
 * controller this program already identified. */
static bool FamilyAgrees(DRIVE_CONTROLLER ctl, const char* family)
{
    bool ours, other;
    if (ctl == CONTROLLER_UNKNOWN) return true;
    FamilyControllerBits(ctl, family, &ours, &other);
    if (!family || !family[0]) return true;
    if (ours) return true;
    return !other;
}

static bool HasCI(const char* low, const char* tok)
{
    return low && tok && strstr(low, tok) != NULL;
}

/* raw24/raw32, raw16(raw16), raw24(raw8): two fields in one RAW.
 * A plain raw48/raw16 has neither marker. */
static int RawFormatComposite(const char* fmt)
{
    if (!fmt || !fmt[0]) return 0;
    return strchr(fmt, '(') != NULL || strchr(fmt, '/') != NULL;
}

/* A name can say "uncorrectable" while the preset stores two fields.
 * That RAW is not a 48-bit count. */
static int NameHintEnc(const char* fmt)
{
    return RawFormatComposite(fmt) ? PROF_ENC_PACKED : PROF_ENC_COUNTER;
}

static void MapAttr(const char* fmt, const char* name, int* sem, int* enc)
{
    char low[44];
    bool write, read, nand, life;
    *sem = PROF_SEM_NONE;
    *enc = PROF_ENC_COUNTER;
    if (!name || !name[0]) {
        *enc = PROF_ENC_NONE;
        return;
    }
    LowerCopy(low, (int)sizeof(low), name);
    if (fmt && (strstr(fmt, "temp") != NULL)) {
        *sem = PROF_SEM_THERMAL;
        return;
    }
    write = HasCI(low, "write");
    read = HasCI(low, "read");
    nand = HasCI(low, "nand") || HasCI(low, "flash") || HasCI(low, "tlc_") ||
           HasCI(low, "slc_");
    if (HasCI(low, "32mib") || HasCI(low, "32mb")) {
        *enc = PROF_ENC_BLOCK32;
        if (nand) *sem = PROF_SEM_NAND_WRITE;
        else if (read && !write) *sem = PROF_SEM_HOST_READ;
        else if (write) *sem = PROF_SEM_HOST_WRITE;
        return;
    }
    if (HasCI(low, "gib") || HasCI(low, "_gb") || HasCI(low, "gb")) {
        *enc = PROF_ENC_GIB;
        if (nand) *sem = PROF_SEM_NAND_WRITE;
        else if (read && !write) *sem = PROF_SEM_HOST_READ;
        else if (write) *sem = PROF_SEM_HOST_WRITE;
        return;
    }
    if (HasCI(low, "lba")) {
        *enc = PROF_ENC_LBA;
        if (nand) *sem = PROF_SEM_NAND_WRITE;
        else if (read && !write) *sem = PROF_SEM_HOST_READ;
        else if (write) *sem = PROF_SEM_HOST_WRITE;
        return;
    }
    life = HasCI(low, "percent_lifetime_remain") ||
           HasCI(low, "remaining_lifetime") ||
           HasCI(low, "lifetime_left") ||
           HasCI(low, "life_left") ||
           HasCI(low, "remaining_life");
    if (life && !HasCI(low, "used") && !HasCI(low, "consumed")) {
        *sem = PROF_SEM_ENDURANCE;
        *enc = PROF_ENC_PERCENT;
        return;
    }
    if ((HasCI(low, "erase_count") || HasCI(low, "erase_ct")) &&
        !HasCI(low, "fail")) {
        *sem = PROF_SEM_ERASE;
        *enc = (fmt && strstr(fmt, "avg16") != NULL)
            ? PROF_ENC_MAXAVG : PROF_ENC_COUNTER;
        return;
    }
    if (HasCI(low, "wear")) {
        *sem = PROF_SEM_WEAR;
        *enc = HasCI(low, "percent") ? PROF_ENC_PERCENT : PROF_ENC_COUNTER;
        return;
    }
    if (HasCI(low, "crc") && !HasCI(low, "fail")) {
        *sem = PROF_SEM_INTERFACE;
        *enc = NameHintEnc(fmt);
        return;
    }
    if (HasCI(low, "realloc") || HasCI(low, "pending_sector") ||
        HasCI(low, "current_pending") || HasCI(low, "uncorrect")) {
        *sem = PROF_SEM_MEDIA;
        *enc = NameHintEnc(fmt);
        return;
    }
    /* A raw format is still a decode. Health scoring ignores sem NONE.
     * Unknown_* stays unnamed in GetAttrDecode; the encoding is only
     * used when the name itself is a real label. */
    if (fmt && strstr(fmt, "avg16") != NULL)
        *enc = PROF_ENC_MAXAVG;
    else if (fmt && (strstr(fmt, "raw16(raw16)") != NULL ||
                     strstr(fmt, "raw24/raw24") != NULL))
        *enc = PROF_ENC_PAIR;
    else if (RawFormatComposite(fmt))
        *enc = PROF_ENC_PACKED;
    else if (strncmp(fmt ? fmt : "", "raw", 3) == 0)
        *enc = PROF_ENC_COUNTER;
    else
        *enc = PROF_ENC_NONE;
}

static bool HealthSem(int sem)
{
    return sem == PROF_SEM_ENDURANCE || sem == PROF_SEM_HOST_WRITE ||
           sem == PROF_SEM_HOST_READ || sem == PROF_SEM_NAND_WRITE ||
           sem == PROF_SEM_ERASE || sem == PROF_SEM_WEAR ||
           sem == PROF_SEM_MEDIA || sem == PROF_SEM_INTERFACE;
}

struct AttrBag {
    DRIVEDB_ATTR* attr;
    int* n;
    int cap;
};

static void StoreAttr(AttrBag* b, unsigned id, int sem, int enc, const char* name)
{
    DRIVEDB_ATTR* a;
    int i, n;
    if (!b || !b->attr || !b->n) return;
    if (!name || !name[0] || id == 0 || id > 255) return;
    if (*b->n >= b->cap || *b->n >= DRIVEDB_ATTR_MAX) return;
    for (i = 0; i < *b->n; i++) {
        if (b->attr[i].id == (BYTE)id) {
            a = &b->attr[i];
            goto fill;
        }
    }
    a = &b->attr[*b->n];
    (*b->n)++;
fill:
    a->id = (BYTE)id;
    a->sem = (BYTE)sem;
    a->enc = (BYTE)enc;
    n = 0;
    while (name[n] && n < (int)sizeof(a->name) - 1) {
        char c = name[n];
        a->name[n] = (c == '_') ? ' ' : c;
        n++;
    }
    a->name[n] = '\0';
}

static void ParsePresets(AttrBag* b, const char* presets, BOOL hdd)
{
    const char* s;
    if (!presets) return;
    s = presets;
    while ((s = strstr(s, "-v")) != NULL) {
        unsigned long id;
        char* end;
        char fmt[48], name[44], flag[8];
        int sem, enc, n;
        const char* fmtStart;
        if (s != presets && s[-1] != ' ' && s[-1] != '\t') {
            s += 2;
            continue;
        }
        s += 2;
        while (*s == ' ' || *s == '\t') s++;
        id = strtoul(s, &end, 0);
        if (end == s || id == 0 || id > 255) {
            if (end != s) s = end;
            else s++;
            continue;
        }
        s = end;
        if (*s != ',') continue;
        s++;
        fmtStart = s;
        n = 0;
        while (*s && *s != ',' && *s != ' ' && *s != '\t' && n < (int)sizeof(fmt) - 1) {
            fmt[n++] = *s++;
        }
        fmt[n] = '\0';
        name[0] = '\0';
        flag[0] = '\0';
        (void)fmtStart;
        if (*s == ',') {
            s++;
            n = 0;
            /* name[] keeps 43 characters. The rest of a longer preset
             * name is skipped through to the comma, so ,HDD or ,SSD
             * is still read. */
            while (*s && *s != ',' && *s != ' ' && *s != '\t') {
                if (n < (int)sizeof(name) - 1)
                    name[n++] = *s;
                s++;
            }
            name[n] = '\0';
            if (*s == ',') {
                s++;
                n = 0;
                while (*s && *s != ' ' && *s != '\t' && n < (int)sizeof(flag) - 1)
                    flag[n++] = *s++;
                flag[n] = '\0';
            }
        }
        if (!name[0]) continue;
        if (strcmp(flag, "HDD") == 0 && !hdd) continue;
        if (strcmp(flag, "SSD") == 0 && hdd) continue;
        MapAttr(fmt, name, &sem, &enc);
        StoreAttr(b, (unsigned)id, sem, enc, name);
    }
}

/* An id both rows name is contested when the semantic or the encoding
 * differs. The same meaning keeps the earlier name. An id only one row
 * names is not a vote: the earlier row's set stays, and the later row's
 * extra ids are not imported. */
static void DropContested(DRIVE_INFO* p, const DRIVEDB_ATTR* extra, int nExtra)
{
    int i = 0;
    if (!p || !extra || nExtra <= 0) return;
    while (i < p->nDriveDbAttrs) {
        int j, contested = 0;
        for (j = 0; j < nExtra; j++) {
            if (extra[j].id != p->driveDbAttr[i].id) continue;
            if (extra[j].sem != p->driveDbAttr[i].sem ||
                extra[j].enc != p->driveDbAttr[i].enc)
                contested = 1;
            break;
        }
        if (!contested) {
            i++;
            continue;
        }
        if (i + 1 < p->nDriveDbAttrs) {
            memmove(&p->driveDbAttr[i], &p->driveDbAttr[i + 1],
                    (size_t)(p->nDriveDbAttrs - i - 1) * sizeof(DRIVEDB_ATTR));
        }
        p->nDriveDbAttrs--;
        memset(&p->driveDbAttr[p->nDriveDbAttrs], 0, sizeof(DRIVEDB_ATTR));
    }
}

static const DRIVEDB_ATTR* FindDbAttr(const DRIVE_INFO* p, BYTE id)
{
    int i;
    if (!p || id == 0) return NULL;
    for (i = 0; i < p->nDriveDbAttrs; i++) {
        if (p->driveDbAttr[i].id == id) return &p->driveDbAttr[i];
    }
    return NULL;
}

/* Pattern shape. This is not a regex-complexity measure.
 * exact  — no metacharacters
 * narrow — one product line: classes, groups, or a single trailing .*
 * wide   — a leading wildcard, .* that is not the whole tail, or a
 *          catch-all such as .*SSD.*
 * run is the longest stretch of literal characters. */
enum { DB_SHAPE_EXACT = 0, DB_SHAPE_NARROW = 1, DB_SHAPE_WIDE = 2 };

struct DbShape {
    int shape;
    int run;
    bool generic;
};

static bool ContainsCI(const char* s, const char* tok)
{
    int i, j;
    if (!s || !tok || !tok[0]) return false;
    for (i = 0; s[i]; i++) {
        for (j = 0; tok[j]; j++) {
            char a = s[i + j];
            char b = tok[j];
            if (!a) return false;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
        }
        if (!tok[j]) return true;
    }
    return false;
}

static bool IsReMeta(char c)
{
    return c == '.' || c == '*' || c == '+' || c == '?' || c == '[' ||
           c == ']' || c == '(' || c == ')' || c == '{' || c == '}' ||
           c == '|' || c == '^' || c == '$';
}

static DbShape InspectPattern(const char* pat)
{
    DbShape d;
    int i, run, best;
    bool meta, leadWild, dotStar, dotStarTail;
    d.shape = DB_SHAPE_EXACT;
    d.run = 0;
    d.generic = false;
    if (!pat || !pat[0]) return d;
    if (ContainsCI(pat, ".*ssd") || ContainsCI(pat, ".*hdd") ||
        ContainsCI(pat, ".*disk") || ContainsCI(pat, ".*drive") ||
        ContainsCI(pat, ".*sata"))
        d.generic = true;

    i = 0;
    run = 0;
    best = 0;
    meta = false;
    leadWild = false;
    dotStar = false;
    dotStarTail = true;
    if (pat[0] == '^') {
        meta = true;
        i = 1;
    }
    if (pat[i] == '.') leadWild = true;

    while (pat[i]) {
        if (pat[i] == '\\' && pat[i + 1]) {
            run++;
            i += 2;
            continue;
        }
        if (pat[i] == '.' && (pat[i + 1] == '*' || pat[i + 1] == '+')) {
            int j = i + 2;
            meta = true;
            dotStar = true;
            if (run > best) best = run;
            run = 0;
            if (pat[j] == '$') j++;
            if (pat[j] != '\0') dotStarTail = false;
            i += 2;
            continue;
        }
        if (IsReMeta(pat[i])) {
            meta = true;
            if (run > best) best = run;
            run = 0;
            i++;
            continue;
        }
        run++;
        i++;
    }
    if (run > best) best = run;
    d.run = best;
    if (!meta) d.shape = DB_SHAPE_EXACT;
    else if (d.generic || leadWild || (dotStar && !dotStarTail))
        d.shape = DB_SHAPE_WIDE;
    else d.shape = DB_SHAPE_NARROW;
    return d;
}

static bool FirmwareConstrains(const char* fw)
{
    if (!fw || !fw[0]) return false;
    if (strcmp(fw, ".*") == 0 || strcmp(fw, ".+") == 0 ||
        strcmp(fw, "^.*$") == 0 || strcmp(fw, "^.*") == 0)
        return false;
    return true;
}

/* 1 = SSD/NVMe, 2 = HDD, 0 = the family does not say. */
static int FamilyDeviceKind(const char* family)
{
    char low[96];
    bool ssd, hdd;
    LowerCopy(low, (int)sizeof(low), family);
    ssd = strstr(low, "ssd") != NULL || strstr(low, "nvme") != NULL;
    hdd = strstr(low, "hdd") != NULL || strstr(low, "hard disk") != NULL ||
          strstr(low, "harddisk") != NULL;
    if (ssd == hdd) return 0;
    return ssd ? 1 : 2;
}

static int DriveDeviceKind(const DRIVE_INFO* p)
{
    if (!p) return 0;
    if (p->bIsNVMe || p->eType == DRIVE_TYPE_NVME ||
        p->eType == DRIVE_TYPE_SSD_SATA || p->eType == DRIVE_TYPE_M2_SATA ||
        p->wRotationRate == 0x0001)
        return 1;
    if (p->eType == DRIVE_TYPE_HDD || p->wRotationRate >= 0x0401)
        return 2;
    return 0;
}

/* Score of a row that already matched. Bases follow the model shape;
 * a matched firmware constraint, our controller, and the device type
 * add to that. .*SSD.* and the other catch-alls are penalized apart
 * from the wide shape. The literal run separates two rows of the same
 * shape (Samsung SSD 870.* loses to Samsung SSD 870 EVO.*). A wide
 * row's run is capped so a long catch-all cannot outrank a product row.
 *
 *   exact model                         85
 *   narrow model                        80
 *   wide model                          60
 *   exact firmware that matched         +15
 *   firmware regex that matched         +10
 *   family names the known controller   +8
 *   family device type agrees           +4
 *   family device type contradicts      -20
 *   catch-all model (.*SSD.* and kin)   -20
 */
static int EntryScore(const DRIVE_INFO* p, const drive_settings* e)
{
    DbShape model;
    int score, run, fk, dk;
    bool ours, other;
    model = InspectPattern(e->modelregexp);
    if (model.shape == DB_SHAPE_EXACT) score = 85;
    else if (model.shape == DB_SHAPE_NARROW) score = 80;
    else score = 60;
    run = model.run;
    if (model.shape == DB_SHAPE_WIDE && run > 9) run = 9;
    else if (run > 40) run = 40;
    score += run;
    if (model.generic) score -= 20;
    if (FirmwareConstrains(e->firmwareregexp)) {
        DbShape fw = InspectPattern(e->firmwareregexp);
        score += (fw.shape == DB_SHAPE_EXACT) ? 15 : 10;
    }
    FamilyControllerBits(p->eController, e->modelfamily, &ours, &other);
    (void)other;
    if (ours) score += 8;
    fk = FamilyDeviceKind(e->modelfamily);
    dk = DriveDeviceKind(p);
    if (fk && dk) score += (fk == dk) ? 4 : -20;
    return score;
}

void DriveDbApply(DRIVE_INFO* p)
{
    char model[48], fw[16];
    int i, t, best, bestScore, nTies, tieOverflow;
    int ties[32];
    BOOL hdd;
    const drive_settings* e;
    AttrBag bag;

    if (!p) return;
    p->szModelFamily[0] = '\0';
    p->szDriveDbWarn[0] = '\0';
    p->szDriveDbTie[0] = '\0';
    p->bDriveDbTieUnnamed = FALSE;
    p->nDriveDbAttrs = 0;
    TrimCopy(model, (int)sizeof(model), p->szModel);
    TrimCopy(fw, (int)sizeof(fw), p->szFirmware);
    if (!model[0]) return;

    if (!CompileDb()) return;
    best = -1;
    bestScore = -1;
    nTies = 0;
    tieOverflow = 0;
    for (i = 0; i < DRIVEDB_N; i++) {
        int score;
        if (EntrySkipped(&kDriveDb[i])) continue;
        if (!FamilyAgrees(p->eController, kDriveDb[i].modelfamily)) continue;
        if (!MatchEntry(i, model, fw)) continue;
        score = EntryScore(p, &kDriveDb[i]);
        /* A higher score replaces the winner. An equal score keeps the
         * earlier row and remembers the other one. */
        if (best < 0 || score > bestScore) {
            best = i;
            bestScore = score;
            nTies = 0;
            tieOverflow = 0;
        } else if (score == bestScore) {
            if (nTies < (int)(sizeof(ties) / sizeof(ties[0])))
                ties[nTies++] = i;
            else
                tieOverflow = 1;
        }
    }
    if (best < 0) return;
    e = &kDriveDb[best];
    if (e->modelfamily && e->modelfamily[0])
        lstrcpynA(p->szModelFamily, e->modelfamily, (int)sizeof(p->szModelFamily));
    if (e->warningmsg && e->warningmsg[0] &&
        strcmp(e->warningmsg, "Default settings") != 0 &&
        strncmp(e->warningmsg, "Version", 7) != 0) {
        /* Family notes are not this disk's verdict. Keep the upstream
         * text. The lecture translates the WD SMR heise note. */
        lstrcpynA(p->szDriveDbWarn, e->warningmsg,
                  (int)sizeof(p->szDriveDbWarn));
    }
    for (t = 0; t < nTies; t++) {
        const char* fam = kDriveDb[ties[t]].modelfamily;
        if (fam && fam[0]) {
            if (!p->szDriveDbTie[0])
                lstrcpynA(p->szDriveDbTie, fam, (int)sizeof(p->szDriveDbTie));
        } else {
            /* An empty family still contests attributes. The lecture
             * needs a mark, because there is no name to print. */
            p->bDriveDbTieUnnamed = TRUE;
        }
    }
    if (nTies > 0 && !(e->modelfamily && e->modelfamily[0]))
        p->bDriveDbTieUnnamed = TRUE;
    if (p->bIsNVMe) return;
    hdd = (p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe);
    bag.attr = p->driveDbAttr;
    bag.n = &p->nDriveDbAttrs;
    bag.cap = DRIVEDB_ATTR_MAX;
    ParsePresets(&bag, e->presets, hdd);
    /* Rows past the ones we stored were not compared. Do not apply a
     * meaning that might be the disputed one. */
    if (tieOverflow) {
        p->nDriveDbAttrs = 0;
        return;
    }
    for (t = 0; t < nTies; t++) {
        DRIVEDB_ATTR extra[DRIVEDB_ATTR_MAX];
        int nExtra = 0;
        AttrBag xb;
        memset(extra, 0, sizeof(extra));
        xb.attr = extra;
        xb.n = &nExtra;
        xb.cap = DRIVEDB_ATTR_MAX;
        ParsePresets(&xb, kDriveDb[ties[t]].presets, hdd);
        DropContested(p, extra, nExtra);
    }
}

const char* DriveDbAttrName(const DRIVE_INFO* p, BYTE id)
{
    const DRIVEDB_ATTR* a = FindDbAttr(p, id);
    if (!a || !a->name[0]) return NULL;
    return a->name;
}

BOOL DriveDbAttrEnc(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    const DRIVEDB_ATTR* a = FindDbAttr(p, id);
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (!a) return FALSE;
    if (sem) *sem = (int)a->sem;
    if (enc) *enc = (int)a->enc;
    return TRUE;
}

BOOL DriveDbLookup(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    const DRIVEDB_ATTR* a = FindDbAttr(p, id);
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (!a || !HealthSem((int)a->sem)) return FALSE;
    if (sem) *sem = (int)a->sem;
    if (enc) *enc = (int)a->enc;
    return TRUE;
}
