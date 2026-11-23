// Loads dmsubs\subtitles.json:
// { "lines": { "<clip name>": { "jp": "...", "en": "...",
//                               "cues": [[0.0, "first part"], [2.4, "second part"]] }, ... } }
// Keys may be "<character>/<clip>" (e.g. "Amechan01/38_F2A") or just "<clip>".
// Display uses "cues" if present, else "en", else "en_auto" (machine translation).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "subtitles.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <unordered_map>

void Log(const char* fmt, ...);

namespace {

std::wstring g_path;
FILETIME g_mtime{};
std::mutex g_mx;
std::unordered_map<std::string, std::shared_ptr<const Line>> g_lines;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// --- minimal JSON reader ------------------------------------------------------
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    double n = 0;
    std::string s;
    std::vector<J> a;
    std::vector<std::pair<std::string, J>> o;
    const J* get(const char* k) const {
        for (auto& kv : o)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct Parser {
    const char* p;
    const char* e;
    bool err = false;
    void ws() { while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p; }
    static void put(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | cp >> 6); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | cp >> 12); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | cp >> 18); o += (char)(0x80 | (cp >> 12 & 63)); o += (char)(0x80 | (cp >> 6 & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4 && p < e; ++i, ++p) {
            char c = *p;
            v = v * 16 + (c >= '0' && c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
        }
        return v;
    }
    std::string str() {
        std::string o;
        ++p;  // opening quote
        while (p < e && *p != '"') {
            if (*p == '\\' && p + 1 < e) {
                ++p;
                char c = *p++;
                switch (c) {
                    case 'n': o += '\n'; break;
                    case 't': o += '\t'; break;
                    case 'r': break;
                    case 'b': case 'f': break;
                    case 'u': {
                        unsigned cp = hex4();
                        if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < e && p[0] == '\\' && p[1] == 'u') {
                            p += 2;
                            unsigned lo = hex4();
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        put(o, cp);
                        break;
                    }
                    default: o += c;
                }
            } else {
                o += *p++;
            }
        }
        if (p < e) ++p; else err = true;
        return o;
    }
    J val() {
        J v;
        ws();
        if (p >= e) { err = true; return v; }
        char c = *p;
        if (c == '{') {
            v.t = J::Obj;
            ++p;
            ws();
            if (p < e && *p == '}') { ++p; return v; }
            while (!err) {
                ws();
                if (p >= e || *p != '"') { err = true; break; }
                std::string k = str();
                ws();
                if (p >= e || *p != ':') { err = true; break; }
                ++p;
                v.o.emplace_back(std::move(k), val());
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; break; }
                err = true;
            }
        } else if (c == '[') {
            v.t = J::Arr;
            ++p;
            ws();
            if (p < e && *p == ']') { ++p; return v; }
            while (!err) {
                v.a.push_back(val());
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; break; }
                err = true;
            }
        } else if (c == '"') {
            v.t = J::Str;
            v.s = str();
        } else if (c == 't' || c == 'f') {
            v.t = J::Bool;
            v.n = c == 't';
            p += c == 't' ? 4 : 5;
        } else if (c == 'n') {
            p += 4;
        } else {
            v.t = J::Num;
            char* end;
            v.n = strtod(p, &end);
            if (end == p) err = true;
            p = end;
        }
        return v;
    }
};

bool ReadWhole(const std::wstring& path, std::string& out, FILETIME& mt) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    GetFileSizeEx(h, &sz);
    GetFileTime(h, nullptr, nullptr, &mt);
    out.resize((size_t)sz.QuadPart);
    DWORD rd = 0;
    BOOL ok = ::ReadFile(h, out.data(), (DWORD)out.size(), &rd, nullptr);
    CloseHandle(h);
    return ok && rd == out.size();
}

void Reload() {
    std::string text;
    FILETIME mt{};
    if (!ReadWhole(g_path, text, mt)) return;
    g_mtime = mt;
    size_t off = (text.size() >= 3 && (unsigned char)text[0] == 0xEF) ? 3 : 0;  // UTF-8 BOM
    Parser ps{text.data() + off, text.data() + text.size()};
    J root = ps.val();
    if (ps.err || root.t != J::Obj) {
        Log("subtitles.json: parse error near byte %d - keeping previous subtitles",
            (int)(ps.p - text.data()));
        return;
    }
    const J* lines = root.get("lines");
    std::unordered_map<std::string, std::shared_ptr<const Line>> m;
    int withEn = 0;
    if (lines && lines->t == J::Obj) {
        for (auto& kv : lines->o) {
            auto ln = std::make_shared<Line>();
            const J& v = kv.second;
            if (v.t == J::Str) ln->en = Widen(v.s);  // "clip": "text" shorthand
            if (const J* x = v.get("en"); x && x->t == J::Str) ln->en = Widen(x->s);
            if (const J* x = v.get("jp"); x && x->t == J::Str) ln->jp = Widen(x->s);
            if (ln->en.empty())  // machine translation is the fallback until someone writes "en"
                if (const J* x = v.get("en_auto"); x && x->t == J::Str) ln->en = Widen(x->s);
            if (const J* x = v.get("cues"); x && x->t == J::Arr) {
                for (auto& c : x->a)
                    if (c.t == J::Arr && c.a.size() >= 2 && c.a[0].t == J::Num && c.a[1].t == J::Str)
                        ln->cues.push_back({c.a[0].n, Widen(c.a[1].s)});
                std::sort(ln->cues.begin(), ln->cues.end(), [](auto& a, auto& b) { return a.t < b.t; });
            }
            if (!ln->en.empty() || !ln->cues.empty()) ++withEn;
            m[kv.first] = std::move(ln);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_lines.swap(m);
    }
    Log("subtitles.json: %d lines (%d translated)", (int)g_lines.size(), withEn);
}

}  // namespace

void Subtitles_Load(const std::wstring& dir) {
    g_path = dir + L"subtitles.json";
    Reload();
}

void Subtitles_Poll() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(g_path.c_str(), GetFileExInfoStandard, &fa)) return;
    if (CompareFileTime(&fa.ftLastWriteTime, &g_mtime) != 0) Reload();
}

std::shared_ptr<const Line> Subtitles_Find(const std::string& character, const std::string& clip) {
    std::lock_guard<std::mutex> lk(g_mx);
    for (const std::string& key : {character + "/" + clip, clip}) {
        auto it = g_lines.find(key);
        if (it != g_lines.end() && (!it->second->en.empty() || !it->second->cues.empty())) return it->second;
    }
    return nullptr;
}
