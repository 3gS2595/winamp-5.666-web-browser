// How far ahead of the time it is playing the visualization plug-in Winamp runs asks for
// its sound.
//
// Winamp hands a visualization plug-in, for each picture, the sound for the time the input
// plug-in says it is playing plus the plug-in's own latencyMs: how long after drawing its
// picture is seen. A player that decodes ahead of what is heard has that sound already.
// in_exodium.dll has only what has been heard, so it says it is playing that much earlier
// than it would otherwise, or that plug-in would be asking for sound there is none of yet.
// MilkDrop 2 asks for none ahead, AVS for 14 ms, Nullsoft's classic plug-in for 10; until
// 2026-11-07, when they weren't allowed for, AVS was given no sound, or sound from seconds
// before, in half its pictures.
//
// Winamp doesn't tell an input plug-in which visualization plug-in runs, so this finds out
// for itself: Winamp loads one only to run it (or, in Preferences, to have it set up), and
// fills in its module's hwndParent and hDllInstance first, which no other memory in that
// DLL holds side by side. Nothing of the plug-in is run to find it, and its memory is only
// ever copied, so one that Winamp unloads at that very moment leaves it nothing to read,
// never a crash.
#pragma once

#include <windows.h>

#include <cstddef>
#include <string>
#include <vector>

namespace exodium {

class VisPlugins {
public:
    // Finds the plug-ins there are: the vis_*.dll files in `folder`, which is where Winamp
    // loads them from.
    void Find(const wchar_t* folder);
    // The latencyMs of the one running in the Winamp whose main window is `winamp`, or 0 when
    // none runs; with more than one about, the most of theirs. Where it found a plug-in's
    // module it looks first the next time, which takes microseconds; looking a plug-in
    // through takes up to a few ms.
    [[nodiscard]] int RunningLatencyMs(HWND winamp) noexcept;

private:
    struct Plugin {
        std::wstring file;
        HMODULE module = nullptr;       // where it was loaded when last looked for
        const std::byte* at = nullptr;  // its module Winamp set up, there
        int searches = 0;               // how often it was looked through there
    };
    std::vector<Plugin> plugins_;
};

} // namespace exodium
