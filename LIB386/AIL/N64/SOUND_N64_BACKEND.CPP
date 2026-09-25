// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Nintendo 64 (libdragon) audio backend: SAMPLE (SFX/voice) + STREAM (music/
// jingles) + CD (routed through the stream), same public contract as the
// NULL/WIIU backends. Everything runs on the game thread: AudioStreamPump()
// (called from ManageTime every frame) polls the RSP mixer and reaps
// finished voices, so no locking is needed.
//
// Channel map: 0..11 = SFX voices (engine expects 12), 12(+13) = music
// (wav64 from rom:/music/, VADPCM-compressed, RSP-decoded; mono uses ch 12,
// the +13 slot stays reserved for a future stereo track).

#include <AIL.H>

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/N64_AUDIODIAG.H>
#include <SYSTEM/TIMER.H>

#include <libdragon.h>

#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NUM_VOICES 12
#define CH_MUSIC NUM_VOICES        // 12 (music is mono VADPCM; 13 stays reserved)
#define CH_VOICE (NUM_VOICES + 2)  // 14 — dedicated speech channel (Opus VOX clips)
#define NUM_CHANNELS (NUM_VOICES + 3)
#define OUTPUT_RATE 32000

// Engine's fixed userhandle for the spoken-voice sample (MESSAGE.CPP). The
// engine passes it to PlaySample AND queries IsSamplePlaying/StopOneSample with
// it, so we treat it as a channel selector, not a normal sample.
#define SPEAK_SAMPLE 0x1234
#define SAMPLE_RATE 44100          // raw-buffer fallback rate (WiiU parity)
#define PITCHBEND_NORMAL 4096
#define SFX_MAX_FREQ 48000        // per-channel mixer ceiling of the SFX voices

S32 Sample_Driver_Enabled = FALSE;
S32 sampleVolume = 127;
S32 streamVolume = 127;
S32 UseWaveMixer = FALSE;

static int s_audioInit = 0;
static int s_sfxLog = 0;
static S32 s_samplesPaused = 0;
static int s_inverseStereo = 0;

// Sample fade (FadeOut/FadeIn over N ms, applied as a master factor)
static int s_fadeDir = 0; // 0 = none, -1 = out, +1 = in
static U32 s_fadeStart = 0;
static S32 s_fadeMs = 0;
static float s_fadeFactor = 1.0f;

/* ── Voice pool ──────────────────────────────────────────────────────────── */

struct N64Voice {
    int active;
    U32 handle;
    U32 userhandle;
    U32 startTimer;    // TimerSystemHR at start; IsSamplePlaying truth value
    U8 *buf;           // owned converted payload (signed 8 or native s16)
    S32 lenSamples;
    S32 bits;          // 8 or 16
    S32 rateHz;
    S32 pitch;         // 4096 = normal
    S32 volume;        // 0..127
    S32 pan;           // 0..127
    S32 repeatsLeft;   // 0 = infinite loop; >=1 = plays remaining
    float lastPos;     // loop-wrap detection for finite repeats
    waveform_t *curWave; // -> s_waves[slot][sel] handed to mixer_ch_play
};

static N64Voice s_voices[NUM_VOICES];
static U32 s_handleCounter = 1;

// libdragon's mixer_ch_play only reconfigures the channel (length, bits,
// frequency, read callback) when the waveform POINTER differs from the one the
// channel last played (`if (wave != sbuf->wv_ctx)`). It treats a waveform_t as
// an immutable descriptor. We synthesise a fresh descriptor per SFX, so reusing
// one struct per slot made libdragon keep the PREVIOUS sample's length/rate —
// a long sample fired right after a footstep inherited the footstep's tiny
// length and stopped instantly (served=0, VoiceRead never called). Alternate
// two descriptors per slot so consecutive plays always present a new pointer
// and force a full reconfigure. (Kept outside N64Voice: VoiceFree memsets the
// voice, which would reset the selector and defeat the alternation.)
static waveform_t s_waves[NUM_VOICES][2];
static U8 s_waveSel[NUM_VOICES];

// Mixer read callback: serve samples from the owned memory buffer.
static void VoiceRead(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
    (void)seeking;
    N64Voice *v = (N64Voice *)ctx;
    int bytesPerSample = v->bits / 8;
    if (wpos >= v->lenSamples) wpos = v->lenSamples;
    int avail = v->lenSamples - wpos;
    int n = (wlen < avail) ? wlen : avail;
    if (n <= 0) return;
    void *dst = samplebuffer_append(sbuf, n);
    // samplebuffer_append returns an uncached pointer (libdragon backs the
    // channel buffer with malloc_uncached), so this CPU memcpy commits straight
    // to RDRAM — no cache writeback needed.
    memcpy(dst, v->buf + (size_t)wpos * bytesPerSample, (size_t)n * bytesPerSample);
}

static void VoiceApplyVolume(int slot) {
    N64Voice *v = &s_voices[slot];
    if (!v->active) return;
    float master = ((float)sampleVolume / 127.0f) * s_fadeFactor;
    if (s_samplesPaused > 0) master = 0.0f;
    float vol = master * ((float)v->volume / 127.0f);
    S32 pan = v->pan;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    float l = (pan <= 64) ? 1.0f : (float)(127 - pan) / 63.0f;
    float r = (pan >= 64) ? 1.0f : (float)pan / 63.0f;
    if (s_inverseStereo) {
        float t = l;
        l = r;
        r = t;
    }
    mixer_ch_set_vol(slot, vol * l, vol * r);
}

// Playback rate for a pitch-bent SFX, clamped to the channel ceiling set in
// EnsureAudioInit. Animation scripts carry their own frequency value (FICHE.CPP
// F_NEW_SAMPLE) and some push past it — the vacuum cleaner in Ker'aooc's house
// (cube 35) asks for 48.5 kHz on a 22 kHz sample — which is a libdragon assert
// (a crash on hardware), not a mixer that quietly saturates like MSS did.
static void VoiceSetFreq(int slot, S32 hz, S32 pitchbend) {
    float f = (float)hz * (float)pitchbend / (float)PITCHBEND_NORMAL;
    if (f > (float)SFX_MAX_FREQ) f = (float)SFX_MAX_FREQ;
    mixer_ch_set_freq(slot, f);
}

static void VoiceFree(int slot) {
    N64Voice *v = &s_voices[slot];
    if (mixer_ch_playing(slot)) mixer_ch_stop(slot);
    if (v->buf) free(v->buf);
    memset(v, 0, sizeof(*v));
}

/* ── VOC parser (ported from AIL/WIIU, itself derived from AIL/SDL) ──────── */

#define VOC_HEADER_MAGIC "reative Voice File\x1A"

static int ParseVocIfPresent(const U8 *buffer, S32 sizeBytes,
                             const U8 **data, S32 *payloadSize, S32 *outHz, int *outIs8bit) {
    if (sizeBytes < 26)
        return 0;
    if (buffer[0] != 'C' && buffer[0] != 'W' && buffer[0] != 0)
        return 0;
    if (memcmp(buffer + 1, VOC_HEADER_MAGIC, 19) != 0)
        return 0;
    *outHz = SAMPLE_RATE;
    *outIs8bit = 1;
    S32 pos = 26;
    S32 extendedRate = 0;
    while (pos + 4 <= sizeBytes) {
        U8 type = buffer[pos];
        S32 blockSize = (S32)(buffer[pos + 1] | (buffer[pos + 2] << 8) | (buffer[pos + 3] << 16));
        if (type == 0x00)
            break;
        if (pos + 4 + blockSize > sizeBytes)
            break;
        if (type == 0x08 && blockSize >= 4) {
            S32 freqDiv = (S32)(buffer[pos + 4] | (buffer[pos + 5] << 8));
            int ch = (int)(buffer[pos + 7]) + 1;
            if (ch < 1 || ch > 8)
                ch = 1;
            if (freqDiv != 65535 && ch > 0)
                extendedRate = 256000000 / ((65536 - freqDiv) * ch);
        }
        if (type == 0x01 && blockSize >= 2) {
            U8 freqDiv = buffer[pos + 4];
            U8 codec = buffer[pos + 5];
            if (extendedRate > 0)
                *outHz = extendedRate;
            else if (freqDiv != 255)
                *outHz = 1000000 / (256 - (S32)freqDiv);
            if (*outHz <= 0 || *outHz > 192000)
                *outHz = SAMPLE_RATE;
            *data = buffer + pos + 6;
            *payloadSize = blockSize - 2;
            *outIs8bit = (codec == 0) ? 1 : 0;
            return 1;
        }
        pos += 4 + blockSize;
    }
    return 0;
}

/* ── WAV parser (ported from AIL/WIIU; permissive first byte for .VOX) ──── */

