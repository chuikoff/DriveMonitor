/* DriveMonitor - health assessment and SSD indicators. MIT: see LICENSE. */
#include "smart_internal.h"

/* ============================================================
 * Health: state + confidence + evidence
 *
 * Overall eHealthStatus is worst-of evidence (GOOD/CAUTION/BAD/UNKNOWN).
 * nHealthPercent is SECONDARY estimated remaining life only (endurance),
 * never a fake % from worst/thresh or raw*N penalties.
 * nConfidence is completeness of evidence, not "how healthy".
 * ============================================================ */

/* Find threshold value for a given attribute ID */
static BYTE FindThreshold(const DRIVE_INFO* pInfo, BYTE bAttrID)
{
    int j;
    if (!pInfo) return 0;
    for (j = 0; j < 30; j++) {
        if (pInfo->threshData.stThresholds[j].bAttrID == bAttrID)
            return pInfo->threshData.stThresholds[j].bThresholdValue;
    }
    return 0;
}

/* RAW of attribute `id`, or -1 if the attribute is absent.
 * 05 uses the low 16 bits (sector count). 187 uses DecodeReportedUncorrect. */
static int AttrRawOrNeg1(const DRIVE_INFO* pInfo, BYTE id)
{
    int i;
    if (!pInfo) return -1;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID == id) {
            const BYTE* raw = pInfo->attrData.stAttributes[i].bRawValue;
            ATTR_DECODE dec;
            unsigned __int64 v;
            if (id == 0xBB)
                return DecodeReportedUncorrect(raw, pInfo->eVendor);
            GetAttrDecode(id, pInfo, &dec);
            if (dec.eEnc == RAW_ENC_SECTORS_LO16)
                v = (unsigned __int64)((WORD)raw[0] | ((WORD)raw[1] << 8));
            else
                v = GetRawValue48(raw);
            if (v > (unsigned __int64)INT_MAX) return INT_MAX;
            return (int)v;
        }
    }
    return -1;
}

static int AttrValueOrNeg1(const DRIVE_INFO* pInfo, BYTE id)
{
    int i;
    if (!pInfo) return -1;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID == id)
            return (int)pInfo->attrData.stAttributes[i].bAttrValue;
    }
    return -1;
}

static int AttrWorstOrNeg1(const DRIVE_INFO* pInfo, BYTE id)
{
    int i;
    if (!pInfo) return -1;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID == id)
            return (int)pInfo->attrData.stAttributes[i].bWorstValue;
    }
    return -1;
}


/* Last G-Sense/C0/POH snapshot per serial. Growth needs a later sample. */
#define MECH_STORE_MAX 48
typedef struct {
    char serial[24];
    int gsense;
    int c0;
    DWORD poh;
    int pending;
    int realloc;
    int uncorr;
} MECH_SNAP;

static void MechStorePath(char* path, int nPath)
{
    char dir[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (!n || n >= MAX_PATH) {
        lstrcpynA(path, "DriveMonitor_mech.txt", nPath);
        return;
    }
    safe_snprintf_n(path, nPath, "%sDriveMonitor_mech.txt", dir);
}

static int MechLoadSnap(const char* serial, MECH_SNAP* out)
{
    char path[MAX_PATH + 32];
    FILE* f;
    char line[128];
    if (!serial || !serial[0] || !out) return 0;
    MechStorePath(path, (int)sizeof(path));
    f = fopen(path, "r");
    if (!f) return 0;
    while (fgets(line, (int)sizeof(line), f)) {
        MECH_SNAP s;
        unsigned long poh = 0;
        memset(&s, 0, sizeof(s));
        if (sscanf(line, "%23s %d %d %lu %d %d %d", s.serial, &s.gsense, &s.c0, &poh,
                   &s.pending, &s.realloc, &s.uncorr) >= 4) {
            s.poh = (DWORD)poh;
            if (strcmp(s.serial, serial) == 0) {
                *out = s;
                fclose(f);
                return 1;
            }
        }
    }
    fclose(f);
    return 0;
}

static void MechSaveSnap(const MECH_SNAP* add)
{
    MECH_SNAP all[MECH_STORE_MAX];
    int n = 0, i, found = 0;
    char path[MAX_PATH + 32];
    FILE* f;
    char line[128];
    if (!add || !add->serial[0]) return;
    MechStorePath(path, (int)sizeof(path));
    f = fopen(path, "r");
    if (f) {
        while (n < MECH_STORE_MAX && fgets(line, (int)sizeof(line), f)) {
            unsigned long poh = 0;
            MECH_SNAP s;
            memset(&s, 0, sizeof(s));
            if (sscanf(line, "%23s %d %d %lu %d %d %d", s.serial, &s.gsense, &s.c0, &poh,
                       &s.pending, &s.realloc, &s.uncorr) >= 4) {
                s.poh = (DWORD)poh;
                all[n++] = s;
            }
        }
        fclose(f);
    }
    for (i = 0; i < n; i++) {
        if (strcmp(all[i].serial, add->serial) == 0) {
            all[i] = *add;
            found = 1;
            break;
        }
    }
    if (!found && n < MECH_STORE_MAX)
        all[n++] = *add;
    f = fopen(path, "w");
    if (!f) return;
    for (i = 0; i < n; i++)
        fprintf(f, "%s %d %d %lu %d %d %d\n", all[i].serial, all[i].gsense, all[i].c0,
                (unsigned long)all[i].poh, all[i].pending, all[i].realloc, all[i].uncorr);
    fclose(f);
}

static BOOL HasNonZeroThreshold(const DRIVE_INFO* pInfo)
{
    int j;
    if (!pInfo) return FALSE;
    for (j = 0; j < 30; j++) {
        if (pInfo->threshData.stThresholds[j].bAttrID != 0 &&
            pInfo->threshData.stThresholds[j].bThresholdValue > 0)
            return TRUE;
    }
    return FALSE;
}

static int Clamp100(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

static BOOL FirmwareFamilyRecognized(const DRIVE_INFO* p)
{
    char szFwU[16];
    if (!p || !p->szFirmware[0]) return FALSE;
    ToUpperCopy(szFwU, sizeof(szFwU), p->szFirmware);
    if (strncmp(szFwU, "HPS", 3) == 0) return TRUE;
    if (strncmp(szFwU, "SBF", 3) == 0) return TRUE;
    if (strncmp(szFwU, "SAFM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "ECFM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "E8FM", 4) == 0) return TRUE;
    if (strncmp(szFwU, "E7FM", 4) == 0) return TRUE;
    if (strstr(szFwU, "SM22") || strstr(szFwU, "SM23") || strstr(szFwU, "SM25"))
        return TRUE;
    return FALSE;
}

static int CountUnknownAttrs(const DRIVE_INFO* pInfo)
{
    int i, n = 0;
    ATTR_DECODE dec;
    if (!pInfo) return 0;
    for (i = 0; i < 30; i++) {
        BYTE id = pInfo->attrData.stAttributes[i].bAttrID;
        if (id == 0) continue;
        GetAttrDecode(id, pInfo, &dec);
        if (dec.eState == ATTR_DECODE_UNKNOWN)
            n++;
    }
    return n;
}

static int CountPresentAttrs(const DRIVE_INFO* pInfo)
{
    int i, n = 0;
    if (!pInfo) return 0;
    for (i = 0; i < 30; i++) {
        if (pInfo->attrData.stAttributes[i].bAttrID != 0)
            n++;
    }
    return n;
}

static BOOL MediaCountersClean(const DRIVE_INFO* p)
{
    if (!p) return FALSE;
    if (p->nReallocated > 0) return FALSE;
    if (p->nPendingSectors > 0) return FALSE;
    if (p->nUncorrectable > 0) return FALSE;
    if (p->qwNVMeMediaErrors > 0) return FALSE;
    return TRUE;
}

static BOOL HasKnownMediaZeros(const DRIVE_INFO* p)
{
    int nPresent = 0;
    if (!p) return FALSE;
    if (p->nReallocated >= 0) {
        nPresent++;
        if (p->nReallocated != 0) return FALSE;
    }
    if (p->nPendingSectors >= 0) {
        nPresent++;
        if (p->nPendingSectors != 0) return FALSE;
    }
    if (p->nUncorrectable >= 0) {
        nPresent++;
        if (p->nUncorrectable != 0) return FALSE;
    }
    return nPresent > 0;
}


static TEMP_BAND TempBandFromC(int nC)
{
    if (nC <= 0) return TEMP_BAND_UNKNOWN;
    if (nC <= 49) return TEMP_BAND_NORMAL;
    if (nC <= 59) return TEMP_BAND_ELEVATED;
    if (nC <= 69) return TEMP_BAND_HIGH;
    return TEMP_BAND_CRITICAL;
}

/* NVMe: compare composite °C to Identify WCTEMP/CCTEMP. 0h = not reported. */
static TEMP_BAND TempBandFromNvme(int nC, USHORT wWarnK, USHORT wCritK)
{
    int warnC, critC, nNear;
    if (nC <= 0) return TEMP_BAND_UNKNOWN;
    warnC = NvmeIdentifyTempC(wWarnK);
    critC = NvmeIdentifyTempC(wCritK);
    if (warnC < 0 && critC < 0)
        return TempBandFromC(nC);
    if (critC > 0 && nC >= critC)
        return TEMP_BAND_CRITICAL;
    if (warnC > 0 && nC >= warnC)
        return TEMP_BAND_HIGH;
    nNear = (warnC > 0) ? warnC - 10 : ((critC > 0) ? critC - 10 : -1);
    if (nNear > 0 && nC >= nNear)
        return TEMP_BAND_ELEVATED;
    return TEMP_BAND_NORMAL;
}

static DRIVE_HEALTH_STATUS TempStatusFromBand(TEMP_BAND eBand)
{
    switch (eBand) {
    case TEMP_BAND_NORMAL:   return HEALTH_STATUS_GOOD;
    case TEMP_BAND_ELEVATED: return HEALTH_STATUS_CAUTION;
    case TEMP_BAND_HIGH:     return HEALTH_STATUS_CAUTION;
    case TEMP_BAND_CRITICAL: return HEALTH_STATUS_BAD;
    default:                 return HEALTH_STATUS_UNKNOWN;
    }
}

static void FormatUintGrouped(unsigned long n, char* buf, int nBuf)
{
    char tmp[32];
    int len, i, o, first;
    if (!buf || nBuf <= 0) return;
    (void)_snprintf(tmp, sizeof(tmp), "%lu", n);
    tmp[sizeof(tmp) - 1] = '\0';
    len = (int)strlen(tmp);
    first = len % 3;
    if (first == 0) first = 3;
    o = 0;
    for (i = 0; i < len && o < nBuf - 1; i++) {
        if (i > 0 && i >= first && ((i - first) % 3) == 0) {
            buf[o++] = ' ';
            if (o >= nBuf - 1) break;
        }
        buf[o++] = tmp[i];
    }
    buf[o] = '\0';
}

static const char* RuCountWord(unsigned n, const char* one,
                               const char* few, const char* many)
{
    unsigned n10 = n % 10;
    unsigned n100 = n % 100;
    if (n10 == 1 && n100 != 11) return one;
    if (n10 >= 2 && n10 <= 4 && (n100 < 12 || n100 > 14)) return few;
    return many;
}

void FormatPowerOnHours(DWORD dwHours, char* szBuf, int nBufLen)
{
    unsigned y, d, h;
    char a[40], b[40], c[40];
    int n = 0;
    const char* p[3];

    if (!szBuf || nBufLen <= 0) return;
    if (dwHours == 0) {
        safe_snprintf_n(szBuf, nBufLen, "нет данных");
        return;
    }
    y = dwHours / 8760u;
    d = (dwHours % 8760u) / 24u;
    h = (dwHours % 8760u) % 24u;
    if (y > 0) {
        safe_snprintf(a, "%u %s", y, RuCountWord(y, "год", "года", "лет"));
        p[n++] = a;
    }
    if (d > 0) {
        safe_snprintf(b, "%u %s", d, RuCountWord(d, "день", "дня", "дней"));
        p[n++] = b;
    }
    if (h > 0 || n == 0) {
        safe_snprintf(c, "%u %s", h, RuCountWord(h, "час", "часа", "часов"));
        p[n++] = c;
    }
    if (n == 1)
        safe_snprintf_n(szBuf, nBufLen, "%s", p[0]);
    else if (n == 2)
        safe_snprintf_n(szBuf, nBufLen, "%s %s", p[0], p[1]);
    else
        safe_snprintf_n(szBuf, nBufLen, "%s %s %s", p[0], p[1], p[2]);
}

const char* GetTempBandName(TEMP_BAND eBand, BOOL bLowercase)
{
    switch (eBand) {
    case TEMP_BAND_NORMAL:   return bLowercase ? "норма" : "Норма";
    case TEMP_BAND_ELEVATED: return bLowercase ? "повышена" : "Повышена";
    case TEMP_BAND_HIGH:     return bLowercase ? "высокая" : "Высокая";
    case TEMP_BAND_CRITICAL: return bLowercase ? "критическая" : "Критическая";
    default:                 return "нет данных";
    }
}

static void FormatTempLecture(const DRIVE_INFO* p, char* buf, int nBuf)
{
    int warnC, critC;
    if (!buf || nBuf <= 0) return;
    if (!p || p->nTemperatureC <= 0) {
        safe_snprintf_n(buf, nBuf, "нет данных");
        return;
    }
    warnC = p->nTempWarnC > 0 ? p->nTempWarnC :
            (p->bIsNVMe ? NvmeIdentifyTempC(p->wNVMeWarnTempThreshold) : -1);
    critC = p->nTempCritC > 0 ? p->nTempCritC :
            (p->bIsNVMe ? NvmeIdentifyTempC(p->wNVMeCritTempThreshold) : -1);
    if (warnC > 0 && critC > 0 && p->nTempMaxC > 0)
        safe_snprintf_n(buf, nBuf,
            "%d °C · %s (макс. %d °C, пред. %d, крит. %d)",
            p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
            p->nTempMaxC, warnC, critC);
    else if (warnC > 0 && critC > 0)
        safe_snprintf_n(buf, nBuf, "%d °C · %s (пред. %d °C, крит. %d °C)",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        warnC, critC);
    else if (p->nTempMaxC > 0 && critC > 0)
        safe_snprintf_n(buf, nBuf, "%d °C · %s (макс. %d, крит. %d)",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        p->nTempMaxC, critC);
    else if (p->nTempMaxC > 0)
        safe_snprintf_n(buf, nBuf, "%d °C · %s (макс. зафиксированная %d °C)",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        p->nTempMaxC);
    else if (warnC > 0)
        safe_snprintf_n(buf, nBuf, "%d °C · %s (пред. %d °C)",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        warnC);
    else if (critC > 0)
        safe_snprintf_n(buf, nBuf, "%d °C · %s (крит. %d °C)",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        critC);
    else
        safe_snprintf_n(buf, nBuf, "%d °C · %s",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE));
}

static const char* QualityNameRu(NORM_QUALITY q, BOOL bRaw)
{
    switch (q) {
    case NORM_QUALITY_LOW:    return "НИЗКОЕ";
    case NORM_QUALITY_MEDIUM: return bRaw ? "СМЕШАННОЕ" : "СРЕДНЕЕ";
    case NORM_QUALITY_HIGH:   return "ВЫСОКОЕ";
    default:                  return "нет данных";
    }
}

static int Utf8DispWidth(const char* s)
{
    int w = 0;
    const unsigned char* p = (const unsigned char*)s;
    if (!p) return 0;
    while (*p) {
        if (*p < 0x80) { p++; w++; }
        else if ((*p & 0xE0) == 0xC0) { p += 2; w++; }
        else if ((*p & 0xF0) == 0xE0) { p += 3; w++; }
        else if ((*p & 0xF8) == 0xF0) { p += 4; w++; }
        else p++;
    }
    return w;
}

static void PadUtf8(char* dst, int nDst, const char* s, int width)
{
    int w;
    size_t used;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    if (s)
        safe_snprintf_n(dst, nDst, "%s", s);
    w = Utf8DispWidth(dst);
    used = strlen(dst);
    while (w < width && used + 1 < (size_t)nDst) {
        dst[used++] = ' ';
        dst[used] = '\0';
        w++;
    }
}

static void FmtIntOrNA(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "нет данных");
    else
        safe_snprintf_n(buf, nBuf, "%d", v);
}

