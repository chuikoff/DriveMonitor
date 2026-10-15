/* DriveMonitor - health assessment and SSD indicators. GPL-2.0-or-later. See LICENSE. */
#include "smart_internal.h"

/* ============================================================
 * Health: state + confidence + evidence
 *
 * eHealthStatus starts from the media channel and keeps that rank.
 * A confirmed side axis at CAUTION or worse raises a clean medium
 * at most to CAUTION. That is the 1.7.23 agreement: CRC, command
 * timeouts, spin retries, temperature, HDD mechanics, and SSD wear.
 * OBSERVE on a side axis stays on its row. A drivedb name hint is
 * only Risk on the row. A media name does not enter the media channel,
 * so it does not set the headline. A CRC name marks the interface axis
 * and does not raise the headline.
 * nHealthPercent is SECONDARY estimated remaining life only (endurance),
 * never a fake % from worst/thresh or raw*N penalties.
 * nConfidence is completeness of evidence, not "how healthy".
 * An HDD has no wear scale, so nHealthPercent stays -1.
 * That absence does not zero nConfidence.
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

static BOOL WriteErrorScored(const DRIVE_INFO* p)
{
    if (!p || p->nWriteErrorValue < 0 || p->nWriteErrorValue > 255)
        return FALSE;
    return NormalizedWriteErrorScored((BYTE)p->nWriteErrorValue,
                                      FindThreshold(p, 0xC8));
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
            /* 196: full 48-bit RAW is not an event count when it is a copy of
             * head-flying hours, and must not clamp to INT_MAX. */
            if (id == 0xC4)
                return DecodeRemapEvents(pInfo, raw);
            /* 188: three WORDs — total, completions >5 s, completions >7.5 s.
             * Score the total. The 48-bit field is not one timeout count. */
            if (id == 0xBC)
                v = (unsigned __int64)((WORD)raw[0] | ((WORD)raw[1] << 8));
            else {
                GetAttrDecode(id, pInfo, &dec);
                if (dec.eEnc == RAW_ENC_SECTORS_LO16)
                    v = (unsigned __int64)((WORD)raw[0] | ((WORD)raw[1] << 8));
                else
                    v = GetRawValue48(raw);
            }
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
        safe_snprintf_n(szBuf, nBufLen, "%s", Tr(STR_POH_NONE));
        return;
    }
    y = dwHours / 8760u;
    d = (dwHours % 8760u) / 24u;
    h = (dwHours % 8760u) % 24u;
    if (y > 0) {
        safe_snprintf(a, "%u %s", y,
            UiLangIsEn() ? ((y == 1) ? Tr(STR_YEAR) : Tr(STR_YEARS))
                         : RuCountWord(y, Tr(STR_YEAR), Tr(STR_YEARS), Tr(STR_YEARS_MANY)));
        p[n++] = a;
    }
    if (d > 0) {
        safe_snprintf(b, "%u %s", d,
            UiLangIsEn() ? ((d == 1) ? Tr(STR_DAY) : Tr(STR_DAYS))
                         : RuCountWord(d, Tr(STR_DAY), Tr(STR_DAYS), Tr(STR_DAYS_MANY)));
        p[n++] = b;
    }
    if (h > 0 || n == 0) {
        safe_snprintf(c, "%u %s", h,
            UiLangIsEn() ? ((h == 1) ? Tr(STR_HOUR) : Tr(STR_HOURS))
                         : RuCountWord(h, Tr(STR_HOUR), Tr(STR_HOURS), Tr(STR_HOURS_MANY)));
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
    case TEMP_BAND_NORMAL:   return Tr(bLowercase ? STR_TEMP_OK_LO : STR_TEMP_OK);
    case TEMP_BAND_ELEVATED: return Tr(bLowercase ? STR_TEMP_ELEV_LO : STR_TEMP_ELEV);
    case TEMP_BAND_HIGH:     return Tr(bLowercase ? STR_TEMP_HIGH_LO : STR_TEMP_HIGH);
    case TEMP_BAND_CRITICAL: return Tr(bLowercase ? STR_TEMP_CRIT_LO : STR_TEMP_CRIT);
    default:                 return Tr(STR_POH_NONE);
    }
}

static void FormatTempLecture(const DRIVE_INFO* p, char* buf, int nBuf)
{
    int warnC, critC;
    if (!buf || nBuf <= 0) return;
    if (!p || p->nTemperatureC <= 0) {
        safe_snprintf_n(buf, nBuf, "%s", Tr(STR_POH_NONE));
        return;
    }
    warnC = p->nTempWarnC > 0 ? p->nTempWarnC :
            (p->bIsNVMe ? NvmeIdentifyTempC(p->wNVMeWarnTempThreshold) : -1);
    critC = p->nTempCritC > 0 ? p->nTempCritC :
            (p->bIsNVMe ? NvmeIdentifyTempC(p->wNVMeCritTempThreshold) : -1);
    if (warnC > 0 && critC > 0 && p->nTempMaxC > 0)
        safe_snprintf_n(buf, nBuf,
            TN("%d °C · %s (макс. %d °C, пред. %d, крит. %d)",
               "%d °C · %s (max %d °C, warn %d, crit %d)"),
            p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
            p->nTempMaxC, warnC, critC);
    else if (warnC > 0 && critC > 0)
        safe_snprintf_n(buf, nBuf, TN("%d °C · %s (пред. %d °C, крит. %d °C)",
                                      "%d °C · %s (warn %d °C, crit %d °C)"),
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        warnC, critC);
    else if (p->nTempMaxC > 0 && critC > 0)
        safe_snprintf_n(buf, nBuf, TN("%d °C · %s (макс. %d, крит. %d)",
                                      "%d °C · %s (max %d, crit %d)"),
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        p->nTempMaxC, critC);
    else if (p->nTempMaxC > 0)
        safe_snprintf_n(buf, nBuf, TN("%d °C · %s (макс. зафиксированная %d °C)",
                                      "%d °C · %s (recorded max %d °C)"),
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        p->nTempMaxC);
    else if (warnC > 0)
        safe_snprintf_n(buf, nBuf, TN("%d °C · %s (пред. %d °C)",
                                      "%d °C · %s (warn %d °C)"),
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        warnC);
    else if (critC > 0)
        safe_snprintf_n(buf, nBuf, TN("%d °C · %s (крит. %d °C)",
                                      "%d °C · %s (crit %d °C)"),
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE),
                        critC);
    else
        safe_snprintf_n(buf, nBuf, "%d °C · %s",
                        p->nTemperatureC, GetTempBandName(p->eTempBand, TRUE));
}

static const char* QualityNameRu(NORM_QUALITY q, BOOL bRaw)
{
    switch (q) {
    case NORM_QUALITY_LOW:
        return bRaw ? TN("НИЗКОЕ", "LOW") : TN("МАЛОИНФОРМАТИВНО", "UNINFORMATIVE");
    case NORM_QUALITY_MEDIUM: return bRaw ? TN("СМЕШАННОЕ", "MIXED") : TN("СРЕДНЕЕ", "MEDIUM");
    case NORM_QUALITY_HIGH:   return TN("ВЫСОКОЕ", "HIGH");
    default:                  return Tr(STR_POH_NONE);
    }
}

static void PadUtf8(char* dst, int nDst, const char* s, int width)
{
    int w = 0;
    size_t used = 0;
    const unsigned char* p;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    p = (const unsigned char*)(s ? s : "");
    /* Stop at `width` columns. A longer value used to run into the next column. */
    while (*p && w < width && used + 1 < (size_t)nDst) {
        int nb = 1;
        if ((*p & 0xE0) == 0xC0) nb = 2;
        else if ((*p & 0xF0) == 0xE0) nb = 3;
        else if ((*p & 0xF8) == 0xF0) nb = 4;
        else if (*p >= 0x80) nb = 1;
        if (used + (size_t)nb >= (size_t)nDst) break;
        memcpy(dst + used, p, (size_t)nb);
        used += (size_t)nb;
        p += nb;
        dst[used] = '\0';
        w++;
    }
    while (w < width && used + 1 < (size_t)nDst) {
        dst[used++] = ' ';
        dst[used] = '\0';
        w++;
    }
}

static void FmtIntOrNA(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "%s", Tr(STR_POH_NONE));
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
        safe_snprintf_n(buf, nBuf, "%s", TN("нет", "none"));
        return;
    }
    if (cw & NVME_CRIT_WARN_SPARE_BELOW_THRESH)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", запас", ", spare") : TN("запас", "spare"));
    if (cw & NVME_CRIT_WARN_TEMP_THRESHOLD)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", температура", ", temperature") : TN("температура", "temperature"));
    if (cw & NVME_CRIT_WARN_RELIABILITY_DEGRADED)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", надёжность", ", reliability") : TN("надёжность", "reliability"));
    if (cw & NVME_CRIT_WARN_READ_ONLY)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", только чтение", ", read-only") : TN("только чтение", "read-only"));
    if (cw & NVME_CRIT_WARN_VOLATILE_MEM_BACKUP)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", резерв энергозависимой памяти", ", volatile memory backup") : TN("резерв энергозависимой памяти", "volatile memory backup"));
    if (cw & NVME_CRIT_WARN_PMR_RO)
        LectureAdd(tmp, (int)sizeof(tmp), tmp[0] ? TN(", постоянная память только чтение", ", persistent memory read-only") : TN("постоянная память только чтение", "persistent memory read-only"));
    if (tmp[0] == '\0')
        safe_snprintf_n(buf, nBuf, "%s", TN("есть", "set"));
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
    if (UseSeagateRateRaw(p))
        return SeagateRateErrs(a->bRawValue);
    return 0;
}

typedef struct {
    BOOL bId7;
    BOOL bId193Val;
    BOOL bId193Raw;
} HDD_WEAR;

static void HddWearFlags(const DRIVE_INFO* p, HDD_WEAR* w)
{
    int v7, w7, v193;
    if (!w) return;
    ZeroMemory(w, sizeof(*w));
    if (!p) return;
    v7   = AttrValueOrNeg1(p, 0x07);
    w7   = AttrWorstOrNeg1(p, 0x07);
    v193 = AttrValueOrNeg1(p, 0xC1);
    w->bId7      = HddRateNearThresh(v7, w7, FindThreshold(p, 0x07)) ||
                   HddRateErrs(p, 0x07) > 0;
    w->bId193Val = (v193 >= 0 && v193 <= 15);
    w->bId193Raw = (p->nLoadUnload >= 300000);
}

void FormatHddObservePrompt(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (!pInfo) return;

    if (pInfo->bPrefailPast) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Диск ещё считает себя исправным. Prefail-атрибут был ниже порога.",
            "The drive still reports itself healthy. A prefail attribute was below its threshold."));
        return;
    }
    if (pInfo->bUsageFailed) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Диск ещё считает себя исправным. Usage-атрибут на пороге — следить.",
            "The drive still reports itself healthy. A usage attribute is at its threshold."));
        return;
    }
    safe_snprintf_n(szBuf, nBufLen, "%s", TN(
        "Есть факторы риска. Нажмите на состояние.",
        "There are risk factors. Click the status for details."));
}

void FormatHddCautionPrompt(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    int cau;
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (!pInfo) return;
    cau = HealthRank(HEALTH_STATUS_CAUTION);
    /* Media already explains a CAUTION/BAD headline. */
    if (HealthRank(pInfo->eReliability) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Есть признаки деградации носителя. Нажмите на состояние.",
            "There are signs of media degradation. Click the status for details."));
        return;
    }
    if (HealthRank(pInfo->eInterface) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Проблема интерфейса: CRC, таймауты или повторы раскрутки. Носитель в норме — кабель, контакт или питание.",
            "Interface problem: CRC, timeouts, or spin retries. The media is fine — cable, contact, or power."));
        return;
    }
    if (HealthRank(pInfo->eTempStatus) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Диск перегрет. Носитель в норме — охладить, это не приговор пластинам.",
            "The drive is too hot. The media is fine — cool it; this is not a platter failure."));
        return;
    }
    if (HealthRank(pInfo->eMechanics) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Механика требует внимания. Плохих секторов нет — это не смерть диска.",
            "Mechanics need attention. There are no bad sectors — the drive is not dying."));
        return;
    }
    safe_snprintf_n(szBuf, nBufLen, "%s", TN(
        "Есть признаки, требующие наблюдения. Нажмите на состояние.",
        "There are signs that need watching. Click the status for details."));
}

void FormatSsdCautionPrompt(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    int cau;
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    if (!pInfo) return;
    cau = HealthRank(HEALTH_STATUS_CAUTION);
    if (HealthRank(pInfo->eReliability) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Есть признаки деградации носителя. Нажмите на состояние.",
            "There are signs of media degradation. Click the status for details."));
        return;
    }
    if (!pInfo->bIsNVMe && HealthRank(pInfo->eInterface) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Проблема интерфейса: CRC или таймауты. NAND в норме — кабель, контакт или питание.",
            "Interface problem: CRC or timeouts. The NAND is fine — cable, contact, or power."));
        return;
    }
    if (HealthRank(pInfo->eTempStatus) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Диск перегрет. NAND в норме — охладить, это не износ флеша.",
            "The drive is too hot. The NAND is fine — cool it; this is not flash wear."));
        return;
    }
    if (HealthRank(pInfo->eWear) >= cau) {
        safe_snprintf_n(szBuf, nBufLen, "%s", TN(
            "Запас ресурса NAND на исходе. Ошибок носителя при этом нет.",
            "NAND endurance is nearly used up. There are no media errors."));
        return;
    }
    safe_snprintf_n(szBuf, nBufLen, "%s", TN(
        "Есть признаки, требующие наблюдения. Нажмите на состояние.",
        "There are signs that need watching. Click the status for details."));
}

/* Confirmed side axis into the headline. CAUTION or worse becomes
 * CAUTION. OBSERVE stays off the headline. Interface BAD (CRC >= 100)
 * is included: the row can say BAD, the headline stops at CAUTION.
 * The same ceiling has been in force since 1.7.23. A name-only CRC
 * hint never reaches CAUTION, so it does not take this path. */
static DRIVE_HEALTH_STATUS SideRaisesHeadline(DRIVE_HEALTH_STATUS e)
{
    if (HealthRank(e) >= HealthRank(HEALTH_STATUS_CAUTION))
        return HEALTH_STATUS_CAUTION;
    return HEALTH_STATUS_GOOD;
}

/* HDD: media sets the headline as-is. Mechanics from CAUTION up, and
 * interface/temperature, can raise it only to CAUTION. */
