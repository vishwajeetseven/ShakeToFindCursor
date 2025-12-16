#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <math.h>
#include <gdiplus.h>
#include <stdio.h>

// Link GDI+
using namespace Gdiplus;

/* --- TUNING --- */
const int WINDOW_SIZE = 300;          // Larger buffer for smoother scaling
const int STROKE_WIDTH = 6;
const int SHAKE_THRESHOLD = 15;
const int SHAKE_DECAY = 5;
const int SHAKE_GROWTH = 35;          // Aggressive growth
const float SMOOTH_FACTOR = 0.35f;    // Snappier response (higher = faster)

/* --- GLOBALS --- */
POINT mLastPt = {0, 0};
int mShakeScore = 0;
int mLastDirX = 0;
int mLastDirY = 0;
float mCurrentScale = 0.0f;

NOTIFYICONDATA nid;
Image* mCustomImage = NULL;

// Global GDI+ Tokens
ULONG_PTR gdiplusToken;

/* --- HELPERS --- */
WCHAR* ToWChar(const char* str) {
    int size = MultiByteToWideChar(CP_ACP, 0, str, -1, NULL, 0);
    WCHAR* wstr = new WCHAR[size];
    MultiByteToWideChar(CP_ACP, 0, str, -1, wstr, size);
    return wstr;
}

void SelectCustomImage(HWND hwnd) {
    OPENFILENAME ofn;
    char szFile[260] = {0};
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = szFile;
    ofn.nMaxFile = sizeof(szFile);
    ofn.lpstrFilter = "Images (*.png;*.ico;*.cur)\0*.png;*.ico;*.cur\0All Files (*.*)\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;

    if (GetOpenFileName(&ofn) == TRUE) {
        if (mCustomImage) { delete mCustomImage; mCustomImage = NULL; }
        WCHAR* wFileName = ToWChar(szFile);
        mCustomImage = new Image(wFileName);
        delete[] wFileName;
        if (mCustomImage->GetLastStatus() != Ok) {
            MessageBox(hwnd, "Failed to load image.", "Error", MB_ICONERROR);
            delete mCustomImage; mCustomImage = NULL;
        }
    }
}

