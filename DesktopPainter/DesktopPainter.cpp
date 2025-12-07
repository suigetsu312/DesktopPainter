// DesktopPainter.cpp : Defines the entry point for the application.
//

#include "framework.h"
#include "DesktopPainter.h"

#include <shellapi.h>
#include <vector>
#include <commctrl.h>

#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Gdi32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Comctl32.lib")

#define MAX_LOADSTRING 100

#ifndef MOD_NOREPEAT
#define MOD_NOREPEAT 0x4000
#endif

#ifndef GET_X_LPARAM
#define GET_X_LPARAM(lp) ((int)(short)LOWORD(lp))
#define GET_Y_LPARAM(lp) ((int)(short)HIWORD(lp))
#endif

// ─── Global Vars ──────────────────────────────────────────────

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];

const wchar_t CONFIG_WINDOW_CLASS[] = L"DesktopPainterConfigWindow";

const UINT WM_TRAYICON = WM_APP + 1;
const UINT TIMER_ID_HERE_FADE = 1;
// 工具種類
enum ToolType {
    TOOL_PEN = 0,
    TOOL_ERASER = 1,
};

ToolType g_tool = TOOL_PEN;

// 右下角工具按鈕在畫面上的位置（client 座標）
RECT g_rcBtnPen = { 0, 0, 0, 0 };
RECT g_rcBtnEraser = { 0, 0, 0, 0 };

// Tray menu IDs
enum {
    ID_TRAY_SHOW = 1001,
    ID_TRAY_CLEAR = 1002,
    ID_TRAY_CONFIG = 1003,
    ID_TRAY_EXIT = 1004,
    ID_TRAY_WHERE = 1005,
};

// 畫筆顏色（Config 用）
enum {
    ID_PEN_COLOR_RED = 2001,
    ID_PEN_COLOR_GREEN = 2002,
    ID_PEN_COLOR_BLUE = 2003,
    ID_PEN_COLOR_YELLOW = 2004,
    ID_PEN_COLOR_WHITE = 2005,
};

const int PEN_COLOR_FIRST = ID_PEN_COLOR_RED;
const int PEN_COLOR_LAST = ID_PEN_COLOR_WHITE;

struct MonitorInfoSimple {
    HMONITOR hMon;
    RECT     rcMonitor;
};

std::vector<MonitorInfoSimple> g_monitors;
int                            g_currentMonitor = 0;

NOTIFYICONDATAW g_nid = { 0 };
HWND            g_mainWnd = nullptr;
HWND            g_configWnd = nullptr;
bool            g_passthrough = false;

int g_canvasWidth = 0;
int g_canvasHeight = 0;

// 顯示層 bitmap（UpdateLayeredWindow 用）
HBITMAP g_hBitmap = nullptr;
BYTE* g_pPixels = nullptr;
int     g_bmpW = 0;
int     g_bmpH = 0;

// 使用者畫布層：ARGB32, size = w*h*4
std::vector<BYTE> g_drawBuf;

// 畫筆設定
int      g_penSize = 4;                    // px, 1~32
COLORREF g_penColor = RGB(255, 0, 0);       // default red

// HERE 淡出狀態（只作用在顯示層）
bool g_hereActive = false;
int  g_hereAlpha = 0;      // 0~255

// 繪圖用 tmp
bool  g_isDrawing = false;
POINT g_lastPoint = { 0, 0 };

// Config UI 控制
HWND g_hwndPenTrack = nullptr;
HWND g_hwndPenSizeLabel = nullptr;

// ─── Forward Decls ────────────────────────────────────────────

ATOM                MyRegisterClass(HINSTANCE hInstance);
BOOL                InitInstance(HINSTANCE, int);
LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
LRESULT CALLBACK    ConfigWndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

void RefreshMonitors();
void ApplyMonitorToMain(int index);

void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);
void ShowTrayMenu(HWND hwnd);
void ClearCanvas(HWND hwnd);
void UpdateTrayTip();
void UpdatePassthrough(HWND hwnd);

// Canvas / Drawing
void CreateCanvasBitmap(int w, int h);
void PresentLayered(HWND hWnd);

// 畫筆 helper（畫在 g_drawBuf）
inline void StampPixelDraw(int x, int y, COLORREF color, BYTE alpha);
void StampBrushDraw(int x, int y, COLORREF color, BYTE alpha);
void DrawLineOnDrawBuf(POINT a, POINT b);
void EraseLineOnDrawBuf(POINT a, POINT b);

// HERE 淡出（只畫在顯示層）
void StartHereMarker();
void UpdateHereMarkerFrame();
void DrawHereOnDisplay(BYTE alpha);
void DrawToolButtonsOnDisplay();
// Config 視窗：螢幕縮圖
void ShowConfigWindow();
void ComputeMonitorRectsForConfig(HWND hWnd, std::vector<RECT>& outRects);

