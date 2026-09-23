// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Audio streaming diagnostics for the Nintendo 64 port (hardware triage).
//
// Dormant unless the ROM carries the marker file rom:/audiodiag (staged by
// `N64_AUDIO_DIAG=1 ./build-n64.sh`). When enabled:
//   - boot: a PI read self-test compares every read path the wav64 streams use
//     (dma_read into misaligned RAM, CPU 16/32-bit cart reads, Opus frame
//     walks and VADPCM chunk reads through read()) against plain aligned DMA,
//     then shows the results until START;
//   - in game: every Opus/VADPCM stream read is checked against the same
//     reference and a HUD line shows channel state and error counters.
// Everything is also logged with debugf (ISViewer / flashcart USB).

#ifndef LIB_SYSTEM_N64_AUDIODIAG_H
#define LIB_SYSTEM_N64_AUDIODIAG_H

#include <libdragon.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int musicOpen, musicPlaying, musicPaused, musicVolume;
    float musicPos;
    int voiceOpen, voicePlaying, voiceVolume, samplesPaused;
    float voicePos;
} N64AudioStatus;

// Implemented by the sound backend (AIL/N64/SOUND_N64_BACKEND.CPP).
void N64AudioGetStatus(N64AudioStatus *st);
void N64AudioTest_Voice(const char *base); // rom:/vox/<base>.wav64, game path
void N64AudioTest_Music(const char *name); // engine stream name, game path
void N64AudioTest_Stop(void);

int N64AudioDiag_Enabled(void);            // after dfs_init
void N64AudioDiag_BootTest(void);          // no-op unless enabled
void N64AudioDiag_WrapStream(wav64_t *wav, int isVoice); // after wav64_play
void N64AudioDiag_StreamClosed(wav64_t *wav);
void N64AudioDiag_PumpTick(void);          // start of AudioStreamPump
void N64AudioDiag_Hud(void);               // inside the frame's rdpq attach

#ifdef __cplusplus
}
#endif

#endif // LIB_SYSTEM_N64_AUDIODIAG_H
