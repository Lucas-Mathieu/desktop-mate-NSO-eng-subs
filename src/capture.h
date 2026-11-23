#pragma once
#include <string>

// Records the game's own audio output (WASAPI process loopback) into a ring
// buffer so a voice line can be saved as a WAV for transcription.
void Capture_Start(const std::wstring& clipDir);
// Save [start, start+len] (DmNow() clock) to <clipDir>\<relPath>.wav once it has been captured.
void Capture_Request(const std::string& relPath, double start, double len);
