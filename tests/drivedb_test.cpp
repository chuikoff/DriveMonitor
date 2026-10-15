/* DriveMonitor - drivedb integration tests. GPL-2.0-or-later. See LICENSE.
 * Links the real drivedb.cpp, identification, decoder, and health engine.
 * profile_test.cpp still stubs DriveDbApply and DriveDbLookup.
 * Ranking stays heuristic: assertions name the winning family and the
 * warning text. They do not lock an EntryScore integer.
 * No physical disk is opened. The serial stays empty, so no mech snap
 * file is written. */
#include "smart.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_check = 0;
static DRIVE_INFO g;

static void Expect(int cond, const char* msg)
{
    g_check++;
    if (!cond) {
        fprintf(stderr, "FAIL %s\n", msg);
        g_fail++;
    }
}

static void ExpectInt(int got, int want, const char* msg)
{
    g_check++;
    if (got != want) {
        fprintf(stderr, "FAIL %s\n  got %d\n  want %d\n", msg, got, want);
        g_fail++;
    }
}

/* Same weights as AssessDriveHealth. withGap subtracts 2 when a shock
 * count has no earlier sample. */
static int EvidenceBlend(const DRIVE_INFO* p, int withGap)
{
    int n;
    if (!p) return 0;
    n = (20 * p->nConfCompleteness + 10 * p->nConfTransport +
         15 * p->nConfModelId + 20 * p->nConfVendorDecoder +
         35 * p->nConfHealthAssess + 50) / 100;
    if (n < 0) n = 0;
    if (n > 100) n = 100;
    if (withGap && p->nGSenseEvents > 0 && p->nGSenseDelta < 0) {
        n -= 2;
        if (n < 0) n = 0;
    }
    return n;
}

static void ExpectStr(const char* got, const char* want, const char* msg)
{
    g_check++;
    if (!got) got = "";
    if (!want) want = "";
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s\n  got \"%s\"\n  want \"%s\"\n", msg, got, want);
        g_fail++;
    }
}

static void ExpectHas(const char* got, const char* needle, const char* msg)
{
    g_check++;
    if (!got || !needle || strstr(got, needle) == NULL) {
        fprintf(stderr, "FAIL %s\n  got \"%s\"\n  needle \"%s\"\n",
                msg, got ? got : "", needle ? needle : "");
        g_fail++;
    }
}

static void ExpectNotHas(const char* got, const char* needle, const char* msg)
{
    g_check++;
    if (got && needle && strstr(got, needle) != NULL) {
        fprintf(stderr, "FAIL %s\n  got \"%s\"\n  unexpected \"%s\"\n",
                msg, got, needle);
        g_fail++;
    }
}

static void Show(const char* tag)
{
    fprintf(stderr,
            "  %s family=\"%s\" warn=\"%s\" profile=%s score=%d ctl=%d "
            "life=%d writes=%d erase=%d wear=%d media=%d iface=%d head=%d "
            "crc=%d end=%d db=%d\n",
            tag,
            g.szModelFamily, g.szDriveDbWarn, g.szProfileId, g.nProfileScore,
            (int)g.eController, g.nSSDLifeLeft, g.nSSDTotalWritesGB,
            g.nSSDAvgEraseCount, (int)g.eWear, (int)g.eReliability,
            (int)g.eInterface, (int)g.eHealthStatus, g.nCrcErrors,
            g.nEndurancePercent, g.nDriveDbAttrs);
}

static void Base(DRIVE_TYPE type, const char* model, const char* fw)
{
    memset(&g, 0, sizeof(g));
    g.eType = type;
    g.bSMART_Supported = TRUE;
    g.nEndurancePercent = -1;
    g.nSSDLifeLeft = -1;
    g.nSSDTotalWritesGB = -1;
    g.nSSDAvgEraseCount = -1;
    g.nHealthPercent = -1;
    lstrcpynA(g.szModel, model ? model : "", (int)sizeof(g.szModel));
    lstrcpynA(g.szFirmware, fw ? fw : "", (int)sizeof(g.szFirmware));
}

static int AddAttr(BYTE id, BYTE value, unsigned long long raw)
{
    int i;
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* a;
        int b;
        if (g.attrData.stAttributes[i].bAttrID != 0) continue;
        a = &g.attrData.stAttributes[i];
        a->bAttrID = id;
        a->bAttrValue = value;
        a->bWorstValue = value;
        for (b = 0; b < 6; b++) {
            a->bRawValue[b] = (BYTE)(raw & 0xFFu);
            raw >>= 8;
        }
        return 1;
    }
    return 0;
}

static void Run(void)
{
    ExtractSSDIndicators(&g);
    AssessDriveHealth(&g);
}

static int DbHas(BYTE id)
{
    int i;
    for (i = 0; i < g.nDriveDbAttrs; i++) {
        if (g.driveDbAttr[i].id == id) return 1;
    }
    return 0;
}

static const char* DbName(BYTE id)
{
    int i;
    for (i = 0; i < g.nDriveDbAttrs; i++) {
        if (g.driveDbAttr[i].id == id) return g.driveDbAttr[i].name;
    }
    return "";
}

static void ExpectSem(BYTE id, int wantSem, int wantEnc, const char* msg)
{
    int sem = -1;
    int enc = -1;
    int ok = ProfileLookup(&g, id, &sem, &enc) ? 1 : 0;
    Expect(ok, msg);
    ExpectInt(sem, wantSem, msg);
    ExpectInt(enc, wantEnc, msg);
}

static void ExpectNoLookup(BYTE id, const char* msg)
{
    int sem = 7;
    int enc = 7;
    int ok = ProfileLookup(&g, id, &sem, &enc) ? 1 : 0;
    Expect(!ok, msg);
}

static void ExpectDecoded(int enc, unsigned long long raw,
                          unsigned long long want, const char* msg)
{
    unsigned __int64 v = 0;
    int rc = ProfileDecodeRaw(enc, (unsigned __int64)raw, &v);
    ExpectInt(rc, 1, msg);
    Expect(v == (unsigned __int64)want, msg);
}

static void ExpectDecodeReject(int enc, unsigned long long raw, const char* msg)
{
    unsigned __int64 v = 99;
    int rc = ProfileDecodeRaw(enc, (unsigned __int64)raw, &v);
    ExpectInt(rc, 0, msg);
}

static void ExpectName(BYTE id, int known, const char* needle, const char* msg)
{
    ATTR_DECODE d;
    memset(&d, 0, sizeof(d));
    GetAttrDecode(id, &g, &d);
    ExpectInt((int)d.eState,
              known ? (int)ATTR_DECODE_KNOWN : (int)ATTR_DECODE_UNKNOWN, msg);
    if (needle)
        ExpectHas(d.szName, needle, msg);
}

static void CaseEnd(const char* tag, int before)
{
    if (g_fail != before) Show(tag);
}

static const SMART_ATTRIBUTE* Attr(BYTE id)
{
    int i;
    for (i = 0; i < 30; i++) {
        if (g.attrData.stAttributes[i].bAttrID == id)
            return &g.attrData.stAttributes[i];
    }
    return NULL;
}

