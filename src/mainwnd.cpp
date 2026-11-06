/* DriveMonitor - main window. GPL-2.0-or-later. See LICENSE. */

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <dbt.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <wchar.h>

#define GDIPVER 0x0110
#include <objbase.h>
#include <shlobj.h>
#include <gdiplus.h>
using namespace Gdiplus;

#include "mainwnd.h"
#include "smart.h"
#include "utf8ui.h"
#include "safestr.h"
#include "lang.h"
#include "latency_probe.h"
#include "dram_verify.h"
#include "dst_engine.h"

#define DONATE_URL "https://boosty.to/chuikoff"

/* ID, parameter, value, worst, threshold, RAW hex, decode, status. */
#define ATTR_NCOLS       8
#define ATTR_COL_RAW     5
#define ATTR_COL_DECODE  6
#define ATTR_COL_STATUS  7

static unsigned __int64 NVMeRead128Lo(const BYTE* p)
{
    unsigned __int64 lo = 0;
    int i;
    for (i = 7; i >= 0; i--) lo = (lo << 8) | (unsigned __int64)p[i];
    return lo;
}

static WORD ReadLE16(const BYTE* p)
{
    return (WORD)((WORD)p[0] | ((WORD)p[1] << 8));
}

/* NVMe Health Log Data Units: 1 unit = 1000 * 512 = 512000 bytes (NVMe spec).
 * CrystalDiskInfo TBW = units * 512000 / 1e12. */
static void FormatNvmeHostBytes(unsigned __int64 units, char* szBuf, int nBufLen)
{
    double bytes = (double)units * 512000.0;
    double tb = bytes / 1e12;
    if (tb >= 1.0)
        safe_snprintf_n(szBuf, nBufLen, TN("%.1f ТБ", "%.1f TB"), tb);
    else
        safe_snprintf_n(szBuf, nBufLen, TN("%.1f ГБ", "%.1f GB"), bytes / 1e9);
}

/* USB enclosure/chip name from SCSI INQUIRY; type name / VID:PID fallback.
 * Names come from DetectUsbBridgeType comments — no extra VID table. */

/* ATA SSD: nSSDTotalWritesGB is a profile host/NAND volume in GiB, or -1.
 * Standard storage metric (like NVMe and CrystalDiskInfo) uses decimal TB/GB. */
static void FormatAtaWrites(int nGiB, char* szBuf, int nBufLen)
{
    if (nGiB < 0) { szBuf[0] = '\0'; return; }
    double bytes = (double)nGiB * 1073741824.0;
    double tb = bytes / 1e12;
    if (tb >= 1.0)
        safe_snprintf_n(szBuf, nBufLen, TN("%.1f ТБ", "%.1f TB"), tb);
    else
        safe_snprintf_n(szBuf, nBufLen, TN("%.1f ГБ", "%.1f GB"), bytes / 1e9);
}

/* Unified SMART headline for every drive type:
 *   Запас …  ·  Износ …  ·  Записано …
 * Missing values are an em dash; fields are never omitted. */
static void FormatSmartHeadline(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    char szSpare[32], szWear[32], szWritten[32];
    if (!pInfo || nBufLen <= 0) return;
    szBuf[0] = '\0';
    lstrcpynA(szSpare, TN("н/д", "n/a"), sizeof(szSpare));
    lstrcpynA(szWear, TN("н/д", "n/a"), sizeof(szWear));
    lstrcpynA(szWritten, TN("н/д", "n/a"), sizeof(szWritten));

    if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        safe_snprintf(szSpare, "%d%%",
                  (int)pInfo->nvmeHealth.AvailableSpare);
        safe_snprintf(szWear, "%d%%",
                  (int)pInfo->nvmeHealth.PercentageUsed);
        FormatNvmeHostBytes(NVMeRead128Lo(pInfo->nvmeHealth.DataUnitsWritten),
                            szWritten, sizeof(szWritten));
    } else if (pInfo->bSMART_Supported) {
        /* ATA SSD: no spare-like field is extracted, so Запас stays н/д. */
        if (pInfo->nSSDLifeLeft >= 0 && pInfo->nSSDLifeLeft <= 100)
            safe_snprintf(szWear, "%d%%", 100 - pInfo->nSSDLifeLeft);
        if (pInfo->nSSDTotalWritesGB >= 0)
            FormatAtaWrites(pInfo->nSSDTotalWritesGB, szWritten, sizeof(szWritten));
    }

    safe_snprintf_n(szBuf, nBufLen,
        TN("Запас %s  ·  Износ %s  ·  Записано %s",
           "Spare %s  ·  Wear %s  ·  Written %s"),
        szSpare, szWear, szWritten);
}

static void FormatUsbAdapterName(const DRIVE_INFO* p, char* szBuf, int nBufLen)
{
    const char* chip = NULL;
    if (!p || nBufLen <= 0) return;
    szBuf[0] = '\0';

    /* SCSI INQUIRY on SAT often returns the *disk* (Kingston A400), not the
     * USB chip. Skip it when it matches the drive model. Prefer USB VID. */
    {
        BOOL inqIsDisk = FALSE;
        if (p->szBridgeProduct[0] && p->szModel[0] &&
            (strstr(p->szModel, p->szBridgeProduct) ||
             strstr(p->szBridgeProduct, p->szModel)))
            inqIsDisk = TRUE;
        if (!inqIsDisk && p->szBridgeProduct[0]) {
            const char* pr = p->szBridgeProduct;
            if (strstr(pr, "RTL") || strstr(pr, "ASM") || strstr(pr, "JMS") ||
                strstr(pr, "Realtek") || strstr(pr, "ASMedia") ||
                strstr(pr, "JMicron")) {
                if (p->szBridgeVendor[0])
                    safe_snprintf_n(szBuf, nBufLen, "%s %s", p->szBridgeVendor, pr);
                else
                    safe_snprintf_n(szBuf, nBufLen, "%s", pr);
                return;
            }
        }
    }

    if (p->wUsbVid == 0x0BDA) {
        if (p->wUsbPid == 0x9210 || p->wUsbPid == 0x9211)
            safe_snprintf_n(szBuf, nBufLen, "Realtek RTL9210 (%04X:%04X)",
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        else if (p->wUsbPid)
            safe_snprintf_n(szBuf, nBufLen, "Realtek (%04X:%04X)",
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        else
            safe_snprintf_n(szBuf, nBufLen, "Realtek");
        return;
    }
    if (p->wUsbVid == 0x152D) {
        safe_snprintf_n(szBuf, nBufLen, "JMicron (%04X:%04X)",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        return;
    }
    if (p->wUsbVid == 0x174C) {
        safe_snprintf_n(szBuf, nBufLen, "ASMedia (%04X:%04X)",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        return;
    }

    switch (p->eUsbBridgeType) {
    case USB_BRIDGE_NVME_JMICRON: chip = "JMicron JMS583"; break;
    case USB_BRIDGE_NVME_ASMEDIA: chip = "ASMedia ASM2362"; break;
    case USB_BRIDGE_NVME_REALTEK: chip = "Realtek RTL9210"; break;
    case USB_BRIDGE_NVME_VLI:     chip = "VLI VL716"; break;
    case USB_BRIDGE_NVME_FMA:     chip = "FMA NL6221"; break;
    case USB_BRIDGE_ASM1352R:     chip = "ASMedia ASM1352R"; break;
    case USB_BRIDGE_JMICRON:      chip = "JMicron"; break;
    case USB_BRIDGE_SUNPLUS:      chip = "Sunplus"; break;
    case USB_BRIDGE_CYPRESS:      chip = "Cypress"; break;
    case USB_BRIDGE_IO_DATA:      chip = "I-O Data"; break;
    case USB_BRIDGE_LOGITEC:      chip = "Logitec"; break;
    case USB_BRIDGE_PROLIFIC:     chip = "Prolific"; break;
    case USB_BRIDGE_SAT:          chip = "SAT"; break;
    default: break;
    }

    if (chip && p->wUsbVid && p->wUsbPid) {
        /* USB_BRIDGE_SAT is also the unknown-VID default. Do not call a
         * stick SAT when IDENTIFY and SMART both failed. */
        if (p->eUsbBridgeType == USB_BRIDGE_SAT &&
            !p->bCapAtaIdentify && !p->bCapSmartData)
            safe_snprintf_n(szBuf, nBufLen, "VID:%04X PID:%04X",
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
        else
            safe_snprintf_n(szBuf, nBufLen, "%s (%04X:%04X)", chip,
                      (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
    } else if (chip)
        safe_snprintf_n(szBuf, nBufLen, "%s", chip);
    else if (p->szBridgeVendor[0])
        safe_snprintf_n(szBuf, nBufLen, "%s", p->szBridgeVendor);
    else if (p->wUsbVid && p->wUsbPid)
        safe_snprintf_n(szBuf, nBufLen, "VID:%04X PID:%04X",
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
    else
        safe_snprintf_n(szBuf, nBufLen, "—");
}

DRIVE_INFO  g_Drives[MAX_DRIVES];
int         g_nDriveCount    = 0;
int         g_nSelectedDrive = 0;
HINSTANCE   g_hInst          = NULL;
HWND        g_hMainWnd       = NULL;
HWND        g_hHealthBar     = NULL;
HWND        g_hDriveBtn[MAX_DRIVES];

static HDEVNOTIFY      g_hDevNotify    = NULL;
static DRIVE_INFO      g_PrevDrives[MAX_DRIVES];
static int             g_nPrevCount    = 0;
static int             g_nTrayIcons    = 0;
static int             g_bMonitorOn    = 0;
static int             g_bMonitorAlert = 0;
static int             g_bMonitorQuiet = 0;
static char            g_szMonitor[384];
#define HOTPLUG_DELAY_MS      1200
#define MONITOR_INTERVAL_MS  60000

/* Note: the previous WinRAR-style nag timer state variables
   (g_nagSecondsLeft, g_bNagPending) have been removed because the
   program is 100% free and open source
   reminder to show anymore. */

HBRUSH  g_hbrBG     = NULL;
HBRUSH  g_hbrPanel  = NULL;
HBRUSH  g_hbrGreen  = NULL;
HBRUSH  g_hbrYellow = NULL;
HBRUSH  g_hbrRed    = NULL;
HFONT   g_hFontTitle  = NULL;
HFONT   g_hFontNormal = NULL;
HFONT   g_hFontSmall  = NULL;
HFONT   g_hFontBig    = NULL;

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

static int   g_nDpi = 96;
static int   g_nZoom = 100;
static HMENU g_hViewMenu = NULL;
static HMENU g_hLangMenu = NULL;

static void LayoutMainWindow(HWND hWnd);
static void RecreateUiFonts(void);
static void UiApplyFonts(HWND hWnd);
static void UiSyncZoomMenu(void);

int UiScale(int px)
{
    int v;
    if (px <= 0) return px;
    v = MulDiv(px, g_nDpi * g_nZoom, 96 * 100);
    return v < 1 ? 1 : v;
}

/* Logical (96 DPI, 100% zoom) SMART-list column widths and visual order.
 * Layout used to force these on every WM_SIZE, so a drag was lost at once
 * and nothing was written to the registry. */
/* Worst is 68: "Худший" does not fit in the old 50 with the header inset. */
static const int kAttrColDef[ATTR_NCOLS] = { 72, 200, 70, 68, 50, 108, 180, 116 };
static int g_attrColLogical[ATTR_NCOLS] = { 72, 200, 70, 68, 50, 108, 180, 116 };
static int g_attrColOrder[ATTR_NCOLS] = { 0, 1, 2, 3, 4, 5, 6, 7 };

static int AttrColOrderValid(const int* order)
{
    unsigned seen = 0;
    int i;
    for (i = 0; i < ATTR_NCOLS; i++) {
        if (order[i] < 0 || order[i] >= ATTR_NCOLS) return 0;
        if (seen & (1u << order[i])) return 0;
        seen |= 1u << order[i];
    }
    return seen == ((1u << ATTR_NCOLS) - 1);
}

static void AttrColsResetDefault(void)
{
    int i;
    for (i = 0; i < ATTR_NCOLS; i++) {
        g_attrColLogical[i] = kAttrColDef[i];
        g_attrColOrder[i] = i;
    }
}

static int AttrColsLogicalFromPx(int px, int dpi, int zoom)
{
    int v;
    if (dpi < 1) dpi = 96;
    if (zoom < 1) zoom = 100;
    if (px < 1) px = 1;
    v = MulDiv(px, 96 * 100, dpi * zoom);
    if (v < 12) v = 12;
    if (v > 2000) v = 2000;
    return v;
}

static void AttrColsCapture(HWND hList, int dpi, int zoom)
{
    int i;
    int order[ATTR_NCOLS];
    if (!hList) return;
    for (i = 0; i < ATTR_NCOLS; i++) {
        int px = ListView_GetColumnWidth(hList, i);
        g_attrColLogical[i] = AttrColsLogicalFromPx(px, dpi, zoom);
    }
    if (ListView_GetColumnOrderArray(hList, ATTR_NCOLS, order) &&
        AttrColOrderValid(order)) {
        for (i = 0; i < ATTR_NCOLS; i++)
            g_attrColOrder[i] = order[i];
    }
}

static void AttrColsApply(HWND hList)
{
    int i;
    if (!hList) return;
    for (i = 0; i < ATTR_NCOLS; i++) {
        int px = UiScale(g_attrColLogical[i]);
        if (px < 8) px = 8;
        ListView_SetColumnWidth(hList, i, px);
    }
    if (AttrColOrderValid(g_attrColOrder))
        ListView_SetColumnOrderArray(hList, ATTR_NCOLS, g_attrColOrder);
}

static void AttrColsLoad(void)
{
    HKEY k;
    DWORD w[ATTR_NCOLS], o[ATTR_NCOLS], sz, t;
    int i;
    AttrColsResetDefault();
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    sz = sizeof(w);
    t = 0;
    if (RegQueryValueExA(k, "AttrColW", NULL, &t, (LPBYTE)w, &sz) == ERROR_SUCCESS &&
        t == REG_BINARY && sz == sizeof(w)) {
        int ok = 1;
        for (i = 0; i < ATTR_NCOLS; i++)
            if (w[i] < 12 || w[i] > 2000) ok = 0;
        if (ok) {
            for (i = 0; i < ATTR_NCOLS; i++)
                g_attrColLogical[i] = (int)w[i];
            /* Saved 50 is the first default. The header "Худший" is clipped there. */
            if (g_attrColLogical[3] == 50)
                g_attrColLogical[3] = kAttrColDef[3];
        }
    }
    sz = sizeof(o);
    if (RegQueryValueExA(k, "AttrColOrder", NULL, &t, (LPBYTE)o, &sz) == ERROR_SUCCESS &&
        t == REG_BINARY && sz == sizeof(o)) {
        int tmp[ATTR_NCOLS];
        for (i = 0; i < ATTR_NCOLS; i++)
            tmp[i] = (int)o[i];
        if (AttrColOrderValid(tmp)) {
            for (i = 0; i < ATTR_NCOLS; i++)
                g_attrColOrder[i] = tmp[i];
        }
    }
    RegCloseKey(k);
}

static void AttrColsSave(HWND hWnd)
{
    HWND hList;
    HKEY k;
    DWORD w[ATTR_NCOLS], o[ATTR_NCOLS];
    int i;
    hList = hWnd ? GetDlgItem(hWnd, IDC_ATTR_LIST) : NULL;
    if (hList)
        AttrColsCapture(hList, g_nDpi, g_nZoom);
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    for (i = 0; i < ATTR_NCOLS; i++) {
        w[i] = (DWORD)g_attrColLogical[i];
        o[i] = (DWORD)g_attrColOrder[i];
    }
    RegSetValueExA(k, "AttrColW", 0, REG_BINARY, (const BYTE*)w, sizeof(w));
    RegSetValueExA(k, "AttrColOrder", 0, REG_BINARY, (const BYTE*)o, sizeof(o));
    RegCloseKey(k);
}

static UINT QueryDpiForMonitor(HMONITOR hMon)
{
    typedef HRESULT (WINAPI *PFN)(HMONITOR, int, UINT*, UINT*);
    static PFN pGet = NULL;
    static int once = 0;
    UINT x = 0, y = 0;
    if (!once) {
        HMODULE h = LoadLibraryW(L"shcore.dll");
        if (h) pGet = (PFN)GetProcAddress(h, "GetDpiForMonitor");
        once = 1;
    }
    if (pGet && hMon && pGet(hMon, 0, &x, &y) == S_OK && x)
        return x;
    {
        HDC hdc = GetDC(NULL);
        x = hdc ? (UINT)GetDeviceCaps(hdc, LOGPIXELSX) : 96;
        if (hdc) ReleaseDC(NULL, hdc);
    }
    return x ? x : 96;
}

static UINT QueryDpiForWindow(HWND hWnd)
{
    typedef UINT (WINAPI *PFN)(HWND);
    static PFN pGet = NULL;
    static int once = 0;
    if (!once) {
        HMODULE h = GetModuleHandleW(L"user32.dll");
        if (h) pGet = (PFN)GetProcAddress(h, "GetDpiForWindow");
        once = 1;
    }
    if (pGet && hWnd) {
        UINT d = pGet(hWnd);
        if (d) return d;
    }
    return QueryDpiForMonitor(MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST));
}

static int SnapZoom(int z)
{
    if (z < 100) z = 100;
    if (z > 200) z = 200;
    z = ((z + 12) / 25) * 25;
    if (z < 100) z = 100;
    if (z > 200) z = 200;
    return z;
}

static int LoadZoomReg(void)
{
    HKEY k;
    DWORD v = 100, sz = sizeof(v), t = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "UiZoom", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS ||
            t != REG_DWORD)
            v = 100;
        RegCloseKey(k);
    }
    return SnapZoom((int)v);
}

static void SaveZoomReg(int z)
{
    HKEY k;
    DWORD d = (DWORD)z;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "UiZoom", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
}

void UiInitScale(void)
{
    POINT pt = { 0, 0 };
    g_nZoom = LoadZoomReg();
    g_nDpi = (int)QueryDpiForMonitor(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY));
    if (g_nDpi < 96) g_nDpi = 96;
}

int UiLoadWindowPlace(int* x, int* y, int* w, int* h, int* showCmd)
{
    HKEY k;
    DWORD v, sz, t;
    int vx, vy, vw, vh, vs;
    RECT wr, vis;
    HMONITOR hMon;
    MONITORINFO mi;

    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return 0;
    sz = sizeof(v); t = 0;
    if (RegQueryValueExA(k, "WinX", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS || t != REG_DWORD)
        { RegCloseKey(k); return 0; }
    vx = (int)v;
    sz = sizeof(v);
    if (RegQueryValueExA(k, "WinY", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS)
        { RegCloseKey(k); return 0; }
    vy = (int)v;
    sz = sizeof(v);
    if (RegQueryValueExA(k, "WinW", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS)
        { RegCloseKey(k); return 0; }
    vw = (int)v;
    sz = sizeof(v);
    if (RegQueryValueExA(k, "WinH", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS)
        { RegCloseKey(k); return 0; }
    vh = (int)v;
    vs = SW_SHOWNORMAL;
    sz = sizeof(v);
    if (RegQueryValueExA(k, "WinShow", NULL, &t, (LPBYTE)&v, &sz) == ERROR_SUCCESS)
        vs = (int)v;
    RegCloseKey(k);

    if (vw < UiWindowW() / 2 || vh < UiWindowHMin() / 2)
        return 0;
    wr.left = vx; wr.top = vy; wr.right = vx + vw; wr.bottom = vy + vh;
    hMon = MonitorFromRect(&wr, MONITOR_DEFAULTTONULL);
    if (!hMon) return 0;
    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(hMon, &mi)) return 0;
    vis = mi.rcWork;
    if (wr.right < vis.left + 40 || wr.bottom < vis.top + 40 ||
        wr.left > vis.right - 40 || wr.top > vis.bottom - 40)
        return 0;
    if (x) *x = vx;
    if (y) *y = vy;
    if (w) *w = vw;
    if (h) *h = vh;
    if (showCmd) *showCmd = vs;
    return 1;
}

void UiSaveWindowPlace(HWND hWnd)
{
    WINDOWPLACEMENT wp;
    HKEY k;
    DWORD d;
    RECT r;
    if (!hWnd) return;
    ZeroMemory(&wp, sizeof(wp));
    wp.length = sizeof(wp);
    if (!GetWindowPlacement(hWnd, &wp)) return;
    r = wp.rcNormalPosition;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    d = (DWORD)r.left;
    RegSetValueExA(k, "WinX", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    d = (DWORD)r.top;
    RegSetValueExA(k, "WinY", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    d = (DWORD)(r.right - r.left);
    RegSetValueExA(k, "WinW", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    d = (DWORD)(r.bottom - r.top);
    RegSetValueExA(k, "WinH", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    d = (DWORD)wp.showCmd;
    RegSetValueExA(k, "WinShow", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
    RegCloseKey(k);
}

int UiWindowW(void)    { return UiScale(WINDOW_W); }
int UiWindowH(void)    { return UiScale(WINDOW_H); }
int UiWindowHMin(void) { return UiScale(WINDOW_H_MIN); }

HACCEL UiCreateAccelTable(void)
{
    ACCEL a[] = {
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)'S',           IDM_SCREENSHOT },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)VK_OEM_PLUS,   IDM_ZOOM_IN },
        { (BYTE)(FVIRTKEY | FCONTROL | FSHIFT),  (WORD)VK_OEM_PLUS,   IDM_ZOOM_IN },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)VK_ADD,        IDM_ZOOM_IN },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)VK_OEM_MINUS,  IDM_ZOOM_OUT },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)VK_SUBTRACT,   IDM_ZOOM_OUT },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)'0',           IDM_ZOOM_100 },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)VK_NUMPAD0,    IDM_ZOOM_100 },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)'M',           IDM_MONITOR },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)'D',           IDM_DRAM_VERIFY },
        { (BYTE)(FVIRTKEY | FCONTROL),           (WORD)'T',           IDM_SELF_TEST },
    };
    return CreateAcceleratorTable(a, (int)(sizeof(a) / sizeof(a[0])));
}

static HFONT UiMakeFont(int px, int weight, BOOL italic)
{
    return CreateFontA(-UiScale(px), 0, 0, 0, weight, italic ? 1u : 0u, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
}

static void RecreateUiFonts(void)
{
    if (g_hFontTitle)  { DeleteObject(g_hFontTitle);  g_hFontTitle  = NULL; }
    if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = NULL; }
    if (g_hFontSmall)  { DeleteObject(g_hFontSmall);  g_hFontSmall  = NULL; }
    if (g_hFontBig)    { DeleteObject(g_hFontBig);    g_hFontBig    = NULL; }
    g_hFontTitle  = UiMakeFont(13, FW_NORMAL, FALSE);
    g_hFontNormal = UiMakeFont(12, FW_NORMAL, FALSE);
    g_hFontSmall  = UiMakeFont(11, FW_NORMAL, FALSE);
    g_hFontBig    = UiMakeFont(32, FW_BOLD,   FALSE);
}

