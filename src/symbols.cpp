// Resolves the UnityPlayer.dll functions the mod needs for whatever build is
// installed, so game updates don't break the mod.
//
// dmsubs\offsets.ini is a cache keyed by the UnityPlayer.dll build id. When it
// is missing or stale, Unity's public PDB for that exact build is downloaded
// from symbolserver.unity3d.com, the functions are looked up with dbghelp, and
// offsets.ini is rewritten. The PDB is deleted afterwards.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "symbols.h"

#include <windows.h>
#include <dbghelp.h>
#include <winhttp.h>

#include <cstdio>
#include <map>
#include <vector>

#pragma comment(lib, "winhttp.lib")

void Log(const char* fmt, ...);

namespace {

// ini key -> undecorated symbol name. Keep in sync with tools/gen_offsets.py.
const std::pair<const wchar_t*, const wchar_t*> kWanted[] = {
    {L"AudioSource_Play", L"AudioSource::Play"},
    {L"AudioSource_PlayOneShot", L"AudioSource::PlayOneShot"},
    {L"AudioSource_Stop", L"AudioSource::Stop"},
    {L"PPtr_Deref", L"PPtr<GameObject>::operator GameObject * __ptr64"},
    {L"NamedObject_GetName", L"NamedObject::GetName"},
    {L"GameObject_GetName", L"GameObject::GetName"},
    {L"Component_GetName", L"Unity::Component::GetName"},
    {L"AudioClip_GetLengthSec", L"AudioClip::GetLengthSec"},
    {L"Transform_GetPosition", L"Transform::GetPosition"},
    {L"FindMainCamera", L"FindMainCamera"},
    {L"Camera_GetWorldToClipMatrix", L"Camera::GetWorldToClipMatrix"},
    {L"AudioManager_Update", L"AudioManager::Update"},
    {L"FindObjectsOfType", L"FindObjectsOfTypeImplementation"},
    {L"RTTI_AudioClip", L"TypeContainer<AudioClip>::rtti"},
};
const wchar_t* kRequired[] = {L"AudioSource_Play", L"AudioSource_PlayOneShot", L"AudioSource_Stop", L"PPtr_Deref",
                              L"NamedObject_GetName", L"Component_GetName", L"AudioClip_GetLengthSec"};

struct BuildId {
    std::string id;       // GUID + age, as used by symbol servers
    std::wstring pdb;     // file name of the PDB
};

BuildId ReadBuildId(HMODULE m) {
    auto base = (uint8_t*)m;
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    auto dbg = (IMAGE_DEBUG_DIRECTORY*)(base + dd.VirtualAddress);
    for (DWORD i = 0; i < dd.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i) {
        if (dbg[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
        auto cv = base + dbg[i].AddressOfRawData;
        if (memcmp(cv, "RSDS", 4)) continue;
        GUID g;
        memcpy(&g, cv + 4, 16);
        DWORD age;
        memcpy(&age, cv + 20, 4);
        char s[64];
        sprintf_s(s, "%08X%04X%04X%02X%02X%02X%02X%02X%02X%02X%02X%X", g.Data1, g.Data2, g.Data3, g.Data4[0],
                  g.Data4[1], g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], age);
        std::string path = (const char*)(cv + 24);
        std::string file = path.substr(path.find_last_of("\\/") + 1);
        return {s, std::wstring(file.begin(), file.end())};
    }
    return {};
}

bool Download(const std::wstring& path, const std::wstring& dst) {
    HINTERNET s = WinHttpOpen(L"dmsubs", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) return false;
    HINTERNET c = WinHttpConnect(s, L"symbolserver.unity3d.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE)
                    : nullptr;
    bool ok = false;
    DWORD status = 0, len = sizeof status;
    if (r && WinHttpSendRequest(r, nullptr, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr) &&
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &len,
                            nullptr) &&
        status == 200) {
        std::wstring tmp = dst + L".part";
        FILE* f = nullptr;
        _wfopen_s(&f, tmp.c_str(), L"wb");
        if (f) {
            std::vector<char> buf(1 << 20);
            DWORD got = 0;
            ok = true;
            while (WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &got) && got) fwrite(buf.data(), 1, got, f);
            fclose(f);
            ok = MoveFileExW(tmp.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
        }
    } else {
        Log("symbols: download failed (HTTP %lu, error %lu)", status, GetLastError());
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

using SymSetOptionsFn = DWORD(WINAPI*)(DWORD);
using SymGetOptionsFn = DWORD(WINAPI*)();
using SymInitializeWFn = BOOL(WINAPI*)(HANDLE, PCWSTR, BOOL);
using SymLoadModuleExWFn = DWORD64(WINAPI*)(HANDLE, HANDLE, PCWSTR, PCWSTR, DWORD64, DWORD, PMODLOAD_DATA, DWORD);
using SymEnumSymbolsWFn = BOOL(WINAPI*)(HANDLE, ULONG64, PCWSTR, PSYM_ENUMERATESYMBOLS_CALLBACKW, PVOID);
using SymCleanupFn = BOOL(WINAPI*)(HANDLE);

struct EnumCtx {
    std::map<std::wstring, uintptr_t>* found;
    DWORD64 base;
};

BOOL CALLBACK OnSymbol(PSYMBOL_INFOW si, ULONG, PVOID p) {
    auto* ctx = (EnumCtx*)p;
    std::wstring name(si->Name, si->NameLen);
    while (!name.empty() && (name.back() == L'\0' || name.back() == L' ')) name.pop_back();
    for (auto& w : kWanted)
        if (name == w.second && !ctx->found->count(w.first)) (*ctx->found)[w.first] = (uintptr_t)(si->Address - ctx->base);
    return TRUE;
}

bool ResolveFromPdb(const std::wstring& image, const std::wstring& pdbDir, std::map<std::wstring, uintptr_t>& out) {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    HMODULE dh = LoadLibraryW((std::wstring(sys) + L"\\dbghelp.dll").c_str());
    if (!dh) return false;
    auto setOpt = (SymSetOptionsFn)GetProcAddress(dh, "SymSetOptions");
    auto getOpt = (SymGetOptionsFn)GetProcAddress(dh, "SymGetOptions");
    auto init = (SymInitializeWFn)GetProcAddress(dh, "SymInitializeW");
    auto load = (SymLoadModuleExWFn)GetProcAddress(dh, "SymLoadModuleExW");
    auto enumSyms = (SymEnumSymbolsWFn)GetProcAddress(dh, "SymEnumSymbolsW");
    auto cleanup = (SymCleanupFn)GetProcAddress(dh, "SymCleanup");
    if (!setOpt || !getOpt || !init || !load || !enumSyms || !cleanup) return false;
    HANDLE h = (HANDLE)(uintptr_t)0xD5D5D5D5;  // private session, not the real process
    DWORD oldOpt = getOpt();
    setOpt(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    bool ok = false;
    if (init(h, pdbDir.c_str(), FALSE)) {
        const DWORD64 fakeBase = 0x10000000;
        DWORD64 base = load(h, nullptr, image.c_str(), nullptr, fakeBase, 0, nullptr, 0);
        if (base) {
            EnumCtx ctx{&out, base};
            ok = enumSyms(h, base, L"*", OnSymbol, &ctx) != 0;
        }
        cleanup(h);
    }
    setOpt(oldOpt);
    return ok;
}

// AudioSource::Stop does `add rcx, <clip PPtr offset>; call PPtr<...>::operator T*`.
uintptr_t FindClipOffset(uintptr_t stop, uintptr_t deref) {
    const uint8_t* p = (const uint8_t*)stop;
    uintptr_t last = 0;
    for (int i = 0; i < 0x200; ++i) {
        if (p[i] == 0x48 && p[i + 1] == 0x81 && p[i + 2] == 0xC1) last = *(const uint32_t*)(p + i + 3);
        if (p[i] == 0x48 && p[i + 1] == 0x83 && p[i + 2] == 0xC1) last = p[i + 3];
        if (p[i] == 0xE8 && last) {
            uintptr_t target = stop + i + 5 + *(const int32_t*)(p + i + 1);
            if (target == deref) return last;
        }
    }
    return 0;
}

}  // namespace

bool Symbols_Ensure(HMODULE up, const std::wstring& dir) {
    std::wstring ini = dir + L"offsets.ini";
    BuildId b = ReadBuildId(up);
    if (b.id.empty()) { Log("symbols: UnityPlayer.dll has no build id"); return false; }
    wchar_t cached[64] = {};
    GetPrivateProfileStringW(L"unityplayer", L"pdb_guid", L"", cached, 64, ini.c_str());
    std::wstring id(b.id.begin(), b.id.end());
    if (id == cached) return true;

    Log("symbols: new UnityPlayer.dll build %s - fetching Unity's public symbols (one-time, ~25 MB)", b.id.c_str());
    std::wstring cache = dir + L"symcache\\";
    CreateDirectoryW(cache.c_str(), nullptr);
    std::wstring pdb = cache + b.pdb;
    if (!Download(L"/" + b.pdb + L"/" + id + L"/" + b.pdb, pdb)) return false;

    wchar_t image[MAX_PATH];
    GetModuleFileNameW(up, image, MAX_PATH);
    std::map<std::wstring, uintptr_t> rva;
    bool ok = ResolveFromPdb(image, cache, rva);
    DeleteFileW(pdb.c_str());
    RemoveDirectoryW(cache.c_str());
    if (!ok) { Log("symbols: could not read the PDB"); return false; }
    for (auto* k : kRequired)
        if (!rva.count(k)) {
            Log("symbols: required function %ls not found in this build", k);
            return false;
        }
    uintptr_t base = (uintptr_t)up;
    uintptr_t clip = FindClipOffset(base + rva[L"AudioSource_Stop"], base + rva[L"PPtr_Deref"]);
    if (!clip) { Log("symbols: could not find the AudioSource clip field"); return false; }

    FILE* f = nullptr;
    _wfopen_s(&f, ini.c_str(), L"w");
    if (!f) return false;
    fprintf(f, "[unityplayer]\npdb_guid=%s\n\n[rva]\n", b.id.c_str());
    for (auto& [k, v] : rva) fprintf(f, "%ls=0x%llX\n", k.c_str(), (unsigned long long)v);
    fprintf(f, "\n[layout]\nAudioSource_ClipPPtr=0x%llX\n", (unsigned long long)clip);
    fclose(f);
    Log("symbols: resolved %d functions for build %s", (int)rva.size(), b.id.c_str());
    return true;
}
