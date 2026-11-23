#pragma once
#include <memory>
#include <string>
#include <vector>

struct Cue {
    double t;           // seconds from clip start
    std::wstring text;  // empty = hide
};

struct Line {
    std::wstring en;        // whole-line English (used when there are no cues)
    std::wstring jp;        // original Japanese (optional display)
    std::vector<Cue> cues;  // optional timed parts, sorted by t
};

void Subtitles_Load(const std::wstring& dir);
// Reloads subtitles.json if it changed on disk. Cheap; call ~1/s.
void Subtitles_Poll();
// Looks up "<character>/<clip>" then "<clip>". Returns nullptr if there is no English yet.
std::shared_ptr<const Line> Subtitles_Find(const std::string& character, const std::string& clip);