static void UiApplyFonts(HWND hWnd)
{
    HWND h;
    for (h = GetWindow(hWnd, GW_CHILD); h; h = GetWindow(h, GW_HWNDNEXT)) {
        int id = GetDlgCtrlID(h);
        HFONT f = g_hFontSmall;
        if (id == IDC_PREDICT_STATIC ||
            id == IDC_MODEL_STATIC || id == IDC_BRAND_STATIC ||
            id == IDC_CONTROLLER_STATIC || id == IDC_SERIAL_STATIC ||
            id == IDC_FIRMWARE_STATIC || id == IDC_SIZE_STATIC ||
            id == IDC_TEMP_STATIC || id == IDC_POH_STATIC ||
            id == IDC_STATUS_STATIC || id == IDC_PROTOCOL_STATIC ||
            id == IDC_ADAPTER_STATIC)
            f = g_hFontNormal;
        SendMessage(h, WM_SETFONT, (WPARAM)f, TRUE);
        if (id == IDC_ATTR_LIST) {
            HWND hHdr = ListView_GetHeader(h);
            if (hHdr) SendMessage(hHdr, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
        }
    }
}

static void UiSyncZoomMenu(void)
{
    UINT id = IDM_ZOOM_100;
    if (g_nZoom == 125) id = IDM_ZOOM_125;
    else if (g_nZoom == 150) id = IDM_ZOOM_150;
    else if (g_nZoom == 175) id = IDM_ZOOM_175;
    else if (g_nZoom == 200) id = IDM_ZOOM_200;
    if (g_hViewMenu)
        CheckMenuRadioItem(g_hViewMenu, IDM_ZOOM_100, IDM_ZOOM_200, id, MF_BYCOMMAND);
}

static void UiChangeZoom(HWND hWnd, int z)
{
    int old = g_nZoom;
    RECT rc, wa;
    int nw, nh;
    HMONITOR hMon;
    MONITORINFO mi;

    z = SnapZoom(z);
    if (z == old) return;
    AttrColsCapture(GetDlgItem(hWnd, IDC_ATTR_LIST), g_nDpi, old);
    g_nZoom = z;
    SaveZoomReg(z);
    RecreateUiFonts();
    UiApplyFonts(hWnd);
    UiSyncZoomMenu();

    GetWindowRect(hWnd, &rc);
    nw = MulDiv(rc.right - rc.left, z, old);
    nh = MulDiv(rc.bottom - rc.top, z, old);
    if (nw < UiWindowW()) nw = UiWindowW();
    if (nh < UiWindowHMin()) nh = UiWindowHMin();

    ZeroMemory(&mi, sizeof(mi));
    mi.cbSize = sizeof(mi);
    hMon = MonitorFromWindow(hWnd, MONITOR_DEFAULTTONEAREST);
    if (GetMonitorInfoW(hMon, &mi))
        wa = mi.rcWork;
    else if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0; wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    if (nw > wa.right - wa.left) nw = wa.right - wa.left;
    if (nh > wa.bottom - wa.top) nh = wa.bottom - wa.top;
    if (rc.left + nw > wa.right) rc.left = wa.right - nw;
    if (rc.top + nh > wa.bottom) rc.top = wa.bottom - nh;
    if (rc.left < wa.left) rc.left = wa.left;
    if (rc.top < wa.top) rc.top = wa.top;

    SetWindowPos(hWnd, NULL, rc.left, rc.top, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMainWindow(hWnd);
    AttrColsApply(GetDlgItem(hWnd, IDC_ATTR_LIST));
    InvalidateRect(hWnd, NULL, TRUE);
}

static void UiOnDpiChanged(HWND hWnd, UINT dpi, const RECT* prc)
{
    int oldDpi = g_nDpi;
    if (dpi < 96) dpi = 96;
    AttrColsCapture(GetDlgItem(hWnd, IDC_ATTR_LIST), oldDpi, g_nZoom);
    g_nDpi = (int)dpi;
    RecreateUiFonts();
    UiApplyFonts(hWnd);
    if (prc)
        SetWindowPos(hWnd, NULL, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMainWindow(hWnd);
    AttrColsApply(GetDlgItem(hWnd, IDC_ATTR_LIST));
    InvalidateRect(hWnd, NULL, TRUE);
}

void CreateGDIObjects(void)
{
    if (!g_hbrBG)     g_hbrBG     = CreateSolidBrush(CLR_BG);
    if (!g_hbrPanel)  g_hbrPanel  = CreateSolidBrush(CLR_PANEL);
    if (!g_hbrGreen)  g_hbrGreen  = CreateSolidBrush(CLR_GREEN);
    if (!g_hbrYellow) g_hbrYellow = CreateSolidBrush(CLR_YELLOW);
    if (!g_hbrRed)    g_hbrRed    = CreateSolidBrush(CLR_RED);
    RecreateUiFonts();
}

static ULONG_PTR g_gdiplusToken = 0;

static int GetEncoderClsid(const WCHAR* format, CLSID* pClsid)
{
    UINT nEncoders = 0, nSize = 0;
    GetImageEncodersSize(&nEncoders, &nSize);
    if (nSize == 0) return -1;

    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(nSize);
    if (!pInfo) return -1;
    GetImageEncoders(nEncoders, nSize, pInfo);

    for (UINT i = 0; i < nEncoders; i++) {
        if (wcscmp(pInfo[i].MimeType, format) == 0) {
            *pClsid = pInfo[i].Clsid;
            free(pInfo);
            return (int)i;
        }
    }
    free(pInfo);
    return -1;
}

static BOOL SaveScreenshotPNG(HWND hWnd, char* szPathOut, int nPathMax,
                               char* szErrOut, int nErrMax)
{

    char szDocDir[MAX_PATH] = "";
    if (!SHGetSpecialFolderPathA(NULL, szDocDir, CSIDL_PERSONAL, TRUE)) {

        GetModuleFileNameA(NULL, szDocDir, MAX_PATH);
        char* p = strrchr(szDocDir, '\\');
        if (p) *p = '\0';
    }
    char szOutDir[MAX_PATH];
    safe_snprintf(szOutDir, "%s\\HDDH_Screenshots", szDocDir);
    CreateDirectoryA(szOutDir, NULL);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char szFile[MAX_PATH];
    safe_snprintf(szFile,
        "%s\\HDDH_%04d%02d%02d_%02d%02d%02d.png",
        szOutDir,
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    if (szPathOut) lstrcpynA(szPathOut, szFile, nPathMax);

    RECT rc;
    GetClientRect(hWnd, &rc);
    int w = rc.right  - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) {
        if (szErrOut) lstrcpynA(szErrOut, "Window has zero size.", nErrMax);
        return FALSE;
    }

    HDC hdcWin = GetDC(hWnd);
    HDC hdcMem = CreateCompatibleDC(hdcWin);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWin, w, h);
    HBITMAP hbmOld = (HBITMAP)SelectObject(hdcMem, hbm);

    if (!PrintWindow(hWnd, hdcMem, PW_CLIENTONLY)) {

        POINT pt = {rc.left, rc.top};
        ClientToScreen(hWnd, &pt);
        HDC hdcScreen = GetDC(NULL);
        BitBlt(hdcMem, 0, 0, w, h, hdcScreen, pt.x, pt.y, SRCCOPY);
        ReleaseDC(NULL, hdcScreen);
    }

    SelectObject(hdcMem, hbmOld);
    DeleteDC(hdcMem);
    ReleaseDC(hWnd, hdcWin);

    BOOL bOK = FALSE;
    Bitmap* pBmp = Bitmap::FromHBITMAP(hbm, NULL);
    if (pBmp && pBmp->GetLastStatus() == Ok) {
        CLSID clsidPng;
        if (GetEncoderClsid(L"image/png", &clsidPng) >= 0) {

            WCHAR wszFile[MAX_PATH];
            MultiByteToWideChar(CP_ACP, 0, szFile, -1, wszFile, MAX_PATH);
            Status st2 = pBmp->Save(wszFile, &clsidPng, NULL);
            bOK = (st2 == Ok);
            if (!bOK && szErrOut)
                safe_snprintf_n(szErrOut, nErrMax,
                    "GDI+ Save failed (status %d).", (int)st2);
        } else {
            if (szErrOut) lstrcpynA(szErrOut, "PNG encoder not found.", nErrMax);
        }
    } else {
        if (szErrOut) lstrcpynA(szErrOut, "GDI+ Bitmap creation failed.", nErrMax);
    }
    delete pBmp;
    DeleteObject(hbm);
    return bOK;
}

static void DoSaveScreenshot(HWND hWnd)
{
    char szPath[MAX_PATH] = "";
    char szErr[256]       = "";
    if (SaveScreenshotPNG(hWnd, szPath, MAX_PATH, szErr, sizeof(szErr))) {
        char szMsg[MAX_PATH + 128];
        safe_snprintf(szMsg,
            "Screenshot saved successfully!\n\n%s\n\nOpen folder now?", szPath);
        int nRet = MessageBoxU8(hWnd, szMsg, "DriveMonitor - Screenshot Saved",
                               MB_YESNO | MB_ICONINFORMATION);
        if (nRet == IDYES) {

            char szCmd[MAX_PATH + 32];
            safe_snprintf(szCmd, "/select,\"%s\"", szPath);
            ShellExecuteA(NULL, "open", "explorer.exe", szCmd, NULL, SW_SHOWNORMAL);
        }
    } else {
        char szMsg[320];
        safe_snprintf(szMsg, "Screenshot failed:\n%s", szErr);
        MessageBoxU8(hWnd, szMsg, "DriveMonitor - Screenshot Error", MB_OK | MB_ICONERROR);
    }
}

/* Append UTF-8 bytes to a heap report buffer. Always NUL-terminates. */
static void ReportCat(char* buf, size_t cap, size_t* pLen, const char* s)
{
    size_t n, room;
    if (!buf || !pLen || cap == 0)
        return;
    if (*pLen >= cap) {
        buf[cap - 1] = '\0';
        return;
    }
    if (!s)
        return;
    room = cap - *pLen - 1;
    n = strlen(s);
    if (n > room)
        n = room;
    if (n > 0)
        memcpy(buf + *pLen, s, n);
    *pLen += n;
    buf[*pLen] = '\0';
}

static const char* ReportDash(const char* s)
{
    return (s && s[0]) ? s : "—";
}

/* Replace \ / : * ? " < > | and control chars with _; keep UTF-8 payload. */
static void SanitizeModelForFilename(const char* src, char* dst, int nDst)
{
    int i, o;
    if (!dst || nDst <= 0)
        return;
    dst[0] = '\0';
    if (nDst == 1)
        return;
    if (!src)
        src = "";
    o = 0;
    for (i = 0; src[i] && o < nDst - 1; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c < 32 || c == 127 || c == '\\' || c == '/' || c == ':' ||
            c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            dst[o++] = '_';
        else
            dst[o++] = (char)c;
    }
    dst[o] = '\0';
    if (dst[0] == '\0')
        lstrcpynA(dst, "disk", nDst);
}

static void Utf8TruncateBytes(char* s, int nMax)
{
    int i = 0;
    if (!s)
        return;
    if (nMax <= 0) {
        s[0] = '\0';
        return;
    }
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        int seq = 1;
        if ((c & 0xE0) == 0xC0)
            seq = 2;
        else if ((c & 0xF0) == 0xE0)
            seq = 3;
        else if ((c & 0xF8) == 0xF0)
            seq = 4;
        if (i + seq > nMax) {
            s[i] = '\0';
            return;
        }
        i += seq;
    }
}

static void ReportMdCell(char* buf, size_t cap, size_t* pLen, const char* s)
{
    char tmp[512];
    int i, o;
    if (!s) s = "";
    o = 0;
    for (i = 0; s[i] && o < (int)sizeof(tmp) - 2; i++) {
        char c = s[i];
        if (c == '\r' || c == '\n' || c == '\t')
            tmp[o++] = ' ';
        else if (c == '|') {
            tmp[o++] = '\\';
            if (o < (int)sizeof(tmp) - 1)
                tmp[o++] = '|';
        } else
            tmp[o++] = c;
    }
    tmp[o] = '\0';
    ReportCat(buf, cap, pLen, tmp[0] ? tmp : "—");
}

static void ReportMdKv(char* buf, size_t cap, size_t* pLen,
                       const char* key, const char* val)
{
    ReportCat(buf, cap, pLen, "| ");
    ReportCat(buf, cap, pLen, key);
    ReportCat(buf, cap, pLen, " | ");
    ReportMdCell(buf, cap, pLen, val);
    ReportCat(buf, cap, pLen, " |\r\n");
}

static void BuildSaveReportFilter(WCHAR* dst, int nDst)
{
    static const char* parts[] = {
        "Markdown (*.md)",
        "*.md",
        UiLangIsEn() ? "All files (*.*)" : "Все файлы (*.*)",
        "*.*",
        ""
    };
    int pos = 0;
    int i;
    if (!dst || nDst <= 0)
        return;
    dst[0] = 0;
    for (i = 0; i < 5; i++) {
        WCHAR tmp[96];
        int n = U8ToW(parts[i], tmp, 96);
        if (n <= 0)
            n = 1;
        if (pos + n > nDst) {
            if (pos < nDst)
                dst[pos] = 0;
            break;
        }
        memcpy(dst + pos, tmp, (size_t)n * sizeof(WCHAR));
        pos += n;
    }
    if (pos < nDst)
        dst[pos] = 0; /* extra NUL if the last copy did not land one */
}

static void DoSaveDriveReport(HWND hWnd)
{
    const size_t kCap = 65536;
    DRIVE_INFO* pInfo;
    char* buf;
    size_t len = 0;
    SYSTEMTIME st;
    char szDocDir[MAX_PATH];
    char szModel[64];
    char szFileName[MAX_PATH];
    char szLine[512];
    WCHAR wzFile[MAX_PATH];
    WCHAR wzDir[MAX_PATH];
    WCHAR wzFilter[192];
    WCHAR wzTitle[64];
    OPENFILENAMEW ofn;
    HANDLE hFile;
    HWND hList;
    int nItems;
    int nMaxModel;
    int nDirLen;

    if (g_nSelectedDrive < 0 || g_nSelectedDrive >= g_nDriveCount) {
        MessageBoxU8(hWnd, Tr(STR_NO_DRIVE), "DriveMonitor",
                     MB_OK | MB_ICONWARNING);
        return;
    }
    pInfo = &g_Drives[g_nSelectedDrive];

    buf = (char*)malloc(kCap);
    if (!buf) {
        MessageBoxU8(hWnd, Tr(STR_NO_MEM), "DriveMonitor",
                     MB_OK | MB_ICONERROR);
        return;
    }
    buf[0] = '\0';

    GetLocalTime(&st);

    ReportCat(buf, kCap, &len, Tr(STR_REPORT_HD));
    safe_snprintf(szLine, TN("**Версия:** %s (сборка %s)  \r\n", "**Version:** %s (build %s)  \r\n"),
                  DRIVEMONITOR_FILEVERSION_STR, DRIVEMONITOR_BUILD_STR);
    ReportCat(buf, kCap, &len, szLine);
    safe_snprintf(szLine, TN("**Дата:** %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
                             "**Date:** %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n"),
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    ReportCat(buf, kCap, &len, szLine);

    ReportCat(buf, kCap, &len, Tr(STR_REPORT_DRIVE));
    safe_snprintf(szLine, "| %s | %s |\r\n| --- | --- |\r\n",
                  Tr(STR_REPORT_FIELD), Tr(STR_REPORT_VALUE));
    ReportCat(buf, kCap, &len, szLine);
    ReportMdKv(buf, kCap, &len, Tr(STR_MODEL), ReportDash(pInfo->szModel));
    if (pInfo->szModelFamily[0])
        ReportMdKv(buf, kCap, &len, TN("Семейство", "Family"), pInfo->szModelFamily);
    else if (pInfo->szDriveDbTie[0] || pInfo->bDriveDbTieUnnamed)
        ReportMdKv(buf, kCap, &len, TN("Семейство", "Family"),
                   TN("нет имени", "no name"));
    if (pInfo->szDriveDbTie[0])
        ReportMdKv(buf, kCap, &len, TN("Та же оценка базы", "Same database score"),
                   pInfo->szDriveDbTie);
    else if (pInfo->bDriveDbTieUnnamed)
        ReportMdKv(buf, kCap, &len, TN("Та же оценка базы", "Same database score"),
                   TN("строка без имени семейства", "row with no family name"));
    {
        const char* brand = GetVendorName(pInfo->eVendor);
        if (pInfo->eVendor == VENDOR_UNKNOWN || pInfo->eVendor == VENDOR_OTHER)
            brand = "—";
        ReportMdKv(buf, kCap, &len, Tr(STR_BRAND), brand);
    }
    if (pInfo->bCounterfeitSuspected) {
        ReportMdKv(buf, kCap, &len,
                   TN("⚠️ Подлинность", "⚠️ Authenticity"),
                   TN("ОБНАРУЖЕНЫ ПРИЗНАКИ ПОДДЕЛКИ / РЕМАРКИРОВАНИЯ", "SUSPECTED COUNTERFEIT / RELABELED"));
        ReportMdKv(buf, kCap, &len,
                   TN("Причина несоответствия", "Mismatch reason"),
                   GetCounterfeitReason(pInfo));
    }
    ReportMdKv(buf, kCap, &len, Tr(STR_CONTROLLER), DriveControllerLabel(pInfo));
    ReportMdKv(buf, kCap, &len, Tr(STR_SERIAL), ReportDash(pInfo->szSerial));
    ReportMdKv(buf, kCap, &len, Tr(STR_FIRMWARE), ReportDash(pInfo->szFirmware));
    {
        char szSize[32], szBoth[80];
        FormatSize(pInfo->dwCapacityMB, szSize, (int)sizeof(szSize));
        safe_snprintf(szBoth, "%s · %s", szSize, GetDriveTypeName(pInfo->eType));
        ReportMdKv(buf, kCap, &len, TN("Объём / тип", "Capacity / type"), szBoth);
    }
    if (pInfo->nTemperatureC > 0) {
        if (pInfo->eTempBand != TEMP_BAND_UNKNOWN)
            safe_snprintf(szLine, "%d °C (%s)", pInfo->nTemperatureC,
                          GetTempBandName(pInfo->eTempBand, TRUE));
        else
            safe_snprintf(szLine, "%d °C", pInfo->nTemperatureC);
        ReportMdKv(buf, kCap, &len, Tr(STR_TEMPERATURE), szLine);
    } else {
        ReportMdKv(buf, kCap, &len, Tr(STR_TEMPERATURE), "—");
    }
    {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        ReportMdKv(buf, kCap, &len, Tr(STR_POH), szPoh);
    }
    if (pInfo->dwPowerCycleCount > 0) {
        safe_snprintf(szLine, "%lu", (unsigned long)pInfo->dwPowerCycleCount);
        ReportMdKv(buf, kCap, &len, TN("Циклы включения", "Power cycles"), szLine);
    } else {
        ReportMdKv(buf, kCap, &len, TN("Циклы включения", "Power cycles"), Tr(STR_POH_NONE));
    }
    ReportMdKv(buf, kCap, &len, Tr(STR_PROTOCOL), ReportDash(pInfo->szProtocol));
    if (pInfo->bIsUSB) {
        char szAdapter[64];
        FormatUsbAdapterName(pInfo, szAdapter, (int)sizeof(szAdapter));
        ReportMdKv(buf, kCap, &len, TN("Переходник/мост", "USB bridge"), ReportDash(szAdapter));
    }

    ReportCat(buf, kCap, &len, TN("\r\n## Оценка\r\n\r\n```\r\n",
                                 "\r\n## Assessment\r\n\r\n```\r\n"));
    if (len < kCap - 1) {
        FormatHealthLecturePlain(pInfo, buf + len, (int)(kCap - len));
        len = strlen(buf);
    }
    if (len == 0 || buf[len - 1] != '\n')
        ReportCat(buf, kCap, &len, "\r\n");
    ReportCat(buf, kCap, &len, TN("```\r\n\r\n## Экспертный отчёт\r\n\r\n```\r\n",
                                 "```\r\n\r\n## Expert notes\r\n\r\n```\r\n"));
    if (len < kCap - 1) {
        FormatHealthLectureExpert(pInfo, buf + len, (int)(kCap - len));
        len = strlen(buf);
    }
    if (len == 0 || buf[len - 1] != '\n')
        ReportCat(buf, kCap, &len, "\r\n");
    ReportCat(buf, kCap, &len, "```\r\n");

    ReportCat(buf, kCap, &len, "\r\n## SMART / NVMe\r\n\r\n");

    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    nItems = hList ? (int)ListView_GetItemCount(hList) : 0;
    if (nItems <= 0) {
        ReportCat(buf, kCap, &len, "_Таблица SMART пуста._\r\n");
    } else {
        int r, c;
        const char* headers[ATTR_NCOLS];
        headers[0] = "ID";
        headers[1] = Tr(STR_COL_PARAM);
        headers[2] = Tr(STR_COL_VALUE);
        headers[3] = Tr(STR_COL_WORST);
        headers[4] = Tr(STR_COL_THRESH);
        headers[5] = "RAW";
        headers[6] = Tr(STR_COL_DECODE);
        headers[7] = Tr(STR_COL_STATUS);
        ReportCat(buf, kCap, &len, "| ");
        for (c = 0; c < ATTR_NCOLS; c++) {
            ReportCat(buf, kCap, &len, headers[c]);
            ReportCat(buf, kCap, &len, (c < ATTR_NCOLS - 1) ? " | " : " |\r\n");
        }
        ReportCat(buf, kCap, &len,
                  "| --- | --- | --- | --- | --- | --- | --- | --- |\r\n");
        for (r = 0; r < nItems; r++) {
            ReportCat(buf, kCap, &len, "| ");
            for (c = 0; c < ATTR_NCOLS; c++) {
                WCHAR wcell[256];
                char ucell[512];
                wcell[0] = 0;
                ListView_GetItemText(hList, r, c, wcell, 256);
                WToU8(wcell, ucell, 512);
                ReportMdCell(buf, kCap, &len, ucell);
                ReportCat(buf, kCap, &len, (c < ATTR_NCOLS - 1) ? " | " : " |\r\n");
            }
        }
    }

    if (pInfo->eventStats.bQueried) {
        ReportCat(buf, kCap, &len, TN("\r\n## Системные события Windows (Event Log, 30 дней)\r\n\r\n",
                                     "\r\n## Windows Storage Events (Event Log, 30 days)\r\n\r\n"));
        if (pInfo->eventStats.nEvent153Count == 0 &&
            pInfo->eventStats.nEvent129Count == 0 &&
            pInfo->eventStats.nEvent51Count == 0 &&
            pInfo->eventStats.nEventHwErrCount == 0) {
            ReportCat(buf, kCap, &len, TN("Аппаратных сбоев ввода-вывода (Event 153, 129, 51, 7) не зафиксировано.\r\n",
                                         "No hardware IO faults (Event 153, 129, 51, 7) recorded.\r\n"));
        } else {
            char szEvtLine[256];
            safe_snprintf(szEvtLine, "- **Event 153 (IO Retries / таймауты)**: %d\r\n", pInfo->eventStats.nEvent153Count);
            ReportCat(buf, kCap, &len, szEvtLine);
            safe_snprintf(szEvtLine, "- **Event 129 (Controller Resets / сбросы шины)**: %d\r\n", pInfo->eventStats.nEvent129Count);
            ReportCat(buf, kCap, &len, szEvtLine);
            safe_snprintf(szEvtLine, "- **Event 51 (Paging Errors / ошибки страничного обмена)**: %d\r\n", pInfo->eventStats.nEvent51Count);
            ReportCat(buf, kCap, &len, szEvtLine);
            safe_snprintf(szEvtLine, "- **Event 7/154 (Bad Blocks / аппаратные сбои блоков)**: %d\r\n", pInfo->eventStats.nEventHwErrCount);
            ReportCat(buf, kCap, &len, szEvtLine);
            if (pInfo->eventStats.szLastEventTime[0]) {
                safe_snprintf(szEvtLine, "- **Последнее событие / Latest event**: %s (%s)\r\n",
                              pInfo->eventStats.szLastEventTime, pInfo->eventStats.szLastEventDesc);
                ReportCat(buf, kCap, &len, szEvtLine);
            }
        }
    }

    {
        const LATENCY_PROBE_RESULT* pLat = GetDriveLatencyResult(pInfo->nDriveIndex);
        if (pLat && pLat->nSamplesTested > 0) {
            char szLatMd[4096];
            FormatLatencyMarkdown(pLat, szLatMd, sizeof(szLatMd));
            ReportCat(buf, kCap, &len, "\r\n");
            ReportCat(buf, kCap, &len, szLatMd);
        }
    }

    if (pInfo->trimInfo.bQueried) {
        char szTrimLine[256];
        ReportCat(buf, kCap, &len, TN("\r\n## Статус TRIM и логические разделы\r\n\r\n",
                                     "\r\n## TRIM Status & Logical Volumes\r\n\r\n"));
        safe_snprintf(szTrimLine, TN("- **TRIM в Windows (DisableDeleteNotify)**: %s\r\n",
                                     "- **Windows TRIM (DisableDeleteNotify)**: %s\r\n"),
                      pInfo->trimInfo.bOsTrimEnabled ? TN("Включен (0)", "Enabled (0)")
                                                     : TN("ОТКЛЮЧЕН (1) ⚠️", "DISABLED (1) ⚠️"));
        ReportCat(buf, kCap, &len, szTrimLine);
        safe_snprintf(szTrimLine, TN("- **Поддержка TRIM накопителем**: %s\r\n",
                                     "- **Drive TRIM Support**: %s\r\n"),
                      pInfo->trimInfo.bDeviceTrimSupported ? TN("Поддерживается", "Supported")
                                                           : TN("Не заявлена / неизвестно", "Not reported / unknown"));
        ReportCat(buf, kCap, &len, szTrimLine);

        if (pInfo->trimInfo.nVolumeCount > 0) {
            int vi;
            ReportCat(buf, kCap, &len, TN("\r\n### Логические разделы\r\n\r\n",
                                         "\r\n### Logical Volumes\r\n\r\n"));
            ReportCat(buf, kCap, &len, TN("| Том | Файловая система | TRIM раздела | Свободно | Всего | Свободно % |\r\n| --- | --- | --- | --- | --- | --- |\r\n",
                                         "| Volume | File System | Volume TRIM | Free | Total | Free % |\r\n| --- | --- | --- | --- | --- | --- |\r\n"));
            for (vi = 0; vi < pInfo->trimInfo.nVolumeCount; vi++) {
                const VOLUME_INFO_ENTRY* v = &pInfo->trimInfo.volumes[vi];
                char szFree[32], szTot[32], szPct[32];
                if (v->qwTotalBytes == 0) {
                    safe_snprintf(szTrimLine, "| %s | %s | %s | %s | %s | %s |\r\n",
                                  v->szLetter[0] ? v->szLetter : "?",
                                  v->szFileSystem[0] ? v->szFileSystem : "RAW",
                                  "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94", "\xe2\x80\x94");
                    ReportCat(buf, kCap, &len, szTrimLine);
                    continue;
                }
                double flFreeGb = (double)v->qwFreeBytes / (1024.0 * 1024.0 * 1024.0);
                double flTotGb = (double)v->qwTotalBytes / (1024.0 * 1024.0 * 1024.0);
                if (flTotGb < 1.0) {
                    double flFreeMb = (double)v->qwFreeBytes / (1024.0 * 1024.0);
                    double flTotMb = (double)v->qwTotalBytes / (1024.0 * 1024.0);
                    safe_snprintf(szFree, "%.0f MB", flFreeMb);
                    safe_snprintf(szTot, "%.0f MB", flTotMb);
                } else {
                    safe_snprintf(szFree, "%.1f GB", flFreeGb);
                    safe_snprintf(szTot, "%.1f GB", flTotGb);
    
                }

                if (v->fFreePercent >= 0.0f) {
                    safe_snprintf(szPct, "%.1f%%", (double)v->fFreePercent);
                } else {
                    safe_snprintf(szPct, "\xe2\x80\x94");
                }
                safe_snprintf(szTrimLine, "| %s | %s | %s | %s | %s | %s |\r\n",
                              v->szLetter, v->szFileSystem,
                              v->bTrimSupportedFs ? TN("Да", "Yes") : TN("НЕТ ⚠️", "NO ⚠️"),
                              szFree, szTot, szPct);
                ReportCat(buf, kCap, &len, szTrimLine);
            }
        }
    }

    {
        char szDramMd[4096];
        FormatDramVerifyMarkdown(GetDriveDramVerifyResult(pInfo->nDriveIndex), szDramMd, sizeof(szDramMd));
        ReportCat(buf, kCap, &len, "\r\n");
        ReportCat(buf, kCap, &len, szDramMd);
    }

    szDocDir[0] = '\0';
    if (!SHGetSpecialFolderPathA(NULL, szDocDir, CSIDL_PERSONAL, TRUE)) {
        GetModuleFileNameA(NULL, szDocDir, MAX_PATH);
        {
            char* slash = strrchr(szDocDir, '\\');
            if (slash) *slash = '\0';
        }
    }

    SanitizeModelForFilename(pInfo->szModel, szModel, (int)sizeof(szModel));
    nDirLen = (int)strlen(szDocDir);
    /* dir + '\' + DriveMonitor_ + model + _YYYYMMDD_HHMMSS.md + NUL */
    nMaxModel = MAX_PATH - nDirLen - 1 - 13 - 20 - 1;
    if (nMaxModel < 1)
        nMaxModel = 1;
    if (nMaxModel > (int)sizeof(szModel) - 1)
        nMaxModel = (int)sizeof(szModel) - 1;
    Utf8TruncateBytes(szModel, nMaxModel);
    if (szModel[0] == '\0')
        lstrcpynA(szModel, "disk", (int)sizeof(szModel));

    safe_snprintf(szFileName, "DriveMonitor_%s_%04d%02d%02d_%02d%02d%02d.md",
                  szModel,
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);

    U8ToW(szFileName, wzFile, MAX_PATH);
    U8ToW(szDocDir, wzDir, MAX_PATH);
    BuildSaveReportFilter(wzFilter, 192);
    U8ToW(Tr(STR_REPORT_TITLE), wzTitle, 64);

    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize     = sizeof(ofn);
    ofn.hwndOwner       = hWnd;
    ofn.lpstrFilter     = wzFilter;
    ofn.nFilterIndex    = 1;
    ofn.lpstrFile       = wzFile;
    ofn.nMaxFile        = MAX_PATH;
    ofn.lpstrInitialDir = wzDir;
    ofn.lpstrTitle      = wzTitle;
    ofn.Flags           = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
                          OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    ofn.lpstrDefExt     = L"md";

    if (!GetSaveFileNameW(&ofn)) {
        free(buf);
        return;
    }

    hFile = CreateFileW(ofn.lpstrFile, GENERIC_WRITE, FILE_SHARE_READ,
                        NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) {
        MessageBoxU8(hWnd, Tr(STR_REPORT_FAIL), "DriveMonitor",
                     MB_OK | MB_ICONERROR);
        free(buf);
        return;
    }
    {
        static const BYTE bom[3] = { 0xEF, 0xBB, 0xBF };
        DWORD written = 0;
        BOOL ok = WriteFile(hFile, bom, 3, &written, NULL);
        if (ok && len > 0)
            ok = WriteFile(hFile, buf, (DWORD)len, &written, NULL);
        CloseHandle(hFile);
        free(buf);
        buf = NULL;
        if (!ok) {
            MessageBoxU8(hWnd, Tr(STR_REPORT_WRITE_FAIL), "DriveMonitor",
                         MB_OK | MB_ICONERROR);
            return;
        }
    }

    {
        char szPathU8[MAX_PATH * 3];
        char szMsg[MAX_PATH * 3 + 64];
        WToU8(ofn.lpstrFile, szPathU8, (int)sizeof(szPathU8));
        safe_snprintf(szMsg, Tr(STR_REPORT_SAVED), szPathU8);
        MessageBoxU8(hWnd, szMsg, "DriveMonitor", MB_OK | MB_ICONINFORMATION);
    }
}

static void SetActionButtonsEnabled(HWND hWnd, BOOL on)
{
    HWND hReread = GetDlgItem(hWnd, IDC_REREAD_BTN);
    HWND hReport = GetDlgItem(hWnd, IDC_REPORT_BTN);
    HWND hEject  = GetDlgItem(hWnd, IDC_EJECT_BTN);
    HWND hTest   = GetDlgItem(hWnd, IDC_LATENCY_BTN);
    HWND hDram   = GetDlgItem(hWnd, IDC_DRAM_VERIFY_BTN);
    HWND hDst    = GetDlgItem(hWnd, IDC_SELF_TEST_BTN);
    BOOL usb = FALSE;
    BOOL hasDrive = (on && g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount);
    if (hasDrive)
        usb = g_Drives[g_nSelectedDrive].bIsUSB;
    if (hReread) EnableWindow(hReread, on);
    if (hReport) EnableWindow(hReport, on && hasDrive);
    if (hEject)  EnableWindow(hEject, on && usb);
    if (hTest)   EnableWindow(hTest, hasDrive);
    if (hDram)   EnableWindow(hDram, hasDrive);
    if (hDst)    EnableWindow(hDst, hasDrive);
}

static void DoSafeEject(HWND hWnd)
{
    DRIVE_INFO* pInfo;
    char szErr[320];
    char szAsk[384];
    const char* szModel;

    if (g_nSelectedDrive < 0 || g_nSelectedDrive >= g_nDriveCount) {
        MessageBoxU8(hWnd, Tr(STR_NO_DRIVE), "DriveMonitor",
                     MB_OK | MB_ICONWARNING);
        return;
    }
    pInfo = &g_Drives[g_nSelectedDrive];
    if (!pInfo->bIsUSB) {
        MessageBoxU8(hWnd, Tr(STR_EJECT_NO_USB),
            "DriveMonitor", MB_OK | MB_ICONINFORMATION);
        return;
    }
    szModel = pInfo->szModel[0] ? pInfo->szModel : TN("диск", "drive");
    safe_snprintf(szAsk, Tr(STR_EJECT_CONFIRM), szModel);
    if (MessageBoxU8(hWnd, szAsk, "DriveMonitor",
                     MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    szErr[0] = '\0';
    if (!SafeEjectPhysicalDrive(pInfo->nDriveIndex, szErr, (int)sizeof(szErr))) {
        MessageBoxU8(hWnd,
            szErr[0] ? szErr :
                TN("Не удалось извлечь диск. Закройте файлы и повторите.",
                   "Could not eject the drive. Close any open files and try again."),
            "DriveMonitor", MB_OK | MB_ICONWARNING);
        return;
    }
    MessageBoxU8(hWnd, Tr(STR_EJECT_OK),
                 "DriveMonitor", MB_OK | MB_ICONINFORMATION);
    RefreshData(hWnd);
}

static void Snapshot_Save(void)
{
    int i;
    g_nPrevCount = g_nDriveCount;
    for (i = 0; i < g_nDriveCount; i++)
        g_PrevDrives[i] = g_Drives[i];
}

static void MonitorShow(HWND hWnd)
{
    char title[448];
    if (!hWnd) hWnd = g_hMainWnd;
    if (!hWnd) return;
    SetDlgItemTextU8(hWnd, IDC_MONITOR_STATIC, g_szMonitor);
    if (g_bMonitorAlert && g_szMonitor[0])
        safe_snprintf(title, "DriveMonitor — %s", g_szMonitor);
    else
        lstrcpynA(title, "DriveMonitor", (int)sizeof(title));
    SetWindowTextU8(hWnd, title);
    InvalidateRect(GetDlgItem(hWnd, IDC_MONITOR_STATIC), NULL, FALSE);
}

static void MonitorSetIdle(void)
{
    if (g_bMonitorAlert) return;
    g_bMonitorQuiet = 0;
    if (g_bMonitorOn)
        lstrcpynA(g_szMonitor,
                  TN("Следит · чтение раз в минуту", "Watching · once a minute"),
                  (int)sizeof(g_szMonitor));
    else
        lstrcpynA(g_szMonitor,
                  TN("Мониторинг выключен", "Monitor is off"),
                  (int)sizeof(g_szMonitor));
}

static void MonitorLoad(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), t = 0;
    g_bMonitorOn = 0;
    g_bMonitorAlert = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "Monitor", NULL, &t, (LPBYTE)&v, &sz) == ERROR_SUCCESS &&
            t == REG_DWORD)
            g_bMonitorOn = (v != 0);
        RegCloseKey(k);
    }
    MonitorSetIdle();
}

static void MonitorSave(void)
{
    HKEY k;
    DWORD d = g_bMonitorOn ? 1u : 0u;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "Monitor", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
}

static void MonitorApplyTimer(HWND hWnd)
{
    if (!hWnd) return;
    KillTimer(hWnd, IDT_MONITOR);
    if (g_bMonitorOn)
        SetTimer(hWnd, IDT_MONITOR, MONITOR_INTERVAL_MS, NULL);
}

static void TrayUpdate(HWND hWnd);

static void MonitorToggle(HWND hWnd)
{
    g_bMonitorOn = !g_bMonitorOn;
    MonitorSave();
    if (g_hViewMenu)
        CheckMenuItem(g_hViewMenu, IDM_MONITOR,
                      MF_BYCOMMAND | (g_bMonitorOn ? MF_CHECKED : MF_UNCHECKED));
    MonitorApplyTimer(hWnd);
    MonitorSetIdle();
    MonitorShow(hWnd);
    /* Icons exist only while monitoring is on. If the window was hidden
     * in the tray, bring it back when monitoring stops. */
    if (!g_bMonitorOn && hWnd && !IsWindowVisible(hWnd))
        ShowWindow(hWnd, SW_SHOW);
    TrayUpdate(hWnd);
}

static void MonitorAlert(HWND hWnd)
{
    FLASHWINFO fw;
    if (!g_bMonitorOn || !hWnd) return;
    MessageBeep(MB_ICONEXCLAMATION);
    ZeroMemory(&fw, sizeof(fw));
    fw.cbSize = sizeof(fw);
    fw.hwnd = hWnd;
    fw.dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG;
    fw.uCount = 4;
    FlashWindowEx(&fw);
}

static int HealthRankUi(DRIVE_HEALTH_STATUS e)
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

static int SerialEq(const char* a, const char* b)
{
    char aa[24], bb[24];
    int i;
    if (!a || !b || !a[0] || !b[0]) return 0;
    lstrcpynA(aa, a, (int)sizeof(aa));
    lstrcpynA(bb, b, (int)sizeof(bb));
    for (i = lstrlenA(aa); i > 0 && (aa[i - 1] == ' ' || aa[i - 1] == '\t'); i--)
        aa[i - 1] = '\0';
    for (i = lstrlenA(bb); i > 0 && (bb[i - 1] == ' ' || bb[i - 1] == '\t'); i--)
        bb[i - 1] = '\0';
    if (!aa[0] || !bb[0]) return 0;
    for (i = 0; aa[i] && bb[i]; i++) {
        unsigned char ca = (unsigned char)aa[i];
        unsigned char cb = (unsigned char)bb[i];
        if (ca >= 'a' && ca <= 'z') ca = (unsigned char)(ca - ('a' - 'A'));
        if (cb >= 'a' && cb <= 'z') cb = (unsigned char)(cb - ('a' - 'A'));
        if (ca != cb) return 0;
    }
    return aa[i] == '\0' && bb[i] == '\0';
}

static void ShortModel(const DRIVE_INFO* p, char* dst, int nDst)
{
    const char* s;
    int i, n = 0;
    if (!dst || nDst <= 0) return;
    s = (p && p->szModel[0]) ? p->szModel : TN("диск", "drive");
    for (i = 0; s[i] && n < nDst - 1 && n < 22; i++) {
        if (s[i] == ' ' && n > 0 && dst[n - 1] == ' ') continue;
        dst[n++] = s[i];
    }
    while (n > 0 && dst[n - 1] == ' ') n--;
    dst[n] = '\0';
    if (!dst[0])
        lstrcpynA(dst, TN("диск", "drive"), nDst);
}

static void AddBit(char* bits, int nBits, const char* s)
{
    int n;
    if (!bits || nBits <= 0 || !s || !s[0]) return;
    n = lstrlenA(bits);
    if (n > 0) {
        if (n + 2 >= nBits) return;
        bits[n++] = ',';
        bits[n++] = ' ';
        bits[n] = '\0';
    }
    if (n >= nBits) return;
    lstrcpynA(bits + n, s, nBits - n);
}

static void AddNote(char* buf, int nBuf, const char* s)
{
    int n;
    if (!buf || nBuf <= 0 || !s || !s[0]) return;
    n = lstrlenA(buf);
    if (n > 0) {
        if (n + 2 >= nBuf) return;
        buf[n++] = ';';
        buf[n++] = ' ';
        buf[n] = '\0';
    }
    if (n >= nBuf) return;
    lstrcpynA(buf + n, s, nBuf - n);
}

static void AddUp(char* bits, int nBits, const char* name, int prev, int now, int* beep)
{
    char tmp[48];
    if (prev < 0 || now < 0 || now <= prev) return;
    safe_snprintf(tmp, "%s +%d", name, now - prev);
    AddBit(bits, nBits, tmp);
    if (beep) *beep = 1;
}

static int FindPrevIndex(const DRIVE_INFO* now)
{
    int i;
    if (!now) return -1;
    if (now->szSerial[0]) {
        for (i = 0; i < g_nPrevCount; i++)
            if (SerialEq(g_PrevDrives[i].szSerial, now->szSerial))
                return i;
        return -1;
    }
    for (i = 0; i < g_nPrevCount; i++) {
        if (g_PrevDrives[i].szSerial[0]) continue;
        if (g_PrevDrives[i].nDriveIndex == now->nDriveIndex)
            return i;
    }
    return -1;
}

static void DiffOneDrive(char* buf, int nBuf, const DRIVE_INFO* prev,
                         const DRIVE_INFO* now, int* beep)
{
    char model[24];
    char bits[240];
    char line[300];
    char tmp[96];
    int rp, rn;

    if (!prev || !now) return;
    ShortModel(now, model, (int)sizeof(model));
    bits[0] = '\0';

    AddUp(bits, (int)sizeof(bits), TN("переназначено", "reallocated"),
          prev->nReallocated, now->nReallocated, beep);
    AddUp(bits, (int)sizeof(bits), TN("ожидающие", "pending"),
          prev->nPendingSectors, now->nPendingSectors, beep);
    AddUp(bits, (int)sizeof(bits), TN("неисправимые", "uncorrectable"),
          prev->nUncorrectable, now->nUncorrectable, beep);
    AddUp(bits, (int)sizeof(bits), TN("переназначения", "remap events"),
          prev->nRemapEvents, now->nRemapEvents, beep);
    AddUp(bits, (int)sizeof(bits), "CRC",
          prev->nCrcErrors, now->nCrcErrors, beep);

    if (prev->bIsNVMe && now->bIsNVMe &&
        now->qwNVMeMediaErrors > prev->qwNVMeMediaErrors) {
        safe_snprintf(tmp, TN("ошибки носителя +%llu", "media errors +%llu"),
                      (unsigned long long)(now->qwNVMeMediaErrors - prev->qwNVMeMediaErrors));
        AddBit(bits, (int)sizeof(bits), tmp);
        if (beep) *beep = 1;
    }
    if (prev->bIsNVMe && now->bIsNVMe &&
        (now->nvmeHealth.CriticalWarning & (BYTE)~prev->nvmeHealth.CriticalWarning) != 0) {
        AddBit(bits, (int)sizeof(bits),
               TN("критическое предупреждение", "critical warning"));
        if (beep) *beep = 1;
    }
    if (prev->bIsNVMe && now->bIsNVMe &&
        now->nvmeHealth.PercentageUsed > prev->nvmeHealth.PercentageUsed) {
        safe_snprintf(tmp, TN("износ +%d%%", "wear +%d%%"),
                      (int)now->nvmeHealth.PercentageUsed - (int)prev->nvmeHealth.PercentageUsed);
        AddBit(bits, (int)sizeof(bits), tmp);
    }
    if (prev->nEndurancePercent >= 0 && now->nEndurancePercent >= 0 &&
        now->nEndurancePercent < prev->nEndurancePercent) {
        safe_snprintf(tmp, TN("ресурс %d%%→%d%%", "life %d%%→%d%%"),
                      prev->nEndurancePercent, now->nEndurancePercent);
        AddBit(bits, (int)sizeof(bits), tmp);
    }
    if (prev->nTemperatureC > 0 && now->nTemperatureC > 0 &&
        prev->eTempBand != TEMP_BAND_UNKNOWN &&
        now->eTempBand != TEMP_BAND_UNKNOWN &&
        prev->eTempBand != now->eTempBand) {
        safe_snprintf(tmp, TN("температура %d→%d", "temperature %d→%d"),
                      prev->nTemperatureC, now->nTemperatureC);
        AddBit(bits, (int)sizeof(bits), tmp);
        if (beep && (int)now->eTempBand >= (int)TEMP_BAND_HIGH &&
            (int)now->eTempBand > (int)prev->eTempBand)
            *beep = 1;
    }

    rp = HealthRankUi(prev->eHealthStatus);
    rn = HealthRankUi(now->eHealthStatus);
    if (rp > 0 && rn > 0 && rp != rn) {
        safe_snprintf(tmp, TN("оценка %s→%s", "status %s→%s"),
                      GetHealthStatusName(prev->eHealthStatus),
                      GetHealthStatusName(now->eHealthStatus));
        AddBit(bits, (int)sizeof(bits), tmp);
        if (beep && rn > rp) *beep = 1;
    }

    if (!bits[0]) return;
    safe_snprintf(line, "%s: %s", model, bits);
    AddNote(buf, nBuf, line);
}

static void Snapshot_Diff(void)
{
    char buf[384];
    int seen[MAX_DRIVES];
    int i, beep = 0;

    buf[0] = '\0';
    for (i = 0; i < MAX_DRIVES; i++) seen[i] = 0;

    if (g_nPrevCount <= 0) {
        g_bMonitorAlert = 0;
        MonitorSetIdle();
        MonitorShow(g_hMainWnd);
        return;
    }

    for (i = 0; i < g_nDriveCount; i++) {
        int pi = FindPrevIndex(&g_Drives[i]);
        char model[24];
        char line[80];
        if (pi >= 0) {
            seen[pi] = 1;
            DiffOneDrive(buf, (int)sizeof(buf), &g_PrevDrives[pi], &g_Drives[i], &beep);
        } else {
            ShortModel(&g_Drives[i], model, (int)sizeof(model));
            safe_snprintf(line, TN("%s: появился", "%s: appeared"), model);
            AddNote(buf, (int)sizeof(buf), line);
        }
    }
    for (i = 0; i < g_nPrevCount; i++) {
        char model[24];
        char line[80];
        if (seen[i]) continue;
        ShortModel(&g_PrevDrives[i], model, (int)sizeof(model));
        safe_snprintf(line, TN("%s: пропал", "%s: removed"), model);
        AddNote(buf, (int)sizeof(buf), line);
        beep = 1;
    }

    if (!buf[0]) {
        g_bMonitorAlert = 0;
        if (g_bMonitorOn) {
            g_bMonitorQuiet = 1;
            lstrcpynA(g_szMonitor,
                      TN("Следит · без изменений", "Watching · no change"),
                      (int)sizeof(g_szMonitor));
        } else {
            MonitorSetIdle();
        }
    } else {
        g_bMonitorAlert = 1;
        g_bMonitorQuiet = 0;
        lstrcpynA(g_szMonitor, buf, (int)sizeof(g_szMonitor));
        if (beep) MonitorAlert(g_hMainWnd);
    }
    MonitorShow(g_hMainWnd);
}

static void DeviceNotify_Register(HWND hWnd)
{
    static const GUID GUID_DEVINTERFACE_DISK =
    { 0x53F56307, 0xB6BF, 0x11D0,
      { 0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B } };

    DEV_BROADCAST_DEVICEINTERFACE_A dbi;
    ZeroMemory(&dbi, sizeof(dbi));
    dbi.dbcc_size       = sizeof(dbi);
    dbi.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbi.dbcc_classguid  = GUID_DEVINTERFACE_DISK;

    g_hDevNotify = RegisterDeviceNotificationA(hWnd, &dbi, DEVICE_NOTIFY_WINDOW_HANDLE);
}

static void DeviceNotify_Unregister(void)
{
    if (g_hDevNotify) {
        UnregisterDeviceNotification(g_hDevNotify);
        g_hDevNotify = NULL;
    }
}

/* Tray disc: green = normal, orange = hot, red = overheating.
 * Saturated on purpose. The window palette yellow/orange pair is too close
 * at tray size, and a mask icon never shows that color in the Win10 tray. */
static COLORREF TrayTempColor(const DRIVE_INFO* p)
{
    if (!p || p->nTemperatureC <= 0)
        return RGB(90, 98, 110);
    if (p->eTempBand == TEMP_BAND_CRITICAL)
        return RGB(220, 32, 32);
    if (p->eTempBand == TEMP_BAND_HIGH || p->eTempBand == TEMP_BAND_ELEVATED)
        return RGB(255, 120, 0);
    if (p->eTempBand == TEMP_BAND_NORMAL)
        return RGB(16, 170, 64);
    if (p->nTemperatureC >= 70) return RGB(220, 32, 32);
    if (p->nTemperatureC >= 50) return RGB(255, 120, 0);
    return RGB(16, 170, 64);
}

/* GDI leaves the alpha byte at 0. The tray then keeps the old black mask. */
static void TrayFixAlpha(DWORD* pix, int n)
{
    int i;
    for (i = 0; i < n; i++) {
        DWORD c = pix[i];
        BYTE b = (BYTE)(c & 0xFFu);
        BYTE g = (BYTE)((c >> 8) & 0xFFu);
        BYTE r = (BYTE)((c >> 16) & 0xFFu);
        if (r < 12 && g < 12 && b < 12)
            pix[i] = 0;
        else
            pix[i] = 0xFF000000u | ((DWORD)r << 16) | ((DWORD)g << 8) | (DWORD)b;
    }
}

