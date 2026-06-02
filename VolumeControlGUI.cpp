// VolumeControlGUI.cpp : Definiert den Einstiegspunkt für die Anwendung.

#include "framework.h"
#include "VolumeControlGUI.h"
#include <shellapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <commctrl.h>
#include <atomic>
#include <stdlib.h>
#include <sys/timeb.h>
#include <vector>
#include <tuple>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "comctl32.lib")

#define MAX_LOADSTRING 100
#define WM_TRAYICON (WM_USER + 1)

// Child control IDs (not in RC file)
#define IDC_EDIT_MAXDELAY       1001
#define IDC_EDIT_BLINDSPOT      1002
#define IDC_EDIT_MINSTEPS       1003
#define IDC_CHECK_AUTOSTART     1004
#define IDC_LABEL_MAXDELAY      1006
#define IDC_LABEL_BLINDSPOT     1007
#define IDC_LABEL_MINSTEPS      1008
#define IDC_CHECK_STARTMIN      1009

using std::vector;
using std::tuple;

// ---- Persistent settings (loaded from / saved to registry) ----

static long long g_maxDelay       = 400;   // ms
static long long g_blindSpot      = 500;   // ms
static unsigned  g_minSteps       = 1;
static bool      g_startMinimized = false;

static const wchar_t* APP_NAME      = L"VolumeControl";
static const wchar_t* SETTINGS_KEY  = L"Software\\VolumeControl";
static const wchar_t* AUTOSTART_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// ---- Settings registry helpers ----

static void SaveSettings()
{
    HKEY hKey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, nullptr,
        REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &hKey, nullptr) != ERROR_SUCCESS)
        return;
    DWORD val;
    val = (DWORD)g_maxDelay;
    RegSetValueExW(hKey, L"MaxDelay",       0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    val = (DWORD)g_blindSpot;
    RegSetValueExW(hKey, L"BlindSpot",      0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    val = g_minSteps;
    RegSetValueExW(hKey, L"MinVolumeSteps", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    val = g_startMinimized ? 1u : 0u;
    RegSetValueExW(hKey, L"StartMinimized", 0, REG_DWORD, (const BYTE*)&val, sizeof(val));
    RegCloseKey(hKey);
}

// Returns true if the key already existed (all values read).
// Returns false if the key was absent — caller should create it with defaults.
static bool LoadSettings()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, SETTINGS_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD type, size, val;

    size = sizeof(val); val = 0;
    if (RegQueryValueExW(hKey, L"MaxDelay", nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_maxDelay = (long long)val;

    size = sizeof(val); val = 0;
    if (RegQueryValueExW(hKey, L"BlindSpot", nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_blindSpot = (long long)val;

    size = sizeof(val); val = 0;
    if (RegQueryValueExW(hKey, L"MinVolumeSteps", nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_minSteps = val;

    size = sizeof(val); val = 0;
    if (RegQueryValueExW(hKey, L"StartMinimized", nullptr, &type, (BYTE*)&val, &size) == ERROR_SUCCESS
        && type == REG_DWORD)
        g_startMinimized = (val != 0);

    RegCloseKey(hKey);
    return true;
}

// Loads settings, or writes defaults to the registry if the key is absent.
static void EnsureSettings()
{
    if (!LoadSettings())
        SaveSettings();
}

// ---- Autostart registry helpers ----

static bool GetAutostart()
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_QUERY_VALUE, &hKey) != ERROR_SUCCESS)
        return false;
    DWORD size = 0;
    bool exists = (RegQueryValueExW(hKey, APP_NAME, nullptr, nullptr, nullptr, &size) == ERROR_SUCCESS);
    RegCloseKey(hKey);
    return exists;
}

static void SetAutostart(bool enable)
{
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, AUTOSTART_KEY, 0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    if (enable) {
        wchar_t exePath[MAX_PATH] = {};
        GetModuleFileNameW(NULL, exePath, MAX_PATH);
        RegSetValueExW(hKey, APP_NAME, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(exePath),
            (DWORD)((wcslen(exePath) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(hKey, APP_NAME);
    }
    RegCloseKey(hKey);
}

// ---- Media key helper ----

static void SendMediaKey(WORD vk)
{
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = vk;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = vk;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

// ---- Volume change callback ----

class VolumeCallback : public IAudioEndpointVolumeCallback
{
    LONG _refCount;
    struct _timeb timebuffer_start;
    long long start_ms;

    // Atomics allow safe live-update from the UI thread
    std::atomic<long long> max_delay{ 400 };      // ms: window for gesture recognition
    std::atomic<long long> blind_spot{ 300 };     // ms: cooldown after a detected gesture
    std::atomic<unsigned>  min_volume_steps{ 1 }; // minimum steps up AND down to count

    vector<tuple<long long, unsigned>> volume_changes;
    unsigned  last_volume_step;
    float     last_volume = 0.0f;
    long long last_detection_time = 0;

    IAudioEndpointVolume* volume_control;

    void cleanList()
    {
        struct _timeb tb;
        _ftime_s(&tb);
        long long now   = static_cast<long long>(tb.time) * 1000 + tb.millitm;
        long long delay = max_delay.load();
        while (!volume_changes.empty() && (now - std::get<0>(volume_changes.front())) > delay)
            volume_changes.erase(volume_changes.begin());
    }

    // Detects an up-then-down pattern in the recorded volume step sequence
    bool detectPattern()
    {
        if (volume_changes.size() < 2) return false;
        unsigned forward = 0, backward = 0;
        bool     peaked = false;
        unsigned steps  = min_volume_steps.load();
        for (size_t i = 1; i < volume_changes.size(); ++i) {
            int change = (int)std::get<1>(volume_changes[i]) - (int)std::get<1>(volume_changes[i - 1]);
            if (change > 0 && !peaked)
                forward += (unsigned)change;
            else if (change < 0) {
                peaked = true;
                backward += (unsigned)(-change);
            }
            if (forward >= steps && backward >= steps)
                return true;
        }
        return false;
    }

public:
    VolumeCallback(IAudioEndpointVolume* vc, unsigned currentVolume)
        : _refCount(1), last_volume_step(currentVolume), volume_control(vc)
    {
        _ftime_s(&timebuffer_start);
        start_ms = static_cast<long long>(timebuffer_start.time) * 1000 + timebuffer_start.millitm;
    }

    void SetMaxDelay(long long v)      { max_delay.store(v); }
    void SetBlindSpot(long long v)     { blind_spot.store(v); }
    void SetMinVolumeSteps(unsigned v) { min_volume_steps.store(v); }

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&_refCount); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = InterlockedDecrement(&_refCount);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, VOID** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IAudioEndpointVolumeCallback)) {
            *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA pNotify) override {
        if (!pNotify) return E_INVALIDARG;

        struct _timeb tb;
        _ftime_s(&tb);
        long long now = static_cast<long long>(tb.time) * 1000 + tb.millitm;

        unsigned step_idx = 0, step_cnt = 0;
        volume_control->GetVolumeStepInfo(&step_idx, &step_cnt);

        cleanList();

        if (last_detection_time != 0 && (now - last_detection_time) < blind_spot.load()) {
            volume_control->SetMasterVolumeLevelScalar(last_volume, NULL);
            volume_changes.clear();
            return S_OK;
        }

        if (volume_changes.empty())
            volume_changes.emplace_back(now, last_volume_step);
        volume_changes.emplace_back(now, step_idx);
        last_volume_step = step_idx;

        if (detectPattern()) {
            SendMediaKey(VK_MEDIA_PLAY_PAUSE);
            last_detection_time = now;
            last_volume      = (float)std::get<1>(volume_changes.front()) / (float)(step_cnt - 1);
            last_volume_step = std::get<1>(volume_changes.front());
            volume_control->SetMasterVolumeLevelScalar(last_volume, NULL);
            volume_changes.clear();
        }
        return S_OK;
    }
};

// ---- Globals ----

HINSTANCE hInst;
WCHAR szTitle[MAX_LOADSTRING];
WCHAR szWindowClass[MAX_LOADSTRING];
static HWND g_hWnd = nullptr; // stored in InitInstance for use by wWinMain start-minimized logic

static NOTIFYICONDATAW g_nid = {};
static bool g_trayAdded = false;

static IMMDeviceEnumerator*  g_deviceEnumerator = nullptr;
static IMMDevice*            g_defaultDevice    = nullptr;
static IAudioEndpointVolume* g_volumeControl    = nullptr;
static VolumeCallback*       g_callback         = nullptr;

// ---- Instant-apply helper ----
// Called on every EN_CHANGE notification from the three edit controls.
// Reads all three fields; if every value is valid, pushes to the live
// callback and persists to the registry immediately.
static void TryApplySettings(HWND hWnd)
{
    wchar_t buf[32];

    GetDlgItemTextW(hWnd, IDC_EDIT_MAXDELAY, buf, 32);
    if (!buf[0]) return;                          // field is empty — skip
    long long maxDelay = wcstoll(buf, nullptr, 10);
    if (maxDelay < 1) return;                     // 0 is not a useful window

    GetDlgItemTextW(hWnd, IDC_EDIT_BLINDSPOT, buf, 32);
    if (!buf[0]) return;
    long long blindSpot = wcstoll(buf, nullptr, 10);
    // 0 is valid (no cooldown); ES_NUMBER prevents negatives

    GetDlgItemTextW(hWnd, IDC_EDIT_MINSTEPS, buf, 32);
    if (!buf[0]) return;
    unsigned minSteps = (unsigned)wcstoul(buf, nullptr, 10);
    if (minSteps < 1) return;                     // must move at least one step

    // All fields valid — update globals, push to callback, persist
    g_maxDelay  = maxDelay;
    g_blindSpot = blindSpot;
    g_minSteps  = minSteps;

    if (g_callback) {
        g_callback->SetMaxDelay(g_maxDelay);
        g_callback->SetBlindSpot(g_blindSpot);
        g_callback->SetMinVolumeSteps(g_minSteps);
    }
    SaveSettings();
}

// ---- Audio init / cleanup ----

static bool InitAudio()
{
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&g_deviceEnumerator))))
        return false;

    if (FAILED(g_deviceEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &g_defaultDevice))) {
        g_deviceEnumerator->Release(); g_deviceEnumerator = nullptr;
        return false;
    }

    if (FAILED(g_defaultDevice->Activate(__uuidof(IAudioEndpointVolume),
        CLSCTX_INPROC_SERVER, nullptr, (void**)&g_volumeControl))) {
        g_defaultDevice->Release();    g_defaultDevice    = nullptr;
        g_deviceEnumerator->Release(); g_deviceEnumerator = nullptr;
        return false;
    }

    unsigned step_idx = 0, step_cnt = 0;
    g_volumeControl->GetVolumeStepInfo(&step_idx, &step_cnt);
    g_callback = new VolumeCallback(g_volumeControl, step_idx);

    // Apply settings loaded from the registry during WM_CREATE / EnsureSettings
    g_callback->SetMaxDelay(g_maxDelay);
    g_callback->SetBlindSpot(g_blindSpot);
    g_callback->SetMinVolumeSteps(g_minSteps);

    if (FAILED(g_volumeControl->RegisterControlChangeNotify(g_callback))) {
        g_callback->Release();         g_callback         = nullptr;
        g_volumeControl->Release();    g_volumeControl    = nullptr;
        g_defaultDevice->Release();    g_defaultDevice    = nullptr;
        g_deviceEnumerator->Release(); g_deviceEnumerator = nullptr;
        return false;
    }
    return true;
}