static void SkipWavHeaderIfPresent(const U8 *buffer, S32 sizeBytes,
                                   const U8 **data, S32 *payloadSize, int *hadWav,
                                   S32 *outWavHz, S32 *outWavChannels, S32 *outWavBits,
                                   S32 *outFmtTag) {
    *data = buffer;
    *payloadSize = sizeBytes;
    *hadWav = 0;
    *outWavHz = SAMPLE_RATE;
    *outWavChannels = 1;
    *outWavBits = 16;
    *outFmtTag = 1;
    if (sizeBytes < 12)
        return;
    if ((memcmp(buffer, "RIFF", 4) != 0 || memcmp(buffer + 8, "WAVE", 4) != 0) &&
        (buffer[1] != 'I' || buffer[2] != 'F' || buffer[3] != 'F' || memcmp(buffer + 8, "WAVE", 4) != 0))
        return;
    S32 pos = 12;
    S32 wavHz = SAMPLE_RATE, wavChannels = 1, wavBits = 16, fmtTag = 1, blkAlign = 0;
    while (pos + 8 <= sizeBytes) {
        S32 chunkSize = (S32)(buffer[pos + 4] | (buffer[pos + 5] << 8) |
                              (buffer[pos + 6] << 16) | (buffer[pos + 7] << 24));
        if (memcmp(buffer + pos, "fmt ", 4) == 0 && chunkSize >= 16 && pos + 8 + 16 <= sizeBytes) {
            const U8 *f = buffer + pos + 8;
            fmtTag = (S32)(f[0] | (f[1] << 8));
            wavChannels = (S32)(f[2] | (f[3] << 8));
            wavHz = (S32)(f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24));
            blkAlign = (S32)(f[12] | (f[13] << 8));
            wavBits = (S32)(f[14] | (f[15] << 8));
            if (wavHz <= 0 || wavHz > 192000)
                wavHz = SAMPLE_RATE;
            if (wavChannels <= 0 || wavChannels > 8)
                wavChannels = 1;
            if (fmtTag == 1) {
                if (blkAlign > 0 && wavChannels > 0)
                    wavBits = (blkAlign * 8) / wavChannels;
                if (wavBits != 8 && wavBits != 16)
                    wavBits = 16;
            }
        }
        if (memcmp(buffer + pos, "data", 4) == 0 && chunkSize > 0 && pos + 8 + chunkSize <= sizeBytes) {
            *data = buffer + pos + 8;
            *payloadSize = chunkSize;
            *hadWav = 1;
            *outWavHz = wavHz;
            *outWavChannels = wavChannels;
            *outWavBits = wavBits;
            *outFmtTag = fmtTag;
            return;
        }
        pos += 8 + ((chunkSize + 1) & ~1);
    }
}

/* ── COMMON / MIDI ───────────────────────────────────────────────────────── */

void InitAIL(void) {}

S32 InitMidiDriver(char *driver_name) {
    (void)driver_name;
    return TRUE;
}

/* ── SAMPLE API ──────────────────────────────────────────────────────────── */

static void EnsureAudioInit(void) {
    if (s_audioInit) return;
    audio_init(OUTPUT_RATE, 4);
    mixer_init(NUM_CHANNELS);
    // Music .wav64 are VADPCM (level 1, RSP-decoded, plays at source rate <32 kHz
    // so no channel-limit tweak needed). Voice .wav64 are Opus, which needs the
    // level-3 decoder registered and always decodes at 48 kHz, so raise the
    // voice channel's frequency ceiling. Opus voice decode runs on the CPU, but
    // speech is intermittent (one clip during a mostly-static dialogue), so the
    // cost lands where there is frame-time headroom.
    wav64_init_compression(3);
    mixer_ch_set_limits(CH_VOICE, 16, 48000, 0);
    // SFX voices: without an explicit limit libdragon sizes the channel's
    // resample buffer for the mixer's own output rate (32 kHz). LBA2 samples
    // are 11/22 kHz but the engine pitch-bends many of them UP (pickups, the
    // thrown magic ball, etc.), pushing the effective playback frequency above
    // 32 kHz — the undersized buffer then aliases/garbles the sound. Raise the
    // ceiling to 48 kHz so pitch-bent one-shots resample cleanly.
    for (int i = 0; i < NUM_VOICES; i++)
        mixer_ch_set_limits(i, 16, SFX_MAX_FREQ, 0);
    s_audioInit = 1;
    LogPrintf("N64 AUDIO: mixer up (%d ch @ %d Hz)\n", NUM_CHANNELS, OUTPUT_RATE);
}

S32 InitSampleDriver(char *driver_name) {
    (void)driver_name;
    EnsureAudioInit();
    Sample_Driver_Enabled = TRUE;
    return TRUE;
}

void SetMasterVolumeSample(S32 volume) {
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    sampleVolume = volume;
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
}

// Engine spin-loops on these (AMBIANCE.CPP): `while (FadeOutSamples(200));`
// and `while (FadeInSamples(200) != 200);`. So: FadeOut returns the REMAINING
// ms (reaches 0), FadeIn returns the ELAPSED ms clamped to delay (reaches
// delay). Time must come straight from the hardware counter — TimerSystemHR
// is frozen inside those loops (nobody calls ManageTime there); using it
// hangs the boot right after the Introduction. Also keep the audio buffers
// fed while spinning.
void AudioStreamPump(void); // forward (defined below)

S32 FadeOutSamples(S32 delay) {
    U32 now = (U32)get_ticks_ms();
    S32 ms = (delay > 0) ? delay : 0;
    if (s_fadeDir != -1 || s_fadeMs != ms) {
        s_fadeDir = -1;
        s_fadeStart = now;
        s_fadeMs = ms;
    }
    S32 elapsed = (S32)(now - s_fadeStart);
    S32 remain = s_fadeMs - elapsed;
    if (remain <= 0) {
        s_fadeFactor = 0.0f;
        remain = 0;
    } else {
        s_fadeFactor = (float)remain / (float)s_fadeMs;
    }
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
    AudioStreamPump();
    return remain;
}