static HICON TrayMakeIcon(const DRIVE_INFO* p)
{
    int cx = GetSystemMetrics(SM_CXSMICON);
    int cy = GetSystemMetrics(SM_CYSMICON);
    BITMAPINFO bi;
    DWORD* pix = NULL;
    HDC hdcScreen, hdc = NULL;
    HBITMAP dib = NULL, oldBm = NULL, hbmMask = NULL;
    HICON hIcon = NULL;
    ICONINFO ii;
    WCHAR wz[8];
    int t, px, digits;
    RECT rc;
    COLORREF bg;

    if (cx < 32) cx = 32;
    if (cy < 32) cy = 32;
    if (cx > 64) cx = 64;
    if (cy > 64) cy = 64;

    t = (p && p->nTemperatureC > 0) ? p->nTemperatureC : -1;
    if (t < 0)
        lstrcpynW(wz, L"--", 8);
    else if (t > 999)
        lstrcpynW(wz, L"999", 8);
    else
        wsprintfW(wz, L"%d", t);
    digits = lstrlenW(wz);
    bg = TrayTempColor(p);

    ZeroMemory(&bi, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cx;
    bi.bmiHeader.biHeight = -cy;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    hdcScreen = GetDC(NULL);
    if (!hdcScreen) return NULL;
    hdc = CreateCompatibleDC(hdcScreen);
    dib = CreateDIBSection(hdcScreen, &bi, DIB_RGB_COLORS, (void**)&pix, NULL, 0);
    ReleaseDC(NULL, hdcScreen);
    if (!hdc || !dib || !pix) {
        if (dib) DeleteObject(dib);
        if (hdc) DeleteDC(hdc);
        return NULL;
    }

    oldBm = (HBITMAP)SelectObject(hdc, dib);
    {
        HBRUSH br = CreateSolidBrush(bg);
        HPEN pen = CreatePen(PS_SOLID, 1, bg);
        HGDIOBJ oldBr = SelectObject(hdc, br);
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        Ellipse(hdc, 0, 0, cx, cy);
        SelectObject(hdc, oldBr);
        SelectObject(hdc, oldPen);
        DeleteObject(br);
        DeleteObject(pen);
    }

    px = (digits >= 3) ? (cy * 2 / 5) : (cy * 11 / 20);
    if (px < 10) px = 10;
    {
        HFONT font = CreateFontW(-px, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 NONANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe UI");
        HFONT oldFont = (HFONT)SelectObject(hdc, font);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(255, 255, 255));
        rc.left = 0;
        rc.top = 0;
        rc.right = cx;
        rc.bottom = cy;
        DrawTextW(hdc, wz, -1, &rc,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(hdc, oldFont);
        DeleteObject(font);
    }
    SelectObject(hdc, oldBm);
    DeleteDC(hdc);
    hdc = NULL;

    TrayFixAlpha(pix, cx * cy);

    hbmMask = CreateBitmap(cx, cy, 1, 1, NULL);
    if (hbmMask) {
        HDC hdcMask = CreateCompatibleDC(NULL);
        if (hdcMask) {
            HGDIOBJ old = SelectObject(hdcMask, hbmMask);
            PatBlt(hdcMask, 0, 0, cx, cy, BLACKNESS);
            SelectObject(hdcMask, old);
            DeleteDC(hdcMask);
        }
    }

    ZeroMemory(&ii, sizeof(ii));
    ii.fIcon = TRUE;
    ii.hbmMask = hbmMask;
    ii.hbmColor = dib;
    hIcon = CreateIconIndirect(&ii);
    if (hbmMask) DeleteObject(hbmMask);
    DeleteObject(dib);
    return hIcon;
}

static BOOL TraySet(HWND hWnd, int index, BOOL add)
{
    NOTIFYICONDATAW nid;
    HICON hIcon;
    char tip[160];
    const DRIVE_INFO* p;
    BOOL ok;
    const char* model;

    if (!hWnd || index < 0 || index >= g_nDriveCount) return FALSE;
    p = &g_Drives[index];
    hIcon = TrayMakeIcon(p);
    if (!hIcon) return FALSE;

    model = p->szModel[0] ? p->szModel : TN("диск", "drive");
    if (p->nTemperatureC > 0)
        safe_snprintf(tip, "%s · %d \xC2\xB0""C", model, p->nTemperatureC);
    else
        safe_snprintf(tip, "%s · %s", model,
                      TN("нет температуры", "no temperature"));

    ZeroMemory(&nid, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd = hWnd;
    nid.uID = (UINT)(index + 1);
    nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hIcon;
    U8ToW(tip, nid.szTip, (int)(sizeof(nid.szTip) / sizeof(nid.szTip[0])));
    ok = Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &nid);
    if (!ok && !add)
        ok = Shell_NotifyIconW(NIM_ADD, &nid);
    DestroyIcon(hIcon);
    return ok;
}

static void TrayRemoveFrom(HWND hWnd, int first)
{
    int i;
    if (first < 0) first = 0;
    for (i = first; i < g_nTrayIcons && i < MAX_DRIVES; i++) {
        NOTIFYICONDATAW nid;
        ZeroMemory(&nid, sizeof(nid));
        nid.cbSize = sizeof(nid);
        nid.hWnd = hWnd;
        nid.uID = (UINT)(i + 1);
        Shell_NotifyIconW(NIM_DELETE, &nid);
    }
    if (first < g_nTrayIcons)
        g_nTrayIcons = first;
}

static void TrayUpdate(HWND hWnd)
{
    int i, n;
    if (!hWnd) return;
    if (!g_bMonitorOn) {
        TrayRemoveFrom(hWnd, 0);
        return;
    }
    n = g_nDriveCount;
    if (n < 0) n = 0;
    if (n > MAX_DRIVES) n = MAX_DRIVES;
    /* Delete and add again. NIM_MODIFY keeps the previous mask icon. */
    TrayRemoveFrom(hWnd, 0);
    for (i = 0; i < n; i++)
        TraySet(hWnd, i, TRUE);
    g_nTrayIcons = n;
}

static void TrayOpenDrive(HWND hWnd, int index)
{
    int i;
    if (IsIconic(hWnd))
        ShowWindow(hWnd, SW_RESTORE);
    else
        ShowWindow(hWnd, SW_SHOW);
    SetForegroundWindow(hWnd);
    if (index < 0 || index >= g_nDriveCount) return;
    g_nSelectedDrive = index;
    for (i = 0; i < g_nDriveCount; i++)
        if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
    UpdateDriveInfo(hWnd, index);
    UpdateAttrList(hWnd, index);
    RepaintHealthBar();
    InvalidateRect(hWnd, NULL, FALSE);
}

static void TrayOnClick(HWND hWnd, int index, UINT mouseMsg)
{
    if (mouseMsg == WM_LBUTTONUP || mouseMsg == WM_LBUTTONDBLCLK ||
        mouseMsg == NIN_SELECT) {
        TrayOpenDrive(hWnd, index);
        return;
    }
    if (mouseMsg != WM_RBUTTONUP && mouseMsg != WM_CONTEXTMENU)
        return;
    {
        HMENU menu = CreatePopupMenu();
        POINT pt;
        int cmd;
        if (!menu) return;
        AppendMenuU8(menu, MF_STRING, 1, TN("Открыть", "Open"));
        AppendMenuU8(menu, MF_STRING, 2, Tr(STR_EXIT));
        GetCursorPos(&pt);
        SetForegroundWindow(hWnd);
        cmd = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                             pt.x, pt.y, 0, hWnd, NULL);
        DestroyMenu(menu);
        PostMessage(hWnd, WM_NULL, 0, 0);
        if (cmd == 1)
            TrayOpenDrive(hWnd, index);
        else if (cmd == 2)
            DestroyWindow(hWnd);
    }
}

void DestroyGDIObjects(void){
    if (g_hbrBG)     { DeleteObject(g_hbrBG);     g_hbrBG     = NULL; }
    if (g_hbrPanel)  { DeleteObject(g_hbrPanel);  g_hbrPanel  = NULL; }
    if (g_hbrGreen)  { DeleteObject(g_hbrGreen);  g_hbrGreen  = NULL; }
    if (g_hbrYellow) { DeleteObject(g_hbrYellow); g_hbrYellow = NULL; }
    if (g_hbrRed)    { DeleteObject(g_hbrRed);    g_hbrRed    = NULL; }
    if (g_hFontTitle)  { DeleteObject(g_hFontTitle);  g_hFontTitle  = NULL; }
    if (g_hFontNormal) { DeleteObject(g_hFontNormal); g_hFontNormal = NULL; }
    if (g_hFontSmall)  { DeleteObject(g_hFontSmall);  g_hFontSmall  = NULL; }
    if (g_hFontBig)    { DeleteObject(g_hFontBig);    g_hFontBig    = NULL; }
}

COLORREF GetHealthStatusColor(DRIVE_HEALTH_STATUS eStatus)
{
    switch (eStatus) {
    case HEALTH_STATUS_GOOD:     return CLR_GREEN;
    case HEALTH_STATUS_OBSERVE:  return CLR_YELLOW;
    case HEALTH_STATUS_CAUTION:  return CLR_ORANGE;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:  return CLR_RED;
    case HEALTH_STATUS_CRITICAL: return CLR_CRITICAL;
    default:                     return CLR_ACCENT;
    }
}

static const char* GetAxisStatusName(DRIVE_HEALTH_STATUS eStatus, BOOL bTemp)
{
    if (bTemp && eStatus == HEALTH_STATUS_GOOD)
        return Tr(STR_AXIS_OK);
    if (bTemp && (eStatus == HEALTH_STATUS_OBSERVE || eStatus == HEALTH_STATUS_CAUTION))
        return Tr(STR_AXIS_ELEV);
    if (bTemp && (eStatus == HEALTH_STATUS_BAD || eStatus == HEALTH_STATUS_WARNING ||
                  eStatus == HEALTH_STATUS_CRITICAL))
        return Tr(STR_AXIS_CRIT);
    return GetHealthStatusName(eStatus);
}

LRESULT CALLBACK HealthBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);

            RECT rc;
            GetClientRect(hWnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;

            HDC     hdc    = CreateCompatibleDC(hdcReal);
            HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
            HBITMAP hbmOldBuf = (HBITMAP)SelectObject(hdc, hbmBuf);

            DRIVE_HEALTH_STATUS eOurs = HEALTH_STATUS_UNKNOWN;
            DRIVE_HEALTH_STATUS eDisk = HEALTH_STATUS_UNKNOWN;
            BOOL bHaveDrive = (g_nDriveCount > 0 && g_nSelectedDrive >= 0 &&
                               g_nSelectedDrive < g_nDriveCount);
            if (bHaveDrive) {
                eOurs = g_Drives[g_nSelectedDrive].eHealthStatus;
                eDisk = g_Drives[g_nSelectedDrive].eDiskStatus;
            }

            {
                HBRUSH hbrFill = CreateSolidBrush(GetHealthStatusColor(eOurs));
                FillRect(hdc, &rc, hbrFill);
                DeleteObject(hbrFill);
            }

            {
                HPEN   hpBorder = CreatePen(PS_SOLID, 1, CLR_BORDER);
                HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                HBRUSH hbOld    = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
                Rectangle(hdc, 0, 0, w, h);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBorder);
            }

            {
                char szDisk[64], szOurs[64];
                HFONT hUseFont = g_hFontTitle ? g_hFontTitle :
                    (g_hFontNormal ? g_hFontNormal : (HFONT)GetStockObject(DEFAULT_GUI_FONT));
                HFONT hOldFont = (HFONT)SelectObject(hdc, hUseFont);
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, RGB(255, 255, 255));
                if (bHaveDrive)
                    safe_snprintf(szDisk, Tr(STR_DISK_COLON),
                        GetDiskStatusName(&g_Drives[g_nSelectedDrive]));
                else
                    safe_snprintf(szDisk, Tr(STR_DISK_COLON), GetHealthStatusNameShort(eDisk));
                safe_snprintf(szOurs, Tr(STR_ASSESS_COLON), GetHealthStatusNameShort(eOurs));
                {
                    RECT rc1 = { UiScale(4), UiScale(2), w - UiScale(4), h / 2 };
                    RECT rc2 = { UiScale(4), h / 2 - 1, w - UiScale(4), h - 2 };
                    DrawTextU8(hdc, szDisk, &rc1, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    DrawTextU8(hdc, szOurs, &rc2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                SelectObject(hdc, hOldFont);
                if (GetFocus() == hWnd)
                    DrawFocusRect(hdc, &rc);
            }

            BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, hbmOldBuf);
            DeleteObject(hbmBuf);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        return 0;

    case WM_LBUTTONUP:
        {
            HWND hParent = GetParent(hWnd);
            if (hParent)
                ShowHealthLectureDialog(hParent);
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_RETURN || wParam == VK_SPACE) {
            HWND hParent = GetParent(hWnd);
            if (hParent)
                ShowHealthLectureDialog(hParent);
            return 0;
        }
        break;

    case WM_GETDLGCODE:
        return DLGC_BUTTON | DLGC_WANTCHARS;

    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, (LPCTSTR)IDC_HAND));
        return TRUE;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK DriveBtnWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_PAINT:
        {
            int nIdx = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);

            int w = rc.right  - rc.left;
            int h = rc.bottom - rc.top;

            HDC     hdc    = CreateCompatibleDC(hdcReal);
            HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
            HBITMAP hbmOldBuf = (HBITMAP)SelectObject(hdc, hbmBuf);

            RECT rcBuf = { 0, 0, w, h };

            BOOL bSelected = (nIdx == g_nSelectedDrive);
            BOOL bHover    = (GetPropA(hWnd, "hover") != NULL);

            COLORREF clrFill, clrBorder, clrText;
            if (bSelected) {
                clrFill   = CLR_HEADER;
                clrBorder = CLR_ACCENT;
                clrText   = CLR_TEXT;
            } else if (bHover) {
                clrFill   = CLR_ROW2;
                clrBorder = RGB(150, 180, 220);
                clrText   = CLR_TEXT;
            } else {
                clrFill   = CLR_PANEL;
                clrBorder = CLR_BORDER;
                clrText   = CLR_TEXT;
            }

            {
                HBRUSH hbrFill = CreateSolidBrush(clrFill);
                FillRect(hdc, &rcBuf, hbrFill);
                DeleteObject(hbrFill);
            }

            {
                HPEN   hpBord = CreatePen(PS_SOLID, 1, clrBorder);
                HPEN   hpOld  = (HPEN)SelectObject(hdc, hpBord);
                HBRUSH hbOld  = (HBRUSH)SelectObject(hdc, (HBRUSH)GetStockObject(NULL_BRUSH));
                Rectangle(hdc, rcBuf.left, rcBuf.top, rcBuf.right, rcBuf.bottom);
                SelectObject(hdc, hpOld);
                SelectObject(hdc, hbOld);
                DeleteObject(hpBord);
            }

            SetBkMode(hdc, TRANSPARENT);

            if (nIdx >= 0 && nIdx < g_nDriveCount) {
                DRIVE_INFO* pD = &g_Drives[nIdx];

                char szName[64];
                if (strlen(pD->szModel) > 0)
                    safe_snprintf(szName, "%s", pD->szModel);
                else
                    safe_snprintf(szName, TN("Диск %d", "Drive %d"), pD->nDriveIndex);

                char szType[16];
                const char* szT = GetDriveTypeName(pD->eType);
                safe_snprintf(szType, "[%s]", szT ? szT : "?");

                char szHealth[48];
                if (pD->eHealthStatus == HEALTH_STATUS_UNKNOWN)
                    safe_snprintf(szHealth, "—");
                else
                    safe_snprintf(szHealth, "%s", GetHealthStatusNameShort(pD->eHealthStatus));

                char szCap[24];
                FormatSize(pD->dwCapacityMB, szCap, sizeof(szCap));

                char szTempStr[24];
                if (pD->nTemperatureC > 0)
                    safe_snprintf(szTempStr, "%d\xC2\xB0""C", pD->nTemperatureC);
                else
                    szTempStr[0] = '\0';

                COLORREF clrH = GetHealthStatusColor(pD->eHealthStatus);

                COLORREF clrTemp;
                if (pD->nTemperatureC <= 0)
                    clrTemp = CLR_TEXT_DIM;
                else if (pD->eTempBand == TEMP_BAND_NORMAL)
                    clrTemp = CLR_GREEN;
                else if (pD->eTempBand == TEMP_BAND_ELEVATED)
                    clrTemp = CLR_YELLOW;
                else if (pD->eTempBand == TEMP_BAND_HIGH ||
                         pD->eTempBand == TEMP_BAND_CRITICAL)
                    clrTemp = CLR_RED;
                else if (pD->nTemperatureC < 50)
                    clrTemp = CLR_GREEN;
                else if (pD->nTemperatureC < 60)
                    clrTemp = CLR_YELLOW;
                else
                    clrTemp = CLR_RED;

                HFONT hOldFont;

                hOldFont = (HFONT)SelectObject(hdc, g_hFontNormal);
                SetTextColor(hdc, clrText);
                RECT rcName = { rcBuf.left + UiScale(8), rcBuf.top + UiScale(4),
                                rcBuf.right - UiScale(8), rcBuf.top + UiScale(20) };
                DrawTextU8(hdc, szName, &rcName, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                SelectObject(hdc, g_hFontSmall);
                RECT rcType = { rcBuf.left + UiScale(8), rcBuf.top + UiScale(20),
                                (rcBuf.left + rcBuf.right) / 2, rcBuf.top + UiScale(36) };
                SetTextColor(hdc, CLR_TEXT_DIM);
                DrawTextU8(hdc, szType, &rcType, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

                RECT rcHealth = { (rcBuf.left + rcBuf.right) / 2, rcBuf.top + UiScale(20),
                                  rcBuf.right - UiScale(8), rcBuf.top + UiScale(36) };
                SetTextColor(hdc, clrH);
                DrawTextU8(hdc, szHealth, &rcHealth,
                    DT_RIGHT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

                {
                    RECT rcCap  = { rcBuf.left + UiScale(8), rcBuf.bottom - UiScale(17),
                                    (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - UiScale(4) };
                    RECT rcTmp  = { (rcBuf.left + rcBuf.right) / 2, rcBuf.bottom - UiScale(17),
                                    rcBuf.right - UiScale(8), rcBuf.bottom - UiScale(4) };
                    SetTextColor(hdc, clrText);
                    DrawTextU8(hdc, szCap, &rcCap, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                    if (szTempStr[0]) {
                        SetTextColor(hdc, clrTemp);
                        DrawTextU8(hdc, szTempStr, &rcTmp, DT_RIGHT | DT_SINGLELINE | DT_VCENTER);
                    }
                }
                SelectObject(hdc, hOldFont);
            }

            BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);

            SelectObject(hdc, hbmOldBuf);
            DeleteObject(hbmBuf);
            DeleteDC(hdc);

            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_MOUSEMOVE:
        if (!GetPropA(hWnd, "hover")) {
            SetPropA(hWnd, "hover", (HANDLE)1);
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hWnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hWnd, NULL, TRUE);
        }
        return 0;

    case WM_MOUSELEAVE:
        RemovePropA(hWnd, "hover");
        InvalidateRect(hWnd, NULL, TRUE);
        return 0;

    case WM_LBUTTONUP:
        {
            int nIdx = (int)GetWindowLongPtrA(hWnd, GWLP_USERDATA);
            if (nIdx >= 0 && nIdx < g_nDriveCount) {
                HWND hParent = GetParent(hWnd);
                g_nSelectedDrive = nIdx;
                int i;
                for (i = 0; i < g_nDriveCount; i++)
                    if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
                UpdateDriveInfo(hParent, nIdx);
                UpdateAttrList(hParent, nIdx);
                InvalidateRect(hParent, NULL, FALSE);
                UpdateWindow(hParent);
            }
        }
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursor(NULL, (LPCTSTR)IDC_HAND));
        return TRUE;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

void RegisterHealthBarClass(HINSTANCE hInst)
{
    WNDCLASSW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = HealthBarWndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_HAND);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"LLHDHealthBar";
    RegisterClassW(&wc);

    ZeroMemory(&wc, sizeof(wc));
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = DriveBtnWndProc;
    wc.hInstance     = hInst;
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"LLHDDriveBtn";
    RegisterClassW(&wc);
}

void RepaintHealthBar(void)
{
    if (g_hHealthBar) { InvalidateRect(g_hHealthBar, NULL, TRUE); UpdateWindow(g_hHealthBar); }
}

void UpdateDriveButtons(HWND hWnd)
{
    int i;
    int nBtnW  = UiScale(DRIVE_BTN_PANEL_W - 12);
    int nBtnH  = UiScale(DRIVE_BTN_H);
    int nStartY = UiScale(40);

    BOOL bNeedRebuild = FALSE;

    if (g_hDriveBtn[0]) {
        char szClass[32] = "";
        GetClassNameA(g_hDriveBtn[0], szClass, sizeof(szClass));
        BOOL bIsStatic   = (strcmp(szClass, "Static") == 0 || strcmp(szClass, "static") == 0);
        BOOL bNeedStatic = (g_nDriveCount == 0);
        if (bIsStatic != bNeedStatic) bNeedRebuild = TRUE;
    }

    {
        int nExisting = 0;
        for (i = 0; i < MAX_DRIVES; i++)
            if (g_hDriveBtn[i]) nExisting++;
        if (nExisting != (g_nDriveCount == 0 ? 1 : g_nDriveCount))
            bNeedRebuild = TRUE;
    }

    if (bNeedRebuild) {
        for (i = 0; i < MAX_DRIVES; i++) {
            if (g_hDriveBtn[i]) {
                DestroyWindow(g_hDriveBtn[i]);
                g_hDriveBtn[i] = NULL;
            }
        }

        if (g_nDriveCount == 0) {
            HWND hPlaceholder = CreateWindowExU8(0, "STATIC", Tr(STR_NO_DRIVES),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(6), nStartY, nBtnW, nBtnH,
                hWnd, (HMENU)(IDC_DRIVE_BTN_BASE), g_hInst, NULL);
            SendMessage(hPlaceholder, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            g_hDriveBtn[0] = hPlaceholder;
        } else {

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            int nY = nStartY + i * (nBtnH + UiScale(DRIVE_BTN_GAP));
            g_hDriveBtn[i] = CreateWindowExU8(
                0, "LLHDDriveBtn", "",
                WS_CHILD | WS_VISIBLE,
                UiScale(6), nY, nBtnW, nBtnH,
                hWnd, (HMENU)(UINT_PTR)(IDC_DRIVE_BTN_BASE + i), g_hInst, NULL
            );
            SetWindowLongPtrA(g_hDriveBtn[i], GWLP_USERDATA, (LONG_PTR)i);
        }
        }
    } else {

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            if (g_hDriveBtn[i])
                InvalidateRect(g_hDriveBtn[i], NULL, FALSE);
        }
    }
    LayoutMainWindow(hWnd);
}


void UpdateDriveInfo(HWND hWnd, int nDriveIdx)
{
    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SetDlgItemTextU8(hWnd, IDC_MODEL_STATIC,       "-");
        SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC,       "—");
        SetDlgItemTextU8(hWnd, IDC_CONTROLLER_STATIC,  "—");
        SetDlgItemTextU8(hWnd, IDC_SERIAL_STATIC,      "-");
        SetDlgItemTextU8(hWnd, IDC_FIRMWARE_STATIC,    "-");
        SetDlgItemTextU8(hWnd, IDC_SIZE_STATIC,        "-");
        SetDlgItemTextU8(hWnd, IDC_TEMP_STATIC,        "-");
        SetDlgItemTextU8(hWnd, IDC_POH_STATIC,         "-");
        SetDlgItemTextU8(hWnd, IDC_STATUS_STATIC,      Tr(STR_NO_DATA));
        SetDlgItemTextU8(hWnd, IDC_PROTOCOL_STATIC,  "-");  /* protocol */
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC,     "—");

        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC,     "");
        SetDlgItemTextU8(hWnd, IDC_AXIS_MEDIA_V,       "");
        SetDlgItemTextU8(hWnd, IDC_AXIS_IFACE_V,       "");
        SetDlgItemTextU8(hWnd, IDC_AXIS_TEMPA_V,       "");
        SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_L,        Tr(STR_WEAR));
        SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_V,        "");
        SetActionButtonsEnabled(hWnd, TRUE);
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];
    char szBuf[256];

    SetDlgItemTextU8(hWnd, IDC_MODEL_STATIC,
                    strlen(pInfo->szModel) ? pInfo->szModel : "-");

    {
        const char* brand = GetVendorName(pInfo->eVendor);
        if (pInfo->eVendor == VENDOR_UNKNOWN || pInfo->eVendor == VENDOR_OTHER)
            brand = "—";
        if (pInfo->bCounterfeitSuspected) {
            char szBrandAlert[128];
            const char* szBadge = GetCounterfeitBadge(pInfo);
            safe_snprintf(szBrandAlert, "%s %s", brand, szBadge);
            SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC, szBrandAlert);
        } else {
            SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC, brand);
        }
    }
    SetDlgItemTextU8(hWnd, IDC_CONTROLLER_STATIC, DriveControllerLabel(pInfo));

    SetDlgItemTextU8(hWnd, IDC_SERIAL_STATIC,
                    strlen(pInfo->szSerial) ? pInfo->szSerial : "-");

    SetDlgItemTextU8(hWnd, IDC_FIRMWARE_STATIC,
                    strlen(pInfo->szFirmware) ? pInfo->szFirmware : "-");

    {
        char szSize[32];
        FormatSize(pInfo->dwCapacityMB, szSize, sizeof(szSize));
        if (pInfo->trimInfo.bQueried && (pInfo->bIsNVMe || pInfo->eType == DRIVE_TYPE_SSD_SATA || pInfo->eType == DRIVE_TYPE_M2_SATA)) {
            char szTrimBrief[96];
            if (!pInfo->trimInfo.bOsTrimEnabled) {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: ВЫКЛЮЧЕН В ОС ⚠️", "TRIM: DISABLED IN OS ⚠️"));
            } else if (pInfo->trimInfo.bPanicLowSpace) {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: ОК · <5% своб. ⚠️", "TRIM: OK · <5% free ⚠️"));
            } else if (pInfo->trimInfo.bCriticallyLowSpace) {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: ОК · <7% своб. !", "TRIM: OK · <7% free !"));
            } else if (pInfo->trimInfo.bHasNonTrimVolume) {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: тома без TRIM (FAT) ⚠️", "TRIM: non-TRIM vols (FAT) ⚠️"));
            } else if (pInfo->trimInfo.nVolumeCount > 0 && pInfo->trimInfo.fMinFreePercent >= 0.0f) {
                safe_snprintf(szTrimBrief, TN("TRIM: активен (своб. %.0f%%)", "TRIM: active (%.0f%% free)"),
                              (double)pInfo->trimInfo.fMinFreePercent);
            } else if (pInfo->trimInfo.nVolumeCount == 0) {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: активен (тома не прочитаны)", "TRIM: active (volumes not read)"));
            } else {
                safe_snprintf(szTrimBrief, "%s", TN("TRIM: активен", "TRIM: active"));
            }
            safe_snprintf(szBuf, TN("%s · %s · %s", "%s · %s · %s"),
                          szSize, GetDriveTypeName(pInfo->eType), szTrimBrief);
        } else {
            safe_snprintf(szBuf, TN("%s · %s", "%s · %s"),
                          szSize, GetDriveTypeName(pInfo->eType));
        }


        SetDlgItemTextU8(hWnd, IDC_SIZE_STATIC, szBuf);
    }

    if (pInfo->nTemperatureC > 0) {
        char szBand[32];
        if (pInfo->eTempBand != TEMP_BAND_UNKNOWN)
            lstrcpynA(szBand, GetTempBandName(pInfo->eTempBand, TRUE), (int)sizeof(szBand));
        else
            szBand[0] = '\0';
        if (pInfo->nTempCritC > 0 && pInfo->nTempMaxC > 0)
            safe_snprintf(szBuf, TN("%d\xC2\xB0""C · %s  макс.%d  крит.%d",
                                    "%d\xC2\xB0""C · %s  max %d  crit %d"),
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempMaxC, pInfo->nTempCritC);
        else if (pInfo->nTempWarnC > 0 && pInfo->nTempCritC > 0)
            safe_snprintf(szBuf, TN("%d\xC2\xB0""C · %s  пред.%d  крит.%d",
                                    "%d\xC2\xB0""C · %s  warn %d  crit %d"),
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempWarnC, pInfo->nTempCritC);
        else if (pInfo->nTempCritC > 0)
            safe_snprintf(szBuf, TN("%d\xC2\xB0""C · %s  крит.%d",
                                    "%d\xC2\xB0""C · %s  crit %d"),
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempCritC);
        else if (pInfo->nTempMaxC > 0)
            safe_snprintf(szBuf, TN("%d\xC2\xB0""C · %s  макс.%d",
                                    "%d\xC2\xB0""C · %s  max %d"),
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempMaxC);
        else if (szBand[0])
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s",
                          pInfo->nTemperatureC, szBand);
        else
            safe_snprintf(szBuf, "%d\xC2\xB0""C", pInfo->nTemperatureC);
    } else {
        safe_snprintf(szBuf, "-");
    }
    SetDlgItemTextU8(hWnd, IDC_TEMP_STATIC, szBuf);

    {
        char szPoh[64];
        FormatPowerOnHours(pInfo->dwPowerOnHours, szPoh, (int)sizeof(szPoh));
        SetDlgItemTextU8(hWnd, IDC_POH_STATIC, szPoh);
    }

    if (SmartSnapshotRejected(pInfo)) {
        safe_snprintf(szBuf, "%s", TN("Ошибка данных SMART", "SMART data error"));
    } else if (!pInfo->bSMART_Supported) {
        if (pInfo->bIsNVMe)
            safe_snprintf(szBuf, TN("NVMe Health Log ошибка (err %lu)",
                                    "NVMe Health Log error (err %lu)"),
                (unsigned long)pInfo->dwErrNvmeProtocol);
        else if (pInfo->bIsUSB) {
            if (IsLikelyUsbFlashDrive(pInfo))
                safe_snprintf(szBuf, "%s", TN("SMART недоступен (USB-флешка)",
                                              "SMART unavailable (USB flash)"));
            else
                safe_snprintf(szBuf, "%s", TN("SMART недоступен (USB-мост)",
                                              "SMART unavailable (USB bridge)"));
        } else {
            safe_snprintf(szBuf, "%s", TN("Не поддерживается", "Not supported"));
        }
    } else {
        FormatSmartHeadline(pInfo, szBuf, sizeof(szBuf));
    }
    SetDlgItemTextU8(hWnd, IDC_STATUS_STATIC, szBuf);

    SetDlgItemTextU8(hWnd, IDC_PROTOCOL_STATIC,
                    pInfo->szProtocol[0] ? pInfo->szProtocol : "-");

    if (pInfo->bIsUSB) {
        char szAdapter[64];
        FormatUsbAdapterName(pInfo, szAdapter, sizeof(szAdapter));
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC, szAdapter);
    } else {
        SetDlgItemTextU8(hWnd, IDC_ADAPTER_STATIC, "—");
    }

    if (SmartSnapshotRejected(pInfo)) {
        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, TN(
            "Пакет SMART повреждён или обрезан. Здоровье не оценивается.",
            "The SMART packet is damaged or truncated. Health is not assessed."));
    } else if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        char szBridge[80], szName[64];
        FormatUsbAdapterName(pInfo, szName, sizeof(szName));
        if (szName[0] && strcmp(szName, "—") != 0)
            safe_snprintf(szBridge, " [%s]", szName);
        else
            szBridge[0] = '\0';

        DWORD dwErr = pInfo->dwErrSat16;
        if (dwErr == 0) dwErr = pInfo->dwErrSat12;
        if (dwErr == 0) dwErr = pInfo->dwErrStorageProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrNvmeProtocol;
        if (dwErr == 0) dwErr = pInfo->dwErrLogSense;

        if (IsLikelyUsbFlashDrive(pInfo)) {
            if (dwErr != 0)
                safe_snprintf(szBuf, TN(
                    "У этой USB-флешки SMART не отдаётся — так у большинства флешек. "
                    "Код ошибки: %lu.",
                    "This USB flash drive does not provide SMART — most flash drives do not. "
                    "Error code: %lu."),
                    (unsigned long)dwErr);
            else
                safe_snprintf(szBuf, "%s", TN(
                    "У этой USB-флешки SMART не отдаётся — так у большинства флешек.",
                    "This USB flash drive does not provide SMART — most flash drives do not."));
        } else {
            if (dwErr != 0)
                safe_snprintf(szBuf, TN(
                    "USB-корпус/мост не отдаёт SMART%s. Код ошибки: %lu.",
                    "The USB enclosure does not provide SMART%s. Error code: %lu."),
                    szBridge, (unsigned long)dwErr);
            else
                safe_snprintf(szBuf, TN(
                    "USB-корпус/мост не отдаёт SMART%s "
                    "(SAT/vendor passthrough недоступен).",
                    "The USB enclosure does not provide SMART%s "
                    "(SAT/vendor passthrough unavailable)."),
                    szBridge);
        }
        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szBuf);
    } else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        {
            char szErr[320];
            safe_snprintf(szErr, TN(
                "NVMe Health Log IOCTL не удался (Win32 %lu). 5=нет прав, 1/50=не поддерживается, 87=плохой запрос.",
                "NVMe Health Log IOCTL failed (Win32 %lu). 5=access denied, 1/50=not supported, 87=bad request."),
                (unsigned long)pInfo->dwErrNvmeProtocol);
            SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szErr);
        }
    } else if (!pInfo->bSMART_Supported) {
        SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, TN(
            "Не удалось прочитать SMART. Запустите от имени администратора и нажмите «Перечитать».",
            "Could not read SMART. Run as administrator and click Reread."));
    } else {
        /* SMART present: prompt to open the lecture. No NVMe field dump, no %. */
        {
            char szObs[256];
            const char* szPred = TN(
                "Нажмите на состояние, чтобы узнать подробности.",
                "Click the status for details.");
            switch (pInfo->eHealthStatus) {
            case HEALTH_STATUS_GOOD:
                szPred = TN("Критических проблем не обнаружено.",
                            "No critical problems found.");
                break;
            case HEALTH_STATUS_OBSERVE:
                if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
                    FormatHddObservePrompt(pInfo, szObs, (int)sizeof(szObs));
                    szPred = szObs;
                } else
                    szPred = TN("Есть факторы риска. Нажмите на состояние.",
                                "There are risk factors. Click the status for details.");
                break;
            case HEALTH_STATUS_CAUTION:
                if (pInfo->eType == DRIVE_TYPE_HDD && !pInfo->bIsNVMe) {
                    FormatHddCautionPrompt(pInfo, szObs, (int)sizeof(szObs));
                    szPred = szObs;
                } else {
                    FormatSsdCautionPrompt(pInfo, szObs, (int)sizeof(szObs));
                    szPred = szObs;
                }
                break;
            case HEALTH_STATUS_BAD:
            case HEALTH_STATUS_WARNING:
                szPred = TN("Обнаружены признаки деградации носителя. Сделайте резервную копию.",
                            "Media degradation found. Make a backup.");
                break;
            case HEALTH_STATUS_CRITICAL:
                szPred = TN("Высокий риск отказа. Немедленно копируйте данные.",
                            "High risk of failure. Copy the data now.");
                break;
            default:
                szPred = TN("Недостаточно данных для достоверной оценки.",
                            "Not enough data for a reliable assessment.");
                break;
            }
            SetDlgItemTextU8(hWnd, IDC_PREDICT_STATIC, szPred);
        }
    }

    {
        char szRow4Buf[48];
        const char* szTempAx;
        const char* szRow4Val;
        DRIVE_HEALTH_STATUS shownWear;
        if (pInfo->nTemperatureC <= 0)
            szTempAx = Tr(STR_POH_NONE);
        else
            szTempAx = GetAxisStatusName(pInfo->eTempStatus, TRUE);
        if (pInfo->eType == DRIVE_TYPE_HDD || pInfo->nGSenseEvents >= 0) {
            SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_L, Tr(STR_MECHANICS));
            if (pInfo->eMechanics == HEALTH_STATUS_GOOD)
                szRow4Val = Tr(STR_AXIS_OK);
            else if (pInfo->eMechanics == HEALTH_STATUS_UNKNOWN)
                szRow4Val = Tr(STR_POH_NONE);
            else
                szRow4Val = GetHealthStatusName(pInfo->eMechanics);
        } else {
            /* eWear stays BAD at 5% or less. The axis stops at CAUTION,
             * the same cap as the headline. Do not invent a percent. */
            SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_L, Tr(STR_WEAR));
            shownWear = pInfo->eWear;
            if (shownWear == HEALTH_STATUS_BAD ||
                shownWear == HEALTH_STATUS_WARNING ||
                shownWear == HEALTH_STATUS_CRITICAL)
                shownWear = HEALTH_STATUS_CAUTION;
            if (pInfo->eWear != HEALTH_STATUS_UNKNOWN && pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%s (%d%%)",
                    GetHealthStatusName(shownWear),
                    pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else if (pInfo->eWear != HEALTH_STATUS_UNKNOWN) {
                szRow4Val = GetHealthStatusName(shownWear);
            } else if (pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%d%%", pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else {
                szRow4Val = TN("нет данных", "no data");
            }
        }
        SetDlgItemTextU8(hWnd, IDC_AXIS_MEDIA_V, GetAxisStatusName(pInfo->eReliability, FALSE));
        SetDlgItemTextU8(hWnd, IDC_AXIS_IFACE_V, GetAxisStatusName(pInfo->eInterface, FALSE));
        SetDlgItemTextU8(hWnd, IDC_AXIS_TEMPA_V, szTempAx);
        SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_V, szRow4Val);
        InvalidateRect(GetDlgItem(hWnd, IDC_AXIS_MEDIA_V), NULL, TRUE);
        InvalidateRect(GetDlgItem(hWnd, IDC_AXIS_IFACE_V), NULL, TRUE);
        InvalidateRect(GetDlgItem(hWnd, IDC_AXIS_TEMPA_V), NULL, TRUE);
        InvalidateRect(GetDlgItem(hWnd, IDC_AXIS_ROW4_V), NULL, TRUE);
    }

    SetActionButtonsEnabled(hWnd, TRUE);
    RepaintHealthBar();
}

static void ListViewSetCellIfChanged(HWND hList, int iItem, int iSubItem, const char* pszNew)
{
    WCHAR wzOld[128];
    WCHAR wzNew[128];
    LVITEMW lvi;
    wzOld[0] = 0;
    U8ToW(pszNew ? pszNew : "", wzNew, 128);
    ZeroMemory(&lvi, sizeof(lvi));
    lvi.mask       = LVIF_TEXT;
    lvi.iItem      = iItem;
    lvi.iSubItem   = iSubItem;
    lvi.pszText    = wzOld;
    lvi.cchTextMax = 128;
    SendMessageW(hList, LVM_GETITEMW, 0, (LPARAM)&lvi);
    if (wcscmp(wzOld, wzNew) != 0) {
        lvi.pszText = wzNew;
        SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
    }
}

typedef struct {
    char col[ATTR_NCOLS][128];
} ATTR_ROW;

#define MAX_ATTR_ROWS 48

static void FormatIdCol(char* dst, int nDst, const char* id)
{
    unsigned long v;
    char* end = NULL;
    const char* p;
    if (!dst || nDst <= 0) return;
    dst[0] = '\0';
    if (!id || !id[0] || id[0] == '-' || id[0] == 'T') {
        safe_snprintf_n(dst, nDst, "%s", id ? id : "--");
        return;
    }
    p = id;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2;
    v = strtoul(p, &end, 16);
    if (end && (*end == 'h' || *end == 'H') && end != p)
        safe_snprintf_n(dst, nDst, "%lu (%02lXh)", v, v);
    else {
        v = strtoul(id, &end, 10);
        if (end != id && *end == '\0')
            safe_snprintf_n(dst, nDst, "%lu (%02lXh)", v, v);
        else
            safe_snprintf_n(dst, nDst, "%s", id);
    }
}

static void AttrRowSet(ATTR_ROW* r,
                       const char* id, const char* name,
                       const char* val, const char* worst, const char* thresh,
                       const char* raw, const char* decoded, const char* stat)
{
    FormatIdCol(r->col[0], (int)sizeof(r->col[0]), id);
    safe_snprintf(r->col[1], "%s", name   ? name   : "");
    safe_snprintf(r->col[2], "%s", val    ? val    : "");
    safe_snprintf(r->col[3], "%s", worst  ? worst  : "");
    safe_snprintf(r->col[4], "%s", thresh ? thresh : "");
    safe_snprintf(r->col[ATTR_COL_RAW], "%s", raw ? raw : "");
    safe_snprintf(r->col[ATTR_COL_DECODE], "%s", decoded ? decoded : "");
    safe_snprintf(r->col[ATTR_COL_STATUS], "%s", TrStatus(stat ? stat : ""));
}

/* Status badge code stored in LVITEM.lParam. Low 8 bits = ATTRST_*.
 * Bit 8 (0x100) selects the alternate label: ПЛОХО vs СБОЙ, Жарко vs Внимание. */
enum {
    ATTRST_NONE = 0,
    ATTRST_OK,
    ATTRST_WARN,
    ATTRST_BAD,
    ATTRST_DIM,
    ATTRST_SKIP,
    ATTRST_INFO,
    ATTRST_POWERLOG,
    ATTRST_RISK,
    ATTRST_PAST
};

static LPARAM AttrStatusParam(const char* s)
{
    if (!s || !s[0])
        return (LPARAM)ATTRST_NONE;
    if (strcmp(s, "СБОЙ") == 0)
        return (LPARAM)ATTRST_BAD;
    if (strcmp(s, "ПЛОХО") == 0)
        return (LPARAM)(ATTRST_BAD | 0x100);
    if (strcmp(s, "Внимание") == 0)
        return (LPARAM)ATTRST_WARN;
    if (strcmp(s, "Жарко") == 0)
        return (LPARAM)(ATTRST_WARN | 0x100);
    if (strcmp(s, "ОК") == 0)
        return (LPARAM)ATTRST_OK;
    if (strcmp(s, "--") == 0)
        return (LPARAM)ATTRST_DIM;
    if (strcmp(s, "Не оценивается") == 0)
        return (LPARAM)ATTRST_SKIP;
    /* Power-log is not «контекст». Same blue badge, different label. */
    if (strcmp(s, "журнал питания") == 0 || strcmp(s, "power log") == 0)
        return (LPARAM)ATTRST_POWERLOG;
    if (strcmp(s, "INFO") == 0 || strcmp(s, "контекст") == 0 ||
        strcmp(s, "context") == 0)
        return (LPARAM)ATTRST_INFO;
    if (strcmp(s, "OK") == 0)
        return (LPARAM)ATTRST_OK;
    if (strcmp(s, "BAD") == 0)
        return (LPARAM)(ATTRST_BAD | 0x100);
    if (strcmp(s, "FAIL") == 0)
        return (LPARAM)ATTRST_BAD;
    if (strcmp(s, "Caution") == 0 || strcmp(s, "Watch") == 0)
        return (LPARAM)(strcmp(s, "Watch") == 0 ? ATTRST_RISK : ATTRST_WARN);
    if (strcmp(s, "Hot") == 0)
        return (LPARAM)(ATTRST_WARN | 0x100);
    if (strcmp(s, "Not scored") == 0)
        return (LPARAM)ATTRST_SKIP;
    if (strcmp(s, "past") == 0)
        return (LPARAM)ATTRST_PAST;
    if (strcmp(s, "Риск") == 0)
        return (LPARAM)ATTRST_RISK;
    if (strcmp(s, "было") == 0)
        return (LPARAM)ATTRST_PAST;
    return (LPARAM)ATTRST_NONE;
}

/* Phison and Samsung 231 are life remaining. Any other SSD uses 231 as a
 * percent only when the resolved profile says so (Apacer AS350 Lifetime_Left
 * via drivedb). SMI firmware without that semantic stays temperature. */
static BOOL E7IsLifePercent(const DRIVE_INFO* p)
{
    int sem = PROF_SEM_NONE;
    if (!p) return FALSE;
    return ProfileLookup(p, 0xE7, &sem, NULL) && sem == PROF_SEM_ENDURANCE;
}

/* Local profile first. drivedb display encoding covers names that are
 * not a health semantic (DriveDbLookup would skip those). */
static BOOL AttrEncOf(const DRIVE_INFO* p, BYTE id, int* sem, int* enc)
{
    if (sem) *sem = PROF_SEM_NONE;
    if (enc) *enc = PROF_ENC_NONE;
    if (!p) return FALSE;
    if (ProfileLookup(p, id, sem, enc)) return TRUE;
    return DriveDbAttrEnc(p, id, sem, enc);
}

static BOOL DbNotInUse(const DRIVE_INFO* p, BYTE id)
{
    const char* n = p ? DriveDbAttrName(p, id) : NULL;
    if (!n) return FALSE;
    return strstr(n, "Not In Use") != NULL || strstr(n, "not in use") != NULL;
}

