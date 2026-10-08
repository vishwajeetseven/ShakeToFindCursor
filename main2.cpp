// main.cpp
// Shake to Find Cursor - enhanced version
//
// Build (MinGW / Dev-C++):
//   Project -> Project Options -> Parameters -> Linker:
//       -lgdiplus -lcomdlg32 -lshell32
//
// Build (MSVC):
//   cl /EHsc /DUNICODE /D_UNICODE main.cpp user32.lib gdi32.lib gdiplus.lib comdlg32.lib shell32.lib

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <string>

#ifdef _MSC_VER
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "shell32.lib")
#endif

using namespace Gdiplus;

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const int      WINDOW_SIZE     = 300;              // Render buffer size (px)
static const UINT     TIMER_INTERVAL  = 10;               // ~100 FPS
static const UINT_PTR TIMER_ID        = 1;
static const UINT     TRAY_ICON_ID    = 1;
static const UINT     WM_TRAY_MSG     = WM_USER + 1;

enum MenuId : UINT {
    IDM_TOGGLE_ENABLED  = 1000,
    IDM_SELECT_IMAGE    = 1001,
    IDM_RESET_IMAGE     = 1002,
    IDM_RELOAD_SETTINGS = 1003,
    IDM_OPEN_SETTINGS   = 1004,
    IDM_EXIT            = 1005,
};

// ---------------------------------------------------------------------------
// Settings (persisted to ShakeCursor.ini next to the .exe)
// ---------------------------------------------------------------------------
struct AppSettings {
    int         shakeThreshold = 15;
    int         shakeDecay     = 5;
    int         shakeGrowth    = 35;
    float       smoothFactor   = 0.35f;
    int         strokeWidth    = 6;
    DWORD       ringColorARGB  = 0xFFFF0000; // AARRGGBB, default opaque red
    bool        enabled        = true;
    std::wstring imagePath;                  // empty => red ring
};

static AppSettings g_settings;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static POINT  g_lastPt            = {0, 0};
static int    g_shakeScore        = 0;
static int    g_lastDirX          = 0;
static int    g_lastDirY          = 0;
static float  g_currentScale      = 0.0f;
static float  g_lastRenderedScale = -1.0f;
static POINT  g_lastRenderPt      = {INT_MIN, INT_MIN};
static bool   g_wasVisible        = false;

static LARGE_INTEGER g_qpcFreq    = {};
static LARGE_INTEGER g_qpcLast    = {};

static NOTIFYICONDATAW g_nid      = {};
static UINT            g_wmTaskbarCreated = 0;
static Image*          g_pCustomImage     = nullptr;

static HDC     g_hdcMem       = nullptr;
static HBITMAP g_hBitmap      = nullptr;
static void*   g_pBits        = nullptr;
static Bitmap* g_pBackBuffer  = nullptr;

static ULONG_PTR g_gdiplusToken = 0;
static HWND      g_hwnd         = nullptr;

// ---------------------------------------------------------------------------
// INI helpers
// ---------------------------------------------------------------------------
static std::wstring GetIniPath() {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    size_t pos = s.find_last_of(L"\\/");
    if (pos != std::wstring::npos) s.resize(pos + 1);
    s += L"ShakeCursor.ini";
    return s;
}

static float ReadFloat(const std::wstring& ini, const wchar_t* section,
                       const wchar_t* key, float def) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(section, key, L"", buf, 64, ini.c_str());
    if (buf[0] == 0) return def;
    return (float)wcstod(buf, nullptr);
}

static void WriteFloat(const std::wstring& ini, const wchar_t* section,
                       const wchar_t* key, float value) {
    int whole = (int)value;
    int frac  = (int)((value - (float)whole) * 1000.0f + 0.5f);
    if (frac < 0) frac = -frac;
    if (frac > 999) frac = 999;
    wchar_t buf[32];
    wsprintfW(buf, L"%d.%03d", whole, frac);
    WritePrivateProfileStringW(section, key, buf, ini.c_str());
}

