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
    ExpectInt((int)g.eReliability, (int)HEALTH_STATUS_CAUTION, "su800 media");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "su800 headline");
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
 * 218 is the drivedb CRC path when 199 has not filled the slot. */
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
    ExpectInt(g.nCrcErrors, 120, "a400 crc");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_BAD, "a400 interface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "a400 headline");
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
    ExpectInt(g.nCrcErrors, 7, "spcc crc");
    ExpectInt((int)g.eInterface, (int)HEALTH_STATUS_CAUTION, "spcc interface");
    ExpectInt((int)g.eHealthStatus, (int)HEALTH_STATUS_CAUTION, "spcc headline");
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

int main(void)
{
    printf("drivedb_test\n");
    fflush(stdout);
    TestSu800();
    TestAs350();
    TestA400Phison();
    TestA400CrcSlot();
    TestA400Smi();
    TestSpcc();
    TestSamsung870();
    TestBarracuda();
    TestUsbSkip();
    if (g_fail) {
        fprintf(stderr, "FAILED %d of %d checks\n", g_fail, g_check);
        return 1;
    }
    printf("ok %d checks\n", g_check);
    return 0;
}