/* Current °C from RAW, plus a lifetime max when the attribute actually stores one. */
static void FormatAtaTempValue(BYTE id, const BYTE* pRaw, BYTE bVal, BYTE bWorst,
                               char* szMain, int nMain)
{
    SMART_ATTRIBUTE a;
    int nC, nF, nMin, life, i;
    ZeroMemory(&a, sizeof(a));
    a.bAttrID = id;
    a.bAttrValue = bVal;
    a.bWorstValue = bWorst;
    for (i = 0; i < 6 && pRaw; i++)
        a.bRawValue[i] = pRaw[i];
    nC = (int)a.bRawValue[0];
    if (id == 0xC2) {
        int w16 = (int)a.bRawValue[0] | ((int)a.bRawValue[1] << 8);
        if (nC <= 0 || nC > 125) nC = w16;
        if (nC <= 0 || nC > 125) nC = (int)bVal;
    }
    nF = nC * 9 / 5 + 32;
    nMin = (int)a.bRawValue[2];
    life = AtaTempLifetimeMaxC(&a);
    if (nMin >= 1 && nMin <= 125 && life > nMin && life <= 125)
        safe_snprintf_n(szMain, nMain, TN(
            "%d \xC2\xB0""C (%d \xC2\xB0""F)  мин:%d макс:%d",
            "%d \xC2\xB0""C (%d \xC2\xB0""F)  min:%d max:%d"), nC, nF, nMin, life);
    else if (life > nC && life >= 1 && life <= 125)
        safe_snprintf_n(szMain, nMain, TN(
            "%d \xC2\xB0""C (%d \xC2\xB0""F)  макс:%d",
            "%d \xC2\xB0""C (%d \xC2\xB0""F)  max:%d"), nC, nF, life);
    else
        safe_snprintf_n(szMain, nMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", nC, nF);
}

static void FormatSmartValue(BYTE bID, BYTE* pRaw,
                              BYTE bVal, BYTE bWorst, BYTE bThresh,
                              const DRIVE_INFO* pDrv,
                              char* szBuf, int nBufLen)
{
    DRIVE_TYPE eType = pDrv ? pDrv->eType : DRIVE_TYPE_UNKNOWN;
    DRIVE_CONTROLLER eCtl = pDrv ? pDrv->eController : CONTROLLER_UNKNOWN;

    DWORD dw32 = GetRawValue(pRaw);
    WORD  w16  = (WORD)(dw32 & 0xFFFFu);
    unsigned __int64 qw48 = GetRawValue48(pRaw);

    char szMain[80] = "";

    {
        ATTR_DECODE dec;
        GetAttrDecode(bID, pDrv, &dec);
        /* PACKED and PAIR are not one counter. A known -v layout prints
         * its fields. Any other composite says the RAW is not decoded.
         * The id switch below does not choose that text. */
        if (FormatAttrRawText(pDrv, bID, pRaw, (unsigned)bVal, (unsigned)bWorst,
                              szMain, (int)sizeof(szMain))) {
            safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
            return;
        }
        if (dec.eState == ATTR_DECODE_UNKNOWN) {
            if (bID == 0xBB) {
                safe_snprintf(szMain, "(vendor-specific)");
                safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
                return;
            }
            if (qw48 > 0xFFFFFFFFULL)
                safe_snprintf(szMain, "%llu  (vendor-specific)",
                              (unsigned long long)qw48);
            else
                safe_snprintf(szMain, "%lu  (vendor-specific)",
                              (unsigned long)dw32);
            safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
            return;
        }
    }

    switch (bID)
    {

    case 0xBE:
    case 0xC2:
        FormatAtaTempValue(bID, pRaw, bVal, bWorst, szMain, (int)sizeof(szMain));
        break;
    case 0xE7:
    {
        int nLife = -1;
        if (bVal <= 100)
            nLife = (int)bVal;
        else if (dw32 <= 100)
            nLife = (int)dw32;
        else if (w16 <= 100)
            nLife = (int)w16;

        if (E7IsLifePercent(pDrv)) {
            /* PERCENT is the low 16 bits of RAW, the same figure the
             * lecture stores. Value can stay at 100 after RAW steps
             * (Apacer AS350: Value 100, RAW 99). VALUE keeps the
             * normalized byte: Samsung RAW is an erase count, Maxio RAW
             * is percent used. When Value and RAW disagree and the
             * headline did not take this RAW, keep Value: Phison then
             * reports life from A9. */
            int enc7 = PROF_ENC_NONE;
            AttrEncOf(pDrv, bID, NULL, &enc7);
            if (pDrv && enc7 != PROF_ENC_VALUE && w16 <= 100) {
                int raw16 = (int)w16;
                int endurance = pDrv->nEndurancePercent;
                BOOL disagree = (bVal <= 100 && (int)bVal != raw16);
                if (!disagree || endurance < 0 || endurance == raw16)
                    nLife = raw16;
            }
            if (nLife >= 0)
                safe_snprintf(szMain, TN("%d%% остаток ресурса", "%d%% life remaining"), nLife);
            else
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            break;
        }
        if (bVal > 0 && bVal <= 100) {
            int nF = (int)bVal * 9 / 5 + 32;
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", (int)bVal, nF);
        } else {
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        }
        break;
    }

    case 0x09:
    {
        unsigned __int64 nHours = qw48;
        if (nHours > 200000) nHours = (unsigned __int64)w16;
        if (nHours > 0xFFFFFFFFULL) nHours = 0xFFFFFFFFULL;
        FormatPowerOnHours((DWORD)nHours, szMain, (int)sizeof(szMain));
        break;
    }

    case 0x04:
        safe_snprintf(szMain, TN("%lu раз", "%lu times"), (unsigned long)dw32);
        break;

    case 0x0C:
        safe_snprintf(szMain, TN("%lu циклов", "%lu cycles"), (unsigned long)dw32);
        break;

    case 0x03:
    {
        WORD wMs = w16;
        if (wMs > 0 && wMs < 30000)
            safe_snprintf(szMain, TN("%u мс", "%u ms"), wMs);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0x0A:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu повторов  (!)", "%lu retries  (!)"), (unsigned long)dw32);
        break;

    case 0x05:
    {
        DWORD dwSec = dw32 & 0xFFFF;
        if (dwSec == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu секторов  (!)", "%lu sectors  (!)"), (unsigned long)dwSec);
        break;
    }

    case 0xC4:
        if (DbNotInUse(pDrv, bID)) {
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            break;
        }
        if (DecodeRemapEvents(pDrv, pRaw) < 0) {
            if (RemapRawIsFlyingHours(pDrv, pRaw))
                safe_snprintf(szMain, TN(
                    "%lu ч полёта (как 240), не события",
                    "%lu flying hours (same as 240), not events"),
                    (unsigned long)dw32);
            else
                safe_snprintf(szMain, "%s", TN("не счётчик событий", "not an event count"));
        } else if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0 событий  (ОК)", "0 events (OK)"));
        else
            safe_snprintf(szMain, TN("%lu событий  (!)", "%lu events  (!)"), (unsigned long)dw32);
        break;

    case 0xC5:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu нестабильных  (!)", "%lu pending  (!)"), (unsigned long)dw32);
        break;

    case 0xC6:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu неисправимых  (!)", "%lu uncorrectable  (!)"), (unsigned long)dw32);
        break;

    case 0xC7:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu ошибок CRC  (!)", "%lu CRC errors  (!)"), (unsigned long)dw32);
        break;

    case 0xBB:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu неисправимых", "%lu uncorrectable"), (unsigned long)dw32);
        break;

    case 0xC3:
        if (UseSeagateRateRaw(pDrv)) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, TN("%u ош. чтения / %lu секторов",
                                     "%u read errors / %lu sectors"),
                          nErr, (unsigned long)nOps);
            break;
        }
        if (eType == DRIVE_TYPE_HDD) {
            if (dw32 == 0)
                safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
            else
                safe_snprintf(szMain, TN("%lu восстановлений ECC", "%lu ECC recovered"), (unsigned long)dw32);
            break;
        }
        /* Reached only when the SSD profile named this counter.
         * An unknown 195 still returns above as vendor-specific. */
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xBC:
    {
        /* Seagate: total, completions over 5 s, completions over 7.5 s. */
        WORD wTotal = ReadLE16(pRaw);
        WORD wOver5 = ReadLE16(pRaw + 2);
        WORD wOver75 = ReadLE16(pRaw + 4);
        if (wTotal == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%u таймаутов (за 5 с: %u, за 7.5 с: %u)",
                                     "%u timeouts (over 5 s: %u, over 7.5 s: %u)"),
                          wTotal, wOver5, wOver75);
        break;
    }

    case 0xBD:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu записей на большой высоте (вибрация)",
                                     "%lu high-fly writes (vibration)"),
                          (unsigned long)dw32);
        break;

    case 0xC1:
        safe_snprintf(szMain, TN("%lu циклов парковки", "%lu load/unload cycles"), (unsigned long)dw32);
        break;

    case 0xC0:
    {
        unsigned long nRetract = (unsigned long)dw32;
        /* Toshiba/Fujitsu HDDs often duplicate the 16-bit retract count in both words:
         * 0x01420142 -> 322 retracts, not 21,102,914. */
        if (((dw32 >> 16) & 0xFFFFu) == (dw32 & 0xFFFFu) && (dw32 & 0xFFFFu) > 0) {
            nRetract = (unsigned long)w16;
        } else if (pDrv && (pDrv->eVendor == VENDOR_TOSHIBA || pDrv->eVendor == VENDOR_KIOXIA) && dw32 > 0xFFFFu) {
            nRetract = (unsigned long)w16;
        }
        if (DriveTreatsC0AsPowerLoss(pDrv))
            safe_snprintf(szMain, TN("%lu событий", "%lu events"), nRetract);
        else
            safe_snprintf(szMain, TN("%lu парковок при выключении", "%lu power-off retracts"), nRetract);
        break;
    }

    case 0xB7:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, "%lu понижений  (!)", (unsigned long)dw32);
        break;

    case 0xB8:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else
            safe_snprintf(szMain, TN("%lu ошибок сквозной передачи",
                                     "%lu end-to-end errors"),
                          (unsigned long)dw32);
        break;

    case 0x0E:
    case 0xBF:
    case 0xDD:
        if (dw32 == 0)
            safe_snprintf(szMain, "0 событий");
        else
            safe_snprintf(szMain, TN("%lu событий", "%lu events"), (unsigned long)dw32);
        break;

    case 0xC8:
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else if (NormalizedWriteErrorScored(bVal, bThresh))
            safe_snprintf(szMain, "%lu  (значение важнее RAW)", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0x01:
    {
        if (UseSeagateRateRaw(pDrv)) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, TN("%u ошибок чтения / %lu секторов",
                                     "%u read errors / %lu sectors"),
                          nErr, (unsigned long)nOps);
            break;
        }
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0x07:
    {
        if (UseSeagateRateRaw(pDrv)) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, TN("%u ошибок позиц. / %lu поисков",
                                     "%u seek errors / %lu seeks"),
                          nErr, (unsigned long)nOps);
            break;
        }
        if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0  (ОК)", "0 (OK)"));
        else if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xF1:
    case 0xF2:
    case 0xF3:
    case 0xF4:
    {
        /* An erase/wear row, or a drivedb counter that only shares the
         * ID (Samsung 243 downshift), is not an LBA total. */
        {
            int semF = PROF_SEM_NONE, encF = PROF_ENC_NONE;
            if (AttrEncOf(pDrv, bID, &semF, &encF) &&
                (semF == PROF_SEM_ERASE || semF == PROF_SEM_WEAR ||
                 encF == PROF_ENC_COUNTER || encF == PROF_ENC_PAIR ||
                 encF == PROF_ENC_MAXAVG) &&
                encF != PROF_ENC_LBA && encF != PROF_ENC_GIB &&
                encF != PROF_ENC_BLOCK32 && encF != PROF_ENC_SCALE) {
                if (qw48 > 0xFFFFFFFFULL)
                    safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
                else
                    safe_snprintf(szMain, "%lu", (unsigned long)dw32);
                break;
            }
        }
        /* Encoding comes from the profile: Phison SCALE, SM2258 BLOCK32.
         * Host reads may exceed NAND writes. */
        {
            int semH = PROF_SEM_NONE, encH = PROF_ENC_NONE;
            BOOL sata = pDrv && (pDrv->eType == DRIVE_TYPE_SSD_SATA ||
                                 pDrv->eType == DRIVE_TYPE_M2_SATA);
            if ((bID == 0xF1 || bID == 0xF2) && sata &&
                AttrEncOf(pDrv, bID, &semH, &encH) &&
                (encH == PROF_ENC_SCALE || encH == PROF_ENC_BLOCK32)) {
                unsigned __int64 nGB = (encH == PROF_ENC_BLOCK32)
                    ? Smi225xHostGiB(qw48)
                    : ScaleAtaHostGiB(pDrv, qw48, bID == 0xF2);
                if (nGB > 4000000ULL)
                    nGB = 4000000ULL;
                double bytes = (double)nGB * 1073741824.0;
                double tb = bytes / 1e12;
                if (tb >= 1.0) {
                    if (bID == 0xF1)
                        safe_snprintf(szMain, TN("%.1f ТБ записано хостом", "%.1f TB written by host"), tb);
                    else
                        safe_snprintf(szMain, TN("%.1f ТБ прочитано хостом", "%.1f TB read by host"), tb);
                } else {
                    if (bID == 0xF1)
                        safe_snprintf(szMain, TN("%.1f ГБ записано хостом", "%.1f GB written by host"), bytes / 1e9);
                    else
                        safe_snprintf(szMain, TN("%.1f ГБ прочитано хостом", "%.1f GB read by host"), bytes / 1e9);
                }
                break;
            }
        }
        /* Profile says the raw value is already GiB. A reading above
         * 4e6 GiB is not GiB (SPCC 241); leave it as LBAs. */
        {
            int semG = PROF_SEM_NONE, encG = PROF_ENC_NONE;
            if ((bID == 0xF1 || bID == 0xF2) &&
                AttrEncOf(pDrv, bID, &semG, &encG) &&
                encG == PROF_ENC_GIB && qw48 <= 4000000ULL) {
                double bytes = (double)qw48 * 1073741824.0;
                double tb = bytes / 1e12;
                if (tb >= 1.0) {
                    if (bID == 0xF1)
                        safe_snprintf(szMain, TN("%.1f ТБ записано хостом", "%.1f TB written by host"), tb);
                    else
                        safe_snprintf(szMain, TN("%.1f ТБ прочитано хостом", "%.1f TB read by host"), tb);
                } else {
                    if (bID == 0xF1)
                        safe_snprintf(szMain, TN("%.1f ГБ записано хостом", "%.1f GB written by host"), bytes / 1e9);
                    else
                        safe_snprintf(szMain, TN("%.1f ГБ прочитано хостом", "%.1f GB read by host"), bytes / 1e9);
                }
                break;
            }
        }
        {
            unsigned __int64 nLBA = qw48;
            double bytes = (double)nLBA * 512.0;
            double tb = bytes / 1e12;
            if (tb >= 1.0) {
                safe_snprintf(szMain, TN("%llu LBA  (~%.1f ТБ)", "%llu LBA  (~%.1f TB)"),
                              (unsigned long long)nLBA, tb);
            } else if (bytes >= 1e9) {
                safe_snprintf(szMain, TN("%llu LBA  (~%.1f ГБ)", "%llu LBA  (~%.1f GB)"),
                              (unsigned long long)nLBA, bytes / 1e9);
            } else {
                safe_snprintf(szMain, "%llu LBA", (unsigned long long)nLBA);
            }
        }
        break;
    }

    case 0xF9:
    case 0xE9:
    {
        int semW = PROF_SEM_NONE, encW = PROF_ENC_NONE;
        BOOL known = AttrEncOf(pDrv, bID, &semW, &encW);
        if (known && encW == PROF_ENC_PERCENT && bVal <= 100) {
            safe_snprintf(szMain, TN("%u%%  индикатор износа",
                                     "%u%%  wear indicator"), (unsigned)bVal);
            break;
        }
        if (known && ProfileWriteVolume(semW, encW) &&
            encW == PROF_ENC_GIB && qw48 > 0 && qw48 <= 4000000ULL) {
            safe_snprintf(szMain, TN("%llu ГиБ записано", "%llu GiB written"),
                          (unsigned long long)qw48);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xA8:
        if (eCtl == CONTROLLER_MAXIO) {
            unsigned hi = (unsigned)((qw48 >> 32) & 0xFFFFu);
            if (hi)
                safe_snprintf(szMain, TN("%lu (старший байт %u)",
                                         "%lu (high byte %u)"),
                              (unsigned long)dw32, hi);
            else
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xA9:
    {
        int enc9 = PROF_ENC_NONE;
        BOOL have9 = AttrEncOf(pDrv, bID, NULL, &enc9);
        /* A pair is already printed by FormatAttrRawText. */
        /* A9 is life remaining only on Phison or a profile that says so. */
        if (pDrv && IsPhisonFamily(pDrv)) {
            /* Summary picked another counter (E7). Do not also call this RAW life. */
            if (dw32 >= 1 && dw32 <= 100 &&
                !(pDrv->nEndurancePercent >= 0 &&
                  (int)dw32 != pDrv->nEndurancePercent))
                safe_snprintf(szMain, TN("%lu%%  заявленный остаток ресурса",
                                         "%lu%% reported life remaining"),
                              (unsigned long)dw32);
            else if (dw32 == 0 && bVal >= 90)
                safe_snprintf(szMain, "%s", TN("не задан (dummy Value)", "not set (dummy Value)"));
            else if (dw32 == 0)
                safe_snprintf(szMain, "%s", TN("0%  заявленный остаток ресурса",
                                               "0% reported life remaining"));
            else
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        } else if (have9 && enc9 == PROF_ENC_PERCENT && dw32 <= 100) {
            safe_snprintf(szMain, TN("%lu%%  остаток ресурса",
                                     "%lu%% life remaining"),
                          (unsigned long)dw32);
        } else {
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        }
        break;
    }

    case 0xB1:
    {
        int semB = PROF_SEM_NONE, encB = PROF_ENC_NONE;
        if (AttrEncOf(pDrv, bID, &semB, &encB) &&
            semB == PROF_SEM_ENDURANCE && encB == PROF_ENC_VALUE &&
            bVal <= 100) {
            safe_snprintf(szMain, TN("износ/остаток %u%%", "wear/life %u%%"),
                          (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xCA:
    {
        int semCa = PROF_SEM_NONE, encCa = PROF_ENC_NONE;
        if (AttrEncOf(pDrv, bID, &semCa, &encCa) &&
            semCa == PROF_SEM_ENDURANCE) {
            if (encCa == PROF_ENC_VALUE && bVal <= 100) {
                safe_snprintf(szMain, TN("%u%% ресурса", "%u%% life remaining"),
                              (unsigned)bVal);
                break;
            }
            if (encCa == PROF_ENC_PERCENT && dw32 <= 100) {
                safe_snprintf(szMain, TN("%lu%% ресурса", "%lu%% life remaining"),
                              (unsigned long)dw32);
                break;
            }
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xAD:
    {
        int encAd = PROF_ENC_NONE;
        BOOL split = AttrEncOf(pDrv, bID, NULL, &encAd) &&
                     encAd == PROF_ENC_MAXAVG;
        if (!split && eCtl == CONTROLLER_PHISON && dw32 > 0xFFFFUL)
            split = TRUE;
        if (eCtl == CONTROLLER_MICRON)
            safe_snprintf(szMain, TN("%lu среднее стираний", "%lu average erases"),
                          (unsigned long)dw32);
        else if (split) {
            unsigned mx, avg;
            PhisonMaxAvgHalves(dw32, &mx, &avg);
            safe_snprintf(szMain, TN("макс. %u / средн. %u", "max %u / avg %u"),
                          mx, avg);
        } else if (eCtl == CONTROLLER_MAXIO) {
            safe_snprintf(szMain, "%lu", (unsigned long)(dw32 & 0xFFFFUL));
        } else
            safe_snprintf(szMain, TN("%lu циклов выравнивания", "%lu wear-leveling cycles"),
                          (unsigned long)dw32);
        break;
    }

    case 0xA1:
    case 0xB2:
        safe_snprintf(szMain, TN("%lu блоков", "%lu blocks"), (unsigned long)dw32);
        break;

    case 0xAB:
    case 0xAC:
    case 0xB5:
    case 0xB6:
        if (pDrv && pDrv->eType == DRIVE_TYPE_HDD) {
            unsigned hi = (unsigned)((dw32 >> 16) & 0xFFFFu);
            unsigned lo = (unsigned)(dw32 & 0xFFFFu);
            safe_snprintf(szMain, "%u / %u", hi, lo);
        } else if (dw32 == 0)
            safe_snprintf(szMain, "%s", TN("0 сбоев  (ОК)", "0 failures (OK)"));
        else
            safe_snprintf(szMain, TN("%lu сбоев  (!)", "%lu failures  (!)"),
                          (unsigned long)dw32);
        break;

    case 0xAA:
    {
        int encA = PROF_ENC_NONE;
        AttrEncOf(pDrv, bID, NULL, &encA);
        if (encA == PROF_ENC_PERCENT && bVal <= 100) {
            safe_snprintf(szMain, TN("%u%%  резервное пространство",
                                     "%u%%  reserved space"), (unsigned)bVal);
        } else {
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        }
        break;
    }

    case 0xE8:
    {
        int spareBlk = -1;
        int si;
        if (eCtl == CONTROLLER_INTEL && bVal <= 100) {
            safe_snprintf(szMain, TN("%u%%  резервное пространство",
                                     "%u%%  reserved space"), (unsigned)bVal);
            break;
        }
        /* Phison S11: 232 RAW copies spare blocks (161). Value is the percent.
         * R5SL480G RAW 47 and attribute 161 are the same 47 blocks, Value is 100. */
        if (pDrv && IsPhisonFamily(pDrv)) {
            for (si = 0; si < 30; si++) {
                const SMART_ATTRIBUTE* sp = &pDrv->attrData.stAttributes[si];
                if (sp->bAttrID == 0xA1) {
                    spareBlk = (int)GetRawValue(sp->bRawValue);
                    break;
                }
            }
        }
        if (spareBlk >= 0 && (int)dw32 == spareBlk &&
            bVal <= 100 && (unsigned)bVal != dw32) {
            safe_snprintf(szMain, TN("%lu блоков", "%lu blocks"),
                          (unsigned long)dw32);
            break;
        }
        {
            DWORD pct = dw32 ? dw32 : (DWORD)bVal;
            safe_snprintf(szMain, TN("%lu%%  резервное пространство",
                                     "%lu%%  reserved space"), (unsigned long)pct);
        }
        break;
    }

    case 0xF0:
        safe_snprintf(szMain, TN("%lu ч полёта головок", "%lu head flying hours"),
                      (unsigned long)(DWORD)w16);
        break;

    case 0xE1:
    {
        int encE1 = PROF_ENC_NONE;
        if (AttrEncOf(pDrv, bID, NULL, &encE1) && encE1 == PROF_ENC_BLOCK32) {
            unsigned __int64 nGiB = ((unsigned __int64)dw32 * 32ULL) / 1024ULL;
            double bytes = (double)nGiB * 1073741824.0;
            double tb = bytes / 1e12;
            if (tb >= 1.0)
                safe_snprintf(szMain, TN("%lu единиц (32 MiB)  (~%.1f ТБ)",
                                         "%lu units (32 MiB)  (~%.1f TB)"),
                              (unsigned long)dw32, tb);
            else
                safe_snprintf(szMain, TN("%lu единиц (32 MiB)  (~%.1f ГБ)",
                                         "%lu units (32 MiB)  (~%.1f GB)"),
                              (unsigned long)dw32, bytes / 1e9);
            break;
        }
        safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xE2:
        if (eCtl == CONTROLLER_INTEL)
            safe_snprintf(szMain, "%lu ч нагрузки", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xF5:
    {
        int encF5 = PROF_ENC_NONE;
        if (AttrEncOf(pDrv, bID, NULL, &encF5) && encF5 == PROF_ENC_BLOCK32) {
            unsigned __int64 nGB = Smi225xHostGiB(qw48);
            if (nGB > 4000000ULL)
                nGB = 4000000ULL;
            safe_snprintf(szMain, TN("%llu ГБ записано в NAND",
                                     "%llu GB written to NAND"),
                          (unsigned long long)nGB);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0xF6:
        if (eCtl == CONTROLLER_MICRON) {
            unsigned __int64 nLBA = qw48;
            double bytes = (double)nLBA * 512.0;
            double tb = bytes / 1e12;
            if (tb >= 1.0)
                safe_snprintf(szMain, TN("%llu LBA  (~%.1f ТБ)", "%llu LBA  (~%.1f TB)"),
                              (unsigned long long)nLBA, tb);
            else if (bytes >= 1e9)
                safe_snprintf(szMain, TN("%llu LBA  (~%.1f ГБ)", "%llu LBA  (~%.1f GB)"),
                              (unsigned long long)nLBA, bytes / 1e9);
            else
                safe_snprintf(szMain, "%llu LBA", (unsigned long long)nLBA);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    default:
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else if (dw32 == 0)
            safe_snprintf(szMain, "0");
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    safe_snprintf_n(szBuf, nBufLen, "%s", szMain);
}

/* Canonical status token. The list paints it through Tr(STR_ST_*). */
static const char* RankToken(int rank)
{
    switch (rank) {
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:
        return "ПЛОХО";
    case HEALTH_STATUS_CAUTION:
        return "Внимание";
    case HEALTH_STATUS_OBSERVE:
        return "Риск";
    case HEALTH_STATUS_GOOD:
        return "ОК";
    default:
        return "Не оценивается";
    }
}

/* Status from the profile channel rule, not from a private ID threshold. */
static const char* AtaRowStatus(const DRIVE_INFO* p, const SMART_ATTRIBUTE* a,
                                BYTE bThresh, BOOL bFailed)
{
    BYTE id = a->bAttrID;
    BYTE val = a->bAttrValue;
    BYTE worst = a->bWorstValue;
    BOOL ssd = p->bIsNVMe || p->eType == DRIVE_TYPE_SSD_SATA ||
               p->eType == DRIVE_TYPE_M2_SATA;
    int rank = (int)HEALTH_STATUS_UNKNOWN;
    int ev;

    if (bFailed) {
        if (a->wStatusFlags & 0x0001)
            return "СБОЙ";
        return "Внимание";
    }
    /* 190/194: Worst is a temperature, not a fallen normalized value.
     * «было» only when the decoded peak crossed the inverted trip. */
    if (id != 0xBE && id != 0xC2 &&
        bThresh > 0 && worst != 0 && worst != 255 && worst <= bThresh)
        return "было";

    ev = AttrEvidenceRank(p, a, &rank);
    if (ev == ATTR_EV_RANK) return RankToken(rank);
    if (ev == ATTR_EV_SKIP) return "Не оценивается";

    if (ssd) {
        int semLife = PROF_SEM_NONE, encLife = PROF_ENC_NONE;
        if (ProfileLookup(p, id, &semLife, &encLife) &&
            semLife == PROF_SEM_ENDURANCE &&
            (encLife == PROF_ENC_PERCENT || encLife == PROF_ENC_VALUE)) {
            if (p->nEndurancePercent < 0) return "Не оценивается";
            return RankToken(WearBadgeStatus(p->nEndurancePercent));
        }
    }

    if (id == 0xC3) {
        if (UseSeagateRateRaw(p)) {
            if (SeagateRateErrs(a->bRawValue) > 0) return "Внимание";
            return "ОК";
        }
        return "Не оценивается";
    }

    if (id == 0xC1 && !ssd) {
        if (val > 0 && val <= 5) return "Внимание";
        if (val > 0 && val <= 15) return "Риск";
        if (GetRawValue(a->bRawValue) >= 300000) return "Риск";
        return "ОК";
    }

    if (id == 0xC8) {
        if (!NormalizedWriteErrorScored(val, bThresh)) return "ОК";
        if (val <= 1) return "ПЛОХО";
        if (val <= 10) return "Внимание";
        return "ОК";
    }
    if (id == 0x01 || id == 0x07) {
        DWORD dw01;
        WORD lo16;
        if (ssd) return "ОК";
        if (UseSeagateRateRaw(p)) {
            if (SeagateRateErrs(a->bRawValue) > 0) return "Внимание";
            return "ОК";
        }
        dw01 = GetRawValue(a->bRawValue);
        lo16 = (WORD)(dw01 & 0xFFFFu);
        /* Hitachi often packs two samples; lo16==0 is not a real error count. */
        if (p->eVendor == VENDOR_HITACHI) {
            if (lo16 == 0 && dw01 != 0) return "Не оценивается";
            if (dw01 > 10) return "Внимание";
            return "ОК";
        }
        /* Samsung F1-style huge counters are noise. */
        if (p->eVendor == VENDOR_SAMSUNG && dw01 > 10000u)
            return "ОК";
        /* WD Value 200 is the best mark. A stale non-zero RAW
         * (WD5000LPCX-24 attr 1 stays 6435 at value 200, threshold 51)
         * is not an error count. Badge only within 10 of the threshold. */
        if (p->eVendor == VENDOR_WDC) {
            if (bThresh > 0 && val > 0 && (int)val <= (int)bThresh + 10)
                return "Внимание";
            return "ОК";
        }
        if (p->eVendor == VENDOR_SAMSUNG && dw01 > 0)
            return "Внимание";
        return "ОК";
    }
    if (id == 0xC0 && DriveTreatsC0AsPowerLoss(p))
        return (GetRawValue(a->bRawValue) > 0) ? "журнал питания" : "ОК";
    if (ssd && id == 0xAE && GetRawValue(a->bRawValue) > 0 && IsPowerLossAttrConfirmed(p, 0xAE))
        return "журнал питания";
    if (ssd && id == 0xA0 && p->eController == CONTROLLER_PHISON && GetRawValue(a->bRawValue) > 0)
        return "журнал питания";
    /* 189 high-fly writes are vibration context. 188 is a profile rule. */
    if (id == 0xBD && !ssd && GetRawValue(a->bRawValue) > 0)
        return "контекст";

    if (!ssd && IsShockSensorAttr(id)) {
        if (p->nGSenseEvents <= 0)
            return "ОК";
        if (p->eMechanics == HEALTH_STATUS_CAUTION ||
            p->eMechanics == HEALTH_STATUS_BAD)
            return "Внимание";
        if (p->eMechanics == HEALTH_STATUS_OBSERVE)
            return "Риск";
        return "ОК";
    }

    if (id == 0xC2 || id == 0xBE) {
        if (p->nTemperatureC > 70)
            return "Жарко";
        if (AtaTempPeakTripped(a, bThresh))
            return "было";
        return "ОК";
    }

    if ((a->wStatusFlags & 0x0001) && bThresh > 0 && bThresh < 50 &&
        val > bThresh && val < bThresh + 10)
        return "Внимание";
    return "ОК";
}

static void FormatSmartRawHex(const BYTE* pRaw, char* szBuf, int nBufLen)
{
    if (!pRaw || !szBuf || nBufLen <= 0) return;
    safe_snprintf_n(szBuf, nBufLen, "%02X%02X%02X%02X%02X%02X",
              pRaw[5], pRaw[4], pRaw[3], pRaw[2], pRaw[1], pRaw[0]);
}

void UpdateAttrList(HWND hWnd, int nDriveIdx)
{
    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);

    ATTR_ROW rows[MAX_ATTR_ROWS];
    int      nDesired = 0;
    ZeroMemory(rows, sizeof(rows));

    if (nDriveIdx < 0 || nDriveIdx >= g_nDriveCount) {
        SendMessage(hList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(hList);
        SendMessage(hList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hList, NULL, TRUE);
        return;
    }

    DRIVE_INFO* pInfo = &g_Drives[nDriveIdx];

    if (SmartSnapshotRejected(pInfo)) {
        AttrRowSet(&rows[0], "--",
            TN("Ошибка данных SMART: пакет повреждён или обрезан. Здоровье не оценивается.",
               "SMART data error: the packet is damaged or truncated. Health is not assessed."),
            "", "—", "—", "", "", "");
        nDesired = 1;
    }
    else if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        const char* msg = IsLikelyUsbFlashDrive(pInfo)
            ? TN("У этой USB-флешки SMART не отдаётся — так у большинства флешек.",
                 "This USB flash drive does not export SMART — most sticks don't.")
            : TN("USB-корпус/мост не отдаёт SMART",
                 "USB enclosure/bridge does not export SMART");
        AttrRowSet(&rows[0], "--", msg, "", "—", "—", "", "", "");
        nDesired = 1;
    }
    else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        char szErr[64];
        safe_snprintf(szErr, "Win32 %lu", (unsigned long)pInfo->dwErrNvmeProtocol);
        AttrRowSet(&rows[0], "--", TN("NVMe Health Log IOCTL не удался",
                                       "NVMe Health Log IOCTL failed"),
                   szErr, "—", "—", "", "", "");
        AttrRowSet(&rows[1], "--", "5=нет прав  1/50=нет в драйвере  87=параметр",
                   "", "—", "—", "", "", "");
        nDesired = 2;
    }
    else if (pInfo->bIsNVMe && pInfo->bSMART_Supported) {
        NVME_HEALTH_INFO_LOG* pLog = &pInfo->nvmeHealth;
        unsigned __int64 qwDataRead    = NVMeRead128Lo(pLog->DataUnitsRead);
        unsigned __int64 qwDataWritten = NVMeRead128Lo(pLog->DataUnitsWritten);
        unsigned __int64 qwPOH         = NVMeRead128Lo(pLog->PowerOnHours);
        unsigned __int64 qwPowerCycles = NVMeRead128Lo(pLog->PowerCycles);
        unsigned __int64 qwUnsafeSDs   = NVMeRead128Lo(pLog->UnsafeShutdowns);
        unsigned __int64 qwMediaErr    = NVMeRead128Lo(pLog->MediaErrors);
        unsigned __int64 qwErrLog      = NVMeRead128Lo(pLog->NumErrLogEntries);
        WORD wTempK = ReadLE16(pLog->CompositeTemperature);
        int  nTempC = (wTempK > 273) ? (int)wTempK - 273 : 0;
        int  nWarnC = NvmeIdentifyTempC(pInfo->wNVMeWarnTempThreshold);
        int  nCritC = NvmeIdentifyTempC(pInfo->wNVMeCritTempThreshold);

        #define NVME_NA TN("н/п", "n/a")
        #define NVME_DASH Tr(STR_NONE)
        /* NVMe has no normalized ATA value. The reading is Value.
         * Decode stays a dash: repeating that reading there is not a
         * second fact. RAW is the log field. Worst stays n/a.
         * Threshold is n/a unless the log has one. */
        #define NVME_ROW_EX(id_, name_, val_, worst_, thresh_, raw_, dec_, stat_) \
        { \
            if (nDesired < MAX_ATTR_ROWS) \
                AttrRowSet(&rows[nDesired++], (id_), (name_), (val_), \
                           (worst_), (thresh_), (raw_), (dec_), (stat_)); \
        }
        #define NVME_ROW(id_, name_, val_, raw_, stat_) \
            NVME_ROW_EX(id_, name_, (val_), NVME_NA, NVME_NA, raw_, NVME_DASH, (stat_))

        char szCrit[64], szCritRaw[16], szVBuf[64];
        char szSpare[32], szSpareRaw[16], szSpTh[32], szSpThRaw[16];
        char szPctU[32], szPctRaw[16];
        char szDUR[64], szDURRaw[32], szDUW[64], szDUWRaw[32];
        char szPOH[64], szPOHRaw[32], szPC[32];
        char szUS[32], szME[32], szEL[32], szWCT[32], szWCTRaw[16];
        char szCCT[32], szCCTRaw[16];

        safe_snprintf(szCritRaw, "%u", (unsigned)pLog->CriticalWarning);
        if (pLog->CriticalWarning == 0) safe_snprintf(szCrit, "%s", TN("0 (нет)", "0 (none)"));
        else safe_snprintf(szCrit, "0x%02X (!)", pLog->CriticalWarning);
        NVME_ROW("01h", TN("Критическое предупреждение", "Critical warning"),
                 szCrit, szCritRaw, (pLog->CriticalWarning?"ПЛОХО":"ОК"));

        if (nWarnC <= 0) nWarnC = pInfo->nTempWarnC;
        if (nCritC <= 0) nCritC = pInfo->nTempCritC;
        if (nWarnC > 0 && nCritC > 0 && pInfo->nTempMaxC > 0)
            safe_snprintf(szVBuf, TN("%d C (макс. %d, пред. %d, крит. %d)",
                                     "%d C (max %d, warn %d, crit %d)"),
                          nTempC, pInfo->nTempMaxC, nWarnC, nCritC);
        else if (nWarnC > 0 && nCritC > 0)
            safe_snprintf(szVBuf, TN("%d C (пред. %d, крит. %d)",
                                     "%d C (warn %d, crit %d)"),
                          nTempC, nWarnC, nCritC);
        else if (nCritC > 0 && pInfo->nTempMaxC > 0)
            safe_snprintf(szVBuf, TN("%d C (макс. %d, крит. %d)",
                                     "%d C (max %d, crit %d)"),
                          nTempC, pInfo->nTempMaxC, nCritC);
        else if (nWarnC > 0)
            safe_snprintf(szVBuf, TN("%d C (пред. %d)", "%d C (warn %d)"),
                          nTempC, nWarnC);
        else if (nCritC > 0)
            safe_snprintf(szVBuf, TN("%d C (крит. %d)", "%d C (crit %d)"),
                          nTempC, nCritC);
        else
            safe_snprintf(szVBuf, "%d C (%d K)", nTempC, (int)wTempK);
        {
            const char* szTstat = "ОК";
            if (pInfo->eTempBand == TEMP_BAND_CRITICAL) szTstat = "Жарко";
            else if (pInfo->eTempBand == TEMP_BAND_HIGH) szTstat = "Внимание";
            else if (pInfo->eTempBand == TEMP_BAND_ELEVATED) szTstat = "Риск";
            {
                char szRawT[24], szWorstT[24], szThrT[24];
                const char* worstT = NVME_NA;
                const char* thrT = NVME_NA;
                /* Health Log composite temperature is Kelvin. */
                safe_snprintf(szRawT, "%u", (unsigned)wTempK);
                if (pInfo->nTempMaxC > 0) {
                    safe_snprintf(szWorstT, "%d C", pInfo->nTempMaxC);
                    worstT = szWorstT;
                }
                if (nWarnC > 0) {
                    safe_snprintf(szThrT, "%d C", nWarnC);
                    thrT = szThrT;
                } else if (nCritC > 0) {
                    safe_snprintf(szThrT, "%d C", nCritC);
                    thrT = szThrT;
                }
                NVME_ROW_EX("02h", TN("Температура", "Temperature"), szVBuf,
                            worstT, thrT, szRawT, NVME_DASH, szTstat);
            }
        }
        if (nWarnC > 0) {
            char szW[32], szWRaw[16];
            safe_snprintf(szW, "%d C", nWarnC);
            if (NvmeIdentifyTempC(pInfo->wNVMeWarnTempThreshold) > 0)
                safe_snprintf(szWRaw, "%u", (unsigned)pInfo->wNVMeWarnTempThreshold);
            else
                safe_snprintf(szWRaw, "%d", nWarnC);
            NVME_ROW("--", TN("Порог предупреждения", "Warning threshold"), szW, szWRaw, "ОК");
        }
        if (nCritC > 0) {
            char szC[32], szCRaw[16];
            safe_snprintf(szC, "%d C", nCritC);
            if (NvmeIdentifyTempC(pInfo->wNVMeCritTempThreshold) > 0)
                safe_snprintf(szCRaw, "%u", (unsigned)pInfo->wNVMeCritTempThreshold);
            else
                safe_snprintf(szCRaw, "%d", nCritC);
            NVME_ROW("--", TN("Макс. безопасная", "Critical threshold"), szC, szCRaw, "ОК");
        }
        if (pInfo->nTempMaxC > 0) {
            char szM[32], szMRaw[16];
            const char* st = "ОК";
            safe_snprintf(szM, "%d C", pInfo->nTempMaxC);
            safe_snprintf(szMRaw, "%d", pInfo->nTempMaxC);
            if (nCritC > 0 && pInfo->nTempMaxC >= nCritC) st = "Жарко";
            else if (nWarnC > 0 && pInfo->nTempMaxC >= nWarnC) st = "Внимание";
            NVME_ROW("--", TN("Макс. зафиксированная", "Lifetime max"), szM, szMRaw, st);
        }

        safe_snprintf(szSpare,"%d %%",(int)pLog->AvailableSpare);
        safe_snprintf(szSpareRaw, "%u", (unsigned)pLog->AvailableSpare);
        safe_snprintf(szSpTh,"%d %%",(int)pLog->AvailableSpareThreshold);
        safe_snprintf(szSpThRaw, "%u", (unsigned)pLog->AvailableSpareThreshold);
        NVME_ROW_EX("03h", TN("Запас блоков", "Available spare"), szSpare,
            NVME_NA, szSpTh, szSpareRaw, NVME_DASH,
            (pLog->AvailableSpare<pLog->AvailableSpareThreshold?"ПЛОХО":"ОК"));

        NVME_ROW("04h", TN("Порог запаса блоков", "Spare threshold"), szSpTh, szSpThRaw, "ОК");

        safe_snprintf(szPctU,"%d %%",(int)pLog->PercentageUsed);
        safe_snprintf(szPctRaw, "%u", (unsigned)pLog->PercentageUsed);
        {
            const char* szWearSt;
            int nLeft = 100 - (int)pLog->PercentageUsed;
            if (nLeft < 0) nLeft = 0;
            szWearSt = RankToken(WearBadgeStatus(nLeft));
            NVME_ROW("05h", TN("Износ", "Percentage used"), szPctU, szPctRaw, szWearSt);
        }

        /* Data Units are counts of 512000-byte units, not the terabyte string. */
        safe_snprintf(szDURRaw, "%llu", (unsigned long long)qwDataRead);
        FormatNvmeHostBytes(qwDataRead, szDUR, sizeof(szDUR));
        NVME_ROW("06h", TN("Прочитано (host)", "Data units read"), szDUR, szDURRaw, "ОК");

        safe_snprintf(szDUWRaw, "%llu", (unsigned long long)qwDataWritten);
        FormatNvmeHostBytes(qwDataWritten, szDUW, sizeof(szDUW));
        NVME_ROW("07h", TN("Записано (host)", "Data units written"), szDUW, szDUWRaw, "ОК");

        {
            char szHR[64], szHW[64], szBT[64], szBTRaw[32];
            safe_snprintf(szHR, "%llu", (unsigned long long)pInfo->qwNVMeHostReads);
            NVME_ROW("08h", TN("Команды чтения хоста", "Host read commands"), szHR, szHR, "ОК");
            safe_snprintf(szHW, "%llu", (unsigned long long)pInfo->qwNVMeHostWrites);
            NVME_ROW("09h", TN("Команды записи хоста", "Host write commands"), szHW, szHW, "ОК");
            safe_snprintf(szBTRaw, "%llu", (unsigned long long)pInfo->qwNVMeControllerBusyTime);
            safe_snprintf(szBT, "%llu мин", (unsigned long long)pInfo->qwNVMeControllerBusyTime);
            NVME_ROW("0Ah", TN("Занятость контроллера", "Controller busy time"), szBT, szBTRaw, "ОК");
        }

        safe_snprintf(szPC,"%llu",(unsigned long long)qwPowerCycles);
        NVME_ROW("0Bh", TN("Циклы включения", "Power cycles"), szPC, szPC, "ОК");

        {
            DWORD dwPoh = (qwPOH > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (DWORD)qwPOH;
            FormatPowerOnHours(dwPoh, szPOH, (int)sizeof(szPOH));
            safe_snprintf(szPOHRaw, "%llu", (unsigned long long)qwPOH);
        }
        NVME_ROW("0Ch", Tr(STR_POH), szPOH, szPOHRaw, "ОК");

        safe_snprintf(szUS,"%llu",(unsigned long long)qwUnsafeSDs);
        NVME_ROW("0Dh", TN("Небезопасные выключения", "Unsafe shutdowns"), szUS, szUS,
            (qwUnsafeSDs>0?"журнал питания":"ОК"));

        safe_snprintf(szME,"%llu",(unsigned long long)qwMediaErr);
        NVME_ROW("0Eh", TN("Ошибки носителя", "Media errors"), szME, szME,
            (qwMediaErr>0?"ПЛОХО":"ОК"));

        safe_snprintf(szEL,"%llu",(unsigned long long)qwErrLog);
        /* Samsung 960 EVO and others count aborts and internal events
         * here. Media errors are the fault counter. A non-zero log
         * is not a warning. */
        NVME_ROW("0Fh", TN("Записи в журнале ошибок", "Error log entries"), szEL, szEL,
            (qwErrLog>0?"Не оценивается":"ОК"));

        safe_snprintf(szWCTRaw, "%lu", (unsigned long)pLog->WarningCompTempTime);
        safe_snprintf(szWCT,"%lu мин",(unsigned long)pLog->WarningCompTempTime);
        NVME_ROW("--", TN("Время при высокой температуре", "Time above warning temp"),
            szWCT, szWCTRaw, (pLog->WarningCompTempTime>0?"контекст":"ОК"));

        safe_snprintf(szCCTRaw, "%lu", (unsigned long)pLog->CriticalCompTempTime);
        safe_snprintf(szCCT,"%lu мин",(unsigned long)pLog->CriticalCompTempTime);
        NVME_ROW("--", TN("Время при критической температуре", "Time above critical temp"),
            szCCT, szCCTRaw, (pLog->CriticalCompTempTime>0?"контекст":"ОК"));
        {
            int ts;
            for (ts = 0; ts < 8; ts++) {
                if (pInfo->nTempSensor[ts] >= 0) {
                    char szId[8], szName[48], szVal[32], szRawS[16];
                    safe_snprintf(szId, "T%d", ts + 1);
                    safe_snprintf(szName, TN("Датчик температуры %d", "Temperature sensor %d"), ts + 1);
                    safe_snprintf(szVal, "%d C", pInfo->nTempSensor[ts]);
                    safe_snprintf(szRawS, "%u", (unsigned)pLog->TempSensor[ts]);
                    {
                        const char* szSt = "ОК";
                        int t = pInfo->nTempSensor[ts];
                        if (nCritC > 0 && t >= nCritC) szSt = "Жарко";
                        else if (nWarnC > 0 && t >= nWarnC) szSt = "Внимание";
                        else if (nWarnC < 0 && nCritC < 0 && t > 70) szSt = "Жарко";
                        NVME_ROW(szId, szName, szVal, szRawS, szSt);
                    }
                }
            }
        }
        if (pLog->ThermalMgmtTemp1TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp1TransCnt);
            NVME_ROW("--", "Thermal Mgmt T1 переходов", sz, sz, "ОК");
        }
        if (pLog->ThermalMgmtTemp2TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp2TransCnt);
            NVME_ROW("--", "Thermal Mgmt T2 переходов", sz, sz, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp1) {
            char sz[32], szRaw[32];
            safe_snprintf(szRaw, "%lu", (unsigned long)pLog->TotalTimeThermalMgmtTemp1);
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp1);
            NVME_ROW("--", "Thermal Mgmt T1 время", sz, szRaw, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp2) {
            char sz[32], szRaw[32];
            safe_snprintf(szRaw, "%lu", (unsigned long)pLog->TotalTimeThermalMgmtTemp2);
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp2);
            NVME_ROW("--", "Thermal Mgmt T2 время", sz, szRaw, "ОК");
        }

        #undef NVME_ROW
        #undef NVME_ROW_EX
        #undef NVME_DASH
        #undef NVME_NA
    }
    else if (pInfo->bSMART_Supported) {
        int i, j;
        for (i = 0; i < 30 && nDesired < MAX_ATTR_ROWS; i++) {
            SMART_ATTRIBUTE* pAttr = &pInfo->attrData.stAttributes[i];
            if (pAttr->bAttrID == 0) continue;

            BYTE bThresh = 0;
            for (j = 0; j < 30; j++) {
                if (pInfo->threshData.stThresholds[j].bAttrID == pAttr->bAttrID) {
                    bThresh = pInfo->threshData.stThresholds[j].bThresholdValue;
                    break;
                }
            }
            BOOL bFailed = (bThresh > 0 && pAttr->bAttrValue <= bThresh);
            const char* szStat;
            char szVal[16], szWorst[16], szThresh[16], szRaw[16], szDec[80];

            safe_snprintf(szVal, "%u", (unsigned)pAttr->bAttrValue);
            safe_snprintf(szWorst, "%u", (unsigned)pAttr->bWorstValue);
            if (bThresh)
                safe_snprintf(szThresh, "%u", (unsigned)bThresh);
            else
                safe_snprintf(szThresh, "—");
            FormatSmartRawHex(pAttr->bRawValue, szRaw, (int)sizeof(szRaw));
            FormatSmartValue(pAttr->bAttrID, pAttr->bRawValue,
                             pAttr->bAttrValue, pAttr->bWorstValue, bThresh,
                             pInfo, szDec, (int)sizeof(szDec));
            szStat = AtaRowStatus(pInfo, pAttr, bThresh, bFailed);
            {
                char szId[8];
                safe_snprintf(szId, "%d", pAttr->bAttrID);
                AttrRowSet(&rows[nDesired++], szId, GetAttrNameEx(pAttr->bAttrID, pInfo),
                           szVal, szWorst, szThresh, szRaw, szDec, szStat);
            }
        }

        if (nDesired == 0) {
            /* Bridge gave us only SCSI LOG SENSE data (no ATA attribute
             * table). Show what was retrieved instead of an empty list. */
            AttrRowSet(&rows[nDesired++], "--", TN("Прогноз отказа (SCSI)", "SCSI predict failure"),
                       pInfo->bPredictFailure
                           ? TN("Отказ прогнозируется", "Failure predicted")
                           : TN("Отказ не прогнозируется", "Failure not predicted"),
                       "—", "—", "", "",
                       pInfo->bPredictFailure ? "СБОЙ" : "ОК");

            if (nDesired < MAX_ATTR_ROWS) {
                char szT[32];
                if (pInfo->nTemperatureC > 0)
                    safe_snprintf(szT, "%d C", pInfo->nTemperatureC);
                else
                    safe_snprintf(szT, "%s", Tr(STR_NO_DATA));
                AttrRowSet(&rows[nDesired++], "--",
                           TN("Температура (SCSI Log Sense)", "Temperature (SCSI Log Sense)"),
                           szT, "—", "—", "", "",
                           (pInfo->nTemperatureC > 70 ? "Жарко" : "ОК"));
            }

            if (nDesired < MAX_ATTR_ROWS) {
                AttrRowSet(&rows[nDesired++], "--",
                           TN("Полная таблица SMART мостом не отдаётся",
                              "Full SMART table is not provided by the bridge"),
                           "", "—", "—", "", "", "");
            }
        }

        /* Extra ATA info already collected in AcquireATASMART — display only. */
        if (nDesired < MAX_ATTR_ROWS && pInfo->wRotationRate >= 0x0401) {
            char sz[32];
            safe_snprintf(sz, TN("%u об/мин", "%u RPM"), (unsigned)pInfo->wRotationRate);
            AttrRowSet(&rows[nDesired++], "--", TN("Обороты", "Rotation"), sz, "—", "—", "", "", "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotErrorLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nErrorLogCount);
            AttrRowSet(&rows[nDesired++], "--", TN("Журнал ошибок SMART", "SMART error log"), sz, "—", "—", "", "",
                       pInfo->nErrorLogCount > 0 ? "Внимание" : "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotSelfTestLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nSelfTestStatus);
            AttrRowSet(&rows[nDesired++], "--", TN("Журнал самопроверки", "SMART self-test log"), sz, "—", "—", "", "", "ОК");
        }
    }

    SendMessage(hList, WM_SETREDRAW, FALSE, 0);

    int nCurrent = ListView_GetItemCount(hList);

    int row;
    for (row = 0; row < nDesired; row++) {
        LPARAM lpStat = AttrStatusParam(rows[row].col[ATTR_COL_STATUS]);
        if (row >= nCurrent) {
            WCHAR wz[128];
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            U8ToW(rows[row].col[0], wz, 128);
            lvi.mask     = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem    = row;
            lvi.iSubItem = 0;
            lvi.pszText  = wz;
            lvi.lParam   = lpStat;
            SendMessageW(hList, LVM_INSERTITEMW, 0, (LPARAM)&lvi);
        } else {
            LVITEMW lvi;
            ZeroMemory(&lvi, sizeof(lvi));
            lvi.mask     = LVIF_PARAM;
            lvi.iItem    = row;
            lvi.iSubItem = 0;
            lvi.lParam   = lpStat;
            SendMessageW(hList, LVM_SETITEMW, 0, (LPARAM)&lvi);
        }
        int col;
        for (col = 0; col < ATTR_NCOLS; col++)
            ListViewSetCellIfChanged(hList, row, col, rows[row].col[col]);
    }

    while (ListView_GetItemCount(hList) > nDesired)
        ListView_DeleteItem(hList, ListView_GetItemCount(hList) - 1);

    SendMessage(hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hList, NULL, FALSE);
    UpdateWindow(hList);
}

static volatile LONG g_bScanBusy = 0;

/* Double buffer. The worker writes only g_ScanBuf / g_nScanBufCount.
 * The UI thread reads only g_Drives / g_nDriveCount (paint, list, buttons).
 * Merge is WM_APP_REFRESH_DONE on the UI thread: Snapshot_Save, then memcpy
 * g_ScanBuf → g_Drives. g_bScanBusy is Interlocked CAS 0→1 in RefreshData
 * and InterlockedExchange 0 after that memcpy, so a second scan cannot start
 * (and cannot overwrite g_ScanBuf) while the copy is in flight. */
static DRIVE_INFO g_ScanBuf[MAX_DRIVES];
static int        g_nScanBufCount;

static void ScanDrivesToBuf(void)
{
    int n = ScanDrives(g_ScanBuf, MAX_DRIVES);
    g_nScanBufCount = n;
}

static DWORD WINAPI RefreshThreadProc(LPVOID lpParam)
{
    HWND hWnd = (HWND)lpParam;
    ScanDrivesToBuf();
    PostMessage(hWnd, WM_APP_REFRESH_DONE, 0, 0);
    return 0;
}

void RefreshData(HWND hWnd)
{
    if (InterlockedCompareExchange(&g_bScanBusy, 1, 0) != 0)
        return;
    SetActionButtonsEnabled(hWnd, FALSE);

    HANDLE hThread = CreateThread(NULL, 0, RefreshThreadProc, hWnd, 0, NULL);
    if (hThread)
        CloseHandle(hThread);
    else {
        /* CreateThread failed: scan on UI thread into the private buffer,
         * then the same merge path via WM_APP_REFRESH_DONE. */
        RefreshThreadProc(hWnd);
    }
}

#define ABOUT_W  540
#define ABOUT_H  342
#define LECTURE_W 640
#define LECTURE_H 640
#define IDC_LECTURE_TEXT      3600
#define PROP_LECTURE_FONT     "dmLecF"

/* Control identifiers used inside the About dialog. */
#define IDC_ABOUT_LINK       3500
#define IDC_ABOUT_LIC_STATUS 3501
#define IDC_ABOUT_ACTIVATE   3502

#define PROP_ABOUT_FONT_BOLD  "dmFntB"
#define PROP_ABOUT_FONT_LINK  "dmFntL"
#define PROP_ABOUT_ICON       "dmIco"

static HCURSOR s_hCursorHand = NULL;

BOOL OpenDonatePage(HWND hParent)
{
    HINSTANCE hResult = ShellExecuteA(
        hParent,
        "open",
        DONATE_URL,
        NULL, NULL,
        SW_SHOWNORMAL);

    if ((INT_PTR)hResult <= 32) {
        MessageBoxU8(hParent,
            TN("Не удалось открыть браузер.\n" DONATE_URL,
               "Could not open the browser.\n" DONATE_URL),
            "DriveMonitor",
            MB_ICONWARNING | MB_OK);
        return FALSE;
    }
    return TRUE;
}

static LRESULT CALLBACK AboutDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            int cx = UiScale(ABOUT_W);
            int ico = UiScale(32);

            HWND hIco = CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ICON | SS_CENTERIMAGE,
                (cx - ico) / 2, UiScale(18), ico, ico,
                hDlg, (HMENU)0, g_hInst, NULL);
            HICON hIc = (HICON)LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                IMAGE_ICON, ico, ico, LR_DEFAULTCOLOR);
            if (hIc) {
                SendMessageA(hIco, STM_SETICON, (WPARAM)hIc, 0);
                SetPropA(hDlg, PROP_ABOUT_ICON, (HANDLE)hIc);
            }

            HFONT hFontBold = UiMakeFont(15, FW_BOLD, FALSE);
            SetPropA(hDlg, PROP_ABOUT_FONT_BOLD, (HANDLE)hFontBold);
            char szAboutName[128];
            safe_snprintf(szAboutName, "DriveMonitor %s (%s %s)",
                DRIVEMONITOR_FILEVERSION_STR,
                TN("сборка", "build"),
                DRIVEMONITOR_BUILD_STR);
            HWND hName = CreateWindowExU8(0, "STATIC",
                szAboutName,
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(20), UiScale(58), cx - UiScale(40), UiScale(22),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hName, WM_SETFONT, (WPARAM)hFontBold, TRUE);

            HWND hDesc = CreateWindowExU8(0, "STATIC",
                Tr(STR_ABOUT_DESC),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(20), UiScale(82), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hDesc, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                UiScale(20), UiScale(108), cx - UiScale(40), UiScale(2),
                hDlg, (HMENU)0, g_hInst, NULL);

            HWND hCopy = CreateWindowExU8(0, "STATIC",
                "\xC2\xA9 2026 chuikoff \xE2\x80\x94 GPL-2.0-or-later",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(20), UiScale(118), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                UiScale(20), UiScale(144), cx - UiScale(40), UiScale(2),
                hDlg, (HMENU)0, g_hInst, NULL);

            {
                HWND hLicStatus = CreateWindowExU8(0, "STATIC",
                    Tr(STR_ABOUT_FOSS),
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    UiScale(20), UiScale(152), cx - UiScale(40), UiScale(18),
                    hDlg, (HMENU)IDC_ABOUT_LIC_STATUS, g_hInst, NULL);
                SendMessageA(hLicStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

                HWND hActivate = CreateWindowExU8(0, "BUTTON", Tr(STR_ABOUT_SUPPORT),
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    (cx - UiScale(120)) / 2, UiScale(178), UiScale(120), UiScale(26),
                    hDlg, (HMENU)IDC_ABOUT_ACTIVATE, g_hInst, NULL);
                SendMessageA(hActivate, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            }

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                UiScale(20), UiScale(214), cx - UiScale(40), UiScale(2),
                hDlg, (HMENU)0, g_hInst, NULL);

            HFONT hFontLink = UiMakeFont(12, FW_NORMAL, TRUE);
            SetPropA(hDlg, PROP_ABOUT_FONT_LINK, (HANDLE)hFontLink);

            HWND hLink = CreateWindowExU8(0, "STATIC",
                DONATE_URL,
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                UiScale(20), UiScale(224), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)IDC_ABOUT_LINK, g_hInst, NULL);
            SendMessageA(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            HWND hBtn = CreateWindowExU8(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                (cx - UiScale(80)) / 2, UiScale(254), UiScale(80), UiScale(26),
                hDlg, (HMENU)IDOK, g_hInst, NULL);
            SendMessageA(hBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            if (!s_hCursorHand)
                s_hCursorHand = LoadCursorA(NULL, (LPCSTR)IDC_HAND);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            DestroyWindow(hDlg);
        }
        else if (LOWORD(wParam) == IDC_ABOUT_ACTIVATE) {
            OpenDonatePage(hDlg);
        }
        else if (LOWORD(wParam) == IDC_ABOUT_LINK &&
                 HIWORD(wParam) == STN_CLICKED) {
            OpenDonatePage(hDlg);
        }
        return 0;

    case WM_NOTIFY:
        return 0;

    case WM_CTLCOLORSTATIC:
        {
            HWND hCtrl = (HWND)lParam;
            int  nID   = GetDlgCtrlID(hCtrl);
            HDC  hdcSt = (HDC)wParam;
            SetBkMode(hdcSt, TRANSPARENT);
            if (nID == IDC_ABOUT_LINK) {
                SetTextColor(hdcSt, RGB(0, 102, 204));
            } else if (nID == IDC_ABOUT_LIC_STATUS) {
                /* FOSS banner - render in green to signal "free / good". */
                SetTextColor(hdcSt, RGB(0, 140, 0));
            }
            return (LRESULT)(HBRUSH)(COLOR_WINDOW + 1);
        }

    case WM_SETCURSOR:
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hDlg, &pt);
            HWND hOver = ChildWindowFromPoint(hDlg, pt);
            if (hOver && s_hCursorHand &&
                GetDlgCtrlID(hOver) == IDC_ABOUT_LINK) {
                SetCursor(s_hCursorHand);
                return TRUE;
            }
            break;
        }

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            DestroyWindow(hDlg);
        }
        return 0;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        {
            HFONT hBold = (HFONT)GetPropA(hDlg, PROP_ABOUT_FONT_BOLD);
            HFONT hLink = (HFONT)GetPropA(hDlg, PROP_ABOUT_FONT_LINK);
            HICON hIco  = (HICON)GetPropA(hDlg, PROP_ABOUT_ICON);
            if (hBold) { DeleteObject(hBold); RemovePropA(hDlg, PROP_ABOUT_FONT_BOLD); }
            if (hLink) { DeleteObject(hLink); RemovePropA(hDlg, PROP_ABOUT_FONT_LINK); }
            if (hIco)  { DestroyIcon(hIco);   RemovePropA(hDlg, PROP_ABOUT_ICON); }
        }
        return 0;
    }

    return DefWindowProc(hDlg, uMsg, wParam, lParam);
}

void ShowAboutDialog(HWND hWndParent)
{

    static BOOL bRegistered = FALSE;
    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = AboutDlgProc;
        wc.hInstance     = g_hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"LLHDAboutDlg";
        RegisterClassW(&wc);
        bRegistered = TRUE;
    }

    HWND hExist = FindWindowA("LLHDAboutDlg", NULL);
    if (hExist) { SetForegroundWindow(hExist); return; }

    int nScrW = GetSystemMetrics(SM_CXSCREEN);
    int nScrH = GetSystemMetrics(SM_CYSCREEN);

    int nX, nY;
    int aw = UiScale(ABOUT_W), ah = UiScale(ABOUT_H);
    if (hWndParent) {
        RECT rcP;
        GetWindowRect(hWndParent, &rcP);
        nX = rcP.left + (rcP.right  - rcP.left - aw) / 2;
        nY = rcP.top  + (rcP.bottom - rcP.top  - ah) / 2;
    } else {
        nX = (nScrW - aw) / 2;
        nY = (nScrH - ah) / 2;
    }

    RECT rcAdj = {0, 0, aw, ah};
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    int nWinW = rcAdj.right  - rcAdj.left;
    int nWinH = rcAdj.bottom - rcAdj.top;

    HWND hDlg = CreateWindowExU8(
        WS_EX_DLGMODALFRAME | WS_EX_TOPMOST,
        "LLHDAboutDlg",
        TN("О программе — DriveMonitor", "About — DriveMonitor"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hWndParent, NULL, g_hInst, NULL
    );
    if (!hDlg) return;

    HICON hIco = LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON));
    SendMessageA(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hIco);
    SendMessageA(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)
        LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static LRESULT CALLBACK HealthLectureDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        {
            CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
            const DRIVE_INFO* pInfo = cs ? (const DRIVE_INFO*)cs->lpCreateParams : NULL;
            RECT rc;
            int cx, cy, btnW, btnH, margin, editH;
            HFONT hFont;
            HWND hEdit, hBtn;
            char szText[16384];
            WCHAR wz[16384];

            GetClientRect(hDlg, &rc);
            cx = rc.right - rc.left;
            cy = rc.bottom - rc.top;
            if (cx < UiScale(200)) cx = UiScale(LECTURE_W);
            if (cy < UiScale(200)) cy = UiScale(LECTURE_H);
            btnW = UiScale(88);
            btnH = UiScale(26);
            margin = UiScale(12);
            editH = cy - margin * 3 - btnH;
            if (editH < UiScale(80)) editH = UiScale(80);

            hFont = UiMakeFont(13, FW_NORMAL, FALSE);
            if (!hFont)
                hFont = CreateFontA(-UiScale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                    CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, "Courier New");
            if (!hFont)
                hFont = g_hFontNormal;
            SetPropA(hDlg, PROP_LECTURE_FONT, (HANDLE)hFont);

            hEdit = CreateWindowExU8(WS_EX_CLIENTEDGE, "EDIT", "",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP |
                ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                margin, margin, cx - 2 * margin, editH,
                hDlg, (HMENU)IDC_LECTURE_TEXT, g_hInst, NULL);
            if (hFont)
                SendMessageA(hEdit, WM_SETFONT, (WPARAM)hFont, TRUE);

            FormatHealthLecture(pInfo, szText, (int)sizeof(szText));
            U8ToW(szText, wz, 16384);
            SetWindowTextW(hEdit, wz);

            hBtn = CreateWindowExU8(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                (cx - btnW) / 2, cy - margin - btnH, btnW, btnH,
                hDlg, (HMENU)IDOK, g_hInst, NULL);
            if (hFont)
                SendMessageA(hBtn, WM_SETFONT, (WPARAM)hFont, TRUE);
            SetFocus(hBtn);
        }
        return 0;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
            DestroyWindow(hDlg);
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE || wParam == VK_RETURN) {
            DestroyWindow(hDlg);
            return 0;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;

    case WM_DESTROY:
        {
            HFONT hFont = (HFONT)GetPropA(hDlg, PROP_LECTURE_FONT);
            if (hFont && hFont != g_hFontNormal) {
                DeleteObject(hFont);
            }
            RemovePropA(hDlg, PROP_LECTURE_FONT);
        }
        return 0;
    }

    return DefWindowProc(hDlg, uMsg, wParam, lParam);
}

void ShowHealthLectureDialog(HWND hParent)
{
    static BOOL bRegistered = FALSE;
    const DRIVE_INFO* pInfo = NULL;
    int nX, nY, nWinW, nWinH;
    RECT rcAdj;
    HWND hDlg, hExist;

    if (g_nDriveCount <= 0 || g_nSelectedDrive < 0 ||
        g_nSelectedDrive >= g_nDriveCount) {
        MessageBoxU8(hParent, Tr(STR_NO_DRIVE), Tr(STR_WHY_TITLE),
            MB_OK | MB_ICONINFORMATION);
        return;
    }
    pInfo = &g_Drives[g_nSelectedDrive];

    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.lpfnWndProc   = HealthLectureDlgProc;
        wc.hInstance     = g_hInst;
        wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"LLHDHealthLecture";
        RegisterClassW(&wc);
        bRegistered = TRUE;
    }

    hExist = FindWindowA("LLHDHealthLecture", NULL);
    if (hExist)
        DestroyWindow(hExist);

    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        nX = rcP.left + (rcP.right  - rcP.left - UiScale(LECTURE_W)) / 2;
        nY = rcP.top  + (rcP.bottom - rcP.top  - UiScale(LECTURE_H)) / 2;
    } else {
        nX = (GetSystemMetrics(SM_CXSCREEN) - UiScale(LECTURE_W)) / 2;
        nY = (GetSystemMetrics(SM_CYSCREEN) - UiScale(LECTURE_H)) / 2;
    }

    rcAdj.left = 0;
    rcAdj.top = 0;
    rcAdj.right = UiScale(LECTURE_W);
    rcAdj.bottom = UiScale(LECTURE_H);
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                       FALSE, WS_EX_DLGMODALFRAME);
    nWinW = rcAdj.right  - rcAdj.left;
    nWinH = rcAdj.bottom - rcAdj.top;

    hDlg = CreateWindowExU8(
        WS_EX_DLGMODALFRAME,
        "LLHDHealthLecture",
        Tr(STR_LECTURE_TITLE),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hParent, NULL, g_hInst, (LPVOID)pInfo
    );
    if (!hDlg) return;

    SendMessageA(hDlg, WM_SETICON, ICON_BIG, (LPARAM)
        LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON)));
    SendMessageA(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)
        LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));

    ShowWindow(hDlg, SW_SHOW);
    UpdateWindow(hDlg);
}

