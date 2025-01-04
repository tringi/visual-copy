#include <Windows.h>
#include <WindowsX.h>
#include <VersionHelpers.h>
#include <MMsystem.h>
#include <DwmAPI.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <math.h>

#pragma warning (disable:6053) // snwprintf may not NUL-terminate
#pragma warning (disable:26819) // unannotated fall-through

USHORT WM_Terminate = WM_NULL;
USHORT WM_TaskbarCreated = WM_NULL;

ATOM aTray;
ATOM aEffect;
HKEY hKeySettings;
HKEY hKeyDWM;
HWND hWndOverlay;
HMENU hMenu;
HHOOK hDlgPosHook;
COLORREF crCustomSet [16];

DWORD progress = 0;

extern "C" IMAGE_DOS_HEADER __ImageBase;
const wchar_t * szInfo [9];

UINT (WINAPI * pfnGetDpiForSystem) () = NULL;
UINT (WINAPI * pfnGetDpiForWindow) (HWND) = NULL;

NOTIFYICONDATA nidTray = {
    sizeof (NOTIFYICONDATA), NULL, 1,
    NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_STATE | NIF_SHOWTIP, WM_USER, NULL, { 0 },
    0u, 0u, { 0 }, { NOTIFYICON_VERSION_4 }, { 0 }, 0, { 0,0,0,{0} }, NULL
};

template <typename P>
bool Symbol (HMODULE h, P & pointer, const char * name) {
    if (P p = reinterpret_cast <P> (GetProcAddress (h, name))) {
        pointer = p;
        return true;
    } else
        return false;
}

bool InitWndClasses (ATOM & main, ATOM & lamp);
void InitVersionInfoStrings ();
void InitTerminationMessage ();
void InitSingleInstance ();
void InitRegistryKey ();
UINT GetDPI (HWND hWnd);
void TrackMenu (HWND hWnd, WPARAM wParam);
BOOL SetPrivilege (LPCTSTR lpszPrivilege, bool enable);
DWORD RegGetSettingsValue (const wchar_t * name, DWORD def = 0);
void RegSetSettingsValue (const wchar_t * name, DWORD value);
void Optimize ();

