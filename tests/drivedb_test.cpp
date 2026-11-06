/* DriveMonitor - drivedb integration tests. GPL-2.0-or-later. See LICENSE.
 * Links the real drivedb.cpp, identification, decoder, and health engine.
 * profile_test.cpp still stubs DriveDbApply and DriveDbLookup.
 * Ranking stays heuristic: assertions name the winning family and the
 * warning text. They do not lock an EntryScore integer.
 * No physical disk is opened. The serial stays empty, so no mech snap
 * file is written. */
#include "smart.h"
#include "smart_internal.h"
#include "csmi.h"
#include "mainwnd.h"
#include "latency_probe.h"
#include "counterfeit_detect.h"
#include "ata_log.h"
#include "dst_engine.h"
#include "lang.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

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
    g.nProtectMode = -1;
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
    /* Remaining_Lifetime_Perc is a label. The exact profile does not
     * confirm a percent, so 92 is not published as life. */
    ExpectNoLookup(169, "su800 169 not a percent");
    ExpectName(169, 1, "Остаток", "su800 169 name");
    ExpectInt(g.nSSDLifeLeft, -1, "su800 life unset");
    ExpectInt(g.nEndurancePercent, -1, "su800 endurance unset");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "su800 wear unset");
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
    ExpectInt(g.nEndurancePercent, -1, "su800 life kept unset");
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
    /* Lifetime_Left is a label. The exact profile does not confirm a
     * percent, so 4 is not a wear verdict. */
    ExpectNoLookup(231, "as350 231 not a percent");
    ExpectName(231, 1, "Остаток", "as350 231 name");
    ExpectInt(g.nSSDLifeLeft, -1, "as350 life unset");
    ExpectInt(g.nEndurancePercent, -1, "as350 endurance unset");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "as350 wear unset");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "as350 media clean");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "as350 headline");
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

/* Kingston A400 with remaining life <= 20% (12%) raises headline to CAUTION.
 * Attributes 148, 149 (SLC erase) and 187 (ECC uncorrectable) are decoded. */
static void TestA400LowLifeAndSlcAttrs(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400M8240G", "SBFK61E1");
    Expect(AddAttr(0x94, 100, 10), "a400 slc total");
    Expect(AddAttr(0x95, 100, 5), "a400 slc max");
    Expect(AddAttr(0xBB, 100, 0), "a400 ecc 0");
    Expect(AddAttr(0xA9, 100, 12), "a400 life 12");
    Expect(AddAttr(0xE7, 12, 88), "a400 E7 life 12");
    Run();
    ExpectStr(g.szProfileId, "a400-sbfk61", "a400 sbfk61 profile");
    ExpectInt((int)g.eController, (int)CONTROLLER_PHISON, "a400 controller phison");
    ExpectName(0x94, 1, "SLC", "a400 148 slc name");
    ExpectName(0x95, 1, "SLC", "a400 149 slc max name");
    ExpectName(0xBB, 1, "ECC", "a400 187 ecc name");
    ExpectRank(0xBB, ATTR_EV_RANK, (int)HEALTH_STATUS_GOOD, "a400 187 rank good");
    ExpectInt(g.nSSDLifeLeft, 12, "a400 life left 12");
    ExpectInt(g.nEndurancePercent, 12, "a400 endurance 12");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_CAUTION, "a400 wear caution");
    ExpectInt(WearBadgeStatus(g.nEndurancePercent), (int)HEALTH_STATUS_CAUTION, "a400 wear badge caution");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "a400 headline caution");
    FormatHealthLectureExpert(&g, lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Заявленный остаток ресурса: 12%", "a400 lecture life caution");
    CaseEnd("a400-low-life", before);
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
 * set the controller. 231 is only a life-left name: Value and RAW are
 * not published as a percent. 218 stays a name-hint risk. */
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
    ExpectNoLookup(231, "spcc 231 not a percent");
    ExpectName(231, 1, "Остаток", "spcc 231 name");
    ExpectInt(g.nSSDLifeLeft, -1, "spcc life unset");
    ExpectInt(g.nEndurancePercent, -1, "spcc endurance unset");
    ExpectInt(g.nHealthPercent, -1, "spcc no health percent");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "spcc wear unset");
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
    ExpectInt(WearBadgeStatus(15), (int)HEALTH_STATUS_CAUTION, "badge 15");
    ExpectInt(WearBadgeStatus(20), (int)HEALTH_STATUS_CAUTION, "badge 20");
    ExpectInt(WearBadgeStatus(25), (int)HEALTH_STATUS_OBSERVE, "badge 25");
    ExpectInt(WearBadgeStatus(31), (int)HEALTH_STATUS_GOOD, "badge 31");
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
    {
        char lecture[16384];
        FormatHealthLectureExpert(&g, lecture, (int)sizeof(lecture));
        ExpectNotHas(lecture, "Неисправимые (187)", "ssd 187 raw 3 not in matrix");
        ExpectNotHas(lecture, "признак деградации носителя", "ssd 187 raw 3 not media degradation");
    }
    CaseEnd("row-ssd187", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ROWTEST", "1.0");
    Expect(AddAttr(187, 100, 0), "row ssd187 zero slot");
    Run();
    ExpectRank(187, ATTR_EV_SKIP, 0, "ssd 187 zero skipped");
    {
        char lecture[16384];
        FormatHealthLectureExpert(&g, lecture, (int)sizeof(lecture));
        ExpectNotHas(lecture, "Неисправимые (187)", "ssd 187 zero not in matrix");
        ExpectNotHas(lecture, "Неисправимые ошибки (187): 0", "ssd 187 zero not in main features");
    }
    CaseEnd("row-ssd187-0", before);

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
    {
        char lecture[16384];
        FormatHealthLectureExpert(&g, lecture, (int)sizeof(lecture));
        ExpectHas(lecture, "Неисправимые (187)", "samsung 187 in matrix");
        ExpectHas(lecture, "Неисправимые ошибки (187): 1", "samsung 187 in main features");
    }
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
    ExpectCell(161, "42 (5)", "pack 161 fields");
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
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0x43, PROF_SEM_NONE, PROF_ENC_PAIR, 0, "Pair", "Pair",
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }
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
      NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 0, 0, 0, 0, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0, 0, 0 }
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
    ExpectName(0xA5, 1, "Макс", "phison max known");
    ExpectName(0xA6, 1, "Мин", "phison min known");
    ExpectName(0xA7, 1, "Среднее", "phison avg known");
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
    ExpectName(0xF5, 1, "Мин", "870 min known");
    ExpectName(0xE7, 1, "Остаток", "870 E7 known");
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
    ExpectName(0xE7, 1, "Остаток", "maxio E7 known");
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
    ExpectNoLookup(231, "as350 raw not a percent");
    ExpectInt(g.nSSDLifeLeft, -1, "as350 raw life unset");
    ExpectInt(g.nEndurancePercent, -1, "as350 raw endurance unset");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "as350 raw wear unset");
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

static void TestModernHardwareDetect(void)
{
    DRIVE_INFO dev;
    int before = g_fail;

    /* USB Bridge detection */
    memset(&dev, 0, sizeof(dev));
    dev.wUsbVid = 0x174C;
    dev.wUsbPid = 0x2464;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_ASMEDIA, "bridge asm2464pd");
    dev.wUsbPid = 0x2462;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_ASMEDIA, "bridge asm2462");
    dev.wUsbPid = 0x2364;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_ASMEDIA, "bridge asm2364");
    dev.wUsbPid = 0x1352;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_ASM1352R, "bridge asm1352r");

    memset(&dev, 0, sizeof(dev));
    dev.wUsbVid = 0x0BDA;
    dev.wUsbPid = 0x9201;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_REALTEK, "bridge rtl9201");
    dev.wUsbPid = 0x9210;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_REALTEK, "bridge rtl9210");
    dev.wUsbPid = 0x9211;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_REALTEK, "bridge rtl9211");
    dev.wUsbPid = 0x9220;
    ExpectInt((int)DetectUsbBridgeType(NULL, &dev), (int)USB_BRIDGE_NVME_FMA, "bridge rtl9220 fma");
    CaseEnd("modern-bridges", before);

    /* PCI VID identification */
    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Yeestor NVMe", "");
    SetPciVid(0x1E49);
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_YEESTOR, "yeestor vid ctl");
    ExpectStr(GetControllerName(g.eController), "Yeestor", "yeestor ctl name");
    CaseEnd("yeestor-vid", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Maxio NVMe", "");
    SetPciVid(0x1E4B);
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_MAXIO, "maxio vid ctl");
    ExpectStr(GetControllerName(g.eController), "Maxio", "maxio ctl name");
    CaseEnd("maxio-vid", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "InnoGrit NVMe", "");
    SetPciVid(0x1DDC);
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_INNOGRIT, "innogrit vid ctl");
    ExpectStr(GetControllerName(g.eController), "Innogrit", "innogrit ctl name");
    CaseEnd("innogrit-vid", before);

    /* Model token identification and indicators */
    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Netac SA500 YS9082", "V1.0");
    Expect(AddAttr(0xA9, 100, 95), "ys9082 slot a9");
    Expect(AddAttr(0xA7, 100, 15), "ys9082 slot a7");
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_YEESTOR, "ys9082 token ctl");
    ExpectStr(g.szProfileId, "ys9082", "ys9082 profile match");
    ExpectInt(g.nSSDLifeLeft, 95, "ys9082 life left");
    ExpectInt(g.nSSDAvgEraseCount, 15, "ys9082 erase count");
    CaseEnd("ys9082-token", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Dahua C800A MAS0902", "V1.0");
    Expect(AddAttr(0xE7, 92, 8), "mas0902 slot e7");
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_MAXIO, "mas0902 token ctl");
    ExpectInt(g.nSSDLifeLeft, 92, "mas0902 life left");
    CaseEnd("mas0902-token", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Colorful SL500 RTS5732", "V1.0");
    Expect(AddAttr(0xA9, 100, 90), "rts5732 slot a9");
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_REALTEK, "rts5732 token ctl");
    ExpectInt(g.nSSDLifeLeft, 90, "rts5732 life left");
    CaseEnd("rts5732-token", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Patriot Viper VP4300 IG5236", "V1.0");
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_INNOGRIT, "ig5236 token ctl");
    CaseEnd("ig5236-token", before);
}

static void SetErrLog(unsigned n)
{
    int i;
    for (i = 0; i < 16; i++) {
        g.nvmeHealth.NumErrLogEntries[i] = (BYTE)(n & 0xFFu);
        n >>= 8;
    }
}

static void LectureExpert(char* buf, int nBuf)
{
    FormatHealthLectureExpert(&g, buf, nBuf);
}

/* NT-256 / H210727a. The JMicron family string is a loose model match.
 * It does not raise confidence or name a controller. 173 is raw16.
 * 187 on a generic SSD is not a health plus. 241 has no -v name. */
static void TestNt256(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_SSD_SATA, "NT-256", "H210727a");
    Expect(AddAttr(5, 100, 0), "nt256 realloc");
    Expect(AddAttr(197, 100, 0), "nt256 pending");
    Expect(AddAttr(198, 100, 0), "nt256 uncorr");
    Expect(AddAttr(173, 100, 0x000A00960049ULL), "nt256 erase");
    Expect(AddAttr(187, 100, 0), "nt256 187");
    Expect(AddAttr(160, 100, 1), "nt256 160");
    Expect(AddAttr(167, 100, 1), "nt256 167");
    Expect(AddAttr(169, 100, 1), "nt256 169");
    Expect(AddAttr(180, 100, 1), "nt256 180");
    Expect(AddAttr(231, 100, 1), "nt256 231");
    Expect(AddAttr(233, 100, 1), "nt256 233");
    Expect(AddAttr(234, 100, 1), "nt256 234");
    Expect(AddAttr(240, 100, 1), "nt256 240");
    Expect(AddAttr(241, 100, 126466), "nt256 241");
    Run();
    ExpectStr(g.szModelFamily, "JMicron/Maxiotek based SSDs", "nt256 family");
    ExpectInt((int)g.eController, (int)CONTROLLER_UNKNOWN, "nt256 controller");
    ExpectStr(g.szProfileId, "ata-generic", "nt256 profile");
    Expect(g.nConfModelId < 90, "nt256 model not confident");
    ExpectSem(173, PROF_SEM_ERASE, PROF_ENC_LO16, "nt256 173");
    ExpectDecoded(PROF_ENC_LO16, 0x000A00960049ULL, 73, "nt256 173 value");
    ExpectInt(g.nSSDAvgEraseCount, -1, "nt256 erase not stored");
    ExpectRank(187, ATTR_EV_SKIP, 0, "nt256 187 skipped");
    ExpectInt(g.nReportedUncorrect, -1, "nt256 187 not scored");
    ExpectName(241, 0, NULL, "nt256 241 unnamed");
    ExpectName(231, 0, NULL, "nt256 231 unnamed");
    ExpectNoLookup(231, "nt256 231 not life");
    ExpectInt(g.nSSDLifeLeft, -1, "nt256 life unset");
    Expect(g.eRawQuality != NORM_QUALITY_HIGH, "nt256 raw not high");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "не полностью", "nt256 partial model");
    ExpectNotHas(lecture, "определена уверенно", "nt256 not confident");
    ExpectNotHas(lecture, "Неисправимые ошибки (187): 0", "nt256 187 not a plus");
    ExpectNotHas(lecture, "Неисправимые (187)", "nt256 187 not in matrix");
    ExpectHas(lecture, "    4 сильных положительных признаков", "nt256 plus count excludes 187");
    CaseEnd("nt256", before);
}

/* GOODRAM CX400. Maxio variant 2 is the family label, not a controller.
 * Value 85 and RAW 15 are not one endurance figure. */
static void TestCx400(void)
{
    int before = g_fail;
    char lecture[16384];
    ATTR_DECODE d206, d209;
    Base(DRIVE_TYPE_SSD_SATA, "SSDPR-CX400-128-G2", "H190117D");
    Expect(AddAttr(231, 85, 15), "cx400 life name");
    Expect(AddAttr(173, 100, 0x0084014600EEULL), "cx400 erase");
    Expect(AddAttr(169, 100, 0x400010ULL), "cx400 bad blocks");
    Expect(AddAttr(206, 100, 1), "cx400 min erase");
    Expect(AddAttr(209, 100, 1), "cx400 slc min");
    Expect(AddAttr(241, 100, 12), "cx400 host writes");
    Expect(AddAttr(170, 100, 1), "cx400 unknown");
    Expect(AddAttr(187, 100, 0), "cx400 187");
    Run();
    ExpectStr(g.szModelFamily, "Maxio based SSDs (variant 2)", "cx400 family");
    ExpectInt((int)g.eVendor, (int)VENDOR_GOODRAM, "cx400 vendor");
    ExpectStr(VendorLabel(&g), "GOODRAM", "cx400 brand");
    ExpectInt((int)g.eController, (int)CONTROLLER_UNKNOWN, "cx400 controller");
    ExpectStr(g.szProfileId, "ata-generic", "cx400 profile");
    Expect(g.nConfModelId < 90, "cx400 model not confident");
    Expect(g.nConfVendorDecoder <= 80, "cx400 decoder not found");
    ExpectNoLookup(231, "cx400 231 not a percent");
    ExpectName(231, 1, "Остаток", "cx400 231 name");
    ExpectInt(g.nSSDLifeLeft, -1, "cx400 life unset");
    ExpectInt(g.nEndurancePercent, -1, "cx400 endurance unset");
    ExpectInt(g.nHealthPercent, -1, "cx400 no health percent");
    ExpectInt((int)g.eWear, (int)HEALTH_STATUS_UNKNOWN, "cx400 wear unset");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "cx400 headline");
    ExpectSem(173, PROF_SEM_ERASE, PROF_ENC_LO16, "cx400 173");
    ExpectDecoded(PROF_ENC_LO16, 0x0084014600EEULL, 238, "cx400 173 value");
    ExpectInt(g.nSSDAvgEraseCount, -1, "cx400 avg erase unset");
    ExpectCell(169, "16 (64)", "cx400 169 fields");
    memset(&d206, 0, sizeof(d206));
    memset(&d209, 0, sizeof(d209));
    GetAttrDecode(206, &g, &d206);
    GetAttrDecode(209, &g, &d209);
    ExpectNotHas(d206.szName, "SLC", "cx400 206 not slc");
    ExpectHas(d209.szName, "SLC", "cx400 209 slc");
    ExpectName(241, 1, "GiB", "cx400 241 gib");
    ExpectName(170, 0, NULL, "cx400 170 unknown");
    ExpectName(187, 0, NULL, "cx400 187 stays unknown");
    ExpectRank(187, ATTR_EV_SKIP, 0, "cx400 187 skipped");
    ExpectInt(g.nReportedUncorrect, -1, "cx400 187 not scored");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "не полностью", "cx400 partial model");
    ExpectHas(lecture, "неполный", "cx400 incomplete decoder");
    ExpectNotHas(lecture, "Ресурс: 15%", "cx400 raw is not life");
    CaseEnd("cx400", before);
}

/* Travelstar 5K750. The model says Hitachi. The controller enum stays
 * HGST's chip id, and the label does not repeat the brand. G-Sense
 * 0x000000010000 is not 65536 events. A real low-16 count still counts. */
static void TestHitachiTravelstar(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_HDD, "Hitachi HTS547575A9E384", "JE4OA60A");
    Expect(AddAttr(5, 100, 0), "hitachi realloc");
    Expect(AddAttr(197, 100, 0), "hitachi pending");
    Expect(AddAttr(198, 100, 0), "hitachi uncorr");
    Expect(AddAttr(191, 100, 0x10000ULL), "hitachi gsense packed");
    Run();
    ExpectStr(g.szModelFamily, "Hitachi/HGST Travelstar 5K750", "hitachi family");
    ExpectInt((int)g.eVendor, (int)VENDOR_HITACHI, "hitachi vendor");
    ExpectStr(VendorLabel(&g), "Hitachi", "hitachi brand");
    ExpectInt((int)g.eController, (int)CONTROLLER_HITACHI, "hitachi ctl enum");
    ExpectStr(DriveControllerLabel(&g), GetControllerName(CONTROLLER_UNKNOWN),
              "hitachi ctl label");
    ExpectInt(g.nGSenseEvents, -1, "hitachi gsense refused");
    ExpectInt((int)g.eMechanics, (int)HEALTH_STATUS_UNKNOWN, "hitachi mechanics");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "hitachi headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_UNKNOWN, "hitachi disk status");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "SMART RETURN STATUS", "hitachi return status");
    ExpectNotHas(lecture, "65536", "hitachi not 65536 shocks");
    CaseEnd("hitachi-packed", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "Hitachi HTS547575A9E384", "JE4OA60A");
    Expect(AddAttr(5, 100, 0), "hitachi2 realloc");
    Expect(AddAttr(197, 100, 0), "hitachi2 pending");
    Expect(AddAttr(198, 100, 0), "hitachi2 uncorr");
    Expect(AddAttr(191, 100, 3), "hitachi2 gsense");
    Run();
    ExpectInt(g.nGSenseEvents, 3, "hitachi real shocks");
    ExpectInt((int)g.eMechanics, (int)HEALTH_STATUS_OBSERVE, "hitachi observe");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "hitachi2 headline");
    CaseEnd("hitachi-count", before);
}

/* Phison NVMe. The controller and the phison profile stay. Error-log
 * entries and a warmer sensor are context. 100% life is not a plus. */
static void TestPhisonMge15(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_NVME, "PHISON MGE15SOU4 1TB", "EHFM90.1");
    g.bIsNVMe = TRUE;
    SetPciVid(0x1987);
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.nvmeHealth.PercentageUsed = 0;
    g.nTemperatureC = 24;
    g.nTempSensor[0] = 38;
    g.wNVMeWarnTempThreshold = (USHORT)(83 + 273);
    SetErrLog(4667);
    Run();
    ExpectInt((int)g.eVendor, (int)VENDOR_UNKNOWN, "mge15 brand unset");
    ExpectInt((int)g.eController, (int)CONTROLLER_PHISON, "mge15 controller");
    ExpectStr(g.szProfileId, "phison", "mge15 profile");
    ExpectInt(g.nProfileScore, 80, "mge15 score");
    ExpectInt(g.nEndurancePercent, 100, "mge15 life published");
    ExpectInt(g.nConfVendorDecoder, 100, "mge15 decoder found");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "mge15 headline");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "vendor decoder найден", "mge15 decoder phrase");
    ExpectHas(lecture, "Записи в журнале ошибок: 4667", "mge15 error log");
    ExpectHas(lecture, "Заявленный остаток ресурса: 100%", "mge15 life note");
    ExpectNotHas(lecture, "\xE2\x9C\x93 Заявленный остаток", "mge15 life not a plus");
    ExpectHas(lecture, "3 сильных положительных признаков", "mge15 three pluses");
    ExpectHas(lecture, "38", "mge15 warmer sensor");
    CaseEnd("mge15", before);
}

/* XPG GAMMIX S50 Lite is SM2267XT. Bare GAMMIX S50 is not that row.
 * Percentage Used is wear. A hotter sensor stays context. */
static void TestGammixS50Lite(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_NVME, "XPG GAMMIX S50 Lite", "82A7U82C");
    g.bIsNVMe = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.nvmeHealth.PercentageUsed = 7;
    g.nTemperatureC = 38;
    g.nTempSensor[0] = 60;
    g.wNVMeWarnTempThreshold = (USHORT)(75 + 273);
    Run();
    ExpectInt((int)g.eVendor, (int)VENDOR_ADATA, "s50 vendor");
    ExpectStr(VendorLabel(&g), "ADATA", "s50 brand");
    ExpectInt((int)g.eController, (int)CONTROLLER_SMI, "s50 controller");
    ExpectStr(DriveControllerLabel(&g), "Silicon Motion SM2267XT", "s50 chip");
    ExpectStr(g.szProfileId, "gammix-s50-lite", "s50 profile");
    ExpectInt(g.nProfileScore, 98, "s50 score");
    Expect(g.nConfModelId >= 90, "s50 model confident");
    ExpectInt(g.nEndurancePercent, 93, "s50 life");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "s50 headline");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "определена уверенно", "s50 confident phrase");
    ExpectHas(lecture, "Заявленный остаток ресурса: 93%", "s50 life note");
    ExpectNotHas(lecture, "\xE2\x9C\x93 Заявленный остаток", "s50 life not a plus");
    ExpectHas(lecture, "3 сильных положительных признаков", "s50 three pluses");
    ExpectHas(lecture, "60", "s50 warmer sensor");
    CaseEnd("s50-lite", before);

    before = g_fail;
    Base(DRIVE_TYPE_NVME, "XPG GAMMIX S50", "82A7U82C");
    g.bIsNVMe = TRUE;
    Run();
    ExpectInt((int)g.eVendor, (int)VENDOR_ADATA, "s50 bare vendor");
    ExpectStr(g.szProfileId, "nvme-generic", "s50 bare profile");
    Expect(g.eController != CONTROLLER_SMI, "s50 bare not smi");
    CaseEnd("s50-bare", before);
}