static void AssessHddHealth(DRIVE_INFO* pInfo)
{
    BOOL bMediaHurt, bMediaClean;
    BOOL bGsenseUp = FALSE, bC5Up = FALSE, bMediaUp = FALSE;
    MECH_SNAP prev, now;
    int havePrev = 0;

    bMediaHurt = (pInfo->nReallocated > 0 ||
                  pInfo->nPendingSectors > 0 ||
                  pInfo->nUncorrectable > 0 ||
                  pInfo->nRemapEvents > 0 ||
                  pInfo->nReportedUncorrect > 0);
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

    /* Media and interface ranks come from profile rules. Prefail and a
     * failed self-test still sit on top of that rank. */
    if (SelfTestFailed(pInfo))
        pInfo->eReliability = HEALTH_STATUS_CRITICAL;
    else {
        pInfo->eReliability = pInfo->eMediaEvidence;
        if (pInfo->bPrefailNow)
            pInfo->eReliability = WorstHealth(pInfo->eReliability,
                                              HEALTH_STATUS_BAD);
        if (pInfo->bPrefailPast || pInfo->bUsageFailed)
            pInfo->eReliability = WorstHealth(pInfo->eReliability,
                                              HEALTH_STATUS_OBSERVE);
    }
    pInfo->eInterface = pInfo->eInterfaceEvidence;

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
    /* A positive shock count stays OBSERVE even when 187 already marks the
     * surface hurt and 5/197/198 are still 0. OBSERVE does not raise the headline. */
    else if (pInfo->nGSenseEvents > 0)
        pInfo->eMechanics = HEALTH_STATUS_OBSERVE;
    else if (pInfo->nGSenseEvents >= 0 || pInfo->nEmergencyRetract >= 0 ||
             pInfo->nLoadUnload >= 0)
        pInfo->eMechanics = HEALTH_STATUS_GOOD;
    else
        pInfo->eMechanics = HEALTH_STATUS_UNKNOWN;

    /* Media as-is. Confirmed interface (CRC from 1, timeouts, spin
     * retries), mechanics, and temperature raise a clean surface
     * at most to CAUTION. Same rule as 1.7.23. */
    pInfo->eHealthStatus = pInfo->eReliability;
    pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                       SideRaisesHeadline(pInfo->eMechanics));
    pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                       SideRaisesHeadline(pInfo->eInterface));
    pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                       SideRaisesHeadline(pInfo->eTempStatus));
    }

    if (SelfTestFailed(pInfo))
        pInfo->eHealthStatus = HEALTH_STATUS_CRITICAL;

    /* No endurance scale. Confidence is the blend in AssessDriveHealth. */
    pInfo->nHealthPercent = -1;
}


static DRIVE_HEALTH_STATUS WearStatusFromRemaining(int nLeft)
{
    if (nLeft < 0)  return HEALTH_STATUS_UNKNOWN;
    if (nLeft <= 5) return HEALTH_STATUS_BAD;
    if (nLeft <= 10) return HEALTH_STATUS_CAUTION;
    if (nLeft <= 20) return HEALTH_STATUS_OBSERVE;
    return HEALTH_STATUS_GOOD;
}

int WearBadgeStatus(int nLeft)
{
    int e = (int)WearStatusFromRemaining(nLeft);
    if (e == HEALTH_STATUS_BAD || e == HEALTH_STATUS_WARNING ||
        e == HEALTH_STATUS_CRITICAL)
        return HEALTH_STATUS_CAUTION;
    return e;
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
    else {
        pInfo->eReliability = pInfo->eMediaEvidence;
        if (pInfo->bPrefailNow || pInfo->qwNVMeMediaErrors > 0 || bSpareLow)
            pInfo->eReliability = WorstHealth(pInfo->eReliability,
                                              HEALTH_STATUS_BAD);
        if (pInfo->bPrefailPast || pInfo->bUsageFailed)
            pInfo->eReliability = WorstHealth(pInfo->eReliability,
                                              HEALTH_STATUS_OBSERVE);
    }

    pInfo->eWear = WearStatusFromRemaining(pInfo->nEndurancePercent);
    if (pInfo->bIsNVMe && (int)pInfo->nvmeHealth.PercentageUsed > 95)
        pInfo->eWear = WorstHealth(pInfo->eWear, HEALTH_STATUS_OBSERVE);
    if (pInfo->bIsNVMe && (int)pInfo->nvmeHealth.PercentageUsed >= 100)
        pInfo->eWear = HEALTH_STATUS_BAD;

    pInfo->eInterface = pInfo->bIsNVMe ? HEALTH_STATUS_GOOD
                                       : pInfo->eInterfaceEvidence;

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

    /* Media as-is. Wear, temperature, and (SATA only) confirmed interface
     * raise a clean NAND at most to CAUTION. OBSERVE wear (11–20% left)
     * stays on its axis. A CRC name hint is Risk and does not raise. */
    pInfo->eHealthStatus = pInfo->eReliability;
    pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                       SideRaisesHeadline(pInfo->eWear));
    pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                       SideRaisesHeadline(pInfo->eTempStatus));
    if (!pInfo->bIsNVMe)
        pInfo->eHealthStatus = WorstHealth(pInfo->eHealthStatus,
                                           SideRaisesHeadline(pInfo->eInterface));
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
        nMax = AtaTempLifetimeMaxC(a);
        if (nMax < 1)
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

static int EvidenceCount(const DRIVE_INFO* p, const SMART_ATTRIBUTE* a,
                         int enc, int* count)
{
    unsigned __int64 v;
    const BYTE* raw;
    int n;
    if (!p || !a || !count) return 0;
    raw = a->bRawValue;
    *count = 0;
    if (enc == PROF_ENC_LO16) {
        *count = (int)((unsigned)raw[0] | ((unsigned)raw[1] << 8));
        return 1;
    }
    if (enc == PROF_ENC_REPORTED) {
        n = DecodeReportedUncorrect(raw, p->eVendor);
        if (n < 0) return 0;
        *count = n;
        return 1;
    }
    if (enc == PROF_ENC_REMAP) {
        n = DecodeRemapEvents(p, raw);
        if (n < 0) return 0;
        *count = n;
        return 1;
    }
    if (enc == PROF_ENC_COUNTER) {
        v = GetRawValue48(raw);
        *count = (v > (unsigned __int64)INT_MAX) ? INT_MAX : (int)v;
        return 1;
    }
    if (ProfileDecodeRaw(enc, GetRawValue48(raw), &v) != 1) return 0;
    *count = (v > (unsigned __int64)INT_MAX) ? INT_MAX : (int)v;
    return 1;
}

static int* EvidenceSlot(DRIVE_INFO* p, int slot)
{
    switch (slot) {
    case PROF_SLOT_REALLOC:  return &p->nReallocated;
    case PROF_SLOT_PENDING:  return &p->nPendingSectors;
    case PROF_SLOT_UNCORR:   return &p->nUncorrectable;
    case PROF_SLOT_REMAP:    return &p->nRemapEvents;
    case PROF_SLOT_CRC:      return &p->nCrcErrors;
    case PROF_SLOT_REPORTED: return &p->nReportedUncorrect;
    default:                 return NULL;
    }
}

/* WORSE media/interface rule whose scope matches this drive. */
static int ChannelScopeOk(const DRIVE_INFO* p, const PROF_HEALTH* h)
{
    if (!p || !h || h->rule != PROF_RULE_WORSE) return 0;
    if (h->sem != PROF_SEM_MEDIA && h->sem != PROF_SEM_INTERFACE) return 0;
    if (h->scope == PROF_SCOPE_HDD && !DriveIsHdd(p)) return 0;
    if (h->scope == PROF_SCOPE_SSD && DriveIsHdd(p)) return 0;
    return 1;
}

static int OtherCrcDecoded(const DRIVE_INFO* p, BYTE selfId)
{
    int i;
    if (!p) return 0;
    for (i = 0; i < 30; i++) {
        const SMART_ATTRIBUTE* b = &p->attrData.stAttributes[i];
        PROF_HEALTH h;
        int count = 0;
        if (b->bAttrID == 0 || b->bAttrID == selfId) continue;
        if (!ProfileHealthRule(p, b->bAttrID, &h)) continue;
        if (h.fallback || h.slot != PROF_SLOT_CRC) continue;
        if (!ChannelScopeOk(p, &h)) continue;
        if (!EvidenceCount(p, b, h.enc, &count)) continue;
        return 1;
    }
    return 0;
}

int AttrEvidenceRank(const DRIVE_INFO* p, const SMART_ATTRIBUTE* a, int* rank)
{
    PROF_HEALTH h;
    int count = 0;
    if (rank) *rank = (int)HEALTH_STATUS_UNKNOWN;
    if (!p || !a || a->bAttrID == 0) return ATTR_EV_NONE;
    if (!ProfileHealthRule(p, a->bAttrID, &h)) {
        if (ProfileListsAttr(p, a->bAttrID)) return ATTR_EV_SKIP;
        return ATTR_EV_NONE;
    }
    if (h.sem != PROF_SEM_MEDIA && h.sem != PROF_SEM_INTERFACE)
        return ATTR_EV_NONE;
    if (h.rule != PROF_RULE_WORSE) return ATTR_EV_SKIP;
    if (!ChannelScopeOk(p, &h)) return ATTR_EV_SKIP;
    if (h.fallback && h.slot == PROF_SLOT_CRC &&
        OtherCrcDecoded(p, a->bAttrID))
        return ATTR_EV_SKIP;
    if (!EvidenceCount(p, a, h.enc, &count)) return ATTR_EV_SKIP;
    if (rank)
        *rank = ProfileCountRank(count, h.observe, h.caution, h.bad,
                                 h.badHdd, DriveIsHdd(p) ? 1 : 0);
    return ATTR_EV_RANK;
}

static void ApplyOneEvidence(DRIVE_INFO* p, const SMART_ATTRIBUTE* a,
                             const PROF_HEALTH* h, int* crcSeen)
{
    int count = 0;
    int rank;
    int* dest;
    if (!ChannelScopeOk(p, h)) return;
    if (h->fallback && h->slot == PROF_SLOT_CRC && crcSeen && *crcSeen) return;
    if (!EvidenceCount(p, a, h->enc, &count)) return;
    if (h->slot == PROF_SLOT_CRC && crcSeen) *crcSeen = 1;
    /* A drivedb name hint is observe-only. It must not replace the
     * confirmed CRC total that the lecture and the matrix read. */
    if (!(h->fallback && h->caution <= 0 && h->bad <= 0 && h->badHdd <= 0)) {
        dest = EvidenceSlot(p, h->slot);
        if (dest && *dest < 0) *dest = count;
    }
    rank = ProfileCountRank(count, h->observe, h->caution, h->bad,
                            h->badHdd, DriveIsHdd(p) ? 1 : 0);
    /* The headline copies the media channel. A name hint stays on the
     * row (AttrEvidenceRank) and must not become that channel. An
     * interface name still marks its axis; OBSERVE there does not raise. */
    if (h->fallback && h->sem == PROF_SEM_MEDIA)
        return;
    if (h->sem == PROF_SEM_MEDIA)
        p->eMediaEvidence = WorstHealth(p->eMediaEvidence,
                                        (DRIVE_HEALTH_STATUS)rank);
    else
        p->eInterfaceEvidence = WorstHealth(p->eInterfaceEvidence,
                                            (DRIVE_HEALTH_STATUS)rank);
}

/* Walk every attribute once. The channel rank is the worst profile rule.
 * A drivedb CRC name is only a hint: it yields when attribute 199 already
 * filled the slot, and it does not publish its own total. */