static void LectureAdd(char* buf, int nBuf, const char* s)
{
    size_t used, rem;
    if (!buf || nBuf <= 1 || !s) return;
    used = strlen(buf);
    if (used >= (size_t)nBuf - 1) return;
    rem = (size_t)nBuf - used;
    safe_snprintf_n(buf + used, (int)rem, "%s", s);
}

static void FormatNvmeCritWarnBits(BYTE cw, char* buf, int nBuf)
{
    char tmp[192];
    tmp[0] = '\0';
    if (!buf || nBuf <= 0) return;
    if (cw == 0) {
        safe_snprintf_n(buf, nBuf, "нет");
        return;
    }
    if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", запас" : "запас");
    if (cw & NVME_CRIT_WARN_TEMP_THRESHOLD)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", температура" : "температура");
    if (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", надёжность" : "надёжность");
    if (cw & NVME_CRIT_WARN_READ_ONLY)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", только чтение" : "только чтение");
    if (cw & NVME_CRIT_WARN_VOLATILE_MEM_BACKUP)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", резерв энергозависимой памяти" : "резерв энергозависимой памяти");
    if (cw & NVME_CRIT_WARN_PMR_RO)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? ", постоянная память только чтение" : "постоянная память только чтение");
    if (tmp[0] == '\0')
        safe_snprintf_n(buf, nBuf, "есть");
    else
        safe_snprintf_n(buf, nBuf, "%s", tmp);
}

static int HealthRank(DRIVE_HEALTH_STATUS e)
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

static DRIVE_HEALTH_STATUS WorstHealth(DRIVE_HEALTH_STATUS a, DRIVE_HEALTH_STATUS b)
{
    return (HealthRank(a) >= HealthRank(b)) ? a : b;
}

static BOOL SelfTestFailed(const DRIVE_INFO* p)
{
    int s;
    if (!p || !p->bGotSelfTestLog) return FALSE;
    s = (p->nSelfTestStatus >> 4) & 0x0F;
    return (s >= 3 && s <= 8);
}

/* ATA-3 / SFF-8035i: bit 0 of the attribute flags is pre-failure vs old-age. */
#define ATA_ATTR_PREFAIL  0x0001

static BOOL AttrIsPrefail(const SMART_ATTRIBUTE* a)
{
    return a && (a->wStatusFlags & ATA_ATTR_PREFAIL) != 0;
}

/* thresh==0 is ATA-3 "always passing". 0/255 Value/Worst are unused/garbage. */
static BOOL AtaFailingNow(BYTE val, BYTE thresh)
{
    if (thresh == 0) return FALSE;
    if (val == 255) return FALSE;
    return val <= thresh;
}

static BOOL AtaFailedPast(BYTE val, BYTE worst, BYTE thresh)
{
    if (thresh == 0) return FALSE;
    if (AtaFailingNow(val, thresh)) return FALSE;
    if (worst == 0 || worst == 255) return FALSE;
    return worst <= thresh;
}

/* Current Value approaching the official threshold. Worst is history only. */
static BOOL HddRateNearThresh(int val, int worst, BYTE thresh)
{
    (void)worst;
    if (val < 0 || thresh == 0)
        return FALSE;
    if (val <= (int)thresh + 10)
        return TRUE;
    return FALSE;
}

static unsigned HddRateErrs(const DRIVE_INFO* p, BYTE id)
{
    const SMART_ATTRIBUTE* a = FindAttr(p, id);
    if (!a) return 0;
    if (p && p->eVendor == VENDOR_SEAGATE)
        return SeagateRateErrs(a->bRawValue);
    return 0;
}

typedef struct {
    BOOL bId1;
    BOOL bId7;
    BOOL bId193Val;
    BOOL bId193Raw;
    BOOL bId195;
} HDD_WEAR;

static void HddWearFlags(const DRIVE_INFO* p, HDD_WEAR* w)
{
    int v1, w1, v7, w7, v193, v195, w195;
    if (!w) return;
    ZeroMemory(w, sizeof(*w));
    if (!p) return;
    v1   = AttrValueOrNeg1(p, 0x01);
    w1   = AttrWorstOrNeg1(p, 0x01);
    v7   = AttrValueOrNeg1(p, 0x07);
    w7   = AttrWorstOrNeg1(p, 0x07);
    v193 = AttrValueOrNeg1(p, 0xC1);
    v195 = AttrValueOrNeg1(p, 0xC3);
    w195 = AttrWorstOrNeg1(p, 0xC3);
    w->bId1      = HddRateNearThresh(v1, w1, FindThreshold(p, 0x01)) ||
                   HddRateErrs(p, 0x01) > 0;
    w->bId7      = HddRateNearThresh(v7, w7, FindThreshold(p, 0x07)) ||
                   HddRateErrs(p, 0x07) > 0;
    w->bId193Val = (v193 >= 0 && v193 <= 15);
    w->bId193Raw = (p->nLoadUnload >= 300000);
    w->bId195    = HddRateNearThresh(v195, w195, FindThreshold(p, 0xC3)) ||
                   HddRateErrs(p, 0xC3) > 0;
}

void FormatHddObservePrompt(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (!pInfo) return;

    if (pInfo->bPrefailPast) {
        safe_snprintf_n(szBuf, nBufLen,
            "Диск ещё считает себя исправным. Prefail-атрибут был ниже порога.");
        return;
    }
    if (pInfo->bUsageFailed) {
        safe_snprintf_n(szBuf, nBufLen,
            "Диск ещё считает себя исправным. Usage-атрибут на пороге — следить.");
        return;
    }
    safe_snprintf_n(szBuf, nBufLen,
        "Есть факторы риска. Нажмите на состояние.");
}

/* HDD: channels are displayed separately. Overall = ATA-3 prefail/media/self-test. */
static void AssessHddHealth(DRIVE_INFO* pInfo)
{
    BOOL bMediaHurt, bMediaClean;
    BOOL bGsenseUp = FALSE, bC5Up = FALSE, bMediaUp = FALSE;
    MECH_SNAP prev, now;
    int havePrev = 0;
    int n187;

    n187 = AttrRawOrNeg1(pInfo, 0xBB);
    bMediaHurt = (pInfo->nReallocated > 0 ||
                  pInfo->nPendingSectors > 0 ||
                  pInfo->nUncorrectable > 0 ||
                  pInfo->nRemapEvents > 0 ||
                  n187 > 0);
    bMediaClean = !bMediaHurt;

    pInfo->nGSenseDelta = -1;
    pInfo->nMechRisk = -1;
    if (pInfo->nGSenseEvents >= 0 && pInfo->dwPowerOnHours > 0)
        pInfo->nGSensePerKh =
            (int)(((long long)pInfo->nGSenseEvents * 1000LL) /
                  (long long)pInfo->dwPowerOnHours);

    memset(&now, 0, sizeof(now));
    if (pInfo->szSerial[0]) {
        lstrcpynA(now.serial, pInfo->szSerial, sizeof(now.serial));
        now.gsense = pInfo->nGSenseEvents < 0 ? 0 : pInfo->nGSenseEvents;
        now.c0 = pInfo->nEmergencyRetract < 0 ? 0 : pInfo->nEmergencyRetract;
        now.poh = pInfo->dwPowerOnHours;
        now.pending = pInfo->nPendingSectors < 0 ? 0 : pInfo->nPendingSectors;
        now.realloc = pInfo->nReallocated < 0 ? 0 : pInfo->nReallocated;
        now.uncorr = pInfo->nUncorrectable < 0 ? 0 : pInfo->nUncorrectable;
        havePrev = MechLoadSnap(now.serial, &prev);
        if (havePrev) {
            int dBf = now.gsense - prev.gsense;
            int dC5 = now.pending - prev.pending;
            int d05 = now.realloc - prev.realloc;
            int dC6 = now.uncorr - prev.uncorr;
            if (dBf < 0) dBf = 0;
            if (dC5 < 0) dC5 = 0;
            if (d05 < 0) d05 = 0;
            if (dC6 < 0) dC6 = 0;
            if (dBf > 0 || now.gsense != prev.gsense)
                pInfo->nGSenseDelta = dBf;
            if (dBf > 0) bGsenseUp = TRUE;
            if (dC5 > 0) bC5Up = TRUE;
            if (d05 > 0 || dC5 > 0 || dC6 > 0) bMediaUp = TRUE;
        }
        MechSaveSnap(&now);
    }

    {
        HDD_WEAR wear;
        int v193 = AttrValueOrNeg1(pInfo, 0xC1);
        BOOL bSeekWorn, bParkLow, bParkBad;
        HddWearFlags(pInfo, &wear);
        bSeekWorn = wear.bId7;
        bParkLow  = wear.bId193Val || wear.bId193Raw;
        bParkBad  = (v193 >= 0 && v193 <= 5);

    /* Media RAW 05/196/197/198/187 and ATA-3 prefail. Not 193/1/7 Value bands. */
    if (SelfTestFailed(pInfo))
        pInfo->eReliability = HEALTH_STATUS_CRITICAL;
    else if (pInfo->bPrefailNow ||
             pInfo->nUncorrectable > 0 ||
             pInfo->nPendingSectors >= 4 ||
             pInfo->nReallocated >= 10 ||
             n187 > 0)
        pInfo->eReliability = HEALTH_STATUS_BAD;
    else if (pInfo->nPendingSectors > 0 || pInfo->nReallocated > 0 ||
             pInfo->nRemapEvents > 0)
        pInfo->eReliability = HEALTH_STATUS_CAUTION;
    else if (pInfo->bPrefailPast || pInfo->bUsageFailed)
        pInfo->eReliability = HEALTH_STATUS_OBSERVE;
    else
        pInfo->eReliability = HEALTH_STATUS_GOOD;

    /* Interface: 199 / CRC. */
    if (pInfo->nCrcErrors >= 100)
        pInfo->eInterface = HEALTH_STATUS_BAD;
    else if (pInfo->nCrcErrors > 0)
        pInfo->eInterface = HEALTH_STATUS_CAUTION;
    else
        pInfo->eInterface = HEALTH_STATUS_GOOD;

    /* Temperature: risk without surface damage is OBSERVE, not BAD. */
    pInfo->eTempBand = TempBandFromC(pInfo->nTemperatureC);
    if (pInfo->eTempBand == TEMP_BAND_CRITICAL)
        pInfo->eTempStatus = HEALTH_STATUS_BAD;
    else if (pInfo->eTempBand == TEMP_BAND_HIGH)
        pInfo->eTempStatus = HEALTH_STATUS_CAUTION;
    else if (pInfo->eTempBand == TEMP_BAND_ELEVATED)
        pInfo->eTempStatus = HEALTH_STATUS_OBSERVE;
    else if (pInfo->eTempBand == TEMP_BAND_NORMAL)
        pInfo->eTempStatus = HEALTH_STATUS_GOOD;
    else
        pInfo->eTempStatus = HEALTH_STATUS_UNKNOWN;

    /* Mechanics + correlation with media. */
    if (pInfo->nGSenseEvents > 0 && bGsenseUp && bMediaUp &&
        (pInfo->nUncorrectable > 0 || pInfo->nReallocated > 0 || pInfo->nPendingSectors > 0))
        pInfo->eMechanics = HEALTH_STATUS_BAD;
    else if (pInfo->nGSenseEvents > 0 && bMediaHurt &&
             (pInfo->nUncorrectable > 0 || pInfo->nPendingSectors >= 4 ||
              pInfo->nReallocated >= 10))
        pInfo->eMechanics = HEALTH_STATUS_BAD;
    else if ((pInfo->nGSenseEvents > 0 && bC5Up) ||
             (pInfo->nGSenseEvents > 0 && pInfo->nPendingSectors > 0) ||
             (bGsenseUp && pInfo->nPendingSectors > 0))
        pInfo->eMechanics = HEALTH_STATUS_CAUTION;
    else if (pInfo->nGSenseDelta >= 100 && bMediaClean)
        pInfo->eMechanics = HEALTH_STATUS_CAUTION;
    else if (bParkBad)
        pInfo->eMechanics = HEALTH_STATUS_CAUTION;
    else if (bParkLow || bSeekWorn)
        pInfo->eMechanics = HEALTH_STATUS_OBSERVE;
    else if (pInfo->nGSenseEvents > 0 && bMediaClean)
        pInfo->eMechanics = HEALTH_STATUS_OBSERVE;
    else if (pInfo->nGSenseEvents >= 0 || pInfo->nEmergencyRetract >= 0 ||
             pInfo->nLoadUnload >= 0)
        pInfo->eMechanics = HEALTH_STATUS_GOOD;
    else
        pInfo->eMechanics = HEALTH_STATUS_UNKNOWN;

    /* Overall = reliability only. Mechanics / CRC / temp stay in their rows. */
    pInfo->eHealthStatus = pInfo->eReliability;
    }

    if (SelfTestFailed(pInfo))
        pInfo->eHealthStatus = HEALTH_STATUS_CRITICAL;

    pInfo->nHealthPercent = -1;
    pInfo->nConfidence = 0;
}


