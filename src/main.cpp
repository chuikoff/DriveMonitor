/* DriveMonitor - WinMain. MIT: see LICENSE. */

#ifdef _MSC_VER
#pragma comment(lib, "comctl32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <stdio.h>
#include <string.h>

#include "mainwnd.h"
#include "smart.h"
#include "resource.h"
#include "utf8ui.h"
#include "safestr.h"
#include "lang.h"

#define MUTEX_NAME  "Global\\DriveMonitor_SingleInstance"

static HANDLE CreateWorldMutex(void)
{
    SECURITY_DESCRIPTOR sd;
    InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE);

    SECURITY_ATTRIBUTES sa;
    sa.nLength              = sizeof(sa);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle       = FALSE;

    return CreateMutexA(&sa, TRUE, MUTEX_NAME);
}

static void BringExistingWindowToFront(void)
{
    HWND hExist = FindWindowW(DRIVEMONITOR_WNDCLASS, NULL);
    if (!hExist) return;

    if (!IsWindowVisible(hExist))
        ShowWindow(hExist, SW_SHOW);

    if (IsIconic(hExist))
        ShowWindow(hExist, SW_RESTORE);

    DWORD dwPid = 0;
    GetWindowThreadProcessId(hExist, &dwPid);
    AllowSetForegroundWindow(dwPid);
    SetForegroundWindow(hExist);
    BringWindowToTop(hExist);
}

static LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    char dir[MAX_PATH];
    char path[MAX_PATH + 64];
    FILE* f = NULL;
    DWORD n = GetTempPathA(MAX_PATH, dir);
    if (n && n < MAX_PATH) {
        safe_snprintf(path, "%sDriveMonitor_crash.txt", dir);
        f = fopen(path, "w");
    }
    if (!f && GetModuleFileNameA(NULL, dir, MAX_PATH)) {
        char* slash = strrchr(dir, '\\');
        if (!slash) slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = '\0';
            safe_snprintf(path, "%sDriveMonitor_crash.txt", dir);
            f = fopen(path, "w");
        }
    }
    if (f) {
        DWORD code = 0;
        void* addr = NULL;
        if (ep && ep->ExceptionRecord) {
            code = ep->ExceptionRecord->ExceptionCode;
            addr = ep->ExceptionRecord->ExceptionAddress;
        }
        fprintf(f, "exception=0x%08lX\naddress=%p\nthread=%lu\n",
                (unsigned long)code, addr, (unsigned long)GetCurrentThreadId());
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                   LPSTR lpCmdLine, int nCmdShow)
{
    (void)hPrevInstance;
    (void)lpCmdLine;

    SetUnhandledExceptionFilter(CrashFilter);

    HANDLE hMutex = CreateWorldMutex();
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        BringExistingWindowToFront();
        CloseHandle(hMutex);
        return 0;
    }

    g_hInst = hInstance;
    UiInitScale();
    UiLangInit();

    INITCOMMONCONTROLSEX icex;
    icex.dwSize = sizeof(icex);
    icex.dwICC  = ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icex);

    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = MainWndProc;
    wc.hInstance     = hInstance;
    wc.hCursor       = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = DRIVEMONITOR_WNDCLASS;
    wc.hIcon         = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm       = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                          IMAGE_ICON,
                                          GetSystemMetrics(SM_CXSMICON),
                                          GetSystemMetrics(SM_CYSMICON),
                                          LR_DEFAULTCOLOR);

    if (!RegisterClassExW(&wc)) {
        MessageBoxU8(NULL, "RegisterClassEx failed!", "Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    RECT wa;
    int nW = UiWindowW(), nH = UiWindowH();
    int nScrW, nScrH, nX, nY;
    int nShow = nCmdShow;
    if (!SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0)) {
        wa.left = 0;
        wa.top = 0;
        wa.right = GetSystemMetrics(SM_CXSCREEN);
        wa.bottom = GetSystemMetrics(SM_CYSCREEN);
    }
    nScrW = wa.right - wa.left;
    nScrH = wa.bottom - wa.top;
    if (nH > nScrH) nH = nScrH;
    if (nW > nScrW) nW = nScrW;
    nX = wa.left + (nScrW - nW) / 2;
    nY = wa.top  + (nScrH - nH) / 2;

    HWND hWnd = CreateWindowExU8(
        0,
        "DriveMonitorMainWnd",
        "DriveMonitor",
        WS_OVERLAPPEDWINDOW,
        nX, nY, nW, nH,
        NULL, NULL, hInstance, NULL
    );

    if (!hWnd) {
        MessageBoxU8(NULL, "CreateWindow failed!", "Error", MB_ICONERROR);
        if (hMutex) { ReleaseMutex(hMutex); CloseHandle(hMutex); }
        return 1;
    }

    /* WinX/WinY are GetWindowPlacement workspace coordinates.
     * CreateWindow takes screen coordinates; SetWindowPlacement does not. */
    {
        int lx, ly, lw, lh, ls;
        if (UiLoadWindowPlace(&lx, &ly, &lw, &lh, &ls)) {
            WINDOWPLACEMENT wp;
            ZeroMemory(&wp, sizeof(wp));
            wp.length = sizeof(wp);
            if (GetWindowPlacement(hWnd, &wp)) {
                wp.showCmd = (ls == SW_SHOWMAXIMIZED || ls == SW_MAXIMIZE)
                    ? (UINT)SW_SHOWMAXIMIZED : (UINT)SW_SHOWNORMAL;
                if (ls == SW_SHOWMINIMIZED || ls == SW_MINIMIZE)
                    wp.showCmd = SW_SHOWMINIMIZED;
                wp.rcNormalPosition.left = lx;
                wp.rcNormalPosition.top = ly;
                wp.rcNormalPosition.right = lx + lw;
                wp.rcNormalPosition.bottom = ly + lh;
                SetWindowPlacement(hWnd, &wp);
                nShow = (int)wp.showCmd;
            }
        }
    }

    ShowWindow(hWnd, nShow);
    UpdateWindow(hWnd);

    {
        HACCEL hAcc = UiCreateAccelTable();
        MSG msg;
        int nExit = 0;
        ZeroMemory(&msg, sizeof(msg));
        while (GetMessage(&msg, NULL, 0, 0)) {
            if (msg.message == WM_MOUSEWHEEL && (LOWORD(msg.wParam) & MK_CONTROL)) {
                SendMessage(hWnd, WM_COMMAND,
                    ((short)HIWORD(msg.wParam) > 0) ? IDM_ZOOM_IN : IDM_ZOOM_OUT, 0);
                continue;
            }
            if (hAcc && TranslateAccelerator(hWnd, hAcc, &msg))
                continue;
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        nExit = (int)msg.wParam;
        if (hAcc) DestroyAcceleratorTable(hAcc);
        if (hMutex) {
            ReleaseMutex(hMutex);
            CloseHandle(hMutex);
        }
        return nExit;
    }
}