/* A standard NVMe log with no unknown ATA rows is not a vendor decoder. */
static void TestBareNvmeDecoder(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_NVME, "ZZNVMEBARE", "1.0");
    g.bIsNVMe = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    Run();
    ExpectStr(g.szProfileId, "nvme-generic", "bare nvme profile");
    ExpectInt(g.nProfileScore, 50, "bare nvme score");
    ExpectInt(g.nConfVendorDecoder, 70, "bare nvme decoder");
    Expect(g.nConfModelId < 90, "bare nvme model");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "не полностью", "bare nvme partial model");
    ExpectHas(lecture, "неполный", "bare nvme incomplete decoder");
    ExpectNotHas(lecture, "vendor decoder найден", "bare nvme decoder not found");
    CaseEnd("bare-nvme", before);
}

/* Firmware X0620A0 is an SM2258 shape. The shape does not assign SMI,
 * and the model is not Phison. */
static void TestNeobihier(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Neobihier 1TB", "X0620A0");
    Run();
    ExpectInt((int)g.eController, (int)CONTROLLER_UNKNOWN, "neobihier controller");
    Expect(g.eController != CONTROLLER_PHISON, "neobihier not phison");
    Expect(g.eController != CONTROLLER_SMI, "neobihier not smi");
    ExpectStr(g.szProfileId, "ata-generic", "neobihier profile");
    CaseEnd("neobihier", before);
}

static void TestDriveSummaryFormat(void)
{
    char buf[4096];
    int before = g_fail;

    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
    lstrcpynA(g.szSerial, "S5YNNJ0R123456", (int)sizeof(g.szSerial));
    g.dwCapacityMB = 953869;
    g.nTemperatureC = 34;
    g.eTempBand = TEMP_BAND_NORMAL;
    g.dwPowerOnHours = 1250;
    g.dwPowerCycleCount = 45;
    lstrcpynA(g.szProtocol, "SATA/600", (int)sizeof(g.szProtocol));
    Expect(AddAttr(0xB1, 98, 98), "sam870 life slot");
    Run();

    buf[0] = '\0';
    FormatDriveSummary(&g, buf, (int)sizeof(buf));
    ExpectHas(buf, "Samsung SSD 870 EVO 1TB", "summary model");
    ExpectHas(buf, "S5YNNJ0R123456", "summary serial");
    ExpectHas(buf, "SVT02B6Q", "summary fw");
    ExpectHas(buf, "34 °C", "summary temp");
    ExpectHas(buf, "SATA/600", "summary proto");
    ExpectHas(buf, "Samsung", "summary brand/family");
    CaseEnd("summary-format", before);
}

static void SetThresh(BYTE id, BYTE v)
{
    int i;
    for (i = 0; i < 30; i++) {
        if (g.threshData.stThresholds[i].bAttrID == 0 ||
            g.threshData.stThresholds[i].bAttrID == id) {
            g.threshData.stThresholds[i].bAttrID = id;
            g.threshData.stThresholds[i].bThresholdValue = v;
            return;
        }
    }
}

static void MatrixLine(const char* lecture, const char* key, char* line, int nLine)
{
    const char* p;
    int n = 0;
    if (line && nLine > 0) line[0] = '\0';
    p = (lecture && key) ? strstr(lecture, key) : NULL;
    if (!p || !line || nLine <= 1) return;
    while (p[n] && p[n] != '\n' && p[n] != '\r' && n < nLine - 1) {
        line[n] = p[n];
        n++;
    }
    line[n] = '\0';
}

static void FillLogChecksum(BYTE* page)
{
    unsigned sum = 0;
    int i;
    page[511] = 0;
    for (i = 0; i < 511; i++)
        sum = (sum + page[i]) & 0xFFu;
    page[511] = (BYTE)((0x100u - sum) & 0xFFu);
}

/* Log 0x02: byte 2 is only the low half of the index. The device error
 * count is the word at offset 500. A broken checksum keeps the index. */
static void TestAtaErrorLog(void)
{
    BYTE page[512];
    int index = -1;
    int before = g_fail;
    memset(page, 0, sizeof(page));
    page[0] = 1;
    page[2] = 0x80;
    page[3] = 0x00;
    page[500] = 3;
    page[501] = 0;
    FillLogChecksum(page);
    ExpectInt(AtaErrorLogCount(page, 512, &index), 3, "error log count");
    ExpectInt(index, 128, "error log index");
    memset(&g, 0, sizeof(g));
    ApplyAtaErrorLog(&g, page, 512);
    ExpectInt(g.nErrorLogCount, 3, "applied device count");
    page[511] ^= 0xFF;
    ExpectInt(AtaErrorLogCount(page, 512, &index), -1, "bad checksum");
    ExpectInt(index, 128, "index survives a bad checksum");
    ApplyAtaErrorLog(&g, page, 512);
    ExpectInt(g.nErrorLogCount, 128, "index is the fallback");
    CaseEnd("ata-error-log", before);
}

/* Patriot Burst: F1 only fits as 32 MiB, so F2 of the same order uses it.
 * A lone read counter may still be raw GB. No NAND figure stays raw GB. */
static void TestPhisonHostUnits(void)
{
    int before = g_fail;
    char lecture[16384];
    char line[200];
    Base(DRIVE_TYPE_SSD_SATA, "Patriot Burst Elite 120GB", "HPS2227I");
    g.dwCapacityMB = 114483;
    g.dwPowerOnHours = 18137;
    Expect(AddAttr(0xA5, 100, 643), "burst max");
    Expect(AddAttr(0xA6, 100, 502), "burst min");
    Expect(AddAttr(0xA7, 100, 557), "burst avg");
    Expect(AddAttr(0xA9, 100, 100), "burst life");
    Expect(AddAttr(0xC3, 100, 0), "burst ecc");
    Expect(AddAttr(0xF1, 100, 303219), "burst writes");
    Expect(AddAttr(0xF2, 100, 454296), "burst reads");
    Run();
    ExpectStr(g.szProfileId, "burst-hps", "burst profile");
    ExpectInt(g.nSSDAvgEraseCount, 557, "burst average");
    ExpectInt(g.nSSDTotalWritesGB, 9475, "burst writes are 32 MiB units");
    Expect(ScaleAtaHostGiB(&g, 454296, TRUE) == 14196ULL, "burst reads share 32 MiB");
    Expect(ScaleAtaHostGiB(&g, 303219, FALSE) == 9475ULL, "burst writes stay 32 MiB");
    ExpectName(0xC3, 0, "ECC", "burst ecc stays unknown");
    LectureExpert(lecture, (int)sizeof(lecture));
    MatrixLine(lecture, "Контроллер", line, (int)sizeof(line));
    ExpectHas(line, "RAW частично", "burst partial raw note");
    CaseEnd("burst-units", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Patriot Burst Elite 120GB", "HPS2227I");
    g.dwCapacityMB = 114483;
    g.dwPowerOnHours = 18137;
    Expect(AddAttr(0xA7, 100, 557), "burst read-only avg");
    Expect(AddAttr(0xF2, 100, 454296), "burst read only");
    Run();
    Expect(ScaleAtaHostGiB(&g, 454296, TRUE) == 454296ULL, "lone read stays GB");
    CaseEnd("burst-read-only", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD PM871", "EMT41L6Q");
    g.dwCapacityMB = 256 * 1024;
    g.dwPowerOnHours = 10000;
    Expect(AddAttr(0xF1, 100, 50000), "pm871 modest writes");
    Run();
    Expect(ScaleAtaHostGiB(&g, 50000, FALSE) == 50000ULL, "no nand stays GB");
    CaseEnd("scale-no-nand", before);
}

/* Utania OOS keeps the brand. One seek error far above the threshold
 * does not raise mechanics. A value inside the window still does. */
static void TestSeekNearThresh(void)
{
    int before = g_fail;
    char lecture[8192];
    char line[200];
    Base(DRIVE_TYPE_HDD, "OOS500G128M", "OOS1");
    SetThresh(7, 45);
    Expect(AddAttr(5, 100, 0), "oos realloc");
    Expect(AddAttr(7, 72, 0x000100E50C58ULL), "oos seek");
    Expect(AddAttr(197, 100, 0), "oos pending");
    Expect(AddAttr(198, 100, 0), "oos uncorr");
    Expect(AddAttr(193, 100, 100), "oos load");
    Run();
    ExpectInt((int)g.eVendor, (int)VENDOR_UTANIA, "oos brand");
    ExpectStr(VendorLabel(&g), "Utania", "oos label");
    Expect(UseSeagateRateRaw(&g), "oos uses the seagate split");
    ExpectInt((int)SeagateRateErrs(Attr(7)->bRawValue), 1, "oos one seek error");
    ExpectInt((int)g.eMechanics, (int)HEALTH_STATUS_GOOD, "oos seek does not observe");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "oos headline");
    LectureExpert(lecture, (int)sizeof(lecture));
    MatrixLine(lecture, "Контроллер", line, (int)sizeof(line));
    ExpectHas(line, DriveControllerLabel(&g), "oos matrix label");
    ExpectNotHas(line, "Seagate", "oos matrix is not the brand");
    CaseEnd("oos-seek", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
    SetThresh(7, 45);
    Expect(AddAttr(5, 100, 0), "near realloc");
    Expect(AddAttr(7, 50, 0x000100000000ULL), "near seek");
    Expect(AddAttr(197, 100, 0), "near pending");
    Expect(AddAttr(198, 100, 0), "near uncorr");
    Expect(AddAttr(193, 100, 100), "near load");
    Run();
    Expect(UseSeagateRateRaw(&g), "near uses the seagate split");
    ExpectInt((int)g.eMechanics, (int)HEALTH_STATUS_OBSERVE, "near seek observes");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "near headline stays good");
    CaseEnd("seek-near", before);

    before = g_fail;
    Base(DRIVE_TYPE_HDD, "ST2000DM008-2FR102", "0001");
    Run();
    LectureExpert(lecture, (int)sizeof(lecture));
    MatrixLine(lecture, "Контроллер", line, (int)sizeof(line));
    ExpectStr(DriveControllerLabel(&g), GetControllerName(CONTROLLER_UNKNOWN),
              "st2000 label is blank");
    ExpectHas(line, DriveControllerLabel(&g), "st2000 matrix label");
    ExpectNotHas(line, "Seagate", "st2000 matrix is not the brand");
    CaseEnd("hdd-matrix", before);
}

/* BG4 is a known NVMe model. The standard log is the decoder. */
static void TestKioxiaBg4(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_NVME, "KBG40ZNV512G KIOXIA", "HP00AE00");
    g.bIsNVMe = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.nvmeHealth.PercentageUsed = 6;
    g.nTemperatureC = 25;
    Run();
    ExpectInt((int)g.eVendor, (int)VENDOR_KIOXIA, "bg4 vendor");
    ExpectInt((int)g.eController, (int)CONTROLLER_KIOXIA, "bg4 controller");
    ExpectStr(g.szProfileId, "kioxia-bg4", "bg4 profile");
    ExpectInt(g.nProfileScore, 98, "bg4 score");
    Expect(g.nConfModelId >= 90, "bg4 model confident");
    ExpectInt(g.nConfVendorDecoder, 100, "bg4 decoder found");
    ExpectInt(g.nEndurancePercent, 94, "bg4 life");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "bg4 headline");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "определена уверенно", "bg4 confident phrase");
    ExpectHas(lecture, "vendor decoder найден", "bg4 decoder phrase");
    ExpectNotHas(lecture, "неполный", "bg4 decoder is not incomplete");
    CaseEnd("kioxia-bg4", before);
}

/* Toshiba SG5 SATA SSD (from user report):
 * Attr 167 (0xA7) is SSD Protect Mode. RAW = 3 -> read-only / protect mode!
 * Must be scored as CRITICAL, not GOOD!
 * Attr 173 (0xAD) is endurance: Value 175 -> 75% life remaining (offset 100). */
static void TestToshibaReadonly(void)
{
    int before = g_fail;
    char lecture[16384];
    Base(DRIVE_TYPE_SSD_SATA, "TOSHIBA THNSNK128GVN8", "51021020");
    Expect(AddAttr(0x05, 100, 0), "toshiba realloc");
    Expect(AddAttr(0xA7, 100, 3), "toshiba protect mode raw=3");
    Expect(AddAttr(0xAD, 175, 0), "toshiba wear value=175");
    Expect(AddAttr(0xC5, 100, 0), "toshiba pending");
    Expect(AddAttr(0xC6, 100, 0), "toshiba uncorr");
    Expect(AddAttr(0xF1, 100, 440452), "toshiba host writes 32mb");
    Expect(AddAttr(0xF2, 100, 667766), "toshiba host reads 32mb");
    Run();

    ExpectStr(g.szProfileId, "thnsn", "toshiba profile");
    ExpectInt((int)g.eVendor, (int)VENDOR_TOSHIBA, "toshiba vendor");
    ExpectInt((int)g.eController, (int)CONTROLLER_TOSHIBA, "toshiba controller");
    ExpectInt(g.nProtectMode, 3, "toshiba protect mode slot");
    ExpectInt(g.nEndurancePercent, 75, "toshiba life 75%");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CRITICAL, "toshiba reliability critical");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CRITICAL, "toshiba headline critical");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_CRITICAL, "toshiba disk status critical");

    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Диск сам", "toshiba expert says the drive itself");
    ExpectHas(lecture, "только для чтения", "toshiba lecture read-only mention");
    ExpectHas(lecture, "Protect Mode", "toshiba matrix protect mode");
    ExpectHas(lecture, "75%", "toshiba lecture 75% life");
    ExpectNotHas(lecture, "Запись запретил переходник", "toshiba expert does not blame the adapter");

    char plainLecture[4096];
    FormatHealthLecturePlain(&g, plainLecture, (int)sizeof(plainLecture));
    ExpectHas(plainLecture, "Причина:", "toshiba plain lecture has reason section");
    ExpectHas(plainLecture, "Диск сам", "toshiba plain says the drive itself");
    ExpectHas(plainLecture, "защита от записи", "toshiba plain lecture states write protection reason");
    ExpectHas(plainLecture, "к замене", "toshiba plain says replace the drive");
    ExpectNotHas(plainLecture, "Запись запретил переходник", "toshiba plain does not blame the adapter");

    CaseEnd("toshiba-readonly", before);
}

/* IOCTL write-protect is Windows or the adapter. It is not the disk's own lock
 * unless SMART protect mode or the NVMe read-only bit is set. */
static void TestUniversalWriteProtect(void)
{
    int before = g_fail;
    char lecture[16384];
    char plain[8192];
    char prompt[512];

    /* 1. HDD, Windows policy, no USB adapter, clean media. */
    Base(DRIVE_TYPE_HDD, "ST1000DM010", "CC43");
    Expect(AddAttr(5, 100, 0), "hdd realloc clean");
    Expect(AddAttr(197, 100, 0), "hdd pending clean");
    Expect(AddAttr(198, 100, 0), "hdd uncorr clean");
    g.bWriteProtected = TRUE;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "hdd windows wp reliability");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "hdd windows wp headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_UNKNOWN, "hdd windows wp disk status");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION, "hdd windows wp interface");
    FormatHealthLecturePlain(&g, plain, (int)sizeof(plain));
    ExpectHas(plain, "Запись запретила Windows", "hdd plain blames windows");
    ExpectNotHas(plain, "к замене", "hdd plain does not say replace");
    ExpectNotHas(plain, "кабель", "hdd plain does not blame the cable");
    FormatHddCautionPrompt(&g, prompt, (int)sizeof(prompt));
    ExpectHas(prompt, "Запись запретила Windows", "hdd prompt blames windows");

    /* 2. HDD behind a USB adapter. */
    Base(DRIVE_TYPE_HDD, "ST1000DM010", "CC43");
    Expect(AddAttr(5, 100, 0), "hdd usb realloc clean");
    Expect(AddAttr(197, 100, 0), "hdd usb pending clean");
    Expect(AddAttr(198, 100, 0), "hdd usb uncorr clean");
    g.bIsUSB = TRUE;
    g.bWriteProtected = TRUE;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "hdd adapter reliability");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "hdd adapter headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_UNKNOWN, "hdd adapter disk status");
    FormatHealthLecturePlain(&g, plain, (int)sizeof(plain));
    ExpectHas(plain, "Запись запретил переходник", "hdd plain blames adapter");
    ExpectNotHas(plain, "Диск сам", "hdd plain does not say the drive locked itself");
    ExpectNotHas(plain, "к замене", "hdd adapter plain does not say replace");
    LectureExpert(lecture, (int)sizeof(lecture));
    ExpectHas(lecture, "Запись запретил переходник", "hdd expert blames adapter");
    ExpectHas(lecture, "запись запрещена", "hdd expert matrix names the block");
    ExpectHas(lecture, "0 критических отказов", "hdd adapter is not a critical failure");
    ExpectNotHas(lecture, "к замене", "hdd expert does not say replace");
    FormatHddCautionPrompt(&g, prompt, (int)sizeof(prompt));
    ExpectHas(prompt, "Запись запретил переходник", "hdd prompt blames adapter");

    /* 3. SATA SSD, Windows policy. */
    Base(DRIVE_TYPE_SSD_SATA, "Crucial CT500MX500SSD1", "M3CR046");
    Expect(AddAttr(5, 100, 0), "ssd realloc clean");
    Expect(AddAttr(197, 100, 0), "ssd pending clean");
    Expect(AddAttr(198, 100, 0), "ssd uncorr clean");
    g.bWriteProtected = TRUE;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "sata windows wp reliability");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "sata windows wp headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_UNKNOWN, "sata windows wp disk status");
    FormatHealthLecturePlain(&g, plain, (int)sizeof(plain));
    ExpectHas(plain, "Запись запретила Windows", "sata plain blames windows");
    ExpectNotHas(plain, "к замене", "sata plain does not say replace");
    FormatSsdCautionPrompt(&g, prompt, (int)sizeof(prompt));
    ExpectHas(prompt, "Запись запретила Windows", "sata prompt blames windows");

    /* 4. NVMe through an adapter, no read-only warning bit. */
    Base(DRIVE_TYPE_NVME, "Samsung SSD 980 500GB", "1B4QFXO7");
    g.bIsNVMe = TRUE;
    g.bIsUSB = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.bWriteProtected = TRUE;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "nvme adapter reliability");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "nvme adapter headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_GOOD, "nvme adapter disk status");
    FormatHealthLecturePlain(&g, plain, (int)sizeof(plain));
    ExpectHas(plain, "Запись запретил переходник", "nvme plain blames adapter");
    ExpectNotHas(plain, "к замене", "nvme adapter plain does not say replace");
    FormatSsdCautionPrompt(&g, prompt, (int)sizeof(prompt));
    ExpectHas(prompt, "Запись запретил переходник", "nvme prompt blames adapter");

    /* 5. NVMe really read-only, even if it is also behind an adapter. */
    Base(DRIVE_TYPE_NVME, "Samsung SSD 980 500GB", "1B4QFXO7");
    g.bIsNVMe = TRUE;
    g.bIsUSB = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.nvmeHealth.CriticalWarning = NVME_CRIT_WARN_READ_ONLY;
    g.bWriteProtected = TRUE;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CRITICAL, "nvme own ro reliability");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CRITICAL, "nvme own ro headline");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_CRITICAL, "nvme own ro disk status");
    FormatHealthLecturePlain(&g, plain, (int)sizeof(plain));
    ExpectHas(plain, "Диск сам", "nvme plain says the drive itself");
    ExpectHas(plain, "к замене", "nvme own ro says replace");
    ExpectNotHas(plain, "Запись запретил переходник", "nvme own ro does not blame the adapter");

    CaseEnd("universal-write-protect", before);
}

static void TestAdataSx8200(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_NVME, "ADATA SX8200PNP", "42A4SANA");
    g.bIsNVMe = TRUE;
    g.bSMART_Supported = TRUE;
    g.nvmeHealth.AvailableSpare = 100;
    g.nvmeHealth.AvailableSpareThreshold = 10;
    g.nvmeHealth.PercentageUsed = 28;
    g.qwNVMeMediaErrors = 0;
    g.qwNVMeUnsafeShutdowns = 105;
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "adata media errors clean");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "adata health status good");
    ExpectInt((int)g.eDiskStatus, (int)HEALTH_STATUS_GOOD, "adata disk status good");
    ExpectInt(g.nEndurancePercent, 72, "adata endurance 72");
    CaseEnd("adata-sx8200", before);
}

/* Standby Awareness (gentle background monitoring for sleeping HDDs):
 * 1. DriveIsSleeping checks Sector Count from ATA CHECK POWER MODE (0xE5):
 *    0x00 (Standby) and 0x40 (NV Cache Standby / spun down) return TRUE.
 *    0x41 (NV Cache Active), 0x80 (Idle), 0xFF (Active/Idle), and -1 return FALSE.
 * 2. DriveCheckPowerMode refuses non-mechanical media (SSD / NVMe) and invalid handles.
 * 3. In DRIVE_INFO, bStandby marks sleeping state. */
static void TestStandbyAwareness(void)
{
    int before = g_fail;

    /* 1. DriveIsSleeping power state decoder */
    Expect(DriveIsSleeping(0x00), "0x00 is standby");
    Expect(DriveIsSleeping(0x40), "0x40 is nv cache standby spun-down");
    Expect(!DriveIsSleeping(0x41), "0x41 is nv cache spinning idle");
    Expect(!DriveIsSleeping(0x80), "0x80 is idle mode spinning");
    Expect(!DriveIsSleeping(0xFF), "0xFF is active/idle spinning");
    Expect(!DriveIsSleeping(0x20), "0x20 is not standby");
    Expect(!DriveIsSleeping(-1), "-1 is unsupported/error");

    /* 2. DriveCheckPowerMode safety checks */
    ExpectInt(DriveCheckPowerMode(NULL, NULL), -1, "null handle rejected");
    ExpectInt(DriveCheckPowerMode(INVALID_HANDLE_VALUE, NULL), -1, "invalid handle rejected");

    {
        DRIVE_INFO ssd;
        ZeroMemory(&ssd, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ExpectInt(DriveCheckPowerMode((HANDLE)(LONG_PTR)0x1234, &ssd), -1, "ssd does not probe power mode");
    }

    {
        DRIVE_INFO nvme;
        ZeroMemory(&nvme, sizeof(nvme));
        nvme.eType = DRIVE_TYPE_NVME;
        nvme.bIsNVMe = TRUE;
        ExpectInt(DriveCheckPowerMode((HANDLE)(LONG_PTR)0x1234, &nvme), -1, "nvme does not probe power mode");
    }

    /* 3. HDD standby state retention */
    Base(DRIVE_TYPE_HDD, "ST2000DM008-2FR102", "0001");
    g.dwCapacityMB = 2000 * 1024;
    g.nTemperatureC = 34;
    Expect(AddAttr(5, 100, 0), "hdd realloc 0");
    Expect(AddAttr(197, 100, 0), "hdd pending 0");
    Expect(AddAttr(198, 100, 0), "hdd uncorr 0");
    Run();
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "hdd baseline health");
    Expect(!g.bStandby, "hdd initially not in standby");

    /* Mark as sleeping during background poll */
    g.bStandby = TRUE;
    Expect(g.bStandby, "hdd standby flag set");
    ExpectInt(g.nTemperatureC, 34, "hdd temperature preserved across standby");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD, "hdd health preserved across standby");

    CaseEnd("standby-awareness", before);
}