static void CollectChannelEvidence(DRIVE_INFO* p)
{
    const SMART_ATTRIBUTE* laterA[30];
    PROF_HEALTH laterH[30];
    int nLater = 0;
    int crcSeen = 0;
    int i;
    if (!p) return;
    p->eMediaEvidence = HEALTH_STATUS_GOOD;
    p->eInterfaceEvidence = HEALTH_STATUS_GOOD;
    for (i = 0; i < 30; i++) {
        const SMART_ATTRIBUTE* a = &p->attrData.stAttributes[i];
        PROF_HEALTH h;
        if (a->bAttrID == 0) continue;
        if (!ProfileHealthRule(p, a->bAttrID, &h)) continue;
        if (h.fallback) {
            if (nLater < 30) {
                laterA[nLater] = a;
                laterH[nLater] = h;
                nLater++;
            }
            continue;
        }
        ApplyOneEvidence(p, a, &h, &crcSeen);
    }
    for (i = 0; i < nLater; i++)
        ApplyOneEvidence(p, laterA[i], &laterH[i], &crcSeen);
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
    pInfo->eMediaEvidence      = HEALTH_STATUS_GOOD;
    pInfo->eInterfaceEvidence  = HEALTH_STATUS_GOOD;
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
    pInfo->nReportedUncorrect  = -1;
    pInfo->bThresholdViolation = FALSE;
    pInfo->bPrefailNow         = FALSE;
    pInfo->bPrefailPast        = FALSE;
    pInfo->bUsageFailed        = FALSE;
    pInfo->szEvidence[0]       = '\0';
    pInfo->nHealthPercent      = -1;
    pInfo->eHealthStatus       = HEALTH_STATUS_UNKNOWN;
    pInfo->eDiskStatus         = HEALTH_STATUS_UNKNOWN;

    if (SmartSnapshotRejected(pInfo)) {
        safe_snprintf(pInfo->szEvidence, "%s", Tr(STR_SMART_DATA_ERR));
        return;
    }
    if (!pInfo->bSMART_Supported) {
        safe_snprintf(pInfo->szEvidence, "%s", TN("SMART недоступен", "SMART unavailable"));
        return;
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
    CollectChannelEvidence(pInfo);

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

    /* Profile score, not a firmware prefix. Exact and family read as
     * identified. Generic ATA/NVMe stays partly identified.
     * An equal-priority tie is not a confident identification. */
    if (pInfo->bProfileAmbiguous)
        nConfM = 70;
    else if (pInfo->nProfileScore >= PROF_SCORE_EXACT_MODEL)
        nConfM = 99;
    else if (pInfo->nProfileScore >= PROF_SCORE_FAMILY)
        nConfM = 95;
    else if (pInfo->nProfileScore >= PROF_SCORE_CONTROLLER)
        nConfM = 90;
    else if (pInfo->nProfileScore >= PROF_SCORE_VENDOR)
        nConfM = 80;
    else
        nConfM = 70;
    if (pInfo->szModelFamily[0] && nConfM < 90)
        nConfM = 90;

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
    /* No wear scale. The evidence blend above stays. */
    if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe)
        pInfo->nHealthPercent = -1;

    /* One-line evidence in the UI language at assessment time. */
    if (pInfo->bIsNVMe) {
        char szCW[128];
        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));
        safe_snprintf(pInfo->szEvidence,
            TN("Критическое предупреждение: %s. Ошибки носителя: %llu.",
               "Critical warning: %s. Media errors: %llu."),
            szCW,
            (unsigned long long)pInfo->qwNVMeMediaErrors);
    } else {
        char szR[24], szPend[24], szU[24];
        FmtIntOrNA(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        FmtIntOrNA(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        FmtIntOrNA(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        safe_snprintf(pInfo->szEvidence,
            TN("Переназначенные сектора: %s  Ожидающие сектора: %s  Неисправимые сектора: %s  Нарушение порога: %s  Прогноз отказа: %s",
               "Reallocated sectors: %s  Pending sectors: %s  Uncorrectable sectors: %s  Threshold exceeded: %s  Failure prediction: %s"),
            szR, szPend, szU,
            pInfo->bThresholdViolation ? TN("да", "yes") : TN("нет", "no"),
            pInfo->bPredictFailure     ? TN("да", "yes") : TN("нет", "no"));
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
    LectureAddF(buf, nBuf, TN("Достоверность оценки: %d%%\r\n\r\n",
                             "Assessment confidence: %d%%\r\n\r\n"), p->nConfidence);
    LectureAdd(buf, nBuf, TN("Причины:\r\n", "Reasons:\r\n"));
    if (p->nConfModelId >= 90)
        LectureAdd(buf, nBuf, TN("  • модель определена уверенно\r\n", "  • model identified confidently\r\n"));
    else
        LectureAdd(buf, nBuf, TN("  • модель определена не полностью\r\n", "  • model only partly identified\r\n"));
    if (p->bSMART_Supported)
        LectureAdd(buf, nBuf, TN("  • SMART полностью прочитан\r\n", "  • SMART fully read\r\n"));
    else
        LectureAdd(buf, nBuf, TN("  • SMART прочитан не полностью\r\n", "  • SMART only partly read\r\n"));
    if (p->nConfVendorDecoder >= 90)
        LectureAdd(buf, nBuf, TN("  • vendor decoder найден\r\n", "  • vendor decoder found\r\n"));
    else
        LectureAdd(buf, nBuf, TN("  • vendor decoder неполный\r\n", "  • vendor decoder incomplete\r\n"));
    if (p->nGSenseEvents > 0 && p->nGSenseDelta < 0)
        LectureAdd(buf, nBuf, TN("  • динамика G-Sense отсутствует\r\n", "  • no G-Sense trend yet\r\n"));
    LectureAdd(buf, nBuf, "\r\n");
}

static void LectureMatrixRow(char* buf, int nBuf,
                             const char* evid, const char* val, const char* infl)
{
    char c1[72], c2[80], c3[40];
    PadUtf8(c1, (int)sizeof(c1), evid, 28);
    PadUtf8(c2, (int)sizeof(c2), val, 52);
    PadUtf8(c3, (int)sizeof(c3), infl, 16);
    LectureAddF(buf, nBuf, "  %s%s%s\r\n", c1, c2, c3);
}

static void LectureProfileRow(char* buf, int nBuf, const DRIVE_INFO* p)
{
    char szV[96];
    if (!p) return;
    if (p->bProfileAmbiguous && p->szProfileConflict[0])
        safe_snprintf(szV, "%s / %s", p->szProfileId, p->szProfileConflict);
    else if (p->szProfileId[0])
        safe_snprintf(szV, "%s (%d)", p->szProfileId, p->nProfileScore);
    else
        safe_snprintf(szV, "%s", "—");
    LectureMatrixRow(buf, nBuf, TN("Профиль", "Profile"), szV,
                     p->bProfileAmbiguous ? TN("−уверенность", "−confidence") : "0");
}

static void LectureFmtInt(char* buf, int nBuf, int v)
{
    if (v < 0)
        safe_snprintf_n(buf, nBuf, "%s", Tr(STR_POH_NONE));
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
        LectureAddF(buf, nBuf, TN(
            "Атрибуты %s не оцениваются: нет доверенного профиля RAW-кодировки "
            "для этого контроллера/прошивки. Большое RAW-значение не доказывает сбои. "
            "Unknown ≠ Bad.\r\n",
            "Attributes %s are not scored: there is no trusted RAW encoding profile "
            "for this controller or firmware. A large RAW value does not prove failures. "
            "Unknown is not Bad.\r\n"),
            szIds);
    }
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
        safe_snprintf_n(sz, nSz, "%s", Tr(STR_POH_NONE));
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

/* Upstream stores the heise warning in English. Translate it here so a
 * language change does not depend on a rescan. Other notes stay as stored. */
static const char* FamilyWarnText(const DRIVE_INFO* p)
{
    if (!p || !p->szDriveDbWarn[0]) return "";
    if (strstr(p->szDriveDbWarn, "heise.de/-10800960"))
        return TN(
            "у старых WD SMR бывали серьёзные проблемы с записью. "
            "https://www.heise.de/-10800960.html",
            "Older WD SMR drives had serious write problems. "
            "https://www.heise.de/-10800960.html");
    return p->szDriveDbWarn;
}

static void LectureAddDualStatus(char* szBuf, int nBufLen, const DRIVE_INFO* pInfo)
{
    LectureAddF(szBuf, nBufLen, TN("Диск: %s%s\r\nОценка: %s%s\r\n\r\n",
                                  "Drive: %s%s\r\nAssessment: %s%s\r\n\r\n"),
                HealthStatusMark(pInfo->eDiskStatus),
                GetDiskStatusName(pInfo),
                HealthStatusMark(pInfo->eHealthStatus),
                GetHealthStatusName(pInfo->eHealthStatus));
    if (pInfo->bIsUSB && pInfo->bSMART_Supported &&
        !pInfo->bGotReturnStatus && !pInfo->bIsNVMe)
        LectureAdd(szBuf, nBufLen, TN(
            "USB-мост не отдаёт SMART RETURN STATUS — это не отказ диска. "
            "Оценка ниже по таблице SMART.\r\n\r\n",
            "The USB bridge does not return SMART RETURN STATUS — that is not a drive failure. "
            "The assessment below comes from the SMART table.\r\n\r\n"));
    if (pInfo->bIsUSB) {
        char cap[160];
        FormatUsbCapability(pInfo, cap, (int)sizeof(cap));
        if (cap[0])
            LectureAddF(szBuf, nBufLen, TN("Переходник: %s\r\n\r\n",
                                           "Adapter: %s\r\n\r\n"), cap);
    }
    if (pInfo->szModelFamily[0])
        LectureAddF(szBuf, nBufLen, TN("Семейство: %s\r\n\r\n",
                                       "Family: %s\r\n\r\n"),
                    pInfo->szModelFamily);
    else if (pInfo->szDriveDbTie[0] || pInfo->bDriveDbTieUnnamed)
        LectureAdd(szBuf, nBufLen, TN("Семейство: нет имени\r\n\r\n",
                                      "Family: no name\r\n\r\n"));
    if (pInfo->szDriveDbTie[0])
        LectureAddF(szBuf, nBufLen, TN(
            "Та же оценка базы у семейства «%s». Подпись взята из более ранней строки. "
            "Атрибут, в котором эти строки расходятся, не применяется.\r\n\r\n",
            "The same database score also matches \"%s\". The label is the earlier row. "
            "An attribute those rows disagree on is not applied.\r\n\r\n"),
            pInfo->szDriveDbTie);
    else if (pInfo->bDriveDbTieUnnamed)
        LectureAdd(szBuf, nBufLen, TN(
            "Та же оценка базы есть у строки без имени семейства. "
            "Подпись взята из более ранней строки. "
            "Атрибут, в котором эти строки расходятся, не применяется.\r\n\r\n",
            "The same database score also matches a row with no family name. "
            "The label is the earlier row. "
            "An attribute those rows disagree on is not applied.\r\n\r\n"));
    if (pInfo->bDriveDbTieUnnamed && !pInfo->szModelFamily[0])
        LectureAdd(szBuf, nBufLen, TN(
            "У более ранней строки нет имени семейства.\r\n\r\n",
            "The earlier row has no family name.\r\n\r\n"));
    if (pInfo->szDriveDbWarn[0])
        LectureAddF(szBuf, nBufLen, TN(
            "Замечание к семейству, не оценка этого диска: %s\r\n\r\n",
            "Family note, not this disk's verdict: %s\r\n\r\n"),
                    FamilyWarnText(pInfo));
}

static BOOL LectureAddSmartDataError(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    if (!SmartSnapshotRejected(pInfo)) return FALSE;
    LectureAdd(szBuf, nBufLen, TN(
        "Ошибка транспорта или данных SMART. Пакет атрибутов повреждён или обрезан, "
        "поэтому снимок недействителен и здоровье не оценивается.\r\n",
        "SMART transport or data error. The attribute packet is damaged or truncated, "
        "so the snapshot is invalid and health is not assessed.\r\n"));
    return TRUE;
}

static void LectureAddParkCause(char* szBuf, int nBufLen, const DRIVE_INFO* pInfo)
{
    int v193;
    if (!pInfo) return;
    v193 = AttrValueOrNeg1(pInfo, 0xC1);
    if (v193 < 0 || v193 > 5) return;
    if (pInfo->nLoadUnload >= 0)
        LectureAddF(szBuf, nBufLen, TN(
            "Парковка головок: значение %d, циклов %d. Это механика, не поверхность.\r\n",
            "Head parking: value %d, %d cycles. That is mechanics, not the platters.\r\n"),
            v193, pInfo->nLoadUnload);
    else
        LectureAddF(szBuf, nBufLen, TN(
            "Парковка головок: значение %d. Это механика, не поверхность.\r\n",
            "Head parking: value %d. That is mechanics, not the platters.\r\n"),
            v193);
}

static void FormatHddLecturePlain(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    int r, pend, u;

    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = '\0';
    LectureAddDualStatus(szBuf, nBufLen, pInfo);

    if (!pInfo->bSMART_Supported) {
        if (LectureAddSmartDataError(pInfo, szBuf, nBufLen))
            return;
        LectureAdd(szBuf, nBufLen, TN(
            "SMART недоступен. Без исходных данных оценка не ставится.\r\n",
            "SMART is unavailable. No assessment without source data.\r\n"));
        return;
    }

    r = pInfo->nReallocated;
    pend = pInfo->nPendingSectors;
    u = pInfo->nUncorrectable;

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        LectureAdd(szBuf, nBufLen, TN(
            "Критических проблем не обнаружено.\r\n"
            "Prefail-атрибуты в норме, повреждение поверхности не подтверждено.\r\n",
            "No critical problems found.\r\n"
            "Prefail attributes are in range; no confirmed media damage.\r\n"));
        break;
    case HEALTH_STATUS_OBSERVE:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        {
            BOOL bSaid = FALSE;
            if (pInfo->bPrefailPast) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Prefail-атрибут раньше опускался до порога. "
                    "Сейчас значение снова выше порога.\r\n",
                    "A prefail attribute reached its threshold before. "
                    "The value is back above the threshold.\r\n"));
                bSaid = TRUE;
            }
            if (pInfo->bUsageFailed) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Атрибут износа (old-age) на пороге. Это износ, не прогноз отказа.\r\n",
                    "An old-age attribute is at its threshold. That is wear, not a failure forecast.\r\n"));
                bSaid = TRUE;
            }
            if (!bSaid)
                LectureAdd(szBuf, nBufLen, TN(
                    "Есть факторы риска, повреждение поверхности не подтверждено.\r\n",
                    "There are risk factors. Media damage is not confirmed.\r\n"));
            LectureAdd(szBuf, nBufLen, "\r\n");
        }
        LectureAdd(szBuf, nBufLen, TN("Признаков повреждения поверхности:\r\n",
                                      "Media-damage signs:\r\n"));
        LectureAddF(szBuf, nBufLen, TN("  %s Переназначенные: %d\r\n",
                                       "  %s Reallocated: %d\r\n"),
                    (r <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", r < 0 ? 0 : r);
        LectureAddF(szBuf, nBufLen, TN("  %s Ожидающие: %d\r\n",
                                       "  %s Pending: %d\r\n"),
                    (pend <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", pend < 0 ? 0 : pend);
        LectureAddF(szBuf, nBufLen, TN("  %s Неисправимые: %d\r\n\r\n",
                                       "  %s Uncorrectable: %d\r\n\r\n"),
                    (u <= 0) ? "\xE2\x9C\x93" : "\xE2\x9C\x97", u < 0 ? 0 : u);
        LectureAdd(szBuf, nBufLen, TN("Рекомендация: Следить за динамикой SMART.\r\n",
                                      "Recommendation: Watch SMART over time.\r\n"));
        break;
    case HEALTH_STATUS_CAUTION:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        if (HealthRank(pInfo->eReliability) >= HealthRank(HEALTH_STATUS_CAUTION)) {
            LectureAdd(szBuf, nBufLen, TN(
                "Есть реальные признаки деградации носителя.\r\n\r\n",
                "There are real signs of media degradation.\r\n\r\n"));
            if (pend > 0)
                LectureAddF(szBuf, nBufLen, TN("Ожидающие сектора: %d\r\n",
                                               "Pending sectors: %d\r\n"), pend);
            if (r > 0)
                LectureAddF(szBuf, nBufLen, TN("Переназначенные сектора: %d\r\n",
                                               "Reallocated sectors: %d\r\n"), r);
            if (pInfo->nRemapEvents > 0)
                LectureAddF(szBuf, nBufLen, TN("События переназначения: %d\r\n",
                                               "Reallocation events: %d\r\n"),
                            pInfo->nRemapEvents);
            LectureAdd(szBuf, nBufLen,
                TN("\r\nРекомендация: Держать резервную копию и следить за SMART.\r\n",
                   "\r\nRecommendation: Keep a backup and watch SMART.\r\n"));
        } else if (HealthRank(pInfo->eInterface) >= HealthRank(HEALTH_STATUS_CAUTION)) {
            int n188 = AttrRawOrNeg1(pInfo, 0xBC);
            BOOL bCrc = pInfo->nCrcErrors > 0;
            BOOL bTmo = n188 >= 10;
            if (bCrc && bTmo) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: ошибки CRC и таймауты команд. Носитель в норме — кабель, контакт или питание, не поверхность.\r\n",
                    "Interface: CRC errors and command timeouts. The media is fine — cable, contact, or power, not the platters.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Ошибки CRC: %d\r\n",
                                               "  CRC errors: %d\r\n"),
                            pInfo->nCrcErrors);
                LectureAddF(szBuf, nBufLen, TN("  Таймауты команд (188): %d\r\n",
                                               "  Command timeouts (188): %d\r\n"),
                            n188);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель, контакт и питание. Рост CRC важнее абсолютного числа.\r\n",
                    "\r\nRecommendation: Check the cable, contact, and power. A rising CRC count matters more than the total.\r\n"));
            } else if (bCrc) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: ошибки CRC. Носитель в норме — кабель, контакт или питание, не поверхность.\r\n",
                    "Interface: CRC errors. The media is fine — cable, contact, or power, not the platters.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Ошибки CRC: %d\r\n",
                                               "  CRC errors: %d\r\n"),
                            pInfo->nCrcErrors);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель, контакт и питание. Рост CRC важнее абсолютного числа.\r\n",
                    "\r\nRecommendation: Check the cable, contact, and power. A rising CRC count matters more than the total.\r\n"));
            } else if (bTmo) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: таймауты команд. Носитель в норме — кабель, контакт или питание, не поверхность.\r\n",
                    "Interface: command timeouts. The media is fine — cable, contact, or power, not the platters.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Таймауты команд (188): %d\r\n",
                                               "  Command timeouts (188): %d\r\n"),
                            n188);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель, контакт и питание.\r\n",
                    "\r\nRecommendation: Check the cable, contact, and power.\r\n"));
            } else {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: повторы раскрутки. Носитель в норме — сначала питание и кабель, не поверхность.\r\n",
                    "Interface: spin retries. The media is fine — check power and the cable first, not the platters.\r\n"));
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить питание и кабель.\r\n",
                    "\r\nRecommendation: Check power and the cable.\r\n"));
            }
            if (HealthRank(pInfo->eMechanics) >= HealthRank(HEALTH_STATUS_CAUTION))
                LectureAddParkCause(szBuf, nBufLen, pInfo);
        } else if (HealthRank(pInfo->eTempStatus) >= HealthRank(HEALTH_STATUS_CAUTION)) {
            LectureAdd(szBuf, nBufLen, TN(
                "Диск перегрет. Поверхность в норме. Это не повреждение пластин.\r\n",
                "The drive is too hot. The surface is fine. This is not platter damage.\r\n"));
            LectureAdd(szBuf, nBufLen, TN(
                "\r\nРекомендация: Охладить диск и следить за температурой.\r\n",
                "\r\nRecommendation: Cool the drive and watch the temperature.\r\n"));
        } else if (HealthRank(pInfo->eMechanics) >= HealthRank(HEALTH_STATUS_CAUTION)) {
            LectureAdd(szBuf, nBufLen, TN(
                "Механика требует внимания. Плохих секторов нет — это не повреждение поверхности.\r\n",
                "Mechanics need attention. There are no bad sectors — the platters are not failing.\r\n"));
            LectureAddParkCause(szBuf, nBufLen, pInfo);
            LectureAdd(szBuf, nBufLen, TN(
                "\r\nРекомендация: Держать резервную копию и следить за SMART.\r\n",
                "\r\nRecommendation: Keep a backup and watch SMART.\r\n"));
        } else {
            LectureAdd(szBuf, nBufLen, TN(
                "Есть признаки, требующие наблюдения. Повреждение поверхности не подтверждено.\r\n",
                "There are signs that need watching. No confirmed media damage.\r\n"));
            LectureAdd(szBuf, nBufLen, TN(
                "\r\nРекомендация: Следить за динамикой SMART.\r\n",
                "\r\nRecommendation: Watch SMART over time.\r\n"));
        }
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        LectureAdd(szBuf, nBufLen, TN(
            "Обнаружены признаки деградации носителя.\r\n\r\n",
            "Signs of media degradation were found.\r\n\r\n"));
        if (pend > 0)
            LectureAddF(szBuf, nBufLen, TN("Ожидающие сектора: %d\r\n",
                                           "Pending sectors: %d\r\n"), pend);
        if (r > 0)
            LectureAddF(szBuf, nBufLen, TN("Переназначенные сектора: %d\r\n",
                                           "Reallocated sectors: %d\r\n"), r);
        if (u > 0)
            LectureAddF(szBuf, nBufLen, TN("Неисправимые сектора: %d\r\n",
                                           "Uncorrectable sectors: %d\r\n"), u);
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 > 0)
                LectureAddF(szBuf, nBufLen, TN("Неисправимые ошибки (187): %d\r\n",
                                               "Uncorrectable errors (187): %d\r\n"), n187);
        }
        LectureAdd(szBuf, nBufLen,
            TN("\r\nРекомендация: Немедленно создать резервную копию.\r\n",
               "\r\nRecommendation: Back up immediately.\r\n"));
        break;
    case HEALTH_STATUS_CRITICAL:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        LectureAdd(szBuf, nBufLen, TN(
            "Высокий риск отказа (самотест failed или быстрое ухудшение).\r\n\r\n",
            "High risk of failure (self-test failed or rapid worsening).\r\n\r\n"));
        LectureAdd(szBuf, nBufLen,
            TN("Рекомендация: Немедленно копировать данные, диск к замене.\r\n",
               "Recommendation: Copy data now; the drive should be replaced.\r\n"));
        break;
    default:
        break;
    }

    if (pInfo->nGSenseEvents >= 0 || pInfo->nEmergencyRetract >= 0 ||
        pInfo->nLoadUnload >= 0) {
        char szC1[32];
        LectureAdd(szBuf, nBufLen, TN("\r\nМеханика\r\n", "\r\nMechanics\r\n"));
        if (pInfo->nGSenseEvents >= 0)
            LectureAddF(szBuf, nBufLen, TN("  G-Sense: %d событий\r\n", "  G-Sense: %d events\r\n"),
                        pInfo->nGSenseEvents);
        if (pInfo->nEmergencyRetract >= 0)
            LectureAddF(szBuf, nBufLen, TN("  Парковки при выключении: %d\r\n", "  Power-off retracts: %d\r\n"),
                        pInfo->nEmergencyRetract);
        if (pInfo->nLoadUnload >= 0) {
            FormatUintGrouped((unsigned long)pInfo->nLoadUnload, szC1, (int)sizeof(szC1));
            LectureAddF(szBuf, nBufLen, TN("  Циклы парковки головок: %s\r\n", "  Load/unload cycles: %s\r\n"),
                        szC1);
        }
    }
    if (pInfo->dwPowerOnHours > 0) {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        LectureAddF(szBuf, nBufLen, TN("\r\nНаработка: %s\r\n", "\r\nPower-on: %s\r\n"), szPoh);
    }
}