// ─── WinMain ──────────────────────────────────────────────────

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
    _In_opt_ HINSTANCE hPrevInstance,
    _In_ LPWSTR    lpCmdLine,
    _In_ int       nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);

    LoadStringW(hInstance, IDS_APP_TITLE, szTitle, MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_DESKTOPPAINTER, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    if (!InitInstance(hInstance, nCmdShow))
        return FALSE;

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_DESKTOPPAINTER));

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }
    return (int)msg.wParam;
}

// ─── Window Class Register ────────────────────────────────────

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    // 主畫布
    WNDCLASSEXW wcex{};
    wcex.cbSize = sizeof(WNDCLASSEX);
    wcex.style = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc = WndProc;
    wcex.cbClsExtra = 0;
    wcex.cbWndExtra = 0;
    wcex.hInstance = hInstance;
    wcex.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_DESKTOPPAINTER));
    wcex.hCursor = LoadCursor(nullptr, IDC_CROSS);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName = nullptr;
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    ATOM mainAtom = RegisterClassExW(&wcex);

    // Config 視窗
    WNDCLASSEXW cfg{};
    cfg.cbSize = sizeof(WNDCLASSEX);
    cfg.style = CS_HREDRAW | CS_VREDRAW;
    cfg.lpfnWndProc = ConfigWndProc;
    cfg.cbClsExtra = 0;
    cfg.cbWndExtra = 0;
    cfg.hInstance = hInstance;
    cfg.hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_DESKTOPPAINTER));
    cfg.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cfg.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    cfg.lpszMenuName = nullptr;
    cfg.lpszClassName = CONFIG_WINDOW_CLASS;
    cfg.hIconSm = LoadIcon(cfg.hInstance, MAKEINTRESOURCE(IDI_SMALL));

    RegisterClassExW(&cfg);

    return mainAtom;
}

// ─── Monitor Helpers ──────────────────────────────────────────

BOOL CALLBACK EnumMonitorsProc(HMONITOR hMon, HDC, LPRECT, LPARAM lParam)
{
    auto vec = reinterpret_cast<std::vector<MonitorInfoSimple>*>(lParam);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hMon, &mi)) {
        MonitorInfoSimple info{};
        info.hMon = hMon;
        info.rcMonitor = mi.rcMonitor;
        vec->push_back(info);
    }
    return TRUE;
}

void RefreshMonitors()
{
    g_monitors.clear();
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc,
        reinterpret_cast<LPARAM>(&g_monitors));

    if (g_monitors.empty()) {
        MonitorInfoSimple info{};
        info.hMon = nullptr;
        info.rcMonitor.left = 0;
        info.rcMonitor.top = 0;
        info.rcMonitor.right = GetSystemMetrics(SM_CXSCREEN);
        info.rcMonitor.bottom = GetSystemMetrics(SM_CYSCREEN);
        g_monitors.push_back(info);
    }

    if (g_currentMonitor >= (int)g_monitors.size())
        g_currentMonitor = 0;
}

void ApplyMonitorToMain(int index)
{
    if (!g_mainWnd) return;
    if (index < 0 || index >= (int)g_monitors.size()) return;

    g_currentMonitor = index;

    RECT rc = g_monitors[index].rcMonitor;
    int newW = rc.right - rc.left;
    int newH = rc.bottom - rc.top;

    if (newW <= 0 || newH <= 0)
        return;

    g_canvasWidth = newW;
    g_canvasHeight = newH;

    // 不保留舊畫面：整張紙換新
    CreateCanvasBitmap(newW, newH);
    ClearCanvas(g_mainWnd);

    SetWindowPos(
        g_mainWnd,
        nullptr,
        rc.left, rc.top,
        newW, newH,
        SWP_NOZORDER | SWP_FRAMECHANGED
    );

    PresentLayered(g_mainWnd);
}

// ─── Canvas 建立 & Clear ─────────────────────────────────────

void CreateCanvasBitmap(int w, int h)
{
    if (g_hBitmap) {
        DeleteObject(g_hBitmap);
        g_hBitmap = nullptr;
        g_pPixels = nullptr;
    }

    g_drawBuf.clear();

    if (w <= 0 || h <= 0) {
        g_bmpW = g_bmpH = 0;
        return;
    }

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC hdc = GetDC(nullptr);
    g_hBitmap = CreateDIBSection(
        hdc, &bi, DIB_RGB_COLORS, (void**)&g_pPixels, nullptr, 0
    );
    ReleaseDC(nullptr, hdc);

    g_bmpW = w;
    g_bmpH = h;

    // 畫布層：全部 ARGB(1,0,0,0)
    g_drawBuf.resize((size_t)w * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            BYTE* px = &g_drawBuf[(y * w + x) * 4];
            px[0] = 0;
            px[1] = 0;
            px[2] = 0;
            px[3] = 1;   // 保持 hit-test
        }
    }
}