/* Same rank the attribute row shows. SKIP means the rule does not grade. */
static void ExpectRank(BYTE id, int wantEv, int wantRank, const char* msg)
{
    const SMART_ATTRIBUTE* a = Attr(id);
    int rank = -99;
    int ev;
    Expect(a != NULL, msg);
    if (!a) return;
    ev = AttrEvidenceRank(&g, a, &rank);
    ExpectInt(ev, wantEv, msg);
    if (wantEv == ATTR_EV_RANK)
        ExpectInt(rank, wantRank, msg);
}

/* ADATA SU800 / R0918B. Six-character firmware, so the 7-character
 * SM2258 overlay does not hide drivedb 169/241/160. */
static void TestSu800(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ADATA SU800", "R0918B");
    Expect(AddAttr(169, 92, 92), "su800 slot 169");
    Expect(AddAttr(241, 100, 1024), "su800 slot 241");
    Expect(AddAttr(251, 100, 7), "su800 slot 251");
    Expect(AddAttr(160, 100, 3), "su800 slot 160");
    Run();
    ExpectStr(g.szModelFamily, "Silicon Motion based SSDs", "su800 family");
    ExpectStr(g.szProfileId, "su800", "su800 profile");
    ExpectInt(g.nProfileScore, 98, "su800 score");
    ExpectInt((int)g.eController, (int)CONTROLLER_SMI, "su800 controller");
    Expect(!g.bProfileAmbiguous, "su800 unique");
    Expect(g.nDriveDbAttrs > 0, "su800 presets stored");
    ExpectSem(169, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "su800 169");
    ExpectDecoded(PROF_ENC_PERCENT, 92, 92, "su800 169 value");
    ExpectInt(g.nSSDLifeLeft, 92, "su800 life");
    ExpectInt(g.nEndurancePercent, 92, "su800 endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "su800 wear");
    ExpectSem(241, PROF_SEM_HOST_WRITE, PROF_ENC_BLOCK32, "su800 241");
    ExpectDecoded(PROF_ENC_BLOCK32, 1024, 32, "su800 241 value");
    ExpectInt(g.nSSDTotalWritesGB, 32, "su800 writes");
    /* 251 is already in the generic name table. The drivedb Unkn_* row
     * does not replace that name and does not enter the health channel. */
    Expect(DbHas(251), "su800 251 stored");
    ExpectNoLookup(251, "su800 251 not health");
    ExpectName(251, 1, "Остаток запасных блоков", "su800 251 generic name");
    ExpectSem(160, PROF_SEM_MEDIA, PROF_ENC_COUNTER, "su800 160");
    /* 160 is a drivedb name, not a local rule. RAW 3 is Risk on the row.
     * The media channel and the headline stay Good. */
    ExpectRank(160, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "su800 160 risk");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "su800 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "su800 headline");
    ExpectInt(g.nEndurancePercent, 92, "su800 life kept");
    CaseEnd("su800", before);
}

/* Apacer AS350. Family is the AS340/350 row, not the firmware-constrained
 * "Apacer SSDs" row. 231 is endurance. 166 is a named counter and is not
 * a health input. 164 is erase and is not stored as the average. */
static void TestAs350(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Apacer AS350 512GB", "VE1R9008");
    Expect(AddAttr(231, 4, 4), "as350 slot 231");
    Expect(AddAttr(166, 100, 7), "as350 slot 166");
    Expect(AddAttr(164, 100, 13), "as350 slot 164");
    Run();
    ExpectStr(g.szModelFamily, "Apacer AS340/350 SSDs", "as350 family");
    ExpectNotHas(g.szModelFamily, "Apacer SSDs", "as350 not the wide Apacer row");
    ExpectStr(g.szProfileId, "as350", "as350 profile");
    ExpectInt(g.nProfileScore, 98, "as350 score");
    ExpectInt((int)g.eController, (int)CONTROLLER_SMI, "as350 controller");
    ExpectSem(231, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "as350 231");
    ExpectDecoded(PROF_ENC_PERCENT, 4, 4, "as350 231 value");
    ExpectInt(g.nSSDLifeLeft, 4, "as350 life");
    ExpectInt(g.nEndurancePercent, 4, "as350 endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_BAD, "as350 wear");
    ExpectInt(WearBadgeStatus(g.nEndurancePercent), (int)HEALTH_STATUS_CAUTION,
              "as350 wear badge");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "as350 media clean");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "as350 headline");
    ExpectNoLookup(166, "as350 166 not health");
    ExpectName(166, 1, "Плохие блоки", "as350 166 named");
    ExpectSem(164, PROF_SEM_ERASE, PROF_ENC_COUNTER, "as350 164");
    ExpectDecoded(PROF_ENC_COUNTER, 13, 13, "as350 164 value");
    ExpectInt(g.nSSDAvgEraseCount, -1, "as350 erase not stored");
    CaseEnd("as350", before);
}

/* Kingston A400 + SBFK. Local Phison rows keep 169 and 241.
 * 218 is a CRC name. It is Risk and does not fill the CRC total. */
static void TestA400Phison(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA9, 47, 47), "a400 slot A9");
    Expect(AddAttr(218, 100, 120), "a400 slot 218");
    Run();
    ExpectStr(g.szProfileId, "a400-sbfk", "a400 profile");
    ExpectInt(g.nProfileScore, 100, "a400 score");
    ExpectInt((int)g.eController, (int)CONTROLLER_PHISON, "a400 controller");
    ExpectStr(g.szModelFamily, "Phison Driven SSDs", "a400 family");
    ExpectSem(0xA9, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "a400 A9 local");
    ExpectDecoded(PROF_ENC_PERCENT, 47, 47, "a400 A9 value");
    ExpectInt(g.nSSDLifeLeft, 47, "a400 life");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "a400 wear");
    ExpectSem(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_SCALE, "a400 F1 local scale");
    ExpectDecodeReject(PROF_ENC_SCALE, 2048, "a400 scale rejected");
    ExpectInt(g.nSSDTotalWritesGB, -1, "a400 no write total");
    /* 218 is a CRC name, not attribute 199. The row is Risk. The confirmed
     * CRC total stays empty, so the headline is not Caution. */
    ExpectInt(g.nCrcErrors, -1, "a400 crc not published");
    ExpectRank(218, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "a400 218 ranked");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_OBSERVE, "a400 interface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "a400 headline");
    CaseEnd("a400-sbfk", before);
}

/* Generic 199 fills the CRC slot even at zero, so drivedb 218 does not. */
static void TestA400CrcSlot(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA9, 47, 47), "a400crc slot A9");
    Expect(AddAttr(199, 100, 0), "a400crc slot 199");
    Expect(AddAttr(218, 100, 50), "a400crc slot 218");
    Run();
    ExpectInt(g.nCrcErrors, 0, "a400crc count");
    ExpectRank(199, ATTR_EV_RANK, (int)HEALTH_STATUS_GOOD, "a400crc 199 ranked");
    ExpectRank(218, ATTR_EV_SKIP, 0, "a400crc 218 suppressed");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_GOOD, "a400crc interface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "a400crc headline");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "a400crc wear");
    CaseEnd("a400-crc", before);
}

/* A400 + 7-character SM2258 firmware. The Phison family names other
 * silicon and is skipped. 241 is local 32 MiB blocks, not drivedb GiB. */