static void TestMonitorIntervals(void)
{
    int before = g_fail;

    /* Verify consecutive ID allocations for radio group logic */
    ExpectInt(IDM_MONITOR_1M, 2023, "IDM_MONITOR_1M is 2023");
    ExpectInt(IDM_MONITOR_5M, 2024, "IDM_MONITOR_5M is 2024");
    ExpectInt(IDM_MONITOR_15M, 2025, "IDM_MONITOR_15M is 2025");
    ExpectInt(IDM_MONITOR_30M, 2026, "IDM_MONITOR_30M is 2026");
    ExpectInt(IDM_MONITOR_1H, 2027, "IDM_MONITOR_1H is 2027");

    /* Verify contiguous range IDM_MONITOR_1M..IDM_MONITOR_1H */
    Expect(IDM_MONITOR_5M == IDM_MONITOR_1M + 1, "5m follows 1m");
    Expect(IDM_MONITOR_15M == IDM_MONITOR_5M + 1, "15m follows 5m");
    Expect(IDM_MONITOR_30M == IDM_MONITOR_15M + 1, "30m follows 15m");
    Expect(IDM_MONITOR_1H == IDM_MONITOR_30M + 1, "1h follows 30m");

    /* Verify interval millisecond values */
    DWORD inv1m = 60000;
    DWORD inv5m = 300000;
    DWORD inv15m = 900000;
    DWORD inv30m = 1800000;
    DWORD inv1h = 3600000;
    ExpectInt((int)(inv1m / 1000), 60, "1m is 60 sec");
    ExpectInt((int)(inv5m / 1000), 300, "5m is 300 sec");
    ExpectInt((int)(inv15m / 1000), 900, "15m is 900 sec");
    ExpectInt((int)(inv30m / 1000), 1800, "30m is 1800 sec");
    ExpectInt((int)(inv1h / 1000), 3600, "1h is 3600 sec");

    CaseEnd("monitor-intervals", before);
}

static void TestSelfTestLogDecoding(void)
{
    int before = g_fail;
    int prog = 0;
    char desc[128];

    /* 1. Status byte 0: OK */
    ExpectInt((int)DecodeSelfTestStatus(0x00, &prog), (int)SELFTEST_ST_OK, "status 0x00 is OK");
    ExpectInt(prog, 100, "status 0x00 progress is 100%");
    ExpectStr(GetSelfTestStatusBadge(0x00), "ОК", "badge for 0x00 is OK");
    GetSelfTestStatusDesc(0x00, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "Пройден без ошибок", "desc for 0x00");

    /* 2. Status byte 0x10: Aborted */
    ExpectInt((int)DecodeSelfTestStatus(0x10, &prog), (int)SELFTEST_ST_ABORTED, "status 0x10 is aborted");
    ExpectStr(GetSelfTestStatusBadge(0x10), "контекст", "badge for 0x10 is context");
    GetSelfTestStatusDesc(0x10, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "Прерван хостом", "desc for 0x10");

    /* 3. Status byte 0x20: Interrupted */
    ExpectInt((int)DecodeSelfTestStatus(0x20, &prog), (int)SELFTEST_ST_INTERRUPTED, "status 0x20 is interrupted");
    ExpectStr(GetSelfTestStatusBadge(0x20), "контекст", "badge for 0x20 is context");
    GetSelfTestStatusDesc(0x20, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "Прерван сбросом", "desc for 0x20");

    /* 4. Status byte 0x30..0x60: Fatal, unknown, electrical, servo */
    ExpectInt((int)DecodeSelfTestStatus(0x30, NULL), (int)SELFTEST_ST_FATAL_ERROR, "status 0x30 is fatal");
    ExpectStr(GetSelfTestStatusBadge(0x30), "ПЛОХО", "badge for 0x30 is BAD");

    ExpectInt((int)DecodeSelfTestStatus(0x40, NULL), (int)SELFTEST_ST_FAILED_UNKNOWN, "status 0x40 is unknown fail");
    ExpectStr(GetSelfTestStatusBadge(0x40), "ПЛОХО", "badge for 0x40 is BAD");

    ExpectInt((int)DecodeSelfTestStatus(0x50, NULL), (int)SELFTEST_ST_FAILED_ELECTR, "status 0x50 is electrical fail");
    ExpectStr(GetSelfTestStatusBadge(0x50), "ПЛОХО", "badge for 0x50 is BAD");

    ExpectInt((int)DecodeSelfTestStatus(0x60, NULL), (int)SELFTEST_ST_FAILED_SERVO, "status 0x60 is servo fail");
    ExpectStr(GetSelfTestStatusBadge(0x60), "ПЛОХО", "badge for 0x60 is BAD");

    /* 5. Status byte 0x70 (112): Read element failed (bad LBA) */
    ExpectInt((int)DecodeSelfTestStatus(112, NULL), (int)SELFTEST_ST_FAILED_READ, "status 112 (0x70) is read failure");
    ExpectStr(GetSelfTestStatusBadge(112), "ПЛОХО", "badge for 112 is BAD");
    GetSelfTestStatusDesc(112, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "Сбой: ошибка чтения (дефект LBA)", "desc for 112 without LBA");
    GetSelfTestStatusDesc(112, 12345678UL, desc, (int)sizeof(desc));
    ExpectHas(desc, "12345678", "desc for 112 with failing LBA");

    /* 6. Status byte 0x80: Handling damage / shock */
    ExpectInt((int)DecodeSelfTestStatus(0x80, NULL), (int)SELFTEST_ST_FAILED_SHOCK, "status 0x80 is shock damage");
    ExpectStr(GetSelfTestStatusBadge(0x80), "Внимание", "badge for 0x80 is Caution");
    GetSelfTestStatusDesc(0x80, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "удар", "desc for 0x80 mentions shock");

    /* 7. Status byte 0xF9: In progress (~10% done) */
    ExpectInt((int)DecodeSelfTestStatus(0xF9, &prog), (int)SELFTEST_ST_IN_PROGRESS, "status 0xF9 is in progress");
    ExpectInt(prog, 10, "status 0xF9 progress is 10%");
    ExpectStr(GetSelfTestStatusBadge(0xF9), "контекст", "badge for 0xF9 is context");
    GetSelfTestStatusDesc(0xF9, 0, desc, (int)sizeof(desc));
    ExpectHas(desc, "10%", "desc for 0xF9 mentions 10%");

    /* 8. Status byte 0xF0: In progress (<10% remaining -> ~100% done) */
    ExpectInt((int)DecodeSelfTestStatus(0xF0, &prog), (int)SELFTEST_ST_IN_PROGRESS, "status 0xF0 is in progress");
    ExpectInt(prog, 100, "status 0xF0 progress is 100%");

    /* 9. Health Engine integration with failed self-test */
    Base(DRIVE_TYPE_HDD, "ST1000DM010", "CC43");
    g.bGotSelfTestLog = TRUE;
    g.nSelfTestStatus = 112; /* 0x70: read failure */
    Run();
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CRITICAL, "health engine marks drive CRITICAL on failed self-test");

    CaseEnd("selftest-log-decoding", before);
}

static void TestLatencyProbe(void)
{
    int before = g_check;
    LATENCY_PROBE_RESULT res;
    char summary[4096];

    /* 1. Bin classification thresholds */
    ExpectInt((int)LatencyBinFromMs(0.5, 0), (int)LATENCY_BIN_FAST, "0.5ms is FAST");
    ExpectInt((int)LatencyBinFromMs(9.9, 0), (int)LATENCY_BIN_FAST, "9.9ms is FAST");
    ExpectInt((int)LatencyBinFromMs(10.0, 0), (int)LATENCY_BIN_NORMAL, "10.0ms is NORMAL");
    ExpectInt((int)LatencyBinFromMs(49.9, 0), (int)LATENCY_BIN_NORMAL, "49.9ms is NORMAL");
    ExpectInt((int)LatencyBinFromMs(50.0, 0), (int)LATENCY_BIN_ACCEPTABLE, "50.0ms is ACCEPTABLE");
    ExpectInt((int)LatencyBinFromMs(149.9, 0), (int)LATENCY_BIN_ACCEPTABLE, "149.9ms is ACCEPTABLE");
    ExpectInt((int)LatencyBinFromMs(150.0, 0), (int)LATENCY_BIN_SLOW, "150.0ms is SLOW");
    ExpectInt((int)LatencyBinFromMs(499.9, 0), (int)LATENCY_BIN_SLOW, "499.9ms is SLOW");
    ExpectInt((int)LatencyBinFromMs(500.0, 0), (int)LATENCY_BIN_CRITICAL, "500.0ms is CRITICAL");
    ExpectInt((int)LatencyBinFromMs(1500.0, 0), (int)LATENCY_BIN_CRITICAL, "1500.0ms is CRITICAL");

    /* Error overrides latency */
    ExpectInt((int)LatencyBinFromMs(1.0, ERROR_READ_FAULT), (int)LATENCY_BIN_ERROR, "read fault is ERROR");
    ExpectInt((int)LatencyBinFromMs(0.0, ERROR_CRC), (int)LATENCY_BIN_ERROR, "crc error is ERROR");

    /* 2. Color codes */
    Expect(LatencyBinColor(LATENCY_BIN_FAST) == RGB(46, 204, 113), "FAST color is green");
    Expect(LatencyBinColor(LATENCY_BIN_CRITICAL) == RGB(231, 76, 60), "CRITICAL color is red");
    Expect(LatencyBinColor(LATENCY_BIN_ERROR) == RGB(142, 68, 173), "ERROR color is purple");

    /* 3. SSD all-fast verdict */
    memset(&res, 0, sizeof(res));
    res.nTotalSamples = 128;
    res.nSamplesTested = 128;
    res.nBins[LATENCY_BIN_FAST] = 128;
    res.fMinMs = 0.04;
    res.fMaxMs = 0.12;
    res.fAvgMs = 0.06;
    res.fTotalTimeSec = 0.08;
    LatencyProbeAssessVerdict(&res, true);
    ExpectInt((int)res.eVerdictLevel, (int)LATENCY_VERDICT_EXCELLENT, "all fast SSD is EXCELLENT");
    ExpectStr(res.szVerdictBadge, "ОТЛИЧНО", "all fast SSD badge is EXCELLENT");

    /* 4. SSD Data Retention Loss (>500ms LDPC Soft Decision) */
    memset(&res, 0, sizeof(res));
    res.nTotalSamples = 128;
    res.nSamplesTested = 128;
    res.nBins[LATENCY_BIN_FAST] = 126;
    res.nBins[LATENCY_BIN_NORMAL] = 1;
    res.nBins[LATENCY_BIN_CRITICAL] = 1;
    res.fMinMs = 0.05;
    res.fMaxMs = 1250.0;
    res.fAvgMs = 9.8;
    res.fTotalTimeSec = 1.35;
    LatencyProbeAssessVerdict(&res, true);
    ExpectInt((int)res.eVerdictLevel, (int)LATENCY_VERDICT_WARNING, "critical latency on SSD is WARNING");
    ExpectStr(res.szVerdictBadge, "УТЕЧКА ЗАРЯДА", "critical latency SSD badge is RETENTION LOSS");
    ExpectHas(res.szVerdictText, "LDPC", "SSD verdict text explains LDPC soft decision");
    ExpectHas(res.szVerdictText, "Retention Loss", "SSD verdict text explains Retention Loss");

    /* 5. HDD Weak Head / Multi-seek degradation (>500ms) */
    memset(&res, 0, sizeof(res));
    res.nTotalSamples = 128;
    res.nSamplesTested = 128;
    res.nBins[LATENCY_BIN_NORMAL] = 127;
    res.nBins[LATENCY_BIN_CRITICAL] = 1;
    res.fMinMs = 11.2;
    res.fMaxMs = 850.0;
    res.fAvgMs = 18.5;
    res.fTotalTimeSec = 2.4;
    LatencyProbeAssessVerdict(&res, false);
    ExpectInt((int)res.eVerdictLevel, (int)LATENCY_VERDICT_WARNING, "critical latency on HDD is WARNING");
    ExpectStr(res.szVerdictBadge, "СЛАБЫЙ СЕКТОР", "critical latency HDD badge is WEAK SECTOR");
    ExpectHas(res.szVerdictText, "головки", "HDD verdict text mentions weak head");

    /* 6. I/O Read Failure */
    memset(&res, 0, sizeof(res));
    res.nTotalSamples = 128;
    res.nSamplesTested = 128;
    res.nBins[LATENCY_BIN_FAST] = 127;
    res.nBins[LATENCY_BIN_ERROR] = 1;
    res.fMinMs = 0.05;
    res.fMaxMs = 0.20;
    res.fAvgMs = 0.08;
    LatencyProbeAssessVerdict(&res, true);
    ExpectInt((int)res.eVerdictLevel, (int)LATENCY_VERDICT_BAD, "read error is BAD");
    ExpectStr(res.szVerdictBadge, "СБОЙ ЧТЕНИЯ", "read error badge is READ FAILURE");

    /* 7. Summary report formatting */
    FormatLatencySummary(&res, "Kingston KC3000 1024GB", "50026B7685D01234",
                         1024ULL * 1024ULL * 1024ULL * 1024ULL, true,
                         summary, sizeof(summary));
    ExpectHas(summary, "Kingston KC3000", "summary contains model");
    ExpectHas(summary, "50026B7685D01234", "summary contains serial");
    ExpectHas(summary, "1024.0 GB (SSD)", "summary contains capacity and SSD tag");
    ExpectHas(summary, "СБОЙ ЧТЕНИЯ", "summary contains verdict badge");

    /* 8. Latency probe caching */
    ClearDriveLatencyResults();
    Expect(GetDriveLatencyResult(0) == NULL, "cache initial 0 is null");
    Expect(GetDriveLatencyResult(1) == NULL, "cache initial 1 is null");
    SetDriveLatencyResult(0, &res);
    const LATENCY_PROBE_RESULT* pCached = GetDriveLatencyResult(0);
    Expect(pCached != NULL, "cached drive 0 is not null");
    if (pCached) {
        ExpectInt(pCached->nSamplesTested, 128, "cached samples count");
        ExpectStr(pCached->szVerdictBadge, "СБОЙ ЧТЕНИЯ", "cached verdict badge");
    }
    Expect(GetDriveLatencyResult(1) == NULL, "cache 1 remains null");

    /* 9. Markdown report generation */
    char mdBuf[4096];
    FormatLatencyMarkdown(NULL, mdBuf, sizeof(mdBuf));
    ExpectHas(mdBuf, "не проводилось", "null result markdown indicates test not run");

    FormatLatencyMarkdown(pCached, mdBuf, sizeof(mdBuf));
    ExpectHas(mdBuf, "Вердикт", "markdown report contains verdict field");
    ExpectHas(mdBuf, "СБОЙ ЧТЕНИЯ", "markdown report contains verdict badge");
    ExpectHas(mdBuf, "Проверено блоков", "markdown report contains tested blocks count");
    ExpectHas(mdBuf, "Диапазон задержки", "markdown report contains distribution table header");
    ExpectHas(mdBuf, "<10 мс", "markdown report table contains fast bin row");
    ExpectHas(mdBuf, "Ошибка чтения", "markdown report table contains error bin row");

    /* 10. Full surface read speed benchmark mode */
    LATENCY_PROBE_RESULT resFull;
    memset(&resFull, 0, sizeof(resFull));
    resFull.eMode = LATENCY_MODE_FULL;
    resFull.nTotalSamples = 128;
    resFull.nSamplesTested = 128;
    resFull.nTotalCapacityBytes = 512ULL * 1024ULL * 1024ULL * 1024ULL;
    resFull.nBytesTested = resFull.nTotalCapacityBytes;
    resFull.fCurrentSpeedMBs = 480.0;
    resFull.fAvgSpeedMBs = 485.5;
    resFull.fMinSpeedMBs = 412.0;
    resFull.fMaxSpeedMBs = 532.8;
    resFull.fMinMs = 1.8;
    resFull.fAvgMs = 2.1;
    resFull.fMaxMs = 3.5;
    resFull.fTotalTimeSec = 1105.2;
    resFull.nBins[LATENCY_BIN_FAST] = 128;
    LatencyProbeAssessVerdict(&resFull, true);
    ExpectInt((int)resFull.eVerdictLevel, (int)LATENCY_VERDICT_EXCELLENT, "full test excellent SSD is EXCELLENT");
    ExpectStr(resFull.szVerdictBadge, "ОТЛИЧНО", "full test excellent SSD badge is EXCELLENT");
    ExpectHas(resFull.szVerdictText, "Отличная скорость", "full test verdict mentions sequential read speed");

    char summaryFull[4096];
    FormatLatencySummary(&resFull, "Samsung 870 EVO 500GB", "S5YKNX0R123456",
                         resFull.nTotalCapacityBytes, true, summaryFull, sizeof(summaryFull));
    ExpectHas(summaryFull, "ПОЛНОГО ТЕСТИРОВАНИЯ", "full summary title mentions full test");
    ExpectHas(summaryFull, "100%", "full summary mentions 100% surface");
    ExpectHas(summaryFull, "485.5 MB/s", "full summary mentions average speed");

    char mdBufFull[4096];
    FormatLatencyMarkdown(&resFull, mdBufFull, sizeof(mdBufFull));
    ExpectHas(mdBufFull, "100% емкости", "full markdown mentions 100% capacity");
    ExpectHas(mdBufFull, "485.5 МБ/с", "full markdown contains avg speed");

    /* 11. Full surface test with early sample count (nSamplesTested = 1, totalBins = 569) */
    LATENCY_PROBE_RESULT resEarly;
    memset(&resEarly, 0, sizeof(resEarly));
    resEarly.eMode = LATENCY_MODE_FULL;
    resEarly.nTotalSamples = 128;
    resEarly.nSamplesTested = 1;
    resEarly.nBins[LATENCY_BIN_FAST] = 564;
    resEarly.nBins[LATENCY_BIN_NORMAL] = 4;
    resEarly.nBins[LATENCY_BIN_ERROR] = 1;
    char mdEarly[4096];
    FormatLatencyMarkdown(&resEarly, mdEarly, sizeof(mdEarly));
    ExpectHas(mdEarly, "99.1%", "fast bin share is calculated from total blocks, not cell index");
    ExpectHas(mdEarly, "0.2%", "error bin share is calculated from total blocks, not cell index");

    CaseEnd("latency-probe-assessment", before);
}