/* --- THE MAGIC: HARDWARE ACCELERATED DRAWING --- */
void UpdateLayeredWindowContent(HWND hwnd, POINT ptPos, float scale) {
    // 1. Setup Memory DC (Off-screen drawing board)
    HDC hdcScreen = GetDC(NULL);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    
    // 2. Create a Bitmap for the Window
    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = WINDOW_SIZE;
    bmi.bmiHeader.biHeight = WINDOW_SIZE;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32; // 32-bit for Transparency (Alpha)
    bmi.bmiHeader.biCompression = BI_RGB;

    void* pBits = NULL;
    HBITMAP hBitmap = CreateDIBSection(hdcScreen, &bmi, DIB_RGB_COLORS, &pBits, NULL, 0);
    HGDIOBJ oldBitmap = SelectObject(hdcMem, hBitmap);

    // 3. Initialize GDI+ on this Memory DC
    Graphics graphics(hdcMem);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    graphics.SetInterpolationMode(InterpolationModeHighQualityBicubic);

    // 4. Draw! (Everything here is drawn to memory, not screen yet)
    if (scale > 0.01f) {
        float drawSize = (float)(WINDOW_SIZE * scale);
        float offset = (WINDOW_SIZE - drawSize) / 2.0f;

        if (mCustomImage) {
            graphics.DrawImage(mCustomImage, offset, offset, drawSize, drawSize);
        } else {
            // Draw Red Circle
            Pen redPen(Color(255, 255, 0, 0), (REAL)STROKE_WIDTH);
            float pad = (float)STROKE_WIDTH;
            graphics.DrawEllipse(&redPen, offset + pad, offset + pad, drawSize - (pad*2), drawSize - (pad*2));
        }
    }

    // 5. Push to Screen using UpdateLayeredWindow (Instant Update)
    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {WINDOW_SIZE, WINDOW_SIZE};
    BLENDFUNCTION blend = {0};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA; // Use per-pixel alpha

    // This function updates Position AND Content in one atomic operation (No tearing/lag)
    // We adjust position to center it on the mouse
    POINT finalPos;
    finalPos.x = ptPos.x - (WINDOW_SIZE / 2);
    finalPos.y = ptPos.y - (WINDOW_SIZE / 2);

    UpdateLayeredWindow(hwnd, hdcScreen, &finalPos, &sizeWnd, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);

    // 6. Cleanup
    SelectObject(hdcMem, oldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(hdcMem);
    ReleaseDC(NULL, hdcScreen);
}

/* --- WINDOW PROCEDURE --- */
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            // High-speed timer (10ms ~ 100fps target)
            SetTimer(hwnd, 1, 10, NULL); 
            
            // Tray Setup
            ZeroMemory(&nid, sizeof(nid));
            nid.cbSize = sizeof(NOTIFYICONDATA);
            nid.hWnd = hwnd;
            nid.uID = 1;
            nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            nid.uCallbackMessage = WM_USER + 1;
            nid.hIcon = LoadIcon(NULL, IDC_ARROW);
            strcpy(nid.szTip, "Smooth Shake Cursor");
            Shell_NotifyIcon(NIM_ADD, &nid);
            return 0;

        case WM_USER + 1: // Tray Click
            if (lParam == WM_RBUTTONUP) {
                POINT p; GetCursorPos(&p);
                SetForegroundWindow(hwnd);
                HMENU hMenu = CreatePopupMenu();
                AppendMenu(hMenu, MF_STRING, 1002, "Select Image...");
                AppendMenu(hMenu, MF_STRING, 1003, "Reset to Red Circle");
                AppendMenu(hMenu, MF_SEPARATOR, 0, NULL);
                AppendMenu(hMenu, MF_STRING, 1001, "Exit");
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN, p.x, p.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            return 0;

        case WM_COMMAND:
            if (LOWORD(wParam) == 1001) SendMessage(hwnd, WM_CLOSE, 0, 0);
            if (LOWORD(wParam) == 1002) SelectCustomImage(hwnd);
            if (LOWORD(wParam) == 1003) { if(mCustomImage) { delete mCustomImage; mCustomImage=NULL; } }
            return 0;

        case WM_TIMER: {
            POINT curPt; GetCursorPos(&curPt);

            // Physics
            int dx = curPt.x - mLastPt.x;
            int dy = curPt.y - mLastPt.y;
            bool shaking = false;
            
            if (abs(dx) > SHAKE_THRESHOLD) {
                int dirX = (dx > 0) ? 1 : -1;
                if (dirX != mLastDirX && mLastDirX != 0) shaking = true;
                mLastDirX = dirX;
            }
            if (abs(dy) > SHAKE_THRESHOLD) {
                int dirY = (dy > 0) ? 1 : -1;
                if (dirY != mLastDirY && mLastDirY != 0) shaking = true;
                mLastDirY = dirY;
            }

            if (shaking) mShakeScore += SHAKE_GROWTH;
            else mShakeScore -= SHAKE_DECAY;

            if (mShakeScore > 100) mShakeScore = 100;
            if (mShakeScore < 0) mShakeScore = 0;

            float targetScale = (float)mShakeScore / 100.0f;
            mCurrentScale += (targetScale - mCurrentScale) * SMOOTH_FACTOR;

            if (mCurrentScale < 0.01f) mCurrentScale = 0;

            // Update Screen ONLY if visible or just became invisible
            static bool wasVisible = false;
            if (mCurrentScale > 0.01f) {
                if (!wasVisible) {
                    ShowWindow(hwnd, SW_SHOWNA);
                    wasVisible = true;
                }
                UpdateLayeredWindowContent(hwnd, curPt, mCurrentScale);
            } else {
                if (wasVisible) {
                    ShowWindow(hwnd, SW_HIDE);
                    wasVisible = false;
                }
            }

            mLastPt = curPt;
            return 0;
        }

        case WM_DESTROY:
            Shell_NotifyIcon(NIM_DELETE, &nid);
            if (mCustomImage) delete mCustomImage;
            PostQuitMessage(0);
            return 0;
            
        default: return DefWindowProc(hwnd, message, wParam, lParam);
    }
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR args, int nCmdShow) {
    // 1. GDI+ Startup
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    // 2. Register Class
    WNDCLASSEX wc = {0};
    wc.cbSize = sizeof(WNDCLASSEX);
    wc.lpfnWndProc = WindowProcedure;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(NULL_BRUSH);
    wc.lpszClassName = "ProShakeClass";
    RegisterClassEx(&wc);

    // 3. Create Layered Window (The magic style: WS_EX_LAYERED)
    HWND hwnd = CreateWindowEx(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        "ProShakeClass", "ProShake", WS_POPUP,
        0, 0, WINDOW_SIZE, WINDOW_SIZE,
        NULL, NULL, hInst, NULL
    );

    GetCursorPos(&mLastPt);

    MSG msg = {0};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(gdiplusToken);
    return 0;
}