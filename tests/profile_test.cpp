/* DriveMonitor - profile resolver tests. GPL-2.0-or-later. See LICENSE.
 * Linked with src/profile.cpp only. DriveDbApply and DriveDbLookup are
 * stubs, so a new row is proven by ProfileRegister, not by an interpreter edit.
 * The real drivedb chain is tests/drivedb_test.cpp. */
#include "smart.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_check = 0;
static int g_dbE7 = 0;
static int g_db11 = 0;
static DRIVE_INFO g;

extern "C" void DriveDbApply(DRIVE_INFO* p)
{
    if (!p) return;
    p->szDriveDbTie[0] = '\0';
    p->bDriveDbTieUnnamed = FALSE;
}

/* When g_dbE7 is set, drivedb claims 231 is wear/counter.
 * A local profile that already decided 231 must keep its own semantic.
 * A contested 231 must not accept this row. */
extern "C" BOOL DriveDbAttrIsCrc(const DRIVE_INFO* p, BYTE id)
{
    (void)p;
    (void)id;
    return FALSE;
}

extern "C" BOOL DriveDbLookup(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    (void)p;
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (g_db11 && id == 0x11) {
        if (sem) *sem = PROF_SEM_WEAR;
        if (enc) *enc = PROF_ENC_COUNTER;
        return TRUE;
    }
    if (!g_dbE7 || id != 0xE7) return FALSE;
    if (sem) *sem = PROF_SEM_WEAR;
    if (enc) *enc = PROF_ENC_COUNTER;
    return TRUE;
}