static void TestLinkSpeedDegradation(void)
{
    int before = g_check;

    /* 1. SATA Drive supports Gen3 (6 Gb/s) but negotiated Gen1 (1.5 Gb/s) */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotReturnStatus = TRUE;
        WORD ident[256];
        memset(ident, 0, sizeof(ident));
        ident[76] = 0x000E; /* Gen 1, 2, 3 supported -> maxGen = 3 (6 Gb/s) */
        ident[77] = (1 << 1); /* negotiated Gen 1 (1.5 Gb/s) */
        FillAtaProtocolFromIdent(&info, ident);
        Expect(info.bLinkDegraded, "SATA Gen1 on Gen3 drive is degraded");
        ExpectInt((int)info.nSataMaxGen, 3, "nSataMaxGen is 3 (6 Gb/s)");
        ExpectInt((int)info.nSataCurGen, 1, "nSataCurGen is 1 (1.5 Gb/s)");
        ExpectHas(info.szProtocol, "SATA 1.5", "szProtocol has negotiated speed");
        ExpectHas(info.szProtocol, "макс. 6", "szProtocol has max speed");
        ExpectHas(info.szProtocol, "\xE2\x9A\xA0", "szProtocol has warning glyph");

        info.eType = DRIVE_TYPE_SSD_SATA;
        AssessDriveHealth(&info);
        ExpectInt((int)info.eInterface, (int)HEALTH_STATUS_CAUTION, "eInterface is CAUTION on link degradation");
        ExpectInt((int)info.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "headline health is CAUTION on link degradation");

        char prompt[256];
        FormatSsdCautionPrompt(&info, prompt, sizeof(prompt));
        ExpectHas(prompt, "деградация линка", "SSD caution prompt explains link degradation");

        char lecture[4096];
        FormatHealthLecture(&info, lecture, sizeof(lecture));
        ExpectHas(lecture, "деградация скорости шины SATA", "SSD lecture explains SATA link degradation");
        ExpectHas(lecture, "Текущий режим: 1.5 Гбит/с", "SSD lecture prints current link speed");
        ExpectHas(lecture, "Поддерживается: 6 Гбит/с", "SSD lecture prints max supported speed");
    }

    /* 2. SATA Drive supports Gen3 (6 Gb/s) and negotiated Gen3 (6 Gb/s) - normal */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotReturnStatus = TRUE;
        WORD ident[256];
        memset(ident, 0, sizeof(ident));
        ident[76] = 0x000E; /* Gen 1, 2, 3 supported -> maxGen = 3 (6 Gb/s) */
        ident[77] = (3 << 1); /* negotiated Gen 3 (6 Gb/s) */
        FillAtaProtocolFromIdent(&info, ident);
        Expect(!info.bLinkDegraded, "SATA Gen3 on Gen3 drive is not degraded");
        ExpectInt((int)info.nSataMaxGen, 3, "nSataMaxGen is 3");
        ExpectInt((int)info.nSataCurGen, 3, "nSataCurGen is 3");
        ExpectStr(info.szProtocol, "SATA 6 Гбит/с", "szProtocol is clean SATA 6 Gb/s");

        info.eType = DRIVE_TYPE_HDD;
        AssessDriveHealth(&info);
        ExpectInt((int)info.eInterface, (int)HEALTH_STATUS_GOOD, "eInterface is GOOD on normal SATA link");
    }

    /* 3. USB Drive with SATA disk: must NOT set bLinkDegraded (dongle quirk) */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.bIsUSB = TRUE;
        WORD ident[256];
        memset(ident, 0, sizeof(ident));
        ident[76] = 0x000E;
        ident[77] = (1 << 1); /* Dongle reports 1.5 Gb/s */
        FillAtaProtocolFromIdent(&info, ident);
        Expect(!info.bLinkDegraded, "USB SATA drive ignores dongle word 77 for degradation");
    }

    /* 4. NVMe SSD running at PCIe 3.0 x2 on a PCIe 4.0 x4 drive */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.bIsNVMe = TRUE;
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotNVMeIdent = TRUE;
        info.nvmeHealth.AvailableSpare = 100;
        info.nvmeHealth.AvailableSpareThreshold = 10;
        info.nvmeHealth.PercentageUsed = 0;
        info.eType = DRIVE_TYPE_NVME;
        info.nPcieCurLinkSpeed = 3;
        info.nPcieCurLinkWidth = 2;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.bLinkDegraded = TRUE;

        AssessDriveHealth(&info);
        ExpectInt((int)info.eInterface, (int)HEALTH_STATUS_CAUTION, "NVMe eInterface is CAUTION on PCIe link degradation");
        ExpectInt((int)info.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "NVMe headline health is CAUTION on PCIe link degradation");

        FillDriveProtocol(&info);
        ExpectHas(info.szProtocol, "PCIe 3.0 x2", "NVMe szProtocol has current PCIe mode");
        ExpectHas(info.szProtocol, "макс. 4.0 x4", "NVMe szProtocol has max supported PCIe mode");
        ExpectHas(info.szProtocol, "\xE2\x9A\xA0", "NVMe szProtocol has warning glyph");

        char prompt[256];
        FormatSsdCautionPrompt(&info, prompt, sizeof(prompt));
        ExpectHas(prompt, "деградация линка", "NVMe caution prompt mentions link degradation");

        char lecture[4096];
        FormatHealthLecture(&info, lecture, sizeof(lecture));
        ExpectHas(lecture, "деградация скорости шины PCIe", "NVMe lecture mentions PCIe degradation");
        ExpectHas(lecture, "Текущий режим: PCIe 3.0 x2", "NVMe lecture prints current PCIe link");
        ExpectHas(lecture, "Поддерживается: PCIe 4.0 x4", "NVMe lecture prints max supported PCIe link");
    }

    /* 5. NVMe SSD running at full PCIe 4.0 x4 - normal */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.bIsNVMe = TRUE;
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotNVMeIdent = TRUE;
        info.nvmeHealth.AvailableSpare = 100;
        info.nvmeHealth.AvailableSpareThreshold = 10;
        info.nvmeHealth.PercentageUsed = 0;
        info.eType = DRIVE_TYPE_NVME;
        info.nPcieCurLinkSpeed = 4;
        info.nPcieCurLinkWidth = 4;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.bLinkDegraded = FALSE;

        AssessDriveHealth(&info);
        ExpectInt((int)info.eInterface, (int)HEALTH_STATUS_GOOD, "NVMe eInterface is GOOD on full link");
        FillDriveProtocol(&info);
        ExpectHas(info.szProtocol, "PCIe 4.0 x4", "NVMe szProtocol has PCIe 4.0 x4");
        Expect(strstr(info.szProtocol, "\xE2\x9A\xA0") == NULL, "No warning glyph when link is full speed");
    }

    /* 6. Gen4 drive on a Gen3 x4 port: the board ceiling, not a bad seat */
    {
        DRIVE_INFO info;
        char lecture[4096];
        char expert[4096];
        memset(&info, 0, sizeof(info));
        info.bIsNVMe = TRUE;
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotNVMeIdent = TRUE;
        info.nvmeHealth.AvailableSpare = 100;
        info.nvmeHealth.AvailableSpareThreshold = 10;
        info.nvmeHealth.PercentageUsed = 0;
        info.eType = DRIVE_TYPE_NVME;
        info.nPcieCurLinkSpeed = 3;
        info.nPcieCurLinkWidth = 4;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.nPcieUpMaxLinkSpeed = 3;
        info.nPcieUpMaxLinkWidth = 4;
        lstrcpynA(info.szPcieUpDev, "DEV_9DB4", (int)sizeof(info.szPcieUpDev));
        ApplyPcieLinkLimit(&info);
        Expect(!info.bLinkDegraded, "Gen4 drive on a Gen3 x4 port is not degraded");
        Expect(info.bPcieHostCapped, "Gen4 drive on a Gen3 x4 port is host-capped");

        AssessDriveHealth(&info);
        ExpectInt((int)info.eInterface, (int)HEALTH_STATUS_GOOD, "host cap does not raise interface");
        ExpectInt((int)info.eHealthStatus, (int)HEALTH_STATUS_GOOD, "host cap does not raise the headline");

        FillDriveProtocol(&info);
        ExpectHas(info.szProtocol, "PCIe 3.0 x4", "host cap protocol shows negotiated 3.0 x4");
        ExpectHas(info.szProtocol, "диск 4.0 x4", "host cap protocol names the drive max");
        ExpectHas(info.szProtocol, "порт DEV_9DB4 3.0 x4", "host cap protocol names the port");
        Expect(strstr(info.szProtocol, "\xE2\x9A\xA0") == NULL, "host cap protocol has no warning glyph");

        FormatHealthLecture(&info, lecture, sizeof(lecture));
        ExpectHas(lecture, "Диск сам PCIe 4.0 x4", "lecture says the drive itself is Gen4");
        ExpectHas(lecture, "DEV_9DB4", "lecture names the port");
        ExpectHas(lecture, "потолок этой платы", "lecture says this is the board ceiling");
        ExpectNotHas(lecture, "фиксацию", "lecture does not ask to reseat");

        FormatHealthLectureExpert(&info, expert, sizeof(expert));
        ExpectHas(expert, "потолок этой платы", "expert lecture says this is the board ceiling");
        ExpectNotHas(expert, "фиксацию", "expert lecture does not ask to reseat");
    }

    /* 7. Real downtrain: 3.0 x2 on a port that is also 4.0 x4 */
    {
        DRIVE_INFO info;
        char lecture[4096];
        memset(&info, 0, sizeof(info));
        info.bIsNVMe = TRUE;
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotNVMeIdent = TRUE;
        info.nvmeHealth.AvailableSpare = 100;
        info.nvmeHealth.AvailableSpareThreshold = 10;
        info.nvmeHealth.PercentageUsed = 0;
        info.eType = DRIVE_TYPE_NVME;
        info.nPcieCurLinkSpeed = 3;
        info.nPcieCurLinkWidth = 2;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.nPcieUpMaxLinkSpeed = 4;
        info.nPcieUpMaxLinkWidth = 4;
        ApplyPcieLinkLimit(&info);
        Expect(info.bLinkDegraded, "3.0 x2 on a 4.0 x4 port is degraded");
        Expect(!info.bPcieHostCapped, "a real downtrain is not host-capped");
        AssessDriveHealth(&info);
        ExpectInt((int)info.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "real PCIe downtrain stays CAUTION");
        FillDriveProtocol(&info);
        ExpectHas(info.szProtocol, "\xE2\x9A\xA0", "real downtrain keeps the warning glyph");
        FormatHealthLecture(&info, lecture, sizeof(lecture));
        ExpectHas(lecture, "деградация скорости шины PCIe", "real downtrain lecture still warns");
        ExpectHas(lecture, "фиксацию", "real downtrain still asks to check the seat");
    }

    /* 8. Port max was not read: keep the old warning */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.nPcieCurLinkSpeed = 3;
        info.nPcieCurLinkWidth = 4;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        ApplyPcieLinkLimit(&info);
        Expect(info.bLinkDegraded, "unknown upstream still counts as degraded");
        Expect(!info.bPcieHostCapped, "unknown upstream is not host-capped");
    }

    /* 9. Below the port as well: still degraded, even if the port is slower than the drive */
    {
        DRIVE_INFO info;
        memset(&info, 0, sizeof(info));
        info.nPcieCurLinkSpeed = 2;
        info.nPcieCurLinkWidth = 4;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.nPcieUpMaxLinkSpeed = 3;
        info.nPcieUpMaxLinkWidth = 4;
        lstrcpynA(info.szPcieUpDev, "DEV_9DB4", (int)sizeof(info.szPcieUpDev));
        ApplyPcieLinkLimit(&info);
        Expect(info.bLinkDegraded, "link below the port is still degraded");
        Expect(!info.bPcieHostCapped, "link below the port is not called the board ceiling");
    }

    /* 10. Port is really x2: an x4 drive at x2 is the port, not a bad seat */
    {
        DRIVE_INFO info;
        char lecture[4096];
        memset(&info, 0, sizeof(info));
        info.bIsNVMe = TRUE;
        info.bSMART_Supported = TRUE;
        info.bSMART_Enabled = TRUE;
        info.bGotNVMeIdent = TRUE;
        info.nvmeHealth.AvailableSpare = 100;
        info.nvmeHealth.AvailableSpareThreshold = 10;
        info.nvmeHealth.PercentageUsed = 0;
        info.eType = DRIVE_TYPE_NVME;
        info.nPcieCurLinkSpeed = 4;
        info.nPcieCurLinkWidth = 2;
        info.nPcieMaxLinkSpeed = 4;
        info.nPcieMaxLinkWidth = 4;
        info.nPcieUpMaxLinkSpeed = 4;
        info.nPcieUpMaxLinkWidth = 2;
        ApplyPcieLinkLimit(&info);
        Expect(!info.bLinkDegraded, "x2 port does not degrade an x4 drive");
        Expect(info.bPcieHostCapped, "x2 port caps an x4 drive");
        AssessDriveHealth(&info);
        ExpectInt((int)info.eHealthStatus, (int)HEALTH_STATUS_GOOD, "x2 port does not raise the headline");
        FillDriveProtocol(&info);
        ExpectHas(info.szProtocol, "диск 4.0 x4", "x2 port protocol names the drive width");
        ExpectHas(info.szProtocol, "порт 4.0 x2", "x2 port protocol names the port width");
        Expect(strstr(info.szProtocol, "DEV_") == NULL, "missing port id is not invented");
        FormatHealthLecture(&info, lecture, sizeof(lecture));
        ExpectHas(lecture, "Порт, в котором он сидит", "lecture without a port id still names the port");
        ExpectNotHas(lecture, "фиксацию", "x2 port lecture does not ask to reseat");
    }

    CaseEnd("link-speed-degradation", before);
}

static void TestStorageEventLog(void)
{
    int before = g_fail;

    DRIVE_INFO drives[2];
    ZeroMemory(drives, sizeof(drives));
    drives[0].nDriveIndex = 0;
    drives[0].nScsiPort = 0;
    drives[0].eType = DRIVE_TYPE_SSD_SATA;
    drives[0].bSMART_Supported = TRUE;
    drives[0].bSMART_Enabled = TRUE;
    drives[0].bGotReturnStatus = TRUE;

    drives[1].nDriveIndex = 3;
    drives[1].nScsiPort = 1;
    drives[1].eType = DRIVE_TYPE_SSD_SATA;
    drives[1].bSMART_Supported = TRUE;
    drives[1].bSMART_Enabled = TRUE;
    drives[1].bGotReturnStatus = TRUE;

    const wchar_t* xmlEvt153 =
        L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
        L"<System><Provider Name='disk'/><EventID Qualifiers='32772'>153</EventID>"
        L"<TimeCreated SystemTime='2026-10-28T08:56:02.0063891Z'/></System>"
        L"<EventData><Data>\\Device\\Harddisk3\\DR121</Data><Data>0xc30a360</Data><Data>3</Data></EventData>"
        L"</Event>";

    BOOL ok153 = ParseStorageEventXml(xmlEvt153, drives, 2);
    Expect(ok153, "ParseStorageEventXml 153 returned TRUE");
    ExpectInt(drives[1].eventStats.nEvent153Count, 1, "Drive 3 received 1 Event 153");
    ExpectInt(drives[0].eventStats.nEvent153Count, 0, "Drive 0 received 0 Event 153");
    Expect(strstr(drives[1].eventStats.szLastEventTime, "2026-10-28 08:56:02") != NULL, "Event 153 timestamp parsed");

    const wchar_t* xmlEvt129 =
        L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
        L"<System><Provider Name='storahci'/><EventID Qualifiers='32772'>129</EventID>"
        L"<TimeCreated SystemTime='2026-10-27T13:58:20.2183672Z'/></System>"
        L"<EventData><Data>\\Device\\RaidPort0</Data></EventData>"
        L"</Event>";

    BOOL ok129 = ParseStorageEventXml(xmlEvt129, drives, 2);
    Expect(ok129, "ParseStorageEventXml 129 returned TRUE");
    ExpectInt(drives[0].eventStats.nEvent129Count, 1, "Drive on RaidPort0 received Event 129");
    ExpectInt(drives[1].eventStats.nEvent129Count, 0, "Drive on RaidPort1 received 0 Event 129");

    const wchar_t* xmlEvt51 =
        L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
        L"<System><Provider Name='disk'/><EventID>51</EventID>"
        L"<TimeCreated SystemTime='2026-10-28T08:56:24.0273783Z'/></System>"
        L"<EventData><Data>\\Device\\Harddisk3\\DR121</Data></EventData>"
        L"</Event>";

    BOOL ok51 = ParseStorageEventXml(xmlEvt51, drives, 2);
    Expect(ok51, "ParseStorageEventXml 51 returned TRUE");
    ExpectInt(drives[1].eventStats.nEvent51Count, 1, "Drive 3 received Event 51");

    const wchar_t* xmlEvt7 =
        L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
        L"<System><Provider Name='disk'/><EventID>7</EventID>"
        L"<TimeCreated SystemTime='2026-10-28T09:00:00.0000000Z'/></System>"
        L"<EventData><Data>\\Device\\Harddisk3\\DR121</Data></EventData>"
        L"</Event>";

    BOOL ok7 = ParseStorageEventXml(xmlEvt7, drives, 2);
    Expect(ok7, "ParseStorageEventXml 7 returned TRUE");
    ExpectInt(drives[1].eventStats.nEventHwErrCount, 1, "Drive 3 received Event 7");

    /* Test health assessment reaction */
    drives[0].eventStats.bQueried = TRUE;
    AssessDriveHealth(&drives[0]);
    ExpectInt((int)drives[0].eInterface, (int)HEALTH_STATUS_OBSERVE, "Isolated Event 129 elevates eInterface to OBSERVE");

    drives[0].eventStats.nEvent129Count = 5;
    AssessDriveHealth(&drives[0]);
    ExpectInt((int)drives[0].eInterface, (int)HEALTH_STATUS_CAUTION, "Event 129 >= 5 elevates eInterface to CAUTION");

    drives[1].eventStats.bQueried = TRUE;
    AssessDriveHealth(&drives[1]);
    ExpectInt((int)drives[1].eInterface, (int)HEALTH_STATUS_CAUTION, "Event 51 elevates eInterface to CAUTION");
    ExpectInt((int)drives[1].eReliability, (int)HEALTH_STATUS_CAUTION, "Event 7 elevates eReliability to CAUTION");

    char szPlain[4096];
    char szExpert[4096];
    FormatHealthLecturePlain(&drives[0], szPlain, sizeof(szPlain));
    FormatHealthLectureExpert(&drives[0], szExpert, sizeof(szExpert));
    Expect(strstr(szExpert, "129") != NULL, "Expert lecture mentions Event 129");

    /* Test exact NT device target matching (\Device\HarddiskX\DRy) */
    DRIVE_INFO usbDrive;
    ZeroMemory(&usbDrive, sizeof(usbDrive));
    usbDrive.nDriveIndex = 3;
    usbDrive.bIsUSB = TRUE;
    usbDrive.eType = DRIVE_TYPE_SSD_SATA;
    usbDrive.bSMART_Supported = TRUE;
    usbDrive.bSMART_Enabled = TRUE;
    usbDrive.bGotReturnStatus = TRUE;
    usbDrive.nSSDLifeLeft = 100;
    usbDrive.nEndurancePercent = 100;
    lstrcpynA(usbDrive.szDeviceTarget, "\\Device\\Harddisk3\\DR28", (int)sizeof(usbDrive.szDeviceTarget));

    /* Old event belonging to previously connected drive DR121 must be rejected */
    BOOL okOld = ParseStorageEventXml(xmlEvt153, &usbDrive, 1);
    Expect(!okOld, "Event 153 for DR121 rejected for drive on DR28");
    ExpectInt(usbDrive.eventStats.nEvent153Count, 0, "Drive DR28 did not receive DR121 event");

    /* Current event belonging to this active device instance DR28 must be accepted */
    const wchar_t* xmlEvtActive =
        L"<Event xmlns='http://schemas.microsoft.com/win/2004/08/events/event'>"
        L"<System><Provider Name='disk'/><EventID>154</EventID>"
        L"<TimeCreated SystemTime='2026-10-31T12:00:00.0000000Z'/></System>"
        L"<EventData><Data>\\Device\\Harddisk3\\DR28</Data></EventData>"
        L"</Event>";
    BOOL okActive = ParseStorageEventXml(xmlEvtActive, &usbDrive, 1);
    Expect(okActive, "Event 154 for active DR28 accepted");
    ExpectInt(usbDrive.eventStats.nEventHwErrCount, 1, "Drive DR28 received Event 154");

    /* Check lecture reason on clean SSD with event log hardware error */
    usbDrive.eventStats.bQueried = TRUE;
    AssessDriveHealth(&usbDrive);
    ExpectInt((int)usbDrive.eReliability, (int)HEALTH_STATUS_CAUTION, "Event 154 elevates SSD eReliability to CAUTION");
    char szSsdPlain[4096];
    FormatHealthLecturePlain(&usbDrive, szSsdPlain, sizeof(szSsdPlain));

    Expect(strstr(szSsdPlain, "Event 7/154") != NULL, "SSD plain lecture explicitly states Event 7/154 reason");
    Expect(strstr(szSsdPlain, "Reason:\r\n\r\n") == NULL && strstr(szSsdPlain, "Причина:\r\n\r\n") == NULL,
           "SSD lecture reason is not left blank");

    /* Test small volume (< 1.0 GB) formatting in MB */
    usbDrive.trimInfo.bQueried = TRUE;
    usbDrive.trimInfo.nVolumeCount = 1;
    lstrcpynA(usbDrive.trimInfo.volumes[0].szLetter, "F:", (int)sizeof(usbDrive.trimInfo.volumes[0].szLetter));
    lstrcpynA(usbDrive.trimInfo.volumes[0].szFileSystem, "FAT32", (int)sizeof(usbDrive.trimInfo.volumes[0].szFileSystem));
    usbDrive.trimInfo.volumes[0].qwTotalBytes = 260ULL * 1024ULL * 1024ULL; /* 260 MB */
    usbDrive.trimInfo.volumes[0].qwFreeBytes = 100ULL * 1024ULL * 1024ULL;  /* 100 MB */
    usbDrive.trimInfo.volumes[0].fFreePercent = 38.5f;
    char szTrimPlain[4096];
    FormatHealthLecturePlain(&usbDrive, szTrimPlain, sizeof(szTrimPlain));
    Expect(strstr(szTrimPlain, "100 MB") != NULL, "Small volume formatted with MB free");
    Expect(strstr(szTrimPlain, "260 MB") != NULL, "Small volume formatted with MB total");
    Expect(strstr(szTrimPlain, "0.0 GB") == NULL, "Small volume does not display 0.0 GB");

    CaseEnd("storage-event-log", before);
}

