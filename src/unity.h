#pragma once
#include <cstdint>
#include <string>

// Native UnityPlayer.dll helpers (addresses resolved from offsets.ini).
struct UnityAddrs {
    uintptr_t pptrDeref, transformGetPosition, findMainCamera, worldToClip, componentGetName;
};
void Unity_Init(const UnityAddrs& a);
void* Unity_Deref(int instanceId);
int Unity_InstanceId(const void* obj);
// Screen position (px) of the AudioSource's GameObject, raised by `up` world units.
// Main thread only.
bool Unity_SpeakerScreenPos(int srcInstanceId, float up, float* x, float* y);
// "Amechan01" etc. (empty if unknown). Main thread only.
std::string Unity_CharacterId(void* src);
