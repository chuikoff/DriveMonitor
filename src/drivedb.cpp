/* DriveMonitor - smartmontools drive database. GPL-2.0-or-later. See LICENSE.
 * Data: third_party/smartmontools/drivedb.h
 *   Copyright (C) 2003-11 Philip Williams, Bruce Allen
 *   Copyright (C) 2008-26 Christian Franke
 *   SPDX-License-Identifier: GPL-2.0-or-later
 *   VERSION: 8.0  https://www.smartmontools.org
 * POSIX extended regexes match the whole model and, when present, firmware.
 * A firmware-constrained row wins over an earlier model-only row.
 * A row whose family names a different controller is skipped when this
 * program already knows the controller. -d device-type options are not sent.
 * USB bridge rows are not probes. */
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
static bool gCompiled = false;

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

static void CompileDb(void)
{
    int i;
    if (gCompiled) return;
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
    gCompiled = true;
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

/* Family text that names some other silicon is not applied on top of a
 * controller this program already identified. */
static bool FamilyAgrees(DRIVE_CONTROLLER ctl, const char* family)
{
    static const struct { const char* needle; int ctl; } kKw[] = {
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
    char low[96];
    bool ours = false;
    bool other = false;
    unsigned i;
    if (ctl == CONTROLLER_UNKNOWN) return true;
    LowerCopy(low, (int)sizeof(low), family);
    if (!low[0]) return true;
    for (i = 0; i < sizeof(kKw) / sizeof(kKw[0]); i++) {
        if (!strstr(low, kKw[i].needle)) continue;
        if (kKw[i].ctl == (int)ctl) ours = true;
        else other = true;
    }
    if (ours) return true;
    return !other;
}

static bool HasCI(const char* low, const char* tok)
{
    return low && tok && strstr(low, tok) != NULL;
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
        *enc = PROF_ENC_COUNTER;
        return;
    }
    if (HasCI(low, "wear")) {
        *sem = PROF_SEM_WEAR;
        *enc = HasCI(low, "percent") ? PROF_ENC_PERCENT : PROF_ENC_COUNTER;
        return;
    }
    *enc = PROF_ENC_NONE;
}

static bool HealthSem(int sem)
{
    return sem == PROF_SEM_ENDURANCE || sem == PROF_SEM_HOST_WRITE ||
           sem == PROF_SEM_HOST_READ || sem == PROF_SEM_NAND_WRITE ||
           sem == PROF_SEM_ERASE || sem == PROF_SEM_WEAR;
}

static void StoreAttr(DRIVE_INFO* p, unsigned id, int sem, int enc, const char* name)
{
    DRIVEDB_ATTR* a;
    int i, n;
    if (!name || !name[0] || id == 0 || id > 255) return;
    if (p->nDriveDbAttrs >= DRIVEDB_ATTR_MAX) return;
    for (i = 0; i < p->nDriveDbAttrs; i++) {
        if (p->driveDbAttr[i].id == (BYTE)id) {
            a = &p->driveDbAttr[i];
            goto fill;
        }
    }
    a = &p->driveDbAttr[p->nDriveDbAttrs];
    p->nDriveDbAttrs++;
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

static void ParsePresets(DRIVE_INFO* p, const char* presets, BOOL hdd)
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
            while (*s && *s != ',' && *s != ' ' && *s != '\t' &&
                   n < (int)sizeof(name) - 1) {
                name[n++] = *s++;
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
        if (!HealthSem(sem)) enc = PROF_ENC_NONE;
        StoreAttr(p, (unsigned)id, sem, enc, name);
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

void DriveDbApply(DRIVE_INFO* p)
{
    char model[48], fw[16];
    int i, best, bestSpec;
    BOOL hdd;
    const drive_settings* e;

    if (!p) return;
    p->szModelFamily[0] = '\0';
    p->szDriveDbWarn[0] = '\0';
    p->nDriveDbAttrs = 0;
    TrimCopy(model, (int)sizeof(model), p->szModel);
    TrimCopy(fw, (int)sizeof(fw), p->szFirmware);
    if (!model[0]) return;

    CompileDb();
    best = -1;
    bestSpec = -1;
    for (i = 0; i < DRIVEDB_N; i++) {
        int spec;
        if (EntrySkipped(&kDriveDb[i])) continue;
        if (!FamilyAgrees(p->eController, kDriveDb[i].modelfamily)) continue;
        if (!MatchEntry(i, model, fw)) continue;
        spec = (kDriveDb[i].firmwareregexp && kDriveDb[i].firmwareregexp[0]) ? 1 : 0;
        if (best < 0 || spec > bestSpec) {
            best = i;
            bestSpec = spec;
        }
    }
    if (best < 0) return;
    e = &kDriveDb[best];
    if (e->modelfamily && e->modelfamily[0])
        lstrcpynA(p->szModelFamily, e->modelfamily, (int)sizeof(p->szModelFamily));
    if (e->warningmsg && e->warningmsg[0] &&
        strcmp(e->warningmsg, "Default settings") != 0 &&
        strncmp(e->warningmsg, "Version", 7) != 0)
        lstrcpynA(p->szDriveDbWarn, e->warningmsg, (int)sizeof(p->szDriveDbWarn));
    if (p->bIsNVMe) return;
    hdd = (p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe);
    ParsePresets(p, e->presets, hdd);
}

const char* DriveDbAttrName(const DRIVE_INFO* p, BYTE id)
{
    const DRIVEDB_ATTR* a = FindDbAttr(p, id);
    if (!a || !a->name[0]) return NULL;
    return a->name;
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
