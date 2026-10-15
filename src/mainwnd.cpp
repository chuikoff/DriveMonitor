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

#define DONATE_URL "https://boosty.to/chuikoff"

/* SMART list: ID, name, value, worst, threshold, raw, decode, status. */
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
    return (WORD)p[0] | ((WORD)p[1] << 8);
}

/* NVMe Health Log Data Units: 1 unit = 1000 * 512 = 512000 bytes (NVMe spec).
 * CrystalDiskInfo TBW = units * 512000 / 1e12. */
static void FormatNvmeHostBytes(unsigned __int64 units, char* szBuf, int nBufLen)
{
    double bytes = (double)units * 512000.0;
    double tb = bytes / 1e12;
    if (tb >= 1.0)
        safe_snprintf_n(szBuf, nBufLen, "%.1f ТБ", tb);
    else
        safe_snprintf_n(szBuf, nBufLen, "%.1f ГБ", bytes / 1e9);
}

/* USB enclosure/chip name from SCSI INQUIRY; type name / VID:PID fallback.
 * Names come from DetectUsbBridgeType comments — no extra VID table. */

/* ATA SSD: nSSDTotalWritesGB is a profile host/NAND volume, or -1. */
static void FormatAtaWrites(int nGiB, char* szBuf, int nBufLen)
{
    if (nGiB < 0) { szBuf[0] = '\0'; return; }
    if (nGiB >= 1024)
        safe_snprintf_n(szBuf, nBufLen, "%.1f ТБ", (double)nGiB / 1024.0);
    else
        safe_snprintf_n(szBuf, nBufLen, "%d ГБ", nGiB);
}

/* Unified SMART headline for every drive type:
 *   Запас …  ·  Износ …  ·  Записано …
 * Missing values are an em dash; fields are never omitted. */
static void FormatSmartHeadline(const DRIVE_INFO* pInfo, char* szBuf, int nBufLen)
{
    char szSpare[32], szWear[32], szWritten[32];
    if (!pInfo || nBufLen <= 0) return;
    szBuf[0] = '\0';
    lstrcpynA(szSpare, "н/д", sizeof(szSpare));
    lstrcpynA(szWear, "н/д", sizeof(szWear));
    lstrcpynA(szWritten, "н/д", sizeof(szWritten));

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

    safe_snprintf_n(szBuf, nBufLen, "Запас %s  ·  Износ %s  ·  Записано %s",
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
static void UiDeleteV2Fonts(void);
extern HFONT g_hFontH1, g_hFontVerdict, g_hFontBody, g_hFontBodySb,
             g_hFontLabel, g_hFontLabelSb, g_hFontPill, g_hFontPillN;
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
        { (BYTE)(FVIRTKEY),                      (WORD)VK_F5,         IDC_REREAD_BTN },
    };
    return CreateAcceleratorTable(a, (int)(sizeof(a) / sizeof(a[0])));
}

static HFONT UiMakeFont(int px, int weight, BOOL italic)
{
    return CreateFontA(-UiScale(px), 0, 0, 0, weight, italic, FALSE, FALSE,
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
    UiDeleteV2Fonts();
    g_hFontH1      = UiMakeFont(20, FW_SEMIBOLD, FALSE);
    g_hFontVerdict = UiMakeFont(24, FW_SEMIBOLD, FALSE);
    g_hFontBody    = UiMakeFont(13, FW_NORMAL,   FALSE);
    g_hFontBodySb  = UiMakeFont(13, FW_SEMIBOLD, FALSE);
    g_hFontLabel   = UiMakeFont(12, FW_NORMAL,   FALSE);
    g_hFontLabelSb = UiMakeFont(12, FW_SEMIBOLD, FALSE);
    g_hFontPill    = UiMakeFont(11, FW_SEMIBOLD, FALSE);
    g_hFontPillN   = UiMakeFont(11, FW_MEDIUM,   FALSE);
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
        if (id == IDC_ATTR_LIST) f = g_hFontBody;
        else if (id == IDC_REREAD_BTN || id == IDC_REPORT_BTN || id == IDC_EJECT_BTN ||
                 id == IDC_MONITOR_STATIC || id == IDC_DRIVE_BTN_BASE)
            f = g_hFontLabel;
        SendMessage(h, WM_SETFONT, (WPARAM)f, TRUE);
        if (id == IDC_ATTR_LIST) {
            HWND hHdr = ListView_GetHeader(h);
            if (hHdr) SendMessage(hHdr, WM_SETFONT, (WPARAM)g_hFontLabelSb, TRUE);
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
    InvalidateRect(hWnd, NULL, TRUE);
}

static void UiOnDpiChanged(HWND hWnd, UINT dpi, const RECT* prc)
{
    if (dpi < 96) dpi = 96;
    g_nDpi = (int)dpi;
    RecreateUiFonts();
    UiApplyFonts(hWnd);
    if (prc)
        SetWindowPos(hWnd, NULL, prc->left, prc->top,
                     prc->right - prc->left, prc->bottom - prc->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    LayoutMainWindow(hWnd);
    InvalidateRect(hWnd, NULL, TRUE);
}

static void UiSyncThemeMenu(void);
static HMENU g_hThemeMenu = NULL;
void ExtractTemperatureFromATA(DRIVE_INFO* pInfo);
void ExtractCommonATACounters(DRIVE_INFO* pInfo);
static int  DemoFill(DRIVE_INFO* d);
static void SubclassAttrHeader(HWND hList);
#define GET_Y_LPARAM_DM(lp) ((int)(short)HIWORD(lp))

/* ===================================================================
 * v2 design: palette, theme, fonts and GDI drawing helpers.
 * Flat fills, 1px borders, rounded rects and text only.
 * =================================================================== */
UI_PALETTE g_Pal;
int        g_bUiDark        = 0;
int        g_bDemoMode      = 0;
int        g_nThemeOverride = 0;
static int g_nThemePref     = 0;   /* 0 system, 1 light, 2 dark (UiTheme) */
static HBRUSH g_hbrSide     = NULL;

HFONT g_hFontH1      = NULL;   /* 20 semibold: model */
HFONT g_hFontVerdict = NULL;   /* 24 semibold: verdict word */
HFONT g_hFontBody    = NULL;   /* 13 regular: reason, table cells */
HFONT g_hFontBodySb  = NULL;   /* 13 semibold: card titles, axis values */
HFONT g_hFontLabel   = NULL;   /* 12 regular: secondary lines, axis labels */
HFONT g_hFontLabelSb = NULL;   /* 12 semibold: table header */
HFONT g_hFontPill    = NULL;   /* 11 semibold: status pills, caption */
HFONT g_hFontPillN   = NULL;   /* 11 medium: neutral chips */

static const UI_PALETTE kPalLight = {
    RGB(0xF3,0xF4,0xF6), RGB(0xEB,0xED,0xF0), RGB(0xFF,0xFF,0xFF),
    RGB(0xFF,0xFF,0xFF), RGB(0xE4,0xE7,0xEB), RGB(0xE1,0xE4,0xE8),
    RGB(0xD0,0xD5,0xDB),
    RGB(0x1B,0x1F,0x24), RGB(0x5B,0x64,0x70), RGB(0x8A,0x92,0x9C),
    RGB(0x0F,0x62,0xC9), RGB(0xFA,0xFB,0xFC), RGB(0xFF,0xF8,0xEC),
    RGB(0xE8,0xF0,0xFB), RGB(0xFF,0xFF,0xFF),
    RGB(0xE6,0xF4,0xEA), RGB(0x1E,0x6B,0x34), RGB(0xFD,0xF4,0xD9), RGB(0x7A,0x53,0x00),
    RGB(0xFD,0xEB,0xDD), RGB(0x9A,0x3D,0x0A), RGB(0xFB,0xE4,0xE4), RGB(0xA1,0x1B,0x1B),
    RGB(0x1F,0x29,0x37), RGB(0xFF,0xFF,0xFF), RGB(0x1F,0x29,0x37),
    RGB(0xC9,0xCE,0xD5), RGB(0x54,0x5D,0x69)
};
static const UI_PALETTE kPalDark = {
    RGB(0x1E,0x1E,0x1E), RGB(0x1A,0x1A,0x1A), RGB(0x26,0x26,0x26),
    RGB(0x2E,0x2E,0x2E), RGB(0x24,0x24,0x24), RGB(0x35,0x35,0x35),
    RGB(0x44,0x44,0x44),
    RGB(0xEC,0xEC,0xEC), RGB(0xA9,0xAD,0xB3), RGB(0x7C,0x80,0x87),
    RGB(0x5A,0xA9,0xF5), RGB(0x2A,0x2A,0x2A), RGB(0x33,0x29,0x1F),
    RGB(0x24,0x33,0x46), RGB(0x2E,0x2E,0x2E),
    RGB(0x1D,0x33,0x24), RGB(0x7F,0xD6,0x9A), RGB(0x3A,0x30,0x15), RGB(0xF2,0xCC,0x6B),
    RGB(0x40,0x28,0x1A), RGB(0xFF,0xAD,0x7A), RGB(0x43,0x20,0x1F), RGB(0xFF,0x9C,0x97),
    RGB(0xF2,0xF2,0xF2), RGB(0x11,0x11,0x11), RGB(0xF2,0xF2,0xF2),
    RGB(0x4E,0x53,0x5A), RGB(0xB4,0xB9,0xC0)
};

static int SystemPrefersDark(void)
{
    HKEY k;
    DWORD v = 1, sz = sizeof(v), t = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "AppsUseLightTheme", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS ||
            t != REG_DWORD)
            v = 1;
        RegCloseKey(k);
    }
    return v == 0;
}

static void LoadThemePref(void)
{
    HKEY k;
    DWORD v = 0, sz = sizeof(v), t = 0;
    if (RegOpenKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                      0, KEY_READ, &k) == ERROR_SUCCESS) {
        if (RegQueryValueExA(k, "UiTheme", NULL, &t, (LPBYTE)&v, &sz) != ERROR_SUCCESS ||
            t != REG_DWORD || v > 2)
            v = 0;
        RegCloseKey(k);
    }
    g_nThemePref = (int)v;
}