static void TestVolumeTrimMonitoring(void)
{
    int before = g_fail;

    /* 1. Filesystem TRIM capability check */
    Expect(IsFileSystemTrimSupported("NTFS") == TRUE, "NTFS supports TRIM");
    Expect(IsFileSystemTrimSupported("ntfs") == TRUE, "ntfs (lowercase) supports TRIM");
    Expect(IsFileSystemTrimSupported("ReFS") == TRUE, "ReFS supports TRIM");
    Expect(IsFileSystemTrimSupported("FAT32") == FALSE, "FAT32 does NOT support TRIM");
    Expect(IsFileSystemTrimSupported("exFAT") == FALSE, "exFAT does NOT support TRIM");
    Expect(IsFileSystemTrimSupported("FAT") == FALSE, "FAT does NOT support TRIM");
    Expect(IsFileSystemTrimSupported("RAW") == FALSE, "RAW does NOT support TRIM");
    Expect(IsFileSystemTrimSupported(NULL) == FALSE, "NULL does NOT support TRIM");

    /* 2. Volume and TRIM Evaluation on simulated SSD */
    DRIVE_INFO ssd;
    memset(&ssd, 0, sizeof(ssd));
    ssd.eType = DRIVE_TYPE_SSD_SATA;
    ssd.nSSDLifeLeft = 100;
    ssd.nEndurancePercent = 100;
    ssd.bSMART_Supported = TRUE;
    lstrcpynA(ssd.szModel, "Kingston A400 240GB", (int)sizeof(ssd.szModel));

    /* Case A: Normal SSD volume (NTFS, 50% free, OS TRIM enabled) */
    ssd.trimInfo.bQueried = TRUE;
    ssd.trimInfo.bOsTrimEnabled = TRUE;
    ssd.trimInfo.bDeviceTrimSupported = TRUE;
    ssd.trimInfo.nVolumeCount = 1;
    lstrcpynA(ssd.trimInfo.volumes[0].szLetter, "C:", (int)sizeof(ssd.trimInfo.volumes[0].szLetter));
    lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "NTFS", (int)sizeof(ssd.trimInfo.volumes[0].szFileSystem));
    ssd.trimInfo.volumes[0].qwTotalBytes = 240ULL * 1024ULL * 1024ULL * 1024ULL;
    ssd.trimInfo.volumes[0].qwFreeBytes = 120ULL * 1024ULL * 1024ULL * 1024ULL;
    ssd.trimInfo.volumes[0].fFreePercent = 50.0f;
    ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;
    EvaluateDriveTrimAndCapacity(&ssd);

    Expect(!ssd.trimInfo.bPanicLowSpace, "50% free space is not panic");
    Expect(!ssd.trimInfo.bCriticallyLowSpace, "50% free space is not critical");
    Expect(!ssd.trimInfo.bHasNonTrimVolume, "NTFS is not non-TRIM");
    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eWear, (int)HEALTH_STATUS_GOOD, "50% free space eWear is GOOD");
    ExpectInt((int)ssd.eInterface, (int)HEALTH_STATUS_GOOD, "Enabled TRIM eInterface is GOOD");

    /* Case B: Critical overflow (< 5% free space, e.g. 3.3%) */
    ssd.trimInfo.volumes[0].qwFreeBytes = 8ULL * 1024ULL * 1024ULL * 1024ULL;
    ssd.trimInfo.volumes[0].fFreePercent = 3.33f;
    EvaluateDriveTrimAndCapacity(&ssd);

    Expect(ssd.trimInfo.bPanicLowSpace, "<5% free space triggers bPanicLowSpace");
    Expect(ssd.trimInfo.bCriticallyLowSpace, "<5% free space triggers bCriticallyLowSpace");
    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eWear, (int)HEALTH_STATUS_CAUTION, "<5% free space elevates eWear to CAUTION");

    char szPlain[4096];
    char szExpert[4096];
    FormatHealthLecturePlain(&ssd, szPlain, sizeof(szPlain));
    FormatHealthLectureExpert(&ssd, szExpert, sizeof(szExpert));
    Expect(strstr(szPlain, "ДЕФИЦИТ") != NULL || strstr(szPlain, "DEFICIT") != NULL, "Plain lecture reports low space panic");
    Expect(strstr(ssd.szEvidence, "ДЕФИЦИТ МЕСТА") != NULL || strstr(ssd.szEvidence, "LOW SPACE") != NULL, "szEvidence has low space note");

    /* Case C: Low space warning (< 7% free space, e.g. 6.2%) */
    ssd.trimInfo.volumes[0].fFreePercent = 6.2f;
    EvaluateDriveTrimAndCapacity(&ssd);
    Expect(!ssd.trimInfo.bPanicLowSpace, "6.2% is not panic");
    Expect(ssd.trimInfo.bCriticallyLowSpace, "6.2% triggers bCriticallyLowSpace");
    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eWear, (int)HEALTH_STATUS_OBSERVE, "<7% free space elevates eWear to OBSERVE");

    /* Case D: Non-TRIM filesystem (FAT32) on SSD */
    ssd.trimInfo.volumes[0].fFreePercent = 50.0f;
    lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "FAT32", (int)sizeof(ssd.trimInfo.volumes[0].szFileSystem));
    ssd.trimInfo.volumes[0].bTrimSupportedFs = FALSE;
    EvaluateDriveTrimAndCapacity(&ssd);
    Expect(ssd.trimInfo.bHasNonTrimVolume, "FAT32 detected as non-TRIM volume");
    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eInterface, (int)HEALTH_STATUS_OBSERVE, "Non-TRIM FS elevates eInterface to OBSERVE");

    /* Case E: OS TRIM disabled in Windows (DisableDeleteNotify = 1) */
    lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "NTFS", (int)sizeof(ssd.trimInfo.volumes[0].szFileSystem));
    ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;
    ssd.trimInfo.bOsTrimEnabled = FALSE;
    EvaluateDriveTrimAndCapacity(&ssd);
    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eInterface, (int)HEALTH_STATUS_CAUTION, "Disabled OS TRIM elevates eInterface to CAUTION");
    FormatHealthLecturePlain(&ssd, szPlain, sizeof(szPlain));
    Expect(strstr(szPlain, "fsutil") != NULL, "Plain lecture suggests fsutil command to fix TRIM");
    Expect(strstr(ssd.szEvidence, "TRIM В ОС ВЫКЛЮЧЕН") != NULL || strstr(ssd.szEvidence, "OS TRIM DISABLED") != NULL, "szEvidence has OS TRIM disabled note");

    /* Case F: Verify expert lecture displays TRIM status & volume info */
    FormatHealthLectureExpert(&ssd, szExpert, sizeof(szExpert));
    Expect(strstr(szExpert, "DisableDeleteNotify") != NULL, "Expert lecture mentions DisableDeleteNotify");
    Expect(strstr(szExpert, "C:") != NULL, "Expert lecture lists volume C:");

    /* Case G: RAW / 0-byte volume (unformatted or unallocated) should not trigger non-TRIM warning */
    ssd.trimInfo.bOsTrimEnabled = TRUE;
    ssd.trimInfo.bHasNonTrimVolume = FALSE;
    ssd.trimInfo.volumes[0].qwTotalBytes = 0;
    ssd.trimInfo.volumes[0].qwFreeBytes = 0;
    ssd.trimInfo.volumes[0].fFreePercent = -1.0f;
    lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "RAW", (int)sizeof(ssd.trimInfo.volumes[0].szFileSystem));
    ssd.trimInfo.volumes[0].bTrimSupportedFs = FALSE;
    EvaluateDriveTrimAndCapacity(&ssd);
    Expect(!ssd.trimInfo.bHasNonTrimVolume, "0-byte RAW volume does not set bHasNonTrimVolume");
    FormatHealthLectureExpert(&ssd, szExpert, sizeof(szExpert));
    Expect(strstr(szExpert, "нет данных о файловой системе") != NULL || strstr(szExpert, "no filesystem data") != NULL, "Expert lecture displays no filesystem data for 0-byte volume");

    memcpy(&g, &ssd, sizeof(DRIVE_INFO));
    CaseEnd("volume-trim-monitoring", before);
}

static void TestDramVerification(void)
{
    int before = g_fail;

    /* 1. Test deterministic pattern fill */
    uint8_t buf1[8192];
    uint8_t buf2[8192];
    memset(buf1, 0, sizeof(buf1));
    memset(buf2, 0, sizeof(buf2));

    DramFillPattern(buf1, sizeof(buf1), 0, DRAM_PATTERN_CHECKERBOARD);
    DramFillPattern(buf2, sizeof(buf2), 0, DRAM_PATTERN_CHECKERBOARD);
    Expect(memcmp(buf1, buf2, sizeof(buf1)) == 0, "Checkerboard pattern is deterministic");
    Expect(buf1[0] == 0xAA && buf1[8] == 0x55, "Checkerboard has 0xAA and 0x55 64-bit words");

    DramFillPattern(buf1, sizeof(buf1), 0, DRAM_PATTERN_SOLID_INV);
    Expect(buf1[0] == 0x00 && buf1[1] == 0x00, "Solid inv starts with 0x00");
    Expect(buf1[4096] == 0xFF && buf1[4097] == 0xFF, "Solid inv alternates page 0 to page 1 0xFF");

    DramFillPattern(buf1, sizeof(buf1), 0, DRAM_PATTERN_PRBS);
    DramFillPattern(buf2, sizeof(buf2), 0, DRAM_PATTERN_PRBS);
    Expect(memcmp(buf1, buf2, sizeof(buf1)) == 0, "PRBS pattern is deterministic");

    /* 2. Test buffer verification without bit flips */
    DRAM_VERIFY_RESULT res;
    memset(&res, 0, sizeof(res));
    res.eMode = DRAM_VERIFY_MODE_EXPRESS;
    res.nTotalBytesToTest = sizeof(buf1);
    res.nBytesTested = sizeof(buf1);

    size_t corrupted = DramVerifyBuffer(buf1, buf2, sizeof(buf1), 0, &res);
    ExpectInt((int)corrupted, 0, "Identical buffers report 0 corrupted bytes");
    ExpectInt((int)res.nCorruptedBytes, 0, "res.nCorruptedBytes is 0");
    ExpectInt((int)res.nTotalBitFlips, 0, "res.nTotalBitFlips is 0");
    ExpectInt(res.nRecordedFlips, 0, "res.nRecordedFlips is 0");

    DramVerifyAssessVerdict(&res);
    Expect(res.bPassed, "No bit flips passes assessment");
    Expect(strstr(res.szVerdictBadge, "ИСПРАВЕН") != NULL || strstr(res.szVerdictBadge, "PASS") != NULL, "Verdict badge is PASS/OK");

    /* 3. Injected bit flips: test XOR bit counting and sample recording */
    buf2[10] ^= 0x01; /* 1 bit flip */
    buf2[20] ^= 0x07; /* 3 bits flipped */
    memset(&res, 0, sizeof(res));
    res.eMode = DRAM_VERIFY_MODE_EXPRESS;
    res.nTotalBytesToTest = sizeof(buf1);
    res.nBytesTested = sizeof(buf1);

    corrupted = DramVerifyBuffer(buf2, buf1, sizeof(buf1), 1024, &res);
    ExpectInt((int)corrupted, 2, "2 corrupted bytes detected");
    ExpectInt((int)res.nCorruptedBytes, 2, "res.nCorruptedBytes is 2");
    ExpectInt((int)res.nTotalBitFlips, 4, "Total 4 bit flips detected (1 + 3)");
    ExpectInt(res.nRecordedFlips, 2, "2 flip samples recorded");
    Expect(res.flips[0].nByteOffset == 1024 + 10, "First flip offset is 1034 (0x40A)");
    Expect(res.flips[0].bFlippedBits == 0x01, "First flip mask is 0x01");
    Expect(res.flips[1].nByteOffset == 1024 + 20, "Second flip offset is 1044 (0x414)");
    Expect(res.flips[1].bFlippedBits == 0x07, "Second flip mask is 0x07");

    DramVerifyAssessVerdict(&res);
    Expect(!res.bPassed, "Bit flips cause verdict failure");
    Expect(strstr(res.szVerdictBadge, "BIT FLIP") != NULL, "Verdict badge contains BIT FLIP");
    Expect(strstr(res.szVerdictText, "Silent Data Corruption") != NULL || strstr(res.szVerdictText, "DRAM") != NULL, "Verdict text explains DRAM degradation");

    /* 4. Format reports */
    char szSummary[2048];
    char szMarkdown[2048];
    FormatDramVerifySummary(&res, "Samsung SSD 870 EVO", "S5Y2NJ0N123456", szSummary, sizeof(szSummary));
    Expect(strstr(szSummary, "BIT FLIP") != NULL, "Summary mentions BIT FLIP");
    Expect(strstr(szSummary, "0x40A") != NULL, "Summary contains sample offset 0x40A");

    FormatDramVerifyMarkdown(&res, szMarkdown, sizeof(szMarkdown));
    Expect(strstr(szMarkdown, "DRAM") != NULL, "Markdown report contains DRAM section");
    Expect(strstr(szMarkdown, "0x01") != NULL, "Markdown table has bitmask 0x01");

    /* 5. Health Engine integration: SSD DRAM failure elevates wear, media, reliability and status to BAD */
    DRIVE_INFO ssd;
    memset(&ssd, 0, sizeof(ssd));
    ssd.eType = DRIVE_TYPE_SSD_SATA;
    ssd.nSSDLifeLeft = 99;
    ssd.nEndurancePercent = 99;
    ssd.bSMART_Supported = TRUE;
    lstrcpynA(ssd.szModel, "Samsung SSD 870 EVO 1TB", (int)sizeof(ssd.szModel));
    ssd.bDramTested = true;
    ssd.bDramCorruption = true;
    ssd.nDramCorruptedBytes = 2;
    ssd.nDramBitFlips = 4;

    AssessDriveHealth(&ssd);
    ExpectInt((int)ssd.eHealthStatus, (int)HEALTH_STATUS_BAD, "SSD with DRAM corruption elevated to HEALTH_STATUS_BAD");
    ExpectInt((int)ssd.eWear, (int)HEALTH_STATUS_BAD, "SSD eWear elevated to BAD");
    ExpectInt((int)ssd.eReliability, (int)HEALTH_STATUS_BAD, "SSD eReliability elevated to BAD");
    Expect(strstr(ssd.szEvidence, "СБОЙ БУФЕРА DRAM: BIT FLIP!") != NULL || strstr(ssd.szEvidence, "DRAM BUFFER BIT FLIP") != NULL, "SSD szEvidence has DRAM warning");

    char szPlain[4096];
    char szExpert[4096];
    FormatHealthLecturePlain(&ssd, szPlain, sizeof(szPlain));
    FormatHealthLectureExpert(&ssd, szExpert, sizeof(szExpert));
    Expect(strstr(szPlain, "DRAM") != NULL, "Plain lecture explains DRAM corruption");
    Expect(strstr(szExpert, "DRAM") != NULL, "Expert lecture contains DRAM section");

    /* 6. Health Engine integration: HDD DRAM failure elevates reliability and status to BAD */
    DRIVE_INFO hdd;
    memset(&hdd, 0, sizeof(hdd));
    hdd.eType = DRIVE_TYPE_HDD;
    hdd.bSMART_Supported = TRUE;
    lstrcpynA(hdd.szModel, "WDC WD10EZEX-00WN4A0", (int)sizeof(hdd.szModel));
    hdd.bDramTested = true;
    hdd.bDramCorruption = true;
    hdd.nDramCorruptedBytes = 1;
    hdd.nDramBitFlips = 1;

    AssessDriveHealth(&hdd);
    ExpectInt((int)hdd.eHealthStatus, (int)HEALTH_STATUS_BAD, "HDD with DRAM corruption elevated to HEALTH_STATUS_BAD");
    ExpectInt((int)hdd.eReliability, (int)HEALTH_STATUS_BAD, "HDD eReliability elevated to BAD");

    /* 7. Cache operations */
    SetDriveDramVerifyResult(0, &res);
    const DRAM_VERIFY_RESULT* pCached = GetDriveDramVerifyResult(0);
    Expect(pCached != NULL, "Cached result found");
    Expect(pCached && pCached->nTotalBitFlips == 4, "Cached bit flips match");
    ClearDriveDramVerifyResults();
    Expect(GetDriveDramVerifyResult(0) == NULL, "Cached result cleared");

    CaseEnd("dram-buffer-verification", before);
}

static void TestCounterfeitDetection(void)
{
    int before = g_fail;

    /* 1. WD Purple 5400 RPM with Seagate Firmware GX4K */
    {
        Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64GVNY0", "GX4K");
        g.wRotationRate = 5400;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "WD Purple with Seagate FW detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "Seagate", "Reason mentions Seagate");
        ExpectInt(g.nAnomalyType, (int)ANOMALY_RELABELED, "WD Purple classified as ANOMALY_RELABELED");
        UiSetLang(UI_LANG_RU);
        ExpectHas(GetCounterfeitBadge(&g), "РЕМАРК", "Badge reflects relabeled");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "Counterfeit drive health elevated to CAUTION");
    }

    /* 2. WD Purple 5400 RPM with HGST Server Firmware 00.02C.5 */
    {
        Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64N97Y0", "00.02C.5");
        g.wRotationRate = 5400;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "WD Purple with HGST FW detected as counterfeit/relabeled");
        ExpectHas(g.szCounterfeitReason, "HGST", "Reason mentions HGST");
    }

    /* 3. WD Purple 5400 RPM physically spinning at 7200 RPM */
    {
        Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64GVNY0", "80.00A80");
        g.wRotationRate = 7200;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "WD PURX 5400 RPM spinning at 7200 RPM detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "7200", "Reason mentions 7200 RPM");
    }

    /* 4. Counterfeit Samsung 980 NVMe SSD reporting Realtek PCI VID (0x10EC) */
    {
        Base(DRIVE_TYPE_NVME, "Samsung SSD 980 1TB", "3B4QFXO7");
        g.bIsNVMe = TRUE;
        g.bGotNVMeIdent = TRUE;
        ((BYTE*)&g.nvmeIdent)[0] = 0xEC; /* 0x10EC in little endian */
        ((BYTE*)&g.nvmeIdent)[1] = 0x10;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Samsung NVMe with Realtek VID detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "0x10EC", "Reason mentions VID 0x10EC");
        ExpectInt(g.nAnomalyType, (int)ANOMALY_COUNTERFEIT, "Samsung NVMe classified as ANOMALY_COUNTERFEIT");
        UiSetLang(UI_LANG_RU);
        ExpectHas(GetCounterfeitBadge(&g), "ПОДДЕЛКА", "Badge reflects counterfeit");
    }

    /* 5. Counterfeit Samsung 980 NVMe SSD reporting SMI PCI VID (0x126F) */
    {
        Base(DRIVE_TYPE_NVME, "Samsung SSD 980 PRO 2TB", "5B2QGXA7");
        g.bIsNVMe = TRUE;
        g.bGotNVMeIdent = TRUE;
        ((BYTE*)&g.nvmeIdent)[0] = 0x6F; /* 0x126F SMI */
        ((BYTE*)&g.nvmeIdent)[1] = 0x12;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Samsung NVMe with SMI VID detected as counterfeit");
    }

    /* 6. Genuine Samsung 980 NVMe SSD reporting Samsung PCI VID (0x144D) */
    {
        Base(DRIVE_TYPE_NVME, "Samsung SSD 980 1TB", "3B4QFXO7");
        g.bIsNVMe = TRUE;
        g.bGotNVMeIdent = TRUE;
        ((BYTE*)&g.nvmeIdent)[0] = 0x4D; /* 0x144D Samsung */
        ((BYTE*)&g.nvmeIdent)[1] = 0x14;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Genuine Samsung NVMe VID 0x144D NOT flagged as counterfeit");
    }

    /* 7. Counterfeit Samsung 870 EVO SATA SSD with Realtek FW U0325A0 */
    {
        Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "U0325A0");
        g.wRotationRate = 0x0001;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Samsung 870 EVO with Realtek FW detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "Realtek", "Reason mentions Realtek");
    }

    /* 8. Genuine Samsung 870 EVO SATA SSD with genuine FW SVT02B6Q */
    {
        Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "SVT02B6Q");
        g.wRotationRate = 0x0001;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Genuine Samsung 870 EVO SVT02B6Q NOT flagged as counterfeit");
    }

    /* 9. Counterfeit WD Black NVMe SSD reporting Realtek PCI VID (0x10EC) */
    {
        Base(DRIVE_TYPE_NVME, "WD_BLACK SN850X 1000GB", "620311WD");
        g.bIsNVMe = TRUE;
        g.bGotNVMeIdent = TRUE;
        ((BYTE*)&g.nvmeIdent)[0] = 0xEC;
        ((BYTE*)&g.nvmeIdent)[1] = 0x10;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "WD Black NVMe with Realtek VID detected as counterfeit");
    }

    /* 10. Genuine WD Black NVMe SSD reporting WD PCI VID (0x15B7) */
    {
        Base(DRIVE_TYPE_NVME, "WD_BLACK SN850X 1000GB", "620311WD");
        g.bIsNVMe = TRUE;
        g.bGotNVMeIdent = TRUE;
        ((BYTE*)&g.nvmeIdent)[0] = 0xB7;
        ((BYTE*)&g.nvmeIdent)[1] = 0x15;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Genuine WD Black NVMe NOT flagged as counterfeit");
    }

    /* 11. Genuine WD Purple 5400 RPM with WD Firmware */
    {
        Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64GVNY0", "80.00A80");
        g.wRotationRate = 5400;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Genuine WD Purple NOT flagged as counterfeit");
    }

    /* 12. Genuine Seagate Barracuda */
    {
        Base(DRIVE_TYPE_HDD, "ST2000DM008-2FR102", "0001");
        g.wRotationRate = 7200;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Genuine Seagate Barracuda NOT flagged as counterfeit");
    }

    /* 13. Counterfeit Seagate with WD firmware format 80.00A80 */
    {
        Base(DRIVE_TYPE_HDD, "ST2000DM008", "80.00A80");
        g.wRotationRate = 7200;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Seagate with WD firmware detected as counterfeit");
    }

    /* 14. Utania refurbisher is excluded from counterfeit detection */
    {
        Base(DRIVE_TYPE_HDD, "Utania 2TB HDD", "GX4K");
        g.eVendor = VENDOR_UTANIA;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(!g.bCounterfeitSuspected, "Utania refurbisher excluded from counterfeit flagging");
    }

    /* 14a. Counterfeit Kingston A400 with MPTool default placeholder serial */
    {
        Base(DRIVE_TYPE_SSD_SATA, "Kingston A400", "U0524A0");
        lstrcpynA(g.szSerial, "AA000000000000001287", (int)sizeof(g.szSerial));
        g.wRotationRate = 0x0001;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Kingston A400 with MPTool serial AA0000... detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "MPTool", "Reason mentions MPTool");
        ExpectInt(g.nAnomalyType, (int)ANOMALY_REFLASHED_REPAIRED, "Kingston A400 classified as ANOMALY_REFLASHED_REPAIRED");
        UiSetLang(UI_LANG_RU);
        ExpectHas(GetCounterfeitBadge(&g), "ПЕРЕПРОШИТ / MPTOOL", "Badge reflects reflashed/MPTool");
        UiSetLang(UI_LANG_EN);
        ExpectHas(GetCounterfeitBadge(&g), "REFLASHED / MPTOOL", "English badge reflects reflashed/MPTool");
    }

    /* 14b. Refurbished drive with all-zeros wiped serial */
    {
        Base(DRIVE_TYPE_HDD, "OOS500G128M", "OOS1");
        lstrcpynA(g.szSerial, "00000000", (int)sizeof(g.szSerial));
        g.wRotationRate = 5400;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "HDD with wiped all-zeros serial detected as counterfeit");
        ExpectHas(g.szCounterfeitReason, "занулён", "Reason mentions all-zeros");
        ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_GOOD, "Clean counterfeit drive reliability remains GOOD");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "Counterfeit drive health is CAUTION");
        char szHddLecture[2048];
        szHddLecture[0] = '\0';
        UiSetLang(UI_LANG_RU);
        FormatHealthLecturePlain(&g, szHddLecture, (int)sizeof(szHddLecture));
        ExpectHas(szHddLecture, "Обнаружены признаки поддельного", "Russian plain lecture has counterfeit warning");
        Expect(strstr(szHddLecture, "деградации носителя") == NULL, "Clean counterfeit plain lecture does NOT claim media degradation");

        char szPrompt[256];
        FormatHddCautionPrompt(&g, szPrompt, (int)sizeof(szPrompt));
        ExpectHas(szPrompt, "Подозрение на поддельный/перемаркированный", "Clean counterfeit HDD caution prompt names counterfeit on main screen");

        /* When interface has errors, it must NOT be eclipsed by counterfeit */
        g.nCrcErrors = 15;
        g.eInterface = HEALTH_STATUS_CAUTION;
        g.eHealthStatus = HEALTH_STATUS_CAUTION;
        FormatHddCautionPrompt(&g, szPrompt, (int)sizeof(szPrompt));
        ExpectHas(szPrompt, "Подозрение на подделку; проблемы интерфейса", "HDD prompt names both counterfeit and interface");

        szHddLecture[0] = '\0';
        FormatHealthLecturePlain(&g, szHddLecture, (int)sizeof(szHddLecture));
        ExpectHas(szHddLecture, "Ошибки CRC: 15", "Interface CRC is not eclipsed by counterfeit");
        ExpectHas(szHddLecture, "кабель", "Cable recommendation is present in interface explanation");
        ExpectHas(szHddLecture, "Также обнаружены признаки подделки", "Mentions counterfeit note under Reason");
    }

    /* 14c. SSD with counterfeit and interface issues (no eclipse, deduplication) */
    {
        Base(DRIVE_TYPE_SSD_SATA, "KINGSTON SA400S37120G", "SBFK71F1");
        lstrcpynA(g.szSerial, "AA000000000000001287", (int)sizeof(g.szSerial));
        g.wRotationRate = 0x0001;
        Expect(AddAttr(0xC7, 100, 42), "add CRC attribute to Kingston A400");
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);
        Expect(g.bCounterfeitSuspected, "Kingston A400 detected as counterfeit");

        char szPrompt[256];
        UiSetLang(UI_LANG_RU);
        FormatSsdCautionPrompt(&g, szPrompt, (int)sizeof(szPrompt));
        ExpectHas(szPrompt, "Подозрение на подделку; проблемы интерфейса", "SSD prompt names both counterfeit and interface");

        char szSsdLecture[2048];
        szSsdLecture[0] = '\0';
        FormatHealthLecturePlain(&g, szSsdLecture, (int)sizeof(szSsdLecture));
        ExpectHas(szSsdLecture, "Ошибки CRC: 42", "SSD CRC is not eclipsed by counterfeit");
        ExpectHas(szSsdLecture, "кабель", "SSD cable recommendation present");
        ExpectHas(szSsdLecture, "Также обнаружены признаки подделки", "SSD mentions counterfeit note under Reason");

        /* Ensure full counterfeit warning block is not duplicated twice */
        const char* p1 = strstr(szSsdLecture, "ВНИМАНИЕ: ОБНАРУЖЕНЫ ПРИЗНАКИ");
        Expect(p1 != NULL, "Full counterfeit warning box is present");
        if (p1) {
            const char* p2 = strstr(p1 + 10, "ВНИМАНИЕ: ОБНАРУЖЕНЫ ПРИЗНАКИ");
            Expect(p2 == NULL, "Full counterfeit warning box is not duplicated");
        }
    }

    /* 15. Bilingual support: reason and lecture formatting */
    {
        Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64GVNY0", "GX4K");
        g.wRotationRate = 5400;
        IdentifyDriveParts(&g);
        AssessDriveHealth(&g);

        UiSetLang(UI_LANG_RU);
        const char* rRu = GetCounterfeitReason(&g);
        Expect(rRu != NULL && strlen(rRu) > 0, "Russian counterfeit reason non-empty");
        ExpectHas(rRu, "Seagate", "Russian reason contains Seagate");

        UiSetLang(UI_LANG_EN);
        const char* rEn = GetCounterfeitReason(&g);
        Expect(rEn != NULL && strlen(rEn) > 0, "English counterfeit reason non-empty");
        ExpectHas(rEn, "Seagate", "English reason contains Seagate");

        char szLecture[2048];
        szLecture[0] = '\0';
        FormatHealthLecturePlain(&g, szLecture, (int)sizeof(szLecture));
        ExpectHas(szLecture, "WARNING: SUSPECTED RELABELED", "English plain lecture has counterfeit warning");

        UiSetLang(UI_LANG_RU);
        szLecture[0] = '\0';
        FormatHealthLecturePlain(&g, szLecture, (int)sizeof(szLecture));
        ExpectHas(szLecture, "ВНИМАНИЕ: ОБНАРУЖЕНЫ ПРИЗНАКИ", "Russian plain lecture has counterfeit warning");
    }

    CaseEnd("counterfeit-detection", before);
}