static void TestA400Smi(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "U0524A0");
    /* 164 is the total. It is listed before the average, so a bare
     * counter stored as the average would hide attribute 167. */
    Expect(AddAttr(0xA4, 100, 9000), "a400smi slot A4");
    Expect(AddAttr(0xA7, 100, 5), "a400smi slot A7");
    Expect(AddAttr(0xA9, 80, 80), "a400smi slot A9");
    Expect(AddAttr(0xF1, 100, 2048), "a400smi slot F1");
    Run();
    ExpectStr(g.szProfileId, "a400-smi225", "a400smi profile");
    ExpectInt(g.nProfileScore, 100, "a400smi score");
    ExpectInt((int)g.eController, (int)CONTROLLER_SMI, "a400smi controller");
    ExpectStr(g.szModelFamily, "", "a400smi family empty");
    ExpectSem(0xF1, PROF_SEM_HOST_WRITE, PROF_ENC_BLOCK32, "a400smi F1");
    ExpectDecoded(PROF_ENC_BLOCK32, 2048, 64, "a400smi F1 value");
    ExpectInt(g.nSSDTotalWritesGB, 64, "a400smi writes");
    ExpectInt(g.nSSDLifeLeft, 80, "a400smi life");
    ExpectSem(0xA7, PROF_SEM_ERASE, PROF_ENC_AVG, "a400smi A7");
    ExpectInt(g.nSSDAvgEraseCount, 5, "a400smi average erase");
    ExpectSem(0xA4, PROF_SEM_ERASE, PROF_ENC_COUNTER, "a400smi A4");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "a400smi wear");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "a400smi headline");
    CaseEnd("a400-smi", before);
}

/* SPCC matches the Phison OEM family by firmware. The family does not
 * set the controller. 231 and 218 come from drivedb. */
static void TestSpcc(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "SPCC Solid State Disk", "SBFM61.2");
    Expect(AddAttr(231, 55, 55), "spcc slot 231");
    Expect(AddAttr(218, 100, 7), "spcc slot 218");
    Expect(AddAttr(244, 100, 9), "spcc slot 244");
    Run();
    ExpectStr(g.szModelFamily, "Phison Driven OEM SSDs", "spcc family");
    ExpectInt((int)g.eController, (int)CONTROLLER_UNKNOWN, "spcc controller");
    ExpectStr(g.szProfileId, "ata-generic", "spcc profile");
    ExpectInt(g.nProfileScore, 50, "spcc score");
    ExpectSem(231, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "spcc 231");
    ExpectInt(g.nSSDLifeLeft, 55, "spcc life");
    ExpectInt(g.nEndurancePercent, 55, "spcc endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "spcc wear");
    ExpectInt(g.nCrcErrors, -1, "spcc crc not published");
    ExpectRank(218, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "spcc 218 risk");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_OBSERVE, "spcc interface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "spcc headline");
    ExpectSem(244, PROF_SEM_ERASE, PROF_ENC_COUNTER, "spcc 244");
    ExpectDecoded(PROF_ENC_COUNTER, 9, 9, "spcc 244 value");
    ExpectInt(g.nSSDAvgEraseCount, -1, "spcc erase not stored");
    CaseEnd("spcc", before);
}

/* Samsung 870 EVO + SVT. Local E7 reads Value. 252 is not a media count. */
static void TestSamsung870(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Expect(AddAttr(0xE7, 98, 26), "870 slot E7");
    Expect(AddAttr(252, 100, 133), "870 slot 252");
    Run();
    ExpectStr(g.szProfileId, "samsung-870-svt", "870 profile");
    ExpectInt(g.nProfileScore, 100, "870 score");
    ExpectInt((int)g.eController, (int)CONTROLLER_SAMSUNG, "870 controller");
    ExpectStr(g.szModelFamily, "Samsung based SSDs", "870 family");
    ExpectStr(g.szDriveDbTie, "", "870 no drivedb tie");
    ExpectSem(0xE7, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "870 E7");
    ExpectInt(ProfileAttrPref(&g, 0xE7), 1, "870 E7 pref");
    ExpectInt(g.nSSDLifeLeft, 98, "870 life from value");
    ExpectInt(g.nEndurancePercent, 98, "870 endurance");
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) == 0, "870 no agreeing flag");
    ExpectNoLookup(252, "870 252 not health");
    ExpectName(252, 1, "Added Bad Flash Blk Ct", "870 252 label");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "870 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "870 headline");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "870 wear");
    CaseEnd("870", before);
}

/* Barracuda part number, old firmware warning, and new firmware row.
 * Unnamed -v lines are not stored. Reallocated 1 on HDD is caution. */
static void TestBarracuda(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST1000DM003-1CH162", "CC47");
    Expect(AddAttr(5, 100, 1), "barra slot 5");
    Run();
    ExpectStr(g.szModelFamily, "Seagate Barracuda 7200.14 (AF)", "barra family");
    ExpectStr(g.szDriveDbWarn, "", "barra warning empty");
    ExpectStr(g.szProfileId, "seagate-hdd", "barra profile");
    ExpectInt(g.nProfileScore, 70, "barra score");
    ExpectInt((int)g.eController, (int)CONTROLLER_SEAGATE, "barra controller");
    Expect(!DbHas(188), "barra unnamed 188 not stored");
    ExpectInt(g.nReallocated, 1, "barra realloc");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CAUTION, "barra media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "barra headline");
    Expect(g.eHealthStatus != HEALTH_STATUS_BAD, "barra not bad");
    ExpectInt(g.nHealthPercent, -1, "barra no health percent");
    Expect(g.nConfCompleteness > 0, "barra completeness stored");
    Expect(EvidenceBlend(&g, 1) > 0, "barra blend above zero");
    ExpectInt(g.nConfidence, EvidenceBlend(&g, 1), "barra confidence is the blend");
    CaseEnd("barra-1ch", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST1000DM003-1CH162", "CC47");
    Expect(AddAttr(197, 100, 2), "barra slot 197");
    Run();
    ExpectInt(g.nPendingSectors, 2, "barra pending");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CAUTION, "barra pending caution");
    Expect(g.eReliability != HEALTH_STATUS_BAD, "barra pending not bad");
    CaseEnd("barra-pending", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST1000DM003-9YN162", "CC46");
    Run();
    ExpectStr(g.szModelFamily, "Seagate Barracuda 7200.14 (AF)", "barra old family");
    ExpectHas(g.szDriveDbWarn, "is available", "barra old warning");
    ExpectNotHas(g.szDriveDbWarn, "may be available", "barra old not the unknown row");
    CaseEnd("barra-cc46", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST3000DM001-9YN166", "CC4H");
    Run();
    ExpectStr(g.szModelFamily, "Seagate Barracuda 7200.14 (AF)", "barra new family");
    ExpectStr(g.szDriveDbWarn, "", "barra new warning empty");
    CaseEnd("barra-cc4h", before);
}

/* HDD confidence is the evidence blend. No wear scale means
 * nHealthPercent stays -1. A shock count with no prior sample
 * still subtracts 2 and does not wipe the blend. */
static void TestHddConfidence(void)
{
    int before = g_fail;
    int blend;
    Base(DRIVE_TYPE_HDD, "ST1000DM003-1CH162", "CC47");
    Expect(AddAttr(5, 100, 0), "hdd conf realloc");
    Expect(AddAttr(197, 100, 0), "hdd conf pending");
    Expect(AddAttr(198, 100, 0), "hdd conf uncorr");
    Expect(AddAttr(191, 100, 10), "hdd conf gsense");
    Run();
    blend = EvidenceBlend(&g, 0);
    ExpectInt(g.nHealthPercent, -1, "hdd conf no health percent");
    ExpectInt(g.nGSenseEvents, 10, "hdd conf gsense count");
    ExpectInt(g.nGSenseDelta, -1, "hdd conf no gsense trend");
    Expect(g.nConfCompleteness > 0, "hdd conf completeness stored");
    Expect(blend > 2, "hdd conf blend above the gap");
    ExpectInt(g.nConfidence, blend - 2, "hdd conf gsense gap");
    ExpectInt(g.nConfidence, EvidenceBlend(&g, 1), "hdd conf keeps the blend");
    CaseEnd("hdd-confidence", before);
}

/* USB bridge rows are skipped. The model string is not a disk family
 * and "-d sat" is not executed. */
static void TestUsbSkip(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "0x0080:0x0578", "");
    Run();
    ExpectStr(g.szModelFamily, "", "usb family empty");
    ExpectStr(g.szDriveDbWarn, "", "usb warning empty");
    ExpectInt(g.nDriveDbAttrs, 0, "usb no presets");
    ExpectInt(g.nEndurancePercent, -1, "usb no endurance");
    ExpectInt((int)g.eController, (int)CONTROLLER_UNKNOWN, "usb controller");
    ExpectStr(g.szProfileId, "ata-generic", "usb profile");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "usb wear");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "usb media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "usb headline");
    CaseEnd("usb", before);
}