static void CreateMenuBar(HWND hWnd)
{
    HMENU hOld = GetMenu(hWnd);
    HMENU hMenuBar = CreateMenu();
    HMENU hFile = CreatePopupMenu();
    HMENU hHelp;

    AppendMenuU8(hFile, MF_STRING, IDM_REPORT,     Tr(STR_SAVE_REPORT));
    AppendMenuU8(hFile, MF_STRING, IDM_LATENCY_TEST, TN("Экспресс-тест отклика...", "Express Latency Probe..."));
    AppendMenuU8(hFile, MF_STRING, IDM_FULL_READ_TEST, TN("Полный тест скорости чтения...", "Full Read Speed Benchmark..."));
    AppendMenuU8(hFile, MF_STRING, IDM_DRAM_VERIFY, TN("Тест буфера DRAM (Read-After-Write)...\tCtrl+D", "DRAM Buffer Test (Read-After-Write)...\tCtrl+D"));
    AppendMenuU8(hFile, MF_STRING, IDM_SELF_TEST, TN("Самодиагностика накопителя (DST)...\tCtrl+T", "Drive Self-Test (DST)...\tCtrl+T"));
    AppendMenuU8(hFile, MF_STRING, IDM_EJECT,      Tr(STR_EJECT_USB));
    AppendMenuU8(hFile, MF_STRING, IDM_SCREENSHOT, Tr(STR_SAVE_SHOT));
    AppendMenuU8(hFile, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(hFile, MF_STRING, IDM_EXIT,       Tr(STR_EXIT));
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)hFile, Tr(STR_FILE));

    g_hViewMenu = CreatePopupMenu();
    AppendMenuU8(g_hViewMenu,
                 MF_STRING | (g_bMonitorOn ? MF_CHECKED : MF_UNCHECKED),
                 IDM_MONITOR, TN("Мониторинг\tCtrl+M", "Monitor\tCtrl+M"));
    AppendMenuU8(g_hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_IN,  Tr(STR_ZOOM_IN));
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_OUT, Tr(STR_ZOOM_OUT));
    AppendMenuU8(g_hViewMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_100, "100%\tCtrl+0");
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_125, "125%");
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_150, "150%");
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_175, "175%");
    AppendMenuU8(g_hViewMenu, MF_STRING, IDM_ZOOM_200, "200%");
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)g_hViewMenu, Tr(STR_VIEW));
    UiSyncZoomMenu();

    g_hLangMenu = CreatePopupMenu();
    AppendMenuU8(g_hLangMenu, MF_STRING, IDM_LANG_RU, "Русский");
    AppendMenuU8(g_hLangMenu, MF_STRING, IDM_LANG_EN, "English");
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)g_hLangMenu, Tr(STR_LANGUAGE));
    CheckMenuRadioItem(g_hLangMenu, IDM_LANG_RU, IDM_LANG_EN,
                       UiLangIsEn() ? IDM_LANG_EN : IDM_LANG_RU, MF_BYCOMMAND);

    hHelp = CreatePopupMenu();
    AppendMenuU8(hHelp, MF_STRING, IDM_DONATE, Tr(STR_DONATE));
    AppendMenuU8(hHelp, MF_SEPARATOR, 0, NULL);
    AppendMenuU8(hHelp, MF_STRING, IDM_ABOUT, Tr(STR_ABOUT));
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)hHelp, Tr(STR_HELP));

    SetMenu(hWnd, hMenuBar);
    if (hOld) DestroyMenu(hOld);
}

static void ReplaceSubstringAll(char* s, int maxLen, const char* from, const char* to)
{
    char tmp[128];
    char* p;
    size_t flen, tlen;
    if (!s || !from || !to || maxLen <= 0) return;
    flen = strlen(from);
    tlen = strlen(to);
    if (flen == 0) return;
    while ((p = strstr(s, from)) != NULL) {
        size_t prefixLen = (size_t)(p - s);
        size_t restLen = strlen(p + flen);
        if (prefixLen + tlen + restLen + 1 > sizeof(tmp) ||
            prefixLen + tlen + restLen + 1 > (size_t)maxLen)
            break;
        memcpy(tmp, s, prefixLen);
        memcpy(tmp + prefixLen, to, tlen);
        memcpy(tmp + prefixLen + tlen, p + flen, restLen + 1);
        lstrcpynA(s, tmp, maxLen);
    }
}

/* szProtocol is filled at scan time. Swap the speed unit, max, and the PCIe port note. */
static void RefitProtocolLang(char* s, int n)
{
    if (!s || !s[0] || n <= 0) return;
    if (UiLangIsEn()) {
        ReplaceSubstringAll(s, n, "Гбит/с", "Gb/s");
        ReplaceSubstringAll(s, n, "макс.", "max");
        ReplaceSubstringAll(s, n, "диск", "drive");
        ReplaceSubstringAll(s, n, "порт", "port");
    } else {
        ReplaceSubstringAll(s, n, "Gb/s", "Гбит/с");
        ReplaceSubstringAll(s, n, "max", "макс.");
        ReplaceSubstringAll(s, n, "drive", "диск");
        ReplaceSubstringAll(s, n, "port", "порт");
    }
}

static void ApplyUiLanguage(HWND hWnd)
{
    HWND hList;
    int i;
    CreateMenuBar(hWnd);
    for (i = 0; i < g_nDriveCount; i++)
        RefitProtocolLang(g_Drives[i].szProtocol, (int)sizeof(g_Drives[i].szProtocol));
    SetDlgItemTextU8(hWnd, IDC_DRIVE_LIST, Tr(STR_DRIVES));
    SetDlgItemTextU8(hWnd, IDC_HEALTH_LABEL, Tr(STR_DISK_ASSESS));
    SetDlgItemTextU8(hWnd, IDC_REREAD_BTN, Tr(STR_REREAD));
    SetDlgItemTextU8(hWnd, IDC_REPORT_BTN, Tr(STR_REPORT_BTN));
    SetDlgItemTextU8(hWnd, IDC_EJECT_BTN, Tr(STR_EJECT_BTN));
    SetDlgItemTextU8(hWnd, IDC_LATENCY_BTN, TN("Тест чтения", "Read Test"));
    SetDlgItemTextU8(hWnd, IDC_DRAM_VERIFY_BTN, TN("Тест DRAM", "DRAM Test"));
    SetDlgItemTextU8(hWnd, IDC_SELF_TEST_BTN, TN("Тест DST", "Self-Test"));
    SetDlgItemTextU8(hWnd, IDC_AXIS_MEDIA_L, Tr(STR_MEDIA));
    SetDlgItemTextU8(hWnd, IDC_AXIS_IFACE_L, Tr(STR_INTERFACE));
    SetDlgItemTextU8(hWnd, IDC_AXIS_TEMPA_L, Tr(STR_TEMPERATURE));
    SetDlgItemTextU8(hWnd, IDC_MODEL_LABEL, Tr(STR_MODEL));
    SetDlgItemTextU8(hWnd, IDC_BRAND_LABEL, Tr(STR_BRAND));
    SetDlgItemTextU8(hWnd, IDC_CONTROLLER_LABEL, Tr(STR_CONTROLLER));
    SetDlgItemTextU8(hWnd, IDC_SERIAL_LABEL, Tr(STR_SERIAL));
    SetDlgItemTextU8(hWnd, IDC_FIRMWARE_LABEL, Tr(STR_FIRMWARE));
    SetDlgItemTextU8(hWnd, IDC_SIZE_LABEL, Tr(STR_CAPACITY));
    SetDlgItemTextU8(hWnd, IDC_TEMP_LABEL, Tr(STR_TEMPERATURE));
    SetDlgItemTextU8(hWnd, IDC_POH_LABEL, Tr(STR_POH));
    SetDlgItemTextU8(hWnd, IDC_ADAPTER_LABEL, Tr(STR_ADAPTER));
    SetDlgItemTextU8(hWnd, IDC_PROTOCOL_LABEL, Tr(STR_PROTOCOL));
    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        LVCOLUMNW col;
        WCHAR w[64];
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT;
        U8ToW(Tr(STR_COL_PARAM), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, 1, (LPARAM)&col);
        U8ToW(Tr(STR_COL_VALUE), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, 2, (LPARAM)&col);
        U8ToW(Tr(STR_COL_WORST), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, 3, (LPARAM)&col);
        U8ToW(Tr(STR_COL_THRESH), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, 4, (LPARAM)&col);
        U8ToW(Tr(STR_COL_DECODE), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, ATTR_COL_DECODE, (LPARAM)&col);
        U8ToW(Tr(STR_COL_STATUS), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, ATTR_COL_STATUS, (LPARAM)&col);
    }
    DrawMenuBar(hWnd);
    if (!g_bMonitorAlert) {
        if (g_bMonitorOn && g_bMonitorQuiet)
            lstrcpynA(g_szMonitor,
                      TN("Следит · без изменений", "Watching · no change"),
                      (int)sizeof(g_szMonitor));
        else
            MonitorSetIdle();
    }
    MonitorShow(hWnd);
    if (g_nDriveCount > 0)
        UpdateDriveInfo(hWnd, g_nSelectedDrive);
    UpdateAttrList(hWnd, g_nSelectedDrive);
    UpdateDriveButtons(hWnd);
    TrayUpdate(hWnd);
    InvalidateRect(hWnd, NULL, TRUE);
}