S32 FadeInSamples(S32 delay) {
    U32 now = (U32)get_ticks_ms();
    S32 ms = (delay > 0) ? delay : 0;
    if (s_fadeDir != 1 || s_fadeMs != ms) {
        s_fadeDir = 1;
        s_fadeStart = now;
        s_fadeMs = ms;
    }
    S32 elapsed = (S32)(now - s_fadeStart);
    if (s_fadeMs <= 0 || elapsed >= s_fadeMs) {
        s_fadeFactor = 1.0f;
        elapsed = s_fadeMs;
    } else {
        s_fadeFactor = (float)elapsed / (float)s_fadeMs;
    }
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
    AudioStreamPump();
    return elapsed;
}

void InverseStereoSample(S32 inverse) {
    s_inverseStereo = (inverse != 0);
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
}

static int StealVoiceSlot(void) {
    // Free slot first.
    for (int i = 0; i < NUM_VOICES; i++)
        if (!s_voices[i].active) return i;
    // Oldest one-shot, then oldest finite, then oldest overall (WiiU order).
    int best = -1;
    for (int pass = 0; pass < 3 && best < 0; pass++) {
        U32 bestTimer = 0xFFFFFFFFu;
        for (int i = 0; i < NUM_VOICES; i++) {
            N64Voice *v = &s_voices[i];
            int match = (pass == 0) ? (v->repeatsLeft == 1)
                      : (pass == 1) ? (v->repeatsLeft >= 1)
                                    : 1;
            if (match && v->startTimer < bestTimer) {
                bestTimer = v->startTimer;
                best = i;
            }
        }
    }
    return best;
}

/* ── VOICE (speech) — Opus wav64 streamed on CH_VOICE ─────────────────────── */
// The engine's speech path (MESSAGE.CPP::PlaySpeakVoc) hands us a placeholder
// buffer "W<basename>\0" through PlaySample(userhandle == SPEAK_SAMPLE). Rather
// than decode it as a sample, we stream rom:/vox/<basename>.wav64 (a small Opus
// clip produced by tools-n64/vox_repack.py) on the dedicated voice channel.
// IsSamplePlaying/StopOneSample(SPEAK_SAMPLE) then drive this channel, so the
// engine's Speak()/TestSpeak() continuation logic works unchanged.
static wav64_t s_voiceWav;
static int s_voiceOpen = 0;
static S32 s_voiceVol = 127;
static S32 s_voicePan = 64;

static void VoiceApplyVol(void) {
    if (!s_voiceOpen) return;
    float v = (s_samplesPaused > 0) ? 0.0f : (float)s_voiceVol / 127.0f;
    S32 pan = s_voicePan;
    if (pan < 0) pan = 0;
    if (pan > 127) pan = 127;
    float l = (pan <= 64) ? 1.0f : (float)(127 - pan) / 63.0f;
    float r = (pan >= 64) ? 1.0f : (float)pan / 63.0f;
    if (s_inverseStereo) { float t = l; l = r; r = t; }
    mixer_ch_set_vol(CH_VOICE, v * l, v * r);
}

static void StopVoice(void) {
    if (s_voiceOpen) {
        if (mixer_ch_playing(CH_VOICE)) mixer_ch_stop(CH_VOICE);
        N64AudioDiag_StreamClosed(&s_voiceWav);
        wav64_close(&s_voiceWav);
        s_voiceOpen = 0;
    }
}

static U32 PlayVoice(const char *bufW, S32 volume, S32 pan) {
    EnsureAudioInit();
    StopVoice();
    // bufW points at BufSpeak: byte 0 is the 'W' the engine wrote over
    // FlagNextVoc, the wav64 basename follows. Sanity-guard a stray buffer.
    const char *base = bufW + 1;
    if (base[0] < 'a' || base[0] > 'z') return 0xFFFFFFFFu;
    char path[ADELINE_MAX_PATH];
    snprintf(path, sizeof(path), "rom:/vox/%s.wav64", base);
    FILE *f = fopen(path, "rb");
    if (!f) { LogPrintf("[voice] missing %s\n", path); return 0xFFFFFFFFu; }
    fclose(f);
    wav64_open(&s_voiceWav, path);
    s_voiceOpen = 1;
    s_voiceVol = (volume > 127) ? 127 : ((volume < 0) ? 0 : volume);
    s_voicePan = pan;
    wav64_play(&s_voiceWav, CH_VOICE);
    N64AudioDiag_WrapStream(&s_voiceWav, 1);
    VoiceApplyVol();
    return SPEAK_SAMPLE;
}

