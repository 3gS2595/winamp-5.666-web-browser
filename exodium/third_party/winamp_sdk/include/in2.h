// Winamp input plug-in interface, as laid out in the Winamp 5 SDK's in2.h.
//
// Written out here for in_exodium.dll (which needs only In_Module, not out.h), since the
// copy of the SDK in this folder had only the general purpose plug-in headers. The field
// order is what Winamp reads and must not change.
#pragma once

#include <windows.h>

#define IN_UNICODE 0x0F000000

#ifdef UNICODE_INPUT_PLUGIN
#define in_char wchar_t
#define IN_VER (IN_UNICODE | 0x100)
#else
#define in_char char
#define IN_VER 0x100
#endif

// Room Winamp gives GetFileInfo for a title, in characters.
#define GETFILEINFO_TITLE_LENGTH 2048

struct Out_Module; // out.h; only ever a pointer here

typedef struct {
    int version;       // IN_VER
    char* description; // with its version

    HWND hMainWindow;       // filled in by Winamp
    HINSTANCE hDllInstance; // filled in by Winamp

    char* FileExtensions; // "mp3\0Layer 3 MPEG\0..." pairs, ending in an empty one

    int is_seekable;
    int UsesOutputPlug; // whether it plays through Winamp's output plug-ins; never changes

    void (*Config)(HWND hwndParent);
    void (*About)(HWND hwndParent);

    void (*Init)(void);
    void (*Quit)(void);

    void (*GetFileInfo)(const in_char* file, in_char* title, int* length_in_ms); // file NULL or "" is the one playing
    int (*InfoBox)(const in_char* file, HWND hwndParent);

    int (*IsOurFile)(const in_char* fn); // before extensions are looked at; for such as mms://

    int (*Play)(const in_char* fn); // 0 on success, -1 file not found, anything else stops Winamp
    void (*Pause)(void);
    void (*UnPause)(void);
    int (*IsPaused)(void);
    void (*Stop)(void);

    int (*GetLength)(void);              // in ms
    int (*GetOutputTime)(void);          // in ms
    void (*SetOutputTime)(int time_in_ms); // seek

    void (*SetVolume)(int volume); // 0 to 255
    void (*SetPan)(int pan);       // -127 to 127

    // Filled in by Winamp, called by the plug-in: the built-in analyzer's data...
    void (*SAVSAInit)(int maxlatency_in_ms, int srate); // in Play()
    void (*SAVSADeInit)(void);                          // in Stop()
    void (*SAAddPCMData)(void* PCMData, int nch, int bps, int timestamp); // at least 576 samples
    int (*SAGetMode)(void);
    int (*SAAdd)(void* data, int timestamp, int csa);

    // ...and the visualization plug-ins'.
    void (*VSAAddPCMData)(void* PCMData, int nch, int bps, int timestamp); // at least 576 samples
    int (*VSAGetMode)(int* specNch, int* waveNch);
    int (*VSAAdd)(void* data, int timestamp);
    void (*VSASetInfo)(int srate, int nch); // in Play()

    // Filled in by Winamp: DSP plug-ins.
    int (*dsp_isactive)(void);
    int (*dsp_dosamples)(short int* samples, int numsamples, int bps, int nch, int srate);

    void (*EQSet)(int on, char data[10], int preamp); // the plug-in's; may do nothing

    void (*SetInfo)(int bitrate, int srate, int stereo, int synched); // filled in by Winamp

    struct Out_Module* outMod; // filled in by Winamp
} In_Module;

// Exported by an input plug-in as winampGetInModule2.
typedef In_Module* (*winampGetInModule2Type)(void);
