#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <string>

// Makes sure <dir>\offsets.ini matches the loaded UnityPlayer.dll, regenerating
// it from Unity's public symbols if the game was updated. False = can't hook.
bool Symbols_Ensure(HMODULE unityPlayer, const std::wstring& dir);