static DRIVE_HEALTH_STATUS WearStatusFromRemaining(int nLeft)
{
    if (nLeft < 0)  return HEALTH_STATUS_UNKNOWN;
    if (nLeft <= 5) return HEALTH_STATUS_BAD;
    if (nLeft <= 10) return HEALTH_STATUS_CAUTION;
    if (nLeft <= 20) return HEALTH_STATUS_OBSERVE;
    return HEALTH_STATUS_GOOD;
}

/* SSD/NVMe: media / wear / interface / temperature. No G-Sense channel. */
static void AssessSsdHealth(DRIVE_INFO* pInfo)
{
    BYTE cw;
    BOOL bSpareLow = FALSE;

    cw = pInfo->bIsNVMe ? pInfo->nvmeHealth.CriticalWarning : (BYTE)0;
    if (pInfo->bIsNVMe) {
        int nSpare = (int)pInfo->nvmeHealth.AvailableSpare;
        int nTh    = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        if (nSpare < nTh)
            bSpareLow = TRUE;
        if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
            bSpareLow = TRUE;
    }

    if (SelfTestFailed(pInfo) ||
        (cw & NVME_CRIT_WARN_READ_ONLY) ||
        (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED))
        pInfo->eReliability = HEALTH_STATUS_CRITICAL;
    else if (pInfo->bPrefailNow ||
             pInfo->qwNVMeMediaErrors > 0 ||
             pInfo->nUncorrectable > 0 ||
             bSpareLow)
        pInfo->eReliability = HEALTH_STATUS_BAD;
    else if (pInfo->nReallocated > 0 || pInfo->nPendingSectors > 0 ||
             pInfo->nRemapEvents > 0)
        pInfo->eReliability = HEALTH_STATUS_CAUTION;
    else if (pInfo->bPrefailPast || pInfo->bUsageFailed)
        pInfo->eReliability = HEALTH_STATUS_OBSERVE;
    else
        pInfo->eReliability = HEALTH_STATUS_GOOD;

    pInfo->eWear = WearStatusFromRemaining(pInfo->nEndurancePercent);
    if (pInfo->bIsNVMe && (int)pInfo->nvmeHealth.PercentageUsed > 95)
        pInfo->eWear = WorstHealth(pInfo->eWear, HEALTH_STATUS_OBSERVE);
    if (pInfo->bIsNVMe && (int)pInfo->nvmeHealth.PercentageUsed >= 100)
        pInfo->eWear = HEALTH_STATUS_BAD;

    if (pInfo->bIsNVMe)
        pInfo->eInterface = HEALTH_STATUS_GOOD;
    else if (pInfo->nCrcErrors >= 100)
        pInfo->eInterface = HEALTH_STATUS_BAD;
    else if (pInfo->nCrcErrors > 0)
        pInfo->eInterface = HEALTH_STATUS_CAUTION;
    else
        pInfo->eInterface = HEALTH_STATUS_GOOD;

    if (pInfo->bIsNVMe)
        pInfo->eTempBand = TempBandFromNvme(pInfo->nTemperatureC,
                                            pInfo->wNVMeWarnTempThreshold,
                                            pInfo->wNVMeCritTempThreshold);
    else
        pInfo->eTempBand = TempBandFromC(pInfo->nTemperatureC);
    if (cw & NVME_CRIT_WARN_TEMP_THRESHOLD)
        pInfo->eTempBand = TEMP_BAND_CRITICAL;
    if (pInfo->eTempBand == TEMP_BAND_CRITICAL)
        pInfo->eTempStatus = HEALTH_STATUS_BAD;
    else if (pInfo->eTempBand == TEMP_BAND_HIGH)
        pInfo->eTempStatus = HEALTH_STATUS_CAUTION;
    else if (pInfo->eTempBand == TEMP_BAND_ELEVATED)
        pInfo->eTempStatus = HEALTH_STATUS_OBSERVE;
    else if (pInfo->eTempBand == TEMP_BAND_NORMAL)
        pInfo->eTempStatus = HEALTH_STATUS_GOOD;
    else
        pInfo->eTempStatus = HEALTH_STATUS_UNKNOWN;

    pInfo->eMechanics = HEALTH_STATUS_UNKNOWN;

    pInfo->eHealthStatus = pInfo->eReliability;
    /* Wear is not health. Only remaining ≤5% (NAND nearly exhausted) may
     * raise overall, and never above CAUTION. */
    if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 5)
        pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                           HEALTH_STATUS_CAUTION);
    else if (pInfo->bIsNVMe && (int)pInfo->nvmeHealth.PercentageUsed > 95)
        pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                           HEALTH_STATUS_OBSERVE);
}

/* ATA 194/190 extra bytes often hold lifetime min/max. NVMe: Identify
 * WCTEMP/CCTEMP are the safe limits; log 0xCA sometimes has lifetime max. */
static void FillTempExtrema(DRIVE_INFO* p)
{
    int i;
    if (!p) return;
    p->nTempWarnC = -1;
    p->nTempCritC = -1;
    if (p->bIsNVMe) {
        p->nTempWarnC = NvmeIdentifyTempC(p->wNVMeWarnTempThreshold);
        p->nTempCritC = NvmeIdentifyTempC(p->wNVMeCritTempThreshold);
    }
    for (i = 0; i < 30; i++) {
        const SMART_ATTRIBUTE* a = &p->attrData.stAttributes[i];
        int nMin, nMax;
        if (a->bAttrID != 0xC2 && a->bAttrID != 0xBE)
            continue;
        nMin = (int)a->bRawValue[2];
        nMax = (int)a->bRawValue[4];
        if (nMax >= 1 && nMax <= 125 &&
            p->nTemperatureC > 0 && nMax >= p->nTemperatureC) {
            if (p->nTempMaxC < 0 || nMax > p->nTempMaxC)
                p->nTempMaxC = nMax;
        }
        if (nMin >= 1 && nMin <= 125 &&
            p->nTemperatureC > 0 && nMin <= p->nTemperatureC) {
            if (p->nTempMinC < 0 || nMin < p->nTempMinC)
                p->nTempMinC = nMin;
        }
    }
}