void CreateControls(HWND hWnd)
{
    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icex);

    int i;
    for (i = 0; i < MAX_DRIVES; i++) g_hDriveBtn[i] = NULL;

    HWND hDriveLabel = CreateWindowExU8(0, "STATIC", Tr(STR_DRIVES),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        6, 8, DRIVE_BTN_PANEL_W - 12, 16,
        hWnd, (HMENU)(IDC_DRIVE_LIST), g_hInst, NULL);
    SendMessage(hDriveLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    int nRightX = DRIVE_BTN_PANEL_W + 10;
    int nBarsW  = 190;

    HWND hLabel = CreateWindowExU8(0, "STATIC", Tr(STR_DISK_ASSESS),
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        nRightX, 40, nBarsW, 14,
        hWnd, (HMENU)IDC_HEALTH_LABEL, g_hInst, NULL);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    g_hHealthBar = CreateWindowExU8(WS_EX_CLIENTEDGE, "LLHDHealthBar", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        nRightX, 56, nBarsW, 48,
        hWnd, (HMENU)IDC_HEALTH_BAR_FRAME, g_hInst, NULL);

    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_REREAD),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX, 110, 90, 24,
        hWnd, (HMENU)IDC_REREAD_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_REPORT_BTN),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX + 100, 110, 90, 24,
        hWnd, (HMENU)IDC_REPORT_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_EJECT_BTN),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX, 138, 90, 24,
        hWnd, (HMENU)IDC_EJECT_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
      EnableWindow(h, FALSE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", TN("Тест чтения", "Read Test"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX + 100, 138, 90, 24,
        hWnd, (HMENU)IDC_LATENCY_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", TN("Тест DRAM", "DRAM Test"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX, 166, 90, 24,
        hWnd, (HMENU)IDC_DRAM_VERIFY_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", TN("Тест DST", "Self-Test"),
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        nRightX + 100, 166, 90, 24,
        hWnd, (HMENU)IDC_SELF_TEST_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }

    {
        static const struct { int idL; int idV; int sid; } ax[] = {
            { IDC_AXIS_MEDIA_L, IDC_AXIS_MEDIA_V, STR_MEDIA },
            { IDC_AXIS_IFACE_L, IDC_AXIS_IFACE_V, STR_INTERFACE },
            { IDC_AXIS_TEMPA_L, IDC_AXIS_TEMPA_V, STR_TEMPERATURE },
            { IDC_AXIS_ROW4_L,  IDC_AXIS_ROW4_V,  STR_WEAR },
        };
        int i;
        for (i = 0; i < 4; i++) {
            int y = 194 + 15 * i;
            HWND hL = CreateWindowExU8(0, "STATIC", Tr(ax[i].sid),
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nRightX, y, 78, 15,
                hWnd, (HMENU)(UINT_PTR)ax[i].idL, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nRightX + 80, y, nBarsW - 80, 15,
                hWnd, (HMENU)(UINT_PTR)ax[i].idV, g_hInst, NULL);
            SendMessage(hL, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            SendMessage(hV, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
        }
    }

    int nInfoX   = nRightX + nBarsW + 10;
    int nInfoY   = 36;
    int nInfoH   = 16;
    int nInfoGap = 2;
    int nLblW    = 100;
    int nValX    = nInfoX + nLblW + 4;
    int nValW    = WINDOW_W - nValX - 8;

    {
        static const struct { int idLbl; int idVal; int sid; const char* val; } rows[] = {
            { IDC_MODEL_LABEL,      IDC_MODEL_STATIC,      STR_MODEL,       "-" },
            { IDC_BRAND_LABEL,      IDC_BRAND_STATIC,      STR_BRAND,       "—" },
            { IDC_CONTROLLER_LABEL, IDC_CONTROLLER_STATIC, STR_CONTROLLER,  "—" },
            { IDC_SERIAL_LABEL,     IDC_SERIAL_STATIC,     STR_SERIAL,      "-" },
            { IDC_FIRMWARE_LABEL,   IDC_FIRMWARE_STATIC,   STR_FIRMWARE,    "-" },
            { IDC_SIZE_LABEL,       IDC_SIZE_STATIC,       STR_CAPACITY,    "-" },
            { IDC_TEMP_LABEL,       IDC_TEMP_STATIC,       STR_TEMPERATURE, "-" },
            { IDC_POH_LABEL,        IDC_POH_STATIC,        STR_POH,         "-" },
            { IDC_STATUS_LABEL,     IDC_STATUS_STATIC,     -1,              "-" },
            { IDC_PROTOCOL_LABEL,   IDC_PROTOCOL_STATIC,   STR_PROTOCOL,    "-" },
            { IDC_ADAPTER_LABEL,    IDC_ADAPTER_STATIC,    STR_ADAPTER,     "—" },
        };
        int r;
        for (r = 0; r < 11; r++) {
            int y = nInfoY + (nInfoH + nInfoGap) * r;
            HWND hL = CreateWindowExU8(0, "STATIC",
                rows[r].sid >= 0 ? Tr(rows[r].sid) : "S.M.A.R.T.",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nInfoX, y, nLblW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idLbl, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", rows[r].val,
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nValX, y, nValW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idVal, g_hInst, NULL);
            SendMessage(hL, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            SendMessage(hV, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
    }

    HWND hPred = CreateWindowExU8(0, "STATIC", "",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        nRightX, 258, 430, 17,
        hWnd, (HMENU)IDC_PREDICT_STATIC, g_hInst, NULL);

    SendMessage(hPred, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

    {
        HWND hMon = CreateWindowExU8(0, "STATIC", "",
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX,
            nRightX, 276, 430, 18,
            hWnd, (HMENU)IDC_MONITOR_STATIC, g_hInst, NULL);
        SendMessage(hMon, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    }

    HWND hList = CreateWindowExU8(
        WS_EX_CLIENTEDGE, "SysListView32", "",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        nRightX, 298, WINDOW_W - nRightX - 8, 575,
        hWnd, (HMENU)IDC_ATTR_LIST, g_hInst, NULL
    );
    SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER |
        LVS_EX_HEADERDRAGDROP);

    ListView_SetBkColor(hList, CLR_PANEL);
    ListView_SetTextBkColor(hList, CLR_ROW1);
    ListView_SetTextColor(hList, CLR_TEXT);

    {
        LVCOLUMNW col;
        WCHAR w0[16], w1[64], w2[64], w3[64], w4[64], w5[64], w6[64], w7[64];
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt  = LVCFMT_LEFT;
        AttrColsLoad();

        U8ToW("ID", w0, 16);
        col.cx = UiScale(g_attrColLogical[0]); col.pszText = w0;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        U8ToW(Tr(STR_COL_PARAM), w1, 64);
        col.cx = UiScale(g_attrColLogical[1]); col.pszText = w1;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        U8ToW(Tr(STR_COL_VALUE), w2, 64);
        col.cx = UiScale(g_attrColLogical[2]); col.pszText = w2;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
        U8ToW(Tr(STR_COL_WORST), w3, 64);
        col.cx = UiScale(g_attrColLogical[3]); col.pszText = w3;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
        U8ToW(Tr(STR_COL_THRESH), w4, 64);
        col.cx = UiScale(g_attrColLogical[4]); col.pszText = w4;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);
        U8ToW("RAW", w5, 64);
        col.cx = UiScale(g_attrColLogical[ATTR_COL_RAW]); col.pszText = w5;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_RAW, (LPARAM)&col);
        U8ToW(Tr(STR_COL_DECODE), w6, 64);
        col.cx = UiScale(g_attrColLogical[ATTR_COL_DECODE]); col.pszText = w6;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_DECODE, (LPARAM)&col);
        U8ToW(Tr(STR_COL_STATUS), w7, 64);
        col.cx = UiScale(g_attrColLogical[ATTR_COL_STATUS]); col.pszText = w7;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_STATUS, (LPARAM)&col);
        AttrColsApply(hList);
    }
}

static LRESULT HandleCtlColor(HWND hWnd, WPARAM wParam)
{
    HDC  hdc     = (HDC)wParam;
    (void)hWnd;

    HWND hSender = WindowFromDC(hdc);
    if (hSender) {
        int id = GetDlgCtrlID(hSender);
        if (id == IDC_MODEL_LABEL    || id == IDC_SERIAL_LABEL   ||
            id == IDC_FIRMWARE_LABEL || id == IDC_SIZE_LABEL      ||
            id == IDC_TEMP_LABEL     || id == IDC_POH_LABEL       ||
            id == IDC_STATUS_LABEL    ||
            id == IDC_PROTOCOL_LABEL || id == IDC_ADAPTER_LABEL ||
            id == IDC_BRAND_LABEL    || id == IDC_CONTROLLER_LABEL ||
            id == IDC_AXIS_MEDIA_L   || id == IDC_AXIS_IFACE_L ||
            id == IDC_AXIS_TEMPA_L   || id == IDC_AXIS_ROW4_L ||
            id == IDC_MODEL_STATIC   || id == IDC_SERIAL_STATIC   ||
            id == IDC_FIRMWARE_STATIC|| id == IDC_SIZE_STATIC     ||
            id == IDC_POH_STATIC     || id == IDC_STATUS_STATIC    ||
            id == IDC_PROTOCOL_STATIC || id == IDC_ADAPTER_STATIC ||
            id == IDC_BRAND_STATIC   || id == IDC_CONTROLLER_STATIC) {
            BOOL bLabel = (id == IDC_MODEL_LABEL || id == IDC_SERIAL_LABEL ||
                           id == IDC_FIRMWARE_LABEL || id == IDC_SIZE_LABEL ||
                           id == IDC_TEMP_LABEL || id == IDC_POH_LABEL ||
                           id == IDC_STATUS_LABEL ||
                           id == IDC_PROTOCOL_LABEL || id == IDC_ADAPTER_LABEL ||
                           id == IDC_BRAND_LABEL || id == IDC_CONTROLLER_LABEL ||
                           id == IDC_AXIS_MEDIA_L || id == IDC_AXIS_IFACE_L ||
                           id == IDC_AXIS_TEMPA_L || id == IDC_AXIS_ROW4_L);
            SetTextColor(hdc, bLabel ? CLR_TEXT_DIM : CLR_TEXT);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
        if (id == IDC_AXIS_MEDIA_V || id == IDC_AXIS_IFACE_V ||
            id == IDC_AXIS_TEMPA_V || id == IDC_AXIS_ROW4_V) {
            COLORREF clr = CLR_TEXT_DIM;
            if (g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount) {
                const DRIVE_INFO* pA = &g_Drives[g_nSelectedDrive];
                DRIVE_HEALTH_STATUS e = HEALTH_STATUS_UNKNOWN;
                if (id == IDC_AXIS_MEDIA_V)
                    e = pA->eReliability;
                else if (id == IDC_AXIS_IFACE_V)
                    e = pA->eInterface;
                else if (id == IDC_AXIS_TEMPA_V)
                    e = pA->eTempStatus;
                else if (pA->eType == DRIVE_TYPE_HDD || pA->nGSenseEvents >= 0)
                    e = pA->eMechanics;
                else
                    e = pA->eWear;
                if (e == HEALTH_STATUS_UNKNOWN)
                    clr = CLR_TEXT_DIM;
                else
                    clr = GetHealthStatusColor(e);
            }
            SetTextColor(hdc, clr);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
        if (id == IDC_TEMP_STATIC) {
            COLORREF clr = CLR_TEXT;
            if (g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount) {
                const DRIVE_INFO* pT = &g_Drives[g_nSelectedDrive];
                if (pT->eTempBand == TEMP_BAND_NORMAL)
                    clr = CLR_GREEN;
                else if (pT->eTempBand == TEMP_BAND_ELEVATED)
                    clr = CLR_YELLOW;
                else if (pT->eTempBand == TEMP_BAND_HIGH ||
                         pT->eTempBand == TEMP_BAND_CRITICAL)
                    clr = CLR_RED;
                else if (pT->nTemperatureC > 0) {
                    if (pT->nTemperatureC < 50)      clr = CLR_GREEN;
                    else if (pT->nTemperatureC < 60) clr = CLR_YELLOW;
                    else                             clr = CLR_RED;
                }
            }
            SetTextColor(hdc, clr);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
        if (id == IDC_MONITOR_STATIC) {
            COLORREF clr = CLR_TEXT_DIM;
            if (g_bMonitorAlert)
                clr = CLR_ORANGE;
            else if (g_bMonitorOn)
                clr = CLR_GREEN;
            SetTextColor(hdc, clr);
            SetBkColor(hdc, CLR_BG);
            return (LRESULT)g_hbrBG;
        }
    }
    SetTextColor(hdc, CLR_TEXT);
    SetBkColor(hdc, CLR_BG);
    return (LRESULT)g_hbrBG;
}

static void LayoutMainWindow(HWND hWnd)
{
    RECT rc;
    int cxClient, cyClient;
    int i, nBtnW, nStartY, nRightX, nBarsW, nInfoX;
    HWND hDriveLabel, hHl, hPred, hList;

    GetClientRect(hWnd, &rc);
    cxClient = rc.right - rc.left;
    cyClient = rc.bottom - rc.top;
    if (cxClient < 100 || cyClient < 100) return;

    nBtnW   = UiScale(DRIVE_BTN_PANEL_W - 12);
    nStartY = UiScale(40);
    for (i = 0; i < MAX_DRIVES; i++) {
        if (g_hDriveBtn[i]) {
            int nY = nStartY + i * (UiScale(DRIVE_BTN_H) + UiScale(DRIVE_BTN_GAP));
            SetWindowPos(g_hDriveBtn[i], NULL, UiScale(6), nY, nBtnW, UiScale(DRIVE_BTN_H),
                         SWP_NOZORDER);
        }
    }

    hDriveLabel = GetDlgItem(hWnd, IDC_DRIVE_LIST);
    if (hDriveLabel)
        SetWindowPos(hDriveLabel, NULL, UiScale(6), UiScale(8), nBtnW, UiScale(16), SWP_NOZORDER);

    nRightX = UiScale(DRIVE_BTN_PANEL_W + 10);
    nBarsW  = UiScale(190);

    hHl = GetDlgItem(hWnd, IDC_HEALTH_LABEL);
    if (hHl) SetWindowPos(hHl, NULL, nRightX, UiScale(40), nBarsW, UiScale(14), SWP_NOZORDER);
    if (g_hHealthBar)
        SetWindowPos(g_hHealthBar, NULL, nRightX, UiScale(56), nBarsW, UiScale(48), SWP_NOZORDER);
    {
        HWND hReread = GetDlgItem(hWnd, IDC_REREAD_BTN);
        HWND hReport = GetDlgItem(hWnd, IDC_REPORT_BTN);
        HWND hEject  = GetDlgItem(hWnd, IDC_EJECT_BTN);
        HWND hTest   = GetDlgItem(hWnd, IDC_LATENCY_BTN);
        HWND hDram   = GetDlgItem(hWnd, IDC_DRAM_VERIFY_BTN);
        HWND hDst    = GetDlgItem(hWnd, IDC_SELF_TEST_BTN);
        if (hReread) SetWindowPos(hReread, NULL, nRightX, UiScale(110), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hReport) SetWindowPos(hReport, NULL, nRightX + UiScale(100), UiScale(110), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hEject)  SetWindowPos(hEject,  NULL, nRightX, UiScale(138), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hTest)   SetWindowPos(hTest,   NULL, nRightX + UiScale(100), UiScale(138), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hDram)   SetWindowPos(hDram,   NULL, nRightX, UiScale(166), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hDst)    SetWindowPos(hDst,    NULL, nRightX + UiScale(100), UiScale(166), UiScale(90), UiScale(24), SWP_NOZORDER);
        {
            int axL[] = { IDC_AXIS_MEDIA_L, IDC_AXIS_IFACE_L,
                          IDC_AXIS_TEMPA_L, IDC_AXIS_ROW4_L };
            int axV[] = { IDC_AXIS_MEDIA_V, IDC_AXIS_IFACE_V,
                          IDC_AXIS_TEMPA_V, IDC_AXIS_ROW4_V };
            int ai;
            for (ai = 0; ai < 4; ai++) {
                HWND hL = GetDlgItem(hWnd, axL[ai]);
                HWND hV = GetDlgItem(hWnd, axV[ai]);
                int y = UiScale(194) + UiScale(15) * ai;
                if (hL) SetWindowPos(hL, NULL, nRightX, y, UiScale(78), UiScale(15), SWP_NOZORDER);
                if (hV) SetWindowPos(hV, NULL, nRightX + UiScale(80), y, nBarsW - UiScale(80), UiScale(15), SWP_NOZORDER);
            }
        }
    }

    nInfoX = nRightX + nBarsW + UiScale(10);
    {
        int nLblW2  = UiScale(100);
        int nValX2  = nInfoX + nLblW2 + UiScale(4);
        int nValW2  = cxClient - nValX2 - UiScale(8);
        int nInfoY2 = UiScale(36), nInfoH2 = UiScale(16), nInfoGap2 = UiScale(2);
        int lblIds[] = { IDC_MODEL_LABEL, IDC_BRAND_LABEL, IDC_CONTROLLER_LABEL,
                         IDC_SERIAL_LABEL, IDC_FIRMWARE_LABEL,
                         IDC_SIZE_LABEL, IDC_TEMP_LABEL, IDC_POH_LABEL, IDC_STATUS_LABEL,
                         IDC_PROTOCOL_LABEL, IDC_ADAPTER_LABEL };
        int valIds[] = { IDC_MODEL_STATIC, IDC_BRAND_STATIC, IDC_CONTROLLER_STATIC,
                         IDC_SERIAL_STATIC, IDC_FIRMWARE_STATIC,
                         IDC_SIZE_STATIC, IDC_TEMP_STATIC, IDC_POH_STATIC, IDC_STATUS_STATIC,
                         IDC_PROTOCOL_STATIC, IDC_ADAPTER_STATIC };
        int k;
        if (nValW2 < UiScale(40)) nValW2 = UiScale(40);
        for (k = 0; k < 11; k++) {
            HWND hL = GetDlgItem(hWnd, lblIds[k]);
            HWND hV = GetDlgItem(hWnd, valIds[k]);
            int y = nInfoY2 + (nInfoH2 + nInfoGap2) * k;
            if (hL) SetWindowPos(hL, NULL, nInfoX, y, nLblW2, nInfoH2, SWP_NOZORDER);
            if (hV) SetWindowPos(hV, NULL, nValX2, y, nValW2, nInfoH2, SWP_NOZORDER);
        }
    }
    hPred = GetDlgItem(hWnd, IDC_PREDICT_STATIC);
    if (hPred) SetWindowPos(hPred, NULL, nRightX, UiScale(258),
                            cxClient - nRightX - UiScale(8), UiScale(17), SWP_NOZORDER);
    {
        HWND hMon = GetDlgItem(hWnd, IDC_MONITOR_STATIC);
        if (hMon) SetWindowPos(hMon, NULL, nRightX, UiScale(276),
                               cxClient - nRightX - UiScale(8), UiScale(18), SWP_NOZORDER);
    }

    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        int nListTop = UiScale(298);
        int nListH   = cyClient - nListTop - UiScale(8);
        int nListW   = cxClient - nRightX - UiScale(8);
        if (nListH < UiScale(50)) nListH = UiScale(50);
        if (nListW < UiScale(200)) nListW = UiScale(200);
        SetWindowPos(hList, NULL, nRightX, nListTop, nListW, nListH, SWP_NOZORDER);
    }
}

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg)
    {
    case WM_CREATE:
        g_hMainWnd = hWnd;
        {
            UINT dpi = QueryDpiForWindow(hWnd);
            if (dpi >= 96) g_nDpi = (int)dpi;
        }
        RegisterHealthBarClass(g_hInst);
        CreateGDIObjects();
        MonitorLoad();
        CreateMenuBar(hWnd);
        CreateControls(hWnd);
        /* Tray icons show each disk temperature. With monitoring on, minimize
         * hides the window and the timer keeps running. Close still exits. */
        DeviceNotify_Register(hWnd);
        MonitorApplyTimer(hWnd);
        MonitorShow(hWnd);
        RefreshData(hWnd);
        {
            GdiplusStartupInput gdipInput;
            GdiplusStartup(&g_gdiplusToken, &gdipInput, NULL);
        }
        return 0;

    case WM_ERASEBKGND:
        {
            HDC hdc = (HDC)wParam;
            RECT rc;
            GetClientRect(hWnd, &rc);
            FillRect(hdc, &rc, g_hbrBG);
        }
        return 1;

    case WM_CTLCOLORSTATIC:
        return HandleCtlColor(hWnd, wParam);

    case WM_CTLCOLORBTN:
        return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        {
            HDC hdc = (HDC)wParam;
            SetTextColor(hdc, CLR_TEXT);
            SetBkColor(hdc, CLR_PANEL);
            return (LRESULT)g_hbrPanel;
        }

    case WM_NOTIFY:
        {
            NMHDR* pHdr = (NMHDR*)lParam;
            /* Status badge is ATTR_COL_STATUS. Code comes from lItemlParam
             * (set at fill time). Never CreateFont here. */
            {
                HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
                HWND hHdr  = hList ? ListView_GetHeader(hList) : NULL;
                if (hHdr && pHdr->hwndFrom == hHdr && pHdr->code == NM_CUSTOMDRAW) {
                    NMCUSTOMDRAW* pCD = (NMCUSTOMDRAW*)lParam;
                    if (pCD->dwDrawStage == CDDS_PREPAINT)
                        return CDRF_NOTIFYITEMDRAW;
                    if (pCD->dwDrawStage == CDDS_ITEMPREPAINT) {
                        HDC hdc = pCD->hdc;
                        HBRUSH hbr = CreateSolidBrush(CLR_HEADER);
                        FillRect(hdc, &pCD->rc, hbr);
                        DeleteObject(hbr);
                        HPEN hp = CreatePen(PS_SOLID, 1, CLR_BORDER);
                        HPEN hpOld = (HPEN)SelectObject(hdc, hp);
                        MoveToEx(hdc, pCD->rc.left, pCD->rc.bottom - 1, NULL);
                        LineTo(hdc, pCD->rc.right, pCD->rc.bottom - 1);
                        SelectObject(hdc, hpOld);
                        DeleteObject(hp);
                        WCHAR wz[64];
                        HDITEMW hi;
                        ZeroMemory(&hi, sizeof(hi));
                        hi.mask = HDI_TEXT;
                        hi.pszText = wz;
                        hi.cchTextMax = 64;
                        SendMessageW(hHdr, HDM_GETITEMW, pCD->dwItemSpec, (LPARAM)&hi);
                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, CLR_TEXT);
                        RECT rcT = pCD->rc;
                        HFONT hOldHdr = (HFONT)SelectObject(hdc, g_hFontSmall);
                        rcT.left += UiScale(6);
                        DrawTextW(hdc, wz, -1, &rcT,
                                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                        SelectObject(hdc, hOldHdr);
                        return CDRF_SKIPDEFAULT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            if (pHdr->idFrom == IDC_ATTR_LIST && pHdr->code == NM_CUSTOMDRAW) {
                NMLVCUSTOMDRAW* pCD = (NMLVCUSTOMDRAW*)lParam;
                switch (pCD->nmcd.dwDrawStage) {
                case CDDS_PREPAINT:
                    return CDRF_NOTIFYITEMDRAW;
                case CDDS_ITEMPREPAINT:
                    pCD->clrTextBk = (pCD->nmcd.dwItemSpec % 2 == 0) ? CLR_ROW1 : CLR_ROW2;
                    pCD->clrText   = CLR_TEXT;
                    return CDRF_NOTIFYSUBITEMDRAW;
                case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
                    {
                        COLORREF clrRowBk = (pCD->nmcd.dwItemSpec % 2 == 0)
                                            ? CLR_ROW1 : CLR_ROW2;
                        pCD->clrTextBk = clrRowBk;
                        pCD->clrText   = CLR_TEXT;
                        if (pCD->iSubItem != ATTR_COL_STATUS)
                            return CDRF_NEWFONT;

                        int nSt = (int)(pCD->nmcd.lItemlParam & 0xFF);
                        RECT rcCell;
                        /* nmcd.rc for a subitem sits above the row. The list
                         * cell rect is in the same coordinates as this HDC. */
                        if (!ListView_GetSubItemRect((HWND)pCD->nmcd.hdr.hwndFrom,
                                                     (int)pCD->nmcd.dwItemSpec,
                                                     ATTR_COL_STATUS, LVIR_BOUNDS,
                                                     &rcCell))
                            rcCell = pCD->nmcd.rc;
                        if (nSt == ATTRST_NONE || (rcCell.right - rcCell.left) < 8)
                            return CDRF_DODEFAULT;

                        COLORREF clrBadgeBg;
                        const char* psz;
                        int nAlt = (int)(pCD->nmcd.lItemlParam & 0x100);
                        switch (nSt) {
                        case ATTRST_BAD:
                            clrBadgeBg = CLR_RED;
                            psz = nAlt ? Tr(STR_ST_BAD) : Tr(STR_ST_FAIL);
                            break;
                        case ATTRST_WARN:
                            clrBadgeBg = CLR_ORANGE;
                            psz = nAlt ? Tr(STR_ST_HOT) : Tr(STR_ST_WARN);
                            break;
                        case ATTRST_RISK:
                            clrBadgeBg = CLR_YELLOW;
                            psz = Tr(STR_ST_RISK);
                            break;
                        case ATTRST_OK:
                            clrBadgeBg = CLR_GREEN;
                            psz = Tr(STR_ST_OK);
                            break;
                        case ATTRST_DIM:
                            clrBadgeBg = RGB(148, 163, 184);
                            psz = Tr(STR_ST_DIM);
                            break;
                        case ATTRST_SKIP:
                            clrBadgeBg = RGB(148, 163, 184);
                            psz = Tr(STR_ST_SKIP);
                            break;
                        case ATTRST_PAST:
                            clrBadgeBg = RGB(100, 116, 139);
                            psz = Tr(STR_ST_PAST);
                            break;
                        case ATTRST_INFO:
                            clrBadgeBg = RGB(71, 99, 128);
                            psz = Tr(STR_ST_INFO);
                            break;
                        case ATTRST_POWERLOG:
                            clrBadgeBg = RGB(71, 99, 128);
                            psz = Tr(STR_ST_POWERLOG);
                            break;
                        default:
                            return CDRF_DODEFAULT;
                        }

                        HDC hdc = pCD->nmcd.hdc;
                        HBRUSH hbrRow = CreateSolidBrush(clrRowBk);
                        FillRect(hdc, &rcCell, hbrRow);
                        DeleteObject(hbrRow);

                        HFONT hOldFont = (HFONT)SelectObject(hdc, g_hFontSmall);
                        WCHAR wz[40];
                        if (nSt == ATTRST_INFO || nSt == ATTRST_POWERLOG) {
                            LVITEMW li;
                            ZeroMemory(&li, sizeof(li));
                            li.mask = LVIF_TEXT;
                            li.iItem = (int)pCD->nmcd.dwItemSpec;
                            li.iSubItem = ATTR_COL_STATUS;
                            li.pszText = wz;
                            li.cchTextMax = 40;
                            if (!SendMessageW(pCD->nmcd.hdr.hwndFrom, LVM_GETITEMW,
                                              0, (LPARAM)&li) || !wz[0])
                                U8ToW(psz, wz, 40);
                        } else {
                            U8ToW(psz, wz, 40);
                        }
                        SIZE sz;
                        GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);

                        int cellCX  = rcCell.right  - rcCell.left;
                        int cellCY  = rcCell.bottom - rcCell.top;
                        int badgeH  = sz.cy + UiScale(4);
                        int badgeW  = sz.cx + UiScale((nSt == ATTRST_SKIP) ? 20 : 16);
                        if (badgeH > cellCY) badgeH = cellCY;
                        if (badgeW > cellCX - 2) badgeW = cellCX - 2;
                        if (badgeW < 4) badgeW = 4;
                        int bx      = rcCell.left + (cellCX - badgeW) / 2;
                        int by      = rcCell.top  + (cellCY - badgeH) / 2;
                        if (bx < rcCell.left) bx = rcCell.left;
                        if (by < rcCell.top) by = rcCell.top;
                        RECT rcBadge = { bx, by, bx + badgeW, by + badgeH };

                        HBRUSH hbrBadge = CreateSolidBrush(clrBadgeBg);
                        HPEN   hpBorder = CreatePen(PS_SOLID, 1, clrBadgeBg);
                        HBRUSH hbrOld   = (HBRUSH)SelectObject(hdc, hbrBadge);
                        HPEN   hpOld    = (HPEN)SelectObject(hdc, hpBorder);
                        RoundRect(hdc, rcBadge.left, rcBadge.top,
                                       rcBadge.right, rcBadge.bottom,
                                       UiScale(8), UiScale(8));
                        SelectObject(hdc, hbrOld);
                        SelectObject(hdc, hpOld);
                        DeleteObject(hbrBadge);
                        DeleteObject(hpBorder);

                        SetBkMode(hdc, TRANSPARENT);
                        SetTextColor(hdc, RGB(255, 255, 255));
                        /* INFO and power-log share a color but not a label.
                         * Paint the list text: psz for INFO is always «контекст». */
                        if (nSt == ATTRST_INFO || nSt == ATTRST_POWERLOG)
                            DrawTextW(hdc, wz, -1, &rcBadge,
                                      DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                        else
                            DrawTextU8(hdc, psz, &rcBadge,
                                       DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

                        SelectObject(hdc, hOldFont);
                        return CDRF_SKIPDEFAULT;
                    }
                default:
                    return CDRF_DODEFAULT;
                }
            }
        }
        return 0;

    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) {
            KillTimer(hWnd, IDT_HOTPLUG);
            SetTimer(hWnd, IDT_HOTPLUG, HOTPLUG_DELAY_MS, NULL);
        }
        return TRUE;

    case WM_KEYDOWN:
        if (wParam == 'S' && (GetKeyState(VK_CONTROL) & 0x8000))
            DoSaveScreenshot(hWnd);
        return 0;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        return 0;

    case WM_COMMAND:
        {
            int nCtrl = LOWORD(wParam);

            if (nCtrl >= IDC_DRIVE_BTN_BASE && nCtrl < IDC_DRIVE_BTN_BASE + MAX_DRIVES) {
                int nSel = nCtrl - IDC_DRIVE_BTN_BASE;
                if (nSel >= 0 && nSel < g_nDriveCount) {
                    g_nSelectedDrive = nSel;
                    int i;
                    for (i = 0; i < g_nDriveCount; i++)
                        if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);
                    UpdateDriveInfo(hWnd, nSel);
                    UpdateAttrList(hWnd, nSel);
                    InvalidateRect(hWnd, NULL, FALSE);
                    UpdateWindow(hWnd);
                }
            }
            else if (nCtrl == IDC_REREAD_BTN) {
                RefreshData(hWnd);  /* full one-shot scan, incl. RTL9210 0xE4 */
            }
            else if (nCtrl == IDC_REPORT_BTN || nCtrl == IDM_REPORT) {
                DoSaveDriveReport(hWnd);
            }
            else if (nCtrl == IDC_EJECT_BTN || nCtrl == IDM_EJECT) {
                DoSafeEject(hWnd);
            }
            else if (nCtrl == IDC_LATENCY_BTN || nCtrl == IDM_LATENCY_TEST) {
                ShowLatencyProbeDialogEx(hWnd, g_nSelectedDrive, LATENCY_MODE_EXPRESS);
            }
            else if (nCtrl == IDM_FULL_READ_TEST) {
                ShowLatencyProbeDialogEx(hWnd, g_nSelectedDrive, LATENCY_MODE_FULL);
            }
            else if (nCtrl == IDC_DRAM_VERIFY_BTN || nCtrl == IDM_DRAM_VERIFY) {
                ShowDramVerifyDialog(hWnd, g_nSelectedDrive);
            }
            else if (nCtrl == IDC_SELF_TEST_BTN || nCtrl == IDM_SELF_TEST) {
                ShowSelfTestDialog(hWnd, g_nSelectedDrive);
            }



            else if (nCtrl == IDM_SCREENSHOT) {
                DoSaveScreenshot(hWnd);
            }
            else if (nCtrl == IDM_ABOUT) {
                ShowAboutDialog(hWnd);
            }
            else if (nCtrl == IDM_DONATE) {
                OpenDonatePage(hWnd);
            }
            else if (nCtrl == IDM_EXIT) {
                DestroyWindow(hWnd);
            }
            else if (nCtrl == IDM_ZOOM_IN) {
                UiChangeZoom(hWnd, g_nZoom + 25);
            }
            else if (nCtrl == IDM_ZOOM_OUT) {
                UiChangeZoom(hWnd, g_nZoom - 25);
            }
            else if (nCtrl == IDM_ZOOM_100) {
                UiChangeZoom(hWnd, 100);
            }
            else if (nCtrl == IDM_ZOOM_125) {
                UiChangeZoom(hWnd, 125);
            }
            else if (nCtrl == IDM_ZOOM_150) {
                UiChangeZoom(hWnd, 150);
            }
            else if (nCtrl == IDM_ZOOM_175) {
                UiChangeZoom(hWnd, 175);
            }
            else if (nCtrl == IDM_ZOOM_200) {
                UiChangeZoom(hWnd, 200);
            }
            else if (nCtrl == IDM_LANG_RU) {
                UiSetLang(UI_LANG_RU);
                ApplyUiLanguage(hWnd);
            }
            else if (nCtrl == IDM_LANG_EN) {
                UiSetLang(UI_LANG_EN);
                ApplyUiLanguage(hWnd);
            }
            else if (nCtrl == IDM_MONITOR) {
                MonitorToggle(hWnd);
            }
        }
        return 0;

    case WM_APP_REFRESH_DONE:
        {
            int i;

            /* UI thread only. Worker has exited (or ran inline); g_ScanBuf is
             * stable. Snapshot g_Drives, then publish the scan buffer. Busy
             * stays 1 until after memcpy so RefreshData cannot launch another
             * writer into g_ScanBuf during this copy. */
            Snapshot_Save();

            memcpy(g_Drives, g_ScanBuf, sizeof(DRIVE_INFO) * MAX_DRIVES);
            g_nDriveCount = g_nScanBufCount;

            Snapshot_Diff();

            InterlockedExchange(&g_bScanBusy, 0);
            SetActionButtonsEnabled(hWnd, TRUE);

            UpdateDriveButtons(hWnd);

            if (g_nSelectedDrive >= g_nDriveCount) g_nSelectedDrive = 0;

            UpdateDriveInfo(hWnd, g_nSelectedDrive);
            UpdateAttrList(hWnd, g_nSelectedDrive);
            RepaintHealthBar();

            for (i = 0; i < g_nDriveCount; i++)
                if (g_hDriveBtn[i]) InvalidateRect(g_hDriveBtn[i], NULL, TRUE);

            InvalidateRect(hWnd, NULL, FALSE);
            UpdateWindow(hWnd);
            TrayUpdate(hWnd);
        }
        return 0;

    case WM_TRAYICON:
        TrayOnClick(hWnd, (int)wParam - 1, (UINT)lParam);
        return 0;

    case WM_SYSCOMMAND:
        /* Minimize goes to the tray only while monitoring is on. */
        if (g_bMonitorOn && (wParam & 0xFFF0) == SC_MINIMIZE) {
            ShowWindow(hWnd, SW_HIDE);
            return 0;
        }
        break;

    case WM_TIMER:
        if (wParam == IDT_HOTPLUG) {
            KillTimer(hWnd, IDT_HOTPLUG);
            RefreshData(hWnd);    /* one-shot full scan on plug/unplug */
        } else if (wParam == IDT_MONITOR) {
            RefreshData(hWnd);    /* skipped while a scan is already running */
        }
        return 0;

    case WM_GETMINMAXINFO:
        {
            MINMAXINFO* pmmi = (MINMAXINFO*)lParam;
            pmmi->ptMinTrackSize.x = UiWindowW();
            pmmi->ptMinTrackSize.y = UiWindowHMin();
        }
        return 0;

    case WM_DPICHANGED:
        UiOnDpiChanged(hWnd, (UINT)HIWORD(wParam), (const RECT*)lParam);
        TrayUpdate(hWnd);
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            LayoutMainWindow(hWnd);
        return 0;

    case WM_DESTROY:
        AttrColsSave(hWnd);
        UiSaveWindowPlace(hWnd);
        KillTimer(hWnd, IDT_HOTPLUG);
        KillTimer(hWnd, IDT_MONITOR);
        TrayRemoveFrom(hWnd, 0);
        DeviceNotify_Unregister();
        DestroyGDIObjects();
        if (g_gdiplusToken) { GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

/* ============================================================================
 * Latency Probe and Full Read Speed Benchmark Dialog
 * ============================================================================ */

#define IDC_LAT_BANNER         5001
#define IDC_LAT_GRP            5002
#define IDC_LAT_MODE_EXPRESS   5003
#define IDC_LAT_MODE_FULL      5004
#define IDC_LAT_PROGRESS_BAR   5005
#define IDC_LAT_STATUS_STATIC  5006
#define IDC_LAT_GRID           5007
#define IDC_LAT_LEGEND         5008
#define IDC_LAT_VERDICT        5009
#define IDC_LAT_START_BTN      5010
#define IDC_LAT_STOP_BTN       5011
#define IDC_LAT_COPY_BTN       5012
#define IDC_LAT_CLOSE_BTN      5013

#define WM_LAT_PROGRESS        (WM_APP + 21)
#define WM_LAT_FINISHED        (WM_APP + 22)

typedef struct _LATENCY_DLG_STATE {
    HWND                  hDlg;
    int                   nDriveIndex;
    uint64_t              nCapacityBytes;
    bool                  bIsSsd;
    char                  szModel[128];
    char                  szSerial[64];
    LATENCY_PROBE_MODE    eMode;
    volatile bool         bRunning;
    volatile bool         bCancel;
    HANDLE                hThread;
    int                   nCurrentActiveCell;
    LATENCY_PROBE_RESULT  result;
    DWORD                 dwLastProgressTick;
} LATENCY_DLG_STATE;

static void CopyTextToClipboard(HWND hWnd, const char* pszUtf8)
{
    if (!pszUtf8 || !pszUtf8[0]) return;
    int cchW = MultiByteToWideChar(CP_UTF8, 0, pszUtf8, -1, NULL, 0);
    if (cchW <= 0) return;
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)cchW * sizeof(WCHAR));
    if (!hMem) return;
    WCHAR* pW = (WCHAR*)GlobalLock(hMem);
    if (!pW) { GlobalFree(hMem); return; }
    MultiByteToWideChar(CP_UTF8, 0, pszUtf8, -1, pW, cchW);
    GlobalUnlock(hMem);
    if (OpenClipboard(hWnd)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, hMem);
        CloseClipboard();
    } else {
        GlobalFree(hMem);
    }
}

static LRESULT CALLBACK LatencyGridWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    switch (uMsg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);

        HBRUSH hBg = CreateSolidBrush(RGB(246, 248, 250));
        FillRect(memDC, &rc, hBg);
        DeleteObject(hBg);

        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hWnd, "DM_LAT_STATE");
        const LATENCY_PROBE_RESULT* pRes = pState ? &pState->result : NULL;

        int cols = 16;
        int rows = 8;
        int padX = UiScale(6);
        int padY = UiScale(6);
        int availW = w - 2 * padX;
        int availH = h - 2 * padY;
        int gap = UiScale(2);

        int cellW = (availW - (cols - 1) * gap) / cols;
        int cellH = (availH - (rows - 1) * gap) / rows;

        HPEN hEmptyPen = CreatePen(PS_SOLID, 1, RGB(220, 224, 230));
        HBRUSH hEmptyBrush = CreateSolidBrush(RGB(236, 239, 242));

        for (int r = 0; r < rows; r++) {
            for (int c = 0; c < cols; c++) {
                int idx = r * cols + c;
                int x1 = padX + c * (cellW + gap);
                int y1 = padY + r * (cellH + gap);
                int x2 = x1 + cellW;
                int y2 = y1 + cellH;

                if (pRes && idx < pRes->nSamplesTested) {
                    COLORREF clr = LatencyBinColor(pRes->samples[idx].bin);
                    HBRUSH hCellBrush = CreateSolidBrush(clr);
                    HPEN hCellPen = CreatePen(PS_SOLID, 1, clr);
                    HGDIOBJ oB = SelectObject(memDC, hCellBrush);
                    HGDIOBJ oP = SelectObject(memDC, hCellPen);
                    Rectangle(memDC, x1, y1, x2, y2);
                    SelectObject(memDC, oP);
                    SelectObject(memDC, oB);
                    DeleteObject(hCellPen);
                    DeleteObject(hCellBrush);
                } else {
                    HGDIOBJ oB = SelectObject(memDC, hEmptyBrush);
                    HGDIOBJ oP = SelectObject(memDC, hEmptyPen);
                    Rectangle(memDC, x1, y1, x2, y2);
                    SelectObject(memDC, oP);
                    SelectObject(memDC, oB);
                }

                if (pState && pState->bRunning && idx == pState->nCurrentActiveCell) {
                    HPEN hActivePen = CreatePen(PS_SOLID, 2, RGB(30, 130, 240));
                    HGDIOBJ oP = SelectObject(memDC, hActivePen);
                    HGDIOBJ oB = SelectObject(memDC, GetStockObject(NULL_BRUSH));
                    Rectangle(memDC, x1 - 1, y1 - 1, x2 + 1, y2 + 1);
                    SelectObject(memDC, oB);
                    SelectObject(memDC, oP);
                    DeleteObject(hActivePen);
                }
            }
        }

        DeleteObject(hEmptyPen);
        DeleteObject(hEmptyBrush);

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK LatencyLegendWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    switch (uMsg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);

        HBRUSH hBg = CreateSolidBrush(GetSysColor(COLOR_BTNFACE));
        FillRect(memDC, &rc, hBg);
        DeleteObject(hBg);

        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hWnd, "DM_LAT_STATE");
        const LATENCY_PROBE_RESULT* pRes = pState ? &pState->result : NULL;

        SetBkMode(memDC, TRANSPARENT);
        SelectObject(memDC, g_hFontSmall);

        int cols = 6;
        int colW = w / cols;
        int boxSize = UiScale(12);
        int yBox = (h - boxSize) / 2;

        for (int i = 0; i < LATENCY_BIN_COUNT; i++) {
            LATENCY_BIN b = (LATENCY_BIN)i;
            int xBox = i * colW + UiScale(4);

            HBRUSH hBoxBrush = CreateSolidBrush(LatencyBinColor(b));
            HPEN hBoxPen = CreatePen(PS_SOLID, 1, RGB(160, 160, 160));
            HGDIOBJ oB = SelectObject(memDC, hBoxBrush);
            HGDIOBJ oP = SelectObject(memDC, hBoxPen);
            Rectangle(memDC, xBox, yBox, xBox + boxSize, yBox + boxSize);
            SelectObject(memDC, oP);
            SelectObject(memDC, oB);
            DeleteObject(hBoxPen);
            DeleteObject(hBoxBrush);

            char szLabel[64];
            int count = pRes ? pRes->nBins[i] : 0;
            safe_snprintf(szLabel, "%s: %d", LatencyBinRange(b), count);

            WCHAR wzLabel[64];
            U8ToW(szLabel, wzLabel, 64);
            RECT textRc = { xBox + boxSize + UiScale(4), 0, (i + 1) * colW - UiScale(2), h };
            SetTextColor(memDC, (b >= LATENCY_BIN_SLOW && count > 0) ? LatencyBinColor(b) : RGB(40, 44, 48));
            DrawTextW(memDC, wzLabel, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static LRESULT CALLBACK LatencyVerdictWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    (void)wParam; (void)lParam;
    switch (uMsg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);

        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hWnd, "DM_LAT_STATE");
        const LATENCY_PROBE_RESULT* pRes = pState ? &pState->result : NULL;

        COLORREF bgClr = RGB(255, 255, 255);
        COLORREF borderClr = RGB(218, 222, 228);
        COLORREF badgeBg = RGB(240, 243, 246);
        COLORREF badgeTextClr = RGB(60, 64, 67);

        if (pRes && pRes->szVerdictBadge[0] != '\0') {
            switch (pRes->eVerdictLevel) {
            case LATENCY_VERDICT_EXCELLENT:
                bgClr = RGB(250, 255, 250);
                borderClr = RGB(163, 230, 184);
                badgeBg = RGB(230, 248, 235);
                badgeTextClr = RGB(20, 108, 48);
                break;
            case LATENCY_VERDICT_GOOD:
                bgClr = RGB(250, 253, 255);
                borderClr = RGB(179, 212, 255);
                badgeBg = RGB(235, 245, 255);
                badgeTextClr = RGB(24, 100, 171);
                break;
            case LATENCY_VERDICT_CAUTION:
                bgClr = RGB(255, 254, 248);
                borderClr = RGB(249, 226, 150);
                badgeBg = RGB(254, 249, 231);
                badgeTextClr = RGB(183, 121, 31);
                break;
            case LATENCY_VERDICT_WARNING:
                bgClr = RGB(255, 250, 249);
                borderClr = RGB(252, 194, 178);
                badgeBg = RGB(254, 237, 232);
                badgeTextClr = RGB(192, 57, 43);
                break;
            case LATENCY_VERDICT_BAD:
                bgClr = RGB(253, 248, 255);
                borderClr = RGB(218, 185, 235);
                badgeBg = RGB(245, 235, 250);
                badgeTextClr = RGB(120, 40, 150);
                break;
            }
        }

        HBRUSH hBgBrush = CreateSolidBrush(bgClr);
        FillRect(memDC, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        HPEN hPen = CreatePen(PS_SOLID, 1, borderClr);
        HGDIOBJ oldPen = SelectObject(memDC, hPen);
        HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, 0, 0, w, h);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(hPen);

        SetBkMode(memDC, TRANSPARENT);

        if (pRes && pRes->szVerdictBadge[0] != '\0') {
            RECT badgeRc = { UiScale(14), UiScale(12), UiScale(210), UiScale(38) };
            HBRUSH hBadgeBrush = CreateSolidBrush(badgeBg);
            HPEN hBadgePen = CreatePen(PS_SOLID, 1, borderClr);
            HGDIOBJ p1 = SelectObject(memDC, hBadgeBrush);
            HGDIOBJ p2 = SelectObject(memDC, hBadgePen);
            RoundRect(memDC, badgeRc.left, badgeRc.top, badgeRc.right, badgeRc.bottom, UiScale(6), UiScale(6));
            SelectObject(memDC, p2);
            SelectObject(memDC, p1);
            DeleteObject(hBadgeBrush);
            DeleteObject(hBadgePen);

            SetTextColor(memDC, badgeTextClr);
            SelectObject(memDC, g_hFontNormal);
            WCHAR wzBadge[64];
            U8ToW(pRes->szVerdictBadge, wzBadge, 64);
            DrawTextW(memDC, wzBadge, -1, &badgeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT textRc = { UiScale(14), UiScale(46), w - UiScale(14), h - UiScale(8) };
            SetTextColor(memDC, RGB(40, 44, 48));
            SelectObject(memDC, g_hFontNormal);
            WCHAR wzText[1024];
            U8ToW(pRes->szVerdictText, wzText, 1024);
            DrawTextW(memDC, wzText, -1, &textRc, DT_LEFT | DT_WORDBREAK);
        } else {
            RECT textRc = { UiScale(16), UiScale(14), w - UiScale(16), h - UiScale(12) };
            SetTextColor(memDC, RGB(110, 115, 122));
            SelectObject(memDC, g_hFontNormal);
            const char* szPrompt = TN(
                "Диагностический вердикт и физическая оценка состояния диска появятся здесь после запуска теста.\r\n\r\n"
                "• Экспресс-тест (128 точек) быстро выявляет деградацию Flash-памяти (утечку заряда, LDPC Soft Decision) или износ пластин/головок HDD.\r\n"
                "• Полный тест (100% поверхности) выполняет поблочное линейное чтение, измеряет скорость в МБ/с и вычисляет оставшееся время.",
                "Diagnostic verdict and physical drive health assessment will appear here after the test starts.\r\n\r\n"
                "• Express probe (128 samples) rapidly detects NAND flash charge loss, LDPC Soft Decision latency, or HDD platter/head wear.\r\n"
                "• Full test (100% surface) performs sequential block reading, measures true MB/s transfer rate, and tracks ETA."
            );
            WCHAR wzPrompt[1024];
            U8ToW(szPrompt, wzPrompt, 1024);
            DrawTextW(memDC, wzPrompt, -1, &textRc, DT_LEFT | DT_WORDBREAK);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static bool LatencyProbeDlgCallback(int nCurrentSample, int nTotalSamples,
                                    const LATENCY_SAMPLE* pSample, void* pUserData)
{
    LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)pUserData;
    if (!pState || pState->bCancel) return false;

    pState->nCurrentActiveCell = nCurrentSample;

    DWORD now = GetTickCount();
    bool bAbnormal = (pSample && pSample->bin >= LATENCY_BIN_SLOW);
    if (bAbnormal || (now - pState->dwLastProgressTick >= 60) || nCurrentSample == nTotalSamples - 1) {
        pState->dwLastProgressTick = now;
        PostMessage(pState->hDlg, WM_LAT_PROGRESS, (WPARAM)nCurrentSample, (LPARAM)nTotalSamples);
    }
    return !pState->bCancel;
}

static DWORD WINAPI LatencyProbeThreadProc(LPVOID lpParam)
{
    LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)lpParam;
    if (!pState) return 0;

    RunLatencyProbeEx(pState->nDriveIndex,
                       pState->nCapacityBytes,
                       pState->eMode,
                       128,
                       &pState->result,
                       LatencyProbeDlgCallback,
                       pState,
                       &pState->bCancel);

    LatencyProbeAssessVerdict(&pState->result, pState->bIsSsd);
    PostMessage(pState->hDlg, WM_LAT_FINISHED, 0, 0);
    return 0;
}

static LRESULT CALLBACK LatencyDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)cs->lpCreateParams;
        if (!pState) return -1;
        pState->hDlg = hDlg;
        SetPropA(hDlg, "DM_LAT_STATE", (HANDLE)pState);

        RECT rc;
        GetClientRect(hDlg, &rc);
        int cx = rc.right - rc.left;
        int cy = rc.bottom - rc.top;

        int margin = UiScale(14);
        int w = cx - 2 * margin;

        char szBanner[512];
        double capGb = (double)pState->nCapacityBytes / (1024.0 * 1024.0 * 1024.0);
        safe_snprintf(szBanner, "%s: %s  ·  SN: %s  ·  %.1f GB (%s)",
            TN("Диск", "Drive"), pState->szModel,
            pState->szSerial[0] ? pState->szSerial : "—",
            capGb, pState->bIsSsd ? "SSD" : "HDD");
        HWND hBanner = CreateWindowExU8(0, "STATIC", szBanner,
            WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
            margin, UiScale(12), w, UiScale(24),
            hDlg, (HMENU)IDC_LAT_BANNER, g_hInst, NULL);
        SendMessage(hBanner, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

        HWND hGrp = CreateWindowExU8(0, "BUTTON", TN("Режим тестирования", "Benchmark Mode"),
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            margin, UiScale(42), w, UiScale(58),
            hDlg, (HMENU)IDC_LAT_GRP, g_hInst, NULL);
        SendMessage(hGrp, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        int radW = (w - UiScale(28)) / 2;
        HWND hRad1 = CreateWindowExU8(0, "BUTTON",
            TN("Экспресс-тест отклика (128 точек, ~1-2 сек)", "Express latency probe (128 samples, ~1-2 s)"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            margin + UiScale(14), UiScale(64), radW, UiScale(24),
            hDlg, (HMENU)IDC_LAT_MODE_EXPRESS, g_hInst, NULL);
        SendMessage(hRad1, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hRad2 = CreateWindowExU8(0, "BUTTON",
            TN("Полный тест скорости (100% поверхности, по 1 МиБ)", "Full read speed test (100% surface, 1 MiB blocks)"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            margin + UiScale(14) + radW + UiScale(10), UiScale(64), radW, UiScale(24),
            hDlg, (HMENU)IDC_LAT_MODE_FULL, g_hInst, NULL);
        SendMessage(hRad2, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        if (pState->eMode == LATENCY_MODE_FULL) {
            CheckRadioButton(hDlg, IDC_LAT_MODE_EXPRESS, IDC_LAT_MODE_FULL, IDC_LAT_MODE_FULL);
        } else {
            CheckRadioButton(hDlg, IDC_LAT_MODE_EXPRESS, IDC_LAT_MODE_FULL, IDC_LAT_MODE_EXPRESS);
        }

        HWND hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE,
            margin, UiScale(108), w, UiScale(16),
            hDlg, (HMENU)IDC_LAT_PROGRESS_BAR, g_hInst, NULL);
        SendMessage(hProgress, PBM_SETRANGE32, 0, 1000);

        HWND hStatus = CreateWindowExU8(0, "STATIC",
            TN("Готов к запуску. Выберите режим и нажмите «Запустить тест».",
               "Ready. Select mode and click 'Start Test'."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            margin, UiScale(130), w, UiScale(20),
            hDlg, (HMENU)IDC_LAT_STATUS_STATIC, g_hInst, NULL);
        SendMessage(hStatus, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        HWND hGrid = CreateWindowExW(WS_EX_CLIENTEDGE, L"DriveMonitorLatencyGrid", L"",
            WS_CHILD | WS_VISIBLE,
            margin, UiScale(154), w, UiScale(176),
            hDlg, (HMENU)IDC_LAT_GRID, g_hInst, NULL);
        SetPropA(hGrid, "DM_LAT_STATE", (HANDLE)pState);

        HWND hLegend = CreateWindowExW(0, L"DriveMonitorLatencyLegend", L"",
            WS_CHILD | WS_VISIBLE,
            margin, UiScale(336), w, UiScale(32),
            hDlg, (HMENU)IDC_LAT_LEGEND, g_hInst, NULL);
        SetPropA(hLegend, "DM_LAT_STATE", (HANDLE)pState);

        HWND hVerdict = CreateWindowExW(WS_EX_CLIENTEDGE, L"DriveMonitorLatencyVerdict", L"",
            WS_CHILD | WS_VISIBLE,
            margin, UiScale(374), w, UiScale(186),
            hDlg, (HMENU)IDC_LAT_VERDICT, g_hInst, NULL);
        SetPropA(hVerdict, "DM_LAT_STATE", (HANDLE)pState);

        int btnY = cy - UiScale(46);
        int btnH = UiScale(32);
        HWND hStart = CreateWindowExU8(0, "BUTTON", TN("Запустить тест", "Start Test"),
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
            margin, btnY, UiScale(140), btnH,
            hDlg, (HMENU)IDC_LAT_START_BTN, g_hInst, NULL);
        SendMessage(hStart, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hStop = CreateWindowExU8(0, "BUTTON", TN("Остановить", "Stop"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            margin + UiScale(150), btnY, UiScale(110), btnH,
            hDlg, (HMENU)IDC_LAT_STOP_BTN, g_hInst, NULL);
        SendMessage(hStop, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        EnableWindow(hStop, FALSE);

        HWND hCopy = CreateWindowExU8(0, "BUTTON", TN("Копировать отчёт", "Copy Report"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            cx - margin - UiScale(250), btnY, UiScale(140), btnH,
            hDlg, (HMENU)IDC_LAT_COPY_BTN, g_hInst, NULL);
        SendMessage(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hClose = CreateWindowExU8(0, "BUTTON", TN("Закрыть", "Close"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON | WS_TABSTOP,
            cx - margin - UiScale(100), btnY, UiScale(100), btnH,
            hDlg, (HMENU)IDC_LAT_CLOSE_BTN, g_hInst, NULL);
        SendMessage(hClose, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        if (pState->result.nSamplesTested > 0) {
            PostMessage(hDlg, WM_LAT_FINISHED, 0, 0);
        }
        return 0;
    }

    case WM_LAT_PROGRESS:
    {
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hDlg, "DM_LAT_STATE");
        if (!pState) return 0;
        HWND hProgress = GetDlgItem(hDlg, IDC_LAT_PROGRESS_BAR);
        HWND hGrid     = GetDlgItem(hDlg, IDC_LAT_GRID);
        HWND hLegend   = GetDlgItem(hDlg, IDC_LAT_LEGEND);

        if (pState->result.eMode == LATENCY_MODE_FULL) {
            double pct = 0.0;
            if (pState->result.nTotalCapacityBytes > 0) {
                pct = (double)pState->result.nBytesTested * 100.0 / (double)pState->result.nTotalCapacityBytes;
            }
            if (pct > 100.0) pct = 100.0;
            if (hProgress) SendMessage(hProgress, PBM_SETPOS, (WPARAM)(int)(pct * 10.0), 0);

            double curGb = (double)pState->result.nBytesTested / (1024.0 * 1024.0 * 1024.0);
            double totGb = (double)pState->result.nTotalCapacityBytes / (1024.0 * 1024.0 * 1024.0);
            int etaSec = (int)pState->result.fEtaSeconds;
            if (etaSec < 0) etaSec = 0;
            int etaMin = etaSec / 60;
            int etaS   = etaSec % 60;

            char szStatus[512];
            safe_snprintf(szStatus,
                "%s: %.1f %s (Avg: %.1f | Min: %.1f | Max: %.1f)  ·  %s: %.1f / %.1f GB (%.1f%%)  ·  ETA: %02d:%02d",
                TN("Скорость", "Speed"), pState->result.fCurrentSpeedMBs, TN("МБ/с", "MB/s"),
                pState->result.fAvgSpeedMBs, pState->result.fMinSpeedMBs, pState->result.fMaxSpeedMBs,
                TN("Прогресс", "Progress"), curGb, totGb, pct,
                etaMin, etaS);
            SetDlgItemTextU8(hDlg, IDC_LAT_STATUS_STATIC, szStatus);
        } else {
            int total = pState->result.nTotalSamples > 0 ? pState->result.nTotalSamples : 128;
            double pct = (double)pState->result.nSamplesTested * 100.0 / (double)total;
            if (pct > 100.0) pct = 100.0;
            if (hProgress) SendMessage(hProgress, PBM_SETPOS, (WPARAM)(int)(pct * 10.0), 0);

            char szStatus[512];
            safe_snprintf(szStatus,
                "%s: %d / %d (%.0f%%)  ·  %s: Min: %.2f ms | Avg: %.2f ms | Max: %.2f ms  ·  %s: %.2f s",
                TN("Проверено", "Tested"), pState->result.nSamplesTested, total, pct,
                TN("Отклик", "Latency"), pState->result.fMinMs, pState->result.fAvgMs, pState->result.fMaxMs,
                TN("Время", "Time"), pState->result.fTotalTimeSec);
            SetDlgItemTextU8(hDlg, IDC_LAT_STATUS_STATIC, szStatus);
        }

        if (hGrid) InvalidateRect(hGrid, NULL, FALSE);
        if (hLegend) InvalidateRect(hLegend, NULL, FALSE);
        return 0;
    }

    case WM_LAT_FINISHED:
    {
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hDlg, "DM_LAT_STATE");
        if (!pState) return 0;
        pState->bRunning = false;
        if (pState->hThread) {
            CloseHandle(pState->hThread);
            pState->hThread = NULL;
        }

        EnableWindow(GetDlgItem(hDlg, IDC_LAT_START_BTN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_LAT_STOP_BTN), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_LAT_COPY_BTN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_EXPRESS), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_FULL), TRUE);

        SetDriveLatencyResult(pState->nDriveIndex, &pState->result);

        HWND hProgress = GetDlgItem(hDlg, IDC_LAT_PROGRESS_BAR);
        if (hProgress) SendMessage(hProgress, PBM_SETPOS, 1000, 0);

        char szStatus[512];
        if (pState->result.bAborted) {
            safe_snprintf(szStatus, "%s (%s: %.2f s)",
                TN("Тестирование остановлено пользователем", "Test stopped by user"),
                TN("Время", "Duration"), pState->result.fTotalTimeSec);
        } else if (pState->result.eMode == LATENCY_MODE_FULL) {
            double totGb = (double)pState->result.nTotalCapacityBytes / (1024.0 * 1024.0 * 1024.0);
            safe_snprintf(szStatus,
                "%s (%.1f GB)  ·  %s: %.1f %s (Min: %.1f | Max: %.1f)  ·  %s: %.1f s",
                TN("Полный тест завершен", "Full test complete"), totGb,
                TN("Ср. скорость", "Avg speed"), pState->result.fAvgSpeedMBs, TN("МБ/с", "MB/s"),
                pState->result.fMinSpeedMBs, pState->result.fMaxSpeedMBs,
                TN("Время", "Duration"), pState->result.fTotalTimeSec);
        } else {
            safe_snprintf(szStatus,
                "%s (%d %s)  ·  %s: Min: %.2f ms | Avg: %.2f ms | Max: %.2f ms  ·  %s: %.2f s",
                TN("Экспресс-тест завершен", "Express test complete"), pState->result.nSamplesTested, TN("точек", "points"),
                TN("Отклик", "Latency"), pState->result.fMinMs, pState->result.fAvgMs, pState->result.fMaxMs,
                TN("Время", "Duration"), pState->result.fTotalTimeSec);
        }
        SetDlgItemTextU8(hDlg, IDC_LAT_STATUS_STATIC, szStatus);

        HWND hGrid    = GetDlgItem(hDlg, IDC_LAT_GRID);
        HWND hLegend  = GetDlgItem(hDlg, IDC_LAT_LEGEND);
        HWND hVerdict = GetDlgItem(hDlg, IDC_LAT_VERDICT);
        if (hGrid) InvalidateRect(hGrid, NULL, FALSE);
        if (hLegend) InvalidateRect(hLegend, NULL, FALSE);
        if (hVerdict) InvalidateRect(hVerdict, NULL, FALSE);
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hDlg, "DM_LAT_STATE");
        if (!pState) return 0;

        if (id == IDC_LAT_MODE_EXPRESS) {
            pState->eMode = LATENCY_MODE_EXPRESS;
        }
        else if (id == IDC_LAT_MODE_FULL) {
            pState->eMode = LATENCY_MODE_FULL;
        }
        else if (id == IDC_LAT_START_BTN) {
            if (pState->bRunning) return 0;

            pState->eMode = IsDlgButtonChecked(hDlg, IDC_LAT_MODE_FULL) ? LATENCY_MODE_FULL : LATENCY_MODE_EXPRESS;

            EnableWindow(GetDlgItem(hDlg, IDC_LAT_START_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_LAT_STOP_BTN), TRUE);
            EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_EXPRESS), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_FULL), FALSE);

            ZeroMemory(&pState->result, sizeof(pState->result));
            pState->result.eMode = pState->eMode;
            pState->result.nTotalSamples = 128;
            pState->nCurrentActiveCell = -1;
            pState->bRunning = true;
            pState->bCancel = false;
            pState->dwLastProgressTick = 0;

            HWND hProgress = GetDlgItem(hDlg, IDC_LAT_PROGRESS_BAR);
            if (hProgress) SendMessage(hProgress, PBM_SETPOS, 0, 0);

            SetDlgItemTextU8(hDlg, IDC_LAT_STATUS_STATIC,
                pState->eMode == LATENCY_MODE_FULL
                    ? TN("Запуск последовательного тестирования скорости по всей поверхности...",
                         "Starting full sequential surface read benchmark...")
                    : TN("Запуск экспресс-проверки времени отклика ячеек...",
                         "Starting express cell latency probe..."));

            InvalidateRect(GetDlgItem(hDlg, IDC_LAT_GRID), NULL, FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_LAT_LEGEND), NULL, FALSE);
            InvalidateRect(GetDlgItem(hDlg, IDC_LAT_VERDICT), NULL, FALSE);

            DWORD dwThreadId = 0;
            pState->hThread = CreateThread(NULL, 0, LatencyProbeThreadProc, pState, 0, &dwThreadId);
            if (!pState->hThread) {
                pState->bRunning = false;
                EnableWindow(GetDlgItem(hDlg, IDC_LAT_START_BTN), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_LAT_STOP_BTN), FALSE);
                EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_EXPRESS), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_LAT_MODE_FULL), TRUE);
                MessageBoxU8(hDlg, TN("Не удалось создать рабочий поток теста.", "Failed to create benchmark thread."),
                             "DriveMonitor", MB_OK | MB_ICONERROR);
            }
        }
        else if (id == IDC_LAT_STOP_BTN) {
            pState->bCancel = true;
            EnableWindow(GetDlgItem(hDlg, IDC_LAT_STOP_BTN), FALSE);
            SetDlgItemTextU8(hDlg, IDC_LAT_STATUS_STATIC, TN("Остановка теста...", "Stopping benchmark..."));
        }
        else if (id == IDC_LAT_COPY_BTN) {
            char szSummary[8192];
            FormatLatencySummary(&pState->result, pState->szModel, pState->szSerial,
                                 pState->nCapacityBytes, pState->bIsSsd,
                                 szSummary, sizeof(szSummary));
            CopyTextToClipboard(hDlg, szSummary);
            MessageBoxU8(hDlg,
                TN("Отчёт о тестировании успешно скопирован в буфер обмена.",
                   "Benchmark report successfully copied to clipboard."),
                "DriveMonitor", MB_OK | MB_ICONINFORMATION);
        }
        else if (id == IDC_LAT_CLOSE_BTN || id == IDCANCEL) {
            SendMessage(hDlg, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
    {
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hDlg, "DM_LAT_STATE");
        if (pState && pState->bRunning) {
            pState->bCancel = true;
            if (pState->hThread) {
                WaitForSingleObject(pState->hThread, 500);
            }
        }
        DestroyWindow(hDlg);
        return 0;
    }

    case WM_DESTROY:
    {
        LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)GetPropA(hDlg, "DM_LAT_STATE");
        if (pState) {
            if (pState->hThread) {
                CloseHandle(pState->hThread);
                pState->hThread = NULL;
            }
            RemovePropA(hDlg, "DM_LAT_STATE");
            free(pState);
        }
        return 0;
    }
    }

    return DefWindowProcW(hDlg, uMsg, wParam, lParam);
}