static void LoadSettings() {
    std::wstring ini = GetIniPath();

    g_settings.shakeThreshold = GetPrivateProfileIntW(L"Physics", L"ShakeThreshold", 15, ini.c_str());
    g_settings.shakeDecay     = GetPrivateProfileIntW(L"Physics", L"ShakeDecay",      5, ini.c_str());
    g_settings.shakeGrowth    = GetPrivateProfileIntW(L"Physics", L"ShakeGrowth",    35, ini.c_str());
    g_settings.smoothFactor   = ReadFloat(ini, L"Physics", L"SmoothFactor", 0.35f);
    g_settings.strokeWidth    = GetPrivateProfileIntW(L"Visual",  L"StrokeWidth",     6, ini.c_str());
    g_settings.ringColorARGB  = (DWORD)GetPrivateProfileIntW(L"Visual", L"RingColorARGB",
                                                             (int)0xFFFF0000, ini.c_str());
    g_settings.enabled        = GetPrivateProfileIntW(L"General", L"Enabled", 1, ini.c_str()) != 0;

    wchar_t buf[MAX_PATH] = {};
    GetPrivateProfileStringW(L"General", L"ImagePath", L"", buf, MAX_PATH, ini.c_str());
    g_settings.imagePath = buf;

    // Sanity clamps
    if (g_settings.shakeThreshold < 1) g_settings.shakeThreshold = 1;
    if (g_settings.shakeDecay     < 0) g_settings.shakeDecay     = 0;
    if (g_settings.shakeGrowth    < 1) g_settings.shakeGrowth    = 1;
    if (g_settings.smoothFactor <= 0.0f || g_settings.smoothFactor > 1.0f)
        g_settings.smoothFactor = 0.35f;
    if (g_settings.strokeWidth < 1) g_settings.strokeWidth = 1;
}

static void SaveSettings() {
    std::wstring ini = GetIniPath();
    wchar_t buf[32];

    wsprintfW(buf, L"%d", g_settings.shakeThreshold);
    WritePrivateProfileStringW(L"Physics", L"ShakeThreshold", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_settings.shakeDecay);
    WritePrivateProfileStringW(L"Physics", L"ShakeDecay", buf, ini.c_str());
    wsprintfW(buf, L"%d", g_settings.shakeGrowth);
    WritePrivateProfileStringW(L"Physics", L"ShakeGrowth", buf, ini.c_str());
    WriteFloat(ini, L"Physics", L"SmoothFactor", g_settings.smoothFactor);

    wsprintfW(buf, L"%d", g_settings.strokeWidth);
    WritePrivateProfileStringW(L"Visual", L"StrokeWidth", buf, ini.c_str());
    wsprintfW(buf, L"%d", (int)g_settings.ringColorARGB);
    WritePrivateProfileStringW(L"Visual", L"RingColorARGB", buf, ini.c_str());

    WritePrivateProfileStringW(L"General", L"Enabled",
        g_settings.enabled ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"General", L"ImagePath",
        g_settings.imagePath.c_str(), ini.c_str());
}

// ---------------------------------------------------------------------------
// GDI+ / DIB resources (allocated once, reused every frame)
// ---------------------------------------------------------------------------
static bool InitOverlayResources() {
    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return false;

    g_hdcMem = CreateCompatibleDC(hdcScreen);
    if (!g_hdcMem) { ReleaseDC(nullptr, hdcScreen); return false; }

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = WINDOW_SIZE;
    bmi.bmiHeader.biHeight      = -WINDOW_SIZE;  // top-down (negative height)
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    g_hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &g_pBits, nullptr, 0);
    ReleaseDC(nullptr, hdcScreen);
    if (!g_hBitmap || !g_pBits) return false;

    SelectObject(g_hdcMem, g_hBitmap);

    // Wrap the DIB bits in a GDI+ Bitmap so alpha is written correctly.
    // PARGB is what UpdateLayeredWindow(ULW_ALPHA) expects.
    g_pBackBuffer = new Bitmap(WINDOW_SIZE, WINDOW_SIZE, WINDOW_SIZE * 4,
                               PixelFormat32bppPARGB, (BYTE*)g_pBits);
    if (!g_pBackBuffer || g_pBackBuffer->GetLastStatus() != Ok) {
        FreeOverlayResources();
        return false;
    }
    return true;
}