static void SaveThemePref(int v)
{
    HKEY k;
    DWORD d = (DWORD)v;
    if (RegCreateKeyExA(HKEY_CURRENT_USER, "Software\\chuikoff\\DriveMonitor",
                        0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExA(k, "UiTheme", 0, REG_DWORD, (const BYTE*)&d, sizeof(d));
        RegCloseKey(k);
    }
}

static void UiApplyPalette(void)
{
    int pref = g_nThemeOverride ? g_nThemeOverride : g_nThemePref;
    g_bUiDark = (pref == 2) || (pref == 0 && SystemPrefersDark());
    g_Pal = g_bUiDark ? kPalDark : kPalLight;
    if (g_hbrBG)    { DeleteObject(g_hbrBG);    g_hbrBG    = NULL; }
    if (g_hbrPanel) { DeleteObject(g_hbrPanel); g_hbrPanel = NULL; }
    if (g_hbrSide)  { DeleteObject(g_hbrSide);  g_hbrSide  = NULL; }
    g_hbrBG    = CreateSolidBrush(CLR_BG);
    g_hbrPanel = CreateSolidBrush(CLR_PANEL);
    g_hbrSide  = CreateSolidBrush(CLR_SIDE);
}

/* Dark title bar through DWM (attribute 20, 19 on old Win10 builds).
 * Flat color only; no Mica or backdrop. Missing dwmapi is fine. */
static void UiApplyTitleBar(HWND hWnd)
{
    typedef HRESULT (WINAPI *PFN)(HWND, DWORD, LPCVOID, DWORD);
    static PFN pSet = NULL;
    static int once = 0;
    DWORD on = g_bUiDark ? 1u : 0u;
    if (!once) {
        HMODULE h = LoadLibraryW(L"dwmapi.dll");
        if (h) pSet = (PFN)(void*)GetProcAddress(h, "DwmSetWindowAttribute");
        once = 1;
    }
    if (!pSet || !hWnd) return;
    if (pSet(hWnd, 20, &on, sizeof(on)) != S_OK)
        pSet(hWnd, 19, &on, sizeof(on));
}

static void UiDeleteV2Fonts(void)
{
    HFONT* f[] = { &g_hFontH1, &g_hFontVerdict, &g_hFontBody, &g_hFontBodySb,
                   &g_hFontLabel, &g_hFontLabelSb, &g_hFontPill, &g_hFontPillN };
    size_t i;
    for (i = 0; i < sizeof(f) / sizeof(f[0]); i++)
        if (*f[i]) { DeleteObject(*f[i]); *f[i] = NULL; }
}

static void FillRectColor(HDC hdc, const RECT* r, COLORREF c)
{
    COLORREF old = SetBkColor(hdc, c);
    ExtTextOutW(hdc, 0, 0, ETO_OPAQUE, r, NULL, 0, NULL);
    SetBkColor(hdc, old);
}

/* Rounded card: fill, optional left stripe, 1px border. Region-based so
 * the stripe follows the corner radius. */
static void DrawCard(HDC hdc, const RECT* r, int radius, COLORREF fill,
                     COLORREF border, BOOL bStripe, COLORREF stripe, int stripeW)
{
    HRGN rgn = CreateRoundRectRgn(r->left, r->top, r->right + 1, r->bottom + 1,
                                  radius * 2, radius * 2);
    HBRUSH hb = CreateSolidBrush(fill);
    FillRgn(hdc, rgn, hb);
    DeleteObject(hb);
    if (bStripe) {
        RECT s = { r->left, r->top, r->left + stripeW, r->bottom };
        HBRUSH hs = CreateSolidBrush(stripe);
        SelectClipRgn(hdc, rgn);
        FillRect(hdc, &s, hs);
        SelectClipRgn(hdc, NULL);
        DeleteObject(hs);
    }
    if (border != fill) {
        HBRUSH hbd = CreateSolidBrush(border);
        FrameRgn(hdc, rgn, hbd, 1, 1);
        DeleteObject(hbd);
    }
    DeleteObject(rgn);
}

static void FillRound(HDC hdc, const RECT* r, int radius, COLORREF fill)
{
    HRGN rgn = CreateRoundRectRgn(r->left, r->top, r->right + 1, r->bottom + 1,
                                  radius * 2, radius * 2);
    HBRUSH hb = CreateSolidBrush(fill);
    FillRgn(hdc, rgn, hb);
    DeleteObject(hb);
    DeleteObject(rgn);
}

typedef enum {
    PILL_NONE = 0, PILL_OK, PILL_RISK, PILL_WARN, PILL_BAD, PILL_CRIT,
    PILL_NEUTRAL, PILL_SKIP
} PILL_KIND;

static PILL_KIND PillFromHealth(DRIVE_HEALTH_STATUS e)
{
    switch (e) {
    case HEALTH_STATUS_GOOD:     return PILL_OK;
    case HEALTH_STATUS_OBSERVE:  return PILL_RISK;
    case HEALTH_STATUS_CAUTION:  return PILL_WARN;
    case HEALTH_STATUS_BAD:
    case HEALTH_STATUS_WARNING:  return PILL_BAD;
    case HEALTH_STATUS_CRITICAL: return PILL_CRIT;
    default:                     return PILL_SKIP;
    }
}

static void PillColors(PILL_KIND k, COLORREF* bg, COLORREF* fg)
{
    switch (k) {
    case PILL_OK:   *bg = g_Pal.okBg;   *fg = g_Pal.ok;   break;
    case PILL_RISK: *bg = g_Pal.riskBg; *fg = g_Pal.risk; break;
    case PILL_WARN: *bg = g_Pal.warnBg; *fg = g_Pal.warn; break;
    case PILL_BAD:  *bg = g_Pal.badBg;  *fg = g_Pal.bad;  break;
    case PILL_CRIT: *bg = g_Pal.critBg; *fg = g_Pal.critFg; break;
    default:        *bg = g_Pal.panel;  *fg = g_Pal.neu;  break;
    }
}

/* Foreground (text/dot/stripe) color of a status; GOOD is green. */
static COLORREF StatusFg(DRIVE_HEALTH_STATUS e)
{
    COLORREF bg, fg;
    if (e == HEALTH_STATUS_UNKNOWN) return g_Pal.textFaint;
    PillColors(PillFromHealth(e), &bg, &fg);
    if (e == HEALTH_STATUS_CRITICAL) return g_Pal.critText;
    return fg;
}

/* Status glyph as vector strokes: check, !, cross, diamond, dash.
 * Fonts are not trusted to carry U+2713 on every Windows build. */
static void DrawGlyph(HDC hdc, PILL_KIND k, int x, int yc, int sz, COLORREF c)
{
    int pw = UiScale(3) / 2; if (pw < 1) pw = 1;
    HPEN hp = CreatePen(PS_SOLID, pw, c);
    HPEN hpo = (HPEN)SelectObject(hdc, hp);
    HBRUSH hb = CreateSolidBrush(c);
    HBRUSH hbo = (HBRUSH)SelectObject(hdc, hb);
    int h = sz / 2;
    switch (k) {
    case PILL_OK: {
        POINT p[3] = { { x, yc }, { x + sz * 3 / 8, yc + h * 3 / 4 }, { x + sz, yc - h * 3 / 4 } };
        Polyline(hdc, p, 3);
        break; }
    case PILL_WARN: {
        RECT r1 = { x + h - pw / 2 - (pw > 1 ? 0 : 0), yc - h, x + h - pw / 2 + pw + (pw < 2 ? 1 : 0), yc + h / 3 };
        RECT r2 = { r1.left, yc + h * 2 / 3, r1.right, yc + h + 1 };
        FillRect(hdc, &r1, hb);
        FillRect(hdc, &r2, hb);
        break; }
    case PILL_BAD:
    case PILL_CRIT:
        MoveToEx(hdc, x + 1, yc - h + 1, NULL); LineTo(hdc, x + sz - 1, yc + h - 1);
        MoveToEx(hdc, x + sz - 1, yc - h + 1, NULL); LineTo(hdc, x + 1, yc + h - 1);
        break;
    case PILL_RISK: {
        POINT p[4] = { { x + h, yc - h }, { x + sz, yc }, { x + h, yc + h }, { x, yc } };
        Polygon(hdc, p, 4);
        break; }
    case PILL_SKIP:
        MoveToEx(hdc, x + 1, yc, NULL); LineTo(hdc, x + sz - 1, yc);
        break;
    default:
        break;
    }
    SelectObject(hdc, hbo); DeleteObject(hb);
    SelectObject(hdc, hpo); DeleteObject(hp);
}

/* Pill: h20 r10, 11px semibold, glyph + gap 5 + text, padding 8.
 * Neutral / skip chips are outlined and transparent so they never read as
 * a verdict. Returns the pill width. bMeasure skips drawing. */
static int DrawPillW(HDC hdc, int x, int yc, const WCHAR* wz, PILL_KIND k, BOOL bMeasure)
{
    SIZE sz;
    int h = UiScale(20), pad = UiScale(8), gsz = UiScale(8), gap = UiScale(5);
    BOOL bGlyph = (k != PILL_NEUTRAL);
    BOOL bOutline = (k == PILL_NEUTRAL || k == PILL_SKIP);
    HFONT fo = (HFONT)SelectObject(hdc, bOutline ? g_hFontPillN : g_hFontPill);
    int w;
    COLORREF bg, fg;
    GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);
    w = pad * 2 + sz.cx + (bGlyph ? gsz + gap : 0);
    if (!bMeasure) {
        RECT r = { x, yc - h / 2, x + w, yc - h / 2 + h };
        RECT rt;
        PillColors(k, &bg, &fg);
        if (bOutline) {
            HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, h, h);
            HBRUSH hbd = CreateSolidBrush(g_Pal.neuBd);
            FrameRgn(hdc, rgn, hbd, 1, 1);
            DeleteObject(hbd);
            DeleteObject(rgn);
        } else {
            FillRound(hdc, &r, h / 2, bg);
        }
        if (bGlyph)
            DrawGlyph(hdc, k, x + pad, yc, gsz, fg);
        rt = r;
        rt.left = x + pad + (bGlyph ? gsz + gap : 0);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, fg);
        DrawTextW(hdc, wz, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(hdc, fo);
    return w;
}

static int DrawPillU8(HDC hdc, int x, int yc, const char* s, PILL_KIND k, BOOL bUpper, BOOL bMeasure)
{
    WCHAR wz[96];
    U8ToW(s ? s : "", wz, 96);
    if (bUpper) CharUpperW(wz);
    return DrawPillW(hdc, x, yc, wz, k, bMeasure);
}

static void DrawTextColor(HDC hdc, const char* s, RECT* r, HFONT f, COLORREF c, UINT fmt)
{
    HFONT fo = (HFONT)SelectObject(hdc, f);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, c);
    DrawTextU8(hdc, s, r, fmt | DT_NOPREFIX);
    SelectObject(hdc, fo);
}

static int TextWidthU8(HDC hdc, const char* s, HFONT f)
{
    WCHAR wz[256];
    SIZE sz;
    HFONT fo = (HFONT)SelectObject(hdc, f);
    U8ToW(s ? s : "", wz, 256);
    GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);
    SelectObject(hdc, fo);
    return sz.cx;
}

static const char* RuYears(unsigned long long c)
{
    unsigned long long y = c / 100, m10 = y % 10, m100 = y % 100;
    if (c % 100) return "года";
    if (m10 == 1 && m100 != 11) return "год";
    if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "года";
    return "лет";
}

/* Power-on time in years only, two decimals ("1,96 года" / "1.96 years"). */
static void FormatPohYears(DWORD dwHours, char* buf, int n)
{
    unsigned long long c;
    if (dwHours == 0) { safe_snprintf_n(buf, n, "%s", Tr(STR_POH_NONE)); return; }
    c = ((unsigned long long)dwHours * 100ull + 4380ull) / 8760ull;
    if (c == 0) c = 1;
    if (UiLangIsEn())
        safe_snprintf_n(buf, n, "%llu.%02llu %s", c / 100, c % 100, c == 100 ? "year" : "years");
    else
        safe_snprintf_n(buf, n, "%llu,%02llu %s", c / 100, c % 100,
                        RuYears(c));
}

/* 3412 -> "3 412" (thin grouping, no locale dependency). */
static void FormatGrouped(unsigned long v, char* buf, int n)
{
    char t[32];
    int len, i, o = 0;
    safe_snprintf(t, "%lu", v);
    len = (int)strlen(t);
    for (i = 0; i < len && o < n - 4; i++) {
        if (i > 0 && (len - i) % 3 == 0) {
            buf[o++] = '\xC2'; buf[o++] = '\xA0';
        }
        buf[o++] = t[i];
    }
    buf[o] = '\0';
}

void CreateGDIObjects(void)
{
    LoadThemePref();
    UiApplyPalette();
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
    safe_snprintf(szLine, TN("**Версия:** %s  \r\n", "**Version:** %s  \r\n"),
                  DRIVEMONITOR_FILEVERSION_STR);
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
    BOOL usb = FALSE;
    if (on && g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount)
        usb = g_Drives[g_nSelectedDrive].bIsUSB;
    if (hReread) EnableWindow(hReread, on);
    if (hReport) EnableWindow(hReport, on);
    if (hEject)  EnableWindow(hEject, on && usb);
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
                "Не удалось извлечь диск. Закройте файлы и повторите.",
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
    if (g_hbrSide)     { DeleteObject(g_hbrSide);     g_hbrSide     = NULL; }
    UiDeleteV2Fonts();
}