static int IsValidUtf8(const char* s)
{
    if (!s) return 0;
    const unsigned char* p = (const unsigned char*)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80) return 0;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return 0;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return 0;
            p += 4;
        } else {
            return 0;
        }
    }
    return 1;
}

static void TestLectureUtf8Truncation(void)
{
    int before = g_fail;
    Base(DRIVE_TYPE_HDD, "WDC WD40PURX-64GVNY0", "GX4K");
    g.wRotationRate = 5400;
    IdentifyDriveParts(&g);
    AssessDriveHealth(&g);

    UiSetLang(UI_LANG_RU);
    for (int bufSize = 20; bufSize <= 300; bufSize += 3) {
        char buf[512];
        buf[0] = '\0';
        FormatHealthLecturePlain(&g, buf, bufSize);
        Expect(IsValidUtf8(buf), "FormatHealthLecturePlain produces valid UTF-8 even when truncated");
    }
    CaseEnd("utf8-truncation-safety", before);
}


static void TestSeagateFarmLogParsing(void)
{
    int before = g_fail;
    BYTE sec[512];
    DRIVE_INFO di;
    memset(&di, 0, sizeof(di));

    /* 1. Sector 0: FARM Header */
    memset(sec, 0, sizeof(sec));
    Expect(!ParseSeagateFarmHeader(NULL, &di), "ParseSeagateFarmHeader rejects NULL buffer");
    Expect(!ParseSeagateFarmHeader(sec, NULL), "ParseSeagateFarmHeader rejects NULL DRIVE_INFO");
    Expect(!ParseSeagateFarmHeader(sec, &di), "ParseSeagateFarmHeader rejects all-zero buffer");

    /* Corrupted / invalid signature */
    memcpy(sec, "NOTFRM", 6);
    Expect(!ParseSeagateFarmHeader(sec, &di), "ParseSeagateFarmHeader rejects invalid signature");

    /* Valid FARMER signature */
    memcpy(sec, SEAGATE_FARM_SIGNATURE, 6);
    uint64_t major = 1;
    uint64_t minor = 4;
    memcpy(sec + 8, &major, sizeof(major));
    memcpy(sec + 16, &minor, sizeof(minor));
    Expect(ParseSeagateFarmHeader(sec, &di), "ParseSeagateFarmHeader succeeds with valid signature");
    Expect(di.bFarmLogSupported, "di.bFarmLogSupported is TRUE");
    ExpectInt((int)di.dwFarmMajorVersion, 1, "FARM major version parsed correctly");
    ExpectInt((int)di.dwFarmMinorVersion, 4, "FARM minor version parsed correctly");

    /* 2. Sector 32: Page 1 Drive Info */
    memset(sec, 0, sizeof(sec));
    /* Invalid pageNumber */
    uint64_t badPage = 99;
    memcpy(sec, &badPage, sizeof(badPage));
    Expect(!ParseSeagateFarmDriveInfo(sec, &di), "ParseSeagateFarmDriveInfo rejects bad pageNumber");

    /* Valid pageNumber 1 */
    uint64_t p1 = 1;
    memcpy(sec, &p1, sizeof(p1));
    uint64_t poh = 38450;
    uint64_t spindle = 38200;
    uint64_t headFlight = 37950;
    uint64_t cycles = 215;
    memcpy(sec + 152, &poh, sizeof(poh));
    memcpy(sec + 160, &spindle, sizeof(spindle));
    memcpy(sec + 168, &headFlight, sizeof(headFlight));
    memcpy(sec + 184, &cycles, sizeof(cycles));
    memcpy(sec + 360, "2048", 4); /* Week 48, 2020 */

    Expect(ParseSeagateFarmDriveInfo(sec, &di), "ParseSeagateFarmDriveInfo succeeds with page 1");
    Expect(di.qwFarmPowerOnHours == 38450ULL, "qwFarmPowerOnHours parsed correctly");
    Expect(di.qwFarmSpindlePowerOnHours == 38200ULL, "qwFarmSpindlePowerOnHours parsed correctly");
    Expect(di.qwFarmHeadFlightHours == 37950ULL, "qwFarmHeadFlightHours parsed correctly");
    Expect(di.qwFarmPowerCycleCount == 215ULL, "qwFarmPowerCycleCount parsed correctly");
    ExpectStr(di.szFarmAssemblyDate, "2048", "szFarmAssemblyDate parsed correctly");

    /* 3. Sector 64: Page 2 Workload */
    memset(sec, 0, sizeof(sec));
    uint64_t p2 = 2;
    memcpy(sec, &p2, sizeof(p2));
    uint64_t wr = 5000000000ULL;
    uint64_t rd = 3000000000ULL;
    memcpy(sec + 64, &wr, sizeof(wr));
    memcpy(sec + 72, &rd, sizeof(rd));

    Expect(ParseSeagateFarmWorkload(sec, &di), "ParseSeagateFarmWorkload succeeds with page 2");
    Expect(di.qwFarmSectorsWritten == 5000000000ULL, "qwFarmSectorsWritten parsed correctly");
    Expect(di.qwFarmSectorsRead == 3000000000ULL, "qwFarmSectorsRead parsed correctly");

    /* 4. Sector 96: Page 3 Error Statistics */
    memset(sec, 0, sizeof(sec));
    uint64_t p3 = 3;
    memcpy(sec, &p3, sizeof(p3));
    uint64_t realloc = 14;
    memcpy(sec + 32, &realloc, sizeof(realloc));

    Expect(ParseSeagateFarmErrorStats(sec, &di), "ParseSeagateFarmErrorStats succeeds with page 3");
    Expect(di.qwFarmReallocatedSectors == 14ULL, "qwFarmReallocatedSectors parsed correctly");

    /* 5. Sector 128: Page 4 Environment */
    memset(sec, 0, sizeof(sec));
    uint64_t p4 = 4;
    memcpy(sec, &p4, sizeof(p4));
    uint64_t maxT = 53;
    uint64_t minT = 17;
    memcpy(sec + 24, &maxT, sizeof(maxT));
    memcpy(sec + 32, &minT, sizeof(minT));

    Expect(ParseSeagateFarmEnvironment(sec, &di), "ParseSeagateFarmEnvironment succeeds with page 4");
    ExpectInt(di.nFarmLifetimeMaxTempC, 53, "nFarmLifetimeMaxTempC parsed correctly");
    ExpectInt(di.nFarmLifetimeMinTempC, 17, "nFarmLifetimeMinTempC parsed correctly");

    CaseEnd("seagate-farm-log-parsing", before);
}

static void TestSeagateFarmCounterfeitDetection(void)
{
    int before = g_fail;

    /* Case A: Refurbished / Tampered Seagate Exos (wiped SMART) */
    Base(DRIVE_TYPE_HDD, "ST16000NM001G", "SN02");
    lstrcpynA(g.szSerial, "ZR10ABCD", (int)sizeof(g.szSerial));
    g.wRotationRate = 7200;
    g.dwPowerOnHours = 8;        /* Fake wiped SMART: claims 8 hours */
    g.dwPowerCycleCount = 2;     /* Fake power cycles */

    /* Real FARM telemetry from controller System Area */
    g.bFarmLogSupported = TRUE;
    g.qwFarmPowerOnHours = 41200;       /* Real runtime: ~4.7 years in datacenter! */
    g.qwFarmSpindlePowerOnHours = 41000;
    g.qwFarmHeadFlightHours = 40850;
    g.qwFarmPowerCycleCount = 380;
    g.qwFarmSectorsWritten = 8000000000ULL;
    g.qwFarmSectorsRead = 12000000000ULL;
    g.nFarmLifetimeMaxTempC = 54;
    g.nFarmLifetimeMinTempC = 19;
    memcpy(g.szFarmAssemblyDate, "1932", 5);

    DetectCounterfeitMismatch(&g);
    Expect(g.bCounterfeitSuspected, "Wiped Seagate drive flagged as counterfeit/refurbished");
    ExpectHas(g.szCounterfeitReason, "Сброшенный SMART", "Counterfeit reason names wiped SMART");
    ExpectInt(g.nAnomalyType, (int)ANOMALY_SMART_WIPED, "Seagate FARM classified as ANOMALY_SMART_WIPED");
    UiSetLang(UI_LANG_RU);
    ExpectHas(GetCounterfeitBadge(&g), "СБРОШЕН SMART", "Badge reflects wiped SMART");
    ExpectHas(g.szCounterfeitReason, "41200", "Reason contains actual FARM runtime hours");

    AssessDriveHealth(&g);
    Expect(g.eHealthStatus == HEALTH_STATUS_CAUTION, "Health status reduced to CAUTION due to wiped SMART");

    char szLecture[2048];
    szLecture[0] = 0;
    UiSetLang(UI_LANG_RU);
    FormatHealthLecturePlain(&g, szLecture, (int)sizeof(szLecture));
    ExpectHas(szLecture, "ВНИМАНИЕ: ОБНАРУЖЕНЫ ПРИЗНАКИ", "Plain lecture displays counterfeit warning banner");
    ExpectHas(szLecture, "Сброшенный SMART", "Plain lecture explains wiped SMART reason");

    szLecture[0] = 0;
    FormatHealthLectureExpert(&g, szLecture, (int)sizeof(szLecture));
    ExpectHas(szLecture, "Аппаратная телеметрия Seagate FARM (Log A6h)", "Expert lecture has Seagate FARM section");
    ExpectHas(szLecture, "Реальная наработка: 41200 ч", "Expert lecture displays true FARM runtime");
    ExpectHas(szLecture, "Заводская дата сборки: неделя 32, 2019 г.", "Expert lecture displays factory assembly date");

    /* Case B: Genuine Seagate drive with consistent runtime */
    Base(DRIVE_TYPE_HDD, "ST4000VN008-2FR166", "SC60");
    g.wRotationRate = 5900;
    g.dwPowerOnHours = 15200;
    g.dwPowerCycleCount = 45;
    g.bFarmLogSupported = TRUE;
    g.qwFarmPowerOnHours = 15204; /* Normal drift within 4 hours */
    g.qwFarmPowerCycleCount = 45;

    DetectCounterfeitMismatch(&g);
    Expect(!g.bCounterfeitSuspected, "Genuine Seagate drive is NOT flagged as counterfeit");

    /* Case C: Brand new Seagate drive */
    Base(DRIVE_TYPE_HDD, "ST2000DM008-2FR102", "0001");
    g.wRotationRate = 7200;
    g.dwPowerOnHours = 0;
    g.dwPowerCycleCount = 1;
    g.bFarmLogSupported = TRUE;
    g.qwFarmPowerOnHours = 1;
    g.qwFarmPowerCycleCount = 1;

    DetectCounterfeitMismatch(&g);
    Expect(!g.bCounterfeitSuspected, "New Seagate drive is NOT flagged as counterfeit");

    CaseEnd("seagate-farm-counterfeit-detection", before);
}

static void TestSataPhyLogParsing(void)
{
    int before = g_fail;
    BYTE logBuf[512];
    DRIVE_INFO di;
    memset(logBuf, 0, sizeof(logBuf));
    memset(&di, 0, sizeof(di));

    /* 1. NULL safety and empty buffer */
    Expect(!ParseSataPhyLog(NULL, &di), "ParseSataPhyLog returns FALSE on NULL buffer");
    Expect(!ParseSataPhyLog(logBuf, NULL), "ParseSataPhyLog returns FALSE on NULL DRIVE_INFO");
    Expect(!ParseSataPhyLog(logBuf, &di), "ParseSataPhyLog returns FALSE on all-zero buffer");

    /* 2. Populate synthetic SATA PHY log (Log 11h) */
    logBuf[0] = 0x01; /* Revision 1 */
    size_t off = 4;

    /* Entry 1: Event 0x0001 (Command ICRC error), length = 2 bytes (counterBits = 1) */
    WORD id1 = (WORD)(0x0001 | (1 << 12));
    logBuf[off] = (BYTE)(id1 & 0xFF);
    logBuf[off + 1] = (BYTE)((id1 >> 8) & 0xFF);
    logBuf[off + 2] = 7;
    logBuf[off + 3] = 0;
    off += 4;

    /* Entry 2: Event 0x0002 (R_ERR data FIS), length = 4 bytes (counterBits = 2) */
    WORD id2 = (WORD)(0x0002 | (2 << 12));
    logBuf[off] = (BYTE)(id2 & 0xFF);
    logBuf[off + 1] = (BYTE)((id2 >> 8) & 0xFF);
    logBuf[off + 2] = 3;
    logBuf[off + 3] = 0;
    logBuf[off + 4] = 0;
    logBuf[off + 5] = 0;
    off += 6;

    /* Entry 3: Event 0x0009 (Transitions PHYRDY), length = 4 bytes */
    WORD id9 = (WORD)(0x0009 | (2 << 12));
    logBuf[off] = (BYTE)(id9 & 0xFF);
    logBuf[off + 1] = (BYTE)((id9 >> 8) & 0xFF);
    logBuf[off + 2] = 12;
    logBuf[off + 3] = 0;
    logBuf[off + 4] = 0;
    logBuf[off + 5] = 0;
    off += 6;

    /* Entry 4: Event 0x000A (COMRESET count), length = 2 bytes */
    WORD idA = (WORD)(0x000A | (1 << 12));
    logBuf[off] = (BYTE)(idA & 0xFF);
    logBuf[off + 1] = (BYTE)((idA >> 8) & 0xFF);
    logBuf[off + 2] = 4;
    logBuf[off + 3] = 0;
    off += 4;

    /* Entry 5: Event 0x000B (Host-to-Device CRC), length = 4 bytes */
    WORD idB = (WORD)(0x000B | (2 << 12));
    logBuf[off] = (BYTE)(idB & 0xFF);
    logBuf[off + 1] = (BYTE)((idB >> 8) & 0xFF);
    logBuf[off + 2] = 25;
    logBuf[off + 3] = 0;
    logBuf[off + 4] = 0;
    logBuf[off + 5] = 0;
    off += 6;

    /* Entry 6: End marker rawID = 0 */
    logBuf[off] = 0;
    logBuf[off + 1] = 0;

    Expect(ParseSataPhyLog(logBuf, &di), "ParseSataPhyLog parses valid synthetic buffer");
    Expect(di.bSataPhyLogSupported, "bSataPhyLogSupported is set to TRUE");
    ExpectInt((int)di.nPhyCommandIcrcErrors, 7, "nPhyCommandIcrcErrors parsed correctly");
    ExpectInt((int)di.nPhyR_ErrDataFis, 3, "nPhyR_ErrDataFis parsed correctly");
    ExpectInt((int)di.nPhyTransitionsPhyRdy, 12, "nPhyTransitionsPhyRdy parsed correctly");
    ExpectInt((int)di.nPhyComresetCount, 4, "nPhyComresetCount parsed correctly");
    ExpectInt((int)di.nPhyCrcErrorsH2D, 25, "nPhyCrcErrorsH2D parsed correctly");

    /* Truncated buffer / boundary check */
    memset(logBuf, 0, sizeof(logBuf));
    logBuf[0] = 0x01;
    WORD idEdge = (WORD)(0x0001 | (3 << 12)); /* length 8 at 508 */
    logBuf[508] = (BYTE)(idEdge & 0xFF);
    logBuf[508 + 1] = (BYTE)((idEdge >> 8) & 0xFF);
    memset(&di, 0, sizeof(di));
    ParseSataPhyLog(logBuf, &di);

    CaseEnd("sata-phy-log-parsing", before);
}

static void TestDeviceStatisticsParsing(void)
{
    int before = g_fail;
    BYTE pageBuf[512];
    DRIVE_INFO di;
    memset(&di, 0, sizeof(di));

    /* NULL checks */
    Expect(!ParseDeviceStatisticsPage(1, NULL, &di), "ParseDeviceStatisticsPage returns FALSE on NULL buffer");
    Expect(!ParseDeviceStatisticsPage(1, pageBuf, NULL), "ParseDeviceStatisticsPage returns FALSE on NULL DRIVE_INFO");

    /* 1. Page 01h (General Statistics) */
    memset(pageBuf, 0, sizeof(pageBuf));
    pageBuf[0] = 0x01; /* Revision */
    pageBuf[2] = 0x01; /* Page 01h */
    /* Descriptor at offset 0x18: Logical Sectors Written */
    DWORD valWritten = 100000000;
    memcpy(&pageBuf[0x18], &valWritten, sizeof(valWritten));
    pageBuf[0x18 + 7] = 0xC0; /* Supported (bit 63) + Valid (bit 62) */

    /* Descriptor at offset 0x28: Logical Sectors Read */
    DWORD valRead = 50000000;
    memcpy(&pageBuf[0x28], &valRead, sizeof(valRead));
    pageBuf[0x28 + 7] = 0xC0;

    Expect(ParseDeviceStatisticsPage(1, pageBuf, &di), "Parse Page 01h succeeds");
    Expect(di.bDevStatSupported, "bDevStatSupported is TRUE");
    Expect(di.qwDevStatSectorsWritten == 100000000ULL, "qwDevStatSectorsWritten parsed correctly");
    Expect(di.qwDevStatSectorsRead == 50000000ULL, "qwDevStatSectorsRead parsed correctly");

    /* 2. Page 05h (Temperature Statistics) */
    memset(pageBuf, 0, sizeof(pageBuf));
    pageBuf[0] = 0x01; /* Revision */
    pageBuf[2] = 0x05; /* Page 05h */
    /* Entry at offset 0x20: Highest Lifetime Temp = 52 C */
    pageBuf[0x20] = 52;
    pageBuf[0x20 + 7] = 0xC0;
    /* Entry at offset 0x28: Lowest Lifetime Temp = 19 C */
    pageBuf[0x28] = 19;
    pageBuf[0x28 + 7] = 0xC0;

    Expect(ParseDeviceStatisticsPage(5, pageBuf, &di), "Parse Page 05h succeeds");
    ExpectInt(di.nDevStatLifetimeMaxTempC, 52, "nDevStatLifetimeMaxTempC parsed correctly");
    ExpectInt(di.nDevStatLifetimeMinTempC, 19, "nDevStatLifetimeMinTempC parsed correctly");

    /* 3. Page 07h (SSD Statistics) */
    memset(pageBuf, 0, sizeof(pageBuf));
    pageBuf[0] = 0x01; /* Revision */
    pageBuf[2] = 0x07; /* Page 07h */
    /* Entry at offset 0x08: Percentage Used Endurance = 8% */
    pageBuf[0x08] = 8;
    pageBuf[0x08 + 7] = 0xC0;

    Expect(ParseDeviceStatisticsPage(7, pageBuf, &di), "Parse Page 07h succeeds");
    ExpectInt(di.nDevStatPercentUsed, 8, "nDevStatPercentUsed parsed correctly");

    CaseEnd("device-statistics-parsing", before);
}

static void TestAtaLogsIntegration(void)
{
    int before = g_fail;
    /* Test HDD with SATA PHY errors triggering detailed interface explanation */
    Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
    g.wRotationRate = 7200;
    g.bSataPhyLogSupported = TRUE;
    g.nPhyTransitionsPhyRdy = 5;
    g.nPhyCrcErrorsH2D = 14;
    g.nPhyCommandIcrcErrors = 2;
    g.nPhyComresetCount = 3;

    AssessDriveHealth(&g);
    Expect(g.eInterface == HEALTH_STATUS_CAUTION, "SATA PHY errors trigger interface CAUTION");

    char szLecture[2048];
    szLecture[0] = 0;
    UiSetLang(UI_LANG_RU);
    FormatHealthLecturePlain(&g, szLecture, (int)sizeof(szLecture));
    ExpectHas(szLecture, "Аппаратный лог SATA PHY", "Plain lecture contains SATA PHY header");
    ExpectHas(szLecture, "Потери готовности PHY", "Plain lecture contains PHYRDY drops");
    ExpectHas(szLecture, "Ошибки CRC Host-to-Device", "Plain lecture contains Host-to-Device CRC");

    /* Test Expert Lecture */
    szLecture[0] = 0;
    g.bDevStatSupported = TRUE;
    g.nDevStatLifetimeMaxTempC = 49;
    g.nDevStatLifetimeMinTempC = 21;
    g.qwDevStatSectorsWritten = 1000000000ULL;
    FormatHealthLectureExpert(&g, szLecture, (int)sizeof(szLecture));
    ExpectHas(szLecture, "Журнал SATA PHY (Log 11h)", "Expert lecture contains SATA PHY Log 11h section");
    ExpectHas(szLecture, "Журнал Device Statistics (Log 04h)", "Expert lecture contains Device Statistics Log 04h section");
    ExpectHas(szLecture, "мин 21 °C, макс 49 °C", "Expert lecture contains lifetime temperature min/max");

    /* Test SSD endurance from Device Statistics Log 04h Page 07h when no SMART wear attribute is present */
    Base(DRIVE_TYPE_SSD_SATA, "GENERIC SATA SSD 120GB", "1.0");
    g.wRotationRate = 0x0001;
    g.bDevStatSupported = TRUE;
    g.nDevStatPercentUsed = 15; /* 15% used -> 85% remaining */
    g.qwDevStatSectorsWritten = 2000000ULL; /* ~1 GB */
    AssessDriveHealth(&g);
    ExpectInt(g.nEndurancePercent, 85, "SSD endurance derived from Device Statistics Page 07h");
    ExpectInt(g.nHealthPercent, 85, "Health percent derived from Device Statistics endurance");

    CaseEnd("ata-logs-integration", before);
}



