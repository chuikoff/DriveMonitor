/* DriveMonitor - main window. GPL-2.0-or-later. See LICENSE. */
#pragma once
#ifndef MAINWND_H
#define MAINWND_H
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "smart.h"
#include "resource.h"

#define DRIVEMONITOR_WNDCLASS L"DriveMonitorMainWnd"
/* Slots kept for the scan. The left column scrolls when they do not fit. */
#define MAX_DRIVES 32

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
#define IDC_LATENCY_BTN        1050
#define IDC_MONITOR_STATIC   1048
#define IDC_DRIVE_BTN_BASE   1100
#define IDM_ABOUT            2001
#define IDM_EXIT             2002
#define IDM_REPORT           2003
#define IDM_SCREENSHOT       2004
#define IDM_COPY_REPORT      2005
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
#define IDM_MONITOR_1M       2023
#define IDM_MONITOR_5M       2024
#define IDM_MONITOR_15M      2025
#define IDM_MONITOR_30M      2026
#define IDM_MONITOR_1H       2027
#define IDM_COPY_ROW         2030
#define IDM_COPY_RAW         2031
#define IDM_COPY_DECODE      2032
#define IDM_COPY_RAW_DECODE  2033
#define IDM_COPY_TABLE_TSV   2034
#define IDM_COPY_TABLE_MD    2035
#define IDM_COPY_SERIAL      2036
#define IDM_COPY_SUMMARY     2037
#define IDM_LATENCY_TEST     2040
#define IDM_FULL_READ_TEST   2041
#define IDM_DRAM_VERIFY      2042
#define IDT_HOTPLUG          3002
#define IDT_MONITOR          3003
#define WM_APP_REFRESH_DONE  (WM_APP + 1)
#define WM_TRAYICON          (WM_APP + 2)
#define DRIVE_BTN_H    56
#define DRIVE_BTN_GAP   6
#define DRIVE_BTN_PANEL_W 200
#define WINDOW_W     1080
#define WINDOW_H     960   /* ~32 SMART rows without v-scroll */
#define WINDOW_H_MIN 620
#define CLR_BG          RGB(236, 242, 252)
#define CLR_PANEL       RGB(252, 254, 255)
#define CLR_BORDER      RGB(186, 198, 216)
#define CLR_TEXT        RGB(30,  35,  50)
#define CLR_TEXT_DIM    RGB(90, 100, 120)
#define CLR_GREEN       RGB(30,  150, 60)
#define CLR_YELLOW      RGB(180, 120, 0)
#define CLR_ORANGE      RGB(220, 110, 20)
#define CLR_CRITICAL    RGB(40,  40,  40)
#define CLR_RED         RGB(190, 30,  30)
#define CLR_ACCENT      RGB(30,  100, 210)
#define CLR_HEADER      RGB(220, 230, 244)
#define CLR_ROW1        RGB(252, 254, 255)
#define CLR_ROW2        RGB(240, 246, 252)

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
void    RefreshDataEx(HWND hWnd, BOOL bBackground);
void    UpdateDriveButtons(HWND hWnd);
void    UpdateDriveInfo(HWND hWnd, int nDrive);
void    UpdateAttrList(HWND hWnd, int nDrive);
void    PaintMain(HWND hWnd, HDC hdc);
void    ShowAboutDialog(HWND hWnd);
void    ShowHealthLectureDialog(HWND hParent);
void    ShowLatencyProbeDialog(HWND hParent, int nDriveIndex);
void    ShowLatencyProbeDialogEx(HWND hParent, int nDriveIndex, int initialMode);
void    ShowDramVerifyDialog(HWND hParent, int nDriveIndex);
/* Wheel over the drive column scrolls that column. Other targets keep the message. */
BOOL    DrivePanelOnMouseWheel(const MSG* msg);
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
