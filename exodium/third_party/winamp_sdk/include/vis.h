// Winamp visualization plug-in interface, as laid out in the Winamp 5 SDK's vis.h.
//
// Written out here for the test probe (tests\vis_probe), since the copy of the SDK in this
// folder had only the general purpose plug-in headers. The field order is what Winamp
// reads and must not change.
#pragma once

#include <windows.h>

typedef struct winampVisModule {
    char* description;
    HWND hwndParent;        // filled in by Winamp
    HINSTANCE hDllInstance; // filled in by Winamp
    int sRate;              // filled in by Winamp
    int nCh;                // filled in by Winamp
    int latencyMs;          // from Render to the frame being seen; Winamp gets data that much ahead
    int delayMs;            // between calls to Render

    // Filled in before each Render, as many channels as asked for (0, 1 or 2).
    int spectrumNch;
    int waveformNch;
    unsigned char spectrumData[2][576];
    unsigned char waveformData[2][576]; // signed 8-bit samples, really

    void (*Config)(struct winampVisModule* this_mod);
    int (*Init)(struct winampVisModule* this_mod);   // 0 on success
    int (*Render)(struct winampVisModule* this_mod); // 0 on success, 1 to end
    void (*Quit)(struct winampVisModule* this_mod);

    void* userData;
} winampVisModule;

typedef struct {
    int version; // VIS_HDRVER
    char* description;
    winampVisModule* (*getModule)(int);
} winampVisHeader;

#define VIS_HDRVER 0x101

// Exported by a visualization plug-in as winampVisGetHeader.
typedef winampVisHeader* (*winampVisGetHeaderType)(HWND);