U32 PlaySample(void *buffer, S32 sizeBytes, U32 userhandle, S32 pitchbend,
               S32 repeat, S32 volume, S32 pan) {
    if (!Sample_Driver_Enabled || !buffer || sizeBytes <= 0)
        return 0xFFFFFFFFu;

    if (userhandle == SPEAK_SAMPLE)
        return PlayVoice((const char *)buffer, volume, pan);
    if (pitchbend <= 0) pitchbend = PITCHBEND_NORMAL;

    const U8 *data = (const U8 *)buffer;
    S32 payload = sizeBytes;
    S32 hz = SAMPLE_RATE;
    int is8bit = 1;
    S32 channels = 1;

    if (!ParseVocIfPresent((const U8 *)buffer, sizeBytes, &data, &payload, &hz, &is8bit)) {
        int hadWav = 0;
        S32 wavHz, wavCh, wavBits, fmtTag;
        SkipWavHeaderIfPresent((const U8 *)buffer, sizeBytes, &data, &payload,
                               &hadWav, &wavHz, &wavCh, &wavBits, &fmtTag);
        if (hadWav) {
            if (fmtTag != 1) {
                LogPrintf("[sfx] unsupported WAV fmt=%d (IMA ADPCM TODO)\n", (int)fmtTag);
                return 0xFFFFFFFFu;
            }
            hz = wavHz;
            is8bit = (wavBits == 8);
            channels = (wavCh == 2) ? 2 : 1;
            if (channels == 2) {
                // Mixer stereo SFX would eat two voice channels; downmix later
                // if it ever shows up. LBA2 samples are mono in practice.
                LogPrintf("[sfx] stereo WAV downmix not implemented; playing L only\n");
                channels = 1;
            }
        }
    }

    int slot = StealVoiceSlot();
    if (slot < 0) return 0xFFFFFFFFu;
    VoiceFree(slot);
    N64Voice *v = &s_voices[slot];

    S32 srcBps = is8bit ? 1 : 2;
    S32 nSamples = payload / srcBps;
    if (nSamples <= 0) return 0xFFFFFFFFu;

    // Always store voices as signed 16-bit mono. libdragon's RSP mixer requires
    // 2-byte-aligned sample positions; an 8-bit waveform played with pitch makes
    // the mixer request odd-length chunks, so samplebuffer wpos eventually lands
    // on an odd byte and samplebuffer_append() asserts (buf->wpos:odd -> crash in
    // VoiceRead). Widening 8-bit to 16-bit makes every position 2-byte aligned by
    // construction, killing that whole class of crash at no audible cost.
    v->buf = (U8 *)malloc((size_t)nSamples * 2);
    if (!v->buf) return 0xFFFFFFFFu;

    {
        S16 *dst = (S16 *)v->buf;
        if (is8bit) {
            // VOC/raw 8-bit is unsigned: center to signed, then widen (<<8).
            for (S32 i = 0; i < nSamples; i++)
                dst[i] = (S16)((S16)((S8)(data[i] ^ 0x80)) << 8);
        } else {
            // 16-bit payloads are LE on disk; convert to native.
            for (S32 i = 0; i < nSamples; i++)
                dst[i] = (S16)(data[i * 2] | (data[i * 2 + 1] << 8));
        }
    }

    v->active = 1;
    v->userhandle = userhandle & 0xFFFFu;
    // Canonical AIL handle layout (MILES/SDL/WIIU): [counter:8][userhandle:16]
    // [slot:8], counter >= 1 so a full handle is always >= 0x1000000 and can
    // be told apart from a bare sample number (see FindVoice).
    if ((s_handleCounter & 0xFFu) == 0) s_handleCounter++;
    v->handle = ((s_handleCounter++ & 0xFFu) << 24) | (v->userhandle << 8) | (U32)slot;
    v->startTimer = TimerSystemHR ? TimerSystemHR : 1;
    v->lenSamples = nSamples;
    v->bits = 16; // always widened to 16-bit (see buffer conversion above)
    v->rateHz = hz;
    v->pitch = pitchbend;
    v->volume = (volume > 127) ? 127 : ((volume < 0) ? 0 : volume);
    v->pan = pan;
    v->repeatsLeft = repeat; // 0 = infinite
    v->lastPos = 0.0f;

    // Alternate the two per-slot descriptors so the pointer always changes and
    // mixer_ch_play reconfigures the channel for THIS sample (see s_waves note).
    U8 sel = (U8)(s_waveSel[slot] ^ 1);
    s_waveSel[slot] = sel;
    waveform_t *w = &s_waves[slot][sel];
    v->curWave = w;

    memset(w, 0, sizeof(*w));
    w->name = "sfx";
    w->bits = (uint8_t)v->bits;
    w->channels = 1;
    w->frequency = (float)hz;
    w->len = nSamples;
    w->loop_len = (repeat != 1) ? nSamples : 0; // full-waveform loop
    w->read = VoiceRead;
    w->ctx = v;

    mixer_ch_play(slot, w);
    VoiceSetFreq(slot, hz, pitchbend);
    VoiceApplyVolume(slot);

    if (s_sfxLog) {
        LogPrintf("[sfx] slot=%d hz=%d bits=%d n=%ld pitch=%ld rep=%ld vol=%ld pan=%ld\n",
                  slot, (int)hz, (int)v->bits, (long)nSamples, (long)pitchbend,
                  (long)repeat, (long)volume, (long)pan);
    }
    return v->handle;
}

