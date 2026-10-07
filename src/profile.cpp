/* DriveMonitor - profile resolver. GPL-2.0-or-later. See LICENSE.
 * Picks the best matching profile. Does not assign a health score.
 * Short firmware prefixes are not profiles. Brand alone is not a controller. */
#include "smart_internal.h"

enum {
    PROF_KIND_EXACT = 1,
    PROF_KIND_FAMILY,
    PROF_KIND_CONTROLLER,
    PROF_KIND_VENDOR,
    PROF_KIND_GENERIC
};

enum {
    PROF_FW_NONE = 0,
    PROF_FW_SUBSTR,
    PROF_FW_SMI225,
    PROF_FW_VE
};

typedef struct _PROF_ATTR {
    BYTE id;
    BYTE sem;
    BYTE enc;
} PROF_ATTR;

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
} DRIVE_PROFILE;

static const PROF_ATTR kAtaGeneric[] = {
    { 0x05, PROF_SEM_MEDIA,     PROF_ENC_COUNTER },
    { 0x09, PROF_SEM_NONE,      PROF_ENC_COUNTER },
    { 0x0C, PROF_SEM_NONE,      PROF_ENC_COUNTER },
    { 0xC2, PROF_SEM_THERMAL,   PROF_ENC_COUNTER },
    { 0xC5, PROF_SEM_MEDIA,     PROF_ENC_COUNTER },
    { 0xC6, PROF_SEM_MEDIA,     PROF_ENC_COUNTER },
    { 0xC7, PROF_SEM_INTERFACE, PROF_ENC_COUNTER },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kPhison[] = {
    { 0xA0, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xA1, PROF_SEM_MEDIA,      PROF_ENC_COUNTER },
    { 0xA3, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xA4, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xA5, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xA6, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xA7, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xA8, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xA9, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT },
    { 0xAF, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xB0, PROF_SEM_NONE,       PROF_ENC_NONE },
    { 0xB1, PROF_SEM_WEAR,       PROF_ENC_NONE },
    { 0xB2, PROF_SEM_MEDIA,      PROF_ENC_COUNTER },
    { 0xB5, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xB6, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xC0, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xC3, PROF_SEM_NONE,       PROF_ENC_NONE },
    { 0xE7, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT },
    { 0xE8, PROF_SEM_WEAR,       PROF_ENC_PERCENT },
    { 0xE9, PROF_SEM_NAND_WRITE, PROF_ENC_COUNTER },
    { 0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_SCALE },
    { 0xF2, PROF_SEM_HOST_READ,  PROF_ENC_SCALE },
    { 0xF3, PROF_SEM_HOST_WRITE, PROF_ENC_SCALE },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kSamsungSsd[] = {
    { 0xB1, PROF_SEM_WEAR,       PROF_ENC_COUNTER },
    { 0xB3, PROF_SEM_MEDIA,      PROF_ENC_COUNTER },
    { 0xB5, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xB6, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xB7, PROF_SEM_MEDIA,      PROF_ENC_COUNTER },
    { 0xBB, PROF_SEM_MEDIA,      PROF_ENC_COUNTER },
    { 0xE7, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT },
    { 0xEB, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_LBA },
    { 0xF2, PROF_SEM_HOST_READ,  PROF_ENC_LBA },
    { 0xF5, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xF6, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xF7, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xF8, PROF_SEM_WEAR,       PROF_ENC_COUNTER },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kSmi225[] = {
    { 0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_BLOCK32 },
    { 0xF2, PROF_SEM_HOST_READ,  PROF_ENC_BLOCK32 },
    { 0xF5, PROF_SEM_NAND_WRITE, PROF_ENC_BLOCK32 },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kIntel[] = {
    { 0xAA, PROF_SEM_WEAR,       PROF_ENC_PERCENT },
    { 0xE1, PROF_SEM_HOST_WRITE, PROF_ENC_COUNTER },
    { 0xE2, PROF_SEM_NONE,       PROF_ENC_COUNTER },
    { 0xE8, PROF_SEM_WEAR,       PROF_ENC_PERCENT },
    { 0xE9, PROF_SEM_WEAR,       PROF_ENC_PERCENT },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kMicron[] = {
    { 0xAD, PROF_SEM_ERASE,      PROF_ENC_COUNTER },
    { 0xCA, PROF_SEM_ENDURANCE,  PROF_ENC_PERCENT },
    { 0xF6, PROF_SEM_HOST_WRITE, PROF_ENC_COUNTER },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kSeagateHdd[] = {
    { 0x01, PROF_SEM_NONE,      PROF_ENC_COUNTER },
    { 0xBB, PROF_SEM_MEDIA,     PROF_ENC_COUNTER },
    { 0xC3, PROF_SEM_NONE,      PROF_ENC_COUNTER },
    { 0xF0, PROF_SEM_NONE,      PROF_ENC_COUNTER },
    { 0x00, 0, 0 }
};

static const PROF_ATTR kToshibaHdd[] = {
    { 0xC7, PROF_SEM_INTERFACE, PROF_ENC_COUNTER },
    { 0x00, 0, 0 }
};

/* nvme: -1 any, 0 ATA, 1 NVMe. hdd: -1 any, 0 not HDD, 1 HDD. */
static const DRIVE_PROFILE kProfiles[] = {
    { "ata-generic", NULL, PROF_KIND_GENERIC, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_UNKNOWN, 0, -1, NULL, NULL, kAtaGeneric },
    { "nvme-generic", NULL, PROF_KIND_GENERIC, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_UNKNOWN, 1, 0, NULL, NULL, NULL },

    { "phison", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, -1, 0, NULL, NULL, kPhison },
    { "samsung-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, NULL, NULL, kSamsungSsd },
    /* NVMe life is Percentage Used. kSamsungSsd stays on ATA: E7 and F1/F2
     * are not NVMe log fields. */
    { "samsung-nvme", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 1, 0, NULL, NULL, NULL },
    { "intel-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_INTEL, -1, 0, NULL, NULL, kIntel },
    { "micron-ssd", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_MICRON, -1, 0, NULL, NULL, kMicron },
    { "smi-2259", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_SMI225,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0,
      "Silicon Motion SM2258/SM2259", NULL, kSmi225 },
    { "smi-ve", NULL, PROF_KIND_CONTROLLER, NULL, NULL, PROF_FW_VE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0,
      "Silicon Motion SM2258", NULL, NULL },
    { "samsung-svt", "samsung-ssd", PROF_KIND_CONTROLLER, NULL, "SVT", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL },

    { "seagate-hdd", NULL, PROF_KIND_VENDOR, NULL, NULL, PROF_FW_NONE,
      VENDOR_SEAGATE, CONTROLLER_UNKNOWN, 0, 1, NULL, NULL, kSeagateHdd },
    { "toshiba-hdd", NULL, PROF_KIND_VENDOR, NULL, NULL, PROF_FW_NONE,
      VENDOR_TOSHIBA, CONTROLLER_UNKNOWN, 0, 1, NULL, NULL, kToshibaHdd },

    { "samsung-870", "samsung-ssd", PROF_KIND_FAMILY, "870", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, NULL, NULL, NULL },

    { "micron-2400", NULL, PROF_KIND_EXACT, "MICRON_2400", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 1, 0, "Silicon Motion SM2269XT", "1.4.0", NULL },
    { "micron-2400-mtfd", NULL, PROF_KIND_EXACT, "2400_MTFD", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 1, 0, "Silicon Motion SM2269XT", "1.4.0", NULL },
    { "sx8200pnp", NULL, PROF_KIND_EXACT, "SX8200PNP", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL },
    { "sx8200-pro", NULL, PROF_KIND_EXACT, "SX8200 PRO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL },
    { "sx8200pro", NULL, PROF_KIND_EXACT, "SX8200PRO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL },
    { "gammix-s11", NULL, PROF_KIND_EXACT, "GAMMIX S11", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL },
    { "xpg-s11", NULL, PROF_KIND_EXACT, "XPG S11", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, -1, 0,
      "Silicon Motion SM2262EN/SM2262ENG/SM2262G", "1.3.0", NULL },
    { "su800", NULL, PROF_KIND_EXACT, "SU800", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258/SM2259", NULL, NULL },
    { "as340", NULL, PROF_KIND_EXACT, "AS340", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258", NULL, NULL },
    { "as350", NULL, PROF_KIND_EXACT, "AS350", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SMI, 0, 0, "Silicon Motion SM2258", NULL, NULL },
    { "samsung-870-evo", "samsung-ssd", PROF_KIND_EXACT, "870 EVO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL },
    { "samsung-870evo", "samsung-ssd", PROF_KIND_EXACT, "870EVO", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_SAMSUNG, 0, 0, "Samsung MKX", NULL, NULL },
    { "r5sl", "phison", PROF_KIND_EXACT, "R5SL", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "gammix-s70", NULL, PROF_KIND_EXACT, "GAMMIX S70", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_INNOGRIT, 1, 0, "Innogrit IG5236", "1.4.0", NULL },
    { "p210-u11", "phison", PROF_KIND_EXACT, "P210", "U11", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "burst-u11", "phison", PROF_KIND_EXACT, "BURST", "U11", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "burst-u10", "phison", PROF_KIND_EXACT, "BURST", "U10", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3110-S10", NULL, NULL },
    { "burst-hps", "phison", PROF_KIND_EXACT, "BURST", "HPS", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, NULL, NULL, NULL },
    { "gstfs-sbfm", "phison", PROF_KIND_EXACT, "GSTFS", "SBFM", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "a400-sbfk61", "phison", PROF_KIND_EXACT, "A400", "SBFK61", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3110-S10", NULL, NULL },
    { "a400-sbfk62", "phison", PROF_KIND_EXACT, "A400", "SBFK62", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "a400-sbfk", "phison", PROF_KIND_EXACT, "A400", "SBFK", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0,
      "Phison PS3110-S10 / PS3111-S11", NULL, NULL },
    { "a400-sbfm", "phison", PROF_KIND_EXACT, "A400", "SBFM", PROF_FW_SUBSTR,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, 0, 0, "Phison PS3111-S11", NULL, NULL },
    { "m450", "phison", PROF_KIND_EXACT, "M450", NULL, PROF_FW_NONE,
      VENDOR_UNKNOWN, CONTROLLER_PHISON, -1, 0, "Phison PS5019-E19T", NULL, NULL }
};

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

static const DRIVE_PROFILE* ProfileById(const char* id)
{
    unsigned i;
    if (!id || !id[0]) return NULL;
    for (i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) {
        if (strcmp(kProfiles[i].id, id) == 0)
            return &kProfiles[i];
    }
    return NULL;
}

static const DRIVE_PROFILE* GenericFor(const DRIVE_INFO* p)
{
    return ProfileById((p && p->bIsNVMe) ? "nvme-generic" : "ata-generic");
}

void ResolveDriveProfile(DRIVE_INFO* p)
{
    char modelU[48], fwU[16];
    const DRIVE_PROFILE* best = NULL;
    int bestScore = 0, bestSpec = -1;
    unsigned i;

    if (!p) return;
    p->nProfileScore = 0;
    p->szProfileId[0] = '\0';
    ToUpperCopy(modelU, sizeof(modelU), p->szModel);
    ToUpperCopy(fwU, sizeof(fwU), p->szFirmware);

    for (i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) {
        int score = ScoreProfile(&kProfiles[i], p, modelU, fwU);
        int spec;
        if (score <= 0) continue;
        spec = Specificity(&kProfiles[i]);
        if (!best || score > bestScore || (score == bestScore && spec > bestSpec)) {
            best = &kProfiles[i];
            bestScore = score;
            bestSpec = spec;
        }
    }
    if (!best) best = GenericFor(p);
    if (best) {
        p->nProfileScore = bestScore > 0 ? bestScore : PROF_SCORE_GENERIC;
        lstrcpynA(p->szProfileId, best->id, (int)sizeof(p->szProfileId));
        if (p->nProfileScore >= PROF_SCORE_CONTROLLER &&
            best->ctl != CONTROLLER_UNKNOWN)
            p->eController = best->ctl;
        if (best->chip && best->chip[0])
            safe_snprintf(p->szControllerChip, "%s", best->chip);
        if (best->nvmeVer && p->bIsNVMe &&
            (p->szProtocol[0] == '\0' || strcmp(p->szProtocol, "NVMe") == 0))
            safe_snprintf(p->szProtocol, "NVMe %s", best->nvmeVer);
    }
    DriveDbApply(p);
}

int DriveProfileScore(const DRIVE_INFO* p)
{
    if (!p) return 0;
    return p->nProfileScore;
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

/* Highest-scoring non-generic local row that lists id, else NULL.
 * The identity winner can be a model row that only names the chip; a
 * firmware row still supplies the encoding. */
static const PROF_ATTR* LocalSpecificAttr(const DRIVE_INFO* p, BYTE id)
{
    char modelU[48], fwU[16];
    const PROF_ATTR* bestA = NULL;
    int bestScore = 0;
    unsigned i;
    if (!p || id == 0) return NULL;
    ToUpperCopy(modelU, sizeof(modelU), p->szModel);
    ToUpperCopy(fwU, sizeof(fwU), p->szFirmware);
    for (i = 0; i < sizeof(kProfiles) / sizeof(kProfiles[0]); i++) {
        const DRIVE_PROFILE* pr = &kProfiles[i];
        int score = ScoreProfile(pr, p, modelU, fwU);
        int guard = 0;
        if (score <= 0 || pr->kind == PROF_KIND_GENERIC) continue;
        while (pr && guard++ < 8) {
            const PROF_ATTR* a = FindProfAttr(pr->attrs, id);
            if (a && score >= bestScore) {
                bestA = a;
                bestScore = score;
                break;
            }
            pr = pr->extends ? ProfileById(pr->extends) : NULL;
        }
    }
    return bestA;
}

BOOL ProfileListsAttr(const DRIVE_INFO* p, BYTE id)
{
    const DRIVE_PROFILE* g;
    if (LocalSpecificAttr(p, id)) return TRUE;
    g = GenericFor(p);
    return g && FindProfAttr(g->attrs, id) != NULL;
}

/* Local profile, then drivedb when that name states a semantic, then
 * the generic ATA/NVMe row. A profile that did not match cannot name it. */
BOOL ProfileLookup(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    const PROF_ATTR* bestA;
    int dsem = PROF_SEM_NONE, denc = PROF_ENC_NONE;
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (!p || id == 0) return FALSE;

    bestA = LocalSpecificAttr(p, id);
    if (bestA) {
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