COLORREF GetHealthStatusColor(DRIVE_HEALTH_STATUS eStatus)
{
    if (eStatus == HEALTH_STATUS_UNKNOWN) return CLR_ACCENT;
    return StatusFg(eStatus);
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

#define DASH_CARD_H   116
#define DASH_TILE_H    52
#define DASH_GAP       12
#define DASH_H        (DASH_CARD_H + DASH_GAP + DASH_TILE_H)
#define DASH_RIGHT_W  290

/* Axis tile wording: GOOD reads as "Норма", others use the short name. */
static const char* GetAxisStatusNameShort(DRIVE_HEALTH_STATUS e)
{
    if (e == HEALTH_STATUS_GOOD) return Tr(STR_AXIS_OK);
    return GetHealthStatusNameShort(e);
}

static const DRIVE_INFO* SelDrive(void)
{
    if (g_nDriveCount > 0 && g_nSelectedDrive >= 0 && g_nSelectedDrive < g_nDriveCount)
        return &g_Drives[g_nSelectedDrive];
    return NULL;
}

static void DashKv(HDC hdc, int x, int r, int yc, const char* k, const char* v,
                   PILL_KIND pill, BOOL bUpper)
{
    RECT rk = { x, yc - UiScale(10), r, yc + UiScale(10) };
    DrawTextColor(hdc, k, &rk, g_hFontLabel, CLR_TEXT_DIM, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    if (pill != PILL_NONE) {
        int w = DrawPillU8(hdc, 0, yc, v, pill, bUpper, TRUE);
        DrawPillU8(hdc, r - w, yc, v, pill, bUpper, FALSE);
    } else {
        RECT rv = rk;
        DrawTextColor(hdc, v, &rv, g_hFontBodySb, CLR_TEXT, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }
}

static void DashTile(HDC hdc, const RECT* r, const char* label, const char* value,
                     DRIVE_HEALTH_STATUS e, BOOL bNeutral)
{
    RECT rl, rv;
    COLORREF dot, txt;
    int pad = UiScale(12), d = UiScale(8);
    DrawCard(hdc, r, UiScale(8), CLR_PANEL, CLR_BORDER, FALSE, 0, 0);
    rl.left = r->left + pad; rl.right = r->right - pad;
    rl.top = r->top + UiScale(9); rl.bottom = rl.top + UiScale(16);
    DrawTextColor(hdc, label, &rl, g_hFontLabel, CLR_TEXT_DIM,
                  DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (bNeutral || e == HEALTH_STATUS_UNKNOWN) { dot = g_Pal.neuBd; txt = CLR_TEXT_FAINT; }
    else if (e == HEALTH_STATUS_GOOD) { dot = g_Pal.ok; txt = CLR_TEXT; }
    else { dot = StatusFg(e); txt = dot; }
    {
        int yc = r->top + UiScale(35);
        RECT rd = { rl.left, yc - d / 2, rl.left + d, yc - d / 2 + d };
        HRGN rg = CreateEllipticRgn(rd.left, rd.top, rd.right + 1, rd.bottom + 1);
        HBRUSH hb = CreateSolidBrush(dot);
        FillRgn(hdc, rg, hb);
        DeleteObject(hb); DeleteObject(rg);
        rv.left = rl.left + d + UiScale(6); rv.right = rl.right;
        rv.top = yc - UiScale(10); rv.bottom = yc + UiScale(10);
        DrawTextColor(hdc, value, &rv, (bNeutral || e == HEALTH_STATUS_UNKNOWN) ? g_hFontBody : g_hFontBodySb,
                      txt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

static void PaintDashboard(HWND hWnd, HDC hdc, int w, int h)
{
    const DRIVE_INFO* p = SelDrive();
    RECT rcAll = { 0, 0, w, h };
    RECT card = { 0, 0, w, UiScale(DASH_CARD_H) };
    DRIVE_HEALTH_STATUS eOurs = p ? p->eHealthStatus : HEALTH_STATUS_UNKNOWN;
    PILL_KIND kOurs = PillFromHealth(eOurs);
    COLORREF fg = StatusFg(eOurs);
    COLORREF bgTint, fgTint;
    int pad = UiScale(18), rw = UiScale(DASH_RIGHT_W);
    int splitX = w - rw;
    char buf[160];

    FillRectColor(hdc, &rcAll, CLR_BG);
    DrawCard(hdc, &card, UiScale(8), CLR_PANEL, CLR_BORDER, TRUE, fg, UiScale(4));
    PillColors(kOurs, &bgTint, &fgTint);

    /* Verdict: glyph disc + word. */
    {
        int cx = UiScale(4) + pad, cy = UiScale(14), d = UiScale(28);
        RECT rd = { cx, cy, cx + d, cy + d };
        HRGN rg = CreateEllipticRgn(rd.left, rd.top, rd.right + 1, rd.bottom + 1);
        HBRUSH hb = CreateSolidBrush(kOurs == PILL_SKIP ? g_Pal.row2 : bgTint);
        RECT rw2;
        FillRgn(hdc, rg, hb);
        DeleteObject(hb); DeleteObject(rg);
        DrawGlyph(hdc, kOurs == PILL_SKIP ? PILL_SKIP : kOurs, cx + d / 2 - UiScale(6),
                  cy + d / 2, UiScale(12), kOurs == PILL_CRIT ? g_Pal.critText : fgTint);
        rw2.left = cx + d + UiScale(10); rw2.right = splitX - pad;
        rw2.top = cy - UiScale(4); rw2.bottom = cy + d + UiScale(4);
        {
            WCHAR wz[64];
            HFONT fo = (HFONT)SelectObject(hdc, g_hFontVerdict);
            U8ToW(p ? GetHealthStatusName(eOurs) : Tr(STR_NO_DATA), wz, 64);
            CharUpperW(wz);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, p ? fg : CLR_TEXT_DIM);
            DrawTextW(hdc, wz, -1, &rw2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, fo);
        }
    }

    /* Reason (up to two lines) and the lecture link. */
    {
        WCHAR wz[512];
        RECT rr = { UiScale(4) + pad, UiScale(50), splitX - pad, UiScale(50) + UiScale(36) };
        RECT rc2 = rr;
        HFONT fo;
        int lineH;
        HWND hParent = GetParent(hWnd);
        wz[0] = 0;
        if (hParent) GetDlgItemTextW(hParent, IDC_PREDICT_STATIC, wz, 512);
        fo = (HFONT)SelectObject(hdc, g_hFontBody);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TEXT);
        DrawTextW(hdc, wz, -1, &rc2, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
        lineH = UiScale(18);
        if (rc2.bottom > rr.bottom) rc2.bottom = rr.bottom;
        rc2.right = rr.right;
        DrawTextW(hdc, wz, -1, &rc2, DT_WORDBREAK | DT_EDITCONTROL | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, fo);
        if (p) {
            RECT rl = { rr.left, rc2.bottom + UiScale(6), rr.right, rc2.bottom + UiScale(6) + lineH };
            if (rl.bottom > UiScale(DASH_CARD_H) - UiScale(8)) {
                rl.bottom = UiScale(DASH_CARD_H) - UiScale(8);
                rl.top = rl.bottom - lineH;
            }
            DrawTextColor(hdc, TN("Почему такая оценка \xE2\x86\x92", "Why this assessment \xE2\x86\x92"),
                          &rl, g_hFontLabel, CLR_ACCENT, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (GetFocus() == hWnd) {
                RECT rf = rl;
                rf.right = rf.left + TextWidthU8(hdc, TN("Почему такая оценка \xE2\x86\x92",
                                                         "Why this assessment \xE2\x86\x92"), g_hFontLabel);
                InflateRect(&rf, UiScale(3), UiScale(1));
                SetTextColor(hdc, CLR_TEXT);
                SetBkColor(hdc, CLR_PANEL);
                DrawFocusRect(hdc, &rf);
            }
        }
    }

    /* Right column: drive self-report vs our assessment, then facts. */
    {
        int x = splitX + pad, r = w - pad;
        HPEN hp = CreatePen(PS_SOLID, 1, CLR_BORDER);
        HPEN hpo = (HPEN)SelectObject(hdc, hp);
        MoveToEx(hdc, splitX, 1, NULL); LineTo(hdc, splitX, UiScale(DASH_CARD_H) - 1);
        MoveToEx(hdc, x, UiScale(62), NULL); LineTo(hdc, r, UiScale(62));
        SelectObject(hdc, hpo); DeleteObject(hp);
        if (p) {
            const char* disk;
            PILL_KIND kd = PillFromHealth(p->eDiskStatus);
            if (p->eDiskStatus == HEALTH_STATUS_GOOD)
                disk = TN("Исправен (SMART OK)", "Healthy (SMART OK)");
            else
                disk = GetDiskStatusName(p);
            DashKv(hdc, x, r, UiScale(22), TN("Диск о себе", "Drive self-report"), disk, kd, FALSE);
            DashKv(hdc, x, r, UiScale(46), TN("Наша оценка", "Our assessment"),
                   GetHealthStatusNameShort(eOurs), kOurs, FALSE);
            FormatPohYears(p->dwPowerOnHours, buf, (int)sizeof(buf));
            DashKv(hdc, x, r, UiScale(80), Tr(STR_POH), buf, PILL_NONE, FALSE);
            if (p->dwPowerCycleCount > 0)
                FormatGrouped((unsigned long)p->dwPowerCycleCount, buf, (int)sizeof(buf));
            else
                safe_snprintf(buf, "\xE2\x80\x94");
            DashKv(hdc, x, r, UiScale(100), TN("Включений", "Power cycles"), buf, PILL_NONE, FALSE);
        }
    }

    /* Five axis tiles. */
    {
        int top = UiScale(DASH_CARD_H + DASH_GAP), gap = UiScale(8), i;
        int tw = (w - gap * 4) / 5;
        char tbuf[64], wbuf[64];
        const char* labels[5] = { Tr(STR_MEDIA), Tr(STR_INTERFACE), Tr(STR_TEMPERATURE),
                                  Tr(STR_MECHANICS), TN("Ресурс", "Endurance") };
        const char* vals[5] = { "", "", "", "", "" };
        DRIVE_HEALTH_STATUS st[5] = { HEALTH_STATUS_UNKNOWN, HEALTH_STATUS_UNKNOWN,
            HEALTH_STATUS_UNKNOWN, HEALTH_STATUS_UNKNOWN, HEALTH_STATUS_UNKNOWN };
        BOOL neu[5] = { FALSE, FALSE, FALSE, FALSE, FALSE };
        BOOL caseFix[5] = { TRUE, TRUE, FALSE, TRUE, FALSE };
        if (p) {
            BOOL bHdd = (p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe);
            st[0] = p->eReliability; vals[0] = GetAxisStatusNameShort(p->eReliability);
            st[1] = p->eInterface;   vals[1] = GetAxisStatusNameShort(p->eInterface);
            if (p->nTemperatureC > 0) {
                WCHAR wz[32]; char low[64];
                U8ToW(GetAxisStatusName(p->eTempStatus, TRUE), wz, 32);
                CharLowerW(wz);
                WToU8(wz, low, (int)sizeof(low));
                safe_snprintf(tbuf, "%d\xC2\xA0\xC2\xB0""C \xC2\xB7 %s", p->nTemperatureC, low);
                st[2] = p->eTempStatus; vals[2] = tbuf;
            } else { vals[2] = Tr(STR_POH_NONE); neu[2] = TRUE; }
            if (bHdd || p->nGSenseEvents >= 0) {
                st[3] = p->eMechanics;
                vals[3] = (p->eMechanics == HEALTH_STATUS_GOOD) ? Tr(STR_AXIS_OK)
                        : (p->eMechanics == HEALTH_STATUS_UNKNOWN) ? Tr(STR_POH_NONE)
                        : GetAxisStatusNameShort(p->eMechanics);
            } else { vals[3] = TN("н/п для SSD", "n/a for SSD"); neu[3] = TRUE; }
            if (bHdd) { vals[4] = TN("н/п для HDD", "n/a for HDD"); neu[4] = TRUE; }
            else {
                /* eWear stays BAD at 5% or less; the tile stops at CAUTION
                 * like the headline. Remaining endurance only, never Health%. */
                DRIVE_HEALTH_STATUS sw = p->eWear;
                if (sw == HEALTH_STATUS_BAD || sw == HEALTH_STATUS_WARNING ||
                    sw == HEALTH_STATUS_CRITICAL) sw = HEALTH_STATUS_CAUTION;
                st[4] = sw;
                if (p->eWear != HEALTH_STATUS_UNKNOWN && p->nEndurancePercent >= 0)
                    safe_snprintf(wbuf, "%s \xC2\xB7 %d%%", GetAxisStatusNameShort(sw),
                                  p->nEndurancePercent);
                else if (p->eWear != HEALTH_STATUS_UNKNOWN)
                    safe_snprintf(wbuf, "%s", GetAxisStatusNameShort(sw));
                else if (p->nEndurancePercent >= 0)
                    safe_snprintf(wbuf, "%d%%", p->nEndurancePercent);
                else { safe_snprintf(wbuf, "%s", TN("нет данных", "no data")); neu[4] = TRUE; }
                vals[4] = wbuf;
            }
        }
        for (i = 0; i < 5; i++) {
            /* Sentence case for tile values: "Норма", "Внимание". */
            char sc[96];
            WCHAR wz[96];
            U8ToW(vals[i], wz, 96);
            if (wz[0] && caseFix[i] && !neu[i]) CharLowerW(wz + 1);
            WToU8(wz, sc, (int)sizeof(sc));
            vals[i] = NULL;
            {
            RECT r;
            r.left = i * (tw + gap);
            r.right = (i == 4) ? w : r.left + tw;
            r.top = top; r.bottom = h;
            DashTile(hdc, &r, labels[i], sc, st[i], neu[i] || !p);
            }
        }
    }
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
            if (w > 0 && h > 0) {
                HDC     hdc    = CreateCompatibleDC(hdcReal);
                HBITMAP hbmBuf = CreateCompatibleBitmap(hdcReal, w, h);
                HBITMAP hbmOld = (HBITMAP)SelectObject(hdc, hbmBuf);
                PaintDashboard(hWnd, hdc, w, h);
                BitBlt(hdcReal, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
                SelectObject(hdc, hbmOld);
                DeleteObject(hbmBuf);
                DeleteDC(hdc);
            }
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_LBUTTONDOWN:
        SetFocus(hWnd);
        return 0;

    case WM_LBUTTONUP:
        if (GET_Y_LPARAM_DM(lParam) < UiScale(DASH_CARD_H) && SelDrive()) {
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
        InvalidateRect(hWnd, NULL, FALSE);
        return 0;

    case WM_SETCURSOR:
        {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hWnd, &pt);
            SetCursor(LoadCursor(NULL, (pt.y < UiScale(DASH_CARD_H) && SelDrive())
                                       ? (LPCTSTR)IDC_HAND : (LPCTSTR)IDC_ARROW));
        }
        return TRUE;
    }

    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}

/* Small drive-type icon: SSD = board with two traces, HDD = platter,
 * USB adds a plug stem. 1px strokes in the dim text color. */
static void DrawDriveIcon(HDC hdc, const RECT* box, const DRIVE_INFO* p)
{
    int cx = (box->left + box->right) / 2, cy = (box->top + box->bottom) / 2;
    int u = UiScale(1);
    HPEN hp = CreatePen(PS_SOLID, u < 1 ? 1 : u, CLR_TEXT_DIM);
    HPEN hpo = (HPEN)SelectObject(hdc, hp);
    HBRUSH hbo = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
    BOOL bHdd = (p->eType == DRIVE_TYPE_HDD && !p->bIsNVMe);
    if (!bHdd) {
        RoundRect(hdc, cx - UiScale(7), cy - UiScale(5), cx + UiScale(8), cy + UiScale(6), UiScale(3), UiScale(3));
        MoveToEx(hdc, cx - UiScale(4), cy - UiScale(2), NULL); LineTo(hdc, cx - UiScale(1), cy - UiScale(2));
        MoveToEx(hdc, cx - UiScale(4), cy + UiScale(2), NULL); LineTo(hdc, cx + UiScale(2), cy + UiScale(2));
    } else {
        int top = p->bIsUSB ? cy - UiScale(8) : cy - UiScale(7);
        int bot = p->bIsUSB ? cy + UiScale(4) : cy + UiScale(8);
        int ccy = (top + bot) / 2 - (p->bIsUSB ? 0 : UiScale(1));
        RoundRect(hdc, cx - UiScale(6), top, cx + UiScale(7), bot, UiScale(3), UiScale(3));
        Ellipse(hdc, cx - UiScale(3), ccy - UiScale(3), cx + UiScale(4), ccy + UiScale(4));
        if (p->bIsUSB) {
            Rectangle(hdc, cx - UiScale(2), bot - 1, cx + UiScale(3), cy + UiScale(8));
        } else {
            MoveToEx(hdc, cx - UiScale(3), bot - UiScale(3), NULL); LineTo(hdc, cx + UiScale(4), bot - UiScale(3));
        }
    }
    SelectObject(hdc, hbo);
    SelectObject(hdc, hpo);
    DeleteObject(hp);
}

static void PaintDriveCard(HDC hdc, int w, int h, int nIdx, BOOL bSelected, BOOL bHover)
{
    RECT rc = { 0, 0, w, h };
    FillRectColor(hdc, &rc, CLR_SIDE);
    if (bSelected)
        DrawCard(hdc, &rc, UiScale(6), g_Pal.sel, CLR_BORDER, FALSE, 0, 0);
    else if (bHover)
        FillRound(hdc, &rc, UiScale(6), g_Pal.hover);
    if (nIdx < 0 || nIdx >= g_nDriveCount) return;
    {
        const DRIVE_INFO* pD = &g_Drives[nIdx];
        char szName[64], szSub[96], szCap[24], szT[24];
        const char* bus;
        RECT bar = { UiScale(5), UiScale(12), UiScale(8), h - UiScale(12) };
        RECT box = { UiScale(16), UiScale(10), UiScale(48), UiScale(42) };
        int tx = UiScale(58);
        RECT rn = { tx, UiScale(9), w - UiScale(10), UiScale(27) };
        RECT rs = { tx, UiScale(27), w - UiScale(10), UiScale(43) };
        int pw;

        FillRound(hdc, &bar, UiScale(2) > 1 ? UiScale(2) / 2 + 1 : 1, StatusFg(pD->eHealthStatus));
        DrawCard(hdc, &box, UiScale(6), CLR_PANEL, CLR_BORDER, FALSE, 0, 0);
        DrawDriveIcon(hdc, &box, pD);

        if (pD->szModel[0]) safe_snprintf(szName, "%s", pD->szModel);
        else safe_snprintf(szName, TN("Диск %d", "Drive %d"), pD->nDriveIndex);
        DrawTextColor(hdc, szName, &rn, g_hFontBodySb, CLR_TEXT,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        FormatSize(pD->dwCapacityMB, szCap, sizeof(szCap));
        bus = pD->bIsUSB ? "USB" : (pD->bIsNVMe ? "NVMe" : "SATA");
        {
            const char* t = GetDriveTypeName(pD->eType);
            safe_snprintf(szSub, "%s \xC2\xB7 %s \xC2\xB7 %s", szCap, t ? t : "?", bus);
        }
        DrawTextColor(hdc, szSub, &rs, g_hFontLabel, CLR_TEXT_DIM,
                      DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

        if (pD->eHealthStatus == HEALTH_STATUS_UNKNOWN)
            pw = DrawPillU8(hdc, tx, UiScale(60), TN("нет оценки", "no verdict"), PILL_SKIP, FALSE, FALSE);
        else
            pw = DrawPillU8(hdc, tx, UiScale(60), GetHealthStatusNameShort(pD->eHealthStatus),
                            PillFromHealth(pD->eHealthStatus), TRUE, FALSE);
        if (pD->nTemperatureC > 0) {
            RECT rt = { tx + pw + UiScale(8), UiScale(50), w - UiScale(8), UiScale(70) };
            COLORREF ct = CLR_TEXT_DIM;
            if (pD->eTempBand == TEMP_BAND_HIGH || pD->eTempBand == TEMP_BAND_CRITICAL) ct = g_Pal.bad;
            else if (pD->eTempBand == TEMP_BAND_ELEVATED) ct = g_Pal.warn;
            safe_snprintf(szT, "%d\xC2\xA0\xC2\xB0""C", pD->nTemperatureC);
            DrawTextColor(hdc, szT, &rt, g_hFontLabel, ct, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
    }
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

            PaintDriveCard(hdc, w, h, nIdx, nIdx == g_nSelectedDrive,
                           GetPropA(hWnd, "hover") != NULL);

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
    int nBtnW  = UiScale(DRIVE_BTN_PANEL_W - 24);
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
                UiScale(12), nStartY, nBtnW, nBtnH,
                hWnd, (HMENU)(IDC_DRIVE_BTN_BASE), g_hInst, NULL);
            SendMessage(hPlaceholder, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            g_hDriveBtn[0] = hPlaceholder;
        } else {

        for (i = 0; i < g_nDriveCount && i < MAX_DRIVES; i++) {
            int nY = nStartY + i * (nBtnH + UiScale(DRIVE_BTN_GAP));
            g_hDriveBtn[i] = CreateWindowExU8(
                0, "LLHDDriveBtn", "",
                WS_CHILD | WS_VISIBLE,
                UiScale(12), nY, nBtnW, nBtnH,
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
        SetDlgItemTextU8(hWnd, IDC_BRAND_STATIC, brand);
    }
    SetDlgItemTextU8(hWnd, IDC_CONTROLLER_STATIC, DriveControllerLabel(pInfo));

    SetDlgItemTextU8(hWnd, IDC_SERIAL_STATIC,
                    strlen(pInfo->szSerial) ? pInfo->szSerial : "-");

    SetDlgItemTextU8(hWnd, IDC_FIRMWARE_STATIC,
                    strlen(pInfo->szFirmware) ? pInfo->szFirmware : "-");

    {
        char szSize[32];
        FormatSize(pInfo->dwCapacityMB, szSize, sizeof(szSize));
        safe_snprintf(szBuf, TN("%s   Тип: %s", "%s   Type: %s"),
                  szSize, GetDriveTypeName(pInfo->eType));
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
            "Не удалось прочитать SMART. Запустите от имени администратора и нажмите «Обновить».",
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
            /* Non-Samsung life is the low 16 bits of RAW, the same figure
             * the lecture stores. Value can stay at 100 after RAW steps
             * (Apacer AS350: Value 100, RAW 99). Samsung RAW is an erase
             * count, so Value stays. When Value and RAW disagree and the
             * headline did not take this RAW, keep Value: Phison then
             * reports life from A9. */
            /* Maxio 231: Value is life remaining, RAW is percent used.
             * Samsung RAW is an erase count. Both stay on Value. */
            if (pDrv && !(pDrv->nProfileFlags & PROF_F_E7_VALUE) && w16 <= 100) {
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
            safe_snprintf(szMain, "%u ms", wMs);
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
            safe_snprintf(szMain, "%u ош. чтения / %lu секторов", nErr, (unsigned long)nOps);
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
        WORD wTotal = (WORD)pRaw[0] | ((WORD)pRaw[1] << 8);
        WORD wOver5 = (WORD)pRaw[2] | ((WORD)pRaw[3] << 8);
        WORD wOver75 = (WORD)pRaw[4] | ((WORD)pRaw[5] << 8);
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
        if (DriveTreatsC0AsPowerLoss(pDrv))
            safe_snprintf(szMain, TN("%lu событий", "%lu events"), (unsigned long)dw32);
        else
            safe_snprintf(szMain, TN("%lu парковок при выключении", "%lu power-off retracts"), (unsigned long)dw32);
        break;

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
            safe_snprintf(szMain, "%u ошибок чтения / %lu секторов", nErr, (unsigned long)nOps);
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
            safe_snprintf(szMain, "%u ошибок позиц. / %lu seeks", nErr, (unsigned long)nOps);
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
                if (bID == 0xF1)
                    safe_snprintf(szMain, TN("%llu ГБ записано хостом",
                                             "%llu GB written by host"),
                                  (unsigned long long)nGB);
                else
                    safe_snprintf(szMain, TN("%llu ГБ прочитано хостом",
                                             "%llu GB read by host"),
                                  (unsigned long long)nGB);
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
                if (bID == 0xF1)
                    safe_snprintf(szMain, TN("%llu ГиБ записано хостом",
                                             "%llu GiB written by host"),
                                  (unsigned long long)qw48);
                else
                    safe_snprintf(szMain, TN("%llu ГиБ прочитано хостом",
                                             "%llu GiB read by host"),
                                  (unsigned long long)qw48);
                break;
            }
        }
        {
            unsigned __int64 nLBA = qw48;
            unsigned __int64 nBytes = nLBA * 512ULL;
            if (nBytes >= 1000000000000ULL) {
                unsigned long long tenths =
                    (unsigned long long)(nBytes / 100000000000ULL);
                safe_snprintf(szMain, "%llu LBA  (~%llu.%llu TB)",
                              (unsigned long long)nLBA,
                              tenths / 10ULL, tenths % 10ULL);
            } else if (nBytes >= 1000000000ULL) {
                safe_snprintf(szMain, "%llu LBA  (~%llu GB)",
                              (unsigned long long)nLBA,
                              (unsigned long long)(nBytes / 1000000000ULL));
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
        if (have9 && enc9 == PROF_ENC_PAIR) {
            unsigned lo = (unsigned)(dw32 & 0xFFFFu);
            unsigned hi = (unsigned)((dw32 >> 16) & 0xFFFFu);
            safe_snprintf(szMain, "%u / %u", lo, hi);
            break;
        }
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
                safe_snprintf(szMain, TN("0%%  заявленный остаток ресурса",
                                         "0%% reported life remaining"));
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
        if (pDrv && (pDrv->nProfileFlags & PROF_F_B1_LIFE) && bVal <= 100) {
            safe_snprintf(szMain, TN("износ/остаток %u%%", "wear/life %u%%"),
                          (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xCA:
    {
        int semCa = PROF_SEM_NONE, encCa = PROF_ENC_NONE;
        if (AttrEncOf(pDrv, bID, &semCa, &encCa) &&
            semCa == PROF_SEM_ENDURANCE && encCa == PROF_ENC_PERCENT &&
            bVal <= 100) {
            safe_snprintf(szMain, TN("%u%% ресурса", "%u%% life remaining"),
                          (unsigned)bVal);
            break;
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
        } else if (encA == PROF_ENC_PAIR) {
            /* MKP-521 / z54z10: bytes [1:0] early, bytes [5:4] later.
             * A low/high 24-bit split labels the factory count as later. */
            unsigned early = (unsigned)pRaw[0] | ((unsigned)pRaw[1] << 8);
            unsigned later = (unsigned)pRaw[4] | ((unsigned)pRaw[5] << 8);
            safe_snprintf(szMain, TN("%u поздних / %u ранних",
                                     "%u later / %u early"),
                          later, early);
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
            if (nGiB >= 1024ULL)
                safe_snprintf(szMain, "%lu единиц (32 MiB)  (~%llu TB)",
                              (unsigned long)dw32, (unsigned long long)(nGiB / 1024ULL));
            else
                safe_snprintf(szMain, "%lu единиц (32 MiB)  (~%llu GiB)",
                              (unsigned long)dw32, (unsigned long long)nGiB);
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
            unsigned __int64 nGB  = nLBA / (1024ULL * 1024ULL * 2ULL);
            if (nGB >= 1024)
                safe_snprintf(szMain, "%llu LBA  (~%llu TB)", (unsigned long long)nLBA, (unsigned long long)(nGB / 1024ULL));
            else if (nGB > 0)
                safe_snprintf(szMain, "%llu LBA  (~%llu GB)", (unsigned long long)nLBA, (unsigned long long)nGB);
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

    if (ssd && (id == 0xE7 || id == 0xA9)) {
        int nLeft = p->nEndurancePercent;
        int semLife = PROF_SEM_NONE;
        BOOL lifeId = (id == 0xE7 && E7IsLifePercent(p)) ||
                      (id == 0xA9 &&
                       ProfileLookup(p, 0xA9, &semLife, NULL) &&
                       semLife == PROF_SEM_ENDURANCE);
        if (!lifeId || nLeft < 0) return "Не оценивается";
        return RankToken(WearBadgeStatus(nLeft));
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
    if ((id == 0xA0 || id == 0xAE) && GetRawValue(a->bRawValue) > 0)
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

/* Six ATA raw bytes, high byte first. The reading lives in its own column. */
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
        /* NVMe has no ATA Value. raw_ is the log field; dec_ is the reading. */
        #define NVME_ROW_EX(id_, name_, val_, worst_, thresh_, raw_, dec_, stat_) \
        { \
            if (nDesired < MAX_ATTR_ROWS) \
                AttrRowSet(&rows[nDesired++], (id_), (name_), (val_), \
                           (worst_), (thresh_), (raw_), (dec_), (stat_)); \
        }
        #define NVME_ROW(id_, name_, dec_, raw_, stat_) \
            NVME_ROW_EX(id_, name_, NVME_NA, NVME_NA, NVME_NA, raw_, dec_, stat_)

        char szCrit[64], szCritRaw[16], szVBuf[64];
        char szSpare[32], szSpareRaw[16], szSpTh[32], szSpThRaw[16];
        char szPctU[32], szPctRaw[16];
        char szDUR[64], szDURRaw[32], szDUW[64], szDUWRaw[32];
        char szPOH[64], szPOHRaw[32], szPC[32];
        char szUS[32], szME[32], szEL[32], szWCT[32], szWCTRaw[16];
        char szCCT[32], szCCTRaw[16];

        safe_snprintf(szCritRaw, "%u", (unsigned)pLog->CriticalWarning);
        if (pLog->CriticalWarning == 0) safe_snprintf(szCrit, TN("0 (нет)", "0 (none)"));
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
                NVME_ROW_EX("02h", TN("Температура", "Temperature"), NVME_NA,
                            worstT, thrT, szRawT, szVBuf, szTstat);
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
        NVME_ROW_EX("03h", TN("Запас блоков", "Available spare"), NVME_NA,
            NVME_NA, szSpTh, szSpareRaw, szSpare,
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

    if (g_bDemoMode) {
        static int s_bDemoFirst = 1;
        g_nScanBufCount = DemoFill(g_ScanBuf);
        if (s_bDemoFirst) { g_nSelectedDrive = 1; s_bDemoFirst = 0; }
        PostMessage(hWnd, WM_APP_REFRESH_DONE, 0, 0);
        return;
    }

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
            if (hIc) SendMessageA(hIco, STM_SETICON, (WPARAM)hIc, 0);

            HFONT hFontBold = UiMakeFont(15, FW_BOLD, FALSE);
            SetPropA(hDlg, PROP_ABOUT_FONT_BOLD, (HANDLE)hFontBold);
            HWND hName = CreateWindowExU8(0, "STATIC",
                "DriveMonitor " DRIVEMONITOR_FILEVERSION_STR,
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
            if (hBold) { DeleteObject(hBold); RemovePropA(hDlg, PROP_ABOUT_FONT_BOLD); }
            if (hLink) { DeleteObject(hLink); RemovePropA(hDlg, PROP_ABOUT_FONT_LINK); }
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
        "О программе — DriveMonitor",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        nX, nY, nWinW, nWinH,
        hWndParent, NULL, g_hInst, NULL
    );
    if (!hDlg) return;

    HICON hIco = LoadIconA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON));
    SendMessageA(hDlg, WM_SETICON, ICON_BIG,   (LPARAM)hIco);
    SendMessageA(hDlg, WM_SETICON, ICON_SMALL, (LPARAM)
        LoadImageA(g_hInst, MAKEINTRESOURCEA(IDI_APPICON),
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

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
                   IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));

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
    AppendMenuU8(g_hViewMenu, MF_SEPARATOR, 0, NULL);
    g_hThemeMenu = CreatePopupMenu();
    AppendMenuU8(g_hThemeMenu, MF_STRING, IDM_THEME_SYSTEM, TN("Как в системе", "Use system setting"));
    AppendMenuU8(g_hThemeMenu, MF_STRING, IDM_THEME_LIGHT,  TN("Светлая", "Light"));
    AppendMenuU8(g_hThemeMenu, MF_STRING, IDM_THEME_DARK,   TN("Тёмная", "Dark"));
    AppendMenuU8(g_hViewMenu, MF_POPUP, (UINT_PTR)g_hThemeMenu, TN("Тема", "Theme"));
    AppendMenuU8(hMenuBar, MF_POPUP, (UINT_PTR)g_hViewMenu, Tr(STR_VIEW));
    UiSyncZoomMenu();
    UiSyncThemeMenu();

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

/* szProtocol is filled at scan time. Swap the speed unit when the UI language changes. */
static void RefitProtocolLang(char* s, int n)
{
    const char* from;
    const char* to;
    char tmp[64];
    char* p;
    size_t flen;
    if (!s || !s[0] || n <= 0) return;
    if (UiLangIsEn()) {
        from = "Гбит/с";
        to = "Gb/s";
    } else {
        from = "Gb/s";
        to = "Гбит/с";
    }
    lstrcpynA(tmp, s, (int)sizeof(tmp));
    p = strstr(tmp, from);
    if (!p) return;
    flen = strlen(from);
    *p = '\0';
    safe_snprintf_n(s, n, "%s%s%s", tmp, to, p + flen);
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

    /* v2: the caption, header facts, axes and the prompt are painted by
     * PaintMainV2 / PaintDashboard. The statics stay as hidden data holders
     * so SetDlgItemText callers keep working. */
    HWND hDriveLabel = CreateWindowExU8(0, "STATIC", Tr(STR_DRIVES),
        WS_CHILD | SS_LEFT,
        6, 8, DRIVE_BTN_PANEL_W - 12, 16,
        hWnd, (HMENU)(IDC_DRIVE_LIST), g_hInst, NULL);
    SendMessage(hDriveLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    int nRightX = DRIVE_BTN_PANEL_W + 10;
    int nBarsW  = 190;

    HWND hLabel = CreateWindowExU8(0, "STATIC", Tr(STR_DISK_ASSESS),
        WS_CHILD | SS_LEFT,
        nRightX, 40, nBarsW, 14,
        hWnd, (HMENU)IDC_HEALTH_LABEL, g_hInst, NULL);
    SendMessage(hLabel, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);

    g_hHealthBar = CreateWindowExU8(0, "LLHDHealthBar", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        nRightX, 56, nBarsW, 48,
        hWnd, (HMENU)IDC_HEALTH_BAR_FRAME, g_hInst, NULL);

    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_REREAD),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        nRightX, 110, 90, 24,
        hWnd, (HMENU)IDC_REREAD_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_REPORT_BTN),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        nRightX + 100, 110, 90, 24,
        hWnd, (HMENU)IDC_REPORT_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE); }
    { HWND h = CreateWindowExU8(0, "BUTTON", Tr(STR_EJECT_BTN),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        nRightX, 138, nBarsW, 24,
        hWnd, (HMENU)IDC_EJECT_BTN, g_hInst, NULL);
      SendMessage(h, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
      EnableWindow(h, FALSE); }

    {
        static const struct { int idL; int idV; int sid; } ax[] = {
            { IDC_AXIS_MEDIA_L, IDC_AXIS_MEDIA_V, STR_MEDIA },
            { IDC_AXIS_IFACE_L, IDC_AXIS_IFACE_V, STR_INTERFACE },
            { IDC_AXIS_TEMPA_L, IDC_AXIS_TEMPA_V, STR_TEMPERATURE },
            { IDC_AXIS_ROW4_L,  IDC_AXIS_ROW4_V,  STR_WEAR },
        };
        int i;
        for (i = 0; i < 4; i++) {
            int y = 166 + 16 * i;
            HWND hL = CreateWindowExU8(0, "STATIC", Tr(ax[i].sid),
                WS_CHILD | SS_LEFT,
                nRightX, y, 78, 16,
                hWnd, (HMENU)(UINT_PTR)ax[i].idL, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | SS_LEFT,
                nRightX + 80, y, nBarsW - 80, 16,
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
                WS_CHILD | SS_LEFT,
                nInfoX, y, nLblW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idLbl, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", rows[r].val,
                WS_CHILD | SS_LEFT,
                nValX, y, nValW, nInfoH,
                hWnd, (HMENU)(UINT_PTR)rows[r].idVal, g_hInst, NULL);
            SendMessage(hL, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
            SendMessage(hV, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
        }
    }

    HWND hPred = CreateWindowExU8(0, "STATIC", "",
        WS_CHILD | SS_LEFT,
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
        0, "SysListView32", "",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        nRightX, 298, 540, 575,
        hWnd, (HMENU)IDC_ATTR_LIST, g_hInst, NULL
    );
    SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

    ListView_SetBkColor(hList, CLR_PANEL);
    ListView_SetTextBkColor(hList, CLR_ROW1);
    ListView_SetTextColor(hList, CLR_TEXT);

    {
        LVCOLUMNW col;
        WCHAR w0[16], w1[64], w2[64], w3[64], w4[64], w5[64], w6[64], w7[64];
        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
        col.fmt  = LVCFMT_LEFT;

        U8ToW("ID", w0, 16);
        col.cx = 72;  col.pszText = w0;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 0, (LPARAM)&col);
        U8ToW(Tr(STR_COL_PARAM), w1, 64);
        col.cx = 200; col.pszText = w1;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 1, (LPARAM)&col);
        U8ToW(Tr(STR_COL_VALUE), w2, 64);
        col.cx = 70;  col.pszText = w2;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 2, (LPARAM)&col);
        U8ToW(Tr(STR_COL_WORST), w3, 64);
        col.cx = 50;  col.pszText = w3;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 3, (LPARAM)&col);
        U8ToW(Tr(STR_COL_THRESH), w4, 64);
        col.cx = 50;  col.pszText = w4;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 4, (LPARAM)&col);
        U8ToW("RAW", w5, 64);
        col.cx = 108; col.pszText = w5;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_RAW, (LPARAM)&col);
        U8ToW(Tr(STR_COL_DECODE), w6, 64);
        col.cx = 180; col.pszText = w6;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_DECODE, (LPARAM)&col);
        U8ToW(Tr(STR_COL_STATUS), w7, 64);
        col.cx = 116; col.pszText = w7;
        SendMessageW(hList, LVM_INSERTCOLUMNW, ATTR_COL_STATUS, (LPARAM)&col);
    }
}

static LRESULT HandleCtlColor(HWND hWnd, WPARAM wParam)
{
    HDC  hdc     = (HDC)wParam;

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
        if (id == IDC_MONITOR_STATIC || id == IDC_DRIVE_BTN_BASE) {
            COLORREF clr = CLR_TEXT_DIM;
            if (id == IDC_MONITOR_STATIC && g_bMonitorAlert)
                clr = CLR_ORANGE;
            else if (id == IDC_MONITOR_STATIC && g_bMonitorOn)
                clr = CLR_GREEN;
            SetTextColor(hdc, clr);
            SetBkColor(hdc, CLR_SIDE);
            return (LRESULT)g_hbrSide;
        }
    }
    SetTextColor(hdc, CLR_TEXT);
    SetBkColor(hdc, CLR_BG);
    return (LRESULT)g_hbrBG;
}

/* ---- v2 layout geometry (unscaled px at 96 DPI) ---- */
#define V2_PAD        20
#define V2_TOP        16
#define V2_BTN_H      30
#define V2_DASH_Y     72
#define V2_ROW_H      26
static RECT g_rcTableCard;
static RECT g_rcHeaderText;
static HIMAGELIST g_hRowImgList = NULL;
static int g_nRowImgH = 0;

static void SetWindowThemeDyn(HWND h, const WCHAR* app)
{
    typedef HRESULT (WINAPI *PFN)(HWND, LPCWSTR, LPCWSTR);
    static PFN pSet = NULL;
    static int once = 0;
    if (!once) {
        HMODULE m = LoadLibraryW(L"uxtheme.dll");
        if (m) pSet = (PFN)(void*)GetProcAddress(m, "SetWindowTheme");
        once = 1;
    }
    if (pSet && h) pSet(h, app, NULL);
}

static void UiSyncThemeMenu(void)
{
    UINT id = IDM_THEME_SYSTEM;
    if (g_nThemePref == 1) id = IDM_THEME_LIGHT;
    else if (g_nThemePref == 2) id = IDM_THEME_DARK;
    if (g_hThemeMenu)
        CheckMenuRadioItem(g_hThemeMenu, IDM_THEME_SYSTEM, IDM_THEME_DARK, id, MF_BYCOMMAND);
}

static void UiApplyThemeAll(HWND hWnd)
{
    HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    UiApplyPalette();
    UiApplyTitleBar(hWnd);
    if (hList) {
        SubclassAttrHeader(hList);
        InvalidateRect(ListView_GetHeader(hList), NULL, FALSE);
        ListView_SetBkColor(hList, CLR_PANEL);
        ListView_SetTextBkColor(hList, CLR_PANEL);
        ListView_SetTextColor(hList, CLR_TEXT);
        SetWindowThemeDyn(hList, g_bUiDark ? L"DarkMode_Explorer" : L"Explorer");
    }
    UiSyncThemeMenu();
    RedrawWindow(hWnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

static void UiSetThemePref(HWND hWnd, int v)
{
    g_nThemePref = v;
    g_nThemeOverride = 0;
    SaveThemePref(v);
    UiApplyThemeAll(hWnd);
}

static int ButtonTextW(HWND hBtn, WCHAR* wz, int n)
{
    wz[0] = 0;
    return GetWindowTextW(hBtn, wz, n);
}

static int MeasureActionButton(HDC hdc, HWND hBtn)
{
    WCHAR wz[64];
    SIZE sz;
    int w;
    HFONT fo = (HFONT)SelectObject(hdc, g_hFontLabel);
    ButtonTextW(hBtn, wz, 64);
    GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);
    w = sz.cx + UiScale(24);
    if (GetDlgCtrlID(hBtn) == IDC_REREAD_BTN) {
        GetTextExtentPoint32W(hdc, L"F5", 2, &sz);
        w += sz.cx + UiScale(8);
    }
    SelectObject(hdc, fo);
    return w;
}

static void DrawActionButton(const DRAWITEMSTRUCT* di)
{
    HDC hdc = di->hDC;
    RECT r = di->rcItem;
    WCHAR wz[64];
    SIZE sz, szH = { 0, 0 };
    BOOL bDis = (di->itemState & ODS_DISABLED) != 0;
    BOOL bDown = (di->itemState & ODS_SELECTED) != 0;
    BOOL bHint = (di->CtlID == IDC_REREAD_BTN);
    int x;
    RECT rt;
    HFONT fo;
    FillRectColor(hdc, &r, CLR_BG);
    DrawCard(hdc, &r, UiScale(5), bDown ? g_Pal.hover : g_Pal.btn,
             bDis ? CLR_BORDER : CLR_BORDER2, FALSE, 0, 0);
    ButtonTextW(di->hwndItem, wz, 64);
    fo = (HFONT)SelectObject(hdc, g_hFontLabel);
    GetTextExtentPoint32W(hdc, wz, lstrlenW(wz), &sz);
    if (bHint) GetTextExtentPoint32W(hdc, L"F5", 2, &szH);
    x = r.left + (r.right - r.left - sz.cx - (bHint ? szH.cx + UiScale(8) : 0)) / 2;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, bDis ? CLR_TEXT_FAINT : CLR_TEXT);
    rt = r; rt.left = x; rt.right = x + sz.cx + 1;
    DrawTextW(hdc, wz, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    if (bHint) {
        rt.left = rt.right + UiScale(8) - 1; rt.right = rt.left + szH.cx + 1;
        SetTextColor(hdc, CLR_TEXT_FAINT);
        DrawTextW(hdc, L"F5", 2, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(hdc, fo);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT rf = r;
        InflateRect(&rf, -UiScale(3), -UiScale(3));
        SetTextColor(hdc, CLR_TEXT);
        SetBkColor(hdc, g_Pal.btn);
        DrawFocusRect(hdc, &rf);
    }
}

static void PaintMainV2(HWND hWnd, HDC hdc, const RECT* rcClient)
{
    const DRIVE_INFO* p = SelDrive();
    int sw = UiScale(DRIVE_BTN_PANEL_W);
    RECT rs = { 0, 0, sw, rcClient->bottom };
    FillRectColor(hdc, rcClient, CLR_BG);
    FillRectColor(hdc, &rs, CLR_SIDE);
    { RECT ln = { sw - 1, 0, sw, rcClient->bottom }; FillRectColor(hdc, &ln, CLR_BORDER); }
    {
        RECT rc = { UiScale(16), UiScale(14), sw - UiScale(16), UiScale(32) };
        WCHAR wz[64];
        HFONT fo = (HFONT)SelectObject(hdc, g_hFontPill);
        U8ToW(Tr(STR_DRIVES), wz, 64);
        CharUpperW(wz);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TEXT_FAINT);
        DrawTextW(hdc, wz, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, fo);
    }
    if (p) {
        RECT r1 = g_rcHeaderText, r2 = g_rcHeaderText;
        char sub[256], cap[32];
        const char* ctl = DriveControllerLabel(p);
        r1.bottom = r1.top + UiScale(28);
        r2.top = r1.bottom + UiScale(2); r2.bottom = r2.top + UiScale(18);
        DrawTextColor(hdc, p->szModel[0] ? p->szModel : "-", &r1, g_hFontH1, CLR_TEXT,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        FormatSize(p->dwCapacityMB, cap, sizeof(cap));
        safe_snprintf(sub, "%s \xC2\xB7 %s", cap, GetDriveTypeName(p->eType));
        if (p->szProtocol[0])
            safe_snprintf_n(sub + strlen(sub), (int)(sizeof(sub) - strlen(sub)), " \xC2\xB7 %s", p->szProtocol);
        if (ctl && ctl[0] && strcmp(ctl, "\xE2\x80\x94") != 0 && strcmp(ctl, "-") != 0)
            safe_snprintf_n(sub + strlen(sub), (int)(sizeof(sub) - strlen(sub)), " \xC2\xB7 %s", ctl);
        if (p->szSerial[0])
            safe_snprintf_n(sub + strlen(sub), (int)(sizeof(sub) - strlen(sub)), " \xC2\xB7 S/N %s", p->szSerial);
        if (p->szFirmware[0])
            safe_snprintf_n(sub + strlen(sub), (int)(sizeof(sub) - strlen(sub)), " \xC2\xB7 %s %s",
                          TN("ПО", "FW"), p->szFirmware);
        DrawTextColor(hdc, sub, &r2, g_hFontLabel, CLR_TEXT_DIM,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (g_rcTableCard.right > g_rcTableCard.left)
        DrawCard(hdc, &g_rcTableCard, UiScale(8), CLR_PANEL, CLR_BORDER, FALSE, 0, 0);
    (void)hWnd;
}

static void UiEnsureRowHeight(HWND hList)
{
    int h = UiScale(V2_ROW_H);
    if (!hList || h == g_nRowImgH) return;
    if (g_hRowImgList) {
        ListView_SetImageList(hList, NULL, LVSIL_SMALL);
        ImageList_Destroy(g_hRowImgList);
    }
    g_hRowImgList = ImageList_Create(1, h, ILC_COLOR32, 1, 1);
    ListView_SetImageList(hList, g_hRowImgList, LVSIL_SMALL);
    g_nRowImgH = h;
}

static void LayoutAttrColumns(HWND hList)
{
    RECT rc;
    int fixed, rest, paramW, i;
    /* Decode is fixed so «Расшифровка» fits. The parameter name takes
     * the width that is left. Do not grow past the list: that adds a
     * horizontal scrollbar. */
    int cw[8] = { 76, 0, 100, 80, 70, 116, 150, 136 };
    GetClientRect(hList, &rc);
    fixed = 0;
    for (i = 0; i < 8; i++) if (cw[i]) fixed += UiScale(cw[i]);
    rest = (rc.right - rc.left) - fixed;
    if (rest < UiScale(96)) rest = UiScale(96);
    paramW = rest;
    for (i = 0; i < 8; i++)
        ListView_SetColumnWidth(hList, i, (i == 1) ? paramW : UiScale(cw[i]));
}

static void LayoutMainWindow(HWND hWnd)
{
    RECT rc;
    int cx, cy, i, sw, x0, xr, bx;
    HWND hList;
    HDC hdc;

    GetClientRect(hWnd, &rc);
    cx = rc.right - rc.left;
    cy = rc.bottom - rc.top;
    if (cx < 100 || cy < 100) return;

    sw = UiScale(DRIVE_BTN_PANEL_W);
    for (i = 0; i < MAX_DRIVES; i++) {
        if (g_hDriveBtn[i]) {
            int nY = UiScale(40) + i * (UiScale(DRIVE_BTN_H) + UiScale(DRIVE_BTN_GAP));
            SetWindowPos(g_hDriveBtn[i], NULL, UiScale(12), nY,
                         sw - UiScale(24), UiScale(DRIVE_BTN_H), SWP_NOZORDER);
        }
    }
    {
        HWND hMon = GetDlgItem(hWnd, IDC_MONITOR_STATIC);
        if (hMon) SetWindowPos(hMon, NULL, UiScale(16), cy - UiScale(30),
                               sw - UiScale(32), UiScale(18), SWP_NOZORDER);
    }

    x0 = sw + UiScale(V2_PAD);
    xr = cx - UiScale(V2_PAD);

    /* Action buttons, right-aligned on the header row. */
    bx = xr;
    hdc = GetDC(hWnd);
    {
        int ids[3] = { IDC_EJECT_BTN, IDC_REPORT_BTN, IDC_REREAD_BTN };
        for (i = 0; i < 3; i++) {
            HWND hb = GetDlgItem(hWnd, ids[i]);
            int w;
            if (!hb) continue;
            w = MeasureActionButton(hdc, hb);
            bx -= w;
            SetWindowPos(hb, NULL, bx, UiScale(V2_TOP), w, UiScale(V2_BTN_H), SWP_NOZORDER);
            bx -= UiScale(6);
        }
    }
    ReleaseDC(hWnd, hdc);
    g_rcHeaderText.left = x0; g_rcHeaderText.top = UiScale(V2_TOP) - UiScale(2);
    g_rcHeaderText.right = bx - UiScale(12); g_rcHeaderText.bottom = UiScale(V2_DASH_Y);

    if (g_hHealthBar)
        SetWindowPos(g_hHealthBar, NULL, x0, UiScale(V2_DASH_Y), xr - x0,
                     UiScale(DASH_H), SWP_NOZORDER);

    g_rcTableCard.left = x0;
    g_rcTableCard.right = xr;
    g_rcTableCard.top = UiScale(V2_DASH_Y) + UiScale(DASH_H) + UiScale(12);
    g_rcTableCard.bottom = cy - UiScale(16);
    if (g_rcTableCard.bottom < g_rcTableCard.top + UiScale(80))
        g_rcTableCard.bottom = g_rcTableCard.top + UiScale(80);

    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        int in = UiScale(3);
        UiEnsureRowHeight(hList);
        SetWindowPos(hList, NULL, g_rcTableCard.left + in, g_rcTableCard.top + in,
                     g_rcTableCard.right - g_rcTableCard.left - 2 * in,
                     g_rcTableCard.bottom - g_rcTableCard.top - 2 * in, SWP_NOZORDER);
        LayoutAttrColumns(hList);
    }
    InvalidateRect(hWnd, NULL, FALSE);
}

/* ---- SMART table custom draw (v2) ---- */
static PILL_KIND AttrPillKind(int nSt)
{
    switch (nSt) {
    case ATTRST_BAD:  return PILL_BAD;
    case ATTRST_WARN: return PILL_WARN;
    case ATTRST_RISK: return PILL_RISK;
    case ATTRST_OK:   return PILL_OK;
    case ATTRST_SKIP: return PILL_SKIP;
    case ATTRST_DIM:
    case ATTRST_PAST:
    case ATTRST_INFO:
    case ATTRST_POWERLOG: return PILL_NEUTRAL;
    default: return PILL_NONE;
    }
}

static LRESULT AttrListCustomDraw(NMLVCUSTOMDRAW* pCD)
{
    HWND hList = pCD->nmcd.hdr.hwndFrom;
    switch (pCD->nmcd.dwDrawStage) {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        pCD->nmcd.uItemState &= ~(UINT)CDIS_FOCUS;
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_SUBITEM | CDDS_ITEMPREPAINT:
        {
            HDC hdc = pCD->nmcd.hdc;
            int item = (int)pCD->nmcd.dwItemSpec;
            int sub = pCD->iSubItem;
            int nSt = (int)(pCD->nmcd.lItemlParam & 0xFF);
            int nAlt = (int)(pCD->nmcd.lItemlParam & 0x100);
            BOOL bHl = (nSt == ATTRST_BAD || nSt == ATTRST_WARN || nSt == ATTRST_RISK);
            BOOL bSel = (ListView_GetItemState(hList, item, LVIS_SELECTED) & LVIS_SELECTED) != 0;
            RECT rc;
            COLORREF bg;
            WCHAR wz[128];
            LVITEMW li;
            int pad = UiScale(12);

            ListView_GetSubItemRect(hList, item, sub, LVIR_BOUNDS, &rc);
            if (sub == 0) {
                RECT r1;
                ListView_GetSubItemRect(hList, item, 1, LVIR_BOUNDS, &r1);
                rc.right = r1.left;
            }
            if (bSel) bg = g_Pal.rowSel;
            else if (bHl) bg = g_Pal.rowHl;
            else bg = (item % 2) ? CLR_ROW2 : CLR_ROW1;
            FillRectColor(hdc, &rc, bg);
            { RECT ln = { rc.left, rc.bottom - 1, rc.right, rc.bottom }; FillRectColor(hdc, &ln, CLR_BORDER); }
            if (sub == 0 && bHl) {
                RECT bar = { rc.left, rc.top, rc.left + UiScale(3), rc.bottom - 1 };
                PILL_KIND k = AttrPillKind(nSt);
                COLORREF b2, f2;
                PillColors(k, &b2, &f2);
                FillRectColor(hdc, &bar, f2);
            }

            ZeroMemory(&li, sizeof(li));
            li.iSubItem = sub;
            li.pszText = wz;
            li.cchTextMax = 128;
            wz[0] = 0;
            SendMessageW(hList, LVM_GETITEMTEXTW, (WPARAM)item, (LPARAM)&li);

            if (sub == ATTR_COL_STATUS) {
                PILL_KIND k = AttrPillKind(nSt);
                const char* psz = NULL;
                WCHAR wl[64];
                if (k == PILL_NONE) return CDRF_SKIPDEFAULT;
                switch (nSt) {
                case ATTRST_BAD:  psz = nAlt ? Tr(STR_ST_BAD) : Tr(STR_ST_FAIL); break;
                case ATTRST_WARN: psz = nAlt ? Tr(STR_ST_HOT) : Tr(STR_ST_WARN); break;
                case ATTRST_RISK: psz = Tr(STR_ST_RISK); break;
                case ATTRST_OK:   psz = Tr(STR_ST_OK); break;
                case ATTRST_DIM:  psz = Tr(STR_ST_DIM); break;
                case ATTRST_SKIP: psz = Tr(STR_ST_SKIP); break;
                case ATTRST_PAST: psz = Tr(STR_ST_PAST); break;
                default: psz = NULL; break;   /* INFO / POWERLOG: list text */
                }
                if (psz) U8ToW(psz, wl, 64);
                else lstrcpynW(wl, wz[0] ? wz : L"", 64);
                if (k == PILL_NEUTRAL || k == PILL_SKIP) CharLowerW(wl);
                else CharUpperW(wl);
                if (wl[0])
                    DrawPillW(hdc, rc.left + pad, (rc.top + rc.bottom - 1) / 2, wl, k, FALSE);
                return CDRF_SKIPDEFAULT;
            }
            if (sub == 0) {
                /* "5 (05h)" -> decimal primary, faint hex if it fits. */
                WCHAR prim[96], sec[64];
                WCHAR* op = wcsstr(wz, L" (");
                RECT rt = rc;
                SIZE szP, szS;
                HFONT fo;
                int room;
                prim[0] = sec[0] = 0;
                if (op) {
                    size_t n = (size_t)(op - wz);
                    WCHAR* cl;
                    lstrcpynW(prim, wz, (int)n + 1);
                    lstrcpynW(sec, op + 2, 64);
                    cl = wcsrchr(sec, L')');
                    if (cl && cl[1] == 0) *cl = 0;
                } else {
                    lstrcpynW(prim, wz, 96);
                }
                rt.left += pad; rt.right -= pad; rt.bottom -= 1;
                SetBkMode(hdc, TRANSPARENT);
                fo = (HFONT)SelectObject(hdc, g_hFontBody);
                GetTextExtentPoint32W(hdc, prim, lstrlenW(prim), &szP);
                SetTextColor(hdc, CLR_TEXT_DIM);
                DrawTextW(hdc, prim, -1, &rt, DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                          DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(hdc, g_hFontLabel);
                GetTextExtentPoint32W(hdc, sec, lstrlenW(sec), &szS);
                room = (rt.right - rt.left) - szP.cx - UiScale(8);
                if (sec[0] && szS.cx <= room) {
                    RECT r2 = rt;
                    SetTextColor(hdc, CLR_TEXT_FAINT);
                    r2.left = rt.left + szP.cx + UiScale(6);
                    DrawTextW(hdc, sec, -1, &r2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
                SelectObject(hdc, fo);
                return CDRF_SKIPDEFAULT;
            }
            {
                RECT rt = rc;
                HFONT fo = (HFONT)SelectObject(hdc,
                    (bHl && (sub == 1 || sub == ATTR_COL_DECODE)) ? g_hFontBodySb : g_hFontBody);
                UINT fmt = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX;
                rt.left += pad; rt.right -= pad; rt.bottom -= 1;
                fmt |= (sub >= 2 && sub <= ATTR_COL_RAW) ? DT_RIGHT : DT_LEFT;
                SetBkMode(hdc, TRANSPARENT);
                SetTextColor(hdc, sub == 0 ? CLR_TEXT_DIM : CLR_TEXT);
                DrawTextW(hdc, wz, -1, &rt, fmt);
                SelectObject(hdc, fo);
            }
            return CDRF_SKIPDEFAULT;
        }
    default:
        return CDRF_DODEFAULT;
    }
}

/* The header is painted here in full (double-buffered) instead of through
 * NM_CUSTOMDRAW: themed headers repaint their own item background on some
 * builds and Wine, which broke the dark palette. */
static WNDPROC g_pfnHeaderProc = NULL;

static void PaintAttrHeader(HWND hHdr, HDC hdc, const RECT* rc)
{
    int n = Header_GetItemCount(hHdr), i;
    FillRectColor(hdc, rc, CLR_PANEL);
    for (i = 0; i < n; i++) {
        RECT ri, rt;
        WCHAR wz[64];
        HDITEMW hi;
        HFONT fo;
        int order = i;
        if (!Header_GetItemRect(hHdr, order, &ri)) continue;
        ZeroMemory(&hi, sizeof(hi));
        hi.mask = HDI_TEXT;
        hi.pszText = wz;
        hi.cchTextMax = 64;
        wz[0] = 0;
        SendMessageW(hHdr, HDM_GETITEMW, (WPARAM)order, (LPARAM)&hi);
        rt = ri;
        rt.left += UiScale(12); rt.right -= UiScale(12); rt.bottom -= 1;
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, CLR_TEXT_DIM);
        fo = (HFONT)SelectObject(hdc, g_hFontLabelSb);
        DrawTextW(hdc, wz, -1, &rt, ((i >= 2 && i <= ATTR_COL_RAW) ? DT_RIGHT : DT_LEFT) |
                  DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(hdc, fo);
    }
    { RECT ln = { rc->left, rc->bottom - 1, rc->right, rc->bottom }; FillRectColor(hdc, &ln, CLR_BORDER); }
}

static LRESULT CALLBACK AttrHeaderProc(HWND hHdr, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ERASEBKGND)
        return 1;
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdcReal = BeginPaint(hHdr, &ps);
        RECT rc;
        GetClientRect(hHdr, &rc);
        if (rc.right > 0 && rc.bottom > 0) {
            HDC hdc = CreateCompatibleDC(hdcReal);
            HBITMAP hbm = CreateCompatibleBitmap(hdcReal, rc.right, rc.bottom);
            HBITMAP hbo = (HBITMAP)SelectObject(hdc, hbm);
            PaintAttrHeader(hHdr, hdc, &rc);
            BitBlt(hdcReal, 0, 0, rc.right, rc.bottom, hdc, 0, 0, SRCCOPY);
            SelectObject(hdc, hbo);
            DeleteObject(hbm);
            DeleteDC(hdc);
        }
        EndPaint(hHdr, &ps);
        return 0;
    }
    return CallWindowProcW(g_pfnHeaderProc, hHdr, msg, wParam, lParam);
}

static void SubclassAttrHeader(HWND hList)
{
    HWND hHdr = hList ? ListView_GetHeader(hList) : NULL;
    if (!hHdr || g_pfnHeaderProc) return;
    g_pfnHeaderProc = (WNDPROC)SetWindowLongPtrW(hHdr, GWLP_WNDPROC, (LONG_PTR)AttrHeaderProc);
}

/* ---- --demo fixtures: three drives, no device I/O. Real engine verdicts. ---- */
static void DemoAttr(DRIVE_INFO* g, BYTE id, BYTE val, BYTE worst, BYTE thr, unsigned long long raw)
{
    int i, b;
    for (i = 0; i < 30; i++) {
        SMART_ATTRIBUTE* a = &g->attrData.stAttributes[i];
        if (a->bAttrID != 0) continue;
        a->bAttrID = id;
        a->wStatusFlags = (WORD)(thr ? 0x0033 : 0x0032);
        a->bAttrValue = val;
        a->bWorstValue = worst;
        for (b = 0; b < 6; b++) { a->bRawValue[b] = (BYTE)(raw & 0xFFu); raw >>= 8; }
        g->threshData.stThresholds[i].bAttrID = id;
        g->threshData.stThresholds[i].bThresholdValue = thr;
        return;
    }
}

static void DemoBase(DRIVE_INFO* g, int idx, DRIVE_TYPE t, const char* model, const char* sn,
                     const char* fw, DWORD mb, const char* proto, int tempC, DWORD poh, DWORD pc)
{
    memset(g, 0, sizeof(*g));
    g->nDriveIndex = idx;
    g->eType = t;
    g->bSMART_Supported = TRUE;
    g->bSMART_Enabled = TRUE;
    g->bGotReturnStatus = TRUE;
    g->nEndurancePercent = -1;
    g->nSSDLifeLeft = -1;
    g->nSSDTotalWritesGB = -1;
    g->nSSDAvgEraseCount = -1;
    g->nHealthPercent = -1;
    g->nTempMaxC = -1; g->nTempMinC = -1; g->nTempWarnC = -1; g->nTempCritC = -1;
    lstrcpynA(g->szModel, model, (int)sizeof(g->szModel));
    lstrcpynA(g->szSerial, sn, (int)sizeof(g->szSerial));
    lstrcpynA(g->szFirmware, fw, (int)sizeof(g->szFirmware));
    lstrcpynA(g->szProtocol, proto, (int)sizeof(g->szProtocol));
    g->dwCapacityMB = mb;
    g->nTemperatureC = tempC;
    g->dwPowerOnHours = poh;
    g->dwPowerCycleCount = pc;
    g->attrData.wRevisionNumber = 0x10;
}

static int DemoFill(DRIVE_INFO* d)
{
    DRIVE_INFO* g;
    const char* sata = UiLangIsEn() ? "SATA III (6 Gb/s)" : "SATA III (6 \xD0\x93\xD0\xB1\xD0\xB8\xD1\x82/\xD1\x81)";

    g = &d[0];
    DemoBase(g, 0, DRIVE_TYPE_SSD_SATA, "Samsung SSD 870 EVO 1TB", "S6PTNM0T812345K", "SVT02B6Q",
             953869, sata, 38, 9120, 1204);
    DemoAttr(g, 5, 100, 100, 10, 0);
    DemoAttr(g, 9, 98, 98, 0, 9120);
    DemoAttr(g, 12, 99, 99, 0, 1204);
    DemoAttr(g, 177, 97, 97, 0, 31);
    DemoAttr(g, 179, 100, 100, 10, 0);
    DemoAttr(g, 181, 100, 100, 10, 0);
    DemoAttr(g, 182, 100, 100, 10, 0);
    DemoAttr(g, 183, 100, 100, 10, 0);
    DemoAttr(g, 187, 100, 100, 0, 0);
    DemoAttr(g, 190, 62, 48, 0, 38);
    DemoAttr(g, 194, 62, 48, 0, 38);
    DemoAttr(g, 199, 100, 100, 0, 0);
    DemoAttr(g, 235, 99, 99, 0, 41);
    DemoAttr(g, 241, 99, 99, 0, 21474836480ull);

    g = &d[1];
    DemoBase(g, 1, DRIVE_TYPE_HDD, "WDC WD20EZBX-00AYRA0", "WD-WX12A80K3P7L", "01.01A01",
             1907729, sata, 41, 17170, 3412);
    DemoAttr(g, 1, 200, 200, 51, 0);
    DemoAttr(g, 3, 171, 169, 21, 4425);
    DemoAttr(g, 4, 97, 97, 0, 3488);
    DemoAttr(g, 5, 199, 199, 140, 8);
    DemoAttr(g, 7, 200, 200, 0, 0);
    DemoAttr(g, 9, 77, 77, 0, 17170);
    DemoAttr(g, 10, 100, 100, 0, 0);
    DemoAttr(g, 11, 100, 100, 0, 0);
    DemoAttr(g, 12, 97, 97, 0, 3412);
    DemoAttr(g, 192, 200, 200, 0, 87);
    DemoAttr(g, 193, 194, 194, 0, 18940);
    DemoAttr(g, 194, 106, 97, 0, 41);
    DemoAttr(g, 196, 200, 200, 0, 8);
    DemoAttr(g, 197, 200, 200, 0, 0);
    DemoAttr(g, 198, 100, 253, 0, 0);
    DemoAttr(g, 199, 200, 200, 0, 0);
    DemoAttr(g, 200, 100, 253, 0, 0);

    g = &d[2];
    DemoBase(g, 2, DRIVE_TYPE_HDD, "ST4000LM024-2AN17V", "WCK1A2B3", "0001",
             3815447, UiLangIsEn() ? "USB SAT \xC2\xB7 SATA 6 Gb/s"
                                   : "USB SAT \xC2\xB7 SATA 6 \xD0\x93\xD0\xB1\xD0\xB8\xD1\x82/\xD1\x81",
             47, 31200, 2210);
    g->bIsUSB = TRUE;
    g->eUsbBridgeType = USB_BRIDGE_SAT;
    g->wUsbVid = 0x0BC2; g->wUsbPid = 0x2343;
    DemoAttr(g, 1, 79, 64, 6, 81240512);
    DemoAttr(g, 3, 99, 99, 0, 0);
    DemoAttr(g, 4, 98, 98, 20, 2410);
    DemoAttr(g, 5, 92, 92, 36, 1384);
    DemoAttr(g, 7, 82, 60, 30, 175938620);
    DemoAttr(g, 9, 65, 65, 0, 31200);
    DemoAttr(g, 10, 100, 100, 97, 0);
    DemoAttr(g, 12, 98, 98, 20, 2210);
    DemoAttr(g, 187, 1, 1, 0, 214);
    DemoAttr(g, 188, 100, 99, 0, 3);
    DemoAttr(g, 190, 53, 41, 40, 47);
    DemoAttr(g, 194, 47, 59, 0, 47);
    DemoAttr(g, 197, 99, 99, 0, 56);
    DemoAttr(g, 198, 99, 99, 0, 56);
    DemoAttr(g, 199, 200, 200, 0, 0);

    {
        int i;
        for (i = 0; i < 3; i++) {
            int t = d[i].nTemperatureC;
            ExtractTemperatureFromATA(&d[i]);
            if (d[i].nTemperatureC <= 0) d[i].nTemperatureC = t;
            ExtractCommonATACounters(&d[i]);
            ExtractSSDIndicators(&d[i]);
            IdentifyDriveParts(&d[i]);
            AssessDriveHealth(&d[i]);
        }
    }
    return 3;
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
        UiApplyFonts(hWnd);
        UiApplyThemeAll(hWnd);
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
        return 1;   /* WM_PAINT paints everything through a back buffer */

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
            HWND hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
            HWND hHdr  = hList ? ListView_GetHeader(hList) : NULL;
            if (hHdr && pHdr->hwndFrom == hHdr && pHdr->code == NM_CUSTOMDRAW)
                return CDRF_DODEFAULT;   /* painted by AttrHeaderProc */
            if (pHdr->idFrom == IDC_ATTR_LIST && pHdr->code == NM_CUSTOMDRAW)
                return AttrListCustomDraw((NMLVCUSTOMDRAW*)lParam);
        }
        return 0;

    case WM_PAINT:
        {
            PAINTSTRUCT ps;
            HDC hdcReal = BeginPaint(hWnd, &ps);
            RECT rc;
            GetClientRect(hWnd, &rc);
            if (rc.right > 0 && rc.bottom > 0) {
                HDC hdc = CreateCompatibleDC(hdcReal);
                HBITMAP hbm = CreateCompatibleBitmap(hdcReal, rc.right, rc.bottom);
                HBITMAP hbo = (HBITMAP)SelectObject(hdc, hbm);
                PaintMainV2(hWnd, hdc, &rc);
                BitBlt(hdcReal, ps.rcPaint.left, ps.rcPaint.top,
                       ps.rcPaint.right - ps.rcPaint.left, ps.rcPaint.bottom - ps.rcPaint.top,
                       hdc, ps.rcPaint.left, ps.rcPaint.top, SRCCOPY);
                SelectObject(hdc, hbo);
                DeleteObject(hbm);
                DeleteDC(hdc);
            }
            EndPaint(hWnd, &ps);
        }
        return 0;

    case WM_DRAWITEM:
        {
            const DRAWITEMSTRUCT* di = (const DRAWITEMSTRUCT*)lParam;
            if (di && di->CtlType == ODT_BUTTON) {
                DrawActionButton(di);
                return TRUE;
            }
        }
        break;

    case WM_SETTINGCHANGE:
        if (lParam && lstrcmpiW((LPCWSTR)lParam, L"ImmersiveColorSet") == 0 &&
            g_nThemePref == 0 && g_nThemeOverride == 0)
            UiApplyThemeAll(hWnd);
        break;

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
            else if (nCtrl == IDM_THEME_SYSTEM) {
                UiSetThemePref(hWnd, 0);
            }
            else if (nCtrl == IDM_THEME_LIGHT) {
                UiSetThemePref(hWnd, 1);
            }
            else if (nCtrl == IDM_THEME_DARK) {
                UiSetThemePref(hWnd, 2);
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