static void FormatSsdLecturePlain(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    if (!szBuf || nBufLen <= 0) return;
    szBuf[0] = 0;
    LectureAddDualStatus(szBuf, nBufLen, pInfo);

    if (!pInfo->bSMART_Supported) {
        if (LectureAddSmartDataError(pInfo, szBuf, nBufLen))
            return;
        LectureAdd(szBuf, nBufLen, TN(
            "SMART недоступен. Без исходных данных оценка не ставится.\r\n",
            "SMART is unavailable. No assessment without source data.\r\n"));
        return;
    }

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        LectureAdd(szBuf, nBufLen, TN(
            "Критических проблем не обнаружено.\r\n",
            "No critical problems found.\r\n"));
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 20)
            LectureAddF(szBuf, nBufLen, TN(
                "Носитель, интерфейс и температура в норме. "
                "Остаток ресурса %d%% — износ NAND, не здоровье.\r\n",
                "Media, interface and temperature are OK. "
                "Life remaining %d%% is NAND wear, not health.\r\n"),
                pInfo->nEndurancePercent);
        else
            LectureAdd(szBuf, nBufLen,
                TN("Носитель, ресурс, интерфейс и температура в норме.\r\n",
                   "Media, wear, interface and temperature are OK.\r\n"));
        break;
    case HEALTH_STATUS_OBSERVE:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        if (pInfo->eWear == HEALTH_STATUS_OBSERVE && pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen, TN(
                "Остаток ресурса %d%%. Это ещё не отказ, но запас NAND снижается.\r\n",
                "Life remaining %d%%. Not a failure yet, but the NAND reserve is falling.\r\n"),
                pInfo->nEndurancePercent);
        else if (pInfo->eTempStatus == HEALTH_STATUS_OBSERVE ||
                 pInfo->eTempStatus == HEALTH_STATUS_CAUTION)
            LectureAdd(szBuf, nBufLen, TN(
                "Повышенная температура. Повреждение носителя не подтверждено.\r\n",
                "Temperature is elevated. Media damage is not confirmed.\r\n"));
        else
            LectureAdd(szBuf, nBufLen, TN(
                "Есть факторы риска, повреждение носителя не подтверждено.\r\n",
                "There are risk factors. Media damage is not confirmed.\r\n"));
        LectureAdd(szBuf, nBufLen, TN(
            "\r\nРекомендация: Следить за SMART и держать резервную копию.\r\n",
            "\r\nRecommendation: Watch SMART and keep a backup.\r\n"));
        break;
    case HEALTH_STATUS_CAUTION:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        if (HealthRank(pInfo->eReliability) < HealthRank(HEALTH_STATUS_CAUTION) &&
            HealthRank(pInfo->eInterface) >= HealthRank(HEALTH_STATUS_CAUTION)) {
            int n188 = AttrRawOrNeg1(pInfo, 0xBC);
            BOOL bCrc = pInfo->nCrcErrors > 0;
            BOOL bTmo = n188 >= 10;
            if (bCrc && bTmo) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: ошибки CRC и таймауты. Носитель в норме — кабель, контакт или порт, не износ NAND.\r\n",
                    "Interface: CRC errors and timeouts. The media is fine — cable, contact, or port, not NAND wear.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Ошибки CRC: %d\r\n", "  CRC errors: %d\r\n"),
                            pInfo->nCrcErrors);
                LectureAddF(szBuf, nBufLen, TN("  Таймауты команд (188): %d\r\n",
                                               "  Command timeouts (188): %d\r\n"), n188);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель и порт. Рост CRC важнее абсолютного числа.\r\n",
                    "\r\nRecommendation: Check the cable and port. A rising CRC count matters more than the total.\r\n"));
            } else if (bCrc) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: ошибки CRC. Носитель в норме — кабель, контакт или порт, не износ NAND.\r\n",
                    "Interface: CRC errors. The media is fine — cable, contact, or port, not NAND wear.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Ошибки CRC: %d\r\n", "  CRC errors: %d\r\n"),
                            pInfo->nCrcErrors);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель и порт. Рост CRC важнее абсолютного числа.\r\n",
                    "\r\nRecommendation: Check the cable and port. A rising CRC count matters more than the total.\r\n"));
            } else if (bTmo) {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: таймауты команд. Носитель в норме — кабель, контакт или порт, не износ NAND.\r\n",
                    "Interface: command timeouts. The media is fine — cable, contact, or port, not NAND wear.\r\n"));
                LectureAddF(szBuf, nBufLen, TN("  Таймауты команд (188): %d\r\n",
                                               "  Command timeouts (188): %d\r\n"), n188);
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель и порт.\r\n",
                    "\r\nRecommendation: Check the cable and port.\r\n"));
            } else {
                LectureAdd(szBuf, nBufLen, TN(
                    "Интерфейс: повторы раскрутки. Носитель в норме — кабель, контакт или порт, не износ NAND.\r\n",
                    "Interface: spin retries. The media is fine — cable, contact, or port, not NAND wear.\r\n"));
                LectureAdd(szBuf, nBufLen, TN(
                    "\r\nРекомендация: Проверить кабель и порт.\r\n",
                    "\r\nRecommendation: Check the cable and port.\r\n"));
            }
            break;
        }
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 5)
            LectureAddF(szBuf, nBufLen, TN(
                "Остаток ресурса %d%%. Запас NAND почти исчерпан — "
                "это износ, не ошибка носителя.\r\n",
                "Life remaining %d%%. The NAND reserve is nearly exhausted — "
                "that is wear, not a media error.\r\n"),
                pInfo->nEndurancePercent);
        else if (pInfo->eWear == HEALTH_STATUS_CAUTION &&
                 pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen, TN("Остаток ресурса %d%%.\r\n",
                                           "Life remaining %d%%.\r\n"),
                        pInfo->nEndurancePercent);
        if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, TN("Переназначенные сектора: %d\r\n",
                                           "Reallocated sectors: %d\r\n"),
                        pInfo->nReallocated);
        if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen,
                TN("Ожидающие сектора: %d. Могут уменьшиться, если сектор потом прочитался.\r\n",
                   "Pending sectors: %d. The count can drop if a sector reads later.\r\n"),
                pInfo->nPendingSectors);
        if (pInfo->qwNVMeMediaErrors > 0)
            LectureAddF(szBuf, nBufLen, TN("Ошибки носителя NVMe: %llu\r\n",
                                           "NVMe media errors: %llu\r\n"),
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureAdd(szBuf, nBufLen,
            TN("\r\nРекомендация: Держать резервную копию и следить за SMART.\r\n",
               "\r\nRecommendation: Keep a backup and watch SMART.\r\n"));
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING: {
        BOOL named = FALSE;
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        if (pInfo->nEndurancePercent >= 0 && pInfo->nEndurancePercent <= 5) {
            LectureAddF(szBuf, nBufLen, TN("Остаток ресурса %d%%.\r\n",
                                           "Life remaining %d%%.\r\n"),
                        pInfo->nEndurancePercent);
            named = TRUE;
        }
        if (pInfo->nReallocated > 0) {
            LectureAddF(szBuf, nBufLen, TN("Переназначенные сектора: %d\r\n",
                                           "Reallocated sectors: %d\r\n"),
                        pInfo->nReallocated);
            named = TRUE;
        }
        if (pInfo->nPendingSectors > 0) {
            LectureAddF(szBuf, nBufLen, TN("Ожидающие сектора: %d\r\n",
                                           "Pending sectors: %d\r\n"),
                        pInfo->nPendingSectors);
            named = TRUE;
        }
        if (pInfo->nUncorrectable > 0) {
            LectureAddF(szBuf, nBufLen, TN("Неисправимые сектора: %d\r\n",
                                           "Uncorrectable sectors: %d\r\n"),
                        pInfo->nUncorrectable);
            named = TRUE;
        }
        if (pInfo->nRemapEvents > 0) {
            LectureAddF(szBuf, nBufLen, TN("События переназначения: %d\r\n",
                                           "Reallocation events: %d\r\n"),
                        pInfo->nRemapEvents);
            named = TRUE;
        }
        if (pInfo->qwNVMeMediaErrors > 0) {
            LectureAddF(szBuf, nBufLen, TN("Ошибки носителя: %llu\r\n",
                                           "Media errors: %llu\r\n"),
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
            named = TRUE;
        }
        if (!named)
            LectureAdd(szBuf, nBufLen, TN(
                "Обнаружены признаки деградации носителя.\r\n",
                "Signs of media degradation were found.\r\n"));
        LectureAdd(szBuf, nBufLen,
            TN("\r\nРекомендация: Немедленно создать резервную копию.\r\n",
               "\r\nRecommendation: Back up immediately.\r\n"));
        break;
    }
    case HEALTH_STATUS_CRITICAL:
        LectureAdd(szBuf, nBufLen, TN("Причина:\r\n", "Reason:\r\n"));
        LectureAdd(szBuf, nBufLen, TN(
            "Контроллер сообщает о серьёзной деградации (read-only или reliability).\r\n\r\n",
            "The controller reports serious degradation (read-only or reliability).\r\n\r\n"));
        LectureAdd(szBuf, nBufLen,
            TN("Рекомендация: Немедленно копировать данные, диск к замене.\r\n",
               "Recommendation: Copy data now; the drive should be replaced.\r\n"));
        break;
    default:
        break;
    }

    if (pInfo->nEndurancePercent >= 0)
        LectureAddF(szBuf, nBufLen, TN("\r\nРесурс: %d%%\r\n",
                                       "\r\nLife remaining: %d%%\r\n"),
                    pInfo->nEndurancePercent);
    if (pInfo->dwPowerOnHours > 0) {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        LectureAddF(szBuf, nBufLen, TN("Наработка: %s\r\n", "Power-on: %s\r\n"), szPoh);
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
        safe_snprintf_n(szBuf, nBufLen, "%s", Tr(STR_NO_DRIVE));
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
        if (LectureAddSmartDataError(pInfo, szBuf, nBufLen))
            return;
        if (pInfo->bIsUSB && IsLikelyUsbFlashDrive(pInfo)) {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. Это USB-флешка: контроллер обычно не отдаёт атрибуты SMART, "
                "поэтому состояние не оценивается. Процент здоровья не выдумывается.\r\n",
                "SMART is unavailable. This is a USB flash drive: the controller usually does not "
                "provide SMART attributes, so the condition is not assessed. A health percent is not invented.\r\n"));
        } else if (pInfo->bIsUSB) {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. USB-мост или корпус не пропускает команды SMART. "
                "Без исходных данных оценка не ставится.\r\n",
                "SMART is unavailable. The USB bridge or enclosure does not pass SMART commands. "
                "No assessment without source data.\r\n"));
        } else if (pInfo->bIsNVMe) {
            LectureAddF(szBuf, nBufLen, TN(
                "Не удалось прочитать журнал здоровья NVMe (код ошибки Windows %lu). "
                "Без журнала оценка не ставится.\r\n",
                "Could not read the NVMe health log (Windows error %lu). "
                "No assessment without the log.\r\n"),
                (unsigned long)pInfo->dwErrNvmeProtocol);
        } else {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. Без исходных данных оценка не ставится. "
                "Запустите программу от имени администратора и нажмите «Обновить».\r\n",
                "SMART is unavailable. No assessment without source data. "
                "Run the program as administrator and press Reread.\r\n"));
        }
        return;
    }

    switch (pInfo->eHealthStatus) {
    case HEALTH_STATUS_GOOD:
        szHead = TN("Критических проблем не обнаружено.",
                    "No critical problems found.");
        break;
    case HEALTH_STATUS_OBSERVE:
        szHead = TN("Есть факторы риска, повреждение поверхности не подтверждено.",
                    "There are risk factors. Media damage is not confirmed.");
        break;
    case HEALTH_STATUS_CAUTION:
        szHead = TN("Есть признаки, требующие наблюдения, но критической деградации не обнаружено.",
                    "There are signs that need watching, but no critical degradation was found.");
        break;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        szHead = TN("Обнаружены признаки физической деградации.",
                    "Signs of physical degradation were found.");
        break;
    case HEALTH_STATUS_CRITICAL:
        szHead = TN("Высокий риск отказа.", "High risk of failure.");
        break;
    default:
        szHead = TN("Критических проблем не обнаружено.",
                    "No critical problems found.");
        break;
    }
    LectureAddF(szBuf, nBufLen, "%s\r\n\r\n", szHead);

    if (pInfo->bPredictFailure)
        lstrcpynA(szSmart, TN("сбой", "failed"), sizeof(szSmart));
    else
        lstrcpynA(szSmart, TN("в норме", "normal"), sizeof(szSmart));

    LectureAdd(szBuf, nBufLen, TN("Носитель\r\n", "Media\r\n"));
    LectureAddFact(szBuf, nBufLen, "SMART", szSmart);
    if (pInfo->bIsNVMe) {
        char szMedia[32], szSpare[32];
        safe_snprintf(szMedia, "%llu", (unsigned long long)pInfo->qwNVMeMediaErrors);
        safe_snprintf(szSpare, "%d%%", (int)pInfo->nvmeHealth.AvailableSpare);
        LectureAddFact(szBuf, nBufLen, TN("Ошибки носителя", "Media errors"), szMedia);
        LectureAddFact(szBuf, nBufLen, TN("Запас блоков", "Available spare"), szSpare);
    } else {
        char szRemap[24];
        FmtCountPlain(szR,    (int)sizeof(szR),    pInfo->nReallocated);
        FmtCountPlain(szPend, (int)sizeof(szPend), pInfo->nPendingSectors);
        FmtCountPlain(szU,    (int)sizeof(szU),    pInfo->nUncorrectable);
        FmtCountPlain(szRemap,(int)sizeof(szRemap), pInfo->nRemapEvents);
        LectureAddFact(szBuf, nBufLen, TN("Переназначенные", "Reallocated"), szR);
        LectureAddFact(szBuf, nBufLen, TN("Ожидающие", "Pending"), szPend);
        if (pInfo->nPendingSectors > 0)
            LectureAdd(szBuf, nBufLen, TN(
                "  (могут уменьшиться при повторном чтении)\r\n",
                "  (can drop on a later read)\r\n"));
        LectureAddFact(szBuf, nBufLen, TN("Неисправимые", "Uncorrectable"), szU);
        LectureAddFact(szBuf, nBufLen, TN("События переназначения", "Reallocation events"), szRemap);
        if (WriteErrorScored(pInfo)) {
            char szWe[64];
            if (pInfo->nWriteErrorValue <= 1)
                safe_snprintf(szWe, TN("значение %d (шкала исчерпана)",
                                       "value %d (scale exhausted)"),
                              pInfo->nWriteErrorValue);
            else
                safe_snprintf(szWe, TN("значение %d", "value %d"),
                              pInfo->nWriteErrorValue);
            LectureAddFact(szBuf, nBufLen, TN("Ошибки записи (200)", "Write errors (200)"), szWe);
        }
    }

    LectureAdd(szBuf, nBufLen, TN("\r\nИнтерфейс\r\n", "\r\nInterface\r\n"));
    if (pInfo->nCrcErrors >= 0) {
        FmtCountPlain(szCrc, (int)sizeof(szCrc), pInfo->nCrcErrors);
        LectureAddFact(szBuf, nBufLen, TN("Ошибки CRC", "CRC errors"), szCrc);
    } else if (pInfo->bIsNVMe) {
        LectureAddFact(szBuf, nBufLen, TN("Ссылка", "Link"), TN("в норме", "normal"));
    } else {
        LectureAddFact(szBuf, nBufLen, TN("Ошибки CRC", "CRC errors"), Tr(STR_POH_NONE));
    }
    {
        int n188 = AttrRawOrNeg1(pInfo, 0xBC);
        int n189 = AttrRawOrNeg1(pInfo, 0xBD);
        char szTmo[24];
        if (n188 >= 0) {
            FmtCountPlain(szTmo, (int)sizeof(szTmo), n188);
            LectureAddFact(szBuf, nBufLen,
                TN("Таймауты команд (188)", "Command timeouts (188)"), szTmo);
        }
        if (n189 > 0) {
            FmtCountPlain(szTmo, (int)sizeof(szTmo), n189);
            LectureAddFact(szBuf, nBufLen,
                TN("Записи на большой высоте (189)", "High-fly writes (189)"), szTmo);
        }
    }

    if (pInfo->eMechanics != HEALTH_STATUS_UNKNOWN || pInfo->nGSenseEvents >= 0) {
        char szGs[40], szC0[32], szC1[32], szMech[48];
        LectureAdd(szBuf, nBufLen, TN("\r\nМеханика\r\n", "\r\nMechanics\r\n"));
        if (pInfo->eMechanics == HEALTH_STATUS_GOOD)
            lstrcpynA(szMech, Tr(STR_AXIS_OK), sizeof(szMech));
        else
            lstrcpynA(szMech, GetHealthStatusName(pInfo->eMechanics), sizeof(szMech));
        LectureAddFact(szBuf, nBufLen, TN("Состояние", "Condition"), szMech);
        if (pInfo->nGSenseEvents >= 0) {
            safe_snprintf(szGs, TN("%d событий", "%d events"), pInfo->nGSenseEvents);
            LectureAddFact(szBuf, nBufLen, "G-Sense", szGs);
        }
        if (pInfo->nEmergencyRetract >= 0) {
            safe_snprintf(szC0, "%d", pInfo->nEmergencyRetract);
            if (DriveTreatsC0AsPowerLoss(pInfo))
                LectureAddFact(szBuf, nBufLen, TN("Аварийные отключения", "Unsafe power-offs"), szC0);
            else
                LectureAddFact(szBuf, nBufLen, TN("Парковки при выключении", "Power-off retracts"), szC0);
        }
        if (pInfo->nLoadUnload >= 0) {
            FormatUintGrouped((unsigned long)pInfo->nLoadUnload, szC1, (int)sizeof(szC1));
            LectureAddFact(szBuf, nBufLen, TN("Циклы парковки головок", "Load/unload cycles"), szC1);
        }
        if (pInfo->nGSenseDelta >= 0) {
            char szD[48];
            safe_snprintf(szD, "%+d", pInfo->nGSenseDelta);
            LectureAddFact(szBuf, nBufLen, TN("Динамика", "Trend"), szD);
        }
        if (pInfo->eMechanics == HEALTH_STATUS_OBSERVE) {
            int n187o = AttrRawOrNeg1(pInfo, 0xBB);
            LectureAdd(szBuf, nBufLen, "\r\n");
            if (n187o > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "G-Sense фиксирует удары и вибрацию. Счётчики 5/196/197/198 равны 0, "
                    "но атрибут 187 = %d. Это отдельный счётчик, не доказательство, что удары повредили поверхность.\r\n",
                    "G-Sense records shock and vibration. Counters 5/196/197/198 are 0, "
                    "but attribute 187 is %d. That is a separate count, not proof the shocks damaged the surface.\r\n"),
                    n187o);
            else
                LectureAdd(szBuf, nBufLen, TN(
                    "G-Sense фиксирует события, связанные с ударом/вибрацией. "
                    "Признаков повреждения поверхности на текущем SMART-снимке нет: 5/196/197/198 = 0.\r\n",
                    "G-Sense records shock or vibration. "
                    "This SMART snapshot shows no surface damage: 5/196/197/198 = 0.\r\n"));
        }
    }

    FormatTempLecture(pInfo, szTemp, (int)sizeof(szTemp));
    LectureAdd(szBuf, nBufLen, "\r\n");
    LectureAddFact(szBuf, nBufLen, TN("Температура", "Temperature"), szTemp);

    if (pInfo->eType != DRIVE_TYPE_HDD) {
        LectureAdd(szBuf, nBufLen, "\r\n");
        if (pInfo->nEndurancePercent >= 0)
            safe_snprintf(szWear, "%d%%", pInfo->nEndurancePercent);
        else
            lstrcpynA(szWear, TN("нет достоверного показателя", "no confirmed figure"),
                      sizeof(szWear));
        LectureAddFact(szBuf, nBufLen, TN("Остаток ресурса", "Life remaining"), szWear);
    }

    LectureAdd(szBuf, nBufLen, "\r\n");
    FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
    LectureAddFact(szBuf, nBufLen, TN("Наработка", "Power-on"), szPoh);
    if (pInfo->dwPowerCycleCount > 0) {
        FormatUintGrouped((unsigned long)pInfo->dwPowerCycleCount, szCyc, (int)sizeof(szCyc));
        LectureAddFact(szBuf, nBufLen, TN("Циклы включения", "Power cycles"), szCyc);
    } else {
        LectureAddFact(szBuf, nBufLen, TN("Циклы включения", "Power cycles"), Tr(STR_POH_NONE));
    }

    LectureAdd(szBuf, nBufLen, TN("\r\nПричина оценки\r\n\r\n", "\r\nWhy this assessment\r\n\r\n"));

    if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
        if (pInfo->eMechanics == HEALTH_STATUS_OBSERVE) {
            LectureAdd(szBuf, nBufLen, TN(
                "Носитель и интерфейс в норме. Механика — риск: G-Sense фиксирует события, "
                "связанные с ударом/вибрацией, но переназначенных, ожидающих и неисправимых секторов нет. "
                "Абсолютное число событий само по себе не снижает оценку.\r\n\r\n",
                "Media and interface are fine. Mechanics are a risk: G-Sense records shock or vibration, "
                "but there are no reallocated, pending, or uncorrectable sectors. "
                "The event count alone does not lower the assessment.\r\n\r\n"));
        } else if (pInfo->bIsNVMe) {
            LectureAdd(szBuf, nBufLen, TN(
                "Диск находится в хорошем состоянии. Журнал здоровья NVMe не сообщает об ошибках носителя "
                "и критических предупреждениях. Запас блоков в норме.\r\n\r\n",
                "The drive is in good condition. The NVMe health log reports no media errors "
                "and no critical warnings. Available spare is normal.\r\n\r\n"));
        } else {
            LectureAdd(szBuf, nBufLen, TN(
                "Диск находится в хорошем состоянии. SMART не показывает признаков деградации носителя: "
                "нет переназначенных, ожидающих или неисправимых секторов, а также ошибок интерфейса.\r\n\r\n",
                "The drive is in good condition. SMART shows no media degradation: "
                "no reallocated, pending, or uncorrectable sectors, and no interface errors.\r\n\r\n"));
        }
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
        if (HealthRank(pInfo->eReliability) < HealthRank(HEALTH_STATUS_CAUTION)) {
            if (HealthRank(pInfo->eInterface) >= HealthRank(HEALTH_STATUS_CAUTION))
                LectureAdd(szBuf, nBufLen, TN(
                    "Итог поднят из-за интерфейса: CRC, таймауты команд или повторы раскрутки. "
                    "Переназначенных и неисправимых секторов нет. Это кабель, контакт или питание, не смерть диска.\r\n\r\n",
                    "The headline is raised by the interface: CRC, command timeouts, or spin retries. "
                    "There are no reallocated or uncorrectable sectors. That is the cable, contact, or power, not a dying drive.\r\n\r\n"));
            else if (HealthRank(pInfo->eTempStatus) >= HealthRank(HEALTH_STATUS_CAUTION))
                LectureAdd(szBuf, nBufLen, TN(
                    "Итог поднят из-за температуры. Поверхность в норме. Диск нужно охладить, это не приговор пластинам.\r\n\r\n",
                    "The headline is raised by temperature. The surface is fine. Cool the drive; this is not a platter failure.\r\n\r\n"));
            else
                LectureAdd(szBuf, nBufLen, TN(
                    "Итог поднят из-за механики. Плохих секторов нет, абсолютное число G-Sense само по себе оценку не снижает.\r\n\r\n",
                    "The headline is raised by mechanics. There are no bad sectors; the raw G-Sense count alone does not lower the assessment.\r\n\r\n"));
        } else if (pInfo->eMechanics == HEALTH_STATUS_CAUTION || pInfo->eMechanics == HEALTH_STATUS_BAD)
            LectureAdd(szBuf, nBufLen, TN(
                "Есть связанная улика: события G-Sense совпадают с проблемными секторами. "
                "Это не штраф за абсолютное число G-Sense, а корреляция механики и поверхности.\r\n\r\n",
                "There is a linked clue: G-Sense events coincide with problem sectors. "
                "This is not a penalty for the raw G-Sense count; mechanics and the surface line up.\r\n\r\n"));
        else
            LectureAdd(szBuf, nBufLen, TN(
                "Есть признаки, требующие наблюдения, но критической деградации носителя не обнаружено.\r\n\r\n",
                "There are signs that need watching, but no critical media degradation was found.\r\n\r\n"));
    } else if (pInfo->eHealthStatus == HEALTH_STATUS_BAD ||
               pInfo->eHealthStatus == HEALTH_STATUS_WARNING) {
        if (WriteErrorScored(pInfo) && pInfo->nWriteErrorValue <= 1)
            LectureAddF(szBuf, nBufLen, TN(
                "Частота ошибок записи (ID 200) исчерпала нормализованную шкалу: значение %d, худший %d. "
                "Это признак проблем поверхности или головок, даже если переназначенных секторов ещё нет.\r\n\r\n",
                "Write error rate (ID 200) has exhausted the normalized scale: value %d, worst %d. "
                "That is a sign of surface or head trouble, even when no sectors have been reallocated yet.\r\n\r\n"),
                pInfo->nWriteErrorValue,
                pInfo->nWriteErrorWorst >= 0 ? pInfo->nWriteErrorWorst : pInfo->nWriteErrorValue);
        else if (pInfo->nGSenseEvents > 0 &&
            (pInfo->nPendingSectors > 0 || pInfo->nReallocated > 0 || pInfo->nUncorrectable > 0))
            LectureAdd(szBuf, nBufLen, TN(
                "Есть признаки возможного повреждения носителя. Зарегистрированы события G-Sense "
                "и одновременно появились проблемные сектора.\r\n\r\n",
                "There are signs of possible media damage. G-Sense events were recorded "
                "and problem sectors appeared at the same time.\r\n\r\n"));
        else if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen, TN(
                "Обнаружены признаки физической деградации: ожидающие сектора %d. "
                "Счётчик может уменьшиться, если сектор потом прочитался — это ещё не приговор.\r\n\r\n",
                "Signs of physical degradation: %d pending sectors. "
                "The count can drop if a sector reads later — not a verdict yet.\r\n\r\n"),
                pInfo->nPendingSectors);
        else if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, TN(
                "Обнаружены признаки физической деградации: переназначенные сектора %d.\r\n\r\n",
                "Signs of physical degradation: %d reallocated sectors.\r\n\r\n"),
                pInfo->nReallocated);
        else if (pInfo->qwNVMeMediaErrors > 0)
            LectureAddF(szBuf, nBufLen, TN(
                "Обнаружены признаки физической деградации: ошибки носителя %llu.\r\n\r\n",
                "Signs of physical degradation: %llu media errors.\r\n\r\n"),
                (unsigned long long)pInfo->qwNVMeMediaErrors);
        else if (pInfo->bPredictFailure)
            LectureAdd(szBuf, nBufLen, TN(
                "SMART сообщает о прогнозе отказа. Диск нужно копировать и заменять.\r\n\r\n",
                "SMART reports a failure prediction. Copy the data and replace the drive.\r\n\r\n"));
        else
            LectureAdd(szBuf, nBufLen, TN(
                "Обнаружены признаки физической деградации по SMART.\r\n\r\n",
                "SMART shows signs of physical degradation.\r\n\r\n"));
    }

    if (pInfo->eType != DRIVE_TYPE_HDD && pInfo->nEndurancePercent < 0)
        LectureAdd(szBuf, nBufLen, TN(
            "Остаток ресурса SSD: нет достоверного показателя.\r\n",
            "SSD life remaining: no confirmed figure.\r\n"));
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
        safe_snprintf_n(szBuf, nBufLen, "%s", Tr(STR_NO_DRIVE));
        return;
    }

    szState = GetHealthStatusName(pInfo->eHealthStatus);
    LectureAddDualStatus(szBuf, nBufLen, pInfo);
    LectureAddF(szBuf, nBufLen, TN("Сводка: %s\r\n", "Summary: %s\r\n"), szState);

    if (!pInfo->bSMART_Supported) {
        if (LectureAddSmartDataError(pInfo, szBuf, nBufLen))
            return;
        if (pInfo->bIsUSB && IsLikelyUsbFlashDrive(pInfo)) {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. Это USB-флешка: контроллер флешки обычно не отдаёт "
                "атрибуты SMART, поэтому состояние не оценивается. Числа здоровья не выдумываются.\r\n",
                "SMART is unavailable. This is a USB flash drive: the flash controller usually does not "
                "provide SMART attributes, so the condition is not assessed. Health figures are not invented.\r\n"));
        } else if (pInfo->bIsUSB) {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. USB-мост или корпус не пропускает команды SMART. "
                "Без исходных данных оценка не ставится — программа не подставляет фиктивные проценты.\r\n",
                "SMART is unavailable. The USB bridge or enclosure does not pass SMART commands. "
                "No assessment without source data — the program does not invent percentages.\r\n"));
        } else if (pInfo->bIsNVMe) {
            LectureAddF(szBuf, nBufLen, TN(
                "Не удалось прочитать журнал здоровья NVMe (код ошибки Windows %lu). "
                "Возможные причины: нет прав администратора, драйвер не поддерживает запрос, "
                "или устройство занято. Без журнала здоровья проценты не выдумываются.\r\n",
                "Could not read the NVMe health log (Windows error %lu). "
                "Possible causes: not running as administrator, the driver does not support the request, "
                "or the device is busy. Percentages are not invented without the health log.\r\n"),
                (unsigned long)pInfo->dwErrNvmeProtocol);
        } else {
            LectureAdd(szBuf, nBufLen, TN(
                "SMART недоступен. Без исходных данных оценка не ставится — "
                "программа не подставляет фиктивные проценты. Запустите программу "
                "от имени администратора и нажмите «Обновить».\r\n",
                "SMART is unavailable. No assessment without source data — "
                "the program does not invent percentages. Run the program "
                "as administrator and press Reread.\r\n"));
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
        LectureAddF(szBuf, nBufLen, TN("Качество нормализованных SMART: %s\r\n",
                                      "Normalized SMART quality: %s\r\n"),
                    QualityNameRu(pInfo->eNormQuality, FALSE));
        LectureAddF(szBuf, nBufLen, TN("Качество vendor RAW: %s\r\n",
                                      "Vendor RAW quality: %s\r\n"),
                    QualityNameRu(pInfo->eRawQuality, TRUE));
        if (pInfo->eNormQuality == NORM_QUALITY_LOW)
            LectureAdd(szBuf, nBufLen, TN(
                "Одинаковые Value/Worst/Threshold (100/100/50) не означают, что всё идеально — "
                "у этой прошивки нормализованные числа слабо различают состояние.\r\n",
                "Identical Value/Worst/Threshold (100/100/50) does not mean everything is fine — "
                "this firmware's normalized numbers barely separate conditions.\r\n"));
        LectureAdd(szBuf, nBufLen, "\r\n");
    }

    if (pInfo->bIsNVMe) {
        char szCW[192];
        char szSpare[32], szMedia[32], szLife[32];
        int nSpare   = (int)pInfo->nvmeHealth.AvailableSpare;
        int nSpareTh = (int)pInfo->nvmeHealth.AvailableSpareThreshold;

        FormatNvmeCritWarnBits(cw, szCW, (int)sizeof(szCW));

        LectureAdd(szBuf, nBufLen, TN("Основные признаки:\r\n", "Main evidence:\r\n"));
        if (cw == 0) {
            LectureAdd(szBuf, nBufLen, TN("  ✓ Критическое предупреждение: нет\r\n",
                                          "  ✓ Critical warning: none\r\n"));
        } else {
            LectureAddF(szBuf, nBufLen, TN("  ✗ Критическое предупреждение: %s\r\n",
                                           "  ✗ Critical warning: %s\r\n"), szCW);
        }
        if (!bSpareLow)
            LectureAddF(szBuf, nBufLen, TN("  ✓ Доступный запас: %d%%\r\n",
                                           "  ✓ Available spare: %d%%\r\n"), nSpare);
        else
            LectureAddF(szBuf, nBufLen, TN("  ✗ Доступный запас: %d%% (порог %d%%)\r\n",
                                           "  ✗ Available spare: %d%% (threshold %d%%)\r\n"),
                        nSpare, nSpareTh);
        if (pInfo->qwNVMeMediaErrors == 0)
            LectureAdd(szBuf, nBufLen, TN("  ✓ Ошибки носителя: 0\r\n",
                                          "  ✓ Media errors: 0\r\n"));
        else
            LectureAddF(szBuf, nBufLen, TN("  ✗ Ошибки носителя: %llu\r\n",
                                           "  ✗ Media errors: %llu\r\n"),
                        (unsigned long long)pInfo->qwNVMeMediaErrors);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen, TN(
                "  %s Заявленный остаток ресурса: %d%%   (износ NAND, не здоровье)\r\n",
                "  %s Reported life remaining: %d%%   (NAND wear, not health)\r\n"),
                pInfo->nEndurancePercent <= 20 ? "\xE2\x9A\xA0" : "\xE2\x9C\x93",
                pInfo->nEndurancePercent);

        LectureAdd(szBuf, nBufLen, TN("\r\nКонтекст (не штраф):\r\n", "\r\nContext (not a penalty):\r\n"));
        LectureAddF(szBuf, nBufLen, TN("  Температура: %s. ", "  Temperature: %s. "), szTempBand);
        LectureAddF(szBuf, nBufLen, TN(
            "Время на высокой температуре: предупреждение %lu мин, критическая %lu мин.\r\n",
            "Time at high temperature: warning %lu min, critical %lu min.\r\n"),
            (unsigned long)pInfo->nvmeHealth.WarningCompTempTime,
            (unsigned long)pInfo->nvmeHealth.CriticalCompTempTime);
        LectureAddF(szBuf, nBufLen, TN(
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n",
            "  Power-on: %s. Power-on hours are not a failure sign.\r\n"), szPoh);
        LectureAddF(szBuf, nBufLen, TN(
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            "  Power cycles: %lu. Context, not a penalty.\r\n"),
            (unsigned long)pInfo->dwPowerCycleCount);
        LectureAddF(szBuf, nBufLen, TN(
            "  Небезопасные выключения: %llu. Журнал питания, не штраф.\r\n",
            "  Unsafe shutdowns: %llu. A power log, not a penalty.\r\n"),
            (unsigned long long)pInfo->qwNVMeUnsafeShutdowns);

        LectureAdd(szBuf, nBufLen, TN("\r\nМатрица улик:\r\n", "\r\nEvidence matrix:\r\n"));
        LectureMatrixRow(szBuf, nBufLen,
                         TN("Улика", "Evidence"),
                         TN("Значение", "Value"),
                         TN("Влияние", "Effect"));
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         pInfo->bPredictFailure ? "FAIL" : "PASS",
                         pInfo->bPredictFailure ? "−" : "+");
        safe_snprintf(szSpare, "%d%%", nSpare);
        LectureMatrixRow(szBuf, nBufLen, TN("Доступный запас", "Available spare"), szSpare,
                         bSpareLow ? "−" : "+");
        safe_snprintf(szMedia, "%llu", (unsigned long long)pInfo->qwNVMeMediaErrors);
        LectureMatrixRow(szBuf, nBufLen, TN("Ошибки носителя", "Media errors"), szMedia,
                         pInfo->qwNVMeMediaErrors == 0 ? "+" : "−");
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, TN("Остаток ресурса", "Life remaining"), szLife,
                             pInfo->nEndurancePercent <= 20 ? "0" : "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "%s", Tr(STR_POH_NONE));
            LectureMatrixRow(szBuf, nBufLen, TN("Температура", "Temperature"), szT, "0");
        }
        {
            char szH[64];
            FormatPowerOnHours(pInfo->dwPowerOnHours, szH, (int)sizeof(szH));
            LectureMatrixRow(szBuf, nBufLen, TN("Наработка", "Power-on"), szH, "0");
        }
        LectureMatrixRow(szBuf, nBufLen, TN("Контроллер", "Controller"),
                         DriveControllerLabel(pInfo),
                         pInfo->eController == CONTROLLER_UNKNOWN ? TN("−уверенность", "−confidence") : "0");
        LectureProfileRow(szBuf, nBufLen, pInfo);

        LectureAdd(szBuf, nBufLen, TN("\r\nИтог:\r\n", "\r\nVerdict:\r\n"));
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD) {
            LectureAdd(szBuf, nBufLen, TN("  ХОРОШО, потому что:\r\n", "  GOOD because:\r\n"));
        } else if (pInfo->eHealthStatus == HEALTH_STATUS_OBSERVE) {
            LectureAdd(szBuf, nBufLen, TN("  РИСК, потому что:\r\n", "  WATCH because:\r\n"));
        } else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION) {
            LectureAdd(szBuf, nBufLen, TN("  ВНИМАНИЕ, потому что:\r\n", "  NEEDS ATTENTION because:\r\n"));
        } else {
            LectureAdd(szBuf, nBufLen, TN("  ПЛОХО, потому что:\r\n", "  BAD because:\r\n"));
        }
        LectureAddF(szBuf, nBufLen, TN("    %d сильных положительных признаков\r\n",
                                      "    %d strong positive signs\r\n"), nPos);
        LectureAddF(szBuf, nBufLen, TN("    %d критических отказов\r\n",
                                      "    %d critical failures\r\n"), nCritFail);
        LectureAddF(szBuf, nBufLen, TN("    %d признаков деградации носителя\r\n",
                                      "    %d signs of media degradation\r\n"), nMediaDeg);
        LectureAdd(szBuf, nBufLen, TN(
            "Использованный ресурс — износ NAND, его не смешивают с запасом и ошибками. "
            "Мы не превращаем использованный ресурс, запас, критическое предупреждение, "
            "ошибки носителя и температуру в один процент здоровья.\r\n",
            "Percentage used is NAND wear. It is not mixed with spare or media errors. "
            "Percentage used, spare, the critical warning, media errors, and temperature "
            "are not folded into one health percent.\r\n"));
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

        LectureAdd(szBuf, nBufLen, TN("Основные признаки:\r\n", "Main evidence:\r\n"));
        if (pInfo->nReallocated == 0)
            LectureAdd(szBuf, nBufLen, TN("  ✓ Переназначенные сектора: 0\r\n",
                                          "  ✓ Reallocated sectors: 0\r\n"));
        else if (pInfo->nReallocated > 0)
            LectureAddF(szBuf, nBufLen, TN("  ✗ Переназначенные сектора: %d\r\n",
                                           "  ✗ Reallocated sectors: %d\r\n"),
                        pInfo->nReallocated);
        if (pInfo->nPendingSectors == 0)
            LectureAdd(szBuf, nBufLen, TN("  ✓ Ожидающие сектора: 0\r\n",
                                          "  ✓ Pending sectors: 0\r\n"));
        else if (pInfo->nPendingSectors > 0)
            LectureAddF(szBuf, nBufLen, TN(
                "  ✗ Ожидающие сектора: %d (могут уменьшиться при повторном чтении)\r\n",
                "  ✗ Pending sectors: %d (can drop on a later read)\r\n"),
                pInfo->nPendingSectors);
        if (pInfo->nUncorrectable == 0)
            LectureAdd(szBuf, nBufLen, TN("  ✓ Неисправимые сектора: 0\r\n",
                                          "  ✓ Uncorrectable sectors: 0\r\n"));
        else if (pInfo->nUncorrectable > 0)
            LectureAddF(szBuf, nBufLen, TN("  ✗ Неисправимые сектора: %d\r\n",
                                           "  ✗ Uncorrectable sectors: %d\r\n"),
                        pInfo->nUncorrectable);
        if (pInfo->nRemapEvents == 0)
            LectureAdd(szBuf, nBufLen, TN("  ✓ События переназначения: 0\r\n",
                                          "  ✓ Reallocation events: 0\r\n"));
        else if (pInfo->nRemapEvents > 0)
            LectureAddF(szBuf, nBufLen, TN("  ✗ События переназначения: %d\r\n",
                                           "  ✗ Reallocation events: %d\r\n"),
                        pInfo->nRemapEvents);
        if (pInfo->nReallocated >= 0 && pInfo->nRemapEvents >= 0 &&
            (pInfo->nReallocated > 0 || pInfo->nRemapEvents > 0)) {
            if (pInfo->nRemapEvents > 0 && pInfo->nReallocated == 0)
                LectureAdd(szBuf, nBufLen, TN(
                    "  196 без роста 05: кандидат оказался софт-бэдом, сектор исправили без ремапа.\r\n",
                    "  196 without 05 growth: candidate was a soft-bad; sector repaired without remap.\r\n"));
            else if (pInfo->nReallocated > 0 && pInfo->nRemapEvents > 0 &&
                     pInfo->nRemapEvents < pInfo->nReallocated)
                LectureAdd(szBuf, nBufLen, TN(
                    "  196 < 05: за одно переназначение ушло несколько секторов.\r\n",
                    "  196 < 05: one remap event covered several sectors.\r\n"));
            else if (pInfo->nReallocated > 0 && pInfo->nRemapEvents > pInfo->nReallocated)
                LectureAdd(szBuf, nBufLen, TN(
                    "  196 > 05: часть попыток ремапа пришлась на софт-бэды, которые потом исправились.\r\n",
                    "  196 > 05: some remap attempts were soft-bads that later recovered.\r\n"));
        }
        if (pInfo->nRemapEvents < 0 && FindAttr(pInfo, 0xC4) &&
            RemapRawIsFlyingHours(pInfo, FindAttr(pInfo, 0xC4)->bRawValue))
            LectureAdd(szBuf, nBufLen, TN(
                "  196 совпадает с 240: прошивка записала сюда часы полёта головок, не события переназначения.\r\n",
                "  196 matches 240: firmware stored head flying hours here, not reallocation events.\r\n"));
        {
            int n184 = AttrRawOrNeg1(pInfo, 0xB8);
            if (n184 > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  ✗ End-to-End (184): %d. Ошибка через кэш диска, не кабель.\r\n",
                    "  ✗ End-to-End (184): %d. Error through the drive cache, not the cable.\r\n"),
                    n184);
        }
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 == 0)
                LectureAdd(szBuf, nBufLen, TN("  ✓ Неисправимые ошибки (187): 0\r\n",
                                              "  ✓ Uncorrectable errors (187): 0\r\n"));
            else if (n187 > 0)
                LectureAddF(szBuf, nBufLen, TN("  ✗ Неисправимые ошибки (187): %d\r\n",
                                               "  ✗ Uncorrectable errors (187): %d\r\n"),
                            n187);
        }
        if (WriteErrorScored(pInfo))
            LectureAddF(szBuf, nBufLen, TN(
                "  ✗ Частота ошибок записи (200): значение %d, худший %d\r\n",
                "  ✗ Write error rate (200): value %d, worst %d\r\n"),
                pInfo->nWriteErrorValue,
                pInfo->nWriteErrorWorst >= 0 ? pInfo->nWriteErrorWorst : pInfo->nWriteErrorValue);
        if (pInfo->nEndurancePercent >= 0)
            LectureAddF(szBuf, nBufLen, TN(
                "  %s Заявленный остаток ресурса: %d%%   (это износ NAND, не здоровье)\r\n",
                "  %s Reported life remaining: %d%%   (NAND wear, not health)\r\n"),
                pInfo->nEndurancePercent <= 20 ? "\xE2\x9A\xA0" : "\xE2\x9C\x93",
                pInfo->nEndurancePercent);

        if (nUnknown > 0) {
            LectureAdd(szBuf, nBufLen, TN("\r\nНаблюдение:\r\n", "\r\nNote:\r\n"));
            LectureAdd(szBuf, nBufLen, TN(
                "  ⚠ Есть vendor-specific SMART-счётчики с нестандартными RAW. "
                "Их нельзя надёжно трактовать без профиля контроллера. "
                "Unknown ≠ Bad: неназванный атрибут не штрафует здоровье.\r\n",
                "  ⚠ Vendor-specific SMART counters use a non-standard RAW. "
                "They cannot be read reliably without a controller profile. "
                "Unknown is not Bad: an unnamed attribute does not lower health.\r\n"));
        }

        LectureAdd(szBuf, nBufLen, TN("\r\nКонтекст (не штраф):\r\n", "\r\nContext (not a penalty):\r\n"));
        if (pInfo->nCrcErrors > 0)
            LectureAddF(szBuf, nBufLen, TN(
                "  CRC UltraDMA: %d. Кабель/мост/порт, не здоровье носителя.\r\n",
                "  UltraDMA CRC: %d. Cable, bridge, or port, not media health.\r\n"),
                pInfo->nCrcErrors);
        else if (pInfo->nCrcErrors == 0)
            LectureAdd(szBuf, nBufLen, TN("  CRC UltraDMA: 0. Контекст, не штраф.\r\n",
                                          "  CRC UltraDMA: 0. Context, not a penalty.\r\n"));
        {
            int n188 = AttrRawOrNeg1(pInfo, 0xBC);
            int n189 = AttrRawOrNeg1(pInfo, 0xBD);
            if (n188 > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  Таймауты команд (188): %d. Питание или кабель, не носитель.\r\n",
                    "  Command timeouts (188): %d. Power or cable, not media.\r\n"),
                    n188);
            if (n189 > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  Записи на большой высоте (189): %d. Вибрация/удар, не штраф к носителю.\r\n",
                    "  High-fly writes (189): %d. Vibration/shock, not a media penalty.\r\n"),
                    n189);
            {
                int n10 = AttrRawOrNeg1(pInfo, 0x0A);
                if (n10 > 2)
                    LectureAddF(szBuf, nBufLen, TN(
                        "  Повторы раскрутки (10): %d. Сначала питание/кабель, не блины.\r\n",
                        "  Spin retries (10): %d. Check PSU/cable first, not the platters.\r\n"),
                        n10);
            }
        }
        LectureAddF(szBuf, nBufLen, TN(
            "  Температура: %s. Время на высокой температуре: нет данных.\r\n",
            "  Temperature: %s. Time at high temperature: no data.\r\n"),
            szTempBand);
        LectureAddF(szBuf, nBufLen, TN(
            "  Наработка: %s. Power-on hours ≠ признак отказа.\r\n",
            "  Power-on: %s. Power-on hours are not a failure sign.\r\n"), szPoh);
        LectureAddF(szBuf, nBufLen, TN(
            "  Циклы включения: %lu. Контекст, не штраф.\r\n",
            "  Power cycles: %lu. Context, not a penalty.\r\n"),
            (unsigned long)pInfo->dwPowerCycleCount);
        if (nC0 >= 0 && DriveTreatsC0AsPowerLoss(pInfo)) {
            LectureAddF(szBuf, nBufLen, TN(
                "  Аварийные отключения питания (192): %d. Журнал питания, не штраф. "
                "Находка только при росте счётчика, не по абсолютному числу.\r\n",
                "  Unsafe power-offs (192): %d. A power log, not a penalty. "
                "It matters when the counter rises, not as an absolute number.\r\n"),
                nC0);
        } else if (nC0 >= 0) {
            LectureAddF(szBuf, nBufLen, TN(
                "  Парковки при выключении (192): %d. Контекст, не штраф.\r\n",
                "  Power-off retracts (192): %d. Context, not a penalty.\r\n"),
                nC0);
        }
        if (pInfo->nGSenseEvents >= 0 || pInfo->eType == DRIVE_TYPE_HDD) {
            LectureAdd(szBuf, nBufLen, TN(
                "\r\nМеханика (отдельный канал, не штраф к состоянию):\r\n",
                "\r\nMechanics (separate channel, not a health penalty):\r\n"));
            if (pInfo->nGSenseEvents >= 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  G-Sense: %d событий. Зарегистрированные события, связанные с ударом/вибрацией (не обязательно столько физических ударов).\r\n",
                    "  G-Sense: %d events. Recorded shock or vibration (not necessarily that many physical shocks).\r\n"),
                    pInfo->nGSenseEvents);
            if (pInfo->nGSensePerKh >= 0 && pInfo->dwPowerOnHours > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  G-Sense / наработка: %d событий ≈ %d на 1000 ч (контекст, не оценка).\r\n",
                    "  G-Sense / power-on: %d events ≈ %d per 1000 h (context, not a score).\r\n"),
                    pInfo->nGSenseEvents, pInfo->nGSensePerKh);
            if (pInfo->nMechRisk >= 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  Mechanical risk: %d/100 (рост и корреляция; абсолютный BF не входит).\r\n",
                    "  Mechanical risk: %d/100 (rise and correlation; the absolute BF count is not included).\r\n"),
                    pInfo->nMechRisk);
            if (pInfo->nEmergencyRetract >= 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  Парковки при выключении (192): %d.\r\n",
                    "  Power-off retracts (192): %d.\r\n"), pInfo->nEmergencyRetract);
            if (pInfo->nLoadUnload >= 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  Циклы парковки головок: %d.\r\n",
                    "  Load/unload cycles: %d.\r\n"), pInfo->nLoadUnload);
            LectureAddF(szBuf, nBufLen,
                TN("  Механическое состояние: %s.\r\n",
                   "  Mechanical condition: %s.\r\n"),
                pInfo->eMechanics == HEALTH_STATUS_GOOD ? Tr(STR_AXIS_OK) :
                    GetHealthStatusName(pInfo->eMechanics));
            if (pInfo->nGSenseEvents > 0 &&
                pInfo->nReallocated <= 0 && pInfo->nPendingSectors <= 0 &&
                pInfo->nUncorrectable <= 0 &&
                pInfo->nRemapEvents <= 0 &&
                AttrRawOrNeg1(pInfo, 0xBB) <= 0) {
                if (pInfo->nRemapEvents < 0)
                    LectureAdd(szBuf, nBufLen, TN(
                        "  G-Sense зарегистрировал события ударов/вибрации, однако SMART не показывает "
                        "связанных с ними признаков повреждения поверхности (5/197/198 = 0). "
                        "Атрибут 196 здесь не счётчик событий переназначения.\r\n",
                        "  G-Sense recorded shock or vibration, but SMART shows no related "
                        "surface damage (5/197/198 = 0). Attribute 196 is not a reallocation count here.\r\n"));
                else
                    LectureAdd(szBuf, nBufLen, TN(
                        "  G-Sense зарегистрировал события ударов/вибрации, однако SMART не показывает "
                        "связанных с ними признаков повреждения поверхности (5/196/197/198 = 0). "
                        "\r\n",
                        "  G-Sense recorded shock or vibration, but SMART shows no related "
                        "surface damage (5/196/197/198 = 0).\r\n"));
            }
            else if (pInfo->nGSenseEvents > 0 &&
                     pInfo->nReallocated <= 0 && pInfo->nPendingSectors <= 0 &&
                     pInfo->nUncorrectable <= 0 &&
                     AttrRawOrNeg1(pInfo, 0xBB) > 0)
                LectureAddF(szBuf, nBufLen, TN(
                    "  G-Sense зарегистрировал удары/вибрацию. Счётчики 5/196/197/198 равны 0. "
                    "Атрибут 187 = %d — отдельный счётчик, не доказательство, что удары повредили поверхность.\r\n",
                    "  G-Sense recorded shock or vibration. Counters 5/196/197/198 are 0. "
                    "Attribute 187 = %d is a separate count, not proof the shocks damaged the surface.\r\n"),
                    AttrRawOrNeg1(pInfo, 0xBB));
            else if (pInfo->nGSenseEvents > 0)
                LectureAdd(szBuf, nBufLen, TN(
                    "  G-Sense совпадает с проблемными секторами — это связанная улика, не отдельный штраф.\r\n",
                    "  G-Sense lines up with bad sectors — related evidence, not a separate penalty.\r\n"));
        }
        if (nC3 >= 0) {
            if (DriveIsHdd(pInfo) && UseSeagateRateRaw(pInfo)) {
                const SMART_ATTRIBUTE* a195 = FindAttr(pInfo, 0xC3);
                unsigned nErr = a195 ? SeagateRateErrs(a195->bRawValue) : 0;
                DWORD nOps = a195 ? SeagateRateOps(a195->bRawValue) : 0;
                LectureAddF(szBuf, nBufLen, TN(
                    "  ID 195 (Hardware ECC Recovered): %u ошибок коррекции / %lu секторов. "
                    "На HDD это штатная коррекция ошибок чтения пластины (как ID 1), "
                    "не NAND и не неисправимые сектора. RAW без тренда не оценивается.\r\n",
                    "  ID 195 (Hardware ECC Recovered): %u corrected errors / %lu sectors. "
                    "On an HDD this is normal platter-read correction (like ID 1), "
                    "not NAND and not uncorrectable sectors. RAW without a trend is not scored.\r\n"),
                    nErr, (unsigned long)nOps);
            } else if (DriveIsHdd(pInfo)) {
                LectureAddF(szBuf, nBufLen, TN(
                    "  ID 195 (Hardware ECC Recovered): RAW %d. "
                    "На HDD это штатная коррекция чтения пластины, не NAND. "
                    "Без профиля вендора абсолютный RAW не оценивается.\r\n",
                    "  ID 195 (Hardware ECC Recovered): RAW %d. "
                    "On an HDD this is normal platter-read correction, not NAND. "
                    "Without a vendor profile the absolute RAW is not scored.\r\n"),
                    nC3);
            } else {
                ATTR_DECODE dec195;
                GetAttrDecode(0xC3, pInfo, &dec195);
                if (dec195.eState == ATTR_DECODE_UNKNOWN)
                    LectureAddF(szBuf, nBufLen, TN(
                        "  ID 195 RAW: %d. На SSD это vendor-specific счётчик, "
                        "не Hardware ECC Recovered HDD и не «механизм NAND» по умолчанию. "
                        "Без профиля не оценивается.\r\n",
                        "  ID 195 RAW: %d. On an SSD this is a vendor-specific counter, "
                        "not HDD Hardware ECC Recovered and not a NAND mechanism by default. "
                        "It is not scored without a profile.\r\n"),
                        nC3);
            }
        }

        LectureAdd(szBuf, nBufLen, TN("\r\nМатрица улик:\r\n", "\r\nEvidence matrix:\r\n"));
        LectureMatrixRow(szBuf, nBufLen,
                         TN("Улика", "Evidence"),
                         TN("Значение", "Value"),
                         TN("Влияние", "Effect"));
        LectureMatrixRow(szBuf, nBufLen, "SMART overall",
                         pInfo->bPredictFailure ? "FAIL" : "PASS",
                         pInfo->bPredictFailure ? "−" : "+");
        LectureMatrixRow(szBuf, nBufLen, "Prefail now",
                         pInfo->bPrefailNow ? "FAIL" : "PASS",
                         pInfo->bPrefailNow ? "−" : "+");
        if (pInfo->nReallocated >= 0)
            LectureMatrixRow(szBuf, nBufLen, TN("Переназначенные", "Reallocated"), szR,
                             pInfo->nReallocated == 0 ? "+" : "−");
        if (pInfo->nPendingSectors >= 0)
            LectureMatrixRow(szBuf, nBufLen, TN("Ожидающие", "Pending"), szPend,
                             pInfo->nPendingSectors == 0 ? "+" : "−");
        if (pInfo->nUncorrectable >= 0)
            LectureMatrixRow(szBuf, nBufLen, TN("Неисправимые", "Uncorrectable"), szU,
                             pInfo->nUncorrectable == 0 ? "+" : "−");
        if (pInfo->nRemapEvents >= 0)
            LectureMatrixRow(szBuf, nBufLen, TN("События переназначения", "Reallocation events"), szRemap,
                             pInfo->nRemapEvents == 0 ? "+" : "−");
        if (pInfo->nCrcErrors >= 0)
            LectureMatrixRow(szBuf, nBufLen, "CRC", szCrc,
                             pInfo->nCrcErrors > 0 ? TN("интерфейс", "interface") : "0");
        {
            int n188 = AttrRawOrNeg1(pInfo, 0xBC);
            if (n188 > 0) {
                char sz188[24];
                safe_snprintf(sz188, "%d", n188);
                LectureMatrixRow(szBuf, nBufLen, TN("Таймауты (188)", "Timeouts (188)"), sz188,
                                 n188 >= 10 ? TN("интерфейс", "interface") : "0");
            }
        }
        if (DriveIsHdd(pInfo)) {
            int v193 = AttrValueOrNeg1(pInfo, 0xC1);
            BOOL notable = (v193 >= 0 && v193 <= 15) || pInfo->nLoadUnload >= 300000;
            if (notable) {
                char sz193[48];
                if (v193 >= 0 && pInfo->nLoadUnload >= 0)
                    safe_snprintf(sz193, TN("знач. %d, %d цикл.", "value %d, %d cycles"),
                                  v193, pInfo->nLoadUnload);
                else if (v193 >= 0)
                    safe_snprintf(sz193, TN("знач. %d", "value %d"), v193);
                else
                    safe_snprintf(sz193, "%d", pInfo->nLoadUnload);
                /* 225 (0xE1) is the load-cycle counter when 193 is absent.
                 * ST1000LM024 stores 340531 cycles there, value 67. */
                LectureMatrixRow(szBuf, nBufLen,
                                 (HasSmartAttr(pInfo, 0xE1) && !HasSmartAttr(pInfo, 0xC1))
                                     ? TN("Парковка (225)", "Park (225)")
                                     : TN("Парковка (193)", "Park (193)"),
                                 sz193,
                                 (v193 >= 0 && v193 <= 5) ? TN("механика", "mechanics") : "0");
            }
        }
        {
            int n187 = AttrRawOrNeg1(pInfo, 0xBB);
            if (n187 >= 0) {
                char sz187[24];
                safe_snprintf(sz187, "%d", n187);
                LectureMatrixRow(szBuf, nBufLen, TN("Неисправимые (187)", "Uncorrectable (187)"), sz187,
                                 n187 == 0 ? "+" : "−");
            }
        }
        if (WriteErrorScored(pInfo)) {
            char szWe[32];
            safe_snprintf(szWe, TN("знач. %d", "value %d"), pInfo->nWriteErrorValue);
            LectureMatrixRow(szBuf, nBufLen, TN("Ошибки записи (200)", "Write errors (200)"), szWe,
                             "−");
        }
        if (pInfo->nEndurancePercent >= 0) {
            safe_snprintf(szLife, "%d%%", pInfo->nEndurancePercent);
            LectureMatrixRow(szBuf, nBufLen, TN("Остаток ресурса", "Life remaining"), szLife,
                             pInfo->nEndurancePercent <= 20 ? "0" : "+");
        }
        {
            char szT[24];
            if (pInfo->nTemperatureC > 0)
                safe_snprintf(szT, "%d °C", pInfo->nTemperatureC);
            else
                safe_snprintf(szT, "%s", Tr(STR_POH_NONE));
            LectureMatrixRow(szBuf, nBufLen, TN("Температура", "Temperature"), szT, "0");
        }
        {
            char szH[64];
            FormatPowerOnHours(pInfo->dwPowerOnHours, szH, (int)sizeof(szH));
            LectureMatrixRow(szBuf, nBufLen, TN("Наработка", "Power-on"), szH, "0");
        }
        if (nC3 >= 0) {
            ATTR_DECODE dec195;
            GetAttrDecode(0xC3, pInfo, &dec195);
            if (DriveIsHdd(pInfo) || dec195.eState == ATTR_DECODE_UNKNOWN) {
                char szEcc[24];
                safe_snprintf(szEcc, "%d", nC3);
                LectureMatrixRow(szBuf, nBufLen, "ECC (195)", szEcc,
                                 DriveIsHdd(pInfo) ? "0" : "UNKNOWN");
            }
        }
        if (nB0 >= 0) {
            ATTR_DECODE decB;
            GetAttrDecode(0xB0, pInfo, &decB);
            if (decB.eState == ATTR_DECODE_UNKNOWN) {
                char szB[24];
                safe_snprintf(szB, "%d", nB0);
                LectureMatrixRow(szBuf, nBufLen, "B0", szB, "UNKNOWN");
            }
        }
        if (nB1 >= 0) {
            ATTR_DECODE decB;
            GetAttrDecode(0xB1, pInfo, &decB);
            if (decB.eState == ATTR_DECODE_UNKNOWN) {
                char szB[24];
                safe_snprintf(szB, "%d", nB1);
                LectureMatrixRow(szBuf, nBufLen, "B1", szB, "UNKNOWN");
            }
        }
        if (nF5 >= 0) {
            ATTR_DECODE decF;
            GetAttrDecode(0xF5, pInfo, &decF);
            if (decF.eState == ATTR_DECODE_UNKNOWN) {
                char szB[24];
                safe_snprintf(szB, "%d", nF5);
                LectureMatrixRow(szBuf, nBufLen, "F5", szB, "UNKNOWN");
            }
        }
        {
            char szCtl[64];
            const char* ctl = GetControllerName(pInfo->eController);
            if (pInfo->eController == CONTROLLER_PHISON && nUnknown > 0)
                safe_snprintf(szCtl, "%s",
                    TN("Phison (RAW частично)", "Phison (RAW partial)"));
            else
                safe_snprintf(szCtl, "%s", ctl);
            LectureMatrixRow(szBuf, nBufLen, TN("Контроллер", "Controller"), szCtl,
                             pInfo->eController == CONTROLLER_UNKNOWN
                                 ? TN("−уверенность", "−confidence") : "0");
            LectureProfileRow(szBuf, nBufLen, pInfo);
        }

        LectureAdd(szBuf, nBufLen, TN("\r\nИтог:\r\n", "\r\nVerdict:\r\n"));
        if (pInfo->eHealthStatus == HEALTH_STATUS_GOOD)
            LectureAdd(szBuf, nBufLen, TN("  ХОРОШО, потому что:\r\n", "  GOOD because:\r\n"));
        else if (pInfo->eHealthStatus == HEALTH_STATUS_OBSERVE)
            LectureAdd(szBuf, nBufLen, TN("  РИСК, потому что:\r\n", "  WATCH because:\r\n"));
        else if (pInfo->eHealthStatus == HEALTH_STATUS_CAUTION)
            LectureAdd(szBuf, nBufLen, TN("  ВНИМАНИЕ, потому что:\r\n", "  NEEDS ATTENTION because:\r\n"));
        else
            LectureAdd(szBuf, nBufLen, TN("  ПЛОХО, потому что:\r\n", "  BAD because:\r\n"));
        LectureAddF(szBuf, nBufLen, TN("    %d сильных положительных признаков\r\n",
                                      "    %d strong positive signs\r\n"), nPos);
        LectureAddF(szBuf, nBufLen, TN("    %d критических отказов\r\n",
                                      "    %d critical failures\r\n"), nCritFail);
        LectureAddF(szBuf, nBufLen, TN("    %d признаков деградации носителя\r\n",
                                      "    %d signs of media degradation\r\n"), nMediaDeg);
        LectureAddF(szBuf, nBufLen, TN("    %d vendor-specific счётчиков не расшифрованы\r\n",
                                      "    %d vendor-specific counters not decoded\r\n"),
                    nUnresolved);
        if (pInfo->nCrcErrors > 0)
            LectureAddF(szBuf, nBufLen, TN("    интерфейс: CRC %d\r\n",
                                          "    interface: CRC %d\r\n"),
                        pInfo->nCrcErrors);
        {
            int n188why = AttrRawOrNeg1(pInfo, 0xBC);
            if (n188why >= 10)
                LectureAddF(szBuf, nBufLen, TN(
                    "    интерфейс: таймауты команд %d\r\n",
                    "    interface: command timeouts %d\r\n"),
                    n188why);
        }

        LectureAdd(szBuf, nBufLen, TN(
            "Мы не переводим SMART Value/Worst/Threshold в процент здоровья: "
            "нормализованная шкала 100 против 200 — это не «в два раза здоровее». "
            "Сырые счётчики — факты, а не шкала от 0 до 100.\r\n",
            "Value/Worst/Threshold are not turned into a health percent: "
            "a normalized scale of 100 versus 200 is not \"twice as healthy\". "
            "Raw counters are facts, not a 0–100 score.\r\n"));
        if (pInfo->nEndurancePercent >= 0)
            LectureAdd(szBuf, nBufLen, TN(
                "Заявленный остаток ресурса — цифра контроллера про износ NAND, "
                "это не здоровье диска и не означает, что диск новый.\r\n",
                "Reported life remaining is the controller's NAND-wear figure. "
                "It is not disk health and does not mean the disk is new.\r\n"));
        else
            LectureAdd(szBuf, nBufLen, TN(
                "Для этого диска заявленный остаток ресурса (износ) не задан — "
                "процент не выдумывается.\r\n",
                "This drive does not report remaining endurance — "
                "no percent is invented.\r\n"));
        LectureAddUnknownIdList(szBuf, nBufLen, pInfo);
    }
}