// The engine addresses voices two ways: with the full handle PlaySample
// returned (>= 0x1000000), or with the bare sample number it passed as
// userhandle (AMBIANCE.CPP: IsSamplePlaying(SAMPLE_RAIN), StopOneSample(
// SAMPLE_RAIN), HQ_ChangePanSample...). Resolving only full handles made every
// number-based query a no-op: the rain loop could never be stopped once the
// storm ended, SAMPLE_TIME_REPEAT never throttled, SampleAlways loops stacked.
static int FindVoice(U32 sample) {
    if (sample < 0x1000000u) {
        for (int i = 0; i < NUM_VOICES; i++)
            if (s_voices[i].active && s_voices[i].userhandle == sample)
                return i;
        return -1;
    }
    int slot = (int)(sample & 0xFFu);
    if (slot >= 0 && slot < NUM_VOICES && s_voices[slot].active &&
        s_voices[slot].handle == sample)
        return slot;
    return -1;
}

void ChangePitchbendSample(U32 sample, S32 pitchbend) {
    if (sample == SPEAK_SAMPLE) return; // voice clips play at native rate
    int slot = FindVoice(sample);
    if (slot < 0) return;
    if (pitchbend <= 0) pitchbend = PITCHBEND_NORMAL;
    s_voices[slot].pitch = pitchbend;
    if (mixer_ch_playing(slot))
        VoiceSetFreq(slot, s_voices[slot].rateHz, pitchbend);
}

void ChangeVolumePanSample(U32 sample, S32 volume, S32 pan) {
    if (sample == SPEAK_SAMPLE) {
        s_voiceVol = (volume > 127) ? 127 : ((volume < 0) ? 0 : volume);
        s_voicePan = pan;
        VoiceApplyVol();
        return;
    }
    int slot = FindVoice(sample);
    if (slot < 0) return;
    s_voices[slot].volume = (volume > 127) ? 127 : ((volume < 0) ? 0 : volume);
    s_voices[slot].pan = pan;
    VoiceApplyVolume(slot);
}

void StopOneSample(U32 sample) {
    if (sample == SPEAK_SAMPLE) { StopVoice(); return; }
    int slot = FindVoice(sample);
    if (slot >= 0) VoiceFree(slot);
}

void StopSamples(void) {
    for (int i = 0; i < NUM_VOICES; i++) VoiceFree(i);
    StopVoice();
    s_fadeDir = 0;
    s_fadeFactor = 1.0f;
}

void PauseSamples(void) {
    s_samplesPaused++;
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
    VoiceApplyVol();
}

void ResumeSamples(void) {
    if (s_samplesPaused > 0) s_samplesPaused--;
    for (int i = 0; i < NUM_VOICES; i++) VoiceApplyVolume(i);
    VoiceApplyVol();
}

U32 IsSamplePlaying(U32 sample) {
    if (sample == SPEAK_SAMPLE)
        return (s_voiceOpen && mixer_ch_playing(CH_VOICE))
                   ? (TimerSystemHR ? TimerSystemHR : 1u)
                   : FALSE;
    int slot = FindVoice(sample);
    if (slot < 0) return FALSE;
    if (!mixer_ch_playing(slot)) return FALSE;
    return s_voices[slot].startTimer;
}

void SfxLogEnable(int enable) {
    s_sfxLog = (enable != 0);
}

int SfxLogIsEnabled(void) {
    return s_sfxLog;
}

S32 GetSamplesPausedCount(void) {
    return s_samplesPaused;
}

/* ── STREAM API (music/jingles via wav64 from rom:/music/) ──────────────── */

static wav64_t s_music;
static int s_musicOpen = 0;
static int s_musicPaused = 0;
static char s_streamName[ADELINE_MAX_PATH]; // engine-side name (StreamName)

// Stereo waveforms own CH_MUSIC+1 as a "sub" channel: the mixer forbids
// touching it directly (volume/stop propagate from the primary channel).
static void StreamApplyVolume(void) {
    float vol = s_musicPaused ? 0.0f : (float)streamVolume / 127.0f;
    mixer_ch_set_vol(CH_MUSIC, vol, vol);
}

void StopStream(void) {
    if (s_musicOpen) {
        if (mixer_ch_playing(CH_MUSIC)) mixer_ch_stop(CH_MUSIC);
        N64AudioDiag_StreamClosed(&s_music);
        wav64_close(&s_music);
        s_musicOpen = 0;
    }
    s_musicPaused = 0;
    s_streamName[0] = '\0';
}