void AssessDriveHealth(DRIVE_INFO* pInfo)
{
    BYTE cw;
    BOOL bHasMediaCounters;
    int nUnknown;
    int nAttr;
    int nNormSame;
    int nConfC, nConfT, nConfM, nConfD, nConfH;

    if (!pInfo) return;

    pInfo->nConfidence         = 0;
    pInfo->nConfCompleteness   = 0;
    pInfo->nConfTransport      = 0;
    pInfo->nConfModelId        = 0;
    pInfo->nConfVendorDecoder  = 0;
    pInfo->nConfHealthAssess   = 0;
    pInfo->nEndurancePercent   = -1;
    pInfo->eReliability        = HEALTH_STATUS_UNKNOWN;
    pInfo->eInterface          = HEALTH_STATUS_UNKNOWN;
    pInfo->eMechanics          = HEALTH_STATUS_UNKNOWN;
    pInfo->eWear               = HEALTH_STATUS_UNKNOWN;
    pInfo->nGSenseEvents       = -1;
    pInfo->nLoadUnload         = -1;
    pInfo->nEmergencyRetract   = -1;
    pInfo->nWriteErrorValue    = -1;
    pInfo->nWriteErrorWorst    = -1;
    pInfo->nWriteErrorRaw      = -1;
    pInfo->nGSenseDelta        = -1;
    pInfo->nGSensePerKh        = -1;
    pInfo->nMechRisk           = -1;
    pInfo->eTempStatus         = HEALTH_STATUS_UNKNOWN;
    pInfo->eTempBand           = TEMP_BAND_UNKNOWN;
    pInfo->eNormQuality        = NORM_QUALITY_UNKNOWN;
    pInfo->eRawQuality         = NORM_QUALITY_UNKNOWN;
    pInfo->nReallocated        = -1;
    pInfo->nPendingSectors     = -1;
    pInfo->nUncorrectable      = -1;
    pInfo->nRemapEvents        = -1;
    pInfo->nCrcErrors          = -1;
    pInfo->bThresholdViolation = FALSE;
    pInfo->bPrefailNow         = FALSE;
    pInfo->bPrefailPast        = FALSE;
    pInfo->bUsageFailed        = FALSE;
    pInfo->szEvidence[0]       = '\0';
    pInfo->nHealthPercent      = -1;
    pInfo->eHealthStatus       = HEALTH_STATUS_UNKNOWN;
    pInfo->eDiskStatus         = HEALTH_STATUS_UNKNOWN;

    if (!pInfo->bSMART_Supported) {
        safe_snprintf(pInfo->szEvidence, "SMART недоступен");
        return;
    }

    pInfo->nReallocated    = AttrRawOrNeg1(pInfo, 0x05);
    pInfo->nPendingSectors = AttrRawOrNeg1(pInfo, 0xC5);
    pInfo->nUncorrectable  = AttrRawOrNeg1(pInfo, 0xC6);
    pInfo->nRemapEvents    = AttrRawOrNeg1(pInfo, 0xC4);
    pInfo->nCrcErrors      = AttrRawOrNeg1(pInfo, 0xC7);
    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
        pInfo->nGSenseEvents = AttrRawOrNeg1(pInfo, 0xBF);
        if (pInfo->nGSenseEvents < 0)
            pInfo->nGSenseEvents = AttrRawOrNeg1(pInfo, 0x0E);
        if (pInfo->nGSenseEvents < 0)
            pInfo->nGSenseEvents = AttrRawOrNeg1(pInfo, 0xDD);
        pInfo->nLoadUnload = AttrRawOrNeg1(pInfo, 0xC1);
        if (pInfo->nLoadUnload < 0)
            pInfo->nLoadUnload = AttrRawOrNeg1(pInfo, 0xE1);
        pInfo->nEmergencyRetract = AttrRawOrNeg1(pInfo, 0xC0);
    }
    pInfo->nWriteErrorValue = AttrValueOrNeg1(pInfo, 0xC8);
    pInfo->nWriteErrorWorst = AttrWorstOrNeg1(pInfo, 0xC8);
    pInfo->nWriteErrorRaw   = AttrRawOrNeg1(pInfo, 0xC8);


    /* ATA-3: thresh 0 always passes. Prefail now vs In the past vs old-age. */
    {
        int i;
        for (i = 0; i < 30; i++) {
            SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[i];
            BYTE bThresh;
            BOOL bPrefail;
            if (pAttr->bAttrID == 0) continue;
            bThresh = FindThreshold(pInfo, pAttr->bAttrID);
            if (bThresh == 0) continue;
            bPrefail = AttrIsPrefail(pAttr);
            if (AtaFailingNow(pAttr->bAttrValue, bThresh)) {
                if (bPrefail) {
                    pInfo->bPrefailNow = TRUE;
                    pInfo->bThresholdViolation = TRUE;
                } else {
                    pInfo->bUsageFailed = TRUE;
                }
            } else if (AtaFailedPast(pAttr->bAttrValue, pAttr->bWorstValue, bThresh)) {
                if (bPrefail)
                    pInfo->bPrefailPast = TRUE;
            }
        }
    }

    cw = pInfo->bIsNVMe ? pInfo->nvmeHealth.CriticalWarning : (BYTE)0;

    /* Endurance: remaining life, or -1 if the drive has no such counter. */
    if (pInfo->bIsNVMe) {
        int nLeft = 100 - (int)pInfo->nvmeHealth.PercentageUsed;
        if (nLeft < 0)   nLeft = 0;
        if (nLeft > 100) nLeft = 100;
        pInfo->nEndurancePercent = nLeft;
    } else if (pInfo->nSSDLifeLeft >= 0 && pInfo->nSSDLifeLeft <= 100) {
        pInfo->nEndurancePercent = pInfo->nSSDLifeLeft;
    } else {
        pInfo->nEndurancePercent = -1;
    }

    /* Secondary estimated health = endurance only. Never worst/thresh. */
    if (pInfo->nEndurancePercent >= 0)
        pInfo->nHealthPercent = pInfo->nEndurancePercent;
    else
        pInfo->nHealthPercent = -1;

    if (pInfo->eType == DRIVE_TYPE_USB && !pInfo->bIsNVMe &&
        pInfo->wRotationRate != 0x0001 &&
        (pInfo->wRotationRate >= 0x0401 ||
         (HasSmartAttr(pInfo, 0xC1) && HasSmartAttr(pInfo, 0x07))))
        pInfo->eType = DRIVE_TYPE_HDD;

    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
        AssessHddHealth(pInfo);
    } else {
        AssessSsdHealth(pInfo);
    }

    /* Drive's own verdict: SMART RETURN STATUS / NVMe Critical Warning.
     * Separate from our assessment (Victoria: disk Good vs Unideal). */
    if (pInfo->bIsNVMe) {
        BYTE nCw = pInfo->nvmeHealth.CriticalWarning;
        if (nCw & (NVME_CRIT_WARN_READ_ONLY | NVME_CRIT_WARN_RELIABILITY_DEGRADED))
            pInfo->eDiskStatus = HEALTH_STATUS_CRITICAL;
        else if (nCw)
            pInfo->eDiskStatus = HEALTH_STATUS_BAD;
        else
            pInfo->eDiskStatus = HEALTH_STATUS_GOOD;
    } else if (!pInfo->bGotReturnStatus)
        pInfo->eDiskStatus = HEALTH_STATUS_UNKNOWN;
    else if (pInfo->bPredictFailure)
        pInfo->eDiskStatus = HEALTH_STATUS_WARNING;
    else
        pInfo->eDiskStatus = HEALTH_STATUS_GOOD;

    nUnknown = CountUnknownAttrs(pInfo);
    nAttr    = CountPresentAttrs(pInfo);

    /* Normalized SMART usefulness: identical 100/100/50 is LOW, not "perfect". */
    nNormSame = 0;
    if (!pInfo->bIsNVMe && nAttr >= 8) {
        int i;
        int nMinV = 255, nMaxV = 0, nDistinctish = 0;
        for (i = 0; i < 30; i++) {
            const SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
            BYTE bThresh, bVal, bWorst;
            BOOL bSame;
            if (pA->bAttrID == 0) continue;
            bVal   = pA->bAttrValue;
            bWorst = pA->bWorstValue;
            bThresh = FindThreshold(pInfo, pA->bAttrID);
            bSame = FALSE;
            if (bVal == 100 && bWorst == 100 && (bThresh == 50 || bThresh == 0))
                bSame = TRUE;
            else if (bVal == bWorst && (bThresh == 0 || bThresh == 50) &&
                     (bVal == 100 || bVal == 200))
                bSame = TRUE;
            if (bSame) nNormSame++;
            if (bVal < nMinV) nMinV = bVal;
            if (bVal > nMaxV) nMaxV = bVal;
        }
        if (nNormSame * 100 >= nAttr * 70)
            pInfo->eNormQuality = NORM_QUALITY_LOW;
        else if ((nMaxV - nMinV) >= 10)
            pInfo->eNormQuality = NORM_QUALITY_HIGH;
        else
            pInfo->eNormQuality = NORM_QUALITY_MEDIUM;
        (void)nDistinctish;
    }

    /* RAW quality: known media counters vs vendor-unknown packing. */
    if (pInfo->bIsNVMe) {
        pInfo->eRawQuality = NORM_QUALITY_HIGH;
    } else {
        int nMedia = 0;
        if (pInfo->nReallocated >= 0) nMedia++;
        if (pInfo->nPendingSectors >= 0) nMedia++;
        if (pInfo->nUncorrectable >= 0) nMedia++;
        if (pInfo->nRemapEvents >= 0) nMedia++;
        if (pInfo->nCrcErrors >= 0) nMedia++;
        if (nMedia >= 3 && (nAttr == 0 || nUnknown * 2 < nAttr))
            pInfo->eRawQuality = NORM_QUALITY_HIGH;
        else if (nMedia == 0 || (nAttr > 0 && nUnknown * 100 >= nAttr * 70))
            pInfo->eRawQuality = NORM_QUALITY_LOW;
        else
            pInfo->eRawQuality = NORM_QUALITY_MEDIUM;
    }

    /* Split confidence. Unknown encodings go ONLY into vendor-decoder. */
    nConfC = 50;
    if (pInfo->bIsNVMe || HasNonZeroThreshold(pInfo))
        nConfC += 20;
    bHasMediaCounters = pInfo->bIsNVMe ||
        (pInfo->nReallocated >= 0) ||
        (pInfo->nPendingSectors >= 0) ||
        (pInfo->nUncorrectable >= 0);
    if (bHasMediaCounters)
        nConfC += 10;
    if (pInfo->nTemperatureC > 0)
        nConfC += 10;
    if (pInfo->nEndurancePercent >= 0 || pInfo->bIsNVMe)
        nConfC += 10;
    nConfC = Clamp100(nConfC);

    if (pInfo->bIsUSB) {
        nConfT = pInfo->bSMART_Supported ? 90 : 40;
    } else {
        nConfT = 100;
    }

    if (pInfo->eController != CONTROLLER_UNKNOWN)
        nConfM = 99;
    else if (FirmwareFamilyRecognized(pInfo) || IsPhisonFamily(pInfo))
        nConfM = 90;
    else
        nConfM = 70;

    if (pInfo->bIsNVMe) {
        nConfD = 100;
    } else if (nUnknown == 0) {
        nConfD = 100;
    } else {
        nConfD = 100 - 10 * nUnknown;
        if (nConfD < 40) nConfD = 40;
    }

    nConfH = 55;
    if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD &&
        MediaCountersClean(pInfo) &&
        !pInfo->bPredictFailure &&
        !pInfo->bThresholdViolation) {
        nConfH = 95;
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
        nConfH = 85;
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
        nConfH = 70;
    } else {
        nConfH = 55;
    }
    if (pInfo->eNormQuality == NORM_QUALITY_LOW)
        nConfH -= 5;
    if (nUnknown >= 3)
        nConfH -= 5;
    if (HasKnownMediaZeros(pInfo) && nConfH < 70)
        nConfH = 70;
    nConfH = Clamp100(nConfH);

    pInfo->nConfCompleteness  = nConfC;
    pInfo->nConfTransport     = nConfT;
    pInfo->nConfModelId       = nConfM;
    pInfo->nConfVendorDecoder = nConfD;
    pInfo->nConfHealthAssess  = nConfH;
    pInfo->nConfidence = (20 * nConfC + 10 * nConfT + 15 * nConfM +
                          20 * nConfD + 35 * nConfH + 50) / 100;
    pInfo->nConfidence = Clamp100(pInfo->nConfidence);
    FillTempExtrema(pInfo);
    if (pInfo->nGSenseEvents > 0 && pInfo->nGSenseDelta < 0)
        pInfo->nConfidence = Clamp100(pInfo->nConfidence - 2);
    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
        pInfo->nConfidence = 0;
        pInfo->nHealthPercent = -1;
    }

    /* One-line Russian evidence. Full words; lecture has the matrix. */
    if (pInfo->bIsNVMe) {
        char szCW[128];
        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));
        safe_snprintf(pInfo->szEvidence,
            "Критическое предупреждение: %s. Ошибки носителя: %llu.",
            szCW,
            (unsigned long long)pInfo->qwNVMeMediaErrors);
    } else {
        char szR[24], szPend[24], szU[24];
        FmtIntOrNA(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        FmtIntOrNA(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        FmtIntOrNA(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        safe_snprintf(pInfo->szEvidence,
            "Переназначенные сектора: %s  Ожидающие сектора: %s  Неисправимые сектора: %s  Нарушение порога: %s  Прогноз отказа: %s",
            szR, szPend, szU,
            pInfo->bThresholdViolation ? "да" : "нет",
            pInfo->bPredictFailure     ? "да" : "нет");
    }
}

static void LectureAddF(char* buf, int nBuf, const char* fmt, ...)
{
    char tmp[768];
    va_list ap;
    if (!buf || nBuf <= 1 || !fmt) return;
    va_start(ap, fmt);
    (void)_vsnprintf(tmp, sizeof(tmp), fmt, ap);
    tmp[sizeof(tmp) - 1] = '\0';
    va_end(ap);
    LectureAdd(buf, nBuf, tmp);
}

static void LectureAddSplitConfidence(char* buf, int nBuf, const DRIVE_INFO* p)
{
    LectureAddF(buf, nBuf, "Достоверность оценки: %d%%\r\n\r\n", p->nConfidence);
    LectureAdd(buf, nBuf, "Причины:\r\n");
    if (p->nConfModelId >= 90)
        LectureAdd(buf, nBuf, "  • модель определена уверенно\r\n");
    else
        LectureAdd(buf, nBuf, "  • модель определена не полностью\r\n");
    if (p->bSMART_Supported)
        LectureAdd(buf, nBuf, "  • SMART полностью прочитан\r\n");
    else
        LectureAdd(buf, nBuf, "  • SMART прочитан не полностью\r\n");
    if (p->nConfVendorDecoder >= 90)
        LectureAdd(buf, nBuf, "  • vendor decoder найден\r\n");
    else
        LectureAdd(buf, nBuf, "  • vendor decoder неполный\r\n");
    if (p->nGSenseEvents > 0 && p->nGSenseDelta < 0)
        LectureAdd(buf, nBuf, "  • динамика G-Sense отсутствует\r\n");
    LectureAdd(buf, nBuf, "\r\n");
}

static void LectureMatrixRow(char* buf, int nBuf,
                             const char* evid, const char* val, const char* infl)
{
    char c1[72], c2[48], c3[40];
    PadUtf8(c1, (int)sizeof(c1), evid, 28);
    PadUtf8(c2, (int)sizeof(c2), val, 22);
    PadUtf8(c3, (int)sizeof(c3), infl, 16);
    LectureAddF(buf, nBuf, "  %s%s%s\r\n", c1, c2, c3);
}

static void LectureFmtInt(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "нет данных");
    else
        safe_snprintf_n(buf, nBuf, "%d", v);
}

static void LectureAddUnknownIdList(char* buf, int nBuf, const DRIVE_INFO* pInfo)
{
    int ui, nUnk = 0;
    char szIds[192];
    szIds[0] = '\0';
    for (ui = 0; ui < 30; ui++) {
        ATTR_DECODE dec;
        BYTE id = pInfo->attrData.stAttributes[ui].bAttrID;
        char piece[16];
        if (id == 0) continue;
        GetAttrDecode(id, pInfo, &dec);
        if (dec.eState != ATTR_DECODE_UNKNOWN) continue;
        safe_snprintf(piece, "%s%d", nUnk ? ", " : "", id);
        {
            size_t used = strlen(szIds);
            size_t rem = sizeof(szIds) - used;
            if (rem > 1)
                safe_snprintf_n(szIds + used, (int)rem, "%s", piece);
        }
        nUnk++;
    }
    if (nUnk > 0) {
        LectureAddF(buf, nBuf,
            "Атрибуты %s не оцениваются: нет доверенного профиля RAW-кодировки "
            "для этого контроллера/прошивки. Большое RAW-значение не доказывает сбои. "
            "Unknown ≠ Bad.\r\n",
            szIds);
    }
}


static const char* ConfidenceBandName(int n)
{
    if (n >= 80) return "ВЫСОКАЯ";
    if (n >= 55) return "СРЕДНЯЯ";
    if (n > 0)   return "НИЗКАЯ";
    return "нет данных";
}

static void LectureAddFact(char* buf, int nBuf, const char* name, const char* value)
{
    char left[48];
    PadUtf8(left, (int)sizeof(left), name, 24);
    LectureAddF(buf, nBuf, "%s%s\r\n", left, value);
}

static void FmtCountPlain(char* sz, int nSz, int v)
{
    if (v < 0)
        safe_snprintf_n(sz, nSz, "нет данных");
    else
        safe_snprintf_n(sz, nSz, "%d", v);
}

static const char* HealthStatusMark(DRIVE_HEALTH_STATUS e)
{
    switch (e) {
    case HEALTH_STATUS_GOOD:     return "\xF0\x9F\x9F\xA2 "; /* green */
    case HEALTH_STATUS_OBSERVE:  return "\xF0\x9F\x9F\xA1 "; /* yellow */
    case HEALTH_STATUS_CAUTION:  return "\xF0\x9F\x9F\xA0 "; /* orange */
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:  return "\xF0\x9F\x94\xB4 "; /* red */
    case HEALTH_STATUS_CRITICAL: return "\xE2\x9A\xAB ";     /* black */
    default:                     return "";
    }
}

static void LectureAddDualStatus(char* szBuf, int nBufLen, const DRIVE_INFO* pInfo)
{
    LectureAddF(szBuf, nBufLen, "Диск: %s%s\r\nОценка: %s%s\r\n\r\n",
                HealthStatusMark(pInfo->eDiskStatus),
                GetDiskStatusName(pInfo),
                HealthStatusMark(pInfo->eHealthStatus),
                GetHealthStatusName(pInfo->eHealthStatus));
    if (pInfo->bIsUSB && pInfo->bSMART_Supported &&
        !pInfo->bGotReturnStatus && !pInfo->bIsNVMe)
        LectureAdd(szBuf, nBufLen,
            "USB-мост не отдаёт SMART RETURN STATUS — это не отказ диска. "
            "Оценка ниже по таблице SMART.\r\n\r\n");
}

static void FormatHddLecturePlain(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    int r, pend, u;

    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    LectureAddDualStatus(szBuf, nBufLen, pInfo);

    if (!pInfo->bSMART_Supported) {
        LectureAdd(szBuf, nBufLen,
            "SMART недоступен. Без исходных данных оценка не ставится.\r\n");
        return;
    }

    r = pInfo->nReallocated;
    pend = pInfo->nPendingSectors;
    u = pInfo->nUncorrectable;

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        LectureAdd(szBuf, nBufLen,
            "Критических проблем не обнаружено.\r\n"
            "Prefail-атрибуты в норме, повреждение поверхности не подтверждено.\r\n");
        break;
    case HEALTH_STATUS_OBSERVE:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        {
            BOOL bSaid = FALSE;
            if (pInfo->bPrefailPast) {
                LectureAdd(szBuf, nBufLen,
                    "Prefail-атрибут раньше опускался до порога. "
                    "Сейчас значение снова выше порога.\r\n");
                bSaid = TRUE;
            }
            if (pInfo->bUsageFailed) {
                LectureAdd(szBuf, nBufLen,
                    "Атрибут износа (old-age) на пороге. Это износ, не прогноз отказа.\r\n");
                bSaid = TRUE;
            }
            if (!bSaid)
                LectureAdd(szBuf, nBufLen,
                    "Есть факторы риска, повреждение поверхности не подтверждено.\r\n");
            LectureAdd(szBuf, nBufLen, "\r\n");
        }
        LectureAdd(szBuf, nBufLen, "Признаков повреждения поверхности:\r\n");
        LectureAddF(szBuf, nBufLen, "  %s Переназначенные: %d\r\n",
                    (r <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", r < 0 ? 0 : r);
        LectureAddF(szBuf, nBufLen, "  %s Ожидающие: %d\r\n",
                    (pend <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", pend < 0 ? 0 : pend);
        LectureAddF(szBuf, nBufLen, "  %s Неисправимые: %d\r\n\r\n",
                    (u <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", u < 0 ? 0 : u);
        LectureAdd(szBuf, nBufLen, "Рекомендация: Следить за динамикой SMART.\r\n");
        break;
    case HEALTH_STATUS_CAUTION:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        LectureAdd(szBuf, nBufLen,
            "Есть реальные признаки деградации носителя.\r\n\r\n");
        if (pend > 0)
            LectureAddF(szBuf, nBufLen, "Ожидающие сектора: %d\r\n", pend);
        if (r > 0)
            LectureAddF(szBuf, nBufLen, "Переназначенные сектора: %d\r\n", r);
        if (pInfo->nRemapEvents > 0)
            LectureAddF(szBuf, nBufLen, "События переназначения: %d\r\n", pInfo->nRemapEvents);
        LectureAdd(szBuf, nBufLen,
            "\r\nРекомендация: Держать резервную копию и следить за SMART.\r\n");
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        LectureAdd(szBuf, nBufLen,
            "Обнаружены признаки деградации носителя.\r\n\r\n");
        if (pend > 0)
            LectureAddF(szBuf, nBufLen, "Ожидающие сектора: %d\r\n", pend);
        if (r > 0)
            LectureAddF(szBuf, nBufLen, "Переназначенные сектора: %d\r\n", r);
        if (u > 0)
            LectureAddF(szBuf, nBufLen, "Неисправимые сектора: %d\r\n", u);
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 > 0)
                LectureAddF(szBuf, nBufLen, "Неисправимые ошибки (187): %d\r\n", n187);
        }
        LectureAdd(szBuf, nBufLen,
            "\r\nРекомендация: Немедленно создать резервную копию.\r\n");
        break;
    case HEALTH_STATUS_CRITICAL:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        LectureAdd(szBuf, nBufLen,
            "Высокий риск отказа (самотест failed или быстрое ухудшение).\r\n\r\n");
        LectureAdd(szBuf, nBufLen,
            "Рекомендация: Немедленно копировать данные, диск к замене.\r\n");
        break;
    default:
        break;
    }

    if (pInfo->nGSenseEvents >= 0 || pInfo->nEmergencyRetract >= 0 ||
        pInfo->nLoadUnload >= 0) {
        char szC1[32];
        LectureAdd(szBuf, nBufLen, "\r\nМеханика\r\n");
        if (pInfo->nGSenseEvents >= 0)
            LectureAddF(szBuf, nBufLen, "  G-Sense: %d событий\r\n", pInfo->nGSenseEvents);
        if (pInfo->nEmergencyRetract >= 0)
            LectureAddF(szBuf, nBufLen, "  Аварийные парковки: %d\r\n", pInfo->nEmergencyRetract);
        if (pInfo->nLoadUnload >= 0) {
            FormatUintGrouped((unsigned long)pInfo->nLoadUnload, szC1, (int)sizeof(szC1));
            LectureAddF(szBuf, nBufLen, "  Циклы парковки головок: %s\r\n", szC1);
        }
    }
    if (pInfo->dwPowerOnHours > 0) {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        LectureAddF(szBuf, nBufLen, "\r\nНаработка: %s\r\n", szPoh);
    }
}

static void FormatSsdLecturePlain(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = 0;
    LectureAddDualStatus(szBuf, nBufLen, pInfo);

    if (!pInfo->bSMART_Supported) {
        LectureAdd(szBuf, nBufLen,
            "SMART недоступен. Без исходных данных оценка не ставится.\r\n");
        return;
    }

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        LectureAdd(szBuf, nBufLen,
            "Критических проблем не обнаружено.\r\n");
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 20)
            LectureAddF(szBuf, nBufLen,
                "Носитель, интерфейс и температура в норме. "
                "Остаток ресурса %d%% — износ NAND, не здоровье.\r\n",
                pInfo->nEndurancePercent);
        else
            LectureAdd(szBuf, nBufLen,
                "Носитель, ресурс, интерфейс и температура в норме.\r\n");
        break;
    case HEALTH_STATUS_OBSERVE:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        if (pInfo->eWear == HEALTH_STATUS_OBSERVE && pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "Остаток ресурса %d%%. Это ещё не отказ, но запас NAND снижается.\r\n",
                pInfo->nEndurancePercent);
        else if (pInfo->eTempStatus == HEALTH_STATUS_OBSERVE ||
                 pInfo->eTempStatus == HEALTH_STATUS_CAUTION)
            LectureAdd(szBuf, nBufLen,
                "Повышенная температура. Повреждение носителя не подтверждено.\r\n");
        else
            LectureAdd(szBuf, nBufLen,
                "Есть факторы риска, повреждение носителя не подтверждено.\r\n");
        LectureAdd(szBuf, nBufLen,
            "\r\nРекомендация: Следить за SMART и держать резервную копию.\r\n");
        break;
    case HEALTH_STATUS_CAUTION:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 5)
            LectureAddF(szBuf, nBufLen,
                "Остаток ресурса %d%%. Запас NAND почти исчерпан — "
                "это износ, не ошибка носителя.\r\n",
                pInfo->nEndurancePercent);
        else if (pInfo->eWear == HEALTH_STATUS_CAUTION &&
                 pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "Остаток ресурса %d%%.\r\n", pInfo->nEndurancePercent);
        if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, "Переназначенные сектора: %d\r\n", pInfo->nReallocated);
        if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen, "Ожидающие сектора: %d\r\n", pInfo->nPendingSectors);
        if (pInfo->qwNVMeMediaErrors > 0)
            LectureAddF(szBuf, nBufLen, "Ошибки носителя NVMe: %llu\r\n",
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureAdd(szBuf, nBufLen,
            "\r\nРекомендация: Держать резервную копию и следить за SMART.\r\n");
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        LectureAdd(szBuf, nBufLen,
            "Обнаружены признаки деградации носителя или ресурса.\r\n");
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 5)
            LectureAddF(szBuf, nBufLen, "Остаток ресурса %d%%.\r\n", pInfo->nEndurancePercent);
        if (pInfo->qwNVMeMediaErrors > 0)
            LectureAddF(szBuf, nBufLen, "Ошибки носителя: %llu\r\n",
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureAdd(szBuf, nBufLen,
            "\r\nРекомендация: Немедленно создать резервную копию.\r\n");
        break;
    case HEALTH_STATUS_CRITICAL:
        LectureAdd(szBuf, nBufLen, "Причина:\r\n");
        LectureAdd(szBuf, nBufLen,
            "Контроллер сообщает о серьёзной деградации (read-only или reliability).\r\n\r\n");
        LectureAdd(szBuf, nBufLen,
            "Рекомендация: Немедленно копировать данные, диск к замене.\r\n");
        break;
    default:
        break;
    }

    if (pInfo->nEndurancePercent >= 0)
        LectureAddF(szBuf, nBufLen, "\r\nРесурс: %d%%\r\n", pInfo->nEndurancePercent);
    if (pInfo->dwPowerOnHours > 0) {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        LectureAddF(szBuf, nBufLen, "Наработка: %s\r\n", szPoh);
    }
}