void ClearCanvas(HWND hwnd)
{
    if (!g_drawBuf.empty() && g_bmpW > 0 && g_bmpH > 0) {
        for (int y = 0; y < g_bmpH; ++y) {
            for (int x = 0; x < g_bmpW; ++x) {
                BYTE* px = &g_drawBuf[(y * g_bmpW + x) * 4];
                px[0] = 0;
                px[1] = 0;
                px[2] = 0;
                px[3] = 1;
            }
        }
    }

    g_hereActive = false;
    g_hereAlpha = 0;
    KillTimer(hwnd, TIMER_ID_HERE_FADE);

    PresentLayered(hwnd);
}

// ─── 畫布層 → 顯示層 + HERE 疊加 ──────────────────────

void PresentLayered(HWND hWnd)
{
    if (!g_hBitmap || !g_pPixels || g_bmpW <= 0 || g_bmpH <= 0) return;
    if ((int)g_drawBuf.size() < g_bmpW * g_bmpH * 4) return;

    // 1) 畫布層 copy 到顯示層
    memcpy(g_pPixels, g_drawBuf.data(), (size_t)g_bmpW * g_bmpH * 4);

    // 2) HERE 疊到顯示層
    if (g_hereActive && g_hereAlpha > 0) {
        DrawHereOnDisplay((BYTE)g_hereAlpha);
    }

    DrawToolButtonsOnDisplay();

    // 3) UpdateLayeredWindow
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);

    HGDIOBJ oldBmp = SelectObject(hdcMem, g_hBitmap);

    RECT rc;
    GetWindowRect(hWnd, &rc);
    POINT ptDst{ rc.left, rc.top };
    SIZE  size{ g_bmpW, g_bmpH };
    POINT ptSrc{ 0, 0 };

    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(
        hWnd,
        hdcScreen,
        &ptDst,
        &size,
        hdcMem,
        &ptSrc,
        0,
        &bf,
        ULW_ALPHA
    );

    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

// ─── 畫布層畫筆 helper ────────────────────────────────────────

inline void StampPixelDraw(int x, int y, COLORREF color, BYTE alpha)
{
    if (x < 0 || x >= g_bmpW || y < 0 || y >= g_bmpH) return;
    if (g_drawBuf.empty()) return;

    BYTE* px = &g_drawBuf[(y * g_bmpW + x) * 4];
    px[0] = GetBValue(color);
    px[1] = GetGValue(color);
    px[2] = GetRValue(color);
    px[3] = alpha;
}

void StampBrushDraw(int x, int y, COLORREF color, BYTE alpha)
{
    int r = g_penSize / 2;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            StampPixelDraw(x + dx, y + dy, color, alpha);
        }
    }
}

void DrawLineOnDrawBuf(POINT a, POINT b)
{
    if (g_drawBuf.empty() || g_bmpW <= 0 || g_bmpH <= 0) return;

    int dx = abs(b.x - a.x);
    int dy = abs(b.y - a.y);

    int sx = (a.x < b.x) ? 1 : -1;
    int sy = (a.y < b.y) ? 1 : -1;

    int err = dx - dy;
    int x = a.x;
    int y = a.y;

    while (true)
    {
        StampBrushDraw(x, y, g_penColor, 255);

        if (x == b.x && y == b.y)
            break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

void EraseLineOnDrawBuf(POINT a, POINT b)
{
    if (g_drawBuf.empty() || g_bmpW <= 0 || g_bmpH <= 0) return;

    int dx = abs(b.x - a.x);
    int dy = abs(b.y - a.y);

    int sx = (a.x < b.x) ? 1 : -1;
    int sy = (a.y < b.y) ? 1 : -1;

    int err = dx - dy;
    int x = a.x;
    int y = a.y;

    while (true)
    {
        int r = g_penSize / 2;
        for (int dy2 = -r; dy2 <= r; ++dy2) {
            for (int dx2 = -r; dx2 <= r; ++dx2) {
                int ex = x + dx2;
                int ey = y + dy2;
                if (ex < 0 || ex >= g_bmpW || ey < 0 || ey >= g_bmpH) continue;

                BYTE* px = &g_drawBuf[(ey * g_bmpW + ex) * 4];
                px[0] = 0;
                px[1] = 0;
                px[2] = 0;
                px[3] = 1; // 還原為幾乎透明
            }
        }

        if (x == b.x && y == b.y)
            break;

        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x += sx; }
        if (e2 < dx) { err += dx; y += sy; }
    }
}

// ─── HERE 淡出（只畫在顯示層） ─────────────────────────────

void DrawHereOnDisplay(BYTE alpha)
{
    if (!g_pPixels || g_bmpW <= 0 || g_bmpH <= 0) return;

    int boxW = 160;
    int boxH = 60;
    int x0 = (g_bmpW - boxW) / 2;
    int y0 = (g_bmpH - boxH) / 2;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    int x1 = x0 + boxW;
    int y1 = y0 + boxH;
    if (x1 > g_bmpW)  x1 = g_bmpW;
    if (y1 > g_bmpH)  y1 = g_bmpH;

    BYTE a = (alpha == 0 ? 1 : alpha);

    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            BYTE* px = g_pPixels + (y * g_bmpW + x) * 4;
            px[0] = 0;
            px[1] = 0;
            px[2] = 0;
            px[3] = a;
        }
    }

    // 在顯示層上畫 HERE 字樣
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldBmp = SelectObject(hdcMem, g_hBitmap);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdcMem, hFont);

    RECT rcText;
    rcText.left = x0;
    rcText.top = y0;
    rcText.right = x1;
    rcText.bottom = y1;

    DrawTextW(hdcMem, L"HERE", -1, &rcText,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdcMem, oldFont);
    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}
