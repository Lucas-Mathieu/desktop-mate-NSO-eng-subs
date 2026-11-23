#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <memory>
#include <string>

void Overlay_Start(const std::wstring& dir);
// Main (Unity) thread: a clip started on AudioSource `src`.
struct Line;
void Overlay_OnClipStart(void* src, const std::string& clip, std::shared_ptr<const Line> line, float len, double delay);
// Main thread: AudioSource::Stop. Returns true if it ended the shown subtitle.
bool Overlay_OnSourceStop(void* src);
// Main thread, once per frame: refresh the on-screen anchor of the speaker.
void Overlay_OnFrame();
// The overlay HWND (nullptr until created).
HWND Overlay_Window();
// Any thread: throw away and recreate the overlay window (e.g. after another component restyled it).
void Overlay_RecreateWindow();