void FormatHealthLecturePlain(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    char szPoh[64], szTemp[96], szWear[64], szCyc[32];
    char szR[24], szPend[24], szU[24], szCrc[24];
    char szSmart[48];
    const char* szHead;

    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (nBufLen < 2) return;

    if (!pInfo) {
        safe_snprintf_n(szBuf, nBufLen, "Нет выбранного диска");
        return;
    }

    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
        FormatHddLecturePlain(pInfo, szBuf, nBufLen);
        return;
    }
    if (pInfo->bIsNVMe || pInfo->eType == DRIVE_TYPE_SSD_SATA ||
        pInfo->eType == DRIVE_TYPE_M2_SATA) {
        FormatSsdLecturePlain(pInfo, szBuf, nBufLen);
        return;
    }

    LectureAddDualStatus(szBuf, nBufLen, pInfo);

    if (!pInfo->bSMART_Supported) {
        if (pInfo->bIsUSB && IsLikelyUsbFlashDrive(pInfo)) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Это USB-флешка: контроллер обычно не отдаёт атрибуты SMART, "
                "поэтому состояние не оценивается. Процент здоровья не выдумывается.\r\n");
        } else if (pInfo->bIsUSB) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. USB-мост или корпус не пропускает команды SMART. "
                "Без исходных данных оценка не ставится.\r\n");
        } else if (pInfo->bIsNVMe) {
            LectureAddF(szBuf, nBufLen,
                "Не удалось прочитать журнал здоровья NVMe (код ошибки Windows %lu). "
                "Без журнала оценка не ставится.\r\n",
                (unsigned long)pInfo->dwErrNvmeProtocol);
        } else {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Без исходных данных оценка не ставится. "
                "Запустите программу от имени администратора и нажмите «Обновить».\r\n");
        }
        return;
    }

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        szHead = "Критических проблем не обнаружено.";
        break;
    case HEALTH_STATUS_OBSERVE:
        szHead = "Есть факторы риска, повреждение поверхности не подтверждено.";
        break;
    case HEALTH_STATUS_CAUTION:
        szHead = "Есть признаки, требующие наблюдения, но критической деградации не обнаружено.";
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        szHead = "Обнаружены признаки физической деградации.";
        break;
    case HEALTH_STATUS_CRITICAL:
        szHead = "Высокий риск отказа.";
        break;
    default:
        szHead = "Критических проблем не обнаружено.";
        break;
    }
    LectureAddF(szBuf, nBufLen, "%s\r\n\r\n", szHead);

    if (pInfo->bPredictFailure)
        lstrcpynA(szSmart, "сбой", sizeof(szSmart));
    else
        lstrcpynA(szSmart, "в норме", sizeof(szSmart));

    LectureAdd(szBuf, nBufLen, "Носитель\r\n");
    LectureAddFact(szBuf, nBufLen, "SMART", szSmart);
    if (pInfo->bIsNVMe) {
        char szMedia[32], szSpare[32];
        safe_snprintf(szMedia, "%llu", (unsigned long long)pInfo->qwNVMeMediaErrors);
        safe_snprintf(szSpare, "%d%%", (int)pInfo->nvmeHealth.AvailableSpare);
        LectureAddFact(szBuf, nBufLen, "Ошибки носителя", szMedia);
        LectureAddFact(szBuf, nBufLen, "Запас блоков", szSpare);
    } else {
        char szRemap[24];
        FmtCountPlain(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        FmtCountPlain(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        FmtCountPlain(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        FmtCountPlain(szRemap,(int)sizeof(szRemap), pInfo->nRemapEvents);
        LectureAddFact(szBuf, nBufLen, "Переназначенные", szR);
        LectureAddFact(szBuf, nBufLen, "Ожидающие", szPend);
        LectureAddFact(szBuf, nBufLen, "Неисправимые", szU);
        LectureAddFact(szBuf, nBufLen, "События переназначения", szRemap);
        if (pInfo->nWriteErrorValue >= 0 && pInfo->nWriteErrorValue <= 10) {
            char szWe[64];
            if (pInfo->nWriteErrorValue <= 1)
                safe_snprintf(szWe, "значение %d (шкала исчерпана)", pInfo->nWriteErrorValue);
            else
                safe_snprintf(szWe, "значение %d", pInfo->nWriteErrorValue);
            LectureAddFact(szBuf, nBufLen, "Ошибки записи (200)", szWe);
        }
    }

    LectureAdd(szBuf, nBufLen, "\r\nИнтерфейс\r\n");
    if (pInfo->nCrcErrors >= 0) {
        FmtCountPlain(szCrc, (int)sizeof(szCrc), pInfo->nCrcErrors);
        LectureAddFact(szBuf, nBufLen, "Ошибки CRC", szCrc);
    } else if (pInfo->bIsNVMe) {
        LectureAddFact(szBuf, nBufLen, "Ссылка", "в норме");
    } else {
        LectureAddFact(szBuf, nBufLen, "Ошибки CRC", "нет данных");
    }

    if (pInfo->eMechanics != HEALTH_STATUS_UNKNOWN || pInfo->nGSenseEvents >= 0) {
        char szGs[40], szC0[32], szC1[32], szMech[48];
        LectureAdd(szBuf, nBufLen, "\r\nМеханика\r\n");
        if (pInfo->eMechanics == HEALTH_STATUS_GOOD)
            lstrcpynA(szMech, "НОРМА", sizeof(szMech));
        else
            lstrcpynA(szMech, GetHealthStatusName(pInfo->eMechanics), sizeof(szMech));
        LectureAddFact(szBuf, nBufLen, "Состояние", szMech);
        if (pInfo->nGSenseEvents >= 0) {
            safe_snprintf(szGs, "%d событий", pInfo->nGSenseEvents);
            LectureAddFact(szBuf, nBufLen, "G-Sense", szGs);
        }
        if (pInfo->nEmergencyRetract >= 0) {
            safe_snprintf(szC0, "%d", pInfo->nEmergencyRetract);
            LectureAddFact(szBuf, nBufLen, "Аварийные парковки", szC0);
        }
        if (pInfo->nLoadUnload >= 0) {
            FormatUintGrouped((unsigned long)pInfo->nLoadUnload, szC1, (int)sizeof(szC1));
            LectureAddFact(szBuf, nBufLen, "Циклы парковки головок", szC1);
        }
        if (pInfo->nGSenseDelta >= 0) {
            char szD[48];
            safe_snprintf(szD, "%+d", pInfo->nGSenseDelta);
            LectureAddFact(szBuf, nBufLen, "Динамика", szD);
        }
        if (pInfo->eMechanics == HEALTH_STATUS_OBSERVE) {
            LectureAdd(szBuf, nBufLen, "\r\n");
            LectureAdd(szBuf, nBufLen,
                "G-Sense фиксирует события, связанные с ударом/вибрацией. "
                "Признаков повреждения поверхности на текущем SMART-снимке нет: 5/196/197/198 = 0.\r\n");
        }
    }

    FormatTempLecture(pInfo, szTemp, (int)sizeof(szTemp));
    LectureAdd(szBuf, nBufLen, "\r\n");
    LectureAddFact(szBuf, nBufLen, "Температура", szTemp);

    if (pInfo->eType != DRIVE_TYPE_HDD) {
        LectureAdd(szBuf, nBufLen, "\r\n");
        if (pInfo->nEndurancePercent >= 0)
            safe_snprintf(szWear, "%d%%", pInfo->nEndurancePercent);
        else
            lstrcpynA(szWear, "нет достоверного показателя", sizeof(szWear));
        LectureAddFact(szBuf, nBufLen, "Остаток ресурса", szWear);
    }

    LectureAdd(szBuf, nBufLen, "\r\n");
    FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
    LectureAddFact(szBuf, nBufLen, "Наработка", szPoh);
    if (pInfo->dwPowerCycleCount > 0) {
        FormatUintGrouped((unsigned long)pInfo->dwPowerCycleCount, szCyc, (int)sizeof(szCyc));
        LectureAddFact(szBuf, nBufLen, "Циклы включения", szCyc);
    } else {
        LectureAddFact(szBuf, nBufLen, "Циклы включения", "нет данных");
    }

    LectureAdd(szBuf, nBufLen, "\r\nПричина оценки\r\n\r\n");

    if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
        if (pInfo->eMechanics == HEALTH_STATUS_OBSERVE) {
            LectureAdd(szBuf, nBufLen,
                "Носитель и интерфейс в норме. Механика — риск: G-Sense фиксирует события, "
                "связанные с ударом/вибрацией, но переназначенных, ожидающих и неисправимых секторов нет. "
                "Абсолютное число событий само по себе не снижает оценку.\r\n\r\n");
        } else if (pInfo->bIsNVMe) {
            LectureAdd(szBuf, nBufLen,
                "Диск находится в хорошем состоянии. Журнал здоровья NVMe не сообщает об ошибках носителя "
                "и критических предупреждениях. Запас блоков в норме.\r\n\r\n");
        } else {
            LectureAdd(szBuf, nBufLen,
                "Диск находится в хорошем состоянии. SMART не показывает признаков деградации носителя: "
                "нет переназначенных, ожидающих или неисправимых секторов, а также ошибок интерфейса.\r\n\r\n");
        }
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
        if (pInfo->eMechanics == HEALTH_STATUS_CAUTION || pInfo->eMechanics == HEALTH_STATUS_BAD)
            LectureAdd(szBuf, nBufLen,
                "Есть связанная улика: события G-Sense совпадают с проблемными секторами. "
                "Это не штраф за абсолютное число G-Sense, а корреляция механики и поверхности.\r\n\r\n");
        else
            LectureAdd(szBuf, nBufLen,
                "Есть признаки, требующие наблюдения, но критической деградации носителя не обнаружено.\r\n\r\n");
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_BAD ||
               pInfo->eHealthStatus == HEALTH_STATUS_WARNING) {
        if (pInfo->nWriteErrorValue >= 0 && pInfo->nWriteErrorValue <= 1)
            LectureAddF(szBuf, nBufLen,
                "Частота ошибок записи (ID 200) исчерпала нормализованную шкалу: значение %d, худший %d. "
                "Это признак проблем поверхности или головок, даже если переназначенных секторов ещё нет.\r\n\r\n",
                pInfo->nWriteErrorValue,
                pInfo->nWriteErrorWorst >= 0 ? pInfo->nWriteErrorWorst : pInfo->nWriteErrorValue);
        else if (pInfo->nGSenseEvents > 0 &&
            (pInfo->nPendingSectors > 0 || pInfo->nReallocated > 0 || pInfo->nUncorrectable > 0))
            LectureAdd(szBuf, nBufLen,
                "Есть признаки возможного повреждения носителя. Зарегистрированы события G-Sense "
                "и одновременно появились проблемные сектора.\r\n\r\n");
        else if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen,
                "Обнаружены признаки физической деградации: ожидающие сектора %d.\r\n\r\n",
                pInfo->nPendingSectors);
        else if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen,
                "Обнаружены признаки физической деградации: переназначенные сектора %d.\r\n\r\n",
                pInfo->nReallocated);
        else if (pInfo->qwNVMeMediaErrors > 0)
            LectureAddF(szBuf, nBufLen,
                "Обнаружены признаки физической деградации: ошибки носителя %llu.\r\n\r\n",
                (unsigned long long)pInfo->qwNVMeMediaErrors);
        else if (pInfo->bPredictFailure)
            LectureAdd(szBuf, nBufLen,
                "SMART сообщает о прогнозе отказа. Диск нужно копировать и заменять.\r\n\r\n");
        else
            LectureAdd(szBuf, nBufLen,
                "Обнаружены признаки физической деградации по SMART.\r\n\r\n");
    }

    if (pInfo->eType != DRIVE_TYPE_HDD && pInfo->nEndurancePercent < 0)
        LectureAdd(szBuf, nBufLen,
            "Остаток ресурса SSD: нет достоверного показателя.\r\n");
}