void DrawToolButtonsOnDisplay()
{
    if (!g_pPixels || g_bmpW <= 0 || g_bmpH <= 0) return;

    // 兩顆按鈕大小與位置（靠右下）
    const int btnW = 48;
    const int btnH = 48;
    const int padding = 10;
    const int spacing = 8;

    int penRight = g_bmpW - padding;
    int penLeft = penRight - btnW;
    int penBottom = g_bmpH - padding;
    int penTop = penBottom - btnH;

    int eraserRight = penLeft - spacing;
    int eraserLeft = eraserRight - btnW;
    int eraserBottom = penBottom;
    int eraserTop = penTop;

    if (penLeft < 0 || eraserLeft < 0 || penTop < 0) {
        // 太小就算了
        SetRectEmpty(&g_rcBtnPen);
        SetRectEmpty(&g_rcBtnEraser);
        return;
    }

    g_rcBtnPen.left = penLeft;
    g_rcBtnPen.top = penTop;
    g_rcBtnPen.right = penRight;
    g_rcBtnPen.bottom = penBottom;

    g_rcBtnEraser.left = eraserLeft;
    g_rcBtnEraser.top = eraserTop;
    g_rcBtnEraser.right = eraserRight;
    g_rcBtnEraser.bottom = eraserBottom;

    auto fillRectAlpha = [&](const RECT& r, BYTE rCol, BYTE gCol, BYTE bCol, BYTE aCol)
        {
            int x0 = max(0, r.left);
            int y0 = max(0, r.top);
            int x1 = min(g_bmpW, r.right);
            int y1 = min(g_bmpH, r.bottom);
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    BYTE* px = g_pPixels + (y * g_bmpW + x) * 4;
                    px[0] = bCol;
                    px[1] = gCol;
                    px[2] = rCol;
                    px[3] = aCol;
                }
            }
        };

    // 背景：目前工具高亮，另一顆半透明灰
    BYTE baseA = 160;
    BYTE hoverA = 220;
    BYTE rPen = 0, gPen = 120, bPen = 215; // 藍系
    BYTE rEraser = 80, gEraser = 80, bEraser = 80;  // 灰

    if (g_tool == TOOL_PEN) {
        fillRectAlpha(g_rcBtnPen, rPen, gPen, bPen, hoverA);
        fillRectAlpha(g_rcBtnEraser, rEraser, gEraser, bEraser, baseA);
    }
    else {
        fillRectAlpha(g_rcBtnPen, rEraser, gEraser, bEraser, baseA);
        fillRectAlpha(g_rcBtnEraser, rPen, gPen, bPen, hoverA);
    }

    // 在顯示層上畫 "P" / "E"
    HDC hdcScreen = GetDC(nullptr);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HGDIOBJ oldBmp = SelectObject(hdcMem, g_hBitmap);

    SetBkMode(hdcMem, TRANSPARENT);
    SetTextColor(hdcMem, RGB(255, 255, 255));
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HFONT oldFont = (HFONT)SelectObject(hdcMem, hFont);

    RECT rcP = g_rcBtnPen;
    RECT rcE = g_rcBtnEraser;

    DrawTextW(hdcMem, L"P", -1, &rcP,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(hdcMem, L"E", -1, &rcE,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    SelectObject(hdcMem, oldFont);
    SelectObject(hdcMem, oldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void StartHereMarker()
{
    g_hereActive = true;
    g_hereAlpha = 220;
    PresentLayered(g_mainWnd);
    SetTimer(g_mainWnd, TIMER_ID_HERE_FADE, 33, nullptr); // ~30fps
}

void UpdateHereMarkerFrame()
{
    if (!g_hereActive) return;

    g_hereAlpha -= 15;
    if (g_hereAlpha <= 0) {
        g_hereAlpha = 0;
        g_hereActive = false;
        KillTimer(g_mainWnd, TIMER_ID_HERE_FADE);
    }
    PresentLayered(g_mainWnd);
}

// ─── Tray & Helpers ──────────────────────────────────────────

void AddTrayIcon(HWND hwnd)
{
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(IDI_DESKTOPPAINTER));

    lstrcpyW(g_nid.szTip, L"Desktop Painter (畫圖模式)");
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

void RemoveTrayIcon(HWND)
{
    if (g_nid.cbSize != 0) {
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        ZeroMemory(&g_nid, sizeof(g_nid));
    }
}

void UpdateTrayTip()
{
    if (g_nid.cbSize == 0) return;

    if (g_passthrough)
        lstrcpyW(g_nid.szTip, L"Desktop Painter (穿透模式)");
    else
        lstrcpyW(g_nid.szTip, L"Desktop Painter (畫圖模式)");

    g_nid.uFlags = NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

void UpdatePassthrough(HWND hwnd)
{
    LONG exStyle = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if (g_passthrough)
        exStyle |= WS_EX_TRANSPARENT;
    else
        exStyle &= ~WS_EX_TRANSPARENT;

    SetWindowLongW(hwnd, GWL_EXSTYLE, exStyle);

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
}

void ShowTrayMenu(HWND hwnd)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;

    AppendMenuW(hMenu, MF_STRING, ID_TRAY_SHOW, L"顯示畫布");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_CLEAR, L"清除畫面");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_CONFIG, L"設定...");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_WHERE, L"顯示畫布位置");
    AppendMenuW(hMenu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"結束");

    SetForegroundWindow(hwnd);
    TrackPopupMenu(hMenu,
        TPM_RIGHTBUTTON | TPM_BOTTOMALIGN,
        pt.x, pt.y,
        0, hwnd, NULL);

    DestroyMenu(hMenu);
}