/* Host or NAND writes with a named unit. A wear percent stays out. */
static int WriteVolumeGiB(const DRIVE_INFO* p, BYTE id,
                          unsigned __int64 nRaw, unsigned __int64* nGiB)
{
    int sem = PROF_SEM_NONE, enc = PROF_ENC_NONE;
    if (!nGiB) return 0;
    *nGiB = 0;
    if (!p || !ProfileLookup(p, id, &sem, &enc) || !ProfileWriteVolume(sem, enc))
        return 0;
    if (enc == PROF_ENC_BLOCK32)
        *nGiB = Smi225xHostGiB(nRaw);
    else if (enc == PROF_ENC_SCALE &&
             (p->eType == DRIVE_TYPE_SSD_SATA || p->eType == DRIVE_TYPE_M2_SATA))
        *nGiB = ScaleAtaHostGiB(p, nRaw, FALSE);
    else if (enc == PROF_ENC_LBA) {
        if (nRaw < 2048ULL) return 0;
        *nGiB = nRaw / (1024ULL * 1024ULL * 2ULL);
    } else if (enc == PROF_ENC_GIB) {
        if (nRaw == 0 || nRaw > 4000000ULL) return 0;
        *nGiB = nRaw;
    } else
        return 0;
    return *nGiB > 0;
}