void FormatHealthLecture(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    FormatHealthLecturePlain(pInfo, szBuf, nBufLen);
}

void FormatHealthLectureExpert(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    BYTE cw;
    BOOL bSpareLow;
    const char* szState;
    char szPoh[64];
    char szTempBand[96];
    int nUnknown;
    int nPos, nCritFail, nMediaDeg, nUnresolved;
    int nC0, nC3, nB0, nB1, nF5, nShock;

    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (nBufLen < 2) return;

    if (!pInfo) {
        safe_snprintf_n(szBuf, nBufLen, "Нет выбранного диска");
        return;
    }

    szState = GetHealthStatusName(pInfo->eHealthStatus);
    LectureAddDualStatus(szBuf, nBufLen, pInfo);
    LectureAddF(szBuf, nBufLen, "Сводка: %s\r\n", szState);

    if (!pInfo->bSMART_Supported) {
        if (pInfo->bIsUSB && IsLikelyUsbFlashDrive(pInfo)) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Это USB-флешка: контроллер флешки обычно не отдаёт "
                "атрибуты SMART, поэтому состояние не оценивается. Числа здоровья не выдумываются.\r\n");
        } else if (pInfo->bIsUSB) {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. USB-мост или корпус не пропускает команды SMART. "
                "Без исходных данных оценка не ставится — программа не подставляет фиктивные проценты.\r\n");
        } else if (pInfo->bIsNVMe) {
            LectureAddF(szBuf, nBufLen,
                "Не удалось прочитать журнал здоровья NVMe (код ошибки Windows %lu). "
                "Возможные причины: нет прав администратора, драйвер не поддерживает запрос, "
                "или устройство занято. Без журнала здоровья проценты не выдумываются.\r\n",
                (unsigned long)pInfo->dwErrNvmeProtocol);
        } else {
            LectureAdd(szBuf, nBufLen,
                "SMART недоступен. Без исходных данных оценка не ставится — "
                "программа не подставляет фиктивные проценты. Запустите программу "
                "от имени администратора и нажмите «Обновить».\r\n");
        }
        return;
    }

    if (pInfo->eType != DRIVE_TYPE_HDD)
        LectureAddSplitConfidence(szBuf, nBufLen, pInfo);

    cw = pInfo->bIsNVMe ? pInfo->nvmeHealth.CriticalWarning : (BYTE)0;
    bSpareLow = FALSE;
    if (pInfo->bIsNVMe) {
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        if (nSpare < nSpareTh)
            bSpareLow = TRUE;
        if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
            bSpareLow = TRUE;
    }

    FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
    FormatTempLecture(pInfo, szTempBand, (int)sizeof(szTempBand));

    nUnknown = CountUnknownAttrs(pInfo);
    nC0 = AttrRawOrNeg1(pInfo, 0xC0);
    nC3 = AttrRawOrNeg1(pInfo, 0xC3);
    nB0 = AttrRawOrNeg1(pInfo, 0xB0);
    nB1 = AttrRawOrNeg1(pInfo, 0xB1);
    nF5 = AttrRawOrNeg1(pInfo, 0xF5);
    nShock = AttrRawOrNeg1(pInfo, 0xBF);
    if (nShock < 0) nShock = AttrRawOrNeg1(pInfo, 0x0E);
    if (nShock < 0) nShock = AttrRawOrNeg1(pInfo, 0xDD);

    nPos = 0;
    nCritFail = 0;
    nMediaDeg = 0;
    nUnresolved = nUnknown;

    if (!pInfo->bPredictFailure) nPos++;
    if (pInfo->nReallocated == 0) nPos++;
    if (pInfo->nPendingSectors == 0) nPos++;
    if (pInfo->nUncorrectable == 0) nPos++;
    if (pInfo->nRemapEvents == 0) nPos++;
    {
        int n187c = AttrRawOrNeg1(pInfo, 0xBB);
        if (n187c == 0) nPos++;
        if (n187c > 0) nMediaDeg++;
    }
    if (pInfo->nEndurancePercent >= 0) nPos++;
    if (pInfo->bIsNVMe && pInfo->qwNVMeMediaErrors == 0) nPos++;
    if (pInfo->bIsNVMe && !bSpareLow) nPos++;

    if (pInfo->bPredictFailure || pInfo->bThresholdViolation ||
        (cw & NVME_CRIT_WARN_READ_ONLY) ||
        (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED))
        nCritFail++;
    if (pInfo->nReallocated > 0 || pInfo->nPendingSectors > 0 ||
        pInfo->nUncorrectable > 0 || pInfo->qwNVMeMediaErrors > 0)
        nMediaDeg++;

    if (!pInfo->bIsNVMe && pInfo->eNormQuality != NORM_QUALITY_UNKNOWN) {
        LectureAddF(szBuf, nBufLen, "Качество нормализованных SMART: %s\r\n",
                    QualityNameRu(pInfo->eNormQuality, FALSE));
        LectureAddF(szBuf, nBufLen, "Качество vendor RAW: %s\r\n",
                    QualityNameRu(pInfo->eRawQuality, TRUE));
        if (pInfo->eNormQuality == NORM_QUALITY_LOW)
            LectureAdd(szBuf, nBufLen,
                "Одинаковые Value/Worst/Threshold (100/100/50) не означают, что всё идеально — "
                "у этой прошивки нормализованные числа слабо различают состояние.\r\n");
        LectureAdd(szBuf, nBufLen, "\r\n");
    }

    if (pInfo->bIsNVMe) {
        char szCW[192];
        char szSpare[32], szMedia[32], szLife[32];
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;
        int nUsed    = (int)pInfo->nvmeHealth.PercentageUsed;

        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));

        LectureAdd(szBuf, nBufLen, "Основные признаки:\r\n");
        if (cw == 0) {
            LectureAdd(szBuf, nBufLen, "  ✓ Критическое предупреждение: нет\r\n");
        } else {
            LectureAddF(szBuf, nBufLen, "  ✗ Критическое предупреждение: %s\r\n", szCW);
        }
        if (!bSpareLow)
            LectureAddF(szBuf, nBufLen, "  ✓ Доступный запас: %d%%\r\n", nSpare);
        else
            LectureAddF(szBuf, nBufLen, "  ✗ Доступный запас: %d%% (порог %d%%)\r\n",
                        nSpare, nSpareTh);
        if (pInfo->qwNVMeMediaErrors == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Ошибки носителя: 0\r\n");
        else
            LectureAddF(szBuf, nBufLen, "  ✗ Ошибки носителя: %llu\r\n",
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "  %s Заявленный остаток ресурса: %d%%   (износ NAND, не здоровье)\r\n",
                pInfo->nEndurancePercent <= 20 ? "\xE2\x9A\xA0" : "\xE2\x9C\x93",
                pInfo->nEndurancePercent);

        LectureAdd(szBuf, nBufLen, "\r\nКонтекст (не штраф):\r\n");
        LectureAddF(szBuf, nBufLen, "  Температура: %s. ", szTempBand);
        LectureAddF(szBuf, nBufLen,
            "Время на высокой температуре: предупреждение %lu мин, критическая %lu мин.\r\n",
            (unsigned long)pInfo->nvmeHealth.WarningCompTempTime,
            (unsigned long)pInfo->nvmeHealth.CriticalCompTempTime);
        LectureAddF(szBuf, nBufLen,
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n", szPoh);
        LectureAddF(szBuf, nBufLen,
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            (unsigned long)pInfo->dwPowerCycleCount);
        LectureAddF(szBuf, nBufLen,
            "  Небезопасные выключения: %llu. Журнал питания, не штраф.\r\n",
            (unsigned long long)pInfo->qwNVMeUnsafeShutdowns);

        LectureAdd(szBuf, nBufLen, "\r\nМатрица улик:\r\n");
        LectureMatrixRow(szBuf, nBufLen, "Улика", "Значение", "Влияние");
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         pInfo->bPredictFailure ? "FAIL" : "PASS",
                         pInfo->bPredictFailure ? "−" : "+");
        safe_snprintf(szSpare, "%d%%", nSpare);
        LectureMatrixRow(szBuf, nBufLen, "Доступный запас", szSpare,
                         bSpareLow ? "−" : "+");
        safe_snprintf(szMedia, "%llu", (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureMatrixRow(szBuf, nBufLen, "Ошибки носителя", szMedia,
                         pInfo->qwNVMeMediaErrors == 0 ? "+" : "−");
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, "Остаток ресурса", szLife,
                             pInfo->nEndurancePercent <= 20 ? "0" : "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Температура", szT, "0");
        }
        {
            char szH[64];
            FormatPowerOnHours(pInfo->dwPowerOnHours, szH, (int)sizeof(szH));
            LectureMatrixRow(szBuf, nBufLen, "Наработка", szH, "0");
        }
        LectureMatrixRow(szBuf, nBufLen, "Контроллер",
                         DriveControllerLabel(pInfo),
                         pInfo->eController == CONTROLLER_UNKNOWN ? "−уверенность" : "0");

        LectureAdd(szBuf, nBufLen, "\r\nИтог:\r\n");
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
            LectureAdd(szBuf, nBufLen, "  ХОРОШО, потому что:\r\n");
        } else if (pInfo->eHealthStatus == HEALTH_STATUS_OBSERVE) {
            LectureAdd(szBuf, nBufLen, "  РИСК, потому что:\r\n");
        } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
            LectureAdd(szBuf, nBufLen, "  ВНИМАНИЕ, потому что:\r\n");
        } else {
            LectureAdd(szBuf, nBufLen, "  ПЛОХО, потому что:\r\n");
        }
        LectureAddF(szBuf, nBufLen, "    %d сильных положительных признаков\r\n", nPos);
        LectureAddF(szBuf, nBufLen, "    %d критических отказов\r\n", nCritFail);
        LectureAddF(szBuf, nBufLen, "    %d признаков деградации носителя\r\n", nMediaDeg);
        LectureAdd(szBuf, nBufLen,
            "Использованный ресурс — износ NAND, его не смешивают с запасом и ошибками. "
            "Мы не превращаем использованный ресурс, запас, критическое предупреждение, "
            "ошибки носителя и температуру в один процент здоровья.\r\n");
        return;
    }

    /* ATA / SATA SMART */
    {
        char szR[24], szPend[24], szU[24], szCrc[24], szRemap[24], szLife[24];
        LectureFmtInt(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        LectureFmtInt(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        LectureFmtInt(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        LectureFmtInt(szCrc,  (int)sizeof(szCrc),  pInfo->nCrcErrors);
        LectureFmtInt(szRemap,(int)sizeof(szRemap),pInfo->nRemapEvents);

        LectureAdd(szBuf, nBufLen, "Основные признаки:\r\n");
        if (pInfo->nReallocated == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Переназначенные сектора: 0\r\n");
        else if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Переназначенные сектора: %d\r\n", pInfo->nReallocated);
        if (pInfo->nPendingSectors == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Ожидающие сектора: 0\r\n");
        else if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Ожидающие сектора: %d\r\n", pInfo->nPendingSectors);
        if (pInfo->nUncorrectable == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ Неисправимые сектора: 0\r\n");
        else if (pInfo->nUncorrectable > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ Неисправимые сектора: %d\r\n", pInfo->nUncorrectable);
        if (pInfo->nRemapEvents == 0)
            LectureAdd(szBuf, nBufLen, "  ✓ События переназначения: 0\r\n");
        else if (pInfo->nRemapEvents > 0)
            LectureAddF(szBuf, nBufLen, "  ✗ События переназначения: %d\r\n", pInfo->nRemapEvents);
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 == 0)
                LectureAdd(szBuf, nBufLen, "  ✓ Неисправимые ошибки (187): 0\r\n");
            else if (n187 > 0)
                LectureAddF(szBuf, nBufLen, "  ✗ Неисправимые ошибки (187): %d\r\n", n187);
        }
        if (pInfo->nWriteErrorValue >= 0 && pInfo->nWriteErrorValue <= 10)
            LectureAddF(szBuf, nBufLen,
                "  ✗ Частота ошибок записи (200): значение %d, худший %d\r\n",
                pInfo->nWriteErrorValue,
                pInfo->nWriteErrorWorst >= 0 ? pInfo->nWriteErrorWorst : pInfo->nWriteErrorValue);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen,
                "  %s Заявленный остаток ресурса: %d%%   (это износ NAND, не здоровье)\r\n",
                pInfo->nEndurancePercent <= 20 ? "\xE2\x9A\xA0" : "\xE2\x9C\x93",
                pInfo->nEndurancePercent);

        if (nUnknown > 0) {
            LectureAdd(szBuf, nBufLen, "\r\nНаблюдение:\r\n");
            LectureAdd(szBuf, nBufLen,
                "  ⚠ Есть vendor-specific SMART-счётчики с нестандартными RAW, которые нельзя "
                "надёжно трактовать без профиля контроллера/прошивки. Unknown ≠ Bad: B0 не штрафует "
                "здоровье, пока декодер не доказал, что число — физические ошибки.\r\n");
        }

        LectureAdd(szBuf, nBufLen, "\r\nКонтекст (не штраф):\r\n");
        if (pInfo->nCrcErrors > 0)
            LectureAddF(szBuf, nBufLen,
                "  CRC UltraDMA: %d. Кабель/мост/порт, не здоровье носителя.\r\n",
                pInfo->nCrcErrors);
        else if (pInfo->nCrcErrors == 0)
            LectureAdd(szBuf, nBufLen, "  CRC UltraDMA: 0. Контекст, не штраф.\r\n");
        LectureAddF(szBuf, nBufLen,
            "  Температура: %s. Время на высокой температуре: нет данных.\r\n",
            szTempBand);
        LectureAddF(szBuf, nBufLen,
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n", szPoh);
        LectureAddF(szBuf, nBufLen,
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            (unsigned long)pInfo->dwPowerCycleCount);
        if (nC0 >= 0 && DriveTreatsC0AsPowerLoss(pInfo)) {
            LectureAddF(szBuf, nBufLen,
                "  Аварийные отключения питания (192): %d. Журнал питания, не штраф. "
                "Находка только при росте счётчика, не по абсолютному числу.\r\n",
                nC0);
        } else if (nC0 >= 0) {
            LectureAddF(szBuf, nBufLen,
                "  Парковки при выключении (192): %d. Контекст, не штраф.\r\n",
                nC0);
        }
        if (pInfo->nGSenseEvents >= 0 || pInfo->eType == DRIVE_TYPE_HDD) {
            LectureAdd(szBuf, nBufLen, "\r\nМеханика (отдельный канал, не штраф к состоянию):\r\n");
            if (pInfo->nGSenseEvents >= 0)
                LectureAddF(szBuf, nBufLen,
                    "  G-Sense: %d событий. Зарегистрированные события, связанные с ударом/вибрацией (не обязательно столько физических ударов).\r\n",
                    pInfo->nGSenseEvents);
            if (pInfo->nGSensePerKh >= 0 && pInfo->dwPowerOnHours > 0)
                LectureAddF(szBuf, nBufLen,
                    "  G-Sense / наработка: %d событий ≈ %d на 1000 ч (контекст, не оценка).\r\n",
                    pInfo->nGSenseEvents, pInfo->nGSensePerKh);
            if (pInfo->nMechRisk >= 0)
                LectureAddF(szBuf, nBufLen,
                    "  Mechanical risk: %d/100 (рост и корреляция; абсолютный BF не входит).\r\n",
                    pInfo->nMechRisk);
            if (pInfo->nEmergencyRetract >= 0)
                LectureAddF(szBuf, nBufLen,
                    "  Аварийные парковки (192): %d.\r\n", pInfo->nEmergencyRetract);
            if (pInfo->nLoadUnload >= 0)
                LectureAddF(szBuf, nBufLen,
                    "  Циклы парковки головок: %d.\r\n", pInfo->nLoadUnload);
            LectureAddF(szBuf, nBufLen,
                "  Механическое состояние: %s.\r\n",
                pInfo->eMechanics == HEALTH_STATUS_GOOD ? "НОРМА" :
                    GetHealthStatusName(pInfo->eMechanics));
            if (pInfo->nGSenseEvents > 0 &&
                pInfo->nReallocated <= 0 && pInfo->nPendingSectors <= 0 &&
                pInfo->nUncorrectable <= 0)
                LectureAdd(szBuf, nBufLen,
                    "  G-Sense зарегистрировал события ударов/вибрации, однако SMART не показывает "
                    "связанных с ними признаков повреждения поверхности (5/196/197/198 = 0). "
                    "\r\n");
            else if (pInfo->nGSenseEvents > 0)
                LectureAdd(szBuf, nBufLen,
                    "  G-Sense совпадает с проблемными секторами — это связанная улика, не отдельный штраф.\r\n");
        }
        if (nC3 >= 0) {
            if (DriveIsHdd(pInfo) && pInfo->eVendor == VENDOR_SEAGATE) {
                const SMART_ATTRIBUTE* a195 = FindAttr(pInfo, 0xC3);
                unsigned nErr = a195 ? SeagateRateErrs(a195->bRawValue) : 0;
                DWORD nOps = a195 ? SeagateRateOps(a195->bRawValue) : 0;
                LectureAddF(szBuf, nBufLen,
                    "  ID 195 (Hardware ECC Recovered): %u ошибок коррекции / %lu секторов. "
                    "На HDD это штатная коррекция ошибок чтения пластины (как ID 1), "
                    "не NAND и не неисправимые сектора. RAW без тренда не оценивается.\r\n",
                    nErr, (unsigned long)nOps);
            } else if (DriveIsHdd(pInfo)) {
                LectureAddF(szBuf, nBufLen,
                    "  ID 195 (Hardware ECC Recovered): RAW %d. "
                    "На HDD это штатная коррекция чтения пластины, не NAND. "
                    "Без профиля вендора абсолютный RAW не оценивается.\r\n",
                    nC3);
            } else {
                LectureAddF(szBuf, nBufLen,
                    "  ID 195 RAW: %d. На SSD это vendor-specific счётчик, "
                    "не Hardware ECC Recovered HDD и не «механизм NAND» по умолчанию. "
                    "Без профиля не оценивается.\r\n",
                    nC3);
            }
        }

        LectureAdd(szBuf, nBufLen, "\r\nМатрица улик:\r\n");
        LectureMatrixRow(szBuf, nBufLen, "Улика", "Значение", "Влияние");
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         pInfo->bPredictFailure ? "FAIL" : "PASS",
                         pInfo->bPredictFailure ? "−" : "+");
        LectureMatrixRow(szBuf, nBufLen, "Prefail now",
                         pInfo->bPrefailNow ? "FAIL" : "PASS",
                         pInfo->bPrefailNow ? "−" : "+");
        if (pInfo->nReallocated >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Переназначенные", szR,
                             pInfo->nReallocated == 0 ? "+" : "−");
        if (pInfo->nPendingSectors >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Ожидающие", szPend,
                             pInfo->nPendingSectors == 0 ? "+" : "−");
        if (pInfo->nUncorrectable >= 0)
            LectureMatrixRow(szBuf, nBufLen, "Неисправимые", szU,
                             pInfo->nUncorrectable == 0 ? "+" : "−");
        if (pInfo->nRemapEvents >= 0)
            LectureMatrixRow(szBuf, nBufLen, "События переназначения", szRemap,
                             pInfo->nRemapEvents == 0 ? "+" : "−");
        if (pInfo->nCrcErrors >= 0)
            LectureMatrixRow(szBuf, nBufLen, "CRC", szCrc, "0");
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 >= 0) {
                char sz187[24];
                safe_snprintf(sz187, "%d", n187);
                LectureMatrixRow(szBuf, nBufLen, "Неисправимые (187)", sz187,
                                 n187 == 0 ? "+" : "−");
            }
        }
        if (pInfo->nWriteErrorValue >= 0 && pInfo->nWriteErrorValue <= 10) {
            char szWe[32];
            const char* inf;
            safe_snprintf(szWe, "знач. %d", pInfo->nWriteErrorValue);
            inf = (pInfo->nWriteErrorValue <= 10) ? "−" : "0";
            LectureMatrixRow(szBuf, nBufLen, "Ошибки записи (200)", szWe, inf);
        }
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, "Остаток ресурса", szLife,
                             pInfo->nEndurancePercent <= 20 ? "0" : "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "нет данных");
            LectureMatrixRow(szBuf, nBufLen, "Температура", szT, "0");
        }
        {
            char szH[64];
            FormatPowerOnHours(pInfo->dwPowerOnHours, szH, (int)sizeof(szH));
            LectureMatrixRow(szBuf, nBufLen, "Наработка", szH, "0");
        }
        if (nC3 >= 0) {
            char szEcc[24];
            safe_snprintf(szEcc, "%d", nC3);
            LectureMatrixRow(szBuf, nBufLen, "ECC (195)", szEcc,
                             DriveIsHdd(pInfo) ? "0" : "UNKNOWN");
        }
        if (nB0 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nB0);
            LectureMatrixRow(szBuf, nBufLen, "B0", szB, "UNKNOWN");
        }
        if (nB1 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nB1);
            LectureMatrixRow(szBuf, nBufLen, "B1", szB, "UNKNOWN");
        }
        if (nF5 >= 0) {
            char szB[24];
            safe_snprintf(szB, "%d", nF5);
            LectureMatrixRow(szBuf, nBufLen, "F5", szB, "UNKNOWN");
        }
        {
            char szCtl[64];
            const char* ctl = GetControllerName(pInfo->eController);
            if (pInfo->eController == CONTROLLER_PHISON && nUnknown > 0)
                safe_snprintf(szCtl, "Phison (RAW частично)");
            else
                safe_snprintf(szCtl, "%s", ctl);
            LectureMatrixRow(szBuf, nBufLen, "Контроллер", szCtl,
                             nUnknown > 0 ? "−уверенность" : "0");
        }

        LectureAdd(szBuf, nBufLen, "\r\nИтог:\r\n");
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD)
            LectureAdd(szBuf, nBufLen, "  ХОРОШО, потому что:\r\n");
        else if (pInfo->eHealthStatus == HEALTH_STATUS_OBSERVE)
            LectureAdd(szBuf, nBufLen, "  РИСК, потому что:\r\n");
        else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION)
            LectureAdd(szBuf, nBufLen, "  ВНИМАНИЕ, потому что:\r\n");
        else
            LectureAdd(szBuf, nBufLen, "  ПЛОХО, потому что:\r\n");
        LectureAddF(szBuf, nBufLen, "    %d сильных положительных признаков\r\n", nPos);
        LectureAddF(szBuf, nBufLen, "    %d критических отказов\r\n", nCritFail);
        LectureAddF(szBuf, nBufLen, "    %d признаков деградации носителя\r\n", nMediaDeg);
        LectureAddF(szBuf, nBufLen, "    %d vendor-specific счётчиков не расшифрованы\r\n",
                    nUnresolved);

        LectureAdd(szBuf, nBufLen,
            "Мы не переводим SMART Value/Worst/Threshold в процент здоровья: "
            "нормализованная шкала 100 против 200 — это не «в два раза здоровее». "
            "Сырые счётчики — факты, а не шкала от 0 до 100.\r\n");
        if (pInfo->nEndurancePercent >= 0)
            LectureAdd(szBuf, nBufLen,
                "Заявленный остаток ресурса (A9) — цифра контроллера про износ NAND, "
                "это не здоровье диска и не означает, что диск новый.\r\n");
        else
            LectureAdd(szBuf, nBufLen,
                "Для этого диска заявленный остаток ресурса (износ) не задан — "
                "процент не выдумывается.\r\n");
        LectureAddUnknownIdList(szBuf, nBufLen, pInfo);
    }
}

