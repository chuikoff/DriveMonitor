/* DriveMonitor - main window. GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef MAINWND_H
#define MAINWND_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "smart.h"
#include "resource.h"

#define DRIVEMONITOR_WNDCLASS L"DriveMonitorMainWnd"
#define MAX_DRIVES 8

#define IDC_DRIVE_LIST       1001
#define IDC_HEALTH_STATIC    1002
#define IDC_HEALTH_BAR_FRAME 1003
#define IDC_MODEL_STATIC     1004
#define IDC_SERIAL_STATIC    1005
#define IDC_FIRMWARE_STATIC  1006
#define IDC_SIZE_STATIC      1007
#define IDC_STATUS_STATIC    1008
#define IDC_ATTR_LIST        1009
#define IDC_REFRESH_BTN      1010
#define IDC_HEALTH_LABEL     1011
#define IDC_PREDICT_STATIC   1012
#define IDC_TEMP_STATIC        1015
#define IDC_PROTOCOL_STATIC    1016
#define IDC_MODEL_LABEL        1018
#define IDC_SERIAL_LABEL       1019
#define IDC_FIRMWARE_LABEL     1020
#define IDC_SIZE_LABEL         1021
#define IDC_TEMP_LABEL         1022
#define IDC_STATUS_LABEL       1023
#define IDC_PROTOCOL_LABEL     1024
#define IDC_REREAD_BTN         1026
#define IDC_REPORT_BTN         1035
#define IDC_EJECT_BTN          1039
#define IDC_MONITOR_STATIC   1048
#define IDC_DRIVE_BTN_BASE   1100
#define IDM_ABOUT            2001
#define IDM_EXIT             2002
#define IDM_REPORT           2003
#define IDM_SCREENSHOT       2004
#define IDM_DONATE           2006
#define IDM_EJECT            2007
#define IDM_ZOOM_IN          2010
#define IDM_ZOOM_OUT         2011
#define IDM_ZOOM_100         2012
#define IDM_ZOOM_125         2013
#define IDM_ZOOM_150         2014
#define IDM_ZOOM_175         2015
#define IDM_ZOOM_200         2016
#define IDM_LANG_RU          2020
#define IDM_LANG_EN          2021
#define IDM_MONITOR          2022
#define IDM_THEME_SYSTEM     2030
#define IDM_THEME_LIGHT      2031
#define IDM_THEME_DARK       2032
#define IDT_HOTPLUG          3002
#define IDT_MONITOR          3003
#define WM_APP_REFRESH_DONE  (WM_APP + 1)
#define WM_TRAYICON          (WM_APP + 2)
#define DRIVE_BTN_H    78
#define DRIVE_BTN_GAP   6
#define DRIVE_BTN_PANEL_W 248
#define WINDOW_W     1040   /* minimum */
#define WINDOW_W_DEF 1200
#define WINDOW_H     860
#define WINDOW_H_MIN 640
/* v2 palette. Light and dark sets live in mainwnd.cpp (UiApplyPalette).
 * Status colors come in pairs: tinted pill background + readable text,
 * both >= 4.5:1. See docs/UI_SPEC.md. */
typedef struct _UI_PALETTE {
    COLORREF bg, side, panel, sel, hover, border, border2;
    COLORREF text, textDim, textFaint, accent, row2, rowHl, rowSel, btn;
    COLORREF okBg, ok, riskBg, risk, warnBg, warn, badBg, bad;
    COLORREF critBg, critFg, critText, neuBd, neu;
} UI_PALETTE;
extern UI_PALETTE g_Pal;
extern int        g_bUiDark;
extern int        g_bDemoMode;     /* --demo: fixture drives, no device I/O */
extern int        g_nThemeOverride; /* --theme=light|dark: 1/2, 0 = none */

#define CLR_BG          g_Pal.bg
#define CLR_SIDE        g_Pal.side
#define CLR_PANEL       g_Pal.panel
#define CLR_BORDER      g_Pal.border
#define CLR_BORDER2     g_Pal.border2
#define CLR_TEXT        g_Pal.text
#define CLR_TEXT_DIM    g_Pal.textDim
#define CLR_TEXT_FAINT  g_Pal.textFaint
#define CLR_GREEN       g_Pal.ok
#define CLR_YELLOW      g_Pal.risk
#define CLR_ORANGE      g_Pal.warn
#define CLR_CRITICAL    g_Pal.critText
#define CLR_RED         g_Pal.bad
#define CLR_ACCENT      g_Pal.accent
#define CLR_HEADER      g_Pal.panel
#define CLR_ROW1        g_Pal.panel
#define CLR_ROW2        g_Pal.row2

extern DRIVE_INFO  g_Drives[MAX_DRIVES];
extern int         g_nDriveCount;
extern int         g_nSelectedDrive;
extern HINSTANCE   g_hInst;
extern HWND        g_hMainWnd;
extern HWND        g_hHealthBar;
extern HWND        g_hDriveBtn[MAX_DRIVES];
extern HBRUSH  g_hbrBG;
extern HBRUSH  g_hbrPanel;
extern HBRUSH  g_hbrGreen;
extern HBRUSH  g_hbrYellow;
extern HBRUSH  g_hbrRed;
extern HFONT   g_hFontTitle;
extern HFONT   g_hFontNormal;
extern HFONT   g_hFontSmall;
extern HFONT   g_hFontBig;

LRESULT CALLBACK MainWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK HealthBarWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK DriveBtnWndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
void    RegisterHealthBarClass(HINSTANCE hInst);
void    RepaintHealthBar(void);
void    CreateControls(HWND hWnd);
void    CreateGDIObjects(void);
void    DestroyGDIObjects(void);
void    RefreshData(HWND hWnd);
void    UpdateDriveButtons(HWND hWnd);
void    UpdateDriveInfo(HWND hWnd, int nDrive);
void    UpdateAttrList(HWND hWnd, int nDrive);
void    PaintMain(HWND hWnd, HDC hdc);
void    ShowAboutDialog(HWND hWnd);
void    ShowHealthLectureDialog(HWND hParent);
COLORREF GetHealthStatusColor(DRIVE_HEALTH_STATUS eStatus);
int     UiScale(int px);
void    UiInitScale(void);
int     UiWindowW(void);
int     UiWindowH(void);
int     UiWindowHMin(void);
HACCEL  UiCreateAccelTable(void);
int     UiLoadWindowPlace(int* x, int* y, int* w, int* h, int* showCmd);
void    UiSaveWindowPlace(HWND hWnd);

#endif
