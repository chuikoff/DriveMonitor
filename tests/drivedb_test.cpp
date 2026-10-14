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
    /* 160 is a drivedb name, not a local rule. RAW 3 is Risk, not Caution. */
    ExpectRank(160, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "su800 160 risk");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_OBSERVE, "su800 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_OBSERVE, "su800 headline");
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
    ExpectSem(0xE7, PROF_SEM_ENDURANCE, PROF_ENC_PERCENT, "870 E7");
    ExpectInt(g.nSSDLifeLeft, 98, "870 life from value");
    ExpectInt(g.nEndurancePercent, 98, "870 endurance");
    Expect(g.nProfileFlags & PROF_F_E7_VALUE, "870 E7 value flag");
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
 * only a hint, so RAW 5 is Risk. RAW 0 stays Good. Attribute 199 keeps
 * the local CRC thresholds. */
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
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_OBSERVE, "smartmod media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_OBSERVE, "smartmod headline");
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

/* Packed presets are not one 48-bit count. A huge raw24/raw32 value
 * stays unscored. A plain raw48 name on the same family is only Risk. */
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
    CaseEnd("pack", before);

    before = g_fail;
    Base(DRIVE_TYPE_SSD_SATA, "ZZPACKPROBE", "1.0");
    Expect(AddAttr(170, 100, 5), "pack170 slot");
    Run();
    ExpectSem(170, PROF_SEM_MEDIA, PROF_ENC_COUNTER, "pack 170");
    ExpectRank(170, ATTR_EV_RANK, (int)HEALTH_STATUS_OBSERVE, "pack 170 risk");
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_OBSERVE, "pack170 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_OBSERVE, "pack170 headline");
    Expect(g.eReliability != HEALTH_STATUS_CAUTION, "pack170 not caution");
    CaseEnd("pack-170", before);
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
    TestBarracuda();
    TestUsbSkip();
    TestDriveDbTie();
    TestRowRank();
    TestSmartModular();
    TestPackedName();
    TestLongPresetName();
    if (g_fail) {
        fprintf(stderr, "FAILED %d of %d checks\n", g_fail, g_check);
        return 1;
    }
    printf("ok %d checks\n", g_check);
    return 0;
}