BOOL DrivePanelOnMouseWheel(const MSG* msg)
{
    (void)msg;
    return FALSE;
}

void ShowLatencyProbeDialogEx(HWND hParent, int nDriveIndex, int initialMode)
{
    static BOOL bClassesRegistered = FALSE;

    if (g_nDriveCount <= 0 || nDriveIndex < 0 || nDriveIndex >= g_nDriveCount) {
        MessageBoxU8(hParent, Tr(STR_NO_DRIVE), "DriveMonitor", MB_OK | MB_ICONWARNING);
        return;
    }

    const DRIVE_INFO* pInfo = &g_Drives[nDriveIndex];

    if (!bClassesRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

        wc.lpfnWndProc   = LatencyGridWndProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DriveMonitorLatencyGrid";
        RegisterClassW(&wc);

        wc.lpfnWndProc   = LatencyLegendWndProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DriveMonitorLatencyLegend";
        RegisterClassW(&wc);

        wc.lpfnWndProc   = LatencyVerdictWndProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DriveMonitorLatencyVerdict";
        RegisterClassW(&wc);

        wc.lpfnWndProc   = LatencyDlgProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"DriveMonitorLatencyDlg";
        RegisterClassW(&wc);

        bClassesRegistered = TRUE;
    }

    HWND hExist = FindWindowW(L"DriveMonitorLatencyDlg", NULL);
    if (hExist) {
        SetForegroundWindow(hExist);
        return;
    }

    LATENCY_DLG_STATE* pState = (LATENCY_DLG_STATE*)calloc(1, sizeof(LATENCY_DLG_STATE));
    if (!pState) return;

    pState->nDriveIndex = nDriveIndex;
    pState->nCapacityBytes = (uint64_t)pInfo->dwCapacityMB * 1024ULL * 1024ULL;
    pState->bIsSsd = (pInfo->bIsNVMe ||
                      pInfo->eType == DRIVE_TYPE_SSD_SATA ||
                      pInfo->eType == DRIVE_TYPE_M2_SATA ||
                      pInfo->eType == DRIVE_TYPE_NVME ||
                      pInfo->eType == DRIVE_TYPE_EMMC);
    lstrcpynA(pState->szModel, pInfo->szModel[0] ? pInfo->szModel : "Drive", (int)sizeof(pState->szModel));
    lstrcpynA(pState->szSerial, pInfo->szSerial, (int)sizeof(pState->szSerial));
    pState->eMode = (initialMode == LATENCY_MODE_FULL) ? LATENCY_MODE_FULL : LATENCY_MODE_EXPRESS;

    const LATENCY_PROBE_RESULT* pCached = GetDriveLatencyResult(nDriveIndex);
    if (pCached && pCached->nSamplesTested > 0) {
        pState->result = *pCached;
    } else {
        pState->result.eMode = pState->eMode;
        pState->result.nTotalSamples = 128;
    }

    int dlgW = UiScale(720);
    int dlgH = UiScale(620);
    int nX, nY;

    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        nX = rcP.left + (rcP.right - rcP.left - dlgW) / 2;
        nY = rcP.top + (rcP.bottom - rcP.top - dlgH) / 2;
    } else {
        nX = (GetSystemMetrics(SM_CXSCREEN) - dlgW) / 2;
        nY = (GetSystemMetrics(SM_CYSCREEN) - dlgH) / 2;
    }

    char szTitle[256];
    safe_snprintf(szTitle, "%s — %s (%s)",
        TN("Тестирование чтения и отклика ячеек", "Read Speed & Latency Benchmark"),
        pState->szModel,
        pState->bIsSsd ? "SSD" : "HDD");

    WCHAR wzTitle[256];
    U8ToW(szTitle, wzTitle, 256);

    RECT rcAdj;
    rcAdj.left = 0; rcAdj.top = 0; rcAdj.right = dlgW; rcAdj.bottom = dlgH;
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    HWND hDlg = CreateWindowExW(
        0,
        L"DriveMonitorLatencyDlg",
        wzTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        nX, nY,
        rcAdj.right - rcAdj.left,
        rcAdj.bottom - rcAdj.top,
        hParent,
        NULL,
        g_hInst,
        (LPVOID)pState
    );

    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        UpdateWindow(hDlg);
    }
}

void ShowLatencyProbeDialog(HWND hParent, int nDriveIndex)
{
    ShowLatencyProbeDialogEx(hParent, nDriveIndex, LATENCY_MODE_EXPRESS);
}



/* ============================================================================
 * DRAM Buffer Integrity & Silent Data Corruption (Bit Flip) Dialog
 * ============================================================================ */

#define IDC_DRAM_BANNER         6001
#define IDC_DRAM_VOL_LBL        6002
#define IDC_DRAM_VOL_COMBO      6003
#define IDC_DRAM_MODE_GRP       6004
#define IDC_DRAM_MODE_EXP       6005
#define IDC_DRAM_MODE_STD       6006
#define IDC_DRAM_MODE_STR       6007
#define IDC_DRAM_PROGRESS       6008
#define IDC_DRAM_STATUS         6009
#define IDC_DRAM_VERDICT        6010
#define IDC_DRAM_LOG            6011
#define IDC_DRAM_START_BTN      6012
#define IDC_DRAM_STOP_BTN       6013
#define IDC_DRAM_COPY_BTN       6014
#define IDC_DRAM_CLOSE_BTN      6015

#define WM_DRAM_PROGRESS        (WM_APP + 31)
#define WM_DRAM_DONE            (WM_APP + 32)

typedef struct _DRAM_DLG_STATE {
    HWND                hDlg;
    int                 nDriveIndex;
    char                szModel[64];
    char                szSerial[64];
    bool                bIsSsd;
    char                szVolume[32];
    DRAM_VERIFY_MODE    eMode;
    DRAM_VERIFY_RESULT  result;
    HANDLE              hThread;
    volatile bool       bRunning;
    volatile bool       bCancel;
    DWORD               dwLastProgressTick;
} DRAM_DLG_STATE;

static LRESULT CALLBACK DramVerdictWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);
        RECT rc;
        GetClientRect(hWnd, &rc);
        int w = rc.right - rc.left;
        int h = rc.bottom - rc.top;

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP memBM = CreateCompatibleBitmap(hdc, w, h);
        HGDIOBJ oldBM = SelectObject(memDC, memBM);

        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hWnd, "DM_DRAM_STATE");
        const DRAM_VERIFY_RESULT* pRes = pState ? &pState->result : NULL;

        COLORREF bgClr = RGB(255, 255, 255);
        COLORREF borderClr = RGB(218, 222, 228);
        COLORREF badgeBg = RGB(240, 243, 246);
        COLORREF badgeTextClr = RGB(60, 64, 67);

        if (pRes && pRes->szVerdictBadge[0] != '\0') {
            if (pRes->bPassed) {
                bgClr = RGB(250, 255, 250);
                borderClr = RGB(163, 230, 184);
                badgeBg = RGB(230, 248, 235);
                badgeTextClr = RGB(20, 108, 48);
            } else if (pRes->bAborted) {
                bgClr = RGB(255, 254, 248);
                borderClr = RGB(249, 226, 150);
                badgeBg = RGB(254, 249, 231);
                badgeTextClr = RGB(183, 121, 31);
            } else {
                bgClr = RGB(255, 248, 248);
                borderClr = RGB(252, 180, 180);
                badgeBg = RGB(254, 232, 232);
                badgeTextClr = RGB(190, 30, 30);
            }
        }

        HBRUSH hBgBrush = CreateSolidBrush(bgClr);
        FillRect(memDC, &rc, hBgBrush);
        DeleteObject(hBgBrush);

        HPEN hPen = CreatePen(PS_SOLID, 1, borderClr);
        HGDIOBJ oldPen = SelectObject(memDC, hPen);
        HGDIOBJ oldBrush = SelectObject(memDC, GetStockObject(NULL_BRUSH));
        Rectangle(memDC, 0, 0, w, h);
        SelectObject(memDC, oldBrush);
        SelectObject(memDC, oldPen);
        DeleteObject(hPen);

        SetBkMode(memDC, TRANSPARENT);

        if (pRes && pRes->szVerdictBadge[0] != '\0') {
            RECT badgeRc = { UiScale(14), UiScale(10), UiScale(240), UiScale(36) };
            HBRUSH hBadgeBrush = CreateSolidBrush(badgeBg);
            HPEN hBadgePen = CreatePen(PS_SOLID, 1, borderClr);
            HGDIOBJ p1 = SelectObject(memDC, hBadgeBrush);
            HGDIOBJ p2 = SelectObject(memDC, hBadgePen);
            RoundRect(memDC, badgeRc.left, badgeRc.top, badgeRc.right, badgeRc.bottom, UiScale(6), UiScale(6));
            SelectObject(memDC, p2);
            SelectObject(memDC, p1);
            DeleteObject(hBadgeBrush);
            DeleteObject(hBadgePen);

            SetTextColor(memDC, badgeTextClr);
            SelectObject(memDC, g_hFontNormal);
            WCHAR wzBadge[64];
            U8ToW(pRes->szVerdictBadge, wzBadge, 64);
            DrawTextW(memDC, wzBadge, -1, &badgeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            RECT textRc = { UiScale(14), UiScale(44), w - UiScale(14), h - UiScale(6) };
            SetTextColor(memDC, RGB(40, 44, 48));
            SelectObject(memDC, g_hFontNormal);
            WCHAR wzText[1024];
            U8ToW(pRes->szVerdictText, wzText, 1024);
            DrawTextW(memDC, wzText, -1, &textRc, DT_LEFT | DT_WORDBREAK);
        } else {
            RECT textRc = { UiScale(14), UiScale(12), w - UiScale(14), h - UiScale(8) };
            SetTextColor(memDC, RGB(110, 115, 122));
            SelectObject(memDC, g_hFontNormal);
            const char* szPrompt = TN(
                "Диагностический вердикт буфера DRAM появится здесь после запуска проверки.\r\n\r\n"
                "• Тест выполняет прямую запись стресс-паттернов в обход кэша Windows (Write-Through / No-Buffering).\r\n"
                "• Затем данные вычитываются и побитово сверяются с эталоном для выявления скрытых битовых сбоев (Bit Flip).",
                "Diagnostic DRAM buffer verdict will appear here after the test starts.\r\n\r\n"
                "• Direct unbuffered write-through patterns bypass Windows host RAM cache.\r\n"
                "• Read-After-Write loopback verification detects silent data corruption and bit-flips."
            );
            WCHAR wzPrompt[1024];
            U8ToW(szPrompt, wzPrompt, 1024);
            DrawTextW(memDC, wzPrompt, -1, &textRc, DT_LEFT | DT_WORDBREAK);
        }

        BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBM);
        DeleteObject(memBM);
        DeleteDC(memDC);
        EndPaint(hWnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(hWnd, uMsg, wParam, lParam);
}

static bool DramVerifyDlgCallback(const DRAM_VERIFY_RESULT* pResult, void* pUserData)
{
    DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)pUserData;
    if (!pState || pState->bCancel) return false;

    DWORD now = GetTickCount();
    if ((now - pState->dwLastProgressTick >= 80) ||
        pResult->eStage == DRAM_STAGE_COMPLETED ||
        pResult->eStage == DRAM_STAGE_FAILED) {
        pState->dwLastProgressTick = now;
        pState->result = *pResult;
        PostMessage(pState->hDlg, WM_DRAM_PROGRESS, 0, 0);
    }
    return !pState->bCancel;
}

static DWORD WINAPI DramVerifyWorkerThread(LPVOID lpParam)
{
    DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)lpParam;
    if (!pState) return 0;

    pState->bRunning = true;
    pState->bCancel = false;

    RunDramVerification(pState->szVolume,
                        pState->eMode,
                        &pState->result,
                        DramVerifyDlgCallback,
                        pState,
                        &pState->bCancel);

    SetDriveDramVerifyResult(pState->nDriveIndex, &pState->result);

    if (pState->nDriveIndex >= 0 && pState->nDriveIndex < g_nDriveCount) {
        DRIVE_INFO* pD = &g_Drives[pState->nDriveIndex];
        pD->bDramTested = TRUE;
        pD->bDramCorruption = (pState->result.nCorruptedBytes > 0);
        pD->nDramCorruptedBytes = pState->result.nCorruptedBytes;
        pD->nDramBitFlips = pState->result.nTotalBitFlips;
        AssessDriveHealth(pD);
    }

    pState->bRunning = false;
    PostMessage(pState->hDlg, WM_DRAM_DONE, 0, 0);
    return 0;
}