static void CleanupAudio()
{
    if (g_volumeControl && g_callback)
        g_volumeControl->UnregisterControlChangeNotify(g_callback);
    if (g_callback)         { g_callback->Release();         g_callback         = nullptr; }
    if (g_volumeControl)    { g_volumeControl->Release();    g_volumeControl    = nullptr; }
    if (g_defaultDevice)    { g_defaultDevice->Release();    g_defaultDevice    = nullptr; }
    if (g_deviceEnumerator) { g_deviceEnumerator->Release(); g_deviceEnumerator = nullptr; }
}

// ---- Tray helpers ----

static void AddTrayIcon(HWND hWnd)
{
    if (g_trayAdded) return;
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = hWnd;
    g_nid.uID              = 1;
    g_nid.uFlags           = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon            = LoadIcon(hInst, MAKEINTRESOURCE(IDI_VOLUMECONTROL));
    wcscpy_s(g_nid.szTip, APP_NAME);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_trayAdded = true;
}

static void RemoveTrayIcon()
{
    if (!g_trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    if (g_nid.hIcon) DestroyIcon(g_nid.hIcon);
    ZeroMemory(&g_nid, sizeof(g_nid));
    g_trayAdded = false;
}

static void ShowTrayContextMenu(HWND hWnd)
{
    POINT pt;
    GetCursorPos(&pt);
    HMENU hMenu = CreatePopupMenu();
    if (!hMenu) return;
    AppendMenuW(hMenu, MF_STRING, IDM_ABOUT, L"&Info");
    AppendMenuW(hMenu, MF_STRING, IDM_EXIT,  L"&Quit");
    SetForegroundWindow(hWnd);
    TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hWnd, NULL);
    DestroyMenu(hMenu);
}