/* One endurance reading. figure is 0..100.
 * agrees is 1 only for a RAW percent that equals its normalized Value. */
struct SsdLife {
    int id;
    int figure;
    int value;
    int agrees;
    int pref;
};

/* Higher preference wins. The same preference keeps the lower id. */
static int LifeBeats(const struct SsdLife* a, const struct SsdLife* b)
{
    if (a->pref != b->pref) return a->pref > b->pref;
    return a->id < b->id;
}

/* PERCENT: the low 16 bits of RAW, when they are 0..100.
 * RAW 0 with Value still at 90 or above is the dummy 100/100/50 scale,
 * not a report of 0% life. A RAW outside 0..100 is not a percent:
 * there is no fallback to Value. That fallback was a per-id rule. */
static int RawPercent(const SMART_ATTRIBUTE* a, int* agrees)
{
    int raw16 = (int)GetRawValue16Lo(a->bRawValue);
    int val = (int)a->bAttrValue;
    if (agrees) *agrees = 0;
    if (raw16 < 0 || raw16 > 100) return -1;
    if (raw16 == 0 && val >= 90) return -1;
    if (agrees) *agrees = (val == raw16) ? 1 : 0;
    return raw16;
}

/* AVG, MAX, and MIN store the raw count, including 0.
 * MAXAVG splits the two halves and keeps each only inside 1..99999.
 * A bare COUNTER is an erase count with no role, so it is not stored. */