/* ============================================================
 * SSD-specific indicator extraction
 * Extracts vendor-specific SSD health info
 * ============================================================ */
void ExtractSSDIndicators(DRIVE_INFO* pInfo)
{
    int i;
    IdentifyDriveParts(pInfo);
    pInfo->nSSDLifeLeft     = -1;
    pInfo->nSSDTotalWritesGB = -1;
    pInfo->nSSDAvgEraseCount = -1;
    pInfo->nSSDMaxEraseCount = -1;
    pInfo->nSSDMinEraseCount = -1;
    pInfo->nSSDWearLevelingCount = -1;

    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* pA = &pInfo->attrData.stAttributes[i];
        if (pA->bAttrID == 0) continue;

        switch (pA->bAttrID) {
        /* Remaining Life / SSD Life Left */
        case 0xA9:
            if (pInfo->nSSDLifeLeft < 0) {
                int raw = (int)GetRawValue(pA->bRawValue);
                /* Phison 169 RAW is remaining % (0 = exhausted). Value is dummy. */
                if (raw <= 100)
                    pInfo->nSSDLifeLeft = raw;
                else
                    pInfo->nSSDLifeLeft = (int)pA->bAttrValue;
            }
            break;
        case 0xE7:
            if (pInfo->nSSDLifeLeft < 0) {
                int v = (int)GetRawValue16Lo(pA->bRawValue);
                if (v >= 0 && v <= 100)
                    pInfo->nSSDLifeLeft = v;
                else if (pA->bAttrValue >= 0 && pA->bAttrValue <= 100)
                    pInfo->nSSDLifeLeft = (int)pA->bAttrValue;
            }
            break;

        /* Total writes / NAND writes */
        case 0xE9:
            if (pInfo->nSSDTotalWritesGB < 0)
                pInfo->nSSDTotalWritesGB = (int)GetRawValue(pA->bRawValue);
            break;
        case 0xF9:
            if (pInfo->nSSDTotalWritesGB < 0)
                pInfo->nSSDTotalWritesGB = (int)GetRawValue(pA->bRawValue);
            break;
        /* Host writes. Phison 241/242 RAW is already host GB, not LBA. */
        case 0xF1:
        case 0xF3: {
            unsigned __int64 nRaw, nGiB;
            if (pInfo->nSSDTotalWritesGB >= 0) break;
            nRaw = GetRawValue48(pA->bRawValue);
            if (IsPhisonFamily(pInfo) && (pA->bAttrID == 0xF1 || pA->bAttrID == 0xF2)) {
                nGiB = ScalePhisonHostGiB(pInfo, nRaw);
            } else {
                if (nRaw < 2048ULL) break;
                nGiB = nRaw / (1024ULL * 1024ULL * 2ULL);
            }
            if (nGiB > 4000000ULL) nGiB = 4000000ULL;
            pInfo->nSSDTotalWritesGB = (int)nGiB;
            break;
        }

        /* Average erase count */
        case 0xA7:
            if (pInfo->nSSDAvgEraseCount < 0)
                pInfo->nSSDAvgEraseCount = (int)GetRawValue(pA->bRawValue);
            break;
        case 0xAD: {
            /* 0xAD: Samsung = wear leveling only; Intel = avg erase only.
             * Other vendors may fill both when neither is set yet. */
            int nAd = (int)GetRawValue(pA->bRawValue);
            if (pInfo->eController == CONTROLLER_SAMSUNG) {
                if (pInfo->nSSDWearLevelingCount < 0)
                    pInfo->nSSDWearLevelingCount = nAd;
            } else if (pInfo->eController == CONTROLLER_INTEL) {
                if (pInfo->nSSDAvgEraseCount < 0)
                    pInfo->nSSDAvgEraseCount = nAd;
            } else {
                if (pInfo->nSSDWearLevelingCount < 0)
                    pInfo->nSSDWearLevelingCount = nAd;
                if (pInfo->nSSDAvgEraseCount < 0)
                    pInfo->nSSDAvgEraseCount = nAd;
            }
            break;
        }
        case 0xEA:
            if (pInfo->nSSDAvgEraseCount < 0)
                pInfo->nSSDAvgEraseCount = (int)GetRawValue(pA->bRawValue);
            break;

        /* Max erase count */
        case 0xA5:
            if (pInfo->nSSDMaxEraseCount < 0)
                pInfo->nSSDMaxEraseCount = (int)GetRawValue(pA->bRawValue);
            break;

        /* Min erase count */
        case 0xA6:
            if (pInfo->nSSDMinEraseCount < 0)
                pInfo->nSSDMinEraseCount = (int)GetRawValue(pA->bRawValue);
            break;
        }
    }
}