/* Two exact ZZTIEPROBE rows share a score. The earlier family is the
 * label. 231 is endurance on that row and erase count on the later row,
 * so it is not applied. 5 agrees and stays. 232 exists only on the later
 * row and is not imported. */
static void TestDriveDbTie(void)
{
    int before = g_fail;
    int sem = -1, enc = -1;
    char lecture[8192];
    Base(DRIVE_TYPE_SSD_SATA, "ZZTIEPROBE", "1.0");
    Expect(AddAttr(0xE7, 40, 40), "tie slot 231");
    Expect(AddAttr(5, 100, 3), "tie slot 5");
    Expect(AddAttr(232, 100, 15), "tie slot 232");
    Run();
    ExpectStr(g.szModelFamily, "Tie Probe Earlier", "tie family is earlier");
    ExpectStr(g.szDriveDbTie, "Tie Probe Later", "tie names the other family");
    Expect(!g.bDriveDbTieUnnamed, "named tie is not unnamed");
    Expect(!g.bProfileAmbiguous, "drivedb tie is not a profile tie");
    ExpectStr(g.szProfileConflict, "", "drivedb tie is not szProfileConflict");
    Expect(!DriveDbAttrEnc(&g, 231, &sem, &enc), "tie 231 dropped");
    Expect(!DriveDbAttrEnc(&g, 232, &sem, &enc), "tie 232 not imported");
    Expect(DriveDbAttrEnc(&g, 5, &sem, &enc), "tie 5 stored");
    ExpectInt(sem, PROF_SEM_MEDIA, "tie 5 sem");
    ExpectInt(enc, PROF_ENC_COUNTER, "tie 5 enc");
    ExpectNoLookup(231, "tie 231 not endurance");
    ExpectInt(g.nSSDLifeLeft, -1, "tie life unset");
    ExpectInt(g.nEndurancePercent, -1, "tie endurance unset");
    FormatHealthLecturePlain(&g, lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Tie Probe Earlier", "tie lecture family");
    ExpectHas(lecture, "Tie Probe Later", "tie lecture other");
    ExpectHas(lecture, "не применяется", "tie lecture refusal");
    CaseEnd("tie", before);
}

/* Equal score with an empty family. Disagreed 231 is still dropped, and
 * the lecture names the gap. ZZTIEANON's earlier row has no family.
 * ZZTIENONE's later row has no family. */
static void TestDriveDbTieUnnamed(void)
{
    int before = g_fail;
    int sem = -1, enc = -1;
    char lecture[8192];

    Base(DRIVE_TYPE_SSD_SATA, "ZZTIEANON", "1.0");
    Expect(AddAttr(231, 100, 40), "anonwin slot 231");
    Expect(AddAttr(5, 100, 3), "anonwin slot 5");
    Run();
    ExpectStr(g.szModelFamily, "", "anonwin family empty");
    ExpectStr(g.szDriveDbTie, "Anon Tie Later", "anonwin names the later family");
    Expect(g.bDriveDbTieUnnamed, "anonwin marks the empty family");
    Expect(!DriveDbAttrEnc(&g, 231, &sem, &enc), "anonwin 231 dropped");
    Expect(DriveDbAttrEnc(&g, 5, &sem, &enc), "anonwin 5 stored");
    FormatHealthLecturePlain(&g, lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Семейство: нет имени", "anonwin lecture family");
    ExpectHas(lecture, "Anon Tie Later", "anonwin lecture other");
    ExpectHas(lecture, "нет имени семейства", "anonwin lecture gap");
    ExpectHas(lecture, "не применяется", "anonwin lecture refusal");
    CaseEnd("tie-anon-win", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ZZTIENONE", "1.0");
    Expect(AddAttr(231, 100, 40), "anontie slot 231");
    Expect(AddAttr(5, 100, 3), "anontie slot 5");
    Run();
    ExpectStr(g.szModelFamily, "Anon Tie Earlier", "anontie family is earlier");
    ExpectStr(g.szDriveDbTie, "", "anontie has no other name");
    Expect(g.bDriveDbTieUnnamed, "anontie marks the empty family");
    Expect(!DriveDbAttrEnc(&g, 231, &sem, &enc), "anontie 231 dropped");
    Expect(DriveDbAttrEnc(&g, 5, &sem, &enc), "anontie 5 stored");
    FormatHealthLecturePlain(&g, lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Anon Tie Earlier", "anontie lecture family");
    ExpectHas(lecture, "без имени семейства", "anontie lecture gap");
    ExpectHas(lecture, "не применяется", "anontie lecture refusal");
    ExpectNotHas(lecture, "Семейство: нет имени", "anontie keeps its family");
    CaseEnd("tie-anon-other", before);
}

/* Row badge and the health channel share one rank. ROWTEST stays generic:
 * an ST* model would select Seagate. */
static void TestRowRank(void)
{
    int before;
    char lecture[8192];

    ExpectInt(WearBadgeStatus(4), (int)HEALTH_STATUS_CAUTION, "badge 4");
    ExpectInt(WearBadgeStatus(5), (int)HEALTH_STATUS_CAUTION, "badge 5");
    ExpectInt(WearBadgeStatus(8), (int)HEALTH_STATUS_CAUTION, "badge 8");
    ExpectInt(WearBadgeStatus(10), (int)HEALTH_STATUS_CAUTION, "badge 10");
    ExpectInt(WearBadgeStatus(15), (int)HEALTH_STATUS_OBSERVE, "badge 15");
    ExpectInt(WearBadgeStatus(21), (int)HEALTH_STATUS_GOOD, "badge 21");
    ExpectInt(WearBadgeStatus(-1), (int)HEALTH_STATUS_UNKNOWN, "badge none");

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(5, 100, 10), "row ssd5 slot");
    Run();
    ExpectStr(g.szProfileId, "ata-generic", "row ssd profile");
    ExpectRank(5, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "ssd realloc 10");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CAUTION, "ssd realloc media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "ssd realloc head");
    CaseEnd("row-ssd5", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ROWTEST", "1.0");
    Expect(AddAttr(5, 100, 10), "row hdd5 slot");
    Run();
    ExpectStr(g.szProfileId, "ata-generic", "row hdd profile");
    ExpectRank(5, ATTR_EV_RANK, (int)HEALTH_STATUS_BAD, "hdd realloc 10");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_BAD, "hdd realloc media");
    CaseEnd("row-hdd5", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ROWTEST", "1.0");
    Expect(AddAttr(197, 100, 2), "row pend2 slot");
    Run();
    ExpectRank(197, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "hdd pending 2");
    CaseEnd("row-pend2", before);

    /* Confirmed media Risk still sets the headline. Attribute 184 is a
     * local HDD rule: observe from 1, caution from 5. */
    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ROWTEST", "1.0");
    Expect(AddAttr(184, 100, 3), "row e2e slot");
    Run();
    ExpectRank(184, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "hdd e2e 3");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_OBSERVE, "hdd e2e media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_OBSERVE, "hdd e2e headline");
    CaseEnd("row-e2e", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ROWTEST", "1.0");
    Expect(AddAttr(197, 100, 4), "row pend4 slot");
    Run();
    ExpectRank(197, ATTR_EV_RANK, (int)HEALTH_STATUS_BAD, "hdd pending 4");
    CaseEnd("row-pend4", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(197, 100, 4), "row ssdp slot");
    Run();
    ExpectRank(197, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "ssd pending 4");
    CaseEnd("row-ssdp", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(199, 100, 10), "row crc10 slot");
    Run();
    ExpectRank(199, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "crc 10");
    CaseEnd("row-crc10", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(199, 100, 99), "row crc99 slot");
    Run();
    ExpectRank(199, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "crc 99");
    CaseEnd("row-crc99", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(199, 100, 100), "row crc100 slot");
    Run();
    ExpectRank(199, ATTR_EV_RANK, (int)HEALTH_STATUS_BAD, "crc 100");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_BAD, "crc 100 iface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "crc 100 head");
    CaseEnd("row-crc100", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(187, 100, 3), "row ssd187 slot");
    Run();
    ExpectRank(187, ATTR_EV_SKIP, 0, "ssd 187 skipped");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "ssd 187 media");
    CaseEnd("row-ssd187", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ROWTEST", "1.0");
    Expect(AddAttr(187, 100, 3), "row hdd187 slot");
    Run();
    ExpectRank(187, ATTR_EV_RANK, (int)HEALTH_STATUS_BAD, "hdd 187");
    CaseEnd("row-hdd187", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Expect(AddAttr(0xBB, 100, 1), "row sam187 slot");
    Run();
    ExpectStr(g.szProfileId, "samsung-870-svt", "sam187 profile");
    ExpectRank(0xBB, ATTR_EV_RANK, (int)HEALTH_STATUS_BAD, "samsung 187");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_BAD, "samsung 187 media");
    CaseEnd("row-sam187", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Crucial_CT500MX200SSD1", "MU01");
    Expect(AddAttr(187, 100, 3), "mx200 slot");
    Run();
    ExpectStr(g.szProfileId, "mx200", "mx200 profile");
    ExpectRank(187, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "mx200 187");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CAUTION, "mx200 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "mx200 head");
    CaseEnd("row-mx200", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Crucial_CT500MX200SSD1", "MU01");
    Expect(AddAttr(187, 100, 0), "mx200 zero slot");
    Run();
    ExpectRank(187, ATTR_EV_RANK, (int)HEALTH_STATUS_GOOD, "mx200 187 zero");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "mx200 zero head");
    CaseEnd("row-mx200-0", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "CT500MX200SSD1", "MU01");
    Expect(AddAttr(187, 100, 3), "mx200 bare slot");
    Run();
    ExpectStr(g.szProfileId, "mx200", "mx200 bare profile");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "mx200 bare head");
    CaseEnd("row-mx200-bare", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Micron_M500_MTFDDAK960MAV", "MU05");
    Expect(AddAttr(187, 100, 3), "m500 slot");
    Run();
    ExpectStr(g.szProfileId, "crucial-micron", "m500 profile");
    ExpectRank(187, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "m500 187");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "m500 head");
    CaseEnd("row-m500", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA1, 100, 5), "row a1 slot");
    Run();
    ExpectRank(0xA1, ATTR_EV_SKIP, 0, "phison spare off");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "phison spare media");
    CaseEnd("row-a1", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "WDC WD40EZAZ-00SF3B0", "80.00A80");
    Run();
    ExpectHas(g.szDriveDbWarn, "heise.de/-10800960", "smr url stored");
    ExpectHas(g.szDriveDbWarn, "older WD SMR", "smr english stored");
    ExpectNotHas(g.szDriveDbWarn, "у старых", "smr russian not stored");
    FormatHealthLecturePlain(&g, lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "у старых WD SMR", "smr lecture ru");
    ExpectHas(lecture, "heise.de/-10800960", "smr lecture url");
    ExpectHas(lecture, "Замечание к семейству", "smr lecture label");
    CaseEnd("row-smr", before);
}

/* SMART Modular attribute 1 is raw48 Uncorrectable_ECC_Cnt. The name is
 * only a hint, so RAW 5 is Risk on the row and the headline stays Good.
 * RAW 0 stays Good. Attribute 199 keeps the local CRC thresholds. */
static void TestSmartModular(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "SH9MST6D16GJSI01", "");
    Expect(AddAttr(1, 100, 5), "smartmod slot 1");
    Run();
    ExpectStr(g.szModelFamily, "SMART Modular Technologies mSATA XL+ SLC SSDs",
              "smartmod family");
    ExpectSem(1, PROF_SEM_MEDIA, PROF_ENC_COUNTER, "smartmod 1");
    ExpectRank(1, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "smartmod 1 risk");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "smartmod media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "smartmod headline");
    Expect(g.eHealthStatus != HEALTH_STATUS_CAUTION, "smartmod not caution");
    CaseEnd("smartmod-5", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "SH9MST6D16GJSI01", "");
    Expect(AddAttr(1, 100, 0), "smartmod0 slot 1");
    Run();
    ExpectRank(1, ATTR_EV_RANK, (int)HEALTH_STATUS_GOOD, "smartmod 1 zero");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "smartmod0 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "smartmod0 headline");
    CaseEnd("smartmod-0", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "SH9MST6D16GJSI01", "");
    Expect(AddAttr(199, 100, 10), "smartmod199 slot");
    Run();
    ExpectRank(199, ATTR_EV_RANK, (int)HEALTH_STATUS_CAUTION, "smartmod 199 local");
    ExpectInt(g.nCrcErrors, 10, "smartmod 199 count");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "smartmod 199 media");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION, "smartmod 199 iface");
    /* Confirmed CRC on a clean medium raises the headline to Caution.
     * That ceiling is the 1.7.23 rule. */
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "smartmod 199 headline");
    CaseEnd("smartmod-199", before);
}