LRESULT CALLBACK Tray (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK Hook (int code, WPARAM wParam, LPARAM lParam);

void Main () {
    InitVersionInfoStrings ();
    InitTerminationMessage ();
    InitSingleInstance ();

    SetErrorMode (SEM_FAILCRITICALERRORS);
    SetProcessDEPPolicy (PROCESS_DEP_ENABLE | PROCESS_DEP_DISABLE_ATL_THUNK_EMULATION);

    InitRegistryKey ();
    RegOpenKeyEx (HKEY_CURRENT_USER, L"SOFTWARE\\Microsoft\\Windows\\DWM", 0, KEY_QUERY_VALUE, &hKeyDWM);

    if (hMenu = LoadMenu (reinterpret_cast <HINSTANCE> (&__ImageBase), MAKEINTRESOURCE (1))) {
        hMenu = GetSubMenu (hMenu, 0);
    }
    if (hMenu) {
        if (auto about = GetSubMenu (hMenu, GetMenuItemCount (hMenu) - 4)) {
            DeleteMenu (about, -1, 0);

            wchar_t text [32];
            _snwprintf (text, 32, L"%s %s", szInfo [6], szInfo [4]);

            AppendMenu (about, MF_DISABLED, -1, text);
            AppendMenu (about, 0, IDHELP, szInfo [3]);
            SetMenuDefaultItem (about, 0, TRUE);
        }
    } else {
        ExitProcess (ERROR_FILE_CORRUPT);
    }

    if (HMODULE hUser32 = GetModuleHandle (L"USER32")) {
        Symbol (hUser32, pfnGetDpiForSystem, "GetDpiForSystem");
        Symbol (hUser32, pfnGetDpiForWindow, "GetDpiForWindow");
    }

    WM_TaskbarCreated = RegisterWindowMessage (TEXT ("TaskbarCreated"));
    ChangeWindowMessageFilter (WM_TaskbarCreated, MSGFLT_ADD);
    ChangeWindowMessageFilter (WM_Terminate, MSGFLT_ADD);

    hDlgPosHook = SetWindowsHookEx (WH_CALLWNDPROCRET, Hook, NULL, GetCurrentThreadId ());

    SetLastError (0);
    if (InitWndClasses (aTray, aEffect)) {
        InitCommonControls ();

        if (CreateWindow ((LPCTSTR) (std::intptr_t) aTray, L"", WS_POPUP, 0, 0, 0, 0, HWND_DESKTOP, NULL, NULL, NULL)) {
            Optimize ();

            MSG message {};
            while (GetMessage (&message, NULL, 0u, 0u)) {
                DispatchMessage (&message);
            }
            ExitProcess ((UINT) message.wParam);
        }
    }
    ExitProcess (GetLastError ());
}

bool InitWndClasses (ATOM & main, ATOM & effect) {
    WNDCLASSEX wndclass = {
        sizeof (WNDCLASSEX), 0,
        Tray, 0, 0, reinterpret_cast <HINSTANCE> (&__ImageBase),
        NULL, NULL, NULL, NULL, szInfo [1], NULL
    };
    main = RegisterClassEx (&wndclass); 

    wndclass.lpszClassName = szInfo [0];
    wndclass.lpfnWndProc = DefWindowProc;
    wndclass.hCursor = NULL;

    effect = RegisterClassEx (&wndclass);
    return main && effect;
}

void InitVersionInfoStrings () {
    if (HRSRC hRsrc = FindResource (NULL, MAKEINTRESOURCE (1), RT_VERSION)) {
        if (HGLOBAL hGlobal = LoadResource (NULL, hRsrc)) {
            auto data = LockResource (hGlobal);
            auto size = SizeofResource (NULL, hRsrc);

            if (data && (size >= 92)) {
                struct Header {
                    WORD wLength;
                    WORD wValueLength;
                    WORD wType;
                };

                // StringFileInfo
                //  - not searching, leap of faith that the layout is stable

                auto pstrings = static_cast <const unsigned char *> (data) + 76
                              + reinterpret_cast <const Header *> (data)->wValueLength;
                auto p = reinterpret_cast <const wchar_t *> (pstrings) + 12;
                auto e = p + reinterpret_cast <const Header *> (pstrings)->wLength / 2 - 12;
                auto i = 0u;

                const Header * header = nullptr;
                do {
                    header = reinterpret_cast <const Header *> (p);
                    auto length = header->wLength / 2;

                    if (header->wValueLength) {
                        szInfo [i++] = p + length - header->wValueLength;
                    } else {
                        szInfo [i++] = L"";
                    }

                    p += length;
                    if (length % 2) {
                        ++p;
                    }
                } while ((p < e) && (i < sizeof szInfo / sizeof szInfo [0]) && header->wLength);

                if (i == sizeof szInfo / sizeof szInfo [0])
                    return;
            }
        }
    }
    ExitProcess (ERROR_FILE_CORRUPT);
}

void InitTerminationMessage () {
    WM_Terminate = RegisterWindowMessage (szInfo [0]);

    bool terminate = false;
    DWORD recipients = BSM_APPLICATIONS;
    auto cmdline = GetCommandLine ();

    if (wcsstr (cmdline, L" -terminate")) { // TODO: ends with
        if (SetPrivilege (SE_TCB_NAME, true)) {
            recipients |= BSM_ALLDESKTOPS;
        }
        terminate = true;
    }

    if (WM_Terminate) {
        if (BroadcastSystemMessage (BSF_FORCEIFHUNG | BSF_IGNORECURRENTTASK,
                                    &recipients, WM_Terminate,
                                    terminate ? 0 : ERROR_RESTART_APPLICATION, 0) > 0) {
            if (terminate) {
                ExitProcess (ERROR_SUCCESS);
            }
        }
    }
    if (terminate) {
        ExitProcess (GetLastError ());
    }
}

void InitSingleInstance () {
    SetLastError (0u);
    if (CreateMutex (NULL, FALSE, szInfo [0])) {
        DWORD error = GetLastError ();
        if (error == ERROR_ALREADY_EXISTS || error == ERROR_ACCESS_DENIED) {
            ExitProcess (error);
        }
    } else {
        ExitProcess (ERROR_SUCCESS);
    }
}

void InitRegistryKey () {
    HKEY hKeySoftware = NULL;
    if (RegCreateKeyEx (HKEY_CURRENT_USER, L"SOFTWARE", 0, NULL, 0,
                        KEY_CREATE_SUB_KEY, NULL, &hKeySoftware, NULL) == ERROR_SUCCESS) {

        HKEY hKeyTRIMCORE = NULL;
        if (RegCreateKeyEx (hKeySoftware, szInfo [2], 0, NULL, 0, // "CompanyName" TRIM CORE SOFTWARE s.r.o.
                            KEY_ALL_ACCESS, NULL, &hKeyTRIMCORE, NULL) == ERROR_SUCCESS) {
            DWORD disp = 0;
            if (RegCreateKeyEx (hKeyTRIMCORE, szInfo [6], 0, NULL, 0, // "ProductName"
                                KEY_ALL_ACCESS, NULL, &hKeySettings, &disp) == ERROR_SUCCESS) {

                if (disp == REG_CREATED_NEW_KEY) {
                    // defaults
                    RegSetSettingsValue (L"audio", 1);
                    RegSetSettingsValue (L"animated", 1);
                    RegSetSettingsValue (L"opacity", 25);
                }
            }
            RegCloseKey (hKeyTRIMCORE);
        }
        RegCloseKey (hKeySoftware);
    }

    if (!hKeySettings) {
        ExitProcess (ERROR_ACCESS_DENIED);
    }
}

UINT GetDPI (HWND hWnd) {
    if (hWnd != NULL) {
        if (pfnGetDpiForWindow)
            return pfnGetDpiForWindow (hWnd);
    } else {
        if (pfnGetDpiForSystem)
            return pfnGetDpiForSystem ();
    }
    if (HDC hDC = GetDC (hWnd)) {
        auto dpi = GetDeviceCaps (hDC, LOGPIXELSX);
        ReleaseDC (hWnd, hDC);
        return dpi;
    } else
        return USER_DEFAULT_SCREEN_DPI;
}

BOOL SetPrivilege (LPCTSTR lpszPrivilege, bool enable) {
    HANDLE hToken;
    if (OpenProcessToken (GetCurrentProcess (), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {

        LUID luid;
        if (LookupPrivilegeValue (NULL, lpszPrivilege, &luid)) {

            TOKEN_PRIVILEGES tp {};
            tp.PrivilegeCount = 1;
            tp.Privileges [0].Luid = luid;
            tp.Privileges [0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;

            if (AdjustTokenPrivileges (hToken, FALSE, &tp, sizeof tp, NULL, NULL)) {
                CloseHandle (hToken);
                return GetLastError () != ERROR_NOT_ALL_ASSIGNED;
            }
        }
        CloseHandle (hToken);
    }
    return FALSE;
}

DWORD RegGetSettingsValue (const wchar_t * name, DWORD default_) {
    DWORD size = sizeof (DWORD);
    DWORD value = 0;
    if (RegQueryValueEx (hKeySettings, name, NULL, NULL, reinterpret_cast <BYTE *> (&value), &size) == ERROR_SUCCESS) {
        return value;
    } else
        return default_;
}
void RegSetSettingsValue (const wchar_t * name, DWORD value) {
    RegSetValueEx (hKeySettings, name, 0, REG_DWORD, reinterpret_cast <const BYTE *> (&value), sizeof value);
}

void Optimize () {
    if (IsWindows8Point1OrGreater ()) {
        HeapSetInformation (NULL, HeapOptimizeResources, NULL, 0);
    }
    SetProcessWorkingSetSize (GetCurrentProcess (), (SIZE_T) -1, (SIZE_T) -1);
}

HBRUSH CreateSolidBrushEx (COLORREF color) {
    BITMAPINFO bi;

    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = 1u;
    bi.bmiHeader.biHeight = 1u;
    bi.bmiHeader.biPlanes = 1u;
    bi.bmiHeader.biBitCount = 32u;
    bi.bmiHeader.biCompression = BI_RGB;
    bi.bmiHeader.biSizeImage = 0u;
    bi.bmiHeader.biXPelsPerMeter = 0u;
    bi.bmiHeader.biYPelsPerMeter = 0u;
    bi.bmiHeader.biClrUsed = 0u;
    bi.bmiHeader.biClrImportant = 0u;

    auto alpha = (BYTE) (color >> 24);

    bi.bmiColors [0].rgbBlue = MulDiv (GetBValue (color), alpha, 0xFFu);
    bi.bmiColors [0].rgbGreen = MulDiv (GetGValue (color), alpha, 0xFFu);
    bi.bmiColors [0].rgbRed = MulDiv (GetRValue (color), alpha, 0xFFu);
    bi.bmiColors [0].rgbReserved = alpha;

    return CreateDIBPatternBrushPt (&bi, DIB_RGB_COLORS);
}

BOOL SetLayeredWindowAlpha (HWND hWnd, BYTE a) {
    BLENDFUNCTION bFn = { AC_SRC_OVER, 0, a, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO ulw = {};
    ulw.cbSize = sizeof ulw;
    ulw.pblend = &bFn;
    ulw.dwFlags = ULW_ALPHA;

    if (!a) {
        SIZE sizeZero = { 0, 0 };
        ulw.psize = &sizeZero;
    }
    return UpdateLayeredWindowIndirect (hWnd, &ulw);
}

void ChooseEffectColor (HWND hWnd) {
    CHOOSECOLOR choose;
    choose.lStructSize = sizeof choose;
    choose.hwndOwner = hWnd;
    choose.Flags = CC_ANYCOLOR | CC_RGBINIT | CC_FULLOPEN;
    choose.rgbResult = RegGetSettingsValue (L"color") & 0x00'FF'FF'FF;
    choose.lpCustColors = crCustomSet;

    if (ChooseColor (&choose)) {
        RegSetSettingsValue (L"color", choose.rgbResult | 0xFF000000);
    }
}

struct Coordinates {
    POINT origin = { 0, 0 };
    SIZE  size   = { 0, 0 };

    explicit operator bool () const {
        return this->size.cx != 0
            && this->size.cy != 0;
    }
};

Coordinates GetWindowCoordinates (HWND hWnd) {
    RECT r = {};
    if (!SUCCEEDED (DwmGetWindowAttribute (hWnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) {
        if (!GetWindowRect (hWnd, &r)) {
            return {};
        }
    }
    return {
        { r.left, r.top },
        { r.right - r.left, r.bottom - r.top }
    };
}

COLORREF GetEffectColor () {
    COLORREF color = RegGetSettingsValue (L"color");
    if (!color) {
        if (hKeyDWM) {
            DWORD size = sizeof color;
            RegQueryValueEx (hKeyDWM, L"AccentColor", NULL, NULL, reinterpret_cast <LPBYTE> (&color), &size);
        }
    }
    if (!color) {
        color = GetSysColor (COLOR_HIGHLIGHT);
    }

    if (auto a = (color >> 24)) {
        color &= 0x00FFFFFF;
        color |= ((a * RegGetSettingsValue (L"opacity")) / 255) << 24;
    } else {
        color |= RegGetSettingsValue (L"opacity") << 24;
    }
    return color;
}

bool IsWindowsBuildOrGreater (WORD wMajorVersion, WORD wMinorVersion, DWORD dwBuildNumber) {
    OSVERSIONINFOEXW osvi = { sizeof (osvi), 0, 0, 0, 0, { 0 }, 0, 0 };
    DWORDLONG mask = 0;

    mask = VerSetConditionMask (mask, VER_MAJORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask (mask, VER_MINORVERSION, VER_GREATER_EQUAL);
    mask = VerSetConditionMask (mask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

    osvi.dwMajorVersion = wMajorVersion;
    osvi.dwMinorVersion = wMinorVersion;
    osvi.dwBuildNumber = dwBuildNumber;

    return VerifyVersionInfoW (&osvi, VER_MAJORVERSION | VER_MINORVERSION | VER_BUILDNUMBER, mask);
}

bool IsWindows11OrGreater () {
    return IsWindowsBuildOrGreater (10, 0, 22000);
}

LONG GetWindowRadius (HWND hWnd) {
    LONG radius = 0;
    if (!IsWindows8OrGreater ()) { // Vista and 7
        BOOL composited = FALSE;
        if (SUCCEEDED (DwmIsCompositionEnabled (&composited))) {
            if (composited) {
                radius = 4;
            } else {
                // TODO: if themes are enabled and Aero.msstyles then top corners are rounded
            }
        }
    }
    if (IsWindows11OrGreater ()) {
        radius = 7;

        /*DWM_WINDOW_CORNER_PREFERENCE preference;
        if (SUCCEEDED (DwmGetWindowAttribute (hWnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference, sizeof preference))) {
            // this doesn't work
        }*/
    }
    radius = RegGetSettingsValue (L"force rounded corners", radius);
    if (radius) {
        radius *= GetDPI (hWnd);
        radius /= 96;
    }
    return radius;
}

int FillRect (HDC hDC, const RECT & r, HBRUSH hBrush) {
    return FillRect (hDC, &r, hBrush);
}

bool GenerateEffect (HDC hDC, HWND hWnd, SIZE size, DWORD effect, COLORREF * image) {
    auto color = GetEffectColor ();
    auto r = GetWindowRadius (hWnd);
    auto d = r * 2;

    if (auto hBrush = CreateSolidBrushEx (color)) {
        auto hOldBrush = SelectObject (hDC, hBrush);
        auto hOldPen = SelectObject (hDC, GetStockObject (NULL_PEN));

        switch (effect) {
            case 0: { // Focus effect 

                // TODO: make elliptic (currently simple circular)
                // TODO: vectorize

                POINT center = { size.cx / 2, size.cy / 2 };
                auto maxdistance = sqrtf (center.x * center.x + center.y * center.y);
                auto opacity = RegGetSettingsValue (L"opacity") / 100.0f;
                auto i = 0L;

                for (auto y = 0L; y != size.cy; ++y) {
                    for (auto x = 0L; x != size.cx; ++x) {

                        auto dx = (x - center.x) * (x - center.x);
                        auto dy = (y - center.y) * (y - center.y);

                        auto distance = sqrtf (dx + dy);
                        auto alpha = distance / maxdistance;

                        if (alpha > 0.5f) { // roughly: alpha ^ 8 * 255 > 0

                            // alpha ^ 8
                            alpha *= alpha;
                            alpha *= alpha;
                            alpha *= alpha;

                            alpha *= opacity;

                            if (alpha) {
                                auto b = GetRValue (color) * alpha;
                                auto g = GetGValue (color) * alpha;
                                auto r = GetBValue (color) * alpha;
                                auto a = 255.0f * alpha;

                                image [i] = RGB (r, g, b) | (((BYTE) a) << 24);
                            }
                        }

                        ++i;
                    }
                }

                if (r) {
                    // TODO: apply rounded corners
                }

                GdiFlush ();
            } break;

            case 1: // Frame effect
                break;

            case 2: // Full window snap
            case 3: // Full window pulse
                if (r) {
                    //Pie (hDC,         0,         0,       d,       d,         r,         0,         0,         r); // top left
                    //Pie (hDC, size.cx-d,         0, size.cx,       d, size.cx-0,         r, size.cx-r,         0); // top right
                    //Pie (hDC,         0, size.cy-d,       d, size.cy,         0, size.cy-r,         r, size.cx-0); // bottom left
                    //Pie (hDC, size.cx-d, size.cy-d, size.cx, size.cy, size.cx-r, size.cy-0, size.cx-0, size.cy-r); // bottom right

                    //FillRect (hDC, { r, 0, size.cx - r, r }, hBrush); // top
                    //FillRect (hDC, { 0, r, r, size.cy - r }, hBrush); // left
                    //FillRect (hDC, { r, size.cy - r, size.cx - r, size.cy }, hBrush); // bottom
                    //FillRect (hDC, { size.cx - r, r, size.cx, size.cy - r }, hBrush); // right

                    // Note: I'm not sure to which extent is RoundRect accelerated.
                    //       FillRect is accelerated. The lines above may be much faster. Need to profile.

                    RoundRect (hDC, 0, 0, size.cx, size.cy, d, d);
                } else {
                    FillRect (hDC, { 0, 0, size.cx, size.cy }, hBrush);
                }
                break;
        }

        if (hOldBrush) {
            SelectObject (hDC, hOldBrush);
        }
        if (hOldPen) {
            SelectObject (hDC, hOldPen);
        }
        DeleteObject (hBrush);
    }
    return true;
}

void EndEffect (HWND hWnd, WPARAM wParam) {
    SetLayeredWindowAlpha (hWndOverlay, 0);
    KillTimer (hWnd, wParam);
    if (IsWindows10OrGreater ()) {
        Optimize (); // expensive on older OSs for some reason
    }
}

LRESULT CALLBACK Tray (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            nidTray.hWnd = hWnd;
            nidTray.hIcon = (HICON) LoadImage (GetModuleHandle (NULL), MAKEINTRESOURCE (1), IMAGE_ICON,
                                               GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), 0);

            _snwprintf (nidTray.szTip, sizeof nidTray.szTip / sizeof nidTray.szTip [0],
                        L"%s %s\n%s", szInfo [6], szInfo [7], szInfo [5]);
            PostMessage (hWnd, WM_TaskbarCreated, 0, 0);

            if (!AddClipboardFormatListener (hWnd))
                return -1;

            hWndOverlay = CreateWindowEx (WS_EX_TOOLWINDOW | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST,
                                          (LPCTSTR) (std::intptr_t) aEffect, L"", WS_POPUP | WS_VISIBLE,
                                          0, 0, 0, 0, HWND_DESKTOP, NULL, (HINSTANCE) &__ImageBase, NULL);
            break;

        case WM_DPICHANGED:
            if (auto hNewIcon = (HICON) LoadImage (GetModuleHandle (NULL), MAKEINTRESOURCE (1), IMAGE_ICON,
                                                   LOWORD (wParam) * GetSystemMetrics (SM_CXSMICON) / 96,
                                                   HIWORD (wParam) * GetSystemMetrics (SM_CYSMICON) / 96, 0)) {
                DestroyIcon (nidTray.hIcon);
                nidTray.hIcon = hNewIcon;

                Shell_NotifyIcon (NIM_MODIFY, &nidTray);
            }
            break;

        case WM_CLOSE:
            DestroyWindow (hWndOverlay);
            Shell_NotifyIcon (NIM_DELETE, &nidTray);
            RemoveClipboardFormatListener (hWnd);
            PostQuitMessage ((int) wParam);
            break;

        case WM_CLIPBOARDUPDATE:
            if (RegGetSettingsValue (L"audio")) {
                PlaySound (MAKEINTRESOURCE (1), (HMODULE) &__ImageBase, SND_ASYNC | SND_RESOURCE);
            }
            if (auto hOwner = GetForegroundWindow ()) {
                if (auto window = GetWindowCoordinates (hOwner)) {

                    if (auto hWindowDC = GetDC (hWndOverlay)) {
                        if (auto hMemoryDC = CreateCompatibleDC (hWindowDC)) {

                            BITMAPINFO info {};
                            info.bmiHeader.biSize = sizeof info;
                            info.bmiHeader.biWidth = window.size.cx;
                            info.bmiHeader.biHeight = -window.size.cy;
                            info.bmiHeader.biPlanes = 1;
                            info.bmiHeader.biBitCount = 32;
                            info.bmiHeader.biCompression = BI_RGB;

                            void * data;
                            if (auto hBitmap = CreateDIBSection (hWindowDC, &info, DIB_RGB_COLORS, &data, NULL, 0u)) {
                                auto hOldBitmap = SelectObject (hMemoryDC, hBitmap);
                                
                                auto effect = RegGetSettingsValue (L"effect");
                                auto animated = RegGetSettingsValue (L"animated");

                                if (GenerateEffect (hMemoryDC, hOwner, window.size, effect, (COLORREF *) data)) {

                                    BYTE start = 255;
                                    if (animated) {
                                        switch (effect) {
                                            case 0: // Focus effect
                                            case 1: // Frame effect
                                            case 3: // Full window pulse
                                                start = 0;
                                                break;
                                            case 2: // Full window snap
                                                break;
                                        }
                                    }

                                    POINT ptZero = { 0, 0 };
                                    BLENDFUNCTION blendFnAlpha = { AC_SRC_OVER, 0, start, AC_SRC_ALPHA };

                                    UPDATELAYEREDWINDOWINFO ulw = {};
                                    ulw.cbSize = sizeof ulw;
                                    ulw.pptDst = &window.origin;
                                    ulw.pptSrc = &ptZero;
                                    ulw.psize = &window.size;
                                    ulw.hdcSrc = hMemoryDC;
                                    ulw.pblend = &blendFnAlpha;
                                    ulw.dwFlags = ULW_ALPHA;

                                    SetLastError (0);
                                    if (UpdateLayeredWindowIndirect (hWndOverlay, &ulw)) {

                                        progress = 0;
                                        if (animated) {
                                            SetTimer (hWnd, 1, USER_TIMER_MINIMUM, NULL);
                                        } else {
                                            SetTimer (hWnd, 2, GetDoubleClickTime () / 2, NULL);
                                        }
                                    }
                                }

                                if (hOldBitmap) {
                                    SelectObject (hMemoryDC, hOldBitmap);
                                }
                                DeleteBitmap (hBitmap);
                            }
                            DeleteDC (hMemoryDC);
                        }
                        ReleaseDC (hWndOverlay, hWindowDC);
                    }
                }
            }
            break;

        case WM_TIMER:
            switch (wParam) {
                case 1:
                    switch (RegGetSettingsValue (L"effect")) {
                        case 0: // Focus effect
                        case 1: // Frame effect
                        case 3: // Full window pulse
                            if (progress < 255) {
                                progress += 8; // 0.32s ...TODO: change to GetDoubleClickTime
                                
                                auto alpha = 255.0f * sinf (3.14159265358979323846 * progress / 255.0f);
                                if (alpha < 0.0f) {
                                    alpha = 0.0f;
                                }

                                SetLayeredWindowAlpha (hWndOverlay, (BYTE) alpha);
                            } else {
                                EndEffect (hWnd, wParam);
                            }
                            break;

                        case 2: // Full window snap
                            if (progress < 255) {
                                progress += 15; // 0.17s
                                SetLayeredWindowAlpha (hWndOverlay, 255 - progress);
                            } else {
                                EndEffect (hWnd, wParam);
                            }
                            break;
                    }
                    break;

                case 2:
                    EndEffect (hWnd, wParam);
                    break;
            }
            break;

        case WM_USER:
            switch (HIWORD (lParam)) {
                case 1:
                    switch (LOWORD (lParam)) {
                        case WM_CONTEXTMENU:
                            TrackMenu (hWnd, wParam);
                            break;
                    }
                    break;
            }
            break;

        case WM_COMMAND:
            switch (auto id = LOWORD (wParam)) {
                case IDHELP:
                    ShellExecute (hWnd, NULL, szInfo [8], NULL, NULL, SW_SHOWDEFAULT);
                    break;
                case IDCLOSE:
                    PostMessage (hWnd, WM_CLOSE, 0, 0);
                    break;
                case IDIGNORE:
                    nidTray.dwState = NIS_HIDDEN;
                    nidTray.dwStateMask = NIS_HIDDEN;
                    Shell_NotifyIcon (NIM_MODIFY, &nidTray);
                    break;

                case 0x10:
                    RegSetSettingsValue (L"audio", RegGetSettingsValue (L"audio") ? 0 : 1);
                    break;
                case 0x11:
                    RegSetSettingsValue (L"animated", RegGetSettingsValue (L"animated") ? 0 : 1);
                    break;

                case 0x30:
                    RegSetSettingsValue (L"color", 0);
                    break;
                case 0x3F:
                    ChooseEffectColor (hWnd);
                    break;

                default:
                    if (id >= 0x20 && id <= 0x2F) {
                        RegSetSettingsValue (L"effect", id - 0x20);
                    }
                    if (id >= 0x40 && id <= 0x4F) {
                        RegSetSettingsValue (L"opacity", 5 * (id - 0x40));
                    }
            }
            break;

        case WM_ENDSESSION:
            if (wParam) {
                PostMessage (hWnd, WM_CLOSE, ERROR_SHUTDOWN_IN_PROGRESS, 0);
            }
            break;

        default:
            if (message == WM_Terminate) {
                PostMessage (hWnd, WM_CLOSE, wParam, 0);
            }
            if (message == WM_TaskbarCreated) {
                Shell_NotifyIcon (NIM_ADD, &nidTray);
                Shell_NotifyIcon (NIM_SETVERSION, &nidTray);
            }
    }
    return DefWindowProc (hWnd, message, wParam, lParam);
}

void TrackMenu (HWND hWnd, WPARAM wParam) {
    CheckMenuItem (hMenu, 0x10, RegGetSettingsValue (L"audio") ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem (hMenu, 0x11, RegGetSettingsValue (L"animated") ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem (hMenu, 0x20, 0x2F, 0x20 + RegGetSettingsValue (L"effect"), MF_BYCOMMAND);
    CheckMenuRadioItem (hMenu, 0x40, 0x4F, 0x40 + RegGetSettingsValue (L"opacity") / 5, MF_BYCOMMAND);

    if (RegGetSettingsValue (L"color")) {
        // TODO: additional colors here?
        CheckMenuRadioItem (hMenu, 0x30, 0x3F, 0x3F, MF_BYCOMMAND);
    } else {
        CheckMenuRadioItem (hMenu, 0x30, 0x3F, 0x30, MF_BYCOMMAND);
    }

    SetForegroundWindow (hWnd);

    UINT style = TPM_RIGHTBUTTON;
    if (GetSystemMetrics (SM_MENUDROPALIGNMENT)) {
        style |= TPM_RIGHTALIGN;
    }

    if (!TrackPopupMenu (hMenu, style, GET_X_LPARAM (wParam), GET_Y_LPARAM (wParam), 0, hWnd, NULL)) {
        Shell_NotifyIcon (NIM_SETFOCUS, &nidTray);
    }
    PostMessage (hWnd, WM_NULL, 0, 0);
}

RECT GetCurrentMonitorWorkArea () {
    POINT cursor = { 0, 0 };
    GetCursorPos (&cursor);

    if (auto hMonitor = MonitorFromPoint (cursor, MONITOR_DEFAULTTOPRIMARY)) {

        MONITORINFO monitor {};
        monitor.cbSize = sizeof monitor;

        if (GetMonitorInfo (hMonitor, &monitor))
            return monitor.rcWork;
    }
    return { 0, 0, GetSystemMetrics (SM_CXSCREEN), GetSystemMetrics (SM_CYSCREEN) };
}

UINT GetTaskbarAlignment () {
    APPBARDATA taskbar {};
    if (HWND hTaskBar = FindWindow (L"Shell_TrayWnd", NULL)) {
        taskbar.hWnd = hTaskBar;
        taskbar.cbSize = sizeof taskbar;

        if (SHAppBarMessage (ABM_GETTASKBARPOS, &taskbar))
            return taskbar.uEdge;
    }

    return ABE_BOTTOM;
}

LRESULT CALLBACK Hook (int code, WPARAM wParam, LPARAM lParam) {
    if ((code == HC_ACTION)
            && (wParam == 0)
            && (reinterpret_cast <CWPRETSTRUCT *> (lParam)->message == WM_INITDIALOG)) {

        HWND hDialog = reinterpret_cast <CWPRETSTRUCT *> (lParam)->hwnd;
        RECT rDialog; GetWindowRect (hDialog, &rDialog);
        RECT rParent = GetCurrentMonitorWorkArea ();

        OffsetRect (&rDialog, -rDialog.left, -rDialog.top);

        switch (GetTaskbarAlignment ()) {
            case ABE_TOP:
                OffsetRect (&rDialog,
                            15 * (rParent.right - rParent.left) / 16 - rDialog.right,
                            1 * (rParent.bottom - rParent.top) / 16);
                break;
            case ABE_LEFT:
                OffsetRect (&rDialog,
                            1 * (rParent.right - rParent.left) / 16,
                            15 * (rParent.bottom - rParent.top) / 16 - rDialog.bottom);
                break;
            case ABE_RIGHT:
            case ABE_BOTTOM:
                OffsetRect (&rDialog,
                            15 * (rParent.right - rParent.left) / 16 - rDialog.right,
                            15 * (rParent.bottom - rParent.top) / 16 - rDialog.bottom);
                break;
        }

        wchar_t text [64];
        if (GetWindowText (hDialog, text, 64)) {
            wchar_t caption [128];
            _snwprintf (caption, 128, L"%s \x2012 %s", szInfo [6], text);
            SetWindowText (hDialog, caption);
        }

        MoveWindow (hDialog,
                    rParent.left + rDialog.left, rParent.top + rDialog.top,
                    rDialog.right - rDialog.left, rDialog.bottom - rDialog.top,
                    TRUE);
    }
    return CallNextHookEx (NULL, code, wParam, lParam);
}