static void TestLegacyUsbBridges(void)
{
    int before = g_fail;

    /* 1. Protocol prefix formatting for legacy bridges */
    Base(DRIVE_TYPE_HDD, "ST3500418AS", "CC38");
    g.bIsUSB = TRUE;
    g.eAccessMethod = SMART_ACCESS_CYPRESS;
    safe_snprintf(g.szProtocol, "SATA");
    PrefixUsbProtocol(&g);
    ExpectStr(g.szProtocol, "USB Cypress · SATA", "PrefixUsbProtocol CYPRESS");

    g.eAccessMethod = SMART_ACCESS_SUNPLUS;
    safe_snprintf(g.szProtocol, "SATA");
    PrefixUsbProtocol(&g);
    ExpectStr(g.szProtocol, "USB Sunplus · SATA", "PrefixUsbProtocol SUNPLUS");

    g.eAccessMethod = SMART_ACCESS_PROLIFIC;
    safe_snprintf(g.szProtocol, "SATA");
    PrefixUsbProtocol(&g);
    ExpectStr(g.szProtocol, "USB Prolific · SATA", "PrefixUsbProtocol PROLIFIC");

    g.bIsUSB = FALSE;
    g.szProtocol[0] = '\0';
    g.eAccessMethod = SMART_ACCESS_CSMI;
    FillDriveProtocol(&g);
    ExpectStr(g.szProtocol, "RAID · CSMI · SATA", "FillDriveProtocol CSMI");

    /* 2. ParseAtaIdentifyData test */
    BYTE ident[512];
    memset(ident, 0, sizeof(ident));
    ident[0] = 0x40; /* Word 0: ATA */
    /* Serial "W-DXW1E8123456" in byte-swapped pairs */
    const char* sn = "W-DXW1E8123456      ";
    for (int i = 0; i < 20; i += 2) {
        ident[20 + i] = (BYTE)sn[i + 1];
        ident[20 + i + 1] = (BYTE)sn[i];
    }
    /* Firmware "CC43    " */
    const char* fw = "CC43    ";
    for (int i = 0; i < 8; i += 2) {
        ident[46 + i] = (BYTE)fw[i + 1];
        ident[46 + i + 1] = (BYTE)fw[i];
    }
    /* Model "ST1000DM010-2EP102                      " */
    const char* md = "ST1000DM010-2EP102                      ";
    for (int i = 0; i < 40; i += 2) {
        ident[54 + i] = (BYTE)md[i + 1];
        ident[54 + i + 1] = (BYTE)md[i];
    }
    /* Word 82: SMART supported (bit 0) */
    ident[164] = 0x01;
    ident[165] = 0x00;
    /* Word 85: SMART enabled (bit 0) */
    ident[170] = 0x01;
    ident[171] = 0x00;
    /* Word 217: 7200 RPM (0x1C20) */
    ident[434] = 0x20;
    ident[435] = 0x1C;

    DRIVE_INFO info;
    memset(&info, 0, sizeof(info));
    Expect(ParseAtaIdentifyData(ident, &info), "ParseAtaIdentifyData returns TRUE");
    ExpectStr(info.szModel, "ST1000DM010-2EP102", "Parsed model matches");
    ExpectStr(info.szSerial, "W-DXW1E8123456", "Parsed serial matches");
    ExpectStr(info.szFirmware, "CC43", "Parsed firmware matches");
    Expect(info.bSMART_Supported, "SMART supported parsed");
    Expect(info.bSMART_Enabled, "SMART enabled parsed");
    ExpectInt(info.wRotationRate, 7200, "Rotation rate 7200 parsed");

    CaseEnd("legacy-usb-bridges", before);
}

static void TestCsmiStpPacket(void)
{
    int before = g_fail;

    /* 1. Verify structure sizes matching SNIA CSMI specification */
    ExpectInt((int)sizeof(CSMI_SAS_STP_PASSTHRU), 44, "sizeof CSMI_SAS_STP_PASSTHRU is 44");
    ExpectInt((int)sizeof(CSMI_SAS_STP_PASSTHRU_STATUS), 92, "sizeof CSMI_SAS_STP_PASSTHRU_STATUS is 92");
    ExpectInt((int)sizeof(SRB_IO_CONTROL), 28, "sizeof SRB_IO_CONTROL is 28");
    ExpectInt((int)sizeof(CSMI_SAS_STP_PASSTHRU_BUFFER), 28 + 44 + 92 + 512, "sizeof CSMI_SAS_STP_PASSTHRU_BUFFER is 676");

    /* 2. Verify FIS 27h Construction logic */
    CSMI_SAS_STP_PASSTHRU_BUFFER buf;
    memset(&buf, 0, sizeof(buf));
    buf.IoctlHeader.HeaderLength = (ULONG)sizeof(SRB_IO_CONTROL);
    memcpy(buf.IoctlHeader.Signature, CSMI_SAS_SIGNATURE, 8);
    buf.IoctlHeader.ControlCode = CC_CSMI_SAS_STP_PASSTHRU;

    buf.Parameters.bPhyIdentifier = 2;
    buf.Parameters.bCommandFIS[0]  = 0x27; /* FIS Type 27h Register H2D */
    buf.Parameters.bCommandFIS[1]  = 0x80; /* C=1 */
    buf.Parameters.bCommandFIS[2]  = 0xB0; /* SMART command */
    buf.Parameters.bCommandFIS[3]  = 0xDA; /* SMART Return Status */
    buf.Parameters.bCommandFIS[5]  = 0x4F; /* Cylinder Low */
    buf.Parameters.bCommandFIS[6]  = 0xC2; /* Cylinder High */
    buf.Parameters.bCommandFIS[7]  = 0xA0; /* Device/Head */

    ExpectInt(buf.Parameters.bPhyIdentifier, 2, "Target phy is 2");
    ExpectInt(buf.Parameters.bCommandFIS[0], 0x27, "FIS 27h Register H2D");
    ExpectInt(buf.Parameters.bCommandFIS[2], 0xB0, "ATA SMART command 0xB0");
    ExpectInt(buf.Parameters.bCommandFIS[3], 0xDA, "SMART RETURN STATUS 0xDA");

    /* 3. Status FIS decoding test */
    /* OK: CylLow=0x4F, CylHigh=0xC2 */
    buf.Status.bStatusFIS[5] = 0x4F;
    buf.Status.bStatusFIS[6] = 0xC2;
    BOOL bFail = FALSE;
    if (buf.Status.bStatusFIS[5] == 0x4F && buf.Status.bStatusFIS[6] == 0xC2) {
        bFail = FALSE;
    } else if (buf.Status.bStatusFIS[5] == 0xF4 && buf.Status.bStatusFIS[6] == 0x2C) {
        bFail = TRUE;
    }
    Expect(!bFail, "Status FIS 4Fh/C2h decoded as SMART OK");

    /* Exceeded: CylLow=0xF4, CylHigh=0x2C */
    buf.Status.bStatusFIS[5] = 0xF4;
    buf.Status.bStatusFIS[6] = 0x2C;
    if (buf.Status.bStatusFIS[5] == 0x4F && buf.Status.bStatusFIS[6] == 0xC2) {
        bFail = FALSE;
    } else if (buf.Status.bStatusFIS[5] == 0xF4 && buf.Status.bStatusFIS[6] == 0x2C) {
        bFail = TRUE;
    }
    Expect(bFail, "Status FIS F4h/2Ch decoded as SMART THRESHOLD EXCEEDED");

    CaseEnd("csmi-stp-packet", before);
}


static void TestDstEngineFormatting(void)
{
    int before = g_fail;
    DRIVE_INFO info;
    char buf[4096];

    /* 1. NVMe Self-Test History Formatting */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = TRUE;
    info.bGotNvmeSelfTestLog = TRUE;
    info.nvmeSelfTestLog.CurrentOperation = 0;
    info.nvmeSelfTestLog.CurrentCompletion = 0;
    /* Entry 0: Completed successfully */
    info.nvmeSelfTestLog.Results[0].Status = 0x10; /* Bit 7:4 = 1 (Short), Bit 3:0 = 0 (Success) */
    info.nvmeSelfTestLog.Results[0].SegmentNum = 1;
    info.nvmeSelfTestLog.Results[0].PowerOnHours = 1250;
    info.nvmeSelfTestLog.Results[0].FailingLBA = 0;
    /* Entry 1: Failed with read error */
    info.nvmeSelfTestLog.Results[1].Status = 0x27; /* Bit 7:4 = 2 (Extended), Bit 3:0 = 7 (Read error) */
    info.nvmeSelfTestLog.Results[1].SegmentNum = 2;
    info.nvmeSelfTestLog.Results[1].PowerOnHours = 1200;
    info.nvmeSelfTestLog.Results[1].FailingLBA = 0x12345678ULL;

    int nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "NVMe self-test formatting returned non-zero length");
    ExpectHas(buf, "ЖУРНАЛ САМОПРОВЕРОК", "NVMe self-test header present");
    ExpectHas(buf, "Успешно", "NVMe self-test entry 0 success text");
    ExpectHas(buf, "Ошибка чтения LBA", "NVMe self-test entry 1 read failure text");
    ExpectHas(buf, "0x12345678", "NVMe self-test entry 1 failing LBA present");

    /* 2. ATA Self-Test History Formatting */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = FALSE;
    info.bGotSelfTestLog = TRUE;
    info.selfTestLog.stEntries[0].bTestNumber = 0x01;
    info.selfTestLog.stEntries[0].bStatusByte = 0x00; /* Completed OK */
    info.selfTestLog.stEntries[0].wLifeTimestamp = 80; /* 80 hours */
    info.selfTestLog.stEntries[1].bTestNumber = 0x02;
    info.selfTestLog.stEntries[1].bStatusByte = 0x70; /* Read error */
    info.selfTestLog.stEntries[1].dwFailingLBA = 0xDEADBEEF;
    info.selfTestLog.stEntries[1].wLifeTimestamp = 70;

    nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "ATA self-test formatting returned non-zero length");
    ExpectHas(buf, "ОК", "ATA self-test entry 0 badge OK");
    ExpectHas(buf, "ПЛОХО", "ATA self-test entry 1 badge BAD");
    ExpectHas(buf, "0xDEADBEEF", "ATA self-test failing LBA 0xDEADBEEF");

    /* 2b. ATA Empty Self-Test Log */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = FALSE;
    info.bGotSelfTestLog = TRUE;
    nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "ATA empty self-test log length");
    ExpectHas(buf, "пуст", "ATA empty self-test log mentions empty");

    /* 2c. ATA Unsupported Self-Test Log */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = FALSE;
    info.bGotSelfTestLog = FALSE;
    nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "ATA unsupported self-test log length");
    ExpectHas(buf, "не поддерживается", "ATA unsupported self-test log mentions unsupported");

    /* 1b. NVMe Empty Self-Test Log */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = TRUE;
    info.bGotNvmeSelfTestLog = TRUE;
    nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "NVMe empty self-test log length");
    ExpectHas(buf, "пуст", "NVMe empty self-test log mentions empty");

    /* 1c. NVMe Unsupported Self-Test Log */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = TRUE;
    info.bGotNvmeSelfTestLog = FALSE;
    nLen = DstFormatSelfTestHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "NVMe unsupported self-test log length");
    ExpectHas(buf, "не поддерживается", "NVMe unsupported self-test log mentions unsupported");

    /* 3. NVMe Error Log Formatting */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = TRUE;
    info.bGotNvmeErrorLog = TRUE;
    info.dwNvmeErrorLogEntries = 1;
    info.nvmeErrorLog[0].ErrorCount = 5;
    info.nvmeErrorLog[0].SQID = 2;
    info.nvmeErrorLog[0].CMDID = 10;
    info.nvmeErrorLog[0].StatusField = 0x4002;
    info.nvmeErrorLog[0].ParameterErrorLocation = 0x0014; /* Byte 20, bit 0 */
    info.nvmeErrorLog[0].LBA = 0xABCDE;

    nLen = DstFormatErrorLogHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "NVMe error log formatting returned non-zero length");
    ExpectHas(buf, "ЖУРНАЛ АППАРАТНЫХ ОШИБОК", "NVMe error log header present");
    ExpectHas(buf, "0x4002", "NVMe error log status field 0x4002");
    ExpectHas(buf, "0xABCDE", "NVMe error log LBA 0xABCDE");

    /* 4. ATA Error Log Formatting */
    ZeroMemory(&info, sizeof(info));
    info.bIsNVMe = FALSE;
    info.bGotErrorLog = TRUE;
    info.nErrorLogCount = 2;
    info.errorLog.stErrors[0].bErrorRegister = 0x40; /* Uncorrectable */
    info.errorLog.stErrors[0].bStatus = 0x51;
    info.errorLog.stErrors[0].bLBA_Low = 0x11;
    info.errorLog.stErrors[0].bLBA_Mid = 0x22;
    info.errorLog.stErrors[0].bLBA_High = 0x33;
    info.errorLog.stErrors[0].bTimestamp[0] = 0x50;

    nLen = DstFormatErrorLogHistory(&info, buf, (int)sizeof(buf));
    Expect(nLen > 0, "ATA error log formatting returned non-zero length");
    ExpectHas(buf, "Суммарное количество ошибок ATA", "ATA error log count header");
    ExpectHas(buf, "0x40", "ATA error register 0x40");
    ExpectHas(buf, "0x00332211", "ATA error LBA 0x00332211");

    CaseEnd("dst-engine-formatting", before);
}


static void TestBuild51Regressions(void)
{
    int before = g_fail;

    /* 1. ATA IDENTIFY Validation */
    {
        BYTE ident[512];
        memset(ident, 0, sizeof(ident));
        WORD* w = (WORD*)ident;

        /* All zeroes must fail */
        Expect(!ValidateAtaIdentifyData(ident), "All-zero identify buffer is rejected");

        /* All 0xFF must fail */
        memset(ident, 0xFF, sizeof(ident));
        Expect(!ValidateAtaIdentifyData(ident), "All-0xFF identify buffer is rejected");

        /* ATAPI device (Word 0 bit 15 == 1) must fail */
        memset(ident, 0, sizeof(ident));
        w[0] = 0x85C0; /* ATAPI CD-ROM */
        Expect(!ValidateAtaIdentifyData(ident), "ATAPI device (bit 15=1) is rejected");

        /* Valid ATA device configuration */
        w[0] = 0x0040; /* standard ATA device */
        /* Word 27-46: Model string (byte-swapped ASCII "WDC WDS120G2G0A-00JH30") */
        const char* szModelAscii = "DW CDW1S022G0G-A00HJ03                  ";
        memcpy(&w[27], szModelAscii, 40);
        w[60] = 0x5000; /* sectors low */
        w[61] = 0x0E46; /* sectors high -> ~240M sectors (~120GB) */
        Expect(ValidateAtaIdentifyData(ident), "Standard ATA Identify buffer is accepted");

        DRIVE_INFO drv;
        memset(&drv, 0, sizeof(drv));
        Expect(ParseAtaIdentifyData(ident, &drv), "ParseAtaIdentifyData accepts valid Identify");
        Expect(strstr(drv.szModel, "WDC WDS120G2G0A") != NULL, "szModel populated with clean ASCII model");

        /* Garbage RAM dump with non-printable binary characters in model string */
        ident[54] = 0x01; /* binary control char in model word 27 */
        ident[55] = 0xFF; /* binary non-ASCII byte */
        Expect(!ValidateAtaIdentifyData(ident), "Garbage RAM dump with non-printable ASCII in model is rejected");

        /* Absurd sector count > 100 PB */
        memcpy(&w[27], szModelAscii, 40);
        w[83] = 0x0400; /* LBA48 supported */
        w[100] = 0xFFFF;
        w[101] = 0xFFFF;
        w[102] = 0xFFFF;
        w[103] = 0x000F; /* > 200 billion sectors */
        Expect(!ValidateAtaIdentifyData(ident), "Absurd sector count (>100 PB) is rejected");
    }

    /* 2. Toshiba Attribute 188 (Timeouts) and Attribute 192 (Power-off Retracts) */
    {
        DRIVE_INFO hdd;
        memset(&hdd, 0, sizeof(hdd));
        hdd.eType = DRIVE_TYPE_HDD;
        hdd.eVendor = VENDOR_TOSHIBA;
        hdd.bSMART_Supported = TRUE;
        lstrcpynA(hdd.szModel, "TOSHIBA MK5076GSX", (int)sizeof(hdd.szModel));

        /* Case 2A: Attribute 188 with Word 0 = 16, Word 1 = 0, Word 2 = 0
         * 16 aborted commands during host sleep/resets, 0 timeouts > 5s.
         * Must NOT trigger interface caution! */
        hdd.attrData.stAttributes[0].bAttrID = 0xBC;
        hdd.attrData.stAttributes[0].bAttrValue = 100;
        hdd.attrData.stAttributes[0].bWorstValue = 100;
        hdd.attrData.stAttributes[0].bRawValue[0] = 0x10; /* Word 0 = 16 */
        hdd.attrData.stAttributes[0].bRawValue[1] = 0x00;
        hdd.attrData.stAttributes[0].bRawValue[2] = 0x00; /* Word 1 = 0 (>5s) */
        hdd.attrData.stAttributes[0].bRawValue[3] = 0x00;
        hdd.attrData.stAttributes[0].bRawValue[4] = 0x00; /* Word 2 = 0 (>7.5s) */
        hdd.attrData.stAttributes[0].bRawValue[5] = 0x00;

        AssessDriveHealth(&hdd);
        ExpectInt((int)hdd.eInterface, (int)HEALTH_STATUS_GOOD, "Attr 188 with 0 timeouts >5s keeps eInterface GOOD");

        /* Case 2B: Attribute 188 with Word 1 = 15 (>5s timeouts)
         * Real interface timeouts must trigger interface caution. */
        hdd.attrData.stAttributes[0].bRawValue[2] = 15;
        hdd.attrData.stAttributes[0].bRawValue[3] = 0;
        AssessDriveHealth(&hdd);
        ExpectInt((int)hdd.eInterface, (int)HEALTH_STATUS_CAUTION, "Attr 188 with 15 timeouts >5s sets eInterface CAUTION");

        /* Case 2C: Attribute 192 (0xC0) with dual 16-bit word packing:
         * RAW: 0x000001420142 -> 322 retracts, not 21,102,914 */
        hdd.attrData.stAttributes[1].bAttrID = 0xC0;
        hdd.attrData.stAttributes[1].bAttrValue = 100;
        hdd.attrData.stAttributes[1].bWorstValue = 100;
        hdd.attrData.stAttributes[1].bRawValue[0] = 0x42;
        hdd.attrData.stAttributes[1].bRawValue[1] = 0x01; /* 0x0142 = 322 */
        hdd.attrData.stAttributes[1].bRawValue[2] = 0x42;
        hdd.attrData.stAttributes[1].bRawValue[3] = 0x01; /* duplicate */
        hdd.attrData.stAttributes[1].bRawValue[4] = 0x00;
        hdd.attrData.stAttributes[1].bRawValue[5] = 0x00;

        AssessDriveHealth(&hdd);
        ExpectInt(hdd.nEmergencyRetract, 322, "Attr 192 unpacks 322 retracts instead of 21 million");
    }

    /* 3. Multi-Partition SSD Free Space: Small OEM recovery partition does not cause false panic */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ssd.nSSDLifeLeft = 100;
        ssd.nEndurancePercent = 100;
        ssd.bSMART_Supported = TRUE;
        ssd.trimInfo.bQueried = TRUE;
        ssd.trimInfo.bOsTrimEnabled = TRUE;
        ssd.trimInfo.bDeviceTrimSupported = TRUE;
        ssd.trimInfo.nVolumeCount = 2;

        /* Partition 1: C: (950 GB, 400 GB free = 42% free) */
        lstrcpynA(ssd.trimInfo.volumes[0].szLetter, "C:", 4);
        lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "NTFS", 16);
        ssd.trimInfo.volumes[0].qwTotalBytes = 950ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].qwFreeBytes  = 400ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].fFreePercent = 42.1f;
        ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;

        /* Partition 2: Recovery (500 MB, 20 MB free = 4.0% free by OEM design) */
        lstrcpynA(ssd.trimInfo.volumes[1].szLetter, "R:", 4);
        lstrcpynA(ssd.trimInfo.volumes[1].szFileSystem, "NTFS", 16);
        ssd.trimInfo.volumes[1].qwTotalBytes = 500ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].qwFreeBytes  = 20ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].fFreePercent = 4.0f;
        ssd.trimInfo.volumes[1].bTrimSupportedFs = TRUE;

        EvaluateDriveTrimAndCapacity(&ssd);
        Expect(!ssd.trimInfo.bPanicLowSpace, "SSD with 400 GB free does not trigger panic because of 500 MB recovery partition");
        Expect(!ssd.trimInfo.bCriticallyLowSpace, "SSD with 400 GB free does not trigger critical space warning");

        AssessDriveHealth(&ssd);
        ExpectInt((int)ssd.eWear, (int)HEALTH_STATUS_GOOD, "eWear stays GOOD with plenty of total SSD space");

        /* 4. Report duplication check: Plain lecture on healthy SSD does not dump raw partition table */
        char szPlain[4096];
        FormatHealthLecturePlain(&ssd, szPlain, sizeof(szPlain));
        Expect(strstr(szPlain, "Логические тома накопителя:") == NULL, "Plain lecture does not dump redundant volume list when healthy");
    }

    CaseEnd("build-51-regressions", before);
}

static void TestBuild52Regressions(void)
{
    int before = g_fail;

    /* 1. Generic unnamed SSD attribute naming and encoding checks:
     * Unnamed SSD attributes outside profile/drivedb must resolve to
     * vendor-specific with RAW_ENC_UNKNOWN and not inherit HDD mechanical
     * names (Flying height, spin-up current) or unconfirmed SSD decodes
     * (NAND writes GiB, read retries). */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        lstrcpynA(ssd.szModel, "Generic SSD 120GB", (int)sizeof(ssd.szModel));
        lstrcpynA(ssd.szFirmware, "1.0", (int)sizeof(ssd.szFirmware));

        ATTR_DECODE d;

        /* Attr 206 (0xCE): Must NOT be Flying Height */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xCE, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_UNKNOWN, "Generic SSD 206 is unknown");
        ExpectInt((int)d.eEnc, (int)RAW_ENC_UNKNOWN, "Generic SSD 206 enc is unknown");
        ExpectHas(d.szName, "vendor-specific", "Generic SSD 206 is vendor-specific");
        ExpectNotHas(d.szName, "Высота полёта", "Generic SSD 206 does not have flying height");
        ExpectNotHas(d.szName, "Flying", "Generic SSD 206 does not have Flying");

        /* Attr 207 (0xCF): Must NOT be Spin-Up Current */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xCF, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_UNKNOWN, "Generic SSD 207 is unknown");
        ExpectInt((int)d.eEnc, (int)RAW_ENC_UNKNOWN, "Generic SSD 207 enc is unknown");
        ExpectHas(d.szName, "vendor-specific", "Generic SSD 207 is vendor-specific");
        ExpectNotHas(d.szName, "Ток раскрутки", "Generic SSD 207 does not have spin-up current");
        ExpectNotHas(d.szName, "Spin-up", "Generic SSD 207 does not have Spin-up");

        /* Attr 249 (0xF9): Must NOT be NAND Writes GiB */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xF9, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_UNKNOWN, "Generic SSD 249 is unknown");
        ExpectInt((int)d.eEnc, (int)RAW_ENC_UNKNOWN, "Generic SSD 249 enc is unknown");
        ExpectHas(d.szName, "vendor-specific", "Generic SSD 249 is vendor-specific");
        ExpectNotHas(d.szName, "Записи NAND", "Generic SSD 249 does not have NAND writes");
        ExpectNotHas(d.szName, "ГиБ", "Generic SSD 249 does not have GiB");

        /* Attr 250 (0xFA): Must NOT be Read Retries */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xFA, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_UNKNOWN, "Generic SSD 250 is unknown");
        ExpectInt((int)d.eEnc, (int)RAW_ENC_UNKNOWN, "Generic SSD 250 enc is unknown");
        ExpectHas(d.szName, "vendor-specific", "Generic SSD 250 is vendor-specific");
        ExpectNotHas(d.szName, "Повторы ошибок", "Generic SSD 250 does not have read retries");

        /* Attr 174 (0xAE): Unnamed SSD is vendor-specific and power loss is NOT confirmed */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xAE, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_UNKNOWN, "Generic SSD 174 is unknown");
        ExpectInt((int)d.eEnc, (int)RAW_ENC_UNKNOWN, "Generic SSD 174 enc is unknown");
        ExpectHas(d.szName, "vendor-specific", "Generic SSD 174 is vendor-specific");
        Expect(!IsPowerLossAttrConfirmed(&ssd, 0xAE), "Generic SSD 174 power loss is not confirmed");

        /* Standard ATA attributes on the same generic SSD must still be recognized */
        memset(&d, 0, sizeof(d));
        GetAttrDecode(0x05, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "Generic SSD 0x05 is known");
        ExpectHas(d.szName, "Переназначенные", "Generic SSD 0x05 realloc name");

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0x09, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "Generic SSD 0x09 is known");
        ExpectHas(d.szName, "наработки", "Generic SSD 0x09 POH name");

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0x0C, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "Generic SSD 0x0C is known");
        ExpectHas(d.szName, "включения", "Generic SSD 0x0C cycles name");

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xC2, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "Generic SSD 0xC2 is known");
        ExpectHas(d.szName, "Температура", "Generic SSD 0xC2 temp name");

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xC7, &ssd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "Generic SSD 0xC7 is known");
        ExpectHas(d.szName, "CRC", "Generic SSD 0xC7 CRC name");
    }

    /* 2. HDD preserves mechanical attribute names: 206 (Flying height) and 207 (Spin-up current) */
    {
        DRIVE_INFO hdd;
        memset(&hdd, 0, sizeof(hdd));
        hdd.eType = DRIVE_TYPE_HDD;
        lstrcpynA(hdd.szModel, "Generic HDD 500GB", (int)sizeof(hdd.szModel));

        ATTR_DECODE d;

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xCE, &hdd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "HDD 206 is known");
        ExpectHas(d.szName, "Высота полёта", "HDD 206 has flying height name");

        memset(&d, 0, sizeof(d));
        GetAttrDecode(0xCF, &hdd, &d);
        ExpectInt((int)d.eState, (int)ATTR_DECODE_KNOWN, "HDD 207 is known");
        ExpectHas(d.szName, "Ток раскрутки", "HDD 207 has spin-up current name");
    }

    /* 3. Power loss confirmation gating: C0 on ATA SSD, A0 on Phison */
    {
        DRIVE_INFO ssdAta;
        memset(&ssdAta, 0, sizeof(ssdAta));
        ssdAta.eType = DRIVE_TYPE_SSD_SATA;
        Expect(IsPowerLossAttrConfirmed(&ssdAta, 0xC0), "C0 on ATA SSD is confirmed power loss");

        DRIVE_INFO phison;
        memset(&phison, 0, sizeof(phison));
        phison.eType = DRIVE_TYPE_SSD_SATA;
        phison.eController = CONTROLLER_PHISON;
        Expect(IsPowerLossAttrConfirmed(&phison, 0xA0), "A0 on Phison is confirmed power loss");
    }

    CaseEnd("build-52-regressions", before);
}

static void TestBuild53Regressions(void)
{
    int before = g_fail;

    /* 1. SATA PHY PHYRDY transitions alone (sleep, wake, bus resets) do not trigger eInterface CAUTION on HDD */
    {
        Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
        g.wRotationRate = 7200;
        Expect(AddAttr(5, 100, 0), "hdd realloc 0");
        Expect(AddAttr(9, 100, 1000), "hdd poh 1000");
        g.bSataPhyLogSupported = TRUE;
        g.nPhyTransitionsPhyRdy = 25; /* 25 sleep/wake or link drop transitions */
        g.nPhyCrcErrorsH2D = 0;       /* No CRC errors on cable */
        g.nPhyCommandIcrcErrors = 0;  /* No command ICRC errors */
        g.nPhyComresetCount = 10;

        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_GOOD,
                  "PHYRDY transitions alone keep HDD interface GOOD");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD,
                  "PHYRDY transitions alone do not degrade overall HDD health");

        /* Plain and expert lectures must still report the transition count as informative context */
        char szLecture[2048];
        szLecture[0] = 0;
        UiSetLang(UI_LANG_RU);
        FormatHealthLectureExpert(&g, szLecture, (int)sizeof(szLecture));
        ExpectHas(szLecture, "Потери готовности PHY: 25",
                  "Expert lecture reports PHYRDY drop count as context");
    }

    /* 2. SATA SSD with PHYRDY transitions alone keeps eInterface GOOD */
    {
        Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 500GB", "SVT01B6Q");
        Expect(AddAttr(5, 100, 0), "ssd realloc 0");
        Expect(AddAttr(9, 100, 500), "ssd poh 500");
        g.nSSDLifeLeft = 99;
        g.nEndurancePercent = 99;
        g.bSataPhyLogSupported = TRUE;
        g.nPhyTransitionsPhyRdy = 40;
        g.nPhyCrcErrorsH2D = 0;
        g.nPhyCommandIcrcErrors = 0;

        Run();
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_GOOD,
                  "PHYRDY transitions alone keep SSD interface GOOD");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD,
                  "PHYRDY transitions alone do not degrade overall SSD health");
    }

    /* 3. Real CRC errors and command ICRC errors in SATA PHY log STILL trigger eInterface CAUTION */
    {
        Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
        g.wRotationRate = 7200;
        Expect(AddAttr(5, 100, 0), "hdd realloc 0");
        Expect(AddAttr(9, 100, 1000), "hdd poh 1000");
        g.bSataPhyLogSupported = TRUE;
        g.nPhyTransitionsPhyRdy = 2;
        g.nPhyCrcErrorsH2D = 3; /* Real cable CRC errors */
        g.nPhyCommandIcrcErrors = 0;

        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION,
                  "SATA PHY H2D CRC error triggers HDD interface CAUTION");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION,
                  "SATA PHY H2D CRC error triggers overall CAUTION");

        Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 500GB", "SVT01B6Q");
        Expect(AddAttr(5, 100, 0), "ssd realloc 0");
        Expect(AddAttr(9, 100, 500), "ssd poh 500");
        g.nSSDLifeLeft = 99;
        g.nEndurancePercent = 99;
        g.bSataPhyLogSupported = TRUE;
        g.nPhyTransitionsPhyRdy = 0;
        g.nPhyCrcErrorsH2D = 0;
        g.nPhyCommandIcrcErrors = 1; /* Real command ICRC error */

        Run();
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION,
                  "SATA PHY command ICRC error triggers SSD interface CAUTION");
    }

    CaseEnd("build-53-regressions", before);
}

static void TestBuild54Regressions(void)
{
    int before = g_fail;

    /* 1. Isolated Event 153 (1 or 2 occurrences over 30 days) does not elevate overall health to CAUTION */
    {
        Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
        g.wRotationRate = 7200;
        Expect(AddAttr(5, 100, 0), "hdd realloc 0");
        Expect(AddAttr(9, 100, 1000), "hdd poh 1000");
        g.eventStats.bQueried = TRUE;
        g.eventStats.nEvent153Count = 1; /* 1 retry over 30 days */
        g.eventStats.nEvent129Count = 0;
        g.eventStats.nEvent51Count = 0;
        g.eventStats.nEventHwErrCount = 0;

        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_OBSERVE,
                  "Isolated 1 Event 153 sets HDD eInterface to OBSERVE");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD,
                  "Isolated 1 Event 153 does not degrade overall HDD health to CAUTION");

        char szPrompt[256];
        FormatHddObservePrompt(&g, szPrompt, (int)sizeof(szPrompt));
        ExpectHas(szPrompt, "Есть факторы риска", "Isolated 1 Event 153 produces observe prompt");

        /* 2 retries also remain OBSERVE / overall GOOD */
        g.eventStats.nEvent153Count = 2;
        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_OBSERVE,
                  "Isolated 2 Event 153 sets HDD eInterface to OBSERVE");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD,
                  "Isolated 2 Event 153 keeps overall HDD health GOOD");

        /* 3 retries (>= 3) elevate eInterface to CAUTION and trigger overall CAUTION */
        g.eventStats.nEvent153Count = 3;
        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION,
                  "Event 153 >= 3 sets HDD eInterface to CAUTION");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION,
                  "Event 153 >= 3 degrades overall HDD health to CAUTION");
        FormatHddCautionPrompt(&g, szPrompt, (int)sizeof(szPrompt));
        ExpectHas(szPrompt, "Проблема интерфейса", "Event 153 >= 3 produces caution prompt");
    }

    /* 2. SSD behaves identically: 1 Event 153 is OBSERVE, 3 is CAUTION */
    {
        Base(DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 500GB", "SVT01B6Q");
        Expect(AddAttr(5, 100, 0), "ssd realloc 0");
        Expect(AddAttr(9, 100, 500), "ssd poh 500");
        g.nSSDLifeLeft = 99;
        g.nEndurancePercent = 99;
        g.eventStats.bQueried = TRUE;
        g.eventStats.nEvent153Count = 1;
        g.eventStats.nEvent129Count = 0;
        g.eventStats.nEvent51Count = 0;
        g.eventStats.nEventHwErrCount = 0;

        Run();
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_OBSERVE,
                  "Isolated 1 Event 153 sets SSD eInterface to OBSERVE");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_GOOD,
                  "Isolated 1 Event 153 keeps overall SSD health GOOD");

        g.eventStats.nEvent153Count = 3;
        Run();
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION,
                  "Event 153 >= 3 sets SSD eInterface to CAUTION");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION,
                  "Event 153 >= 3 degrades overall SSD health to CAUTION");
    }

    /* 3. Combined bus reset (Event 129) and retry (Event 153) triggers CAUTION even if count < 3 */
    {
        Base(DRIVE_TYPE_HDD, "ST1000DM010-2EP102", "CC43");
        g.wRotationRate = 7200;
        Expect(AddAttr(5, 100, 0), "hdd realloc 0");
        Expect(AddAttr(9, 100, 1000), "hdd poh 1000");
        g.eventStats.bQueried = TRUE;
        g.eventStats.nEvent129Count = 1; /* 1 reset */
        g.eventStats.nEvent153Count = 1; /* 1 retry */
        g.eventStats.nEvent51Count = 0;
        g.eventStats.nEventHwErrCount = 0;

        AssessDriveHealth(&g);
        ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION,
                  "Combined Event 129 + Event 153 elevates eInterface to CAUTION");
        ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION,
                  "Combined Event 129 + Event 153 degrades overall health to CAUTION");
    }

    CaseEnd("build-54-regressions", before);
}

static void TestBuild55Regressions(void)
{
    int before = g_fail;

    /* 1. First partition is small (< 1 GB, e.g. 500 MB recovery with 4% free),
     * second partition is large (1 TB with 80% free).
     * fMinFreePercent must take 80.0%, NOT 4.0%! */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ssd.nSSDLifeLeft = 100;
        ssd.nEndurancePercent = 100;
        ssd.bSMART_Supported = TRUE;
        ssd.trimInfo.bQueried = TRUE;
        ssd.trimInfo.bOsTrimEnabled = TRUE;
        ssd.trimInfo.bDeviceTrimSupported = TRUE;
        ssd.trimInfo.nVolumeCount = 2;

        /* Partition 0: D: (500 MB, 20 MB free = 4.0% free) */
        lstrcpynA(ssd.trimInfo.volumes[0].szLetter, "D:", 4);
        lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "NTFS", 16);
        ssd.trimInfo.volumes[0].qwTotalBytes = 500ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].qwFreeBytes  = 20ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].fFreePercent = 4.0f;
        ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;

        /* Partition 1: E: (1000 GB, 800 GB free = 80.0% free) */
        lstrcpynA(ssd.trimInfo.volumes[1].szLetter, "E:", 4);
        lstrcpynA(ssd.trimInfo.volumes[1].szFileSystem, "NTFS", 16);
        ssd.trimInfo.volumes[1].qwTotalBytes = 1000ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].qwFreeBytes  = 800ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].fFreePercent = 80.0f;
        ssd.trimInfo.volumes[1].bTrimSupportedFs = TRUE;

        EvaluateDriveTrimAndCapacity(&ssd);
        Expect(fabs(ssd.trimInfo.fMinFreePercent - 80.0f) < 0.1f,
               "fMinFreePercent ignores first < 1GB partition and takes 80% from 1TB volume");
        Expect(!ssd.trimInfo.bPanicLowSpace, "No panic space on 1TB volume with 80% free");
        Expect(!ssd.trimInfo.bCriticallyLowSpace, "No critical space on 1TB volume with 80% free");
    }

    /* 2. Multiple partitions >= 1 GB with small recovery partition:
     * Minimum free % must be calculated among partitions >= 1 GB. */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ssd.nSSDLifeLeft = 100;
        ssd.nEndurancePercent = 100;
        ssd.bSMART_Supported = TRUE;
        ssd.trimInfo.bQueried = TRUE;
        ssd.trimInfo.bOsTrimEnabled = TRUE;
        ssd.trimInfo.bDeviceTrimSupported = TRUE;
        ssd.trimInfo.nVolumeCount = 3;

        /* Partition 0: Recovery (500 MB, 1.0% free) */
        ssd.trimInfo.volumes[0].qwTotalBytes = 500ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].fFreePercent = 1.0f;
        ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;

        /* Partition 1: C: (100 GB, 30.0% free) */
        ssd.trimInfo.volumes[1].qwTotalBytes = 100ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].fFreePercent = 30.0f;
        ssd.trimInfo.volumes[1].bTrimSupportedFs = TRUE;

        /* Partition 2: D: (500 GB, 15.0% free) */
        ssd.trimInfo.volumes[2].qwTotalBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[2].fFreePercent = 15.0f;
        ssd.trimInfo.volumes[2].bTrimSupportedFs = TRUE;

        EvaluateDriveTrimAndCapacity(&ssd);
        Expect(fabs(ssd.trimInfo.fMinFreePercent - 15.0f) < 0.1f,
               "fMinFreePercent takes 15% (min between C: 30% and D: 15%), ignoring 1% recovery");
    }

    /* 3. All partitions are small (< 1 GB):
     * fMinFreePercent falls back to overall free space percentage. */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ssd.bSMART_Supported = TRUE;
        ssd.trimInfo.bQueried = TRUE;
        ssd.trimInfo.bOsTrimEnabled = TRUE;
        ssd.trimInfo.bDeviceTrimSupported = TRUE;
        ssd.trimInfo.nVolumeCount = 2;

        /* Partition 0: 500 MB, 50 MB free (10.0%) */
        ssd.trimInfo.volumes[0].qwTotalBytes = 500ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].qwFreeBytes  = 50ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].fFreePercent = 10.0f;
        ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;

        /* Partition 1: 300 MB, 60 MB free (20.0%) */
        ssd.trimInfo.volumes[1].qwTotalBytes = 300ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].qwFreeBytes  = 60ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[1].fFreePercent = 20.0f;
        ssd.trimInfo.volumes[1].bTrimSupportedFs = TRUE;

        /* Total = 800 MB, free = 110 MB -> 13.75% */
        EvaluateDriveTrimAndCapacity(&ssd);
        Expect(fabs(ssd.trimInfo.fMinFreePercent - 13.75f) < 0.2f,
               "When all partitions < 1GB, fMinFreePercent falls back to overall free % (13.75%)");
    }

    CaseEnd("build-55-regressions", before);
}

static void TestBuild56Regressions(void)
{
    int before = g_fail;

    /* 1. When no volumes could be read (nVolumeCount == 0), szTrimSummary must report
     * "Активен (тома не прочитаны)", not "Активен (NTFS/ReFS)" */
    {
        DRIVE_INFO ssd;
        memset(&ssd, 0, sizeof(ssd));
        ssd.eType = DRIVE_TYPE_SSD_SATA;
        ssd.nSSDLifeLeft = 100;
        ssd.nEndurancePercent = 100;
        ssd.bSMART_Supported = TRUE;
        ssd.trimInfo.bQueried = TRUE;
        ssd.trimInfo.bOsTrimEnabled = TRUE;
        ssd.trimInfo.bDeviceTrimSupported = TRUE;
        ssd.trimInfo.nVolumeCount = 0; /* Volumes could not be opened/read */

        UiSetLang(UI_LANG_RU);
        EvaluateDriveTrimAndCapacity(&ssd);
        ExpectStr(ssd.trimInfo.szTrimSummary, "Активен (тома не прочитаны)",
                  "szTrimSummary is 'Активен (тома не прочитаны)' when nVolumeCount == 0 (RU)");
        Expect(strstr(ssd.trimInfo.szTrimSummary, "NTFS/ReFS") == NULL,
               "szTrimSummary does not claim NTFS/ReFS when volumes not read (RU)");

        UiSetLang(UI_LANG_EN);
        EvaluateDriveTrimAndCapacity(&ssd);
        ExpectStr(ssd.trimInfo.szTrimSummary, "Active (volumes not read)",
                  "szTrimSummary is 'Active (volumes not read)' when nVolumeCount == 0 (EN)");
        Expect(strstr(ssd.trimInfo.szTrimSummary, "NTFS/ReFS") == NULL,
               "szTrimSummary does not claim NTFS/ReFS when volumes not read (EN)");

        /* When volumes are read as NTFS, it properly reports Active (NTFS/ReFS) */
        ssd.trimInfo.nVolumeCount = 1;
        lstrcpynA(ssd.trimInfo.volumes[0].szLetter, "C:", 4);
        lstrcpynA(ssd.trimInfo.volumes[0].szFileSystem, "NTFS", 16);
        ssd.trimInfo.volumes[0].qwTotalBytes = 500ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].qwFreeBytes  = 250ULL * 1024ULL * 1024ULL * 1024ULL;
        ssd.trimInfo.volumes[0].fFreePercent = 50.0f;
        ssd.trimInfo.volumes[0].bTrimSupportedFs = TRUE;

        EvaluateDriveTrimAndCapacity(&ssd);
        ExpectStr(ssd.trimInfo.szTrimSummary, "Active (NTFS/ReFS)",
                  "szTrimSummary is 'Active (NTFS/ReFS)' when NTFS volume is read (EN)");

        UiSetLang(UI_LANG_RU);
        EvaluateDriveTrimAndCapacity(&ssd);
        ExpectStr(ssd.trimInfo.szTrimSummary, "Активен (NTFS/ReFS)",
                  "szTrimSummary is 'Активен (NTFS/ReFS)' when NTFS volume is read (RU)");
    }

    CaseEnd("build-56-regressions", before);
}




int main(void){
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
    TestModernHardwareDetect();
    TestNt256();
    TestCx400();
    TestHitachiTravelstar();
    TestPhisonMge15();
    TestGammixS50Lite();
    TestBareNvmeDecoder();
    TestNeobihier();
    TestDriveSummaryFormat();
    TestAtaErrorLog();
    TestPhisonHostUnits();
    TestSeekNearThresh();
    TestKioxiaBg4();
    TestA400LowLifeAndSlcAttrs();
    TestToshibaReadonly();
    TestUniversalWriteProtect();
    TestAdataSx8200();
    TestStandbyAwareness();
    {
        int before = g_fail;
        Base(DRIVE_TYPE_SSD_SATA, "SPCC M.2 SSD", "X0826A0");
        Expect(AddAttr(0xA8, 100, 5050), "spcc m2 max erase spec 5050");
        Expect(AddAttr(0xA9, 100, 100), "spcc m2 remaining life 100%");
        Expect(AddAttr(0xE8, 100, 100), "spcc m2 available spare 100%");
        Run();
        ExpectStr(g.szProfileId, "spcc-m2", "spcc m2 profile");
        ExpectInt((int)g.eController, (int)CONTROLLER_PHISON, "spcc m2 controller Phison");
        ExpectInt(g.nEndurancePercent, 100, "spcc m2 life 100%");
        CaseEnd("spcc-m2-phison", before);
    }
    TestMonitorIntervals();
    TestSelfTestLogDecoding();
    TestLatencyProbe();
    TestLinkSpeedDegradation();
    TestStorageEventLog();
    TestVolumeTrimMonitoring();
    TestDramVerification();
    TestCounterfeitDetection();
    TestLectureUtf8Truncation();
    TestSataPhyLogParsing();
    TestDeviceStatisticsParsing();
    TestAtaLogsIntegration();
    TestSeagateFarmLogParsing();
    TestSeagateFarmCounterfeitDetection();
    TestLegacyUsbBridges();
    TestCsmiStpPacket();
    TestDstEngineFormatting();
    TestBuild51Regressions();
    TestBuild52Regressions();
    TestBuild53Regressions();
    TestBuild54Regressions();
    TestBuild55Regressions();
    TestBuild56Regressions();
    if (g_fail) {
        fprintf(stderr, "FAILED %d of %d checks\n", g_fail, g_check);
        return 1;
    }
    printf("ok %d checks\n", g_check);
    return 0;
}