// ---- Forward declarations ----

ATOM             MyRegisterClass(HINSTANCE hInstance);
BOOL             InitInstance(HINSTANCE, int);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK About(HWND, UINT, WPARAM, LPARAM);

// ---- Entry point ----

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
                      _In_opt_ HINSTANCE hPrevInstance,
                      _In_ LPWSTR lpCmdLine,
                      _In_ int nCmdShow)
{
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    CoInitialize(NULL);

    // Required for tooltip common control class
    INITCOMMONCONTROLSEX icex = { sizeof(icex), ICC_WIN95_CLASSES };
    InitCommonControlsEx(&icex);

    LoadStringW(hInstance, IDS_APP_TITLE,     szTitle,       MAX_LOADSTRING);
    LoadStringW(hInstance, IDC_VOLUMECONTROL, szWindowClass, MAX_LOADSTRING);
    MyRegisterClass(hInstance);

    // InitInstance creates the window (firing WM_CREATE → EnsureSettings, which
    // populates g_startMinimized) and conditionally shows it.
    if (!InitInstance(hInstance, nCmdShow)) {
        CoUninitialize();
        return FALSE;
    }

    // InitAudio runs after WM_CREATE so the registry-loaded settings are ready.
    InitAudio();

    // If start-minimised was requested the window was never shown in InitInstance;
    // drop straight to the tray so the app is usable from there.
    if (g_startMinimized)
        AddTrayIcon(g_hWnd);

    HACCEL hAccelTable = LoadAccelerators(hInstance, MAKEINTRESOURCE(IDC_VOLUMECONTROL));
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        if (!TranslateAccelerator(msg.hwnd, hAccelTable, &msg)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    CleanupAudio();
    CoUninitialize();
    return (int)msg.wParam;
}

// ---- Window class registration ----

ATOM MyRegisterClass(HINSTANCE hInstance)
{
    WNDCLASSEXW wcex;
    wcex.cbSize        = sizeof(WNDCLASSEX);
    wcex.style         = CS_HREDRAW | CS_VREDRAW;
    wcex.lpfnWndProc   = WndProc;
    wcex.cbClsExtra    = 0;
    wcex.cbWndExtra    = 0;
    wcex.hInstance     = hInstance;
    wcex.hIcon         = LoadIcon(hInstance, MAKEINTRESOURCE(IDI_VOLUMECONTROL));
    wcex.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wcex.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wcex.lpszMenuName  = MAKEINTRESOURCEW(IDC_VOLUMECONTROL);
    wcex.lpszClassName = szWindowClass;
    wcex.hIconSm       = LoadIcon(wcex.hInstance, MAKEINTRESOURCE(IDI_SMALL));
    return RegisterClassExW(&wcex);
}

// ---- InitInstance ----

BOOL InitInstance(HINSTANCE hInstance, int nCmdShow)
{
    hInst = hInstance;

    // WM_CREATE fires inside CreateWindowW and calls EnsureSettings, so
    // g_startMinimized is already set by the time we reach ShowWindow.
    HWND hWnd = CreateWindowW(szWindowClass, szTitle,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 310, 255,
        nullptr, nullptr, hInstance, nullptr);

    if (!hWnd)
        return FALSE;

    g_hWnd = hWnd;

    // Skip ShowWindow when start-minimised; wWinMain adds the tray icon instead.
    if (!g_startMinimized) {
        ShowWindow(hWnd, nCmdShow);
        UpdateWindow(hWnd);
    }
    return TRUE;
}

// ---- Main window procedure ----

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        // Load settings (or write defaults) before creating controls so the
        // edit boxes are pre-populated with the correct persisted values.
        EnsureSettings();

        const int x      = 10;
        const int labelW = 135;
        const int editW  = 75;
        const int gap    = 5;
        const int h      = 22;
        const int rowH   = 34;
        int y = 12;
        wchar_t buf[32];

        // ---- Max Delay ----
        CreateWindowW(L"STATIC", L"Max Delay (ms):",
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,    // SS_NOTIFY: receive WM_MOUSEMOVE for tooltips
            x, y, labelW, h, hWnd, (HMENU)IDC_LABEL_MAXDELAY, hInst, nullptr);
        swprintf_s(buf, L"%lld", g_maxDelay);
        CreateWindowW(L"EDIT", buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            x + labelW + gap, y, editW, h, hWnd, (HMENU)IDC_EDIT_MAXDELAY, hInst, nullptr);
        y += rowH;

        // ---- Blind Spot ----
        CreateWindowW(L"STATIC", L"Blind Spot (ms):",
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            x, y, labelW, h, hWnd, (HMENU)IDC_LABEL_BLINDSPOT, hInst, nullptr);
        swprintf_s(buf, L"%lld", g_blindSpot);
        CreateWindowW(L"EDIT", buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            x + labelW + gap, y, editW, h, hWnd, (HMENU)IDC_EDIT_BLINDSPOT, hInst, nullptr);
        y += rowH;

        // ---- Min Volume Steps ----
        CreateWindowW(L"STATIC", L"Min Volume Steps:",
            WS_CHILD | WS_VISIBLE | SS_NOTIFY,
            x, y, labelW, h, hWnd, (HMENU)IDC_LABEL_MINSTEPS, hInst, nullptr);
        swprintf_s(buf, L"%u", g_minSteps);
        CreateWindowW(L"EDIT", buf,
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_NUMBER,
            x + labelW + gap, y, editW, h, hWnd, (HMENU)IDC_EDIT_MINSTEPS, hInst, nullptr);
        y += rowH;

        // ---- Start with Windows (autostart) ----
        CreateWindowW(L"BUTTON", L"Start with Windows",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 200, h, hWnd, (HMENU)IDC_CHECK_AUTOSTART, hInst, nullptr);
        SendDlgItemMessageW(hWnd, IDC_CHECK_AUTOSTART, BM_SETCHECK,
            GetAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);
        y += rowH;

        // ---- Start minimized ----
        CreateWindowW(L"BUTTON", L"Start minimized",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            x, y, 200, h, hWnd, (HMENU)IDC_CHECK_STARTMIN, hInst, nullptr);
        SendDlgItemMessageW(hWnd, IDC_CHECK_STARTMIN, BM_SETCHECK,
            g_startMinimized ? BST_CHECKED : BST_UNCHECKED, 0);

        // ---- Tooltips ----
        // WS_EX_TOPMOST + explicit SetWindowPos ensure the tip appears above
        // all other windows. TTS_NOPREFIX prevents '&' being eaten as an
        // accelerator character in tip text. SS_NOTIFY on the STATIC labels
        // (above) is required so they receive WM_MOUSEMOVE instead of passing
        // it transparently to the parent.
        HWND hwndTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, NULL,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
            hWnd, NULL, hInst, NULL);

        if (hwndTip) {
            SetWindowPos(hwndTip, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);

            TOOLINFOW ti = {};
            // Use the V1 struct size, not sizeof(TOOLINFOW). Without a
            // comctl32 v6 manifest this app loads comctl32 v5.8, which rejects
            // the larger modern (lParam/lpReserved) cbSize: TTM_ADDTOOL would
            // silently return FALSE and no tooltip would ever appear. The V1
            // size is accepted by every comctl32 version and covers every
            // field used below (uFlags, hwnd, uId, lpszText).
            ti.cbSize    = TTTOOLINFOW_V1_SIZE;
            ti.uFlags    = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd      = hWnd;

            struct { int id; LPCWSTR text; } tips[] = {
                { IDC_LABEL_MAXDELAY,
                  L"Time window (ms) within which the entire volume gesture\n"
                  L" (up then back down) must be completed to be recognised." },
                { IDC_EDIT_MAXDELAY,
                  L"Time window (ms) within which the entire volume gesture\n"
                  L" (up then back down) must be completed to be recognised." },
                { IDC_LABEL_BLINDSPOT,
                  L"Cooldown (ms) after a gesture is detected.\n"
                  L" Any volume changes in this period are silently reset." },
                { IDC_EDIT_BLINDSPOT,
                  L"Cooldown (ms) after a gesture is detected.\n"
                  L" Any volume changes in this period are silently reset." },
                { IDC_LABEL_MINSTEPS,
                  L"Minimum number of volume steps that must be moved\n"
                  L" both up AND down to trigger a media Play/Pause." },
                { IDC_EDIT_MINSTEPS,
                  L"Minimum number of volume steps that must be moved\n"
                  L" both up AND down to trigger a media Play/Pause." },
            };

            for (auto& t : tips) {
                ti.uId      = (UINT_PTR)GetDlgItem(hWnd, t.id);
                ti.lpszText = (LPWSTR)t.text;
                SendMessageW(hwndTip, TTM_ADDTOOL, 0, (LPARAM)&ti);
            }
        }
    }
    break;

    case WM_COMMAND:
    {
        int wmId = LOWORD(wParam);
        switch (wmId)
        {
        case IDM_ABOUT:
            DialogBox(hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
            break;

        case IDM_EXIT:
            RemoveTrayIcon();
            DestroyWindow(hWnd);
            break;

        // Edit controls — apply immediately whenever the content changes and is valid
        case IDC_EDIT_MAXDELAY:
        case IDC_EDIT_BLINDSPOT:
        case IDC_EDIT_MINSTEPS:
            if (HIWORD(wParam) == EN_CHANGE)
                TryApplySettings(hWnd);
            break;

        case IDC_CHECK_AUTOSTART:
            if (HIWORD(wParam) == BN_CLICKED) {
                LRESULT state = SendDlgItemMessageW(hWnd, IDC_CHECK_AUTOSTART, BM_GETCHECK, 0, 0);
                SetAutostart(state == BST_CHECKED);
            }
            break;

        case IDC_CHECK_STARTMIN:
            if (HIWORD(wParam) == BN_CLICKED) {
                LRESULT state = SendDlgItemMessageW(hWnd, IDC_CHECK_STARTMIN, BM_GETCHECK, 0, 0);
                g_startMinimized = (state == BST_CHECKED);
                SaveSettings();
            }
            break;

        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
        }
    }
    break;

    case WM_TRAYICON:
        if (lParam == WM_LBUTTONDBLCLK || lParam == WM_LBUTTONUP) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
            RemoveTrayIcon();
        } else if (lParam == WM_RBUTTONUP) {
            ShowTrayContextMenu(hWnd);
        }
        break;

    case WM_CLOSE:
        ShowWindow(hWnd, SW_HIDE);
        AddTrayIcon(hWnd);
        break;

    case WM_CTLCOLORSTATIC:
        // Make STATIC label backgrounds transparent so they blend with the
        // window surface instead of painting an opaque COLOR_WINDOW rectangle.
        SetBkMode((HDC)wParam, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);

    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        BeginPaint(hWnd, &ps);
        EndPaint(hWnd, &ps);
    }
    break;

    case WM_DESTROY:
        RemoveTrayIcon();
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return 0;
}

// ---- About dialog ----

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