// ─── Config Window Helpers ────────────────────────────────────

void ShowConfigWindow()
{
    if (g_configWnd && IsWindow(g_configWnd)) {
        ShowWindow(g_configWnd, SW_SHOW);
        SetForegroundWindow(g_configWnd);
        return;
    }

    int width = 640;
    int height = 480;

    g_configWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        CONFIG_WINDOW_CLASS,
        L"Desktop Painter 設定",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT,
        width, height,
        g_mainWnd, nullptr,
        hInst, nullptr
    );

    if (g_configWnd) {
        ShowWindow(g_configWnd, SW_SHOW);
        UpdateWindow(g_configWnd);
    }
}

void ComputeMonitorRectsForConfig(HWND hWnd, std::vector<RECT>& outRects)
{
    outRects.clear();
    if (g_monitors.empty()) return;

    RECT client;
    GetClientRect(hWnd, &client);

    int totalW = client.right - client.left;
    int totalH = client.bottom - client.top;

    // 上半部給螢幕縮圖，下半部給設定
    int margin = 10;
    int mapTop = 10;
    int mapBottom = totalH / 2 + 40; // 上半 + 一點 buffer

    if (mapBottom <= mapTop + margin * 2) return;

    int availW = totalW - margin * 2;
    int availH = (mapBottom - mapTop) - margin * 2;
    if (availW <= 0 || availH <= 0) return;

    // 虛擬桌面 bounding box
    int minX = g_monitors[0].rcMonitor.left;
    int minY = g_monitors[0].rcMonitor.top;
    int maxX = g_monitors[0].rcMonitor.right;
    int maxY = g_monitors[0].rcMonitor.bottom;
    for (auto& m : g_monitors) {
        if (m.rcMonitor.left < minX) minX = m.rcMonitor.left;
        if (m.rcMonitor.top < minY) minY = m.rcMonitor.top;
        if (m.rcMonitor.right > maxX) maxX = m.rcMonitor.right;
        if (m.rcMonitor.bottom > maxY) maxY = m.rcMonitor.bottom;
    }

    int vW = maxX - minX;
    int vH = maxY - minY;
    if (vW <= 0 || vH <= 0) return;

    float sx = (float)availW / (float)vW;
    float sy = (float)availH / (float)vH;
    float scale = (sx < sy ? sx : sy);

    int scaledW = (int)(vW * scale);
    int scaledH = (int)(vH * scale);
    int offsetX = margin + (availW - scaledW) / 2;
    int offsetY = mapTop + margin + (availH - scaledH) / 2;

    for (auto& m : g_monitors) {
        RECT r;
        r.left = offsetX + (int)((m.rcMonitor.left - minX) * scale);
        r.top = offsetY + (int)((m.rcMonitor.top - minY) * scale);
        r.right = offsetX + (int)((m.rcMonitor.right - minX) * scale);
        r.bottom = offsetY + (int)((m.rcMonitor.bottom - minY) * scale);
        outRects.push_back(r);
    }
}