static void FreeOverlayResources() {
    if (g_pBackBuffer) { delete g_pBackBuffer; g_pBackBuffer = nullptr; }
    if (g_hBitmap)     { DeleteObject(g_hBitmap); g_hBitmap = nullptr; }
    if (g_hdcMem)      { DeleteDC(g_hdcMem); g_hdcMem = nullptr; }
    g_pBits = nullptr;
}

// ---------------------------------------------------------------------------
// Custom image loading
// ---------------------------------------------------------------------------
static bool LoadCustomImage(const std::wstring& path) {
    if (g_pCustomImage) { delete g_pCustomImage; g_pCustomImage = nullptr; }
    if (path.empty()) return true;

    Image* img = new Image(path.c_str());
    if (img->GetLastStatus() != Ok) {
        delete img;
        return false;
    }
    g_pCustomImage = img;
    return true;
}

static bool PromptForImage(HWND hwnd, std::wstring& outPath) {
    wchar_t szFile[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize  = sizeof(ofn);
    ofn.hwndOwner    = hwnd;
    ofn.lpstrFile    = szFile;
    ofn.nMaxFile     = MAX_PATH;
    ofn.lpstrFilter  = L"Images (*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.cur)\0"
                       L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.ico;*.cur\0"
                       L"All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags        = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (!GetOpenFileNameW(&ofn)) return false;
    outPath = szFile;
    return true;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
static void RenderOverlay(POINT ptPos, float scale) {
    if (!g_pBackBuffer) return;

    Graphics g(g_pBackBuffer);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetInterpolationMode(InterpolationModeHighQualityBicubic);
    g.SetPixelOffsetMode(PixelOffsetModeHighQuality);
    g.Clear(Color(0, 0, 0, 0));  // fully transparent

    if (scale > 0.01f) {
        const float drawSize = (float)WINDOW_SIZE * scale;
        const float offset   = ((float)WINDOW_SIZE - drawSize) * 0.5f;

        if (g_pCustomImage && g_pCustomImage->GetLastStatus() == Ok) {
            float iw = (float)g_pCustomImage->GetWidth();
            float ih = (float)g_pCustomImage->GetHeight();
            if (iw > 0.0f && ih > 0.0f) {
                float ratio = (std::min)(drawSize / iw, drawSize / ih);
                float w = iw * ratio;
                float h = ih * ratio;
                g.DrawImage(g_pCustomImage,
                            offset + (drawSize - w) * 0.5f,
                            offset + (drawSize - h) * 0.5f,
                            w, h);
            }
        } else {
            BYTE a = (BYTE)((g_settings.ringColorARGB >> 24) & 0xFF);
            BYTE r = (BYTE)((g_settings.ringColorARGB >> 16) & 0xFF);
            BYTE gch = (BYTE)((g_settings.ringColorARGB >> 8) & 0xFF);
            BYTE b = (BYTE)( g_settings.ringColorARGB        & 0xFF);

            Pen pen(Color(a, r, gch, b), (REAL)g_settings.strokeWidth);
            float pad = (float)g_settings.strokeWidth;
            g.DrawEllipse(&pen,
                          offset + pad, offset + pad,
                          drawSize - pad * 2.0f, drawSize - pad * 2.0f);
        }
    }

    POINT ptSrc    = {0, 0};
    SIZE  sizeWnd  = {WINDOW_SIZE, WINDOW_SIZE};
    BLENDFUNCTION blend = {};
    blend.BlendOp             = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat         = AC_SRC_ALPHA;

    POINT finalPos = { ptPos.x - WINDOW_SIZE / 2, ptPos.y - WINDOW_SIZE / 2 };

    UpdateLayeredWindow(g_hwnd, nullptr, &finalPos, &sizeWnd,
                        g_hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
}

// ---------------------------------------------------------------------------
// Tray icon
// ---------------------------------------------------------------------------
static void AddTrayIcon() {
    g_nid.cbSize           = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = TRAY_ICON_ID;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY_MSG;
    g_nid.hIcon            = LoadIconW(nullptr, IDI_APPLICATION);
    lstrcpynW(g_nid.szTip, L"Shake to Find Cursor", ARRAYSIZE(g_nid.szTip));
    Shell_NotifyIconW(NIM_ADD, &g_nid);
}

// ---------------------------------------------------------------------------
// Per-frame update
// ---------------------------------------------------------------------------
static void UpdateFrame(HWND hwnd) {
    POINT curPt;
    GetCursorPos(&curPt);

    // Delta time (QPC, high resolution)
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    float dt = (float)((double)(now.QuadPart - g_qpcLast.QuadPart)
                       / (double)g_qpcFreq.QuadPart);
    g_qpcLast = now;
    if (dt <= 0.0f) dt = 0.001f;
    if (dt > 0.1f)  dt = 0.1f;

    // Disabled? Keep everything parked.
    if (!g_settings.enabled) {
        if (g_wasVisible) { ShowWindow(hwnd, SW_HIDE); g_wasVisible = false; }
        g_currentScale = 0.0f;
        g_shakeScore   = 0;
        g_lastPt       = curPt;
        return;
    }

    // -------- Shake detection (direction reversal) --------
    int dx = curPt.x - g_lastPt.x;
    int dy = curPt.y - g_lastPt.y;
    bool shaking = false;

    if (abs(dx) > g_settings.shakeThreshold) {
        int dirX = (dx > 0) ? 1 : -1;
        if (dirX != g_lastDirX && g_lastDirX != 0) shaking = true;
        g_lastDirX = dirX;
    }
    if (abs(dy) > g_settings.shakeThreshold) {
        int dirY = (dy > 0) ? 1 : -1;
        if (dirY != g_lastDirY && g_lastDirY != 0) shaking = true;
        g_lastDirY = dirY;
    }

    if (shaking) g_shakeScore += g_settings.shakeGrowth;
    else         g_shakeScore -= g_settings.shakeDecay;

    if (g_shakeScore > 100) g_shakeScore = 100;
    if (g_shakeScore < 0)   g_shakeScore = 0;

    // -------- Smooth towards target (frame-rate independent) --------
    float targetScale = (float)g_shakeScore / 100.0f;
    float alpha = 1.0f - powf(1.0f - g_settings.smoothFactor, dt * 100.0f);
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    g_currentScale += (targetScale - g_currentScale) * alpha;

    if (g_currentScale < 0.005f) g_currentScale = 0.0f;

    // -------- Show / hide / redraw --------
    bool posChanged   = (curPt.x != g_lastRenderPt.x || curPt.y != g_lastRenderPt.y);
    bool scaleChanged = fabsf(g_currentScale - g_lastRenderedScale) > 0.002f;

    if (g_currentScale > 0.005f) {
        bool justShown = false;
        if (!g_wasVisible) {
            ShowWindow(hwnd, SW_SHOWNA);
            g_wasVisible = true;
            justShown = true;
        }
        if (posChanged || scaleChanged || justShown) {
            RenderOverlay(curPt, g_currentScale);
            g_lastRenderPt      = curPt;
            g_lastRenderedScale = g_currentScale;
        }
    } else {
        if (g_wasVisible) {
            ShowWindow(hwnd, SW_HIDE);
            g_wasVisible = false;
        }
    }

    g_lastPt = curPt;
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    // Explorer restarted? Re-add tray icon.
    if (g_wmTaskbarCreated != 0 && msg == g_wmTaskbarCreated) {
        AddTrayIcon();
        return 0;
    }

    switch (msg) {
        case WM_CREATE:
            AddTrayIcon();
            SetTimer(hwnd, TIMER_ID, TIMER_INTERVAL, nullptr);
            QueryPerformanceFrequency(&g_qpcFreq);
            QueryPerformanceCounter(&g_qpcLast);
            GetCursorPos(&g_lastPt);
            return 0;

        case WM_TRAY_MSG:
            switch (LOWORD(lParam)) {
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    POINT p;
                    GetCursorPos(&p);
                    SetForegroundWindow(hwnd);

                    HMENU hMenu = CreatePopupMenu();
                    UINT enFlags = MF_STRING | (g_settings.enabled ? MF_CHECKED : 0);
                    AppendMenuW(hMenu, enFlags,           IDM_TOGGLE_ENABLED,  L"Enabled");
                    AppendMenuW(hMenu, MF_SEPARATOR,      0,                    nullptr);
                    AppendMenuW(hMenu, MF_STRING,         IDM_SELECT_IMAGE,    L"Select Image...");
                    AppendMenuW(hMenu, MF_STRING,         IDM_RESET_IMAGE,     L"Reset to Red Circle");
                    AppendMenuW(hMenu, MF_SEPARATOR,      0,                    nullptr);
                    AppendMenuW(hMenu, MF_STRING,         IDM_RELOAD_SETTINGS,L"Reload Settings");
                    AppendMenuW(hMenu, MF_STRING,         IDM_OPEN_SETTINGS,  L"Open Settings File");
                    AppendMenuW(hMenu, MF_SEPARATOR,      0,                    nullptr);
                    AppendMenuW(hMenu, MF_STRING,         IDM_EXIT,            L"Exit");

                    TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_RIGHTALIGN,
                                   p.x, p.y, 0, hwnd, nullptr);
                    DestroyMenu(hMenu);
                    PostMessage(hwnd, WM_NULL, 0, 0);
                    return 0;
                }
                case WM_LBUTTONDBLCLK:
                    g_settings.enabled = !g_settings.enabled;
                    SaveSettings();
                    return 0;
            }
            return 0;

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDM_TOGGLE_ENABLED:
                    g_settings.enabled = !g_settings.enabled;
                    SaveSettings();
                    break;

                case IDM_SELECT_IMAGE: {
                    std::wstring path;
                    if (PromptForImage(hwnd, path)) {
                        if (LoadCustomImage(path)) {
                            g_settings.imagePath = path;
                            SaveSettings();
                        } else {
                            MessageBoxW(hwnd, L"Failed to load image.",
                                        L"Error", MB_ICONERROR);
                        }
                    }
                    break;
                }

                case IDM_RESET_IMAGE:
                    LoadCustomImage(L"");
                    g_settings.imagePath.clear();
                    SaveSettings();
                    break;

                case IDM_RELOAD_SETTINGS:
                    LoadSettings();
                    LoadCustomImage(g_settings.imagePath);
                    break;

                case IDM_OPEN_SETTINGS:
                    ShellExecuteW(nullptr, L"open", L"notepad.exe",
                                  GetIniPath().c_str(), nullptr, SW_SHOW);
                    break;

                case IDM_EXIT:
                    DestroyWindow(hwnd);
                    break;
            }
            return 0;

        case WM_TIMER:
            if (wParam == TIMER_ID) {
                UpdateFrame(hwnd);
                return 0;
            }
            break;

        case WM_DESTROY:
            KillTimer(hwnd, TIMER_ID);
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            if (g_pCustomImage) { delete g_pCustomImage; g_pCustomImage = nullptr; }
            FreeOverlayResources();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
    // 1. DPI awareness (prevents blurry / misaligned overlay on high-DPI monitors)
    SetProcessDPIAware();

    // 2. Single-instance guard
    HANDLE hMutex = CreateMutexW(nullptr, FALSE, L"ShakeCursor_SingleInstance_v1");
    if (hMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }

    // 3. GDI+ startup
    GdiplusStartupInput gpInput;
    if (GdiplusStartup(&g_gdiplusToken, &gpInput, nullptr) != Ok) {
        return 1;
    }

    // 4. Load settings + image
    LoadSettings();
    if (!g_settings.imagePath.empty()) {
        if (!LoadCustomImage(g_settings.imagePath)) {
            // Bad path in INI - forget it so we don't keep failing.
            g_settings.imagePath.clear();
            SaveSettings();
        }
    }

    // 5. Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = L"ProShakeClass";
    if (!RegisterClassExW(&wc)) {
        GdiplusShutdown(g_gdiplusToken);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 6. Create layered, click-through, top-most, non-activating window
    g_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
        WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        L"ProShakeClass", L"ProShake",
        WS_POPUP,
        0, 0, WINDOW_SIZE, WINDOW_SIZE,
        nullptr, nullptr, hInst, nullptr);

    if (!g_hwnd || !InitOverlayResources()) {
        if (g_hwnd) DestroyWindow(g_hwnd);
        GdiplusShutdown(g_gdiplusToken);
        if (hMutex) CloseHandle(hMutex);
        return 1;
    }

    // 7. Track "TaskbarCreated" so the tray icon survives Explorer restarts
    g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    // 8. Message loop
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // 9. Cleanup
    GdiplusShutdown(g_gdiplusToken);
    if (hMutex) CloseHandle(hMutex);
    return 0;
}
