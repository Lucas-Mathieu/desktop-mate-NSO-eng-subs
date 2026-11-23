// dmsubs — English subtitles for Desktop Mate voice lines.
//
// Loaded by the game as a version.dll proxy. Hooks Unity's native
// AudioSource::Play / PlayOneShot / Stop (addresses from Unity's public PDB,
// see tools/gen_offsets.py) to learn exactly which voice clip starts and when,
// then shows the matching subtitle from dmsubs\subtitles.json in a
// click-through window above the character.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

#include "MinHook.h"
#include "capture.h"
#include "overlay.h"
#include "subtitles.h"
#include "symbols.h"
#include "unity.h"

#pragma comment(lib, "ws2_32.lib")

// ---- version.dll forwarding -------------------------------------------------
#define FWD(n) __pragma(comment(linker, "/export:" #n "=C:\\Windows\\System32\\version." #n))
FWD(GetFileVersionInfoA) FWD(GetFileVersionInfoByHandle) FWD(GetFileVersionInfoExA)
FWD(GetFileVersionInfoExW) FWD(GetFileVersionInfoSizeA) FWD(GetFileVersionInfoSizeExA)
FWD(GetFileVersionInfoSizeExW) FWD(GetFileVersionInfoSizeW) FWD(GetFileVersionInfoW)
FWD(VerFindFileA) FWD(VerFindFileW) FWD(VerInstallFileA) FWD(VerInstallFileW)
FWD(VerLanguageNameA) FWD(VerLanguageNameW) FWD(VerQueryValueA) FWD(VerQueryValueW)

// ---- globals ----------------------------------------------------------------
static std::wstring g_dir;  // <game>\dmsubs\ (with trailing slash)
static std::mutex g_logMx;
static FILE* g_log = nullptr;
static SOCKET g_udp = INVALID_SOCKET;
static sockaddr_in g_udpAddr{};
static LARGE_INTEGER g_qpf;

void Log(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_logMx);
    if (!g_log) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

double DmNow();
static void Emit(const std::string& json) {
    if (g_udp == INVALID_SOCKET) return;
    sendto(g_udp, json.data(), (int)json.size(), 0, (sockaddr*)&g_udpAddr, sizeof g_udpAddr);
}

static std::string JsonEsc(const char* s) {
    std::string o;
    for (; s && *s; ++s) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { o += '\\'; o += (char)c; }
        else if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); o += b; }
        else o += (char)c;
    }
    return o;
}

// ---- Unity natives ----------------------------------------------------------
using PlayFn = void(__fastcall*)(void* src, double delay);
using PlayOneShotFn = void(__fastcall*)(void* src, void* clipPPtr, float vol);
using StopFn = void(__fastcall*)(void* src, bool stopOneShots);
using PPtrDerefFn = void*(__fastcall*)(const void* pptr);
using GetNameFn = const char*(__fastcall*)(const void* obj);
using GetLenFn = float(__fastcall*)(const void* clip);

static PlayFn o_Play;
static PlayOneShotFn o_PlayOneShot;
static StopFn o_Stop;
using UpdateFn = void(__fastcall*)(void* mgr);
static UpdateFn o_AudioUpdate;
static PPtrDerefFn PPtrDeref;
static GetNameFn NamedObjectGetName;
static GetNameFn ComponentGetName;
static GetLenFn ClipGetLength;
static uintptr_t g_clipOff = 0;

static const char* SafeStr(const char* s) {
    __try {
        if (s && !IsBadReadPtr(s, 1)) { volatile char c = *s; (void)c; return s; }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return "";
}

struct ClipInfo { std::string clip, speaker; float len = 0; bool ok = false; };

// SEH can't live in a function with C++ destructors, so raw reads go through this.
static bool DescribeRaw(void* src, void* clip, const char** name, const char** speaker, float* len) {
    __try {
        *name = SafeStr(NamedObjectGetName(clip));
        *len = ClipGetLength(clip);
        *speaker = src ? SafeStr(ComponentGetName(src)) : "";
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

static ClipInfo Describe(void* src, void* clip) {
    ClipInfo ci;
    const char *n = "", *sp = "";
    if (clip && DescribeRaw(src, clip, &n, &sp, &ci.len)) {
        ci.clip = n;
        ci.speaker = sp;
        ci.ok = true;
    }
    return ci;
}

double DmNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)g_qpf.QuadPart;
}

static bool g_record = true;
static int g_lastSpeakerId;
static std::wstring g_clipDir;

static bool FileExists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    for (auto& c : w)
        if (c == L'/') c = L'\\';
    return w;
}

static void OnClipStart(const char* kind, void* src, void* clip, double delay) {
    ClipInfo ci = Describe(src, clip);
    if (!ci.ok || ci.clip.empty()) return;
    std::string chr = Unity_CharacterId(src);
    if (src && !chr.empty()) g_lastSpeakerId = Unity_InstanceId(src);
    auto line = Subtitles_Find(chr, ci.clip);
    Log("%s %s/%s len=%.3f%s", kind, chr.c_str(), ci.clip.c_str(), ci.len, line ? "" : " (no subtitle)");
    char buf[64];
    sprintf_s(buf, "%.4f", ci.len);
    Emit(std::string("{\"ev\":\"play\",\"kind\":\"") + kind + "\",\"char\":\"" + JsonEsc(chr.c_str()) +
         "\",\"clip\":\"" + JsonEsc(ci.clip.c_str()) + "\",\"len\":" + buf + "}");
    Overlay_OnClipStart(src, ci.clip, line, ci.len, delay);
    if (!line && g_record && ci.len >= 0.3f) {
        std::string rel = (chr.empty() ? "_unknown" : chr) + "/" + ci.clip;
        if (!FileExists(g_clipDir + Utf8ToWide(rel) + L".wav")) Capture_Request(rel, DmNow() + delay, ci.len);
    }
}

static void __fastcall h_Play(void* src, double delay) {
    void* clip = nullptr;
    __try { clip = PPtrDeref((char*)src + g_clipOff); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_Play(src, delay);
    OnClipStart("play", src, clip, delay > 0 ? delay : 0);
}

static void __fastcall h_PlayOneShot(void* src, void* clipPPtr, float vol) {
    void* clip = nullptr;
    __try { clip = PPtrDeref(clipPPtr); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    o_PlayOneShot(src, clipPPtr, vol);
    OnClipStart("oneshot", src, clip, 0);
}

static void __fastcall h_Stop(void* src, bool stopOneShots) {
    o_Stop(src, stopOneShots);
    if (Overlay_OnSourceStop(src)) Log("stop src=%p", src);
}

// ---- harvest: play every not-yet-recorded clip once so it gets captured ------
// Opt-in (Ctrl+Alt+Shift+H, or create dmsubs\harvest.flag). Runs on the main thread.
struct DynArray { void** data; uint64_t label; uint64_t size; uint64_t cap; };
using FindObjectsFn = void(__fastcall*)(void* unused, const void* type, DynArray* out);
static FindObjectsFn FindObjects;
static const void* g_rttiAudioClip;
static std::vector<int> g_harvest;       // clip instance ids still to play
static double g_harvestNext;
static bool g_harvesting;
static bool g_keyWasDown;

static std::vector<std::string> g_harvestSkip;  // substrings; other mascots' / SFX clips stay loaded too

static bool ClipLooksRecordable(void* clip, const std::string& chr, std::string* name, float* len) {
    ClipInfo ci = Describe(nullptr, clip);
    if (!ci.ok || ci.clip.empty() || ci.len < 0.5f || ci.len > 60.f) return false;
    for (auto& sk : g_harvestSkip)
        if (!sk.empty() && ci.clip.find(sk) != std::string::npos) return false;
    if (Subtitles_Find(chr, ci.clip)) return false;
    if (FileExists(g_clipDir + Utf8ToWide((chr.empty() ? "_unknown" : chr) + "/" + ci.clip) + L".wav")) return false;
    *name = ci.clip;
    *len = ci.len;
    return true;
}

static bool FindAllClips(DynArray* arr) {
    __try { FindObjects(nullptr, g_rttiAudioClip, arr); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void StartHarvest() {
    void* src = Unity_Deref(g_lastSpeakerId);
    if (!src || !FindObjects || !g_rttiAudioClip) {
        Log("harvest: wait until the character has spoken once, then try again");
        return;
    }
    std::string chr = Unity_CharacterId(src);
    DynArray arr{nullptr, 1, 0, 1};
    if (!FindAllClips(&arr)) arr.size = 0;
    g_harvest.clear();
    double total = 0;
    for (uint64_t i = 0; i < arr.size; ++i) {
        std::string n;
        float len;
        if (ClipLooksRecordable(arr.data[i], chr, &n, &len)) {
            g_harvest.push_back(Unity_InstanceId(arr.data[i]));
            total += len + 1.0;
        }
    }
    Log("harvest: %d of %d loaded clips to record for %s (~%.0f s). Press Ctrl+Alt+Shift+H again to stop.",
        (int)g_harvest.size(), (int)arr.size, chr.c_str(), total);
    g_harvesting = !g_harvest.empty();
    g_harvestNext = DmNow() + 0.5;
}

static void HarvestTick() {
    if (!g_record) return;  // harvesting only makes sense while recording
    bool down = (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_MENU) & 0x8000) &&
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) && (GetAsyncKeyState('H') & 0x8000);
    bool flag = false;
    static double lastFlagCheck;
    if (DmNow() - lastFlagCheck > 1.0) {
        lastFlagCheck = DmNow();
        flag = DeleteFileW((g_dir + L"harvest.flag").c_str()) != 0;
    }
    if ((down && !g_keyWasDown) || flag) {
        if (g_harvesting) {
            g_harvesting = false;
            g_harvest.clear();
            Log("harvest: stopped");
        } else {
            StartHarvest();
        }
    }
    g_keyWasDown = down;
    if (!g_harvesting || DmNow() < g_harvestNext) return;
    void* src = Unity_Deref(g_lastSpeakerId);
    while (!g_harvest.empty()) {
        int id = g_harvest.back();
        g_harvest.pop_back();
        void* clip = Unity_Deref(id);
        if (!src || !clip) continue;
        ClipInfo ci = Describe(nullptr, clip);
        h_PlayOneShot(src, &id, 1.0f);  // goes through our hook -> recorded like a normal line
        g_harvestNext = DmNow() + ci.len + 1.0;
        Log("harvest: %d left", (int)g_harvest.size());
        return;
    }
    g_harvesting = false;
    Log("harvest: done - run tools\\learn.py to transcribe and translate");
}

static void __fastcall h_AudioUpdate(void* mgr) {
    o_AudioUpdate(mgr);
    Overlay_OnFrame();
    HarvestTick();
}

static bool Hook(uintptr_t addr, void* detour, void** orig, const char* name);

// ---- LibUniWinC guard ---------------------------------------------------------
// The game's transparent-window plugin attaches to "the first visible window of
// this process". Our subtitle window must never be that window, or the game
// loses its transparency. Hook the attach calls and re-target Unity's window.
using AttachFn = BOOL(__stdcall*)();
using AttachHandleFn = BOOL(__stdcall*)(HWND);
using GetHandleFn = HWND(__stdcall*)();
static AttachFn o_AttachMy, o_AttachActive;
static AttachHandleFn UwAttachHandle;
static GetHandleFn UwGetHandle;

static BOOL CALLBACK FindUnityWnd(HWND h, LPARAM out) {
    DWORD pid;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[64];
    if (pid == GetCurrentProcessId() && GetClassNameW(h, cls, 64) && wcscmp(cls, L"UnityWndClass") == 0) {
        *(HWND*)out = h;
        return FALSE;
    }
    return TRUE;
}

static BOOL FixAttach(BOOL r) {
    HWND ov = Overlay_Window();
    if (ov && UwGetHandle && UwGetHandle() == ov) {
        HWND u = nullptr;
        EnumWindows(FindUnityWnd, (LPARAM)&u);
        Log("LibUniWinC attached to the subtitle window - re-attaching to Unity window %p", u);
        if (u) r = UwAttachHandle(u);
        Overlay_RecreateWindow();  // the plugin restyled it; start fresh
    }
    return r;
}
static BOOL __stdcall h_AttachMy() { return FixAttach(o_AttachMy()); }
static BOOL __stdcall h_AttachActive() { return FixAttach(o_AttachActive()); }

static void GuardUniWin() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe;
    p = p.substr(0, p.size() - 4) + L"_Data\\Plugins\\x86_64\\LibUniWinC.dll";
    HMODULE m = LoadLibraryW(p.c_str());
    if (!m) { Log("LibUniWinC.dll not found - transparency guard off"); return; }
    UwAttachHandle = (AttachHandleFn)GetProcAddress(m, "AttachWindowHandle");
    UwGetHandle = (GetHandleFn)GetProcAddress(m, "GetWindowHandle");
    if (!UwAttachHandle || !UwGetHandle) return;
    struct { const char* n; void* d; void** o; } hs[] = {{"AttachMyWindow", (void*)h_AttachMy, (void**)&o_AttachMy},
                                                         {"AttachMyActiveWindow", (void*)h_AttachActive, (void**)&o_AttachActive}};
    for (auto& h : hs)
        if (void* f = GetProcAddress(m, h.n)) Hook((uintptr_t)f, h.d, h.o, h.n);
}

// ---- setup ------------------------------------------------------------------
static uintptr_t IniHex(const wchar_t* sec, const wchar_t* key) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(sec, key, L"", buf, 64, (g_dir + L"offsets.ini").c_str());
    return (uintptr_t)wcstoull(buf, nullptr, 0);
}

static bool Hook(uintptr_t addr, void* detour, void** orig, const char* name) {
    if (MH_CreateHook((void*)addr, detour, orig) != MH_OK || MH_EnableHook((void*)addr) != MH_OK) {
        Log("hook %s failed", name);
        return false;
    }
    return true;
}

static DWORD WINAPI InitThread(void*) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) == 0) {
        g_udp = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        g_udpAddr.sin_family = AF_INET;
        g_udpAddr.sin_port = htons(47811);
        inet_pton(AF_INET, "127.0.0.1", &g_udpAddr.sin_addr);
    }

    HMODULE up = nullptr;
    for (int i = 0; i < 600 && !(up = GetModuleHandleW(L"UnityPlayer.dll")); ++i) Sleep(50);
    if (!up) { Log("UnityPlayer.dll not found"); return 0; }

    if (!Symbols_Ensure(up, g_dir)) {
        Log("could not resolve UnityPlayer.dll functions for this game build - subtitles off this session "
            "(needs internet once after each game update)");
        return 0;
    }
    auto base = (uintptr_t)up;
    auto rva = [&](const wchar_t* k) { uintptr_t r = IniHex(L"rva", k); return r ? base + r : 0; };
    PPtrDeref = (PPtrDerefFn)rva(L"PPtr_Deref");
    NamedObjectGetName = (GetNameFn)rva(L"NamedObject_GetName");
    ComponentGetName = (GetNameFn)rva(L"Component_GetName");
    ClipGetLength = (GetLenFn)rva(L"AudioClip_GetLengthSec");
    g_clipOff = IniHex(L"layout", L"AudioSource_ClipPPtr");
    uintptr_t play = rva(L"AudioSource_Play"), one = rva(L"AudioSource_PlayOneShot"), stop = rva(L"AudioSource_Stop");
    if (!PPtrDeref || !NamedObjectGetName || !ComponentGetName || !ClipGetLength || !g_clipOff || !play || !one ||
        !stop) {
        Log("offsets.ini incomplete - delete it to regenerate");
        return 0;
    }

    Unity_Init({(uintptr_t)PPtrDeref, rva(L"Transform_GetPosition"), rva(L"FindMainCamera"),
                rva(L"Camera_GetWorldToClipMatrix"), (uintptr_t)ComponentGetName});
    if (MH_Initialize() != MH_OK) { Log("MH_Initialize failed"); return 0; }
    GuardUniWin();  // before the overlay window exists
    Subtitles_Load(g_dir);
    Overlay_Start(g_dir);
    g_clipDir = g_dir + L"clips\\";
    g_record = GetPrivateProfileIntW(L"capture", L"record_new_lines", 1, (g_dir + L"dmsubs.ini").c_str()) != 0;
    if (g_record) Capture_Start(g_clipDir);
    FindObjects = (FindObjectsFn)rva(L"FindObjectsOfType");
    {
        wchar_t skip[512];
        GetPrivateProfileStringW(L"capture", L"harvest_skip", L"Voi_,SE", skip, 512, (g_dir + L"dmsubs.ini").c_str());
        wchar_t* ctx = nullptr;
        for (wchar_t* tok = wcstok_s(skip, L",", &ctx); tok; tok = wcstok_s(nullptr, L",", &ctx)) {
            char u[256];
            WideCharToMultiByte(CP_UTF8, 0, tok, -1, u, sizeof u, nullptr, nullptr);
            g_harvestSkip.emplace_back(u);
        }
    }
    g_rttiAudioClip = (const void*)rva(L"RTTI_AudioClip");

    bool ok = Hook(play, (void*)h_Play, (void**)&o_Play, "Play") &
              Hook(one, (void*)h_PlayOneShot, (void**)&o_PlayOneShot, "PlayOneShot") &
              Hook(stop, (void*)h_Stop, (void**)&o_Stop, "Stop");
    if (uintptr_t upd = rva(L"AudioManager_Update"))
        ok &= Hook(upd, (void*)h_AudioUpdate, (void**)&o_AudioUpdate, "AudioManager::Update");
    Log("hooks %s (UnityPlayer @ %p)", ok ? "installed" : "PARTIAL", up);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(inst);
    QueryPerformanceFrequency(&g_qpf);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    g_dir = exe;
    g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/") + 1) + L"dmsubs\\";
    if (GetFileAttributesW(g_dir.c_str()) == INVALID_FILE_ATTRIBUTES) return TRUE;  // mod not installed

    g_log = _wfsopen((g_dir + L"dmsubs.log").c_str(), L"wb", _SH_DENYWR);
    Log("dmsubs loaded");
    HANDLE th = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
    if (th) CloseHandle(th);
    return TRUE;
}