// ─── Config WndProc ───────────────────────────────────────────

LRESULT CALLBACK ConfigWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        RefreshMonitors();

        RECT rcClient;
        GetClientRect(hWnd, &rcClient);
        int totalW = rcClient.right - rcClient.left;
        int totalH = rcClient.bottom - rcClient.top;

        // 顏色 group
        int groupColorY = totalH - 160;
        HWND hGroupColor = CreateWindowW(
            L"BUTTON", L"畫筆顏色",
            WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
            10, groupColorY, totalW - 20, 70,
            hWnd, nullptr, hInst, nullptr);

        UNREFERENCED_PARAMETER(hGroupColor);

        int colorX = 20, colorY = groupColorY + 20, colorW = 80, colorH = 20;
        CreateWindowW(L"BUTTON", L"紅色",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | WS_GROUP,
            colorX, colorY, colorW, colorH,
            hWnd, (HMENU)ID_PEN_COLOR_RED, hInst, nullptr);
        CreateWindowW(L"BUTTON", L"綠色",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            colorX + 90, colorY, colorW, colorH,
            hWnd, (HMENU)ID_PEN_COLOR_GREEN, hInst, nullptr);
        CreateWindowW(L"BUTTON", L"藍色",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            colorX + 180, colorY, colorW, colorH,
            hWnd, (HMENU)ID_PEN_COLOR_BLUE, hInst, nullptr);
        CreateWindowW(L"BUTTON", L"黃色",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            colorX, colorY + 25, colorW, colorH,
            hWnd, (HMENU)ID_PEN_COLOR_YELLOW, hInst, nullptr);
        CreateWindowW(L"BUTTON", L"白色",
            WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON,
            colorX + 90, colorY + 25, colorW, colorH,
            hWnd, (HMENU)ID_PEN_COLOR_WHITE, hInst, nullptr);

        int defaultColorId = ID_PEN_COLOR_RED;
        if (g_penColor == RGB(0, 255, 0))          defaultColorId = ID_PEN_COLOR_GREEN;
        else if (g_penColor == RGB(0, 0, 255))     defaultColorId = ID_PEN_COLOR_BLUE;
        else if (g_penColor == RGB(255, 255, 0))   defaultColorId = ID_PEN_COLOR_YELLOW;
        else if (g_penColor == RGB(255, 255, 255)) defaultColorId = ID_PEN_COLOR_WHITE;

        CheckRadioButton(hWnd, PEN_COLOR_FIRST, PEN_COLOR_LAST, defaultColorId);

        // 粗細 Label + Trackbar
        int labelY = totalH - 80;
        int trackY = labelY + 20;
        int trackX = 20;
        int trackW = totalW - 40;

        g_hwndPenSizeLabel = CreateWindowW(
            L"STATIC", L"",
            WS_CHILD | WS_VISIBLE,
            trackX, labelY, trackW, 20,
            hWnd, nullptr, hInst, nullptr);

        wchar_t buf[64];
        wsprintfW(buf, L"畫筆粗細：%d px (1 ~ 32)", g_penSize);
        SetWindowTextW(g_hwndPenSizeLabel, buf);

        g_hwndPenTrack = CreateWindowExW(
            0, TRACKBAR_CLASSW, L"",
            WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
            trackX, trackY, trackW, 30,
            hWnd, nullptr, hInst, nullptr);

        SendMessageW(g_hwndPenTrack, TBM_SETRANGEMIN, FALSE, 1);
        SendMessageW(g_hwndPenTrack, TBM_SETRANGEMAX, FALSE, 32);
        int initPos = g_penSize;
        if (initPos < 1)  initPos = 1;
        if (initPos > 32) initPos = 32;
        SendMessageW(g_hwndPenTrack, TBM_SETPOS, TRUE, initPos);
    }
    break;

    case WM_COMMAND:
    {
        int id = LOWORD(wParam);

        // 顏色
        if (id >= PEN_COLOR_FIRST && id <= PEN_COLOR_LAST) {
            CheckRadioButton(hWnd, PEN_COLOR_FIRST, PEN_COLOR_LAST, id);
            switch (id) {
            case ID_PEN_COLOR_RED:    g_penColor = RGB(255, 0, 0);   break;
            case ID_PEN_COLOR_GREEN:  g_penColor = RGB(0, 255, 0);   break;
            case ID_PEN_COLOR_BLUE:   g_penColor = RGB(0, 0, 255);   break;
            case ID_PEN_COLOR_YELLOW: g_penColor = RGB(255, 255, 0);  break;
            case ID_PEN_COLOR_WHITE:  g_penColor = RGB(255, 255, 255); break;
            }
            return 0;
        }

        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    break;

    case WM_HSCROLL:
    {
        if ((HWND)lParam == g_hwndPenTrack && g_hwndPenTrack) {
            int pos = (int)SendMessageW(g_hwndPenTrack, TBM_GETPOS, 0, 0);
            if (pos < 1)  pos = 1;
            if (pos > 32) pos = 32;
            g_penSize = pos;

            if (g_hwndPenSizeLabel) {
                wchar_t buf[64];
                wsprintfW(buf, L"畫筆粗細：%d px (1 ~ 32)", g_penSize);
                SetWindowTextW(g_hwndPenSizeLabel, buf);
            }
        }
    }
    break;

    case WM_LBUTTONUP:
    {
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        std::vector<RECT> rects;
        ComputeMonitorRectsForConfig(hWnd, rects);

        for (size_t i = 0; i < rects.size(); ++i) {
            if (PtInRect(&rects[i], POINT{ x, y })) {
                ApplyMonitorToMain((int)i);
                g_currentMonitor = (int)i;
                InvalidateRect(hWnd, NULL, TRUE);
                break;
            }
        }
    }
    break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hWnd, &ps);

        std::vector<RECT> rects;
        ComputeMonitorRectsForConfig(hWnd, rects);

        HDC hdcScreen = GetDC(nullptr);

        for (size_t i = 0; i < rects.size(); ++i) {
            RECT r = rects[i];

            // 背景
            HBRUSH brush = CreateSolidBrush(RGB(40, 40, 40));
            FillRect(hdc, &r, brush);
            DeleteObject(brush);

            // 螢幕縮圖
            MonitorInfoSimple& m = g_monitors[i];
            int monW = m.rcMonitor.right - m.rcMonitor.left;
            int monH = m.rcMonitor.bottom - m.rcMonitor.top;
            if (monW > 0 && monH > 0) {
                StretchBlt(
                    hdc,
                    r.left, r.top, r.right - r.left, r.bottom - r.top,
                    hdcScreen,
                    m.rcMonitor.left, m.rcMonitor.top, monW, monH,
                    SRCCOPY
                );
            }

            // 外框
            HPEN pen = CreatePen(
                i == (size_t)g_currentMonitor ? PS_SOLID : PS_DOT,
                i == (size_t)g_currentMonitor ? 3 : 1,
                RGB(0, 120, 215)
            );
            HPEN oldPen = (HPEN)SelectObject(hdc, pen);
            HBRUSH oldBrush = (HBRUSH)SelectObject(hdc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(hdc, r.left, r.top, r.right, r.bottom);
            SelectObject(hdc, oldBrush);
            SelectObject(hdc, oldPen);
            DeleteObject(pen);

            // 編號
            wchar_t text[16];
            wsprintfW(text, L"%d", (int)i + 1);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, text, -1, &r,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        ReleaseDC(nullptr, hdcScreen);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_CLOSE:
        DestroyWindow(hWnd);
        break;

    case WM_DESTROY:
        if (g_configWnd == hWnd)
            g_configWnd = nullptr;
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ─── InitInstance ─────────────────────────────────────────────

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    RefreshMonitors();
    g_currentMonitor = 0;

    RECT rc = g_monitors[g_currentMonitor].rcMonitor;
    int screenW = rc.right - rc.left;
    int screenH = rc.bottom - rc.top;

    g_canvasWidth = screenW;
    g_canvasHeight = screenH;

    HWND hWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TOOLWINDOW,
        szWindowClass,
        szTitle,
        WS_POPUP,
        rc.left, rc.top,
        screenW, screenH,
        nullptr, nullptr,
        hInstance,
        nullptr
    );

    if (!hWnd)
        return FALSE;

    g_mainWnd = hWnd;

    CreateCanvasBitmap(screenW, screenH);
    ClearCanvas(hWnd);

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    AddTrayIcon(hWnd);

    if (!RegisterHotKey(hWnd, 1, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'D')) {
        MessageBoxW(hWnd, L"註冊熱鍵 Ctrl+Alt+D 失敗", L"Hotkey", MB_ICONWARNING);
    }

    g_passthrough = false;
    UpdatePassthrough(hWnd);
    UpdateTrayTip();

    return TRUE;
}