static void ExpectCell(BYTE id, const char* want, const char* msg);
static int CellKind(BYTE id);
static int PlainCell(BYTE id);

/* Packed presets are not one 48-bit count. A huge raw24/raw32 value
 * stays unscored. A plain raw48 name on the same family is only Risk.
 * The value cell prints the -v fields, not the 48-bit integer. */
static void TestPackedName(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ZZPACKPROBE", "1.0");
    Expect(AddAttr(160, 100, 0x0100000005ULL), "pack slot 160");
    Expect(AddAttr(161, 100, 0x0005002AULL), "pack slot 161");
    Expect(AddAttr(218, 100, 0x0100000078ULL), "pack slot 218");
    Run();
    ExpectStr(g.szModelFamily, "Packed Name Probe", "pack family");
    ExpectSem(160, PROF_SEM_MEDIA, PROF_ENC_PACKED, "pack 160");
    ExpectSem(161, PROF_SEM_MEDIA, PROF_ENC_PACKED, "pack 161");
    ExpectSem(218, PROF_SEM_INTERFACE, PROF_ENC_PACKED, "pack 218");
    ExpectDecodeReject(PROF_ENC_PACKED, 0x0100000005ULL, "pack decode");
    ExpectRank(160, ATTR_EV_SKIP, 0, "pack 160 skipped");
    ExpectRank(161, ATTR_EV_SKIP, 0, "pack 161 skipped");
    ExpectRank(218, ATTR_EV_SKIP, 0, "pack 218 skipped");
    ExpectInt(g.nCrcErrors, -1, "pack crc empty");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "pack media");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_GOOD, "pack iface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "pack headline");
    /* The value cell is this text. FormatSmartValue returns it before
     * any id switch, so the 48-bit integer is not the cell. */
    ExpectCell(160, "1/5", "pack 160 fields");
    ExpectCell(161, "42 (0 5)", "pack 161 fields");
    ExpectCell(218, "1/120", "pack 218 fields");
    ExpectInt(CellKind(160), (int)RAW_ENC_PACKED, "pack 160 kind");
    ExpectInt(CellKind(161), (int)RAW_ENC_PACKED, "pack 161 kind");
    ExpectInt(CellKind(218), (int)RAW_ENC_PACKED, "pack 218 kind");
    CaseEnd("pack", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ZZPACKPROBE", "1.0");
    Expect(AddAttr(170, 100, 5), "pack170 slot");
    Run();
    ExpectSem(170, PROF_SEM_MEDIA, PROF_ENC_COUNTER, "pack 170");
    ExpectRank(170, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "pack 170 risk");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "pack170 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "pack170 headline");
    Expect(g.eReliability != HEALTH_STATUS_CAUTION, "pack170 not caution");
    ExpectInt(PlainCell(170), 0, "pack 170 stays a counter");
    ExpectInt(CellKind(170), (int)RAW_ENC_COUNTER48, "pack 170 kind");
    CaseEnd("pack-170", before);
}

