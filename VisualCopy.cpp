#include <Windows.h>
#include <WindowsX.h>
#include <VersionHelpers.h>
#include <MMsystem.h>
#include <DwmAPI.h>

#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <math.h>

#pragma warning (disable:6053) // snwprintf may not NUL-terminate
#pragma warning (disable:26819) // unannotated fall-through
#pragma warning (disable:28159) // GetTickCount

USHORT WM_Terminate = WM_NULL;
USHORT WM_TaskbarCreated = WM_NULL;

ATOM aTray;
ATOM aEffect;
HKEY hKeySettings;
HKEY hKeyDWM;
HWND hWndTray;
HWND hWndOverlay;
HMENU hMenu;
HHOOK hMouseHook;
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
LRESULT CALLBACK DlgHook (int code, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK MouseHook (int code, WPARAM wParam, LPARAM lParam);

#ifndef _DEBUG
void Main () {
#else
int main () {
#endif
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

    hMouseHook = SetWindowsHookEx (WH_MOUSE_LL, MouseHook, NULL, 0);
    hDlgPosHook = SetWindowsHookEx (WH_CALLWNDPROCRET, DlgHook, NULL, GetCurrentThreadId ());

    SetLastError (0);
    if (InitWndClasses (aTray, aEffect)) {
        InitCommonControls ();

        if (hWndTray = CreateWindow ((LPCTSTR) (std::intptr_t) aTray, L"", WS_POPUP, 0, 0, 0, 0, HWND_DESKTOP, NULL, NULL, NULL)) {
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

bool ends_with (const wchar_t * haystack, const wchar_t * needle) {
    if (auto location = wcsstr (haystack, needle)) {
        return *(location + wcslen (needle)) == L'\0';
    } else
        return false;
}

void InitTerminationMessage () {
    WM_Terminate = RegisterWindowMessage (szInfo [0]);

    bool terminate = false;
    DWORD recipients = BSM_APPLICATIONS;
    auto cmdline = GetCommandLine ();

    if (ends_with (cmdline, L" -terminate")) {
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
                    RegSetSettingsValue (L"opacity", 50);
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

BOOL SetLayeredWindowAlpha (HWND hWnd, BYTE a) {
    SIZE sizeZero = { 0, 0 };
    BLENDFUNCTION bFn = { AC_SRC_OVER, 0, a, AC_SRC_ALPHA };
    UPDATELAYEREDWINDOWINFO ulw = {};
    ulw.cbSize = sizeof ulw;
    ulw.pblend = &bFn;
    ulw.dwFlags = ULW_ALPHA;

    if (!a) {
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
    POINT center = { 0, 0 };
    LONG  corners = 0;
    bool  top_corners_only = false;

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

LONG GetWindowRadius (HWND hWnd, bool & top_only) {
    LONG radius = 0;
    // if (hWnd != GetDesktopWindow () nor WS_POPUP

    if (!IsZoomed (hWnd)) { // maximized windows have square corners
        if (!IsWindows8OrGreater ()) { // Vista and 7
            BOOL composited = FALSE;
            if (SUCCEEDED (DwmIsCompositionEnabled (&composited))) {
                if (composited) {
                    radius = 8; // DPI?
                } else
                if (IsThemeActive ()) {
                    wchar_t filename [MAX_PATH];
                    if (GetCurrentThemeName (filename, MAX_PATH, NULL, 0, NULL, 0) == S_OK) {
                        if (ends_with (filename, L"Aero.msstyles")) {
                            radius = 8; // does not depend on DPI
                            top_only = true;
                        }
                    }
                }
            }
        }
        if (IsWindows11OrGreater ()) {
            radius = 8 * GetDPI (hWnd) / 96;
            // TODO: not if DWM effect mode is disabled
        }
    }

    // user-overriden
    radius = RegGetSettingsValue (L"force rounded corners", radius);
    return radius;
}

enum class Effect : DWORD {
    Focus = 0,
    Corners = 1,
    Snap = 2,
    Pulse = 3,
    End = 16, // non-animated effect timer only
    MouseFind = 17,
    MouseShakeEval = 18, // delay timer for MouseFind only
};

bool GenerateEffect (HDC hDC, HWND hWnd, Coordinates coords, Effect effect, COLORREF * image) {
    auto color = GetEffectColor ();

    switch (effect) {
        using enum Effect;

        case Focus:
        case Corners:
        case MouseFind:
        {
            // TODO: make elliptic (currently simple circular)
            // TODO: vectorize

            auto maxdistance = sqrtf (float (coords.size.cx * coords.size.cx + coords.size.cy * coords.size.cy)) / 2;
            auto opacity = RegGetSettingsValue (L"opacity") / 100.0f;
            COLORREF background = 0;

            if (RegGetSettingsValue (L"dim")) {
                background = ((BYTE) (opacity * 255.0f)) << 24;
            }

            float alpha_cutout;
            switch (effect) {
                case MouseFind:
                    maxdistance /= 1.4142135f;
                    switch (RegGetSettingsValue (L"circle")) {
                        case 14: maxdistance /= 24.0f; break;
                        case 15: maxdistance /= 12.0f; break;
                        case 0: maxdistance /= 6.0f; break;
                        case 1: maxdistance /= 4.0f; break;
                        case 2: maxdistance /= 2.0f; break;
                    }
                    alpha_cutout = 0.5f;
                    break;
                case Focus:
                    alpha_cutout = 0.5f; // roughly: alpha ^ 8 * 255 > 0
                    break;
                case Corners:
                    alpha_cutout = 0.7f; // roughly: alpha ^ 16 * 255 > 0
                    break;
            }

            const auto symmetric = (effect != MouseFind);
            const auto yZ = symmetric ? (coords.size.cy + 1) / 2 : coords.size.cy;
            const auto xZ = symmetric ? (coords.size.cx + 1) / 2 : coords.size.cx;

            for (auto y = 0L; y != yZ; ++y) {
                auto dy = (y - coords.center.y) * (y - coords.center.y);

                for (auto x = 0L; x != xZ; ++x) {
                    auto dx = (x - coords.center.x) * (x - coords.center.x);
                    auto distance = sqrtf (float (dx + dy));
                    if (distance < maxdistance) {
                        auto alpha = distance / maxdistance;

                        if (alpha > alpha_cutout) {

                            alpha *= alpha;
                            alpha *= alpha;

                            if (effect != MouseFind) {
                                alpha *= alpha; // alpha ^ 8

                                if (effect == Corners) {
                                    alpha *= alpha; // make it: alpha ^ 16
                                }
                            }

                            alpha *= opacity;
                            alpha *= 255.0f;

                            auto a = (UINT) alpha;
                            if (a) {
                                auto b = GetRValue (color) * a / 255;
                                auto g = GetGValue (color) * a / 255;
                                auto r = GetBValue (color) * a / 255;
                                auto v = RGB (r, g, b) | (((BYTE) a) << 24);

                                auto yO = coords.size.cx * y;
                                image [yO + x] = v;

                                if (symmetric) {
                                    auto xR = coords.size.cx - x - 1;
                                    auto yR = coords.size.cx * (coords.size.cy - y - 1);

                                    image [yO + xR] = v;
                                    image [yR + x] = v;
                                    image [yR + xR] = v;
                                }
                            }
                        }
                    } else {
                        if (background) { // TODO: remove this 'if' when we start reusing the buffer
                            image [coords.size.cx * y + x] = background;
                        }
                    }
                }
            }
        } break;

        case Snap:
        case Pulse:
            if (auto a = RegGetSettingsValue (L"opacity")) {
                auto b = GetRValue (color) * a / 255;
                auto g = GetGValue (color) * a / 255;
                auto r = GetBValue (color) * a / 255;

                color = RGB (r, g, b) | (((BYTE) a) << 24);

                auto n = coords.size.cx * coords.size.cy;
                for (auto i = 0L; i != n; ++i) {
                    image [i] = color;
                }
            }
            break;
    }

    // rounded corners

    if (auto r = coords.corners) {

        if (r > coords.size.cx) r = coords.size.cx;
        if (r > coords.size.cy) r = coords.size.cy;

        auto diagonal = r * 1.4142135f;
        for (auto y = 0L; y != r; ++y) {

            auto yO = coords.size.cx * (r - y - 1);
            auto yR = coords.size.cx * (coords.size.cy - (r - y - 1) - 1);

            for (auto x = 0L; x != r; ++x) {
                auto distance = sqrtf (float (x * x + y * y));
                if (distance < r - 1.0f)
                    continue;

                auto x0 = r - x - 1;
                auto xR = coords.size.cx - (r - x - 1) - 1;

                COLORREF v = 0;
                if (distance <= r) {
                    distance -= r;
                    distance = -distance;

                    v = image [yO + x0];

                    auto a = (UINT) (distance * 255.0f);
                    auto r = GetRValue (v) * a / 255;
                    auto g = GetGValue (v) * a / 255;
                    auto b = GetBValue (v) * a / 255;
                    a = (v >> 24) * a / 255;

                    v = RGB (r, g, b) | (((BYTE) a) << 24);
                }

                image [yO + x0] = v;
                image [yO + xR] = v;

                if (!coords.top_corners_only) {
                    image [yR + x0] = v;
                    image [yR + xR] = v;
                }
            }
        }
    }

    GdiFlush ();
    return true;
}

void StartEffect (HWND hWnd, Coordinates coords, Effect effect) {
    if (auto hWindowDC = GetDC (hWndOverlay)) {
        if (auto hMemoryDC = CreateCompatibleDC (hWindowDC)) {

            // TODO: cache the bitmap and reuse if settings haven't changed
            //       and if the window size didn't (or the effect is compatible with smaller)

            BITMAPINFO info {};
            info.bmiHeader.biSize = sizeof info;
            info.bmiHeader.biWidth = coords.size.cx;
            info.bmiHeader.biHeight = -coords.size.cy;
            info.bmiHeader.biPlanes = 1;
            info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;

            void * data;
            if (auto hBitmap = CreateDIBSection (hWindowDC, &info, DIB_RGB_COLORS, &data, NULL, 0u)) {
                auto hOldBitmap = SelectObject (hMemoryDC, hBitmap);
                auto animated = RegGetSettingsValue (L"animated");

                if (GenerateEffect (hMemoryDC, hWnd, coords, effect, (COLORREF *) data)) {

                    BYTE start = 255;
                    if (animated) {
                        switch (effect) {
                            default:
                                start = 0;
                                break;
                            case Effect::Snap:
                                break;
                        }
                    }

                    POINT ptZero = { 0, 0 };
                    BLENDFUNCTION blendFnAlpha = { AC_SRC_OVER, 0, start, AC_SRC_ALPHA };

                    UPDATELAYEREDWINDOWINFO ulw = {};
                    ulw.cbSize = sizeof ulw;
                    ulw.pptDst = &coords.origin;
                    ulw.pptSrc = &ptZero;
                    ulw.psize = &coords.size;
                    ulw.hdcSrc = hMemoryDC;
                    ulw.pblend = &blendFnAlpha;
                    ulw.dwFlags = ULW_ALPHA;

                    SetLastError (0);
                    if (UpdateLayeredWindowIndirect (hWndOverlay, &ulw)) {

                        progress = 0;
                        if (animated) {
                            SetTimer (hWnd, (DWORD) effect, USER_TIMER_MINIMUM, NULL);
                        } else {
                            SetTimer (hWnd, (DWORD) Effect::End, GetDoubleClickTime () / 2, NULL);
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

void EndEffect (HWND hWnd, WPARAM wParam) {
    SetLayeredWindowAlpha (hWndOverlay, 0);
    KillTimer (hWnd, wParam);
    if (IsWindows10OrGreater ()) {
        Optimize (); // expensive on older OSs for some reason
    }
}

class Mouse {
    static constexpr std::size_t SIZE = 256u;

    struct Sample {
        DWORD t;
        POINT pt;
    };

    std::size_t index = 0u;
    Sample      history [SIZE] = {};

public:
    static constexpr float distance (const POINT & a, const POINT & b) noexcept {
        auto x = a.x - b.x;
        auto y = a.y - b.y;
        return sqrtf (float (x * x) + float (y * y));
    }

    void record (DWORD t, POINT pt) {
        this->history [++this->index % SIZE] = { t, pt };
    }
    bool evaluate (UINT frame, float * result) {
        const auto now = GetTickCount ();
        const auto oldest = 3 * GetDoubleClickTime ();
        const auto latest = this->history [this->index % SIZE].pt;

        auto p = latest;
        auto d = 0.0f;

        for (std::size_t i = 1; i != SIZE; ++i) {
            auto s = this->history [(this->index - i) % SIZE];
            if (now - s.t < oldest) {
                d += this->distance (s.pt, p);
                p = s.pt;
            } else
                break;
        }

        if (d > this->distance (p, latest) * 3.2f) {
            *result = d;
            return true;
        } else
            return false;
    }
} mouse;

LRESULT CALLBACK Tray (HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            if (!AddClipboardFormatListener (hWnd))
                return -1;

            nidTray.hWnd = hWnd;
            nidTray.hIcon = (HICON) LoadImage (GetModuleHandle (NULL), MAKEINTRESOURCE (1), IMAGE_ICON,
                                               GetSystemMetrics (SM_CXSMICON), GetSystemMetrics (SM_CYSMICON), 0);

            if (ends_with (GetCommandLine (), L" -hidden")) {
                nidTray.dwState = NIS_HIDDEN;
                nidTray.dwStateMask = NIS_HIDDEN;
            }

            _snwprintf (nidTray.szTip, sizeof nidTray.szTip / sizeof nidTray.szTip [0],
                        L"%s %s\n%s", szInfo [6], szInfo [7], szInfo [5]);
            PostMessage (hWnd, WM_TaskbarCreated, 0, 0);

            hWndOverlay = CreateWindowEx (WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE,// | WS_EX_NOREDIRECTIONBITMAP,
                                          (LPCTSTR) (std::intptr_t) aEffect, L"", WS_POPUP | WS_VISIBLE,
                                          0, 0, 0, 0, HWND_DESKTOP, NULL, (HINSTANCE) &__ImageBase, NULL);
            
            SetProp (hWndOverlay, L"NonRudeHWND", (HANDLE) 1);
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
            if (auto audio = RegGetSettingsValue (L"audio")) {
                PlaySound (MAKEINTRESOURCE (audio), (HMODULE) &__ImageBase, SND_ASYNC | SND_RESOURCE);
            }
            if (auto hOwner = GetForegroundWindow ()) {
                SetWindowPos (hWndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

                if (auto window = GetWindowCoordinates (hOwner)) {
                    window.corners = GetWindowRadius (hOwner, window.top_corners_only);
                    window.center.x = window.size.cx / 2;
                    window.center.y = window.size.cy / 2;
                    
                    StartEffect (hWnd, window, (Effect) RegGetSettingsValue (L"effect"));
                }
            }
            break;

        case WM_TIMER:
            switch ((Effect) wParam) {
                using enum Effect;

                case Focus:
                case Corners:
                case Pulse:
                case MouseFind:
                    if (progress < 255) {
                        progress += 8; // 0.32s ...TODO: change to GetDoubleClickTime

                        auto alpha = 255.0f * sinf (3.1415927f * progress / 255.0f);
                        if (alpha < 0.0f) {
                            alpha = 0.0f;
                        }

                        SetLayeredWindowAlpha (hWndOverlay, (BYTE) alpha);
                    } else {
                        EndEffect (hWnd, wParam);
                    }
                    break;

                case Snap:
                    if (progress < 255) {
                        progress += 15; // 0.17s
                        SetLayeredWindowAlpha (hWndOverlay, 255 - progress);
                    } else {
                        EndEffect (hWnd, wParam);
                    }
                    break;

                case End:
                    EndEffect (hWnd, wParam);
                    break;

                case MouseShakeEval:
                    KillTimer (hWnd, wParam);
                    if (RegGetSettingsValue (L"shake")) {

                        // do not show over fullscreen games

                        QUERY_USER_NOTIFICATION_STATE state {};
                        SHQueryUserNotificationState (&state);

                        switch (state) {
                            case QUNS_NOT_PRESENT: // The user is not present.  Heuristic check for modes like: screen saver, locked machine, non-active FUS session
                            case QUNS_BUSY: // The user is busy.  Heuristic check for modes like: full-screen app
                            case QUNS_RUNNING_D3D_FULL_SCREEN: // full-screen (exlusive-mode) D3D app
                                break;

                            default:
                            case QUNS_APP: // App-mode application
                            case QUNS_PRESENTATION_MODE: // Windows presentation mode (laptop feature) is turned on
                            case QUNS_ACCEPTS_NOTIFICATIONS: // notifications can be freely sent
                            case QUNS_QUIET_TIME: // We are in OOBE quiet period

                                float distance;
                                if (mouse.evaluate (GetDoubleClickTime (), &distance)) {

                                    POINT pt;
                                    if (GetCursorPos (&pt)) {

                                        MONITORINFO monitor;
                                        monitor.cbSize = sizeof monitor;
                                        if (GetMonitorInfo (MonitorFromPoint (pt, MONITOR_DEFAULTTONEAREST), &monitor)) {

                                            Coordinates coords;
                                            coords.origin.x = monitor.rcMonitor.left;
                                            coords.origin.y = monitor.rcMonitor.top;
                                            coords.size.cx = monitor.rcMonitor.right - monitor.rcMonitor.left;
                                            coords.size.cy = monitor.rcMonitor.bottom - monitor.rcMonitor.top;
                                            coords.center.x = pt.x - monitor.rcMonitor.left;
                                            coords.center.y = pt.y - monitor.rcMonitor.top;

                                            if (distance > mouse.distance (coords.origin, { monitor.rcMonitor.right, monitor.rcMonitor.bottom }) / 4.0f) {
                                                SetWindowPos (hWndOverlay, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                                                StartEffect (hWnd, coords, Effect::MouseFind);
                                            }
                                        }
                                    }
                                }
                        }
                    }
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

                case 0x11:
                    RegSetSettingsValue (L"animated", RegGetSettingsValue (L"animated") ? 0 : 1);
                    break;
                case 0x12:
                    RegSetSettingsValue (L"shake", RegGetSettingsValue (L"shake") ? 0 : 1);
                    break;
                case 0x13:
                    RegSetSettingsValue (L"dim", RegGetSettingsValue (L"dim") ? 0 : 1);
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
                    if (id >= 0x50 && id <= 0x5F) {
                        RegSetSettingsValue (L"circle", id - 0x50);
                    }
                    if (id >= 0x60 && id <= 0x6F) {
                        RegSetSettingsValue (L"audio", id - 0x60);
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
    CheckMenuItem (hMenu, 0x11, RegGetSettingsValue (L"animated") ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem (hMenu, 0x12, RegGetSettingsValue (L"shake") ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem (hMenu, 0x13, RegGetSettingsValue (L"dim") ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuRadioItem (hMenu, 0x20, 0x2F, 0x20 + RegGetSettingsValue (L"effect"), MF_BYCOMMAND);
    CheckMenuRadioItem (hMenu, 0x40, 0x4F, 0x40 + RegGetSettingsValue (L"opacity") / 5, MF_BYCOMMAND);
    CheckMenuRadioItem (hMenu, 0x60, 0x6F, 0x60 + RegGetSettingsValue (L"audio"), MF_BYCOMMAND);
    CheckMenuRadioItem (hMenu, 0x50, 0x5F, 0x50 + RegGetSettingsValue (L"circle"), MF_BYCOMMAND);

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

LRESULT CALLBACK DlgHook (int code, WPARAM wParam, LPARAM lParam) {
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

LRESULT CALLBACK MouseHook (int code, WPARAM wParam, LPARAM lParam) {
    if ((code == HC_ACTION) && (wParam == WM_MOUSEMOVE)) {
        if (auto data = reinterpret_cast <MSLLHOOKSTRUCT *> (lParam)) {
            mouse.record (data->time, data->pt);
            SetTimer (hWndTray, (UINT_PTR) Effect::MouseShakeEval, GetDoubleClickTime (), NULL);
        }
    }
    return CallNextHookEx (NULL, code, wParam, lParam);
}