static LRESULT CALLBACK DramDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)cs->lpCreateParams;
        SetPropA(hDlg, "DM_DRAM_STATE", (HANDLE)pState);
        pState->hDlg = hDlg;

        RECT rcC;
        GetClientRect(hDlg, &rcC);
        int w = rcC.right - rcC.left;

        HWND hBanner = CreateWindowExU8(0, "STATIC",
            TN("Аппаратная проверка буфера памяти DRAM (Read-After-Write)",
               "DRAM Buffer Integrity & Silent Bit Flip Verification"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(14), w - UiScale(40), UiScale(24),
            hDlg, (HMENU)IDC_DRAM_BANNER, g_hInst, NULL);
        SendMessage(hBanner, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

        /* Volume selection */
        HWND hVolLbl = CreateWindowExU8(0, "STATIC",
            TN("Целевой том:", "Target volume:"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(46), UiScale(110), UiScale(22),
            hDlg, (HMENU)IDC_DRAM_VOL_LBL, g_hInst, NULL);
        SendMessage(hVolLbl, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hCombo = CreateWindowExU8(0, "COMBOBOX", "",
            WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
            UiScale(130), UiScale(42), UiScale(280), UiScale(160),
            hDlg, (HMENU)IDC_DRAM_VOL_COMBO, g_hInst, NULL);
        SendMessage(hCombo, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        /* Populate volume combo */
        const DRIVE_INFO* pInfo = (pState->nDriveIndex >= 0 && pState->nDriveIndex < g_nDriveCount)
                                  ? &g_Drives[pState->nDriveIndex] : NULL;
        int nAdded = 0;
        if (pInfo && pInfo->trimInfo.bQueried && pInfo->trimInfo.nVolumeCount > 0) {
            for (int vi = 0; vi < pInfo->trimInfo.nVolumeCount; vi++) {
                const VOLUME_INFO_ENTRY* v = &pInfo->trimInfo.volumes[vi];
                char szItem[128];
                double flFree = (double)v->qwFreeBytes / (1024.0 * 1024.0 * 1024.0);
                safe_snprintf(szItem, "%s [%s] (%.1f GB %s)",
                    v->szLetter, v->szFileSystem[0] ? v->szFileSystem : "FS",
                    flFree, TN("свободно", "free"));
                WCHAR wzItem[128];
                U8ToW(szItem, wzItem, 128);
                SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)wzItem);
                nAdded++;
            }
        }
        if (nAdded == 0) {
            SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)L"C:\\ (Системный том)");
        }
        SendMessageW(hCombo, CB_SETCURSEL, 0, 0);

        /* Mode radio buttons */
        HWND hGrp = CreateWindowExU8(0, "BUTTON",
            TN("Режим тестирования", "Verification mode"),
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            UiScale(20), UiScale(74), w - UiScale(40), UiScale(56),
            hDlg, (HMENU)IDC_DRAM_MODE_GRP, g_hInst, NULL);
        SendMessage(hGrp, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        HWND hRad1 = CreateWindowExU8(0, "BUTTON",
            TN("Экспресс (32 МБ)", "Express (32 MB)"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            UiScale(32), UiScale(94), UiScale(190), UiScale(24),
            hDlg, (HMENU)IDC_DRAM_MODE_EXP, g_hInst, NULL);
        SendMessage(hRad1, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hRad2 = CreateWindowExU8(0, "BUTTON",
            TN("Стандарт (128 МБ)", "Standard (128 MB)"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            UiScale(230), UiScale(94), UiScale(190), UiScale(24),
            hDlg, (HMENU)IDC_DRAM_MODE_STD, g_hInst, NULL);
        SendMessage(hRad2, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hRad3 = CreateWindowExU8(0, "BUTTON",
            TN("Полный стресс (512 МБ)", "Full stress (512 MB)"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            UiScale(430), UiScale(94), UiScale(220), UiScale(24),
            hDlg, (HMENU)IDC_DRAM_MODE_STR, g_hInst, NULL);
        SendMessage(hRad3, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        CheckRadioButton(hDlg, IDC_DRAM_MODE_EXP, IDC_DRAM_MODE_STR, IDC_DRAM_MODE_EXP);

        /* Progress Bar */
        HWND hProg = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            UiScale(20), UiScale(140), w - UiScale(40), UiScale(20),
            hDlg, (HMENU)IDC_DRAM_PROGRESS, g_hInst, NULL);
        SendMessage(hProg, PBM_SETRANGE32, 0, 1000);

        /* Status static */
        HWND hStat = CreateWindowExU8(0, "STATIC",
            TN("Готов к проверке буфера DRAM", "Ready for DRAM buffer verification"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(166), w - UiScale(40), UiScale(18),
            hDlg, (HMENU)IDC_DRAM_STATUS, g_hInst, NULL);
        SendMessage(hStat, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        /* Verdict custom window */
        HWND hVerdict = CreateWindowExW(0, L"DriveMonitorDramVerdict", L"",
            WS_CHILD | WS_VISIBLE,
            UiScale(20), UiScale(190), w - UiScale(40), UiScale(100),
            hDlg, (HMENU)IDC_DRAM_VERDICT, g_hInst, NULL);
        SetPropA(hVerdict, "DM_DRAM_STATE", (HANDLE)pState);

        /* Detail / log window */
        HWND hLog = CreateWindowExU8(WS_EX_CLIENTEDGE, "EDIT", "",
            WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL,
            UiScale(20), UiScale(300), w - UiScale(40), UiScale(210),
            hDlg, (HMENU)IDC_DRAM_LOG, g_hInst, NULL);
        SendMessage(hLog, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        /* Buttons */
        HWND hStart = CreateWindowExU8(0, "BUTTON",
            TN("Начать тест", "Start Test"),
            WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            UiScale(20), UiScale(524), UiScale(140), UiScale(30),
            hDlg, (HMENU)IDC_DRAM_START_BTN, g_hInst, NULL);
        SendMessage(hStart, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hStop = CreateWindowExU8(0, "BUTTON",
            TN("Остановить", "Stop"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(170), UiScale(524), UiScale(110), UiScale(30),
            hDlg, (HMENU)IDC_DRAM_STOP_BTN, g_hInst, NULL);
        SendMessage(hStop, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        EnableWindow(hStop, FALSE);

        HWND hCopy = CreateWindowExU8(0, "BUTTON",
            TN("Копировать отчёт", "Copy Report"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(290), UiScale(524), UiScale(150), UiScale(30),
            hDlg, (HMENU)IDC_DRAM_COPY_BTN, g_hInst, NULL);
        SendMessage(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        EnableWindow(hCopy, FALSE);

        HWND hClose = CreateWindowExU8(0, "BUTTON",
            TN("Закрыть", "Close"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            w - UiScale(130), UiScale(524), UiScale(110), UiScale(30),
            hDlg, (HMENU)IDC_DRAM_CLOSE_BTN, g_hInst, NULL);
        SendMessage(hClose, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        return 0;
    }

    case WM_DRAM_PROGRESS:
    {
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hDlg, "DM_DRAM_STATE");
        if (!pState) return 0;

        uint64_t tested = pState->result.nBytesTested;
        uint64_t total  = pState->result.nTotalBytesToTest;
        if (total == 0) total = 1;

        int nPos = (int)((tested * 1000ULL) / total);
        if (nPos > 1000) nPos = 1000;
        SendMessage(GetDlgItem(hDlg, IDC_DRAM_PROGRESS), PBM_SETPOS, (WPARAM)nPos, 0);

        char szStatus[256];
        double mbDone = (double)tested / (1024.0 * 1024.0);
        double mbTot  = (double)total  / (1024.0 * 1024.0);

        if (pState->result.eStage == DRAM_STAGE_WRITING) {
            safe_snprintf(szStatus, "%s... %.1f / %.1f MB (%.1f %s)",
                TN("Прямая запись проверочных стресс-паттернов (Write-Through)",
                   "Direct stress pattern writing (Write-Through)"),
                mbDone, mbTot, pState->result.fWriteSpeedMBs, TN("МБ/с", "MB/s"));
        } else if (pState->result.eStage == DRAM_STAGE_FLUSHING) {
            safe_snprintf(szStatus, "%s",
                TN("Аппаратный сброс буферов контроллера (FlushFileBuffers)...",
                   "Flushing controller hardware buffers (FlushFileBuffers)..."));
        } else if (pState->result.eStage == DRAM_STAGE_READING_VERIFYING) {
            safe_snprintf(szStatus, "%s... %.1f / %.1f MB (%.1f %s)  [Ошибок: %llu]",
                TN("Побитовая верификация (Read-After-Write)",
                   "Bitwise verification (Read-After-Write)"),
                mbDone, mbTot, pState->result.fReadSpeedMBs, TN("МБ/с", "MB/s"),
                (unsigned long long)pState->result.nCorruptedBytes);
        } else {
            safe_snprintf(szStatus, "%s", TN("Тестирование завершено", "Test completed"));
        }
        SetDlgItemTextU8(hDlg, IDC_DRAM_STATUS, szStatus);
        return 0;
    }

    case WM_DRAM_DONE:
    {
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hDlg, "DM_DRAM_STATE");
        if (!pState) return 0;

        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_START_BTN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_STOP_BTN), FALSE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_COPY_BTN), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_VOL_COMBO), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_EXP), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_STD), TRUE);
        EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_STR), TRUE);

        SendMessage(GetDlgItem(hDlg, IDC_DRAM_PROGRESS), PBM_SETPOS, 1000, 0);

        char szStatus[256];
        if (pState->result.bPassed) {
            safe_snprintf(szStatus, "%s: %.1f MB (W: %.1f MB/s, R: %.1f MB/s) — 0 %s",
                TN("Проверка успешно пройдена", "Verification passed"),
                (double)pState->result.nBytesTested / (1024.0 * 1024.0),
                pState->result.fWriteSpeedMBs, pState->result.fReadSpeedMBs,
                TN("ошибок", "errors"));
        } else if (pState->result.bAborted) {
            safe_snprintf(szStatus, "%s", TN("Тест остановлен пользователем", "Test aborted by user"));
        } else {
            safe_snprintf(szStatus, "%s: %llu %s (%llu %s)!",
                TN("ОБНАРУЖЕНЫ СБОИ", "FAILURES DETECTED"),
                (unsigned long long)pState->result.nCorruptedBytes,
                TN("искажённых байт", "corrupted bytes"),
                (unsigned long long)pState->result.nTotalBitFlips,
                TN("битовых инверсий", "bit flips"));
        }
        SetDlgItemTextU8(hDlg, IDC_DRAM_STATUS, szStatus);

        InvalidateRect(GetDlgItem(hDlg, IDC_DRAM_VERDICT), NULL, FALSE);

        char szSummary[8192];
        FormatDramVerifySummary(&pState->result, pState->szModel, pState->szSerial,
                                szSummary, sizeof(szSummary));
        SetDlgItemTextU8(hDlg, IDC_DRAM_LOG, szSummary);

        if (g_hMainWnd) {
            InvalidateRect(g_hMainWnd, NULL, TRUE);
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hDlg, "DM_DRAM_STATE");
        if (!pState) return 0;

        if (id == IDC_DRAM_START_BTN) {
            if (pState->bRunning) return 0;

            if (IsDlgButtonChecked(hDlg, IDC_DRAM_MODE_STR)) {
                pState->eMode = DRAM_VERIFY_MODE_STRESS;
            } else if (IsDlgButtonChecked(hDlg, IDC_DRAM_MODE_STD)) {
                pState->eMode = DRAM_VERIFY_MODE_STANDARD;
            } else {
                pState->eMode = DRAM_VERIFY_MODE_EXPRESS;
            }

            WCHAR wzVol[128] = L"";
            GetDlgItemTextW(hDlg, IDC_DRAM_VOL_COMBO, wzVol, 128);
            if (wzVol[0]) {
                char szTmp[128];
                WToU8(wzVol, szTmp, 128);
                char* sp = strchr(szTmp, ' ');
                if (sp) *sp = '\0';
                safe_snprintf(pState->szVolume, "%s", szTmp);
            } else {
                safe_snprintf(pState->szVolume, "C:\\");
            }

            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_START_BTN), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_STOP_BTN), TRUE);
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_VOL_COMBO), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_EXP), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_STD), FALSE);
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_MODE_STR), FALSE);

            SendMessage(GetDlgItem(hDlg, IDC_DRAM_PROGRESS), PBM_SETPOS, 0, 0);
            SetDlgItemTextU8(hDlg, IDC_DRAM_STATUS,
                TN("Подготовка проверочного файла прямого доступа...",
                   "Preparing direct unbuffered test file..."));
            SetDlgItemTextU8(hDlg, IDC_DRAM_LOG, "");
            InvalidateRect(GetDlgItem(hDlg, IDC_DRAM_VERDICT), NULL, FALSE);

            DWORD dwThreadId = 0;
            pState->hThread = CreateThread(NULL, 0, DramVerifyWorkerThread, pState, 0, &dwThreadId);
            if (!pState->hThread) {
                EnableWindow(GetDlgItem(hDlg, IDC_DRAM_START_BTN), TRUE);
                EnableWindow(GetDlgItem(hDlg, IDC_DRAM_STOP_BTN), FALSE);
                MessageBoxU8(hDlg, TN("Не удалось создать поток проверки.", "Failed to create verification thread."),
                             "DriveMonitor", MB_OK | MB_ICONERROR);
            }
        }
        else if (id == IDC_DRAM_STOP_BTN) {
            pState->bCancel = true;
            EnableWindow(GetDlgItem(hDlg, IDC_DRAM_STOP_BTN), FALSE);
            SetDlgItemTextU8(hDlg, IDC_DRAM_STATUS, TN("Остановка теста...", "Stopping test..."));
        }
        else if (id == IDC_DRAM_COPY_BTN) {
            char szSummary[8192];
            FormatDramVerifySummary(&pState->result, pState->szModel, pState->szSerial,
                                    szSummary, sizeof(szSummary));
            CopyTextToClipboard(hDlg, szSummary);
            MessageBoxU8(hDlg,
                TN("Отчёт о тестировании буфера DRAM скопирован в буфер обмена.",
                   "DRAM buffer test report copied to clipboard."),
                "DriveMonitor", MB_OK | MB_ICONINFORMATION);
        }
        else if (id == IDC_DRAM_CLOSE_BTN || id == IDCANCEL) {
            SendMessage(hDlg, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
    {
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hDlg, "DM_DRAM_STATE");
        if (pState && pState->bRunning) {
            pState->bCancel = true;
            if (pState->hThread) {
                WaitForSingleObject(pState->hThread, 500);
            }
        }
        DestroyWindow(hDlg);
        return 0;
    }

    case WM_DESTROY:
    {
        DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)GetPropA(hDlg, "DM_DRAM_STATE");
        if (pState) {
            if (pState->hThread) {
                CloseHandle(pState->hThread);
                pState->hThread = NULL;
            }
            RemovePropA(hDlg, "DM_DRAM_STATE");
            free(pState);
        }
        return 0;
    }
    }

    return DefWindowProcW(hDlg, uMsg, wParam, lParam);
}

void ShowDramVerifyDialog(HWND hParent, int nDriveIndex)
{
    static BOOL bRegistered = FALSE;

    if (g_nDriveCount <= 0 || nDriveIndex < 0 || nDriveIndex >= g_nDriveCount) {
        MessageBoxU8(hParent, Tr(STR_NO_DRIVE), "DriveMonitor", MB_OK | MB_ICONWARNING);
        return;
    }

    const DRIVE_INFO* pInfo = &g_Drives[nDriveIndex];

    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

        wc.lpfnWndProc   = DramVerdictWndProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"DriveMonitorDramVerdict";
        RegisterClassW(&wc);

        wc.lpfnWndProc   = DramDlgProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"DriveMonitorDramVerifyDlg";
        RegisterClassW(&wc);

        bRegistered = TRUE;
    }

    HWND hExist = FindWindowW(L"DriveMonitorDramVerifyDlg", NULL);
    if (hExist) {
        SetForegroundWindow(hExist);
        return;
    }

    DRAM_DLG_STATE* pState = (DRAM_DLG_STATE*)calloc(1, sizeof(DRAM_DLG_STATE));
    if (!pState) return;

    pState->nDriveIndex = nDriveIndex;
    pState->bIsSsd = (pInfo->bIsNVMe ||
                      pInfo->eType == DRIVE_TYPE_SSD_SATA ||
                      pInfo->eType == DRIVE_TYPE_M2_SATA ||
                      pInfo->eType == DRIVE_TYPE_NVME ||
                      pInfo->eType == DRIVE_TYPE_EMMC);
    lstrcpynA(pState->szModel, pInfo->szModel[0] ? pInfo->szModel : "Drive", (int)sizeof(pState->szModel));
    lstrcpynA(pState->szSerial, pInfo->szSerial, (int)sizeof(pState->szSerial));
    pState->eMode = DRAM_VERIFY_MODE_EXPRESS;

    const DRAM_VERIFY_RESULT* pCached = GetDriveDramVerifyResult(nDriveIndex);
    if (pCached && pCached->nBytesTested > 0) {
        pState->result = *pCached;
    } else {
        pState->result.eMode = pState->eMode;
    }

    int dlgW = UiScale(720);
    int dlgH = UiScale(600);
    int nX, nY;

    if (hParent) {
        RECT rcP;
        GetWindowRect(hParent, &rcP);
        nX = rcP.left + (rcP.right - rcP.left - dlgW) / 2;
        nY = rcP.top + (rcP.bottom - rcP.top - dlgH) / 2;
    } else {
        nX = (GetSystemMetrics(SM_CXSCREEN) - dlgW) / 2;
        nY = (GetSystemMetrics(SM_CYSCREEN) - dlgH) / 2;
    }

    char szTitle[256];
    safe_snprintf(szTitle, "%s — %s (%s)",
        TN("Тестирование буфера DRAM (Read-After-Write)", "DRAM Buffer Verification (Read-After-Write)"),
        pState->szModel,
        pState->bIsSsd ? "SSD" : "HDD");

    WCHAR wzTitle[256];
    U8ToW(szTitle, wzTitle, 256);

    RECT rcAdj;
    rcAdj.left = 0; rcAdj.top = 0; rcAdj.right = dlgW; rcAdj.bottom = dlgH;
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    HWND hDlg = CreateWindowExW(
        0,
        L"DriveMonitorDramVerifyDlg",
        wzTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        nX, nY,
        rcAdj.right - rcAdj.left,
        rcAdj.bottom - rcAdj.top,
        hParent,
        NULL,
        g_hInst,
        (LPVOID)pState
    );

    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        UpdateWindow(hDlg);
    }
}

/* ============================================================================
 * Hardware Self-Test (DST) and Error Log History Dialog
 * ============================================================================ */

#define IDC_DST_BANNER         7001
#define IDC_DST_CAPS_LBL       7002
#define IDC_DST_PROGRESS       7003
#define IDC_DST_STATUS_TEXT    7004
#define IDC_DST_SHORT_BTN      7005
#define IDC_DST_EXT_BTN        7006
#define IDC_DST_ABORT_BTN      7007
#define IDC_DST_VIEW_DST       7008
#define IDC_DST_VIEW_ERR       7009
#define IDC_DST_LOG_BOX        7010
#define IDC_DST_REFRESH_BTN    7011
#define IDC_DST_COPY_BTN       7012
#define IDC_DST_CLOSE_BTN      7013

#define IDT_DST_POLL_TIMER     8001

typedef struct _DST_DLG_STATE {
    HWND        hDlg;
    int         nDriveIndex;
    char        szModel[64];
    char        szSerial[64];
    bool        bIsNvme;
    bool        bIsSsd;
    bool        bSelfTestSupported;
    int         nShortMinutes;
    int         nExtMinutes;
    bool        bRunning;
    bool        bShowingErrorLog;
    DST_PROGRESS lastProgress;
    HFONT       hFontMono;
} DST_DLG_STATE;

static void UpdateDstLogDisplay(HWND hDlg, DST_DLG_STATE* pState)
{
    if (!pState || pState->nDriveIndex < 0 || pState->nDriveIndex >= g_nDriveCount)
        return;
    const DRIVE_INFO* pInfo = &g_Drives[pState->nDriveIndex];
    char* pBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32768);
    if (!pBuf) return;

    if (pState->bShowingErrorLog) {
        DstFormatErrorLogHistory(pInfo, pBuf, 32768);
    } else {
        DstFormatSelfTestHistory(pInfo, pBuf, 32768);
    }
    SetDlgItemTextU8(hDlg, IDC_DST_LOG_BOX, pBuf);
    HeapFree(GetProcessHeap(), 0, pBuf);
}

static void RefreshDstLogsFromDrive(HWND hDlg, DST_DLG_STATE* pState)
{
    if (!pState || pState->nDriveIndex < 0 || pState->nDriveIndex >= g_nDriveCount)
        return;
    DRIVE_INFO* pInfo = &g_Drives[pState->nDriveIndex];
    HANDLE hDrive = INVALID_HANDLE_VALUE;
    if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
        if (pInfo->bIsNVMe) {
            DstQueryNvmeLogs(hDrive, pInfo);
        } else {
            DstQueryAtaLogs(hDrive, pInfo);
        }
        CloseHandle(hDrive);
    }
    UpdateDstLogDisplay(hDlg, pState);
}

static LRESULT CALLBACK DstDlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
    switch (uMsg) {
    case WM_CREATE:
    {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lParam;
        DST_DLG_STATE* pState = (DST_DLG_STATE*)cs->lpCreateParams;
        if (!pState) return -1;
        pState->hDlg = hDlg;
        SetPropA(hDlg, "DM_DST_STATE", (HANDLE)pState);

        RECT rcClient;
        GetClientRect(hDlg, &rcClient);
        int w = rcClient.right - rcClient.left;

        /* Create monospaced font for logs */
        pState->hFontMono = CreateFontW(-UiScale(12), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Consolas");

        /* Title banner */
        HWND hBanner = CreateWindowExU8(0, "STATIC",
            TN("Аппаратная самодиагностика накопителя (DST)", "Hardware Drive Self-Test (DST)"),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(12), w - UiScale(40), UiScale(24),
            hDlg, (HMENU)IDC_DST_BANNER, g_hInst, NULL);
        SendMessage(hBanner, WM_SETFONT, (WPARAM)g_hFontTitle, TRUE);

        /* Capabilities / drive summary label */
        char szCaps[512];
        const DRIVE_INFO* pInfo = &g_Drives[pState->nDriveIndex];
        if (pState->bIsNvme) {
            safe_snprintf(szCaps, "%s: %s | NVMe | %s: %s",
                TN("Модель", "Model"), pState->szModel,
                TN("DST", "DST"),
                pState->bSelfTestSupported ?
                    TN("Поддерживается контроллером (Короткий ~2 мин, Расширенный ~30+ мин)",
                       "Supported by controller (Short ~2 min, Extended ~30+ min)") :
                    TN("Не заявлена контроллером (OACS bit 4)", "Not reported by controller (OACS bit 4)"));
        } else {
            safe_snprintf(szCaps, "%s: %s | %s | %s: %s",
                TN("Модель", "Model"), pState->szModel,
                pState->bIsSsd ? "SATA SSD" : "SATA HDD",
                TN("DST", "DST"),
                pState->bSelfTestSupported ?
                    TN("Поддерживается (Короткий ~1-2 мин, Расширенный доступен)",
                       "Supported (Short ~1-2 min, Extended available)") :
                    TN("Не заявлена (Identify word 84/87)", "Not reported (Identify word 84/87)"));
        }

        HWND hCaps = CreateWindowExU8(0, "STATIC", szCaps,
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(38), w - UiScale(40), UiScale(20),
            hDlg, (HMENU)IDC_DST_CAPS_LBL, g_hInst, NULL);
        SendMessage(hCaps, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

        /* Test initiation buttons */
        HWND hShortBtn = CreateWindowExU8(0, "BUTTON",
            TN("Короткий тест (~2 мин)", "Short Test (~2 min)"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(20), UiScale(66), UiScale(170), UiScale(28),
            hDlg, (HMENU)IDC_DST_SHORT_BTN, g_hInst, NULL);
        SendMessage(hShortBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hExtBtn = CreateWindowExU8(0, "BUTTON",
            TN("Расширенный тест", "Extended Test"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(200), UiScale(66), UiScale(160), UiScale(28),
            hDlg, (HMENU)IDC_DST_EXT_BTN, g_hInst, NULL);
        SendMessage(hExtBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hAbortBtn = CreateWindowExU8(0, "BUTTON",
            TN("Прервать тест", "Abort Test"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(370), UiScale(66), UiScale(130), UiScale(28),
            hDlg, (HMENU)IDC_DST_ABORT_BTN, g_hInst, NULL);
        SendMessage(hAbortBtn, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        EnableWindow(hAbortBtn, FALSE);

        /* Progress Bar */
        HWND hProg = CreateWindowExW(0, PROGRESS_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | PBS_SMOOTH,
            UiScale(20), UiScale(104), w - UiScale(40), UiScale(20),
            hDlg, (HMENU)IDC_DST_PROGRESS, g_hInst, NULL);
        SendMessage(hProg, PBM_SETRANGE32, 0, 100);

        /* Status Text */
        HWND hStatus = CreateWindowExU8(0, "STATIC",
            TN("Готов к запуску теста.", "Ready to start self-test."),
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            UiScale(20), UiScale(130), w - UiScale(40), UiScale(20),
            hDlg, (HMENU)IDC_DST_STATUS_TEXT, g_hInst, NULL);
        SendMessage(hStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        /* Radio buttons for log views */
        HWND hRadDst = CreateWindowExU8(0, "BUTTON",
            TN("Журнал самотестирования (Self-Test Log)", "Self-Test Log"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            UiScale(20), UiScale(158), UiScale(260), UiScale(22),
            hDlg, (HMENU)IDC_DST_VIEW_DST, g_hInst, NULL);
        SendMessage(hRadDst, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hRadErr = CreateWindowExU8(0, "BUTTON",
            TN("Аппаратный журнал ошибок (Error Log)", "Hardware Error Log"),
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            UiScale(290), UiScale(158), UiScale(260), UiScale(22),
            hDlg, (HMENU)IDC_DST_VIEW_ERR, g_hInst, NULL);
        SendMessage(hRadErr, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        CheckRadioButton(hDlg, IDC_DST_VIEW_DST, IDC_DST_VIEW_ERR, IDC_DST_VIEW_DST);

        /* Multiline Edit Control for Logs */
        HWND hLog = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
            WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
            UiScale(20), UiScale(186), w - UiScale(40), UiScale(320),
            hDlg, (HMENU)IDC_DST_LOG_BOX, g_hInst, NULL);
        if (pState->hFontMono) {
            SendMessage(hLog, WM_SETFONT, (WPARAM)pState->hFontMono, TRUE);
        }

        /* Bottom action buttons */
        HWND hRefresh = CreateWindowExU8(0, "BUTTON",
            TN("Перечитать журналы", "Refresh Logs"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(20), UiScale(518), UiScale(160), UiScale(28),
            hDlg, (HMENU)IDC_DST_REFRESH_BTN, g_hInst, NULL);
        SendMessage(hRefresh, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hCopy = CreateWindowExU8(0, "BUTTON",
            TN("Копировать в буфер", "Copy to Clipboard"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            UiScale(190), UiScale(518), UiScale(160), UiScale(28),
            hDlg, (HMENU)IDC_DST_COPY_BTN, g_hInst, NULL);
        SendMessage(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        HWND hClose = CreateWindowExU8(0, "BUTTON",
            TN("Закрыть", "Close"),
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            w - UiScale(130), UiScale(518), UiScale(110), UiScale(28),
            hDlg, (HMENU)IDC_DST_CLOSE_BTN, g_hInst, NULL);
        SendMessage(hClose, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

        /* Initial check if test is running */
        HANDLE hDrive = INVALID_HANDLE_VALUE;
        if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
            DST_PROGRESS initProg;
            DstPollProgress(hDrive, (DRIVE_INFO*)pInfo, &initProg);
            CloseHandle(hDrive);
            if (initProg.bRunning) {
                pState->bRunning = true;
                SendMessage(hProg, PBM_SETPOS, (WPARAM)initProg.nPercentComplete, 0);
                SetDlgItemTextU8(hDlg, IDC_DST_STATUS_TEXT, initProg.szStatusText);
                EnableWindow(hShortBtn, FALSE);
                EnableWindow(hExtBtn, FALSE);
                EnableWindow(hAbortBtn, TRUE);
                SetTimer(hDlg, IDT_DST_POLL_TIMER, 1000, NULL);
            }
        }

        /* Query logs and populate edit box */
        RefreshDstLogsFromDrive(hDlg, pState);

        return 0;
    }

    case WM_TIMER:
    {
        if (wParam == IDT_DST_POLL_TIMER) {
            DST_DLG_STATE* pState = (DST_DLG_STATE*)GetPropA(hDlg, "DM_DST_STATE");
            if (!pState || pState->nDriveIndex < 0 || pState->nDriveIndex >= g_nDriveCount)
                return 0;
            DRIVE_INFO* pInfo = &g_Drives[pState->nDriveIndex];
            HANDLE hDrive = INVALID_HANDLE_VALUE;
            if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
                DST_PROGRESS prog;
                DstPollProgress(hDrive, pInfo, &prog);
                CloseHandle(hDrive);

                SendMessage(GetDlgItem(hDlg, IDC_DST_PROGRESS), PBM_SETPOS, (WPARAM)prog.nPercentComplete, 0);
                SetDlgItemTextU8(hDlg, IDC_DST_STATUS_TEXT, prog.szStatusText);

                if (!prog.bRunning) {
                    pState->bRunning = false;
                    KillTimer(hDlg, IDT_DST_POLL_TIMER);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_SHORT_BTN), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_EXT_BTN), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_ABORT_BTN), FALSE);
                    RefreshDstLogsFromDrive(hDlg, pState);
                }
            }
        }
        return 0;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);
        DST_DLG_STATE* pState = (DST_DLG_STATE*)GetPropA(hDlg, "DM_DST_STATE");
        if (!pState || pState->nDriveIndex < 0 || pState->nDriveIndex >= g_nDriveCount)
            return 0;
        DRIVE_INFO* pInfo = &g_Drives[pState->nDriveIndex];

        if (id == IDC_DST_SHORT_BTN) {
            HANDLE hDrive = INVALID_HANDLE_VALUE;
            if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
                char szErr[256];
                if (DstStartSelfTest(hDrive, pInfo, DST_TEST_SHORT, szErr, sizeof(szErr))) {
                    pState->bRunning = true;
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_SHORT_BTN), FALSE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_EXT_BTN), FALSE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_ABORT_BTN), TRUE);
                    SendMessage(GetDlgItem(hDlg, IDC_DST_PROGRESS), PBM_SETPOS, 0, 0);
                    SetDlgItemTextU8(hDlg, IDC_DST_STATUS_TEXT,
                        TN("Короткий тест запущен (фоновый режим)...", "Short self-test started (background mode)..."));
                    SetTimer(hDlg, IDT_DST_POLL_TIMER, 1000, NULL);
                } else {
                    MessageBoxU8(hDlg, szErr, "DriveMonitor", MB_OK | MB_ICONWARNING);
                }
                CloseHandle(hDrive);
            }
        }
        else if (id == IDC_DST_EXT_BTN) {
            int ret = MessageBoxU8(hDlg,
                TN("Расширенный тест выполняет полное сканирование всей поверхности накопителя.\n"
                   "Это может занять от 30 минут до нескольких часов в фоновом режиме.\n\n"
                   "Запустить расширенное самотестирование?",
                   "Extended test performs full surface scanning across the drive.\n"
                   "This may take from 30 minutes to several hours in the background.\n\n"
                   "Start extended self-test?"),
                "DriveMonitor", MB_YESNO | MB_ICONQUESTION);
            if (ret == IDYES) {
                HANDLE hDrive = INVALID_HANDLE_VALUE;
                if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
                    char szErr[256];
                    if (DstStartSelfTest(hDrive, pInfo, DST_TEST_EXTENDED, szErr, sizeof(szErr))) {
                        pState->bRunning = true;
                        EnableWindow(GetDlgItem(hDlg, IDC_DST_SHORT_BTN), FALSE);
                        EnableWindow(GetDlgItem(hDlg, IDC_DST_EXT_BTN), FALSE);
                        EnableWindow(GetDlgItem(hDlg, IDC_DST_ABORT_BTN), TRUE);
                        SendMessage(GetDlgItem(hDlg, IDC_DST_PROGRESS), PBM_SETPOS, 0, 0);
                        SetDlgItemTextU8(hDlg, IDC_DST_STATUS_TEXT,
                            TN("Расширенный тест запущен...", "Extended self-test started..."));
                        SetTimer(hDlg, IDT_DST_POLL_TIMER, 1000, NULL);
                    } else {
                        MessageBoxU8(hDlg, szErr, "DriveMonitor", MB_OK | MB_ICONWARNING);
                    }
                    CloseHandle(hDrive);
                }
            }
        }
        else if (id == IDC_DST_ABORT_BTN) {
            HANDLE hDrive = INVALID_HANDLE_VALUE;
            if (OpenDrive(pInfo->nDriveIndex, &hDrive)) {
                char szErr[256];
                if (DstAbortSelfTest(hDrive, pInfo, szErr, sizeof(szErr))) {
                    pState->bRunning = false;
                    KillTimer(hDlg, IDT_DST_POLL_TIMER);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_SHORT_BTN), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_EXT_BTN), TRUE);
                    EnableWindow(GetDlgItem(hDlg, IDC_DST_ABORT_BTN), FALSE);
                    SetDlgItemTextU8(hDlg, IDC_DST_STATUS_TEXT,
                        TN("Тест остановлен пользователем.", "Self-test aborted by user."));
                    RefreshDstLogsFromDrive(hDlg, pState);
                } else {
                    MessageBoxU8(hDlg, szErr, "DriveMonitor", MB_OK | MB_ICONWARNING);
                }
                CloseHandle(hDrive);
            }
        }
        else if (id == IDC_DST_VIEW_DST) {
            pState->bShowingErrorLog = false;
            CheckRadioButton(hDlg, IDC_DST_VIEW_DST, IDC_DST_VIEW_ERR, IDC_DST_VIEW_DST);
            UpdateDstLogDisplay(hDlg, pState);
        }
        else if (id == IDC_DST_VIEW_ERR) {
            pState->bShowingErrorLog = true;
            CheckRadioButton(hDlg, IDC_DST_VIEW_DST, IDC_DST_VIEW_ERR, IDC_DST_VIEW_ERR);
            UpdateDstLogDisplay(hDlg, pState);
        }
        else if (id == IDC_DST_REFRESH_BTN) {
            RefreshDstLogsFromDrive(hDlg, pState);
        }
        else if (id == IDC_DST_COPY_BTN) {
            char* pBuf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 32768);
            if (pBuf) {
                if (pState->bShowingErrorLog) {
                    DstFormatErrorLogHistory(pInfo, pBuf, 32768);
                } else {
                    DstFormatSelfTestHistory(pInfo, pBuf, 32768);
                }
                CopyTextToClipboard(hDlg, pBuf);
                HeapFree(GetProcessHeap(), 0, pBuf);
                MessageBoxU8(hDlg,
                    TN("Журнал скопирован в буфер обмена.", "Log copied to clipboard."),
                    "DriveMonitor", MB_OK | MB_ICONINFORMATION);
            }
        }
        else if (id == IDC_DST_CLOSE_BTN || id == IDCANCEL) {
            SendMessage(hDlg, WM_CLOSE, 0, 0);
        }
        return 0;
    }

    case WM_CLOSE:
    {
        DST_DLG_STATE* pState = (DST_DLG_STATE*)GetPropA(hDlg, "DM_DST_STATE");
        if (pState && pState->bRunning) {
            int ret = MessageBoxU8(hDlg,
                TN("Тест всё ещё выполняется в контроллере накопителя.\n"
                   "Закрыть диалог? (Проверка продолжит выполняться внутри диска)",
                   "Self-test is still executing in the drive controller.\n"
                   "Close dialog? (The test will continue running inside the drive)"),
                "DriveMonitor", MB_YESNO | MB_ICONQUESTION);
            if (ret != IDYES) return 0;
        }
        KillTimer(hDlg, IDT_DST_POLL_TIMER);
        DestroyWindow(hDlg);
        return 0;
    }

    case WM_DESTROY:
    {
        DST_DLG_STATE* pState = (DST_DLG_STATE*)GetPropA(hDlg, "DM_DST_STATE");
        RemovePropA(hDlg, "DM_DST_STATE");
        if (pState) {
            if (pState->hFontMono) DeleteObject(pState->hFontMono);
            free(pState);
        }
        return 0;
    }

    default:
        break;
    }
    return DefWindowProcW(hDlg, uMsg, wParam, lParam);
}

void ShowSelfTestDialog(HWND hParent, int nDriveIndex)
{
    static BOOL bRegistered = FALSE;

    if (g_nDriveCount <= 0 || nDriveIndex < 0 || nDriveIndex >= g_nDriveCount) {
        MessageBoxU8(hParent, Tr(STR_NO_DRIVE), "DriveMonitor", MB_OK | MB_ICONWARNING);
        return;
    }

    const DRIVE_INFO* pInfo = &g_Drives[nDriveIndex];

    if (!bRegistered) {
        WNDCLASSW wc;
        ZeroMemory(&wc, sizeof(wc));
        wc.style         = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance     = g_hInst;
        wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
        wc.lpfnWndProc   = DstDlgProc;
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.hIcon         = LoadIconW(g_hInst, MAKEINTRESOURCEW(IDI_APPICON));
        wc.lpszClassName = L"DriveMonitorSelfTestDlg";
        RegisterClassW(&wc);
        bRegistered = TRUE;
    }

    HWND hExist = FindWindowW(L"DriveMonitorSelfTestDlg", NULL);
    if (hExist) {
        SetForegroundWindow(hExist);
        return;
    }

    DST_DLG_STATE* pState = (DST_DLG_STATE*)calloc(1, sizeof(DST_DLG_STATE));
    if (!pState) return;

    pState->nDriveIndex = nDriveIndex;
    pState->bIsNvme = pInfo->bIsNVMe;
    pState->bIsSsd = (pInfo->bIsNVMe ||
                      pInfo->eType == DRIVE_TYPE_SSD_SATA ||
                      pInfo->eType == DRIVE_TYPE_M2_SATA ||
                      pInfo->eType == DRIVE_TYPE_NVME ||
                      pInfo->eType == DRIVE_TYPE_EMMC);
    pState->bSelfTestSupported = pInfo->bSelfTestSupported;
    pState->nShortMinutes = pInfo->nShortSelfTestMinutes;
    pState->nExtMinutes = pInfo->nExtSelfTestMinutes;
    safe_snprintf(pState->szModel, "%s", pInfo->szModel);
    safe_snprintf(pState->szSerial, "%s", pInfo->szSerial);

    int dlgW = UiScale(720);
    int dlgH = UiScale(590);

    RECT rcParent;
    if (hParent && GetWindowRect(hParent, &rcParent)) {
        /* Center relative to parent */
    } else {
        rcParent.left = 100; rcParent.top = 100;
        rcParent.right = 100 + dlgW; rcParent.bottom = 100 + dlgH;
    }
    int nX = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int nY = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;
    if (nX < 10) nX = 10;
    if (nY < 10) nY = 10;

    char szTitle[256];
    safe_snprintf(szTitle, "%s — %s (%s)",
        TN("Самодиагностика накопителя (DST)", "Hardware Drive Self-Test (DST)"),
        pState->szModel,
        pState->bIsNvme ? "NVMe" : (pState->bIsSsd ? "SSD" : "HDD"));

    WCHAR wzTitle[256];
    U8ToW(szTitle, wzTitle, 256);

    RECT rcAdj;
    rcAdj.left = 0; rcAdj.top = 0; rcAdj.right = dlgW; rcAdj.bottom = dlgH;
    AdjustWindowRectEx(&rcAdj, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE, 0);

    HWND hDlg = CreateWindowExW(
        0,
        L"DriveMonitorSelfTestDlg",
        wzTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        nX, nY,
        rcAdj.right - rcAdj.left,
        rcAdj.bottom - rcAdj.top,
        hParent,
        NULL,
        g_hInst,
        (LPVOID)pState
    );

    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        UpdateWindow(hDlg);
    }
}