/* Representation of one value cell. 1 means the text replaced a counter. */
static int PlainCell(BYTE id)
{
    const SMART_ATTRIBUTE* a = Attr(id);
    char buf[80];
    if (!a) return -1;
    buf[0] = '\0';
    return FormatAttrRawText(&g, id, a->bRawValue,
                             (unsigned)a->bAttrValue, (unsigned)a->bWorstValue,
                             buf, (int)sizeof(buf));
}

static void ExpectCell(BYTE id, const char* want, const char* msg)
{
    const SMART_ATTRIBUTE* a = Attr(id);
    char buf[80];
    int rc;
    Expect(a != NULL, msg);
    if (!a) return;
    buf[0] = '\0';
    rc = FormatAttrRawText(&g, id, a->bRawValue,
                           (unsigned)a->bAttrValue, (unsigned)a->bWorstValue,
                           buf, (int)sizeof(buf));
    ExpectInt(rc, 1, msg);
    ExpectStr(buf, want, msg);
    Expect(strstr(buf, "4294967") == NULL, msg);
}

static int CellKind(BYTE id)
{
    ATTR_DECODE d;
    memset(&d, 0, sizeof(d));
    GetAttrDecode(id, &g, &d);
    return (int)d.eEnc;
}

/* A local packed row has no field widths, so the cell says so.
 * A local pair is bytes [1:0] and [3:2]. A drivedb byte order is not
 * an id: z54z10 prints the two 24-bit fields of that order. */