// ─── Main Overlay WndProc ─────────────────────────────────────

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;

        case IDM_EXIT:
        case ID_TRAY_EXIT:
            DestroyWindow(hWnd);
            break;

        case ID_TRAY_SHOW:
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            break;

        case ID_TRAY_CLEAR:
            ClearCanvas(hWnd);
            break;

        case ID_TRAY_CONFIG:
            ShowConfigWindow();
            break;

        case ID_TRAY_WHERE:
            StartHereMarker();
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_ERASEBKGND:
        return 1;

        // 左鍵畫線
    case WM_LBUTTONDOWN:
    {
        POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

        // 先看有沒有點到工具按鈕
        if (PtInRect(&g_rcBtnPen, p)) {
            g_tool = TOOL_PEN;
            PresentLayered(hWnd); // 讓按鈕高亮更新
            break;
        }
        if (PtInRect(&g_rcBtnEraser, p)) {
            g_tool = TOOL_ERASER;
            PresentLayered(hWnd);
            break;
        }

        if (!g_passthrough) {
            g_isDrawing = true;
            SetCapture(hWnd);

            g_lastPoint = p;
            if (g_tool == TOOL_PEN) {
                DrawLineOnDrawBuf(p, p);
            }
            else { // 橡皮擦
                EraseLineOnDrawBuf(p, p);
            }
            PresentLayered(hWnd);
        }
    }
    break;

    case WM_MOUSEMOVE:
        if (!g_passthrough && g_isDrawing) {
            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };

            if (wParam & MK_LBUTTON) {
                if (g_tool == TOOL_PEN) {
                    DrawLineOnDrawBuf(g_lastPoint, p);
                }
                else {
                    EraseLineOnDrawBuf(g_lastPoint, p);
                }
                g_lastPoint = p;
                PresentLayered(hWnd);
            }
            else if (wParam & MK_RBUTTON) {
                // 右鍵一律當橡皮擦（保留原本行為）
                EraseLineOnDrawBuf(g_lastPoint, p);
                g_lastPoint = p;
                PresentLayered(hWnd);
            }
        }
        break;

    case WM_LBUTTONUP:
        if (!g_passthrough && g_isDrawing && !(wParam & MK_RBUTTON)) {
            g_isDrawing = false;
            ReleaseCapture();
        }
        break;

        // 右鍵橡皮擦
    case WM_RBUTTONDOWN:
        if (!g_passthrough) {
            g_isDrawing = true;
            SetCapture(hWnd);

            POINT p = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            g_lastPoint = p;
            EraseLineOnDrawBuf(p, p);
            PresentLayered(hWnd);
        }
        break;

    case WM_RBUTTONUP:
        if (!g_passthrough && g_isDrawing && !(wParam & MK_LBUTTON)) {
            g_isDrawing = false;
            ReleaseCapture();
        }
        break;

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        PresentLayered(hWnd);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_TIMER:
        if (wParam == TIMER_ID_HERE_FADE) {
            UpdateHereMarkerFrame();
        }
        break;

    case WM_TRAYICON:
        if (LOWORD(wParam) == g_nid.uID) {
            if (lParam == WM_RBUTTONUP) {
                ShowTrayMenu(hWnd);
            }
            else if (lParam == WM_LBUTTONUP) {
                ShowWindow(hWnd, SW_SHOW);
                SetForegroundWindow(hWnd);
            }
        }
        break;

    case WM_HOTKEY:
        if (wParam == 1) {
            g_passthrough = !g_passthrough;
            UpdatePassthrough(hWnd);
            UpdateTrayTip();
        }
        return 0;

    case WM_DESTROY:
        KillTimer(hWnd, TIMER_ID_HERE_FADE);
        UnregisterHotKey(hWnd, 1);
        RemoveTrayIcon(hWnd);
        if (g_hBitmap) {
            DeleteObject(g_hBitmap);
            g_hBitmap = nullptr;
            g_pPixels = nullptr;
        }
        g_drawBuf.clear();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ─── About Dialog（模板原樣保留） ───────────────────────────

INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
    UNREFERENCED_PARAMETER(lParam);
    switch (message)
    {
    case WM_INITDIALOG:
        return (INT_PTR)TRUE;

    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
            EndDialog(hDlg, LOWORD(wParam));
            return (INT_PTR)TRUE;
        }
        break;
    }
    return (INT_PTR)FALSE;
}