static void StoreEraseRole(DRIVE_INFO* p, int enc, DWORD raw)
{
    if (enc == PROF_ENC_MAXAVG) {
        unsigned avg = 0, mx = 0;
        PhisonMaxAvgHalves((unsigned)raw, &mx, &avg);
        if (p->nSSDAvgEraseCount < 0 && avg > 0 && avg < 100000u)
            p->nSSDAvgEraseCount = (int)avg;
        if (p->nSSDMaxEraseCount < 0 && mx > 0 && mx < 100000u)
            p->nSSDMaxEraseCount = (int)mx;
        return;
    }
    if (raw > 0x7FFFFFFFu) return;
    if (enc == PROF_ENC_AVG && p->nSSDAvgEraseCount < 0)
        p->nSSDAvgEraseCount = (int)raw;
    else if (enc == PROF_ENC_MAX && p->nSSDMaxEraseCount < 0)
        p->nSSDMaxEraseCount = (int)raw;
    else if (enc == PROF_ENC_MIN && p->nSSDMinEraseCount < 0)
        p->nSSDMinEraseCount = (int)raw;
}

/* Wear-leveling counter. A wear percent is not this field and not life. */
static void StoreWearCount(DRIVE_INFO* p, DWORD raw)
{
    if (p->nSSDWearLevelingCount >= 0) return;
    if (raw > 0 && raw < 100000u)
        p->nSSDWearLevelingCount = (int)raw;
}