static void Expect(int cond, const char* msg)
{
    g_check++;
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static void Blank(DRIVE_TYPE type, BOOL nvme)
{
    memset(&g, 0, sizeof(g));
    g.eType = type;
    g.bIsNVMe = nvme;
    g.nEndurancePercent = -1;
}

static void Name(const char* model, const char* fw)
{
    lstrcpynA(g.szModel, model ? model : "", (int)sizeof(g.szModel));
    lstrcpynA(g.szFirmware, fw ? fw : "", (int)sizeof(g.szFirmware));
}

static void Go(void)
{
    ResolveDriveProfile(&g);
}

static int Lookup(BYTE id, int* sem, int* enc)
{
    return ProfileLookup(&g, id, sem, enc) ? 1 : 0;
}

static void ExpectId(const char* id, int score, const char* msg)
{
    Expect(strcmp(g.szProfileId, id) == 0, msg);
    Expect(g.nProfileScore == score, msg);
    Expect(strcmp(g.szProfileOrigin, "builtin") == 0, msg);
}

static const PROF_ATTR kLifeA[] = {
    { 0xE7, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, 0, "A life", "A life", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kLifeB[] = {
    { 0xE7, PROF_SEM_WEAR, PROF_ENC_COUNTER, 0, "B wear", "B wear", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kLifeAgree[] = {
    { 0xE7, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, 0, "Same", "Same", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kWidget[] = {
    { 0xE7, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, 0, "Widget life", "Widget life", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Widget media", "Widget media", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kSpecLong[] = {
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Long media", "Long media", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kSpecShort[] = {
    { 0x11, PROF_SEM_WEAR, PROF_ENC_COUNTER, 0, "Short wear", "Short wear", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kCoverMedia[] = {
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Cover media", "Cover media", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kCoverWear[] = {
    { 0x11, PROF_SEM_WEAR, PROF_ENC_COUNTER, 0, "Cover wear", "Cover wear", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kCoverSame[] = {
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Cover same", "Cover same", NULL, NULL,
      0, 0, 0, 0, 2, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kPrefHi[] = {
    { 0x42, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, 0, "Hi", "Hi", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0, 1 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kPrefLo[] = {
    { 0x42, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, 0, "Lo", "Lo", NULL, NULL,
      0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static void DefExact(PROFILE_DEF* d, const char* id, const char* model,
                     const PROF_ATTR* attrs, const char* source)
{
    memset(d, 0, sizeof(*d));
    d->id = id;
    d->kind = PROF_KIND_EXACT;
    d->model = model;
    d->fwMode = PROF_FW_NONE;
    d->vendor = VENDOR_UNKNOWN;
    d->ctl = CONTROLLER_UNKNOWN;
    d->nvme = 0;
    d->hdd = 0;
    d->attrs = attrs;
    d->source = source;
}

static void TestPriority(void)
{
    int sem = 0, enc = 0;

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Go();
    ExpectId("samsung-870-svt", 100, "870 EVO + SVT");
    Expect(g.eController == CONTROLLER_SAMSUNG, "870 SVT controller");
    Expect(strcmp(g.szControllerChip, "Samsung MKX") == 0, "870 SVT chip");
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "870 E7 value");
    Expect(Lookup(0xB1, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "870 B1 value");
    Expect(ProfileAttrPref(&g, 0xE7) > ProfileAttrPref(&g, 0xB1),
           "870 E7 preferred over B1");
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) == 0,
           "870 does not prefer an agreeing raw");
    Expect(!g.bProfileAmbiguous, "870 SVT unique");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("Samsung SSD 870 EVO 1TB", "RVT02B6Q");
    Go();
    ExpectId("samsung-870-evo", 98, "870 EVO model");
    Expect(strcmp(g.szControllerChip, "Samsung MKX") == 0, "870 EVO chip");
    Expect(Lookup(0xE7, &sem, &enc) && enc == PROF_ENC_VALUE, "870 EVO E7 value");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("Samsung SSD 870 QVO 1TB", "SVQ01B6Q");
    Go();
    ExpectId("samsung-870", 90, "870 family");
    Expect(g.eController == CONTROLLER_SAMSUNG, "870 family controller");
    Expect(g.szControllerChip[0] == '\0', "870 family has no chip");
    Expect(Lookup(0xB1, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "870 family inherits B1 value");
    Expect(ProfileAttrPref(&g, 0xE7) == 1, "870 family keeps E7 pref");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("MYSTERYSSD", "");
    Go();
    ExpectId("ata-generic", 50, "unknown SATA SSD");
    Expect(g.eController == CONTROLLER_UNKNOWN, "unknown SSD controller");
    Expect(!Lookup(0xE7, &sem, &enc), "unknown SSD has no life");
    Expect(!ProfileListsAttr(&g, 0xFC), "0xFC is not a generic attribute");
    Expect(Lookup(0x05, &sem, &enc) && sem == PROF_SEM_MEDIA, "generic realloc");

    Blank(DRIVE_TYPE_NVME, TRUE);
    Name("WDC PC SN530 SDBPNPZ-256G", "");
    Go();
    ExpectId("nvme-generic", 50, "unknown NVMe");
    Expect(g.eController == CONTROLLER_UNKNOWN, "unknown NVMe controller");

    Blank(DRIVE_TYPE_HDD, FALSE);
    g.eVendor = VENDOR_SEAGATE;
    Name("ST1000DM010-2EP102", "CC43");
    Go();
    ExpectId("seagate-hdd", 70, "Seagate HDD vendor");
    Expect(Lookup(0xC3, &sem, &enc) && enc == PROF_ENC_COUNTER, "Seagate 195");
}

static void TestSameIdDifferentMeaning(void)
{
    int sem = 0, enc = 0;

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KINGSTON SA400S37240G", "SBFKB1H5");
    Go();
    ExpectId("a400-sbfk", 100, "A400 SBFK");
    Expect(g.eController == CONTROLLER_PHISON, "A400 SBFK Phison");
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) != 0, "A400 prefer agreeing");
    Expect(Lookup(0xA9, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_PERCENT, "Phison A9 endurance");
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE, "Phison E7 endurance");
    Expect(Lookup(0xF1, &sem, &enc) && enc == PROF_ENC_SCALE, "Phison F1 scale");
    Expect(Lookup(0xE9, &sem, &enc) && sem == PROF_SEM_NAND_WRITE &&
           enc == PROF_ENC_GIB && ProfileWriteVolume(sem, enc), "Phison E9 GiB");
    Expect(!Lookup(0xFC, &sem, &enc), "Phison 252 unknown");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KINGSTON SA400S37480G", "SBFK62B1");
    Go();
    ExpectId("a400-sbfk62", 100, "SBFK62 beats SBFK");
    Expect(!g.bProfileAmbiguous, "SBFK62 is not a tie");
    Expect(strstr(g.szControllerChip, "PS3111-S11") != NULL, "SBFK62 chip");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("P3-512", "SN123456");
    Go();
    ExpectId("p3-maxio-sn", 100, "P3 SN");
    Expect(g.eController == CONTROLLER_MAXIO, "P3 Maxio");
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) == 0, "P3 has no agreeing flag");
    Expect(!Lookup(0xB1, &sem, &enc), "P3 has no B1");
    Expect(Lookup(0xA9, &sem, &enc) && sem == PROF_SEM_NONE &&
           enc == PROF_ENC_PAIR, "Maxio A9 is a pair");
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "Maxio E7 value");
    Expect(Lookup(0xF1, &sem, &enc) && enc == PROF_ENC_GIB, "Maxio F1 GiB");
}

static void TestFirmwareShape(void)
{
    int sem = 0, enc = 0;

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KINGSTON SA400S37120G", "U0524A0");
    Go();
    ExpectId("a400-smi225", 100, "A400 SM2258 firmware");
    Expect(g.eController == CONTROLLER_SMI, "A400 SMI");
    Expect(strstr(g.szControllerChip, "SM2258") != NULL, "A400 chip");
    Expect(Lookup(0xA9, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_PERCENT, "A400 SMI A9 raw percent");
    Expect(!Lookup(0xE7, &sem, &enc), "A400 SMI has no E7");
    Expect(Lookup(0xF1, &sem, &enc) && enc == PROF_ENC_BLOCK32, "A400 F1 32 MiB");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("", "U0524A0");
    Go();
    ExpectId("ata-generic", 50, "firmware shape alone");
    Expect(g.eController == CONTROLLER_UNKNOWN, "shape does not set SMI");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_SMI;
    Name("NV2", "U0524A0");
    Go();
    ExpectId("smi-s225-fw", 80, "SMI already known");
    Expect(g.szControllerChip[0] == '\0', "smi-s225-fw names no chip");
    Expect(Lookup(0xF5, &sem, &enc) && enc == PROF_ENC_BLOCK32, "SMI 245 blocks");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("Apacer AS350 512GB", "VE1R9008");
    Go();
    ExpectId("as350", 98, "AS350 model");
    Expect(g.eController == CONTROLLER_SMI, "AS350 SMI");
    Expect(strcmp(g.szControllerChip, "Silicon Motion SM2258") == 0, "AS350 chip");
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) == 0,
           "AS350 does not prefer an agreeing raw");
    Expect(!Lookup(0xE7, &sem, &enc), "AS350 231 is not in the local profile");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KINGSTON SV300S37A120G", "");
    Go();
    ExpectId("ata-generic", 50, "Kingston brand alone");
    Expect(g.eController == CONTROLLER_UNKNOWN, "Kingston is not Phison");
    Expect(!Lookup(0xA9, &sem, &enc), "brand does not invent A9");
}

/* KPSS120G2 is sold with more than one controller. The SM2258 map
 * applies only together with the 7-character firmware shape. */
static void TestKpssSmi225(void)
{
    int sem = 0, enc = 0;

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KPSS120G2", "W0714A0");
    Go();
    ExpectId("kpss-smi225", 100, "KPSS120G2 W0714A0");
    Expect(g.eController == CONTROLLER_SMI, "KPSS SMI");
    Expect(strstr(g.szControllerChip, "SM2258") != NULL, "KPSS chip");
    Expect(Lookup(0xA9, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_PERCENT, "KPSS 169 life");
    Expect(Lookup(0xA0, &sem, &enc) && sem == PROF_SEM_NONE, "KPSS 160 not media");
    Expect(Lookup(0xC3, &sem, &enc) && sem == PROF_SEM_NONE, "KPSS 195 not scored");
    Expect(Lookup(0xE8, &sem, &enc) && enc == PROF_ENC_PERCENT, "KPSS 232 spare");
    Expect(Lookup(0xF1, &sem, &enc) && sem == PROF_SEM_HOST_WRITE &&
           enc == PROF_ENC_BLOCK32, "KPSS 241 32 MiB");
    Expect(Lookup(0xF5, &sem, &enc) && sem == PROF_SEM_NAND_WRITE &&
           enc == PROF_ENC_BLOCK32, "KPSS 245 32 MiB");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("KPSS120G2", "HT3618C1");
    Go();
    ExpectId("ata-generic", 50, "KPSS other firmware");
    Expect(g.eController == CONTROLLER_UNKNOWN, "HT3618C1 is not SMI");
    Expect(!Lookup(0xA9, &sem, &enc), "other firmware does not name 169");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("OTHERSSD", "W0714A0");
    Go();
    ExpectId("ata-generic", 50, "W0714A0 alone");
    Expect(g.eController == CONTROLLER_UNKNOWN, "firmware shape does not set SMI");
    Expect(!Lookup(0xF1, &sem, &enc), "shape alone does not name 241");
}

static void TestRegister(void)
{
    PROFILE_DEF a, b, w, extra;
    int sem = 0, enc = 0;
    const char* ru = NULL;
    char diag[160];
    int i;
    static const char* ids[8] = {
        "extra-0", "extra-1", "extra-2", "extra-3",
        "extra-4", "extra-5", "extra-6", "extra-7"
    };
    static const char* models[8] = {
        "FILLER0", "FILLER1", "FILLER2", "FILLER3",
        "FILLER4", "FILLER5", "FILLER6", "FILLER7"
    };

    ProfileClearRegistered();
    Expect(!ProfileRegister(NULL), "NULL profile rejected");
    DefExact(&extra, "", "NOID", NULL, NULL);
    Expect(!ProfileRegister(&extra), "empty id rejected");
    DefExact(&extra, "ata-generic", "NOID", NULL, NULL);
    Expect(!ProfileRegister(&extra), "duplicate builtin id rejected");

    DefExact(&w, "widget-z1", "WIDGETZ1", kWidget, "test-register");
    Expect(ProfileRegister(&w), "widget registered");
    Expect(!ProfileRegister(&w), "duplicate widget rejected");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("WIDGETZ1 256GB", "");
    Go();
    Expect(strcmp(g.szProfileId, "widget-z1") == 0, "widget id");
    Expect(g.nProfileScore == 98, "widget exact model");
    Expect(strcmp(g.szProfileOrigin, "test-register") == 0, "widget origin");
    Expect(!g.bProfileAmbiguous, "widget unique");
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_PERCENT, "widget life");
    Expect(Lookup(0x11, &sem, &enc) && sem == PROF_SEM_MEDIA, "widget media id");
    {
        PROF_HEALTH hh;
        Expect(ProfileHealthRule(&g, 0x11, &hh) &&
               hh.rule == PROF_RULE_WORSE && hh.sem == PROF_SEM_MEDIA &&
               hh.caution == 1 && hh.bad == 0 && hh.badHdd == 0,
               "widget media enters the channel");
    }
    Expect(ProfileAttrLabel(&g, 0xE7, &sem, &enc, &ru, NULL, NULL, NULL, NULL) &&
           ru && strcmp(ru, "Widget life") == 0, "widget label");
    ProfileClearRegistered();
    Go();
    Expect(strcmp(g.szProfileId, "ata-generic") == 0, "cleared widget falls back");

    DefExact(&a, "alpha-tie", "CONFLICTX", kLifeA, "test-register");
    DefExact(&b, "zeta-tie", "CONFLICTX", kLifeB, "test-register");
    g_dbE7 = 1;
    Expect(ProfileRegister(&b), "zeta first");
    Expect(ProfileRegister(&a), "alpha second");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("CONFLICTX", "");
    Go();
    Expect(strcmp(g.szProfileId, "alpha-tie") == 0, "lexical winner, zeta first");
    Expect(strcmp(g.szProfileConflict, "zeta-tie") == 0, "conflict is zeta");
    Expect(g.bProfileAmbiguous, "conflict flagged");
    Expect(g.nProfileScore == 98, "tie score");
    Expect(!Lookup(0xE7, &sem, &enc), "contested 231 refused");
    Expect(ProfileListsAttr(&g, 0xE7), "contested 231 stays listed");
    Expect(!ProfileAttrLabel(&g, 0xE7, &sem, &enc, &ru, NULL, NULL, NULL, NULL),
           "contested 231 has no label");
    FormatProfileDiagnostic(&g, diag, (int)sizeof(diag));
    Expect(strstr(diag, "ambiguous=1") != NULL, "diagnostic ambiguous");
    Expect(strstr(diag, "conflict=zeta-tie") != NULL, "diagnostic conflict");

    ProfileClearRegistered();
    Expect(ProfileRegister(&a), "alpha first");
    Expect(ProfileRegister(&b), "zeta second");
    Go();
    Expect(strcmp(g.szProfileId, "alpha-tie") == 0, "lexical winner, alpha first");
    Expect(strcmp(g.szProfileConflict, "zeta-tie") == 0, "same conflict either order");
    Expect(!Lookup(0xE7, &sem, &enc), "contested 231 refused either order");
    ProfileClearRegistered();

    DefExact(&a, "agree-b", "AGREEX", kLifeAgree, "test-register");
    DefExact(&b, "agree-a", "AGREEX", kLifeAgree, "test-register");
    Expect(ProfileRegister(&a), "agree b");
    Expect(ProfileRegister(&b), "agree a");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("AGREEX", "");
    Go();
    Expect(g.bProfileAmbiguous, "agreeing tie is still ambiguous");
    Expect(strcmp(g.szProfileId, "agree-a") == 0, "agree lexical id");
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_PERCENT, "agreeing 231 is used");
    ProfileClearRegistered();
    g_dbE7 = 0;

    DefExact(&a, "order-family", "ZZORDER", NULL, "test-register");
    a.kind = PROF_KIND_FAMILY;
    DefExact(&b, "order-exact", "ZZORDERX", NULL, "test-register");
    Expect(ProfileRegister(&a), "family first");
    Expect(ProfileRegister(&b), "exact second");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("ZZORDERX", "");
    Go();
    Expect(strcmp(g.szProfileId, "order-exact") == 0, "exact beats family");
    Expect(g.nProfileScore == 98, "exact score");
    Expect(!g.bProfileAmbiguous, "different scores are not a tie");
    ProfileClearRegistered();
    Expect(ProfileRegister(&b), "exact first");
    Expect(ProfileRegister(&a), "family second");
    Go();
    Expect(strcmp(g.szProfileId, "order-exact") == 0, "exact wins either order");
    Expect(g.nProfileScore == 98, "exact score either order");
    ProfileClearRegistered();

    for (i = 0; i < 8; i++) {
        DefExact(&extra, ids[i], models[i], NULL, "test-register");
        Expect(ProfileRegister(&extra), "filler slot");
    }
    DefExact(&extra, "extra-8", "FILLER8", NULL, "test-register");
    Expect(!ProfileRegister(&extra), "ninth profile rejected");
    ProfileClearRegistered();

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("MYSTERYSSD", "");
    Go();
    g_dbE7 = 1;
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_WEAR &&
           enc == PROF_ENC_COUNTER, "drivedb fills a missing local id");
    Expect(!ProfileListsAttr(&g, 0xE7), "drivedb is not a local profile");
    g_dbE7 = 0;
    Expect(!Lookup(0xE7, &sem, &enc), "without drivedb the id stays unknown");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Go();
    g_dbE7 = 1;
    Expect(Lookup(0xE7, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "local 231 beats drivedb");
    g_dbE7 = 0;
}

static void TestWriteVolume(void)
{
    int sem = 0, enc = 0;

    Expect(!ProfileWriteVolume(PROF_SEM_WEAR, PROF_ENC_PERCENT), "wear percent is not a volume");
    Expect(!ProfileWriteVolume(PROF_SEM_NAND_WRITE, PROF_ENC_COUNTER), "bare counter is not GiB");
    Expect(!ProfileWriteVolume(PROF_SEM_NONE, PROF_ENC_GIB), "GiB without a write semantic");
    Expect(ProfileWriteVolume(PROF_SEM_HOST_WRITE, PROF_ENC_GIB), "host GiB");
    Expect(ProfileWriteVolume(PROF_SEM_NAND_WRITE, PROF_ENC_BLOCK32), "NAND 32 MiB");
    Expect(ProfileWriteVolume(PROF_SEM_HOST_WRITE, PROF_ENC_LBA), "host LBA");
    Expect(ProfileWriteVolume(PROF_SEM_HOST_WRITE, PROF_ENC_SCALE), "host scale");
    Expect(ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_COUNTER), "erase counter");
    Expect(ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_MAXAVG), "erase max/avg");
    Expect(ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_AVG), "erase average");
    Expect(ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_MAX), "erase maximum");
    Expect(ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_MIN), "erase minimum");
    Expect(!ProfileEraseCount(PROF_SEM_ERASE, PROF_ENC_PERCENT), "erase percent is not a count");
    Expect(!ProfileEraseCount(PROF_SEM_WEAR, PROF_ENC_COUNTER), "wear counter is not erase");
    Expect(!ProfileEraseCount(PROF_SEM_NONE, PROF_ENC_COUNTER), "unnamed counter is not erase");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_INTEL;
    Name("INTEL SSDSC2KB480G8", "");
    Go();
    Expect(strcmp(g.szProfileId, "intel-ssd") == 0, "Intel controller profile");
    Expect(Lookup(0xE9, &sem, &enc) && sem == PROF_SEM_WEAR &&
           enc == PROF_ENC_PERCENT, "Intel E9 wear percent");
    Expect(!ProfileWriteVolume(sem, enc), "Intel E9 is not host writes");
    Expect(Lookup(0xE1, &sem, &enc) && sem == PROF_SEM_HOST_WRITE &&
           enc == PROF_ENC_BLOCK32 && ProfileWriteVolume(sem, enc), "Intel E1 blocks");
    Expect(!Lookup(0xEA, &sem, &enc), "Intel EA is not an erase count");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_PHISON;
    Name("PHISON-EA", "");
    Go();
    Expect(!Lookup(0xEA, &sem, &enc), "Phison EA is not listed");
    Expect(Lookup(0xA7, &sem, &enc) && enc == PROF_ENC_AVG &&
           ProfileEraseCount(sem, enc), "Phison A7 average");
    Expect(Lookup(0xA5, &sem, &enc) && enc == PROF_ENC_MAX &&
           ProfileEraseCount(sem, enc), "Phison A5 max");
    Expect(Lookup(0xA6, &sem, &enc) && enc == PROF_ENC_MIN &&
           ProfileEraseCount(sem, enc), "Phison A6 min");
    Expect(Lookup(0xA4, &sem, &enc) && enc == PROF_ENC_COUNTER &&
           ProfileEraseCount(sem, enc), "Phison A4 total stays a counter");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_MICRON;
    Name("Micron_1100_MTFD", "");
    Go();
    Expect(strcmp(g.szProfileId, "micron-ssd") == 0, "Micron controller profile");
    Expect(Lookup(0xCA, &sem, &enc) && sem == PROF_SEM_ENDURANCE &&
           enc == PROF_ENC_VALUE, "Micron CA is normalized Value");
    Expect(ProfileAttrPref(&g, 0xCA) == 0, "Micron CA pref");
    Expect(Lookup(0xAD, &sem, &enc) && sem == PROF_SEM_ERASE &&
           enc == PROF_ENC_AVG && ProfileEraseCount(sem, enc), "Micron AD average");
}

static void TestDecodeAndUsb(void)
{
    unsigned __int64 out = 1;
    long long delta = 0;
    int sat = 1, smart = 1, thresh = 1, nvme = 1;
    unsigned __int64 lbaLimit = 0xFFFFFFFFFFFFFFFFULL / 512ULL;
    unsigned __int64 blockLimit = (4000000ULL * 1024ULL) / 32ULL;

    Expect(ProfileDecodeRaw(PROF_ENC_PERCENT, 40, &out) == 1 && out == 40, "percent 40");
    Expect(ProfileDecodeRaw(PROF_ENC_PERCENT, 0, &out) == 1 && out == 0, "percent 0");
    Expect(ProfileDecodeRaw(PROF_ENC_PERCENT, 101, &out) == 0, "percent 101 unsafe");
    Expect(ProfileDecodeRaw(PROF_ENC_SCALE, 14664, &out) == 0, "scale unsafe");
    Expect(ProfileDecodeRaw(PROF_ENC_NONE, 1, &out) == 0, "none unsafe");
    Expect(ProfileDecodeRaw(PROF_ENC_COUNTER, 0xFFFFFFFFFFFFULL, &out) == 1, "counter");
    Expect(ProfileDecodeRaw(PROF_ENC_GIB, 4000000ULL, &out) == 1 && out == 4000000ULL, "GiB cap");
    Expect(ProfileDecodeRaw(PROF_ENC_GIB, 4000001ULL, &out) == 0, "GiB overflow");
    Expect(ProfileDecodeRaw(PROF_ENC_GIB, 0, &out) == 0, "GiB zero");
    Expect(ProfileDecodeRaw(PROF_ENC_BLOCK32, 0, &out) == 0, "block32 zero");
    Expect(ProfileDecodeRaw(PROF_ENC_BLOCK32, 1024, &out) == 1 && out == 32, "block32 1024");
    Expect(ProfileDecodeRaw(PROF_ENC_BLOCK32, blockLimit, &out) == 1 && out == 4000000ULL,
           "block32 cap");
    Expect(ProfileDecodeRaw(PROF_ENC_BLOCK32, blockLimit + 1ULL, &out) == 0, "block32 overflow");
    Expect(ProfileDecodeRaw(PROF_ENC_LBA, 1, &out) == 1 && out == 1, "lba sectors");
    Expect(ProfileDecodeRaw(PROF_ENC_LBA, lbaLimit, &out) == 1, "lba limit");
    Expect(ProfileDecodeRaw(PROF_ENC_LBA, lbaLimit + 1ULL, &out) == 0, "lba overflow");
    Expect(ProfileDecodeRaw(PROF_ENC_MAXAVG, 0xFFFFFFFFULL, &out) == 1, "maxavg");
    Expect(ProfileDecodeRaw(PROF_ENC_MAXAVG, 0x100000000ULL, &out) == 0, "maxavg wide");
    Expect(ProfileDecodeRaw(PROF_ENC_PAIR, 0x100000000ULL, &out) == 0, "pair wide");
    Expect(ProfileDecodeRaw(PROF_ENC_PACKED, 5, &out) == 0, "packed unsafe");
    Expect(ProfileDecodeRaw(PROF_ENC_AVG, 42, &out) == 1 && out == 42, "avg count");
    Expect(ProfileDecodeRaw(PROF_ENC_MAX, 9, &out) == 1 && out == 9, "max count");
    Expect(ProfileDecodeRaw(PROF_ENC_MIN, 1, &out) == 1 && out == 1, "min count");
    Expect(ProfileDecodeRaw(PROF_ENC_VALUE, 98, &out) == 0, "value is not raw");
    Expect(ProfileDecodeRaw(PROF_ENC_PERCENT, 1, NULL) == 0, "decode NULL out");

    Expect(ProfileRawDelta(10, 15, &delta) == 1 && delta == 5, "raw delta");
    Expect(ProfileRawDelta(15, 10, &delta) == 0, "backwards counter");
    Expect(ProfileRawDelta(0, 0x7FFFFFFFFFFFFFFFULL, &delta) == 1, "delta max");
    Expect(ProfileRawDelta(0, 0x8000000000000000ULL, &delta) == 0, "delta overflow");
    Expect(ProfileRawDelta(1, 2, NULL) == 0, "delta NULL");

    ResolveDriveProfile(NULL);
    Expect(!ProfileLookup(NULL, 0x05, NULL, NULL), "lookup NULL");
    Expect(!ProfileLookup(&g, 0, NULL, NULL), "lookup id 0");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("", "");
    Go();
    Expect(strcmp(g.szProfileId, "ata-generic") == 0, "empty identity");

    memset(&g, 0, sizeof(g));
    UsbProbeFlags(&g, &sat, &smart, &thresh, &nvme);
    Expect(sat == 0 && smart == 0 && thresh == 0 && nvme == 0, "non-USB probes stay off");

    g.bIsUSB = TRUE;
    UsbProbeFlags(&g, &sat, &smart, &thresh, &nvme);
    Expect(sat == 0 && smart == 0 && thresh == 0 && nvme == 0, "USB with no capability");

    g.bCapAtaIdentify = TRUE;
    g.bCapSmartData = TRUE;
    UsbProbeFlags(&g, &sat, &smart, &thresh, &nvme);
    Expect(sat == 1 && smart == 1 && thresh == 0 && nvme == 0, "USB SAT and SMART");

    Blank(DRIVE_TYPE_USB, FALSE);
    g.bIsUSB = TRUE;
    g.eController = CONTROLLER_UNKNOWN;
    Name("JetFlash Transcend 16GB", "");
    Go();
    Expect(strcmp(g.szProfileId, "ata-generic") == 0, "USB stick generic");
    Expect(g.eController == CONTROLLER_UNKNOWN, "USB stick controller");
    Expect(!ProfileLookup(&g, 0xE7, NULL, NULL), "USB stick has no endurance");
    Expect(!ProfileLookup(&g, 0xA9, NULL, NULL), "USB stick has no A9");

    Expect(ProfileDecodeRaw(PROF_ENC_LO16, 0x1000AULL, &out) == 1 && out == 10,
           "low 16");
}

static const PROF_ATTR kRankBad[] = {
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Bad", "Bad", NULL, NULL,
      PROF_RULE_WORSE, PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 1, 1, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};
static const PROF_ATTR kRankCau[] = {
    { 0x11, PROF_SEM_MEDIA, PROF_ENC_COUNTER, 0, "Cau", "Cau", NULL, NULL,
      PROF_RULE_WORSE, PROF_SLOT_NONE, PROF_SCOPE_ANY, 0, 1, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static void TestHealthRule(void)
{
    PROF_HEALTH h;
    PROFILE_DEF a, b;

    Expect(ProfileCountRank(0, 0, 1, 0, 10, 1) == HEALTH_STATUS_GOOD, "zero stays good");
    Expect(ProfileCountRank(1, 0, 1, 0, 10, 0) == HEALTH_STATUS_CAUTION, "ssd realloc caution");
    Expect(ProfileCountRank(10, 0, 1, 0, 10, 0) == HEALTH_STATUS_CAUTION, "ssd realloc 10 stays caution");
    Expect(ProfileCountRank(10, 0, 1, 0, 10, 1) == HEALTH_STATUS_BAD, "hdd realloc 10 is bad");
    Expect(ProfileCountRank(9, 0, 1, 0, 10, 1) == HEALTH_STATUS_CAUTION, "hdd realloc 9 is caution");
    Expect(ProfileCountRank(2, 0, 1, 0, 4, 1) == HEALTH_STATUS_CAUTION, "hdd pending 2");
    Expect(ProfileCountRank(4, 0, 1, 0, 4, 1) == HEALTH_STATUS_BAD, "hdd pending 4");
    Expect(ProfileCountRank(4, 0, 1, 0, 4, 0) == HEALTH_STATUS_CAUTION, "ssd pending 4");
    Expect(ProfileCountRank(1, 0, 0, 1, 0, 0) == HEALTH_STATUS_BAD, "one uncorrectable is bad");
    Expect(ProfileCountRank(1, 0, 1, 100, 0, 0) == HEALTH_STATUS_CAUTION, "one crc is caution");
    Expect(ProfileCountRank(100, 0, 1, 100, 0, 0) == HEALTH_STATUS_BAD, "crc 100 is bad");
    Expect(ProfileCountRank(3, 1, 5, 0, 0, 1) == HEALTH_STATUS_OBSERVE, "e2e below caution");
    Expect(ProfileCountRank(5, 1, 5, 0, 0, 1) == HEALTH_STATUS_CAUTION, "e2e at caution");
    Expect(ProfileCountRank(2, 0, 3, 0, 0, 1) == HEALTH_STATUS_GOOD, "two spin retries");
    Expect(ProfileCountRank(3, 0, 3, 0, 0, 1) == HEALTH_STATUS_CAUTION, "three spin retries");
    Expect(ProfileCountRank(-1, 0, 1, 0, 0, 0) == HEALTH_STATUS_UNKNOWN, "absent count");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("UNKNOWN-SSD", "");
    Go();
    Expect(ProfileHealthRule(&g, 0x05, &h) && h.enc == PROF_ENC_LO16 &&
           h.slot == PROF_SLOT_REALLOC && h.caution == 1 && h.bad == 0 &&
           h.badHdd == 10 && h.rule == PROF_RULE_WORSE, "generic realloc rule");
    Expect(ProfileHealthRule(&g, 0xC5, &h) && h.slot == PROF_SLOT_PENDING &&
           h.badHdd == 4 && h.caution == 1, "generic pending rule");
    Expect(ProfileHealthRule(&g, 0xC6, &h) && h.bad == 1 && h.slot == PROF_SLOT_UNCORR,
           "generic uncorrectable rule");
    Expect(ProfileHealthRule(&g, 0xC4, &h) && h.enc == PROF_ENC_REMAP &&
           h.caution == 1 && h.bad == 0, "generic remap rule");
    Expect(ProfileHealthRule(&g, 0xC7, &h) && h.sem == PROF_SEM_INTERFACE &&
           h.slot == PROF_SLOT_CRC && h.bad == 100, "generic crc rule");
    Expect(ProfileHealthRule(&g, 0xBC, &h) && h.sem == PROF_SEM_INTERFACE &&
           h.enc == PROF_ENC_LO16 && h.caution == 10 && h.bad == 100,
           "generic timeout rule");
    Expect(ProfileHealthRule(&g, 0x0A, &h) && h.scope == PROF_SCOPE_HDD &&
           h.caution == 3, "spin retry is hdd only");
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.scope == PROF_SCOPE_HDD &&
           h.enc == PROF_ENC_REPORTED && h.bad == 1, "reported uncorrectable is hdd");
    Expect(ProfileHealthRule(&g, 0xB8, &h) && h.scope == PROF_SCOPE_HDD &&
           h.observe == 1 && h.caution == 5, "end to end is hdd");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_PHISON;
    Name("PHISON-RULE", "");
    Go();
    Expect(ProfileHealthRule(&g, 0xA1, &h) && h.rule == PROF_RULE_OFF,
           "spare remaining does not move the rank");
    Expect(ProfileHealthRule(&g, 0xB2, &h) && h.sem == PROF_SEM_MEDIA &&
           h.rule == PROF_RULE_WORSE && h.caution == 1 && h.bad == 0,
           "used spare enters media");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eController = CONTROLLER_SAMSUNG;
    Name("SAMSUNG-RULE", "");
    Go();
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.bad == 1 && h.rule == PROF_RULE_WORSE,
           "samsung uncorrectable is bad");
    Expect(ProfileHealthRule(&g, 0xB7, &h) && h.rule == PROF_RULE_OFF,
           "samsung 183 does not move the rank");
    Expect(ProfileHealthRule(&g, 0xB3, &h) && h.caution == 1 && h.bad == 0,
           "samsung used reserve enters media");

    Blank(DRIVE_TYPE_HDD, FALSE);
    g.eVendor = VENDOR_SEAGATE;
    Name("ST1000DM003", "");
    Go();
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.enc == PROF_ENC_REPORTED &&
           h.slot == PROF_SLOT_REPORTED && h.bad == 1, "seagate 187 rule");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eVendor = VENDOR_MICRON;
    Name("CRUCIAL-RULE", "");
    Go();
    Expect(strcmp(g.szProfileId, "crucial-micron") == 0, "crucial vendor profile");
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.enc == PROF_ENC_COUNTER &&
           h.scope == PROF_SCOPE_SSD && h.caution == 1 && h.bad == 0 &&
           h.rule == PROF_RULE_WORSE, "crucial micron 187 is caution");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    g.eVendor = VENDOR_MICRON;
    g.eController = CONTROLLER_MICRON;
    Name("MICRON-CTL", "");
    Go();
    Expect(strcmp(g.szProfileId, "micron-ssd") == 0, "micron controller identity");
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.caution == 1 && h.bad == 0 &&
           h.enc == PROF_ENC_COUNTER, "micron controller uses vendor 187");

    Blank(DRIVE_TYPE_HDD, FALSE);
    g.eVendor = VENDOR_MICRON;
    Name("CRUCIAL-HDD", "");
    Go();
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.scope == PROF_SCOPE_HDD &&
           h.bad == 1, "micron vendor does not cover hdd 187");

    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("CT500MX200SSD1", "");
    Go();
    Expect(strcmp(g.szProfileId, "mx200") == 0, "mx200 family without brand");
    Expect(ProfileHealthRule(&g, 0xBB, &h) && h.caution == 1 && h.bad == 0,
           "mx200 187 is caution");

    ProfileClearRegistered();
    DefExact(&a, "rank-bad", "RANKTIE", kRankBad, "test-register");
    DefExact(&b, "rank-cau", "RANKTIE", kRankCau, "test-register");
    Expect(ProfileRegister(&a), "rank bad");
    Expect(ProfileRegister(&b), "rank caution");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("RANKTIE", "");
    Go();
    Expect(!ProfileHealthRule(&g, 0x11, &h), "contested threshold is refused");
    Expect(!Lookup(0x11, NULL, NULL), "contested threshold has no semantic");
    Expect(ProfileListsAttr(&g, 0x11), "contested threshold stays listed");
    ProfileClearRegistered();
}

