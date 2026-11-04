// What the plugin and in_exodium.dll both know about each other. in_exodium.dll is the
// input plug-in that gives Winamp's visualizers the sound Exodium's pages make; it has
// to be a DLL of its own, since Winamp only takes input plug-ins from in_*.dll files.
// Both are loaded into Winamp's process and talk by plain function calls.
#pragma once

namespace exodium {

// What Winamp is given to play. in_exodium.dll takes anything that starts with the scheme.
inline constexpr wchar_t kSoundUrl[] = L"exodium://sound";
inline constexpr wchar_t kSoundScheme[] = L"exodium://";
inline constexpr wchar_t kSoundPluginFile[] = L"in_exodium.dll";

// in_exodium.dll's export that sets the title Winamp shows for the sound, which is the
// page's. Called on Winamp's UI thread; Winamp asks again for the title on IPC_UPDTITLE.
inline constexpr char kSetSoundTitleExport[] = "ExodiumSetSoundTitle";
using SetSoundTitleFunction = void(__cdecl*)(const wchar_t* title);

// in_exodium.dll's export that says whether what Winamp plays, or is paused on, is the
// pages' sound: between Winamp calling its Play and its Stop. Winamp's thread only.
inline constexpr char kSoundPlayingExport[] = "ExodiumSoundPlaying";
using SoundPlayingFunction = int(__cdecl*)();

} // namespace exodium