/* ============================================================
 * SSD indicators from profile semantics.
 * Every attribute is visited. The id is not a case label.
 * PERCENT reads RAW. VALUE reads the normalized byte.
 * Erase roles are AVG, MAX, MIN, and MAXAVG.
 * Writes run after erase counts so a SCALE volume can see them.
 * ============================================================ */
void ExtractSSDIndicators(DRIVE_INFO* pInfo)
{
    struct SsdLife life[30];
    int nLife = 0;
    int best = -1;
    int i;

    if (!pInfo) return;
    IdentifyDriveParts(pInfo);
    pInfo->nSSDLifeLeft     = -1;
    pInfo->nSSDTotalWritesGB = -1;
    pInfo->nSSDAvgEraseCount = -1;
    pInfo->nSSDMaxEraseCount = -1;
    pInfo->nSSDMinEraseCount = -1;
    pInfo->nSSDWearLevelingCount = -1;

    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* a = &pInfo->attrData.stAttributes[i];
        int sem = PROF_SEM_NONE, enc = PROF_ENC_NONE;
        int figure = -1;
        int agrees = 0;
        if (a->bAttrID == 0) continue;
        if (!ProfileLookup(pInfo, a->bAttrID, &sem, &enc)) continue;

        if (sem == PROF_SEM_ENDURANCE &&
            (enc == PROF_ENC_PERCENT || enc == PROF_ENC_VALUE)) {
            if (enc == PROF_ENC_VALUE) {
                if (a->bAttrValue <= 100)
                    figure = (int)a->bAttrValue;
            } else {
                figure = RawPercent(a, &agrees);
            }
            if (figure >= 0 && nLife < 30) {
                life[nLife].id = (int)a->bAttrID;
                life[nLife].figure = figure;
                life[nLife].value = (int)a->bAttrValue;
                life[nLife].agrees = agrees;
                life[nLife].pref = ProfileAttrPref(pInfo, a->bAttrID);
                nLife++;
            }
        } else if (sem == PROF_SEM_ERASE) {
            StoreEraseRole(pInfo, enc, GetRawValue(a->bRawValue));
        } else if (sem == PROF_SEM_WEAR && enc == PROF_ENC_COUNTER) {
            StoreWearCount(pInfo, GetRawValue(a->bRawValue));
        }
    }

    for (i = 0; i < nLife; i++) {
        if (best < 0 || LifeBeats(&life[i], &life[best]))
            best = i;
    }
    /* A RAW percent that matches its Value wins over another reading
     * whose Value is still at 90 or above and whose figure differs. */
    if (best >= 0 && (pInfo->nProfileFlags & PROF_F_PREFER_AGREEING) &&
        life[best].value >= 90) {
        int alt = -1;
        for (i = 0; i < nLife; i++) {
            if (i == best || !life[i].agrees) continue;
            if (life[i].figure == life[best].figure) continue;
            if (alt < 0 || LifeBeats(&life[i], &life[alt]))
                alt = i;
        }
        if (alt >= 0) best = alt;
    }
    if (best >= 0)
        pInfo->nSSDLifeLeft = life[best].figure;

    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* a = &pInfo->attrData.stAttributes[i];
        unsigned __int64 nGiB = 0;
        if (a->bAttrID == 0) continue;
        if (pInfo->nSSDTotalWritesGB >= 0) break;
        if (!WriteVolumeGiB(pInfo, a->bAttrID,
                            GetRawValue48(a->bRawValue), &nGiB))
            continue;
        if (nGiB > 4000000ULL) nGiB = 4000000ULL;
        pInfo->nSSDTotalWritesGB = (int)nGiB;
    }
}

