/* DriveMonitor - main window. MIT: see LICENSE. */

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

/* ATA SSD: nSSDTotalWritesGB is vendor RAW, typically GiB (attr E9/F9). */
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

    if (chip && p->wUsbVid && p->wUsbPid)
        safe_snprintf_n(szBuf, nBufLen, "%s (%04X:%04X)", chip,
                  (unsigned)p->wUsbVid, (unsigned)p->wUsbPid);
    else if (chip)
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
#define HOTPLUG_DELAY_MS  1200

/* Note: the previous WinRAR-style nag timer state variables
   (g_nagSecondsLeft, g_bNagPending) have been removed because the
   program is 100% free and open source
   reminder to show anymore. */

static void UpdateWindowTitle(HWND hWnd)
{
    SetWindowTextU8(hWnd, "DriveMonitor");
}

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
    safe_snprintf(szLine, TN("**Версия:** %s  \r\n", "**Version:** %s  \r\n"),
                  DRIVEMONITOR_VERSION);
    ReportCat(buf, kCap, &len, szLine);
    safe_snprintf(szLine, TN("**Сборка:** %s  \r\n", "**Build:** %s  \r\n"),
                  DRIVEMONITOR_BUILD_STR);
    ReportCat(buf, kCap, &len, szLine);
    safe_snprintf(szLine, "**Дата:** %04d-%02d-%02d %02d:%02d:%02d\r\n\r\n",
                  (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                  (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    ReportCat(buf, kCap, &len, szLine);

    ReportCat(buf, kCap, &len, Tr(STR_REPORT_DRIVE));
    safe_snprintf(szLine, "| %s | %s |\r\n| --- | --- |\r\n",
                  Tr(STR_REPORT_FIELD), Tr(STR_REPORT_VALUE));
    ReportCat(buf, kCap, &len, szLine);
    ReportMdKv(buf, kCap, &len, Tr(STR_MODEL), ReportDash(pInfo->szModel));
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
        ReportCat(buf, kCap, &len, TN("_Таблица SMART пуста._\r\n", "_SMART table is empty._\r\n"));
    } else {
        int r, c;
        const char* headers[7] = {
            "ID",
            Tr(STR_COL_PARAM),
            Tr(STR_COL_VALUE),
            Tr(STR_COL_WORST),
            Tr(STR_COL_THRESH),
            "RAW",
            Tr(STR_COL_STATUS)
        };
        ReportCat(buf, kCap, &len, "| ");
        for (c = 0; c < 7; c++) {
            ReportCat(buf, kCap, &len, headers[c]);
            ReportCat(buf, kCap, &len, (c < 6) ? " | " : " |\r\n");
        }
        ReportCat(buf, kCap, &len,
                  "| --- | --- | --- | --- | --- | --- | --- |\r\n");
        for (r = 0; r < nItems; r++) {
            ReportCat(buf, kCap, &len, "| ");
            for (c = 0; c < 7; c++) {
                WCHAR wcell[256];
                char ucell[512];
                wcell[0] = 0;
                ListView_GetItemText(hList, r, c, wcell, 256);
                WToU8(wcell, ucell, 512);
                ReportMdCell(buf, kCap, &len, ucell);
                ReportCat(buf, kCap, &len, (c < 6) ? " | " : " |\r\n");
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

static void Snapshot_Diff(void)
{

    (void)g_nPrevCount;
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
        safe_snprintf(szBuf, "%s   Тип: %s",
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
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s  макс.%d  крит.%d",
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempMaxC, pInfo->nTempCritC);
        else if (pInfo->nTempWarnC > 0 && pInfo->nTempCritC > 0)
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s  пред.%d  крит.%d",
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempWarnC, pInfo->nTempCritC);
        else if (pInfo->nTempCritC > 0)
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s  крит.%d",
                          pInfo->nTemperatureC, szBand[0] ? szBand : "—",
                          pInfo->nTempCritC);
        else if (pInfo->nTempMaxC > 0)
            safe_snprintf(szBuf, "%d\xC2\xB0""C · %s  макс.%d",
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

    if (!pInfo->bSMART_Supported) {
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

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
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
                szPred = TN("Есть признаки деградации. Нажмите на состояние.",
                            "There are signs of degradation. Click the status for details.");
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
            SetDlgItemTextU8(hWnd, IDC_AXIS_ROW4_L, Tr(STR_WEAR));
            if (pInfo->eWear != HEALTH_STATUS_UNKNOWN && pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%s (%d%%)",
                    GetHealthStatusName(pInfo->eWear), pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else if (pInfo->eWear != HEALTH_STATUS_UNKNOWN) {
                szRow4Val = GetHealthStatusName(pInfo->eWear);
            } else if (pInfo->nEndurancePercent >= 0) {
                safe_snprintf(szRow4Buf, "%d%%", pInfo->nEndurancePercent);
                szRow4Val = szRow4Buf;
            } else {
                szRow4Val = "нет данных";
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
    char col[7][128];
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
                       const char* raw, const char* stat)
{
    FormatIdCol(r->col[0], (int)sizeof(r->col[0]), id);
    safe_snprintf(r->col[1], "%s", name   ? name   : "");
    safe_snprintf(r->col[2], "%s", val    ? val    : "");
    safe_snprintf(r->col[3], "%s", worst  ? worst  : "");
    safe_snprintf(r->col[4], "%s", thresh ? thresh : "");
    safe_snprintf(r->col[5], "%s", raw    ? raw    : "");
    safe_snprintf(r->col[6], "%s", TrStatus(stat ? stat : ""));
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
    /* TODO: prefer ATTRST_* end-to-end; see https://github.com/chuikoff/DriveMonitor/issues/3 */
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

static BOOL IsAtaSsdType(DRIVE_TYPE t)
{
    return t == DRIVE_TYPE_SSD_SATA || t == DRIVE_TYPE_M2_SATA;
}

static BOOL VendorUsesE7AsLife(DRIVE_CONTROLLER c, DRIVE_TYPE t)
{
    (void)t;
    /* Only Phison is known-sure: E7 is SSD life, not temperature.
     * SMI is left alone — E7 meaning varies by SM225/SM226 firmware. */
    return c == CONTROLLER_PHISON;
}

static void FormatSmartValue(BYTE bID, BYTE* pRaw,
                              BYTE bVal, BYTE bWorst, BYTE bThresh,
                              const DRIVE_INFO* pDrv,
                              char* szBuf, int nBufLen)
{
    DRIVE_VENDOR eVendor = pDrv ? pDrv->eVendor : VENDOR_UNKNOWN;
    DRIVE_TYPE eType = pDrv ? pDrv->eType : DRIVE_TYPE_UNKNOWN;
    DRIVE_CONTROLLER eCtl = pDrv ? pDrv->eController : CONTROLLER_UNKNOWN;

    DWORD dw32 = GetRawValue(pRaw);
    WORD  w16  = (WORD)(dw32 & 0xFFFFu);
    unsigned __int64 qw48 = GetRawValue48(pRaw);

    (void)bWorst;
    (void)bThresh;
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
            if (pDrv && pDrv->eType == DRIVE_TYPE_HDD &&
                (bID == 0xB5 || bID == 0xB6 || bID == 0xAB || bID == 0xAC)) {
                unsigned hi = (unsigned)((dw32 >> 16) & 0xFFFFu);
                unsigned lo = (unsigned)(dw32 & 0xFFFFu);
                safe_snprintf(szMain, "%u / %u", hi, lo);
            } else if (qw48 > 0xFFFFFFFFULL)
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
    {
        int nC = (int)pRaw[0];
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax < 100)
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)  мин:%d макс:%d", nC, nF, nMin, nMax);
        else
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", nC, nF);
        break;
    }
    case 0xC2:
    {
        int nC = (int)pRaw[0];
        if (nC <= 0 || nC > 125) nC = (int)w16;
        if (nC <= 0 || nC > 125) nC = (int)bVal;
        int nF = nC * 9 / 5 + 32;
        int nMin = (int)pRaw[2], nMax = (int)pRaw[4];
        if (nMin > 0 && nMax > nMin && nMax <= 125)
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)  мин:%d макс:%d", nC, nF, nMin, nMax);
        else
            safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", nC, nF);
        break;
    }
    case 0xE7:
    {
        int nLife = -1;
        if (bVal <= 100)
            nLife = (int)bVal;
        else if (dw32 <= 100)
            nLife = (int)dw32;
        else if (w16 <= 100)
            nLife = (int)w16;

        if (VendorUsesE7AsLife(eCtl, eType)) {
            if (nLife >= 0)
                safe_snprintf(szMain, TN("%d%% остаток ресурса", "%d%% life remaining"), nLife);
            else
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            break;
        }
        if (eCtl == CONTROLLER_UNKNOWN &&
            IsAtaSsdType(eType)) {
            if (nLife >= 0 && nLife <= 100)
                safe_snprintf(szMain, TN("%d%% остаток ресурса", "%d%% life remaining"), nLife);
            else if (bVal > 0 && bVal <= 100) {
                int nF = (int)bVal * 9 / 5 + 32;
                safe_snprintf(szMain, "%d \xC2\xB0""C (%d \xC2\xB0""F)", (int)bVal, nF);
            } else {
                safe_snprintf(szMain, "%lu", (unsigned long)dw32);
            }
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
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu повторов  (!)", "%lu retries  (!)"), (unsigned long)dw32);
        break;

    case 0x05:
    {
        DWORD dwSec = dw32 & 0xFFFF;
        if (dwSec == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu секторов  (!)", "%lu sectors  (!)"), (unsigned long)dwSec);
        break;
    }

    case 0xC4:
        if (DecodeRemapEvents(pDrv, pRaw) < 0) {
            if (RemapRawIsFlyingHours(pDrv, pRaw))
                safe_snprintf(szMain, TN(
                    "%lu ч полёта (как 240), не события",
                    "%lu flying hours (same as 240), not events"),
                    (unsigned long)dw32);
            else
                safe_snprintf(szMain, "%s", TN("не счётчик событий", "not an event count"));
        } else if (dw32 == 0)
            safe_snprintf(szMain, "0 событий  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu событий  (!)", "%lu events  (!)"), (unsigned long)dw32);
        break;

    case 0xC5:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu нестабильных  (!)", "%lu pending  (!)"), (unsigned long)dw32);
        break;

    case 0xC6:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu неисправимых  (!)", "%lu uncorrectable  (!)"), (unsigned long)dw32);
        break;

    case 0xC7:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu ошибок CRC  (!)", "%lu CRC errors  (!)"), (unsigned long)dw32);
        break;

    case 0xBB:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%lu неисправимых", "%lu uncorrectable"), (unsigned long)dw32);
        break;

    case 0xC3:
        if (eType == DRIVE_TYPE_HDD && eVendor == VENDOR_SEAGATE) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, "%u ош. чтения / %lu секторов", nErr, (unsigned long)nOps);
            break;
        }
        if (eType == DRIVE_TYPE_HDD) {
            if (dw32 == 0)
                safe_snprintf(szMain, "0  (ОК)");
            else
                safe_snprintf(szMain, TN("%lu восстановлений ECC", "%lu ECC recovered"), (unsigned long)dw32);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu  (vendor-specific)",
                          (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu  (vendor-specific)",
                          (unsigned long)dw32);
        break;

    case 0xBC:
    {
        /* Seagate: total, completions over 5 s, completions over 7.5 s. */
        WORD wTotal = (WORD)pRaw[0] | ((WORD)pRaw[1] << 8);
        WORD wOver5 = (WORD)pRaw[2] | ((WORD)pRaw[3] << 8);
        WORD wOver75 = (WORD)pRaw[4] | ((WORD)pRaw[5] << 8);
        if (wTotal == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, TN("%u таймаутов  (>%u за 5 с, >%u за 7.5 с)",
                                     "%u timeouts (>%u over 5 s, >%u over 7.5 s)"),
                          wTotal, wOver5, wOver75);
        break;
    }

    case 0xBD:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
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
            safe_snprintf(szMain, TN("%lu аварийных парковок", "%lu emergency retracts"), (unsigned long)dw32);
        break;

    case 0xB7:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu понижений  (!)", (unsigned long)dw32);
        break;

    case 0xB8:
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
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
            safe_snprintf(szMain, "0  (ОК)");
        else
            safe_snprintf(szMain, "%lu  (значение важнее RAW)", (unsigned long)dw32);
        break;

    case 0x01:
    {
        if (eVendor == VENDOR_SEAGATE) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, "%u ошибок чтения / %lu секторов", nErr, (unsigned long)nOps);
            break;
        }
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
        else if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;
    }

    case 0x07:
    {
        if (eVendor == VENDOR_SEAGATE) {
            unsigned nErr = SeagateRateErrs(pRaw);
            DWORD nOps = SeagateRateOps(pRaw);
            safe_snprintf(szMain, "%u ошибок позиц. / %lu seeks", nErr, (unsigned long)nOps);
            break;
        }
        if (dw32 == 0)
            safe_snprintf(szMain, "0  (ОК)");
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
        /* Phison 241/242: GB or 32 MiB units. Other SATA SSDs are LBAs. */
        if (pDrv && IsPhisonFamily(pDrv) &&
            (pDrv->eType == DRIVE_TYPE_SSD_SATA ||
             pDrv->eType == DRIVE_TYPE_M2_SATA) &&
            (bID == 0xF1 || bID == 0xF2)) {
            unsigned __int64 nGB = ScaleAtaHostGiB(pDrv, qw48);
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
        {
            unsigned __int64 nLBA = qw48;
            unsigned __int64 nGB  = nLBA / (1024ULL * 1024ULL * 2ULL);
            if (nGB >= 1024)
                safe_snprintf(szMain, "%llu LBA  (~%llu TB)",
                              (unsigned long long)nLBA,
                              (unsigned long long)(nGB / 1024ULL));
            else if (nGB > 0)
                safe_snprintf(szMain, "%llu LBA  (~%llu GB)",
                              (unsigned long long)nLBA,
                              (unsigned long long)nGB);
            else
                safe_snprintf(szMain, "%llu LBA", (unsigned long long)nLBA);
        }
        break;
    }

    case 0xF9:
    {
        unsigned __int64 nGiB = qw48;
        if (nGiB > 0 && nGiB < 1000000ULL)
            safe_snprintf(szMain, "%llu ГиБ записано", (unsigned long long)nGiB);
        else
            safe_snprintf(szMain, "%llu", (unsigned long long)nGiB);
        break;
    }

    case 0xE9:
    {
        if (eCtl == CONTROLLER_INTEL && bVal <= 100) {
            safe_snprintf(szMain, "%u%%  индикатор износа", (unsigned)bVal);
            break;
        }
        unsigned __int64 nGiB = qw48;
        if (nGiB > 0 && nGiB < 1000000ULL)
            safe_snprintf(szMain, "%llu ГиБ записано", (unsigned long long)nGiB);
        else
            safe_snprintf(szMain, "%llu", (unsigned long long)nGiB);
        break;
    }

    case 0xA9:
    {
        if (dw32 >= 1 && dw32 <= 100)
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
        break;
    }

    case 0xB1:
        if (eCtl == CONTROLLER_SAMSUNG && bVal <= 100) {
            safe_snprintf(szMain, "износ/остаток %u%%", (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xCA:
        if (eCtl == CONTROLLER_MICRON && bVal <= 100) {
            safe_snprintf(szMain, "%u%% ресурса", (unsigned)bVal);
            break;
        }
        if (qw48 > 0xFFFFFFFFULL)
            safe_snprintf(szMain, "%llu", (unsigned long long)qw48);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

    case 0xAD:
        if (eCtl == CONTROLLER_MICRON)
            safe_snprintf(szMain, "%lu среднее стираний", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu циклов выравнивания", (unsigned long)dw32);
        break;

    case 0xA1:
    case 0xB2:
        safe_snprintf(szMain, "%lu блоков", (unsigned long)dw32);
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
            safe_snprintf(szMain, "0 сбоев  (ОК)");
        else
            safe_snprintf(szMain, "%lu сбоев  (!)", (unsigned long)dw32);
        break;

    case 0xAA:
    {
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        if (eCtl == CONTROLLER_INTEL && bVal <= 100)
            pct = (DWORD)bVal;
        safe_snprintf(szMain, "%lu%%  резервное пространство", (unsigned long)pct);
        break;
    }

    case 0xE8:
    {
        if (eCtl == CONTROLLER_INTEL && bVal <= 100) {
            safe_snprintf(szMain, "%u%%  резервное пространство", (unsigned)bVal);
            break;
        }
        DWORD pct = dw32 ? dw32 : (DWORD)bVal;
        safe_snprintf(szMain, "%lu%%  резервное пространство", (unsigned long)pct);
        break;
    }

    case 0xF0:
        safe_snprintf(szMain, "%lu ч полёта головок", (unsigned long)(DWORD)w16);
        break;

    case 0xE1:
        if (eCtl == CONTROLLER_INTEL) {
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

    case 0xE2:
        if (eCtl == CONTROLLER_INTEL)
            safe_snprintf(szMain, "%lu ч нагрузки", (unsigned long)dw32);
        else
            safe_snprintf(szMain, "%lu", (unsigned long)dw32);
        break;

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

/* Status from RAW counters and collapsed Value, not from Value==100. */
static const char* AtaRowStatus(const DRIVE_INFO* p, const SMART_ATTRIBUTE* a,
                                BYTE bThresh, BOOL bFailed)
{
    BYTE id = a->bAttrID;
    BYTE val = a->bAttrValue;
    BYTE worst = a->bWorstValue;
    BOOL ssd = p->bIsNVMe || p->eType == DRIVE_TYPE_SSD_SATA ||
               p->eType == DRIVE_TYPE_M2_SATA;
    int n = -1;

    if (bFailed) {
        if (a->wStatusFlags & 0x0001)
            return "СБОЙ";
        return "Внимание";
    }
    if (bThresh > 0 && worst != 0 && worst != 255 && worst <= bThresh)
        return "было";

    if (ssd && (id == 0xE7 || id == 0xA9)) {
        int nLeft = p->nEndurancePercent;
        if (nLeft >= 0 && nLeft <= 5) return "Внимание";
        if (nLeft >= 0 && nLeft <= 10) return "Внимание";
        if (nLeft >= 0 && nLeft <= 20) return "Риск";
        return "ОК";
    }

    if (id == 0x05) n = p->nReallocated;
    else if (id == 0xC4) {
        if (p->nRemapEvents < 0 && GetRawValue48(a->bRawValue) != 0)
            return "Не оценивается";
        n = p->nRemapEvents;
    }
    else if (id == 0xC5) n = p->nPendingSectors;
    else if (id == 0xC6) n = p->nUncorrectable;
    else if (id == 0xC7) n = p->nCrcErrors;
    if (n > 0) {
        if (id == 0xC6 || n >= 10 || (id == 0xC5 && n >= 4))
            return "ПЛОХО";
        return "Внимание";
    }

    if (id == 0xBB) {
        int n187 = DecodeReportedUncorrect(a->bRawValue, p->eVendor);
        if (n187 < 0) return "Не оценивается";
        if (n187 > 0) return "ПЛОХО";
        if (val <= 1) return "ПЛОХО";
        if (val <= 10) return "Внимание";
        return "ОК";
    }

    if (id == 0xC3) {
        if (!ssd && p->eVendor == VENDOR_SEAGATE) {
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
        if (val > 0 && val <= 1) return "ПЛОХО";
        if (val > 0 && val <= 10) return "Внимание";
        return "ОК";
    }
    if (id == 0x01 || id == 0x07) {
        DWORD dw01;
        WORD lo16;
        if (ssd) return "ОК";
        if (p->eVendor == VENDOR_SEAGATE) {
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
        /* WD: RAW 01/07 is normally 0. Samsung F1-style huge counters are noise. */
        if (p->eVendor == VENDOR_SAMSUNG && dw01 > 10000u)
            return "ОК";
        if ((p->eVendor == VENDOR_WDC || p->eVendor == VENDOR_SAMSUNG) && dw01 > 0)
            return "Внимание";
        return "ОК";
    }
    if (id == 0xB8 && !ssd && GetRawValue(a->bRawValue) > 0)
        return "Внимание";
    if (id == 0x0A && !ssd && GetRawValue(a->bRawValue) > 2)
        return "контекст";

    if (id == 0xC0 && DriveTreatsC0AsPowerLoss(p))
        return (GetRawValue(a->bRawValue) > 0) ? "журнал питания" : "ОК";
    if ((id == 0xA0 || id == 0xAE) && GetRawValue(a->bRawValue) > 0)
        return "журнал питания";
    /* 188 command timeout: cable/power, not media. 189 high-fly: vibration. */
    if ((id == 0xBC || id == 0xBD) && !ssd && GetRawValue(a->bRawValue) > 0)
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

    if ((id == 0xC2 || id == 0xBE) && p->nTemperatureC > 70)
        return "Жарко";

    if ((a->wStatusFlags & 0x0001) && bThresh > 0 && bThresh < 50 &&
        val > bThresh && val < bThresh + 10)
        return "Внимание";
    return "ОК";
}

static void FormatSmartRaw(BYTE bID, BYTE* pRaw,
                           BYTE bVal, BYTE bWorst, BYTE bThresh,
                           const DRIVE_INFO* pDrv,
                           char* szBuf, int nBufLen)
{
    char szDec[80];
    FormatSmartValue(bID, pRaw, bVal, bWorst, bThresh,
                     pDrv, szDec, sizeof(szDec));
    safe_snprintf_n(szBuf, nBufLen, "%02X%02X%02X%02X%02X%02X (%s)",
              pRaw[5], pRaw[4], pRaw[3], pRaw[2], pRaw[1], pRaw[0], szDec);
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

    if (pInfo->bIsUSB && !pInfo->bSMART_Supported) {
        const char* msg = IsLikelyUsbFlashDrive(pInfo)
            ? TN("У этой USB-флешки SMART не отдаётся — так у большинства флешек.",
                 "This USB flash drive does not export SMART — most sticks don't.")
            : TN("USB-корпус/мост не отдаёт SMART",
                 "USB enclosure/bridge does not export SMART");
        AttrRowSet(&rows[0], "--", msg, "", "—", "—", "", "");
        nDesired = 1;
    }
    else if (pInfo->bIsNVMe && !pInfo->bSMART_Supported) {
        char szErr[64];
        safe_snprintf(szErr, "Win32 %lu", (unsigned long)pInfo->dwErrNvmeProtocol);
        AttrRowSet(&rows[0], "--", TN("NVMe Health Log IOCTL не удался",
                                       "NVMe Health Log IOCTL failed"),
                   szErr, "—", "—", "", "");
        AttrRowSet(&rows[1], "--", "5=нет прав  1/50=нет в драйвере  87=параметр",
                   "", "—", "—", "", "");
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

        #define NVME_ROW(id_, name_, val_, stat_) \
        { \
            if (nDesired < MAX_ATTR_ROWS) \
                AttrRowSet(&rows[nDesired++], (id_), (name_), (val_), "—", "—", "", (stat_)); \
        }

        char szCrit[64], szVBuf[64], szSpare[32], szSpTh[32], szPctU[32];
        char szDUR[64], szDUW[64], szPOH[64], szPC[32];
        char szUS[32], szME[32], szEL[32], szWCT[32], szCCT[32];

        if (pLog->CriticalWarning == 0) safe_snprintf(szCrit, TN("0 (нет)", "0 (none)"));
        else safe_snprintf(szCrit, "0x%02X (!)", pLog->CriticalWarning);
        NVME_ROW("01h", TN("Критическое предупреждение", "Critical warning"),
                 szCrit, (pLog->CriticalWarning?"ПЛОХО":"ОК"));

        if (nWarnC <= 0) nWarnC = pInfo->nTempWarnC;
        if (nCritC <= 0) nCritC = pInfo->nTempCritC;
        if (nWarnC > 0 && nCritC > 0 && pInfo->nTempMaxC > 0)
            safe_snprintf(szVBuf, "%d C (макс. %d, пред. %d, крит. %d)",
                          nTempC, pInfo->nTempMaxC, nWarnC, nCritC);
        else if (nWarnC > 0 && nCritC > 0)
            safe_snprintf(szVBuf, "%d C (пред. %d, крит. %d)", nTempC, nWarnC, nCritC);
        else if (nCritC > 0 && pInfo->nTempMaxC > 0)
            safe_snprintf(szVBuf, "%d C (макс. %d, крит. %d)",
                          nTempC, pInfo->nTempMaxC, nCritC);
        else if (nWarnC > 0)
            safe_snprintf(szVBuf, "%d C (пред. %d)", nTempC, nWarnC);
        else if (nCritC > 0)
            safe_snprintf(szVBuf, "%d C (крит. %d)", nTempC, nCritC);
        else
            safe_snprintf(szVBuf, "%d C (%d K)", nTempC, (int)wTempK);
        {
            const char* szTstat = "ОК";
            if (pInfo->eTempBand == TEMP_BAND_CRITICAL) szTstat = "Жарко";
            else if (pInfo->eTempBand == TEMP_BAND_HIGH) szTstat = "Внимание";
            else if (pInfo->eTempBand == TEMP_BAND_ELEVATED) szTstat = "Риск";
            NVME_ROW("02h", TN("Температура", "Temperature"), szVBuf, szTstat);
        }
        if (nWarnC > 0) {
            char szW[32];
            safe_snprintf(szW, "%d C", nWarnC);
            NVME_ROW("--", TN("Порог предупреждения", "Warning threshold"), szW, "ОК");
        }
        if (nCritC > 0) {
            char szC[32];
            safe_snprintf(szC, "%d C", nCritC);
            NVME_ROW("--", TN("Макс. безопасная", "Critical threshold"), szC, "ОК");
        }
        if (pInfo->nTempMaxC > 0) {
            char szM[32];
            const char* st = "ОК";
            safe_snprintf(szM, "%d C", pInfo->nTempMaxC);
            if (nCritC > 0 && pInfo->nTempMaxC >= nCritC) st = "Жарко";
            else if (nWarnC > 0 && pInfo->nTempMaxC >= nWarnC) st = "Внимание";
            NVME_ROW("--", TN("Макс. зафиксированная", "Lifetime max"), szM, st);
        }

        safe_snprintf(szSpare,"%d %%",(int)pLog->AvailableSpare);
        NVME_ROW("03h", TN("Запас блоков", "Available spare"), szSpare,
            (pLog->AvailableSpare<pLog->AvailableSpareThreshold?"ПЛОХО":"ОК"));

        safe_snprintf(szSpTh,"%d %%",(int)pLog->AvailableSpareThreshold);
        NVME_ROW("04h", TN("Порог запаса блоков", "Spare threshold"), szSpTh, "ОК");

        safe_snprintf(szPctU,"%d %%",(int)pLog->PercentageUsed);
        {
            const char* szWearSt = "ОК";
            int nLeft = 100 - (int)pLog->PercentageUsed;
            if (nLeft < 0) nLeft = 0;
            if (pLog->PercentageUsed >= 100 || nLeft <= 5) szWearSt = "Внимание";
            else if (nLeft <= 10) szWearSt = "Внимание";
            else if (nLeft <= 20) szWearSt = "Риск";
            NVME_ROW("05h", TN("Износ", "Percentage used"), szPctU, szWearSt);
        }

        FormatNvmeHostBytes(qwDataRead, szDUR, sizeof(szDUR));
        NVME_ROW("06h", TN("Прочитано (host)", "Data units read"), szDUR, "ОК");

        FormatNvmeHostBytes(qwDataWritten, szDUW, sizeof(szDUW));
        NVME_ROW("07h", TN("Записано (host)", "Data units written"), szDUW, "ОК");

        {
            char szHR[64], szHW[64], szBT[64];
            safe_snprintf(szHR, "%llu", (unsigned long long)pInfo->qwNVMeHostReads);
            NVME_ROW("08h", TN("Команды чтения хоста", "Host read commands"), szHR, "ОК");
            safe_snprintf(szHW, "%llu", (unsigned long long)pInfo->qwNVMeHostWrites);
            NVME_ROW("09h", TN("Команды записи хоста", "Host write commands"), szHW, "ОК");
            safe_snprintf(szBT, "%llu мин", (unsigned long long)pInfo->qwNVMeControllerBusyTime);
            NVME_ROW("0Ah", TN("Занятость контроллера", "Controller busy time"), szBT, "ОК");
        }

        safe_snprintf(szPC,"%llu",(unsigned long long)qwPowerCycles);
        NVME_ROW("0Bh", TN("Циклы включения", "Power cycles"), szPC, "ОК");

        {
            DWORD dwPoh = (qwPOH > 0xFFFFFFFFULL) ? 0xFFFFFFFFUL : (DWORD)qwPOH;
            FormatPowerOnHours(dwPoh, szPOH, (int)sizeof(szPOH));
        }
        NVME_ROW("0Ch", Tr(STR_POH), szPOH, "ОК");

        safe_snprintf(szUS,"%llu",(unsigned long long)qwUnsafeSDs);
        NVME_ROW("0Dh", TN("Небезопасные выключения", "Unsafe shutdowns"), szUS,
            (qwUnsafeSDs>0?"журнал питания":"ОК"));

        safe_snprintf(szME,"%llu",(unsigned long long)qwMediaErr);
        NVME_ROW("0Eh", TN("Ошибки носителя", "Media errors"), szME,
            (qwMediaErr>0?"ПЛОХО":"ОК"));

        safe_snprintf(szEL,"%llu",(unsigned long long)qwErrLog);
        NVME_ROW("0Fh", TN("Записи в журнале ошибок", "Error log entries"), szEL,
            (qwErrLog>0?"Внимание":"ОК"));

        safe_snprintf(szWCT,"%lu мин",(unsigned long)pLog->WarningCompTempTime);
        NVME_ROW("--", TN("Время при высокой температуре", "Time above warning temp"),
            szWCT, (pLog->WarningCompTempTime>0?"контекст":"ОК"));

        safe_snprintf(szCCT,"%lu мин",(unsigned long)pLog->CriticalCompTempTime);
        NVME_ROW("--", TN("Время при критической температуре", "Time above critical temp"),
            szCCT, (pLog->CriticalCompTempTime>0?"контекст":"ОК"));
        {
            int ts;
            for (ts = 0; ts < 8; ts++) {
                if (pInfo->nTempSensor[ts] >= 0) {
                    char szId[8], szName[48], szVal[32];
                    safe_snprintf(szId, "T%d", ts + 1);
                    safe_snprintf(szName, TN("Датчик температуры %d", "Temperature sensor %d"), ts + 1);
                    safe_snprintf(szVal, "%d C", pInfo->nTempSensor[ts]);
                    {
                        const char* szSt = "ОК";
                        int t = pInfo->nTempSensor[ts];
                        if (nCritC > 0 && t >= nCritC) szSt = "Жарко";
                        else if (nWarnC > 0 && t >= nWarnC) szSt = "Внимание";
                        else if (nWarnC < 0 && nCritC < 0 && t > 70) szSt = "Жарко";
                        NVME_ROW(szId, szName, szVal, szSt);
                    }
                }
            }
        }
        if (pLog->ThermalMgmtTemp1TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp1TransCnt);
            NVME_ROW("--", TN("Thermal Mgmt T1 переходов", "Thermal Mgmt T1 transitions"), sz, "ОК");
        }
        if (pLog->ThermalMgmtTemp2TransCnt) {
            char sz[32];
            safe_snprintf(sz, "%lu", (unsigned long)pLog->ThermalMgmtTemp2TransCnt);
            NVME_ROW("--", TN("Thermal Mgmt T2 переходов", "Thermal Mgmt T2 transitions"), sz, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp1) {
            char sz[32];
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp1);
            NVME_ROW("--", TN("Thermal Mgmt T1 время", "Thermal Mgmt T1 time"), sz, "ОК");
        }
        if (pLog->TotalTimeThermalMgmtTemp2) {
            char sz[32];
            safe_snprintf(sz, "%lu с", (unsigned long)pLog->TotalTimeThermalMgmtTemp2);
            NVME_ROW("--", TN("Thermal Mgmt T2 время", "Thermal Mgmt T2 time"), sz, "ОК");
        }

        #undef NVME_ROW
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
            char szVal[16], szWorst[16], szThresh[16], szRaw[128];

            safe_snprintf(szVal, "%u", (unsigned)pAttr->bAttrValue);
            safe_snprintf(szWorst, "%u", (unsigned)pAttr->bWorstValue);
            if (bThresh)
                safe_snprintf(szThresh, "%u", (unsigned)bThresh);
            else
                safe_snprintf(szThresh, "—");
            FormatSmartRaw(pAttr->bAttrID, pAttr->bRawValue,
                           pAttr->bAttrValue, pAttr->bWorstValue, bThresh,
                           pInfo,
                           szRaw, 128);
            szStat = AtaRowStatus(pInfo, pAttr, bThresh, bFailed);
            {
                char szId[8];
                safe_snprintf(szId, "%d", pAttr->bAttrID);
                AttrRowSet(&rows[nDesired++], szId, GetAttrNameEx(pAttr->bAttrID, pInfo),
                           szVal, szWorst, szThresh, szRaw, szStat);
            }
        }

        if (nDesired == 0) {
            /* Bridge gave us only SCSI LOG SENSE data (no ATA attribute
             * table). Show what was retrieved instead of an empty list. */
            AttrRowSet(&rows[nDesired++], "--", TN("Прогноз отказа (SCSI)", "SCSI predict failure"),
                       pInfo->bPredictFailure
                           ? TN("Отказ прогнозируется", "Failure predicted")
                           : TN("Отказ не прогнозируется", "Failure not predicted"),
                       "—", "—", "",
                       pInfo->bPredictFailure ? "СБОЙ" : "ОК");

            if (nDesired < MAX_ATTR_ROWS) {
                char szT[32];
                if (pInfo->nTemperatureC > 0)
                    safe_snprintf(szT, "%d C", pInfo->nTemperatureC);
                else
                    safe_snprintf(szT, "%s", Tr(STR_NO_DATA));
                AttrRowSet(&rows[nDesired++], "--",
                           TN("Температура (SCSI Log Sense)", "Temperature (SCSI Log Sense)"),
                           szT, "—", "—", "",
                           (pInfo->nTemperatureC > 70 ? "Жарко" : "ОК"));
            }

            if (nDesired < MAX_ATTR_ROWS) {
                AttrRowSet(&rows[nDesired++], "--",
                           TN("Полная таблица SMART мостом не отдаётся",
                              "Full SMART table is not provided by the bridge"),
                           "", "—", "—", "", "");
            }
        }

        /* Extra ATA info already collected in AcquireATASMART — display only. */
        if (nDesired < MAX_ATTR_ROWS && pInfo->wRotationRate >= 0x0401) {
            char sz[32];
            safe_snprintf(sz, TN("%u об/мин", "%u RPM"), (unsigned)pInfo->wRotationRate);
            AttrRowSet(&rows[nDesired++], "--", TN("Обороты", "Rotation"), sz, "—", "—", "", "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotErrorLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nErrorLogCount);
            AttrRowSet(&rows[nDesired++], "--", TN("Журнал ошибок SMART", "SMART error log"), sz, "—", "—", "",
                       pInfo->nErrorLogCount > 0 ? "Внимание" : "ОК");
        }
        if (nDesired < MAX_ATTR_ROWS && pInfo->bGotSelfTestLog) {
            char sz[32];
            safe_snprintf(sz, "%d", pInfo->nSelfTestStatus);
            AttrRowSet(&rows[nDesired++], "--", TN("Журнал самопроверки", "SMART self-test log"), sz, "—", "—", "", "ОК");
        }
    }

    SendMessage(hList, WM_SETREDRAW, FALSE, 0);

    int nCurrent = ListView_GetItemCount(hList);

    int row;
    for (row = 0; row < nDesired; row++) {
        LPARAM lpStat = AttrStatusParam(rows[row].col[6]);
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
        for (col = 0; col < 7; col++)
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

#define ABOUT_W  440
#define ABOUT_H  360
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
            "Не удалось открыть браузер.\n" DONATE_URL,
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
                "DriveMonitor " DRIVEMONITOR_VERSION " (сборка " DRIVEMONITOR_BUILD_STR ")",
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
                "\xC2\xA9 2026 chuikoff",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(20), UiScale(118), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hCopy, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            HWND hDrv = CreateWindowExU8(0, "STATIC",
                Tr(STR_ABOUT_AUTHOR),
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                UiScale(20), UiScale(140), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)0, g_hInst, NULL);
            SendMessageA(hDrv, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                UiScale(20), UiScale(166), cx - UiScale(40), UiScale(2),
                hDlg, (HMENU)0, g_hInst, NULL);

            {
                HWND hLicStatus = CreateWindowExU8(0, "STATIC",
                    Tr(STR_ABOUT_FOSS),
                    WS_CHILD | WS_VISIBLE | SS_CENTER,
                    UiScale(20), UiScale(176), cx - UiScale(40), UiScale(18),
                    hDlg, (HMENU)IDC_ABOUT_LIC_STATUS, g_hInst, NULL);
                SendMessageA(hLicStatus, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);

                HWND hActivate = CreateWindowExU8(0, "BUTTON", Tr(STR_ABOUT_SUPPORT),
                    WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                    (cx - UiScale(120)) / 2, UiScale(200), UiScale(120), UiScale(26),
                    hDlg, (HMENU)IDC_ABOUT_ACTIVATE, g_hInst, NULL);
                SendMessageA(hActivate, WM_SETFONT, (WPARAM)g_hFontNormal, TRUE);
            }

            CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_ETCHEDHORZ,
                UiScale(20), UiScale(234), cx - UiScale(40), UiScale(2),
                hDlg, (HMENU)0, g_hInst, NULL);

            HFONT hFontLink = UiMakeFont(12, FW_NORMAL, TRUE);
            SetPropA(hDlg, PROP_ABOUT_FONT_LINK, (HANDLE)hFontLink);
            HWND hLink = CreateWindowExU8(0, "STATIC",
                DONATE_URL,
                WS_CHILD | WS_VISIBLE | SS_CENTER | SS_NOTIFY,
                UiScale(20), UiScale(244), cx - UiScale(40), UiScale(18),
                hDlg, (HMENU)IDC_ABOUT_LINK, g_hInst, NULL);
            SendMessageA(hLink, WM_SETFONT, (WPARAM)hFontLink, TRUE);

            HWND hBtn = CreateWindowExU8(0, "BUTTON", "OK",
                WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                (cx - UiScale(80)) / 2, UiScale(274), UiScale(80), UiScale(26),
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
                /* Boosty link - render in hyperlink blue. */
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
            if (hOver && GetDlgCtrlID(hOver) == IDC_ABOUT_LINK && s_hCursorHand) {
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
        U8ToW(Tr(STR_COL_STATUS), w, 64); col.pszText = w;
        SendMessageW(hList, LVM_SETCOLUMNW, 6, (LPARAM)&col);
    }
    DrawMenuBar(hWnd);
    if (g_nDriveCount > 0)
        UpdateDriveInfo(hWnd, g_nSelectedDrive);
    UpdateAttrList(hWnd, g_nSelectedDrive);
    UpdateDriveButtons(hWnd);
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
                WS_CHILD | WS_VISIBLE | SS_LEFT,
                nRightX, y, 78, 16,
                hWnd, (HMENU)(UINT_PTR)ax[i].idL, g_hInst, NULL);
            HWND hV = CreateWindowExU8(0, "STATIC", "",
                WS_CHILD | WS_VISIBLE | SS_LEFT,
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

    HWND hList = CreateWindowExU8(
        WS_EX_CLIENTEDGE, "SysListView32", "",
        WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER,
        nRightX, 283, 540, 590,
        hWnd, (HMENU)IDC_ATTR_LIST, g_hInst, NULL
    );
    SendMessage(hList, WM_SETFONT, (WPARAM)g_hFontSmall, TRUE);
    ListView_SetExtendedListViewStyle(hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    ListView_SetBkColor(hList, CLR_PANEL);
    ListView_SetTextBkColor(hList, CLR_ROW1);
    ListView_SetTextColor(hList, CLR_TEXT);

    {
        LVCOLUMNW col;
        WCHAR w0[16], w1[64], w2[64], w3[64], w4[64], w5[64], w6[64];
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
        col.cx = 120; col.pszText = w5;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 5, (LPARAM)&col);
        U8ToW(Tr(STR_COL_STATUS), w6, 64);
        col.cx = 124; col.pszText = w6;
        SendMessageW(hList, LVM_INSERTCOLUMNW, 6, (LPARAM)&col);
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
        if (hReread) SetWindowPos(hReread, NULL, nRightX, UiScale(110), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hReport) SetWindowPos(hReport, NULL, nRightX + UiScale(100), UiScale(110), UiScale(90), UiScale(24), SWP_NOZORDER);
        if (hEject)  SetWindowPos(hEject,  NULL, nRightX, UiScale(138), nBarsW, UiScale(24), SWP_NOZORDER);
        {
            int axL[] = { IDC_AXIS_MEDIA_L, IDC_AXIS_IFACE_L,
                          IDC_AXIS_TEMPA_L, IDC_AXIS_ROW4_L };
            int axV[] = { IDC_AXIS_MEDIA_V, IDC_AXIS_IFACE_V,
                          IDC_AXIS_TEMPA_V, IDC_AXIS_ROW4_V };
            int ai;
            for (ai = 0; ai < 4; ai++) {
                HWND hL = GetDlgItem(hWnd, axL[ai]);
                HWND hV = GetDlgItem(hWnd, axV[ai]);
                int y = UiScale(166) + UiScale(16) * ai;
                if (hL) SetWindowPos(hL, NULL, nRightX, y, UiScale(78), UiScale(16), SWP_NOZORDER);
                if (hV) SetWindowPos(hV, NULL, nRightX + UiScale(80), y, nBarsW - UiScale(80), UiScale(16), SWP_NOZORDER);
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

    hList = GetDlgItem(hWnd, IDC_ATTR_LIST);
    if (hList) {
        int nListTop = UiScale(283);
        int nListH   = cyClient - nListTop - UiScale(8);
        int nListW   = cxClient - nRightX - UiScale(8);
        int nRaw;
        if (nListH < UiScale(50)) nListH = UiScale(50);
        if (nListW < UiScale(200)) nListW = UiScale(200);
        SetWindowPos(hList, NULL, nRightX, nListTop, nListW, nListH, SWP_NOZORDER);
        ListView_SetColumnWidth(hList, 0, UiScale(72));
        ListView_SetColumnWidth(hList, 1, UiScale(200));
        ListView_SetColumnWidth(hList, 2, UiScale(70));
        ListView_SetColumnWidth(hList, 3, UiScale(50));
        ListView_SetColumnWidth(hList, 4, UiScale(50));
        ListView_SetColumnWidth(hList, 6, UiScale(124));
        nRaw = nListW - UiScale(42) - UiScale(200) - UiScale(70) - UiScale(50)
             - UiScale(50) - UiScale(124) - UiScale(24);
        if (nRaw < UiScale(80)) nRaw = UiScale(80);
        ListView_SetColumnWidth(hList, 5, nRaw);
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
        CreateMenuBar(hWnd);
        CreateControls(hWnd);
        /* No tray: close exits, no background monitoring. */
        DeviceNotify_Register(hWnd);
        /* SMART is read once at create and on hotplug — no periodic refresh. */

        UpdateWindowTitle(hWnd);
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
            /* Status badge for col 6 only. Code comes from lItemlParam
             * (set at fill time). Never LVM_GETITEM / CreateFont here. */
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
                        if (pCD->iSubItem != 6)
                            return CDRF_NEWFONT;

                        int nSt = (int)(pCD->nmcd.lItemlParam & 0xFF);
                        RECT rcCell = pCD->nmcd.rc;
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
                            li.iSubItem = 6;
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

                        int badgeH  = sz.cy + UiScale(6);
                        int badgeW  = sz.cx + UiScale((nSt == ATTRST_SKIP) ? 20 : 16);
                        int cellCX  = rcCell.right  - rcCell.left;
                        int cellCY  = rcCell.bottom - rcCell.top;
                        int bx      = rcCell.left + (cellCX - badgeW) / 2;
                        int by      = rcCell.top  + (cellCY - badgeH) / 2;
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
        }
        return 0;

    case WM_TIMER:
        if (wParam == IDT_HOTPLUG) {
            KillTimer(hWnd, IDT_HOTPLUG);
            RefreshData(hWnd);    /* one-shot full scan on plug/unplug */
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
        return 0;

    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED)
            LayoutMainWindow(hWnd);
        return 0;

    case WM_DESTROY:
        UiSaveWindowPlace(hWnd);
        KillTimer(hWnd, IDT_HOTPLUG);
        DeviceNotify_Unregister();
        DestroyGDIObjects();
        if (g_gdiplusToken) { GdiplusShutdown(g_gdiplusToken); g_gdiplusToken = 0; }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, uMsg, wParam, lParam);
}