// A stream that reached its end must stop being "the music": GetMusic()
// (MUSIC.CPP) decides whether something plays from StreamName() alone, and
// PlayJingle() skips a jingle whose name matches it. A name that outlived its
// stream wedged the whole queue - every later PlayMusic saw cur != 0 and
// re-queued forever - so after the first jingle ran out (Twinsen's house on a
// new game) no music played again. Same fix as AIL/WIIU/STREAM_WIIU.CPP.
// Paused streams are collected too: on N64 a pause only mutes, the track
// keeps running and can end underneath it.
static void CollectDrainedStream(void) {
    if (s_musicOpen && !mixer_ch_playing(CH_MUSIC)) {
        LogPrintf("[music] ended %s%c", s_streamName, 10);
        StopStream();
    }
}

void OpenStream(void) {
    EnsureAudioInit();
    StopStream();
}

// "…/JADPCM01.WAV" -> "rom:/music/jadpcm01.wav64"
static int ResolveMusicPath(const char *name, char *out, size_t outMax) {
    const char *base = name;
    for (const char *p = name; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    char stem[64];
    size_t n = 0;
    for (; base[n] && base[n] != '.' && n < sizeof(stem) - 1; n++)
        stem[n] = (char)tolower((unsigned char)base[n]);
    stem[n] = '\0';
    // The engine's "TADPCMn" jingles are the CD tracks (GOG ships them as
    // trackn.ogg, staged here as trackn.wav64): remap the stem.
    if (strncmp(stem, "tadpcm", 6) == 0 && stem[6] >= '0' && stem[6] <= '9') {
        char digits[16];
        strncpy(digits, stem + 6, sizeof(digits) - 1);
        digits[sizeof(digits) - 1] = '\0';
        snprintf(stem, sizeof(stem), "track%s", digits);
    }
    snprintf(out, outMax, "rom:/music/%s.wav64", stem);
    FILE *f = fopen(out, "rb");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

void PlayStream(char *name) {
    if (!name || !*name) return;
    EnsureAudioInit();
    U32 t0 = (U32)get_ticks_ms();
    StopStream();
    U32 t1 = (U32)get_ticks_ms();

    char path[ADELINE_MAX_PATH];
    if (!ResolveMusicPath(name, path, sizeof(path))) {
        LogPrintf("[music] no wav64 for '%s'\n", name);
        s_streamName[0] = '\0';
        return;
    }
    U32 t2 = (U32)get_ticks_ms();

    wav64_open(&s_music, path);
    U32 t3 = (U32)get_ticks_ms();
    s_musicOpen = 1;
    s_musicPaused = 0;
    strncpy(s_streamName, name, sizeof(s_streamName) - 1);
    s_streamName[sizeof(s_streamName) - 1] = '\0';
    wav64_play(&s_music, CH_MUSIC);
    N64AudioDiag_WrapStream(&s_music, 0);
    U32 t4 = (U32)get_ticks_ms();
    StreamApplyVolume();
    LogPrintf("[music] playing %s | stop=%lums probe=%lums open=%lums play=%lums\n", path,
              (unsigned long)(t1 - t0), (unsigned long)(t2 - t1),
              (unsigned long)(t3 - t2), (unsigned long)(t4 - t3));
}

void ChangeVolumeStream(S32 volume) {
    if (volume < 0) volume = 0;
    if (volume > 127) volume = 127;
    streamVolume = volume;
    if (s_musicOpen) StreamApplyVolume();
}

S32 GetVolumeStream(void) {
    return streamVolume;
}

void PauseStream(void) {
    // V1: mute rather than pause (the mixer has no positional pause);
    // the track keeps advancing silently.
    s_musicPaused = 1;
    if (s_musicOpen) StreamApplyVolume();
    LogPrintf("[music] pause (open=%d '%s')%c", s_musicOpen, s_streamName, 10);
}

void ResumeStream(void) {
    s_musicPaused = 0;
    if (s_musicOpen) StreamApplyVolume();
    LogPrintf("[music] resume (open=%d playing=%d '%s')%c", s_musicOpen,
              s_musicOpen ? (int)mixer_ch_playing(CH_MUSIC) : 0, s_streamName, 10);
}

S32 IsStreamPlaying(void) {
    CollectDrainedStream();
    return (s_musicOpen && !s_musicPaused && mixer_ch_playing(CH_MUSIC)) ? TRUE : FALSE;
}

char *StreamName(void) {
    CollectDrainedStream();
    return s_streamName;
}

/* ── CD API (tracks live as rom:/music/trackN.wav64, GOG naming) ────────── */

#define FIRST_CD_TRACK_EU 6
#define FIRST_CD_TRACK_US 2

static S32 cdVolume = 127;
static int cdPlaying = 0;

static int BuildCDTrackPath(char *outName, size_t outMax, S32 trackOneBased) {
    char probe[ADELINE_MAX_PATH];
    snprintf(outName, outMax, "track%d.wav", (int)trackOneBased);
    if (ResolveMusicPath(outName, probe, sizeof(probe))) return 1;
    snprintf(outName, outMax, "track%02d.wav", (int)trackOneBased);
    if (ResolveMusicPath(outName, probe, sizeof(probe))) return 1;
    return 0;
}

const char *OpenCD(char *volume_name) {
    (void)volume_name;
    cdVolume = 127;
    cdPlaying = 0;
    return "D";
}

void PlayCD(S32 track) {
    char name[ADELINE_MAX_PATH];
    if (BuildCDTrackPath(name, sizeof(name), FIRST_CD_TRACK_EU + track) ||
        BuildCDTrackPath(name, sizeof(name), FIRST_CD_TRACK_US + track)) {
        LogPrintf("[CD] PlayCD track=%d -> %s\n", (int)track, name);
        PlayStream(name);
        ChangeVolumeStream(cdVolume);
        cdPlaying = 1;
    } else {
        LogPrintf("[CD] PlayCD track=%d: no track%02d/%02d wav64 in ROM\n",
                  (int)track, (int)(FIRST_CD_TRACK_EU + track),
                  (int)(FIRST_CD_TRACK_US + track));
    }
}

void ChangeVolumeCD(S32 volume) {
    cdVolume = volume;
    if (cdPlaying) ChangeVolumeStream(cdVolume);
}

S32 GetVolumeCD(void) {
    return cdVolume;
}

void StopCD(void) {
    StopStream();
    cdPlaying = 0;
}

void PauseCD(void) {
    PauseStream();
}

void ResumeCD(void) {
    ResumeStream();
}

S32 IsCDPlaying(void) {
    return (cdPlaying && IsStreamPlaying()) ? TRUE : FALSE;
}

/* ── Per-frame pump (overrides the weak no-op in N64_BACKEND.CPP) ───────── */

// Audio diagnostics ROM: drive the game's own stream paths from the boot
// report (SYSTEM/N64_AUDIODIAG.CPP), before the engine is up.
void N64AudioTest_Voice(const char *base) {
    char buf[80];
    buf[0] = 'W'; // what the engine writes over FlagNextVoc
    snprintf(buf + 1, sizeof(buf) - 1, "%s", base);
    PlayVoice(buf, 127, 64);
}

// "DebugVoiceWait: 1" repro hook (PERSO.CPP): play a clip, then wait for it
// the way GameAskChoice does — spinning on the input poll only, no ManageTime.
// Returns the ms until IsSamplePlaying(SPEAK_SAMPLE) dropped, -1 on timeout.
S32 N64AudioTest_VoiceWait(const char *base, void (*poll)(void), U32 timeoutMs) {
    N64AudioTest_Voice(base);
    U32 t0 = (U32)get_ticks_ms();
    while (IsSamplePlaying(SPEAK_SAMPLE)) {
        if ((U32)get_ticks_ms() - t0 > timeoutMs) {
            StopVoice();
            return -1;
        }
        poll();
    }
    return (S32)((U32)get_ticks_ms() - t0);
}

void N64AudioTest_Music(const char *name) {
    PlayStream((char *)name);
}

void N64AudioTest_Stop(void) {
    StopVoice();
    StopStream();
}

// Snapshot for the audio diagnostics HUD (SYSTEM/N64_AUDIODIAG.CPP).
void N64AudioGetStatus(N64AudioStatus *st) {
    memset(st, 0, sizeof(*st));
    if (!s_audioInit) return;
    st->musicOpen = s_musicOpen;
    st->musicPlaying = s_musicOpen && mixer_ch_playing(CH_MUSIC);
    st->musicPaused = s_musicPaused;
    st->musicVolume = streamVolume;
    st->musicPos = st->musicPlaying ? mixer_ch_get_pos(CH_MUSIC) : 0.0f;
    st->voiceOpen = s_voiceOpen;
    st->voicePlaying = s_voiceOpen && mixer_ch_playing(CH_VOICE);
    st->voiceVolume = s_voiceVol;
    st->samplesPaused = s_samplesPaused;
    st->voicePos = st->voicePlaying ? mixer_ch_get_pos(CH_VOICE) : 0.0f;
}

void AudioStreamPump(void) {
    if (!s_audioInit) return;
    N64AudioDiag_PumpTick();

    while (audio_can_write()) {
        short *out = audio_write_begin();
        mixer_poll(out, audio_get_buffer_length());
        audio_write_end();
    }

    for (int i = 0; i < NUM_VOICES; i++) {
        N64Voice *v = &s_voices[i];
        if (!v->active) continue;
        if (!mixer_ch_playing(i)) {
            VoiceFree(i); // one-shot finished
            continue;
        }
        if (v->repeatsLeft > 1) {
            // Finite repeats ride the mixer loop; count wraparounds.
            float pos = mixer_ch_get_pos(i);
            if (pos < v->lastPos) {
                if (--v->repeatsLeft <= 1)
                    v->curWave->loop_len = 0; // last iteration: let it end
            }
            v->lastPos = pos;
        }
    }
}

#ifdef __cplusplus
}
#endif