/* Same score, higher specificity wins. A lower tier that disagrees refuses
 * without marking the identity ambiguous. Agreement on that tier is used. */
static void TestAttrTier(void)
{
    PROFILE_DEF longer, shorter, exact, media, wear;
    int sem = 0, enc = 0;
    const char* ru = NULL;
    PROF_HEALTH h;

    ProfileClearRegistered();
    DefExact(&longer, "spec-long", "ZQWIDSPEC", kSpecLong, "test-register");
    longer.kind = PROF_KIND_FAMILY;
    DefExact(&shorter, "spec-short", "ZQWID", kSpecShort, "test-register");
    shorter.kind = PROF_KIND_FAMILY;

    Expect(ProfileRegister(&shorter), "short family first");
    Expect(ProfileRegister(&longer), "long family second");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("ZQWIDSPEC-DRIVE", "");
    Go();
    Expect(strcmp(g.szProfileId, "spec-long") == 0, "longer key is the profile");
    Expect(g.nProfileScore == 90, "family score");
    Expect(!g.bProfileAmbiguous, "different specificity is not a tie");
    Expect(g.szProfileConflict[0] == '\0', "no identity conflict");
    Expect(Lookup(0x11, &sem, &enc) && sem == PROF_SEM_MEDIA &&
           enc == PROF_ENC_COUNTER, "longer row supplies media");
    Expect(ProfileAttrLabel(&g, 0x11, &sem, &enc, &ru, NULL, NULL, NULL, NULL) &&
           ru && strcmp(ru, "Long media") == 0, "label follows the longer row");

    ProfileClearRegistered();
    Expect(ProfileRegister(&longer), "long family first");
    Expect(ProfileRegister(&shorter), "short family later");
    Go();
    Expect(strcmp(g.szProfileId, "spec-long") == 0, "longer key either order");
    Expect(!g.bProfileAmbiguous, "still one identity winner");
    Expect(Lookup(0x11, &sem, &enc) && sem == PROF_SEM_MEDIA,
           "later shorter row does not replace media");
    Expect(ProfileAttrLabel(&g, 0x11, &sem, &enc, &ru, NULL, NULL, NULL, NULL) &&
           ru && strcmp(ru, "Long media") == 0, "label stays on the longer row");
    ProfileClearRegistered();

    DefExact(&exact, "cover-exact", "ZQCOVERX", NULL, "test-register");
    DefExact(&media, "cover-media", "ZQCOVER", kCoverMedia, "test-register");
    media.kind = PROF_KIND_FAMILY;
    DefExact(&wear, "cover-wear", "ZQCOVER", kCoverWear, "test-register");
    wear.kind = PROF_KIND_FAMILY;
    g_db11 = 1;
    Expect(ProfileRegister(&exact), "exact cover");
    Expect(ProfileRegister(&media), "cover media first");
    Expect(ProfileRegister(&wear), "cover wear later");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("ZQCOVERX", "");
    Go();
    Expect(strcmp(g.szProfileId, "cover-exact") == 0, "exact profile stays");
    Expect(g.nProfileScore == 98, "exact score above the families");
    Expect(!g.bProfileAmbiguous, "lower-tier conflict is not identity ambiguity");
    Expect(g.szProfileConflict[0] == '\0', "identity conflict stays empty");
    Expect(g.eController == CONTROLLER_UNKNOWN, "cover does not invent a controller");
    Expect(!Lookup(0x11, &sem, &enc), "disagreeing lower tier is refused");
    Expect(ProfileListsAttr(&g, 0x11), "refused id stays listed");
    Expect(!ProfileAttrLabel(&g, 0x11, &sem, &enc, &ru, NULL, NULL, NULL, NULL),
           "refused id has no label");
    Expect(!ProfileHealthRule(&g, 0x11, &h), "refused id has no health rule");

    ProfileClearRegistered();
    Expect(ProfileRegister(&wear), "cover wear first");
    Expect(ProfileRegister(&exact), "exact cover second");
    Expect(ProfileRegister(&media), "cover media later");
    Go();
    Expect(strcmp(g.szProfileId, "cover-exact") == 0, "exact profile either order");
    Expect(!g.bProfileAmbiguous, "order does not make the profile ambiguous");
    Expect(!Lookup(0x11, &sem, &enc), "lower tier still refused");
    Expect(ProfileListsAttr(&g, 0x11), "listed either order");
    g_db11 = 0;
    ProfileClearRegistered();

    DefExact(&exact, "cover-exact", "ZQCOVERX", NULL, "test-register");
    DefExact(&media, "cover-same-b", "ZQCOVER", kCoverSame, "test-register");
    media.kind = PROF_KIND_FAMILY;
    DefExact(&wear, "cover-same-a", "ZQCOVER", kCoverSame, "test-register");
    wear.kind = PROF_KIND_FAMILY;
    Expect(ProfileRegister(&exact), "exact above agreement");
    Expect(ProfileRegister(&media), "same b");
    Expect(ProfileRegister(&wear), "same a");
    Go();
    Expect(strcmp(g.szProfileId, "cover-exact") == 0, "agreement keeps the exact profile");
    Expect(g.nProfileScore == 98, "agreement does not change the score");
    Expect(!g.bProfileAmbiguous, "agreeing lower tier is not an identity tie");
    Expect(Lookup(0x11, &sem, &enc) && sem == PROF_SEM_MEDIA &&
           enc == PROF_ENC_COUNTER, "agreeing lower tier supplies media");
    memset(&h, 0, sizeof(h));
    Expect(ProfileHealthRule(&g, 0x11, &h) && h.sem == PROF_SEM_MEDIA &&
           h.caution == 2 && h.bad == 0, "agreed rule fields are kept");
    ProfileClearRegistered();

    DefExact(&exact, "pref-hi", "ZZPREF", kPrefHi, "test-register");
    DefExact(&media, "pref-lo", "ZZPREF", kPrefLo, "test-register");
    Expect(ProfileRegister(&exact), "higher pref");
    Expect(ProfileRegister(&media), "lower pref");
    Blank(DRIVE_TYPE_SSD_SATA, FALSE);
    Name("ZZPREF", "");
    Go();
    Expect(g.bProfileAmbiguous, "equal rows stay an identity tie");
    Expect(!Lookup(0x42, &sem, &enc), "different pref refuses the attribute");
    Expect(ProfileListsAttr(&g, 0x42), "refused pref stays listed");
    Expect(ProfileAttrPref(&g, 0x42) == 0, "refused pref is not a number");
    ProfileClearRegistered();
}

int main(void)
{
    ProfileClearRegistered();
    TestPriority();
    TestSameIdDifferentMeaning();
    TestFirmwareShape();
    TestKpssSmi225();
    TestWriteVolume();
    TestRegister();
    TestDecodeAndUsb();
    TestHealthRule();
    TestAttrTier();
    ProfileClearRegistered();
    if (g_fail) {
        fprintf(stderr, "%d failed, %d checks\n", g_fail, g_check);
        return 1;
    }
    printf("ok %d checks\n", g_check);
    return 0;
}