static const PROF_ATTR kWidgetPack[] = {
    { 0x42, PROF_SEM_NONE, PROF_ENC_PACKED, 0, "Packed", "Packed",
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0 },
    { 0x43, PROF_SEM_NONE, PROF_ENC_PAIR, 0, "Pair", "Pair",
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static void TestPackedCell(void)
{
    PROFILE_DEF d;
    BYTE raw[6];
    char buf[80];
    int before = g_fail;
    int rc;
    ProfileClearRegistered();
    Base(DRIVE_TYPE_SSD_SATA, "WIDGETPACK", "");
    Expect(AddAttr(0x42, 100, 0x0100000005ULL), "local pack slot");
    Expect(AddAttr(0x43, 100, 0x0005002AULL), "local pair slot");
    memset(&d, 0, sizeof(d));
    d.id = "widget-pack";
    d.kind = PROF_KIND_EXACT;
    d.model = "WIDGETPACK";
    d.fwMode = PROF_FW_NONE;
    d.vendor = VENDOR_UNKNOWN;
    d.ctl = CONTROLLER_UNKNOWN;
    d.attrs = kWidgetPack;
    d.source = "test";
    Expect(ProfileRegister(&d), "pack register");
    Run();
    ExpectStr(g.szProfileId, "widget-pack", "pack profile");
    ExpectCell(0x42, "не декодирован", "local packed text");
    ExpectInt(CellKind(0x42), (int)RAW_ENC_PACKED, "local packed kind");
    ExpectCell(0x43, "42 / 5", "local pair text");
    ExpectInt(CellKind(0x43), (int)RAW_ENC_PAIR, "local pair kind");
    ExpectInt(g.nSSDLifeLeft, -1, "local packed is not life");
    ProfileClearRegistered();
    CaseEnd("pack-local", before);

    before = g_fail;
    memset(raw, 0, sizeof(raw));
    raw[0] = 7;
    raw[4] = 9;
    buf[0] = '\0';
    rc = FormatRawFields(PROF_VIEW_RAW24_24, "z54z10", raw, 100, 100,
                         buf, (int)sizeof(buf));
    ExpectInt(rc, 1, "z54z10 rc");
    ExpectStr(buf, "9/7", "z54z10 fields");
    buf[0] = '\0';
    rc = FormatRawFields(PROF_VIEW_UNDECODED, "", raw, 0, 0,
                         buf, (int)sizeof(buf));
    ExpectInt(rc, 1, "undecoded rc");
    ExpectStr(buf, "не декодирован", "undecoded text");
    buf[0] = '\0';
    rc = FormatRawFields(PROF_VIEW_NUMBER, "", raw, 0, 0,
                         buf, (int)sizeof(buf));
    ExpectInt(rc, 0, "number view is not fields");
    ExpectStr(buf, "", "number view writes nothing");
    CaseEnd("pack-order", before);
}

/* A 44-character -v name is stored as 43. The HDD/SSD flag after the
 * comma still applies. 162 is a short HDD name on the same row. */
static void TestLongPresetName(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_HDD, "ZZLONGNAME", "");
    Run();
    ExpectStr(g.szModelFamily, "Long Name Probe", "long family");
    Expect(DbHas(160), "long hdd keeps hdd name");
    Expect(!DbHas(161), "long hdd drops ssd name");
    Expect(DbHas(162), "long hdd keeps short name");
    ExpectStr(DbName(160),
              "ABCDEFGHIJ ABCDEFGHIJ ABCDEFGHIJ ABCDEFGHIJ",
              "long hdd name cut");
    CaseEnd("long-hdd", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ZZLONGNAME", "");
    Run();
    Expect(!DbHas(160), "long ssd drops hdd name");
    Expect(DbHas(161), "long ssd keeps ssd name");
    Expect(!DbHas(162), "long ssd drops short hdd name");
    ExpectStr(DbName(161),
              "ABCDEFGHIJ ABCDEFGHIJ ABCDEFGHIJ ABCDEFGHIJ",
              "long ssd name cut");
    CaseEnd("long-ssd", before);
}

/* Must run before any other DriveDbApply. Two threads enter compile
 * together. Each keeps its own DRIVE_INFO. The families are checked
 * after both threads return. */
static DRIVE_INFO gOnceA, gOnceB;

struct OnceJob {
    DRIVE_INFO* info;
    DRIVE_TYPE type;
    const char* model;
    const char* fw;
};

/* PCI VID is bytes 0-1 of NVMe Identify. The drive stays a SATA SSD:
 * bIsNVMe is clear, so life is not Percentage Used. */
static void SetPciVid(unsigned vid)
{
    g.bGotNVMeIdent = TRUE;
    g.nvmeIdent.Reserved0[0] = (BYTE)(vid & 0xFFu);
    g.nvmeIdent.Reserved0[1] = (BYTE)((vid >> 8) & 0xFFu);
}

/* Micron 202 is endurance on the controller profile. RAW 9999 is not the
 * percent. The normalized Value is. 173 is the average erase count. */
static void TestMicronCa(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Micron_1100_MTFD", "M0MU031");
    SetPciVid(0x1344);
    Expect(AddAttr(0xCA, 73, 9999), "micron slot CA");
    Expect(AddAttr(0xAD, 100, 42), "micron slot AD");
    Run();
    ExpectStr(g.szProfileId, "micron-ssd", "micron profile");
    ExpectInt((int)g.eController, (int)CONTROLLER_MICRON, "micron controller");
    ExpectSem(0xCA, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "micron CA");
    ExpectInt(g.nSSDLifeLeft, 73, "micron life");
    ExpectInt(g.nEndurancePercent, 73, "micron endurance");
    ExpectInt(g.nHealthPercent, 73, "micron health percent");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "micron wear");
    ExpectInt(g.nSSDAvgEraseCount, 42, "micron average erase");
    ExpectInt(g.nSSDTotalWritesGB, -1, "micron writes unset");
    CaseEnd("micron-ca", before);
}

/* Intel 233 is a wear percent, not life and not GiB. 225 is host writes
 * in 32 MiB blocks. 233 is stored first so a mistaken volume would hide 225. */
static void TestIntelIndicators(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "INTEL SSDSC2KB480G8", "");
    SetPciVid(0x8086);
    Expect(AddAttr(0xE9, 30, 30), "intel slot E9");
    Expect(AddAttr(0xE1, 100, 2048), "intel slot E1");
    Run();
    ExpectStr(g.szProfileId, "intel-ssd", "intel profile");
    ExpectInt((int)g.eController, (int)CONTROLLER_INTEL, "intel controller");
    ExpectSem(0xE9, PROF_SEM_WEAR, PROF_ENC_PERCENT, "intel E9");
    ExpectInt(g.nSSDLifeLeft, -1, "intel wear percent is not life");
    ExpectInt(g.nEndurancePercent, -1, "intel endurance unset");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "intel wear");
    ExpectInt(g.nSSDTotalWritesGB, 64, "intel E1 writes");
    CaseEnd("intel", before);
}

/* An id the builtin tables do not name becomes life only after a profile
 * says so. No new case in ExtractSSDIndicators. */
static const PROF_ATTR kWidgetLife[] = {
    { 0x42, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, 0, "Widget life", "Widget life",
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0 }
};

static void TestWidgetLife(void)
{
    PROFILE_DEF d;
    int before = g_fail;
    ProfileClearRegistered();
    Base(DRIVE_TYPE_SSD_SATA, "WIDGETLIFE", "");
    Expect(AddAttr(0x42, 4, 1), "widget slot");
    Run();
    ExpectInt(g.nSSDLifeLeft, -1, "unknown id is not life");
    ExpectInt(g.nEndurancePercent, -1, "unknown id endurance");
    memset(&d, 0, sizeof(d));
    d.id = "widget-life";
    d.kind = PROF_KIND_EXACT;
    d.model = "WIDGETLIFE";
    d.fwMode = PROF_FW_NONE;
    d.vendor = VENDOR_UNKNOWN;
    d.ctl = CONTROLLER_UNKNOWN;
    d.nvme = 0;
    d.hdd = 0;
    d.attrs = kWidgetLife;
    d.source = "test";
    Expect(ProfileRegister(&d), "widget register");
    Run();
    ExpectStr(g.szProfileId, "widget-life", "widget profile");
    ExpectSem(0x42, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "widget 0x42");
    ExpectInt(g.nSSDLifeLeft, 4, "widget life");
    ExpectInt(g.nEndurancePercent, 4, "widget endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_BAD, "widget wear");
    ExpectInt(WearBadgeStatus(g.nEndurancePercent), (int)HEALTH_STATUS_CAUTION,
              "widget badge");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "widget headline");
    ProfileClearRegistered();
    CaseEnd("widget-life", before);
}

/* Phison: 169 wins unless its Value is still >= 90 and 231's RAW matches
 * 231's Value. The total erase counter is not the average. */
static void TestPhisonReadings(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA9, 100, 40), "phison dummy A9");
    Expect(AddAttr(0xE7, 78, 78), "phison agreeing E7");
    Run();
    Expect((g.nProfileFlags & PROF_F_PREFER_AGREEING) != 0, "phison agreeing flag");
    ExpectInt(g.nSSDLifeLeft, 78, "phison life from agreeing E7");
    ExpectInt(g.nEndurancePercent, 78, "phison endurance 78");
    CaseEnd("phison-agree", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA9, 47, 26), "phison A9 26");
    Expect(AddAttr(0xE7, 47, 53), "phison E7 disagrees");
    Run();
    ExpectInt(g.nSSDLifeLeft, 26, "phison life stays A9");
    ExpectInt(g.nEndurancePercent, 26, "phison endurance 26");
    CaseEnd("phison-a9", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA9, 100, 0), "phison A9 dummy zero");
    Expect(AddAttr(0xE7, 78, 78), "phison E7 only figure");
    Run();
    ExpectInt(g.nSSDLifeLeft, 78, "phison dummy A9 yields to E7");
    CaseEnd("phison-dummy", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xE7, 100, 0), "phison E7 dummy zero");
    Run();
    ExpectInt(g.nSSDLifeLeft, -1, "phison dummy percent is not 0% life");
    CaseEnd("phison-e7-dummy", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37240G", "SBFKB1H5");
    Expect(AddAttr(0xA4, 100, 80000), "phison total first");
    Expect(AddAttr(0xA7, 100, 5), "phison average");
    Expect(AddAttr(0xA5, 100, 9), "phison max");
    Expect(AddAttr(0xA6, 100, 1), "phison min");
    Expect(AddAttr(0xB1, 100, 12), "phison wear count");
    Expect(AddAttr(0xA9, 90, 90), "phison life 90");
    Run();
    ExpectInt(g.nSSDAvgEraseCount, 5, "phison average not the total");
    ExpectInt(g.nSSDMaxEraseCount, 9, "phison max erase");
    ExpectInt(g.nSSDMinEraseCount, 1, "phison min erase");
    ExpectInt(g.nSSDWearLevelingCount, 12, "phison wear-leveling count");
    ExpectInt(g.nSSDLifeLeft, 90, "phison life 90");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "phison wear axis");
    CaseEnd("phison-erase", before);
}

/* 231 Value wins over 177. 177 alone is the fallback. RAW of 231 is not
 * the percent. 245/246/247 are the erase roles. 177 is not a wear count. */
static void TestSamsungLife(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Expect(AddAttr(0xB1, 40, 12), "870 slot B1");
    Expect(AddAttr(0xF5, 100, 3), "870 slot F5");
    Expect(AddAttr(0xF6, 100, 11), "870 slot F6");
    Expect(AddAttr(0xF7, 100, 7), "870 slot F7");
    Expect(AddAttr(0xE7, 98, 26), "870 slot E7 again");
    Run();
    ExpectInt(g.nSSDLifeLeft, 98, "870 E7 beats B1");
    ExpectInt(g.nEndurancePercent, 98, "870 endurance still 98");
    ExpectInt(g.nSSDMinEraseCount, 3, "870 min erase");
    ExpectInt(g.nSSDMaxEraseCount, 11, "870 max erase");
    ExpectInt(g.nSSDAvgEraseCount, 7, "870 average erase");
    ExpectInt(g.nSSDWearLevelingCount, -1, "870 B1 is not a wear count");
    CaseEnd("870-pref", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    Expect(AddAttr(0xB1, 40, 99), "870 B1 alone");
    Run();
    ExpectSem(0xB1, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "870 B1 value");
    ExpectInt(g.nSSDLifeLeft, 40, "870 B1 fallback");
    ExpectInt(g.nEndurancePercent, 40, "870 B1 endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "870 B1 wear");
    ExpectInt(g.nSSDAvgEraseCount, -1, "870 B1 raw is not erase");
    CaseEnd("870-b1", before);
}

/* Maxio 231 Value is life. RAW 6 is percent used. 173 has no role. */
static void TestMaxioLife(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "P3-512", "SN123456");
    Expect(AddAttr(0xE7, 94, 6), "maxio slot E7");
    Expect(AddAttr(0xAD, 100, 173), "maxio slot AD");
    Run();
    ExpectStr(g.szProfileId, "p3-maxio-sn", "maxio profile");
    ExpectSem(0xE7, PROF_SEM_ENDURANCE, PROF_ENC_VALUE, "maxio E7");
    ExpectInt(g.nSSDLifeLeft, 94, "maxio life from value");
    ExpectInt(g.nEndurancePercent, 94, "maxio endurance");
    ExpectInt(g.nSSDAvgEraseCount, -1, "maxio 173 not erase");
    ExpectInt(g.nSSDWearLevelingCount, -1, "maxio 173 not wear");
    CaseEnd("maxio", before);
}

/* Apacer 231 stays on the RAW percent when Value is still 100. */
static void TestAs350Raw(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Apacer AS350 512GB", "VE1R9008");
    Expect(AddAttr(231, 100, 99), "as350 raw slot");
    Run();
    ExpectSem(231, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "as350 raw encoding");
    ExpectInt(g.nSSDLifeLeft, 99, "as350 life from raw");
    ExpectInt(g.nEndurancePercent, 99, "as350 raw endurance");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_GOOD, "as350 raw wear");
    CaseEnd("as350-raw", before);
}

static DWORD WINAPI OnceThread(LPVOID p)
{
    OnceJob* job = (OnceJob*)p;
    memset(job->info, 0, sizeof(*job->info));
    job->info->eType = job->type;
    lstrcpynA(job->info->szModel, job->model, (int)sizeof(job->info->szModel));
    lstrcpynA(job->info->szFirmware, job->fw, (int)sizeof(job->info->szFirmware));
    DriveDbApply(job->info);
    return 0;
}

static void TestCompileOnce(void)
{
    OnceJob a, b;
    HANDLE th[2];
    DWORD wr;
    a.info = &gOnceA;
    a.type = DRIVE_TYPE_SSD_SATA;
    a.model = "Samsung SSD 870 EVO 1TB";
    a.fw = "SVT02B6Q";
    b.info = &gOnceB;
    b.type = DRIVE_TYPE_HDD;
    b.model = "ST1000DM003-1CH162";
    b.fw = "CC47";
    th[0] = CreateThread(NULL, 0, OnceThread, &a, 0, NULL);
    th[1] = CreateThread(NULL, 0, OnceThread, &b, 0, NULL);
    Expect(th[0] != NULL && th[1] != NULL, "once threads");
    if (th[0] && th[1]) {
        wr = WaitForMultipleObjects(2, th, TRUE, 60000);
        Expect((int)wr == (int)WAIT_OBJECT_0, "once wait");
    }
    if (th[0]) CloseHandle(th[0]);
    if (th[1]) CloseHandle(th[1]);
    ExpectStr(gOnceA.szModelFamily, "Samsung based SSDs", "once samsung");
    ExpectStr(gOnceB.szModelFamily, "Seagate Barracuda 7200.14 (AF)", "once barra");
}

int main(void)
{
    printf("drivedb_test\n");
    fflush(stdout);
    TestCompileOnce();
    TestSu800();
    TestAs350();
    TestA400Phison();
    TestA400CrcSlot();
    TestA400Smi();
    TestSpcc();
    TestSamsung870();
    TestSamsungLife();
    TestAs350Raw();
    TestPhisonReadings();
    TestMaxioLife();
    TestMicronCa();
    TestIntelIndicators();
    TestWidgetLife();
    TestBarracuda();
    TestHddConfidence();
    TestUsbSkip();
    TestDriveDbTie();
    TestDriveDbTieUnnamed();
    TestRowRank();
    TestSmartModular();
    TestPackedName();
    TestPackedCell();
    TestLongPresetName();
    if (g_fail) {
        fprintf(stderr, "FAILED %d of %d checks\n", g_fail, g_check);
        return 1;
    }
    printf("ok %d checks\n", g_check);
    return 0;
}
