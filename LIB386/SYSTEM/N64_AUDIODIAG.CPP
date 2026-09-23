// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Audio streaming diagnostics (see SYSTEM/N64_AUDIODIAG.H).
//
// Why this exists: on real hardware (not in Ares) the streamed audio goes
// silent - each voice clip plays for a second or two, music only in the first
// scene - while sample-based SFX keep working. The wav64 decoders assert on
// corrupt data, so silence without a crash points at reads that come back
// empty: an Opus frame size of 0 decodes as packet-loss silence and never
// advances the file, and VADPCM zeros are silence too. The suspects are the
// PI read paths the streams use and nothing else does: dma_read into RAM that
// is not 8-byte aligned (libdragon moves the head bytes with 16-bit CPU reads
// of the cartridge bus) and small reads such as Opus's 2-byte frame size.
//
// The reference for every comparison is a plain PI DMA with an 8-byte aligned
// RAM buffer and an 8-byte aligned ROM address: the path the HQR loads take,
// which demonstrably works on the same hardware.

#include <SYSTEM/N64_AUDIODIAG.H>

#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef N64LBA_BUILD_TAG
#define N64LBA_BUILD_TAG ""
#endif

#define DIAG_PI_STATUS (*(volatile uint32_t *)0xA4600010)
#define HUD_FONT 1

void AudioStreamPump(void); // sound backend

static int s_enabled = -1;

int N64AudioDiag_Enabled(void) {
    if (s_enabled < 0) s_enabled = (dfs_rom_addr("audiodiag") != 0);
    return s_enabled;
}

// --- Reference reads ------------------------------------------------------------

static uint8_t s_bounce[2048 + 16] __attribute__((aligned(16)));

static uint32_t PiNorm(uint32_t pi) { return (pi | 0x10000000u) & 0x1FFFFFFFu; }

static void RefRead(uint32_t pi, void *dst, int len) {
    uint8_t *out = (uint8_t *)dst;
    pi = PiNorm(pi);
    while (len > 0) {
        uint32_t a = pi & ~7u;
        int skip = (int)(pi - a);
        int n = (len < 2048 - skip) ? len : 2048 - skip;
        uint32_t dl = ((uint32_t)(skip + n) + 7u) & ~7u;
        data_cache_hit_writeback_invalidate(s_bounce, sizeof(s_bounce));
        dma_read_raw_async(s_bounce, a, dl);
        dma_wait();
        data_cache_hit_invalidate(s_bounce, sizeof(s_bounce));
        memcpy(out, s_bounce + skip, (size_t)n);
        out += n;
        pi += (uint32_t)n;
        len -= n;
    }
}

static uint32_t s_rng = 0x12345678u;
static uint32_t Rnd(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng >> 8;
}

// --- Boot self-test ---------------------------------------------------------------

typedef struct {
    char path[72]; // DFS path without "rom:/"
    uint32_t pi;
    int size;
} DiagFile;

#define MAX_VOX_FILES 48
#define MAX_MUS_FILES 32
static DiagFile s_files[MAX_VOX_FILES + MAX_MUS_FILES];
static int s_numVox = 0, s_numMus = 0;

typedef struct {
    int t1n, t1bad;
    int t2n, t2bad;
    int t3n, t3bad;
    int t4files, t4frames, t4badSize, t4zero, t4badData;
    int t5chunks, t5bad;
    char err[4][48];
    int nerr;
} DiagResults;

static DiagResults R;

static void Err(const char *fmt, ...) {
    char line[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    debugf("[audiodiag] %s\n", line);
    if (R.nerr < 4) {
        snprintf(R.err[R.nerr], sizeof(R.err[0]), "%.47s", line);
        R.nerr++;
    }
}

static int CountWav64(const char *dir) {
    char p[32];
    dir_t d;
    int n = 0;
    snprintf(p, sizeof(p), "rom:/%s/", dir);
    for (int err = dir_findfirst(p, &d); err == 0; err = dir_findnext(p, &d))
        if (d.d_type == DT_REG && strstr(d.d_name, ".wav64")) n++;
    return n;
}

static void ListWav64(const char *dir, DiagFile *out, int max, int *count) {
    char p[32];
    dir_t d;
    int total = CountWav64(dir);
    int stride = (total + max - 1) / max;
    if (stride < 1) stride = 1;
    int idx = 0;
    snprintf(p, sizeof(p), "rom:/%s/", dir);
    for (int err = dir_findfirst(p, &d); err == 0; err = dir_findnext(p, &d)) {
        if (d.d_type != DT_REG || !strstr(d.d_name, ".wav64")) continue;
        if ((idx++ % stride) != 0 || *count >= max) continue;
        const char *name = strrchr(d.d_name, '/');
        name = name ? name + 1 : d.d_name;
        DiagFile *f = &out[*count];
        snprintf(f->path, sizeof(f->path), "%s/%.60s", dir, name);
        f->pi = dfs_rom_addr(f->path);
        f->size = (int)d.d_size;
        if (f->size <= 0) {
            char full[96];
            snprintf(full, sizeof(full), "rom:/%s", f->path);
            int fd = open(full, O_RDONLY);
            if (fd >= 0) {
                f->size = (int)lseek(fd, 0, SEEK_END);
                close(fd);
            }
        }
        if (f->pi && f->size > 128) (*count)++;
    }
}

// T1: dma_read into RAM at every alignment (the path dfs_read takes when RAM
// and ROM share the 2-byte phase, including CPU reads for the head bytes).
static void TestDmaMisaligned(void) {
    static uint8_t buf[160] __attribute__((aligned(16)));
    uint8_t ref[64];
    int nf = s_numVox + s_numMus;
    for (int i = 0; i < 3000; i++) {
        DiagFile *f = &s_files[Rnd() % nf];
        uint32_t pi = PiNorm(f->pi) + Rnd() % (uint32_t)(f->size - 64);
        int len = 1 + (int)(Rnd() % 48);
        int ro = (int)(Rnd() % 16);
        if ((ro ^ (int)pi) & 1) ro ^= 1;
        memset(buf, 0xA5, sizeof(buf));
        data_cache_hit_writeback_invalidate(buf, sizeof(buf));
        dma_read(buf + ro, pi, (unsigned long)len);
        data_cache_hit_invalidate(buf, sizeof(buf));
        RefRead(pi, ref, len);
        R.t1n++;
        if (memcmp(buf + ro, ref, (size_t)len) != 0) {
            R.t1bad++;
            int k = 0;
            while (k < len && buf[ro + k] == ref[k]) k++;
            Err("T1 %08lx+%d l%d r%d: %02x!=%02x", (unsigned long)pi, k, len, ro,
                buf[ro + k], ref[k]);
        }
    }
}

// T2/T3: single CPU reads of the cartridge bus, 16-bit (at 4-byte aligned
// addresses, the only ones libdragon issues) and 32-bit.
static void TestCpuReads(void) {
    uint8_t ref[4];
    int nf = s_numVox + s_numMus;
    for (int i = 0; i < 1500; i++) {
        DiagFile *f = &s_files[Rnd() % nf];
        uint32_t pi = (PiNorm(f->pi) + Rnd() % (uint32_t)(f->size - 8)) & ~3u;
        RefRead(pi, ref, 4);

        disable_interrupts();
        while (DIAG_PI_STATUS & 3) {}
        uint16_t v16 = *(volatile uint16_t *)(uintptr_t)(pi | 0xA0000000u);
        enable_interrupts();
        uint32_t v32 = io_read(pi);

        uint16_t e16 = (uint16_t)((ref[0] << 8) | ref[1]);
        uint32_t e32 = ((uint32_t)ref[0] << 24) | ((uint32_t)ref[1] << 16) |
                       ((uint32_t)ref[2] << 8) | ref[3];
        R.t2n++;
        R.t3n++;
        if (v16 != e16) {
            R.t2bad++;
            Err("T2 %08lx: %04x!=%04x", (unsigned long)pi, v16, e16);
        }
        if (v32 != e32) {
            R.t3bad++;
            Err("T3 %08lx: %08lx!=%08lx", (unsigned long)pi, (unsigned long)v32,
                (unsigned long)e32);
        }
    }
}

typedef struct __attribute__((packed)) {
    char id[4];
    int8_t version, format, channels, nbits;
    int32_t freq, len, loop_len, start_offset;
} DiagWavHeader;

static int OpenWav(const DiagFile *f, DiagWavHeader *h) {
    char full[96];
    snprintf(full, sizeof(full), "rom:/%s", f->path);
    int fd = open(full, O_RDONLY);
    if (fd < 0) return -1;
    if (read(fd, h, sizeof(*h)) != (int)sizeof(*h) || memcmp(h->id, "WV64", 4) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// T4: walk Opus frames exactly as wav64_opus reads them: a 2-byte frame size
// into a small stack-like slot, then the (even-padded) frame into a 16-byte
// aligned buffer.
static void TestOpusWalk(void) {
    static uint8_t nbslot[16] __attribute__((aligned(16)));
    static uint8_t fbuf[2048] __attribute__((aligned(16)));
    static uint8_t ref[2048];
    for (int i = 0; i < s_numVox; i++) {
        DiagFile *f = &s_files[i];
        DiagWavHeader h;
        int fd = OpenWav(f, &h);
        if (fd < 0 || h.format != 3) {
            if (fd >= 0) close(fd);
            continue;
        }
        uint32_t xh[2];
        read(fd, xh, sizeof(xh));
        uint32_t frameSize = xh[0], maxCmp = xh[1];
        if (frameSize == 0 || maxCmp + 2 > sizeof(fbuf)) {
            close(fd);
            continue;
        }
        R.t4files++;
        uint32_t pos = (uint32_t)h.start_offset;
        lseek(fd, (off_t)pos, SEEK_SET);
        uint32_t pi = PiNorm(f->pi);
        int nframes = (int)((h.len + (int32_t)frameSize - 1) / (int32_t)frameSize);
        if (nframes > 400) nframes = 400;
        for (int fr = 0; fr < nframes && pos + 2 <= (uint32_t)f->size; fr++) {
            uint8_t *slot = nbslot + (fr & 3) * 2;
            data_cache_hit_writeback_invalidate(nbslot, sizeof(nbslot));
            read(fd, slot, 2);
            uint16_t nb = (uint16_t)((slot[0] << 8) | slot[1]);
            uint8_t rb[2];
            RefRead(pi + pos, rb, 2);
            uint16_t enb = (uint16_t)((rb[0] << 8) | rb[1]);
            R.t4frames++;
            if (enb == 0) R.t4zero++;
            if (nb != enb) {
                R.t4badSize++;
                Err("T4 %s f%d nb %u!=%u", f->path + 4, fr, nb, enb);
                lseek(fd, (off_t)(pos + 2), SEEK_SET); // resync on the reference
            }
            uint32_t al = (enb + 1u) & ~1u;
            if (al > maxCmp + 1) break;
            if (al) {
                data_cache_hit_writeback_invalidate(fbuf, al);
                read(fd, fbuf, al);
                RefRead(pi + pos + 2, ref, (int)al);
                if (memcmp(fbuf, ref, al) != 0) {
                    R.t4badData++;
                    Err("T4 %s f%d frame data", f->path + 4, fr);
                }
            }
            pos += 2 + al;
        }
        close(fd);
    }
}

// T5: VADPCM chunk reads as wav64_vadpcm does them: 9*n bytes at the tail of
// an uncached sample-buffer window, through the cached alias.
static void TestVadpcmChunks(void) {
    uint8_t *win = (uint8_t *)malloc_uncached(4096);
    static uint8_t ref[1024];
    if (!win) return;
    for (int i = s_numVox; i < s_numVox + s_numMus; i++) {
        DiagFile *f = &s_files[i];
        DiagWavHeader h;
        int fd = OpenWav(f, &h);
        if (fd < 0 || h.format != 1) {
            if (fd >= 0) close(fd);
            continue;
        }
        uint32_t pos = (uint32_t)h.start_offset;
        lseek(fd, (off_t)pos, SEEK_SET);
        uint32_t pi = PiNorm(f->pi);
        for (int c = 0; c < 120; c++) {
            // Even frame counts only: wav64_vadpcm rounds every read to 32
            // samples so RAM and ROM keep the same 2-byte phase (fast path).
            int n = 2 * (1 + (int)(Rnd() % 47));
            int bytes = 9 * n;
            if (pos + (uint32_t)bytes > (uint32_t)f->size) break;
            uint8_t *dest = win + (Rnd() % 8) * 2;
            uint8_t *src = dest + 32 * n - bytes;
            memset(src, 0xA5, (size_t)bytes);
            read(fd, CachedAddr(src), bytes);
            RefRead(pi + pos, ref, bytes);
            R.t5chunks++;
            if (memcmp(src, ref, (size_t)bytes) != 0) {
                R.t5bad++;
                int k = 0;
                while (k < bytes && src[k] == ref[k]) k++;
                Err("T5 %s @%lu+%d/%d src&7=%d", f->path + 6, (unsigned long)pos, k, bytes,
                    (int)((uintptr_t)src & 7));
            }
            pos += (uint32_t)bytes;
        }
        close(fd);
    }
    free_uncached(win);
}

static int DrawResults(surface_t *d) {
    char line[64];
    int y = 18;
    const int x = 18;
    graphics_fill_screen(d, graphics_make_color(0, 0, 0, 255));
    graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
#define LINE(...)                                  \
    do {                                           \
        snprintf(line, sizeof(line), __VA_ARGS__); \
        graphics_draw_text(d, x, y, line);         \
        y += 10;                                   \
    } while (0)
    LINE("LBA2 AUDIO DIAGNOSTIC %s", N64LBA_BUILD_TAG);
    y += 4;
    LINE("T1 DMA misaligned %4d/%d bad", R.t1bad, R.t1n);
    LINE("T2 CPU 16-bit     %4d/%d bad", R.t2bad, R.t2n);
    LINE("T3 CPU 32-bit     %4d/%d bad", R.t3bad, R.t3n);
    LINE("T4 Opus %d files %d frames", R.t4files, R.t4frames);
    LINE("   size bad %d zero %d data bad %d", R.t4badSize, R.t4zero, R.t4badData);
    LINE("T5 VADPCM %d chunks %d bad", R.t5chunks, R.t5bad);
    y += 4;
    int bad = R.t1bad + R.t2bad + R.t3bad + R.t4badSize + R.t4badData + R.t5bad;
    if (bad) {
        graphics_set_color(graphics_make_color(255, 96, 96, 255), 0);
        LINE("RESULT: READ ERRORS FOUND");
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
        for (int i = 0; i < R.nerr; i++) LINE("%.36s", R.err[i]);
    } else {
        graphics_set_color(graphics_make_color(96, 255, 96, 255), 0);
        LINE("RESULT: ALL READS OK");
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
    }
#undef LINE
    return y;
}

static void BootReportLoop(void);

void N64AudioDiag_BootTest(void) {
    if (!N64AudioDiag_Enabled()) return;
    memset(&R, 0, sizeof(R));
    s_numVox = s_numMus = 0;
    ListWav64("vox", s_files, MAX_VOX_FILES, &s_numVox);
    ListWav64("music", s_files + s_numVox, MAX_MUS_FILES, &s_numMus);
    debugf("[audiodiag] boot test: %d vox, %d music files\n", s_numVox, s_numMus);

    uint32_t t0 = (uint32_t)get_ticks_ms();
    if (s_numVox + s_numMus > 0) {
        TestDmaMisaligned();
        TestCpuReads();
        TestOpusWalk();
        TestVadpcmChunks();
    }
    debugf("[audiodiag] T1 %d/%d T2 %d/%d T3 %d/%d T4 files=%d frames=%d size=%d zero=%d "
           "data=%d T5 %d/%d (%lu ms)\n",
           R.t1bad, R.t1n, R.t2bad, R.t2n, R.t3bad, R.t3n, R.t4files, R.t4frames,
           R.t4badSize, R.t4zero, R.t4badData, R.t5bad, R.t5chunks,
           (unsigned long)((uint32_t)get_ticks_ms() - t0));

    BootReportLoop();
}

// --- In-game stream checks ------------------------------------------------------

typedef struct {
    wav64_t *wav;
    WaveformRead orig;
    uint32_t pi;
    int format;
    uint32_t frameSize;
    // current stream
    int calls, bad, silent;
    // since boot
    int totCalls, totBad, totSilent;
} DiagStream;

static DiagStream s_ds[2]; // 0 = music, 1 = voice

static DiagStream *FindStream(void *ctx) {
    for (int i = 0; i < 2; i++)
        if (s_ds[i].wav && (void *)s_ds[i].wav == ctx) return &s_ds[i];
    return NULL;
}

static void DiagRead(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
    DiagStream *d = FindStream(ctx);
    if (!d) return; // cannot happen: the wrapper is only installed with an entry
    wav64_t *w = d->wav;
    int verify = !seeking || wpos == 0;
    uint32_t pos0 = (seeking && wpos == 0) ? (uint32_t)w->base_offset
                                           : (uint32_t)lseek(w->current_fd, 0, SEEK_CUR);

    d->orig(ctx, sbuf, wpos, wlen, seeking);

    uint32_t pos1 = (uint32_t)lseek(w->current_fd, 0, SEEK_CUR);
    uint32_t got = pos1 - pos0;
    d->calls++;
    d->totCalls++;
    if (!verify || wlen <= 0) return;

    uint32_t expect = 0;
    if (d->format == 3 && d->frameSize) {
        // Replay the frame walk on the reference path.
        int nframes = (int)((wlen + (int)d->frameSize - 1) / (int)d->frameSize);
        uint32_t p = pos0;
        for (int i = 0; i < nframes; i++) {
            uint8_t rb[2];
            RefRead(d->pi + p, rb, 2);
            uint32_t nb = (uint32_t)((rb[0] << 8) | rb[1]);
            p += 2 + ((nb + 1u) & ~1u);
        }
        expect = p - pos0;
        if (got == 2u * (uint32_t)nframes && expect != got) {
            d->silent++;
            d->totSilent++;
        }
    } else if (d->format == 1) {
        int n = ((wlen + 31) & ~31) / 16;
        expect = 9u * (uint32_t)n * (uint32_t)w->wave.channels;
    } else {
        return;
    }
    if (expect != got) {
        d->bad++;
        d->totBad++;
        if (d->totBad <= 20)
            debugf("[audiodiag] %s read mismatch @%lu wlen=%d consumed=%lu expected=%lu\n",
                   d == &s_ds[1] ? "voice" : "music", (unsigned long)pos0, wlen,
                   (unsigned long)got, (unsigned long)expect);
    }
}

void N64AudioDiag_WrapStream(wav64_t *wav, int isVoice) {
    if (!N64AudioDiag_Enabled() || !wav || !wav->wave.read) return;
    DiagStream *d = &s_ds[isVoice ? 1 : 0];
    const char *name = wav->wave.name ? wav->wave.name : "";
    if (strncmp(name, "rom:/", 5) == 0) name += 5;
    d->wav = wav;
    d->orig = wav->wave.read;
    d->pi = PiNorm(dfs_rom_addr(name));
    d->format = wav->format;
    d->frameSize = (wav->format == 3 && wav->ext) ? ((uint32_t *)wav->ext)[0] : 0;
    d->calls = d->bad = d->silent = 0;
    if (d->pi == 0x10000000u) return; // not a DFS file: leave it alone
    wav->wave.read = DiagRead; // ctx stays the wav64_t the codec expects
}

void N64AudioDiag_StreamClosed(wav64_t *wav) {
    for (int i = 0; i < 2; i++)
        if (s_ds[i].wav == wav) s_ds[i].wav = NULL;
}

// --- Boot report with live streams ------------------------------------------------

// While the report is on screen, play the longest voice clip in the sample (in
// a loop) and a music track through the game's own paths, so the stream
// checks above run - and can be heard - before any gameplay.
static void BootReportLoop(void) {
    char voiceBase[64] = "";
    int best = -1;
    for (int i = 0; i < s_numVox; i++)
        if (best < 0 || s_files[i].size > s_files[best].size) best = i;
    if (best >= 0) {
        const char *n = strrchr(s_files[best].path, '/');
        snprintf(voiceBase, sizeof(voiceBase), "%s", n ? n + 1 : s_files[best].path);
        char *dot = strstr(voiceBase, ".wav64");
        if (dot) *dot = '\0';
    }
    int voicePlays = 0;
    N64AudioTest_Music("TRACK6.WAV");

    // Hold the report until START (a fresh press, not one held from boot), or
    // 90 s so a pad problem cannot leave the game stuck here.
    int armed = 0;
    uint32_t shown = (uint32_t)get_ticks_ms();
    for (;;) {
        N64AudioStatus st;
        N64AudioGetStatus(&st);
        if (voiceBase[0] && !st.voicePlaying) {
            N64AudioTest_Voice(voiceBase);
            voicePlays++;
        }
        AudioStreamPump();

        surface_t *d = display_get();
        int y = DrawResults(d) + 6;
        char line[64];
        graphics_set_color(graphics_make_color(255, 255, 160, 255), 0);
        snprintf(line, sizeof(line), "LIVE voice x%d rd%d sil%d bad%d", voicePlays,
                 s_ds[1].totCalls, s_ds[1].totSilent, s_ds[1].totBad);
        graphics_draw_text(d, 18, y, line);
        y += 10;
        snprintf(line, sizeof(line), "LIVE music p%d rd%d bad%d pos%.0f", st.musicPlaying,
                 s_ds[0].totCalls, s_ds[0].totBad, st.musicPos);
        graphics_draw_text(d, 18, y, line);
        y += 14;
        graphics_set_color(graphics_make_color(255, 255, 255, 255), 0);
        graphics_draw_text(d, 18, y, "Voice + music should play nonstop");
        y += 10;
        graphics_draw_text(d, 18, y, "Take a photo, then press START");
        display_show(d);

        joypad_poll();
        joypad_buttons_t held = joypad_get_buttons_held(JOYPAD_PORT_1);
        if (!held.start) armed = 1;
        else if (armed) break;
        if ((uint32_t)get_ticks_ms() - shown > 90000) break;
    }
    debugf("[audiodiag] live: voice %s x%d rd=%d sil=%d bad=%d | music rd=%d bad=%d\n",
           voiceBase, voicePlays, s_ds[1].totCalls, s_ds[1].totSilent, s_ds[1].totBad,
           s_ds[0].totCalls, s_ds[0].totBad);
    N64AudioTest_Stop();
    for (int i = 0; i < 3; i++) { // blank the report before the engine starts
        surface_t *d = display_get();
        graphics_fill_screen(d, 0);
        display_show(d);
    }
}

static uint32_t s_lastPump = 0, s_maxGap = 0, s_shownGap = 0, s_windowStart = 0;

void N64AudioDiag_PumpTick(void) {
    if (!N64AudioDiag_Enabled()) return;
    uint32_t now = (uint32_t)get_ticks_ms();
    if (s_lastPump && now - s_lastPump > s_maxGap) s_maxGap = now - s_lastPump;
    s_lastPump = now;
    if (now - s_windowStart >= 2000) {
        s_shownGap = s_maxGap;
        s_maxGap = 0;
        s_windowStart = now;
        N64AudioStatus st;
        N64AudioGetStatus(&st);
        debugf("[audiodiag] mus o%d p%d v%d pz%d pos%.0f rd%d bad%d | vox o%d p%d pos%.0f "
               "rd%d sil%d bad%d | tot vox sil%d bad%d mus bad%d | gap %lums\n",
               st.musicOpen, st.musicPlaying, st.musicVolume, st.musicPaused, st.musicPos,
               s_ds[0].calls, s_ds[0].bad, st.voiceOpen, st.voicePlaying, st.voicePos,
               s_ds[1].calls, s_ds[1].silent, s_ds[1].bad, s_ds[1].totSilent,
               s_ds[1].totBad, s_ds[0].totBad, (unsigned long)s_shownGap);
    }
}

void N64AudioDiag_Hud(void) {
    static int fontReady = 0;
    if (!N64AudioDiag_Enabled()) return;
    if (!fontReady) {
        rdpq_text_register_font(HUD_FONT, rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_MONO));
        fontReady = 1;
    }
    N64AudioStatus st;
    N64AudioGetStatus(&st);

    rdpq_set_mode_fill(RGBA32(0, 0, 0, 255));
    rdpq_fill_rectangle(12, 10, 308, 40);
    rdpq_set_mode_standard();
    rdpq_text_printf(NULL, HUD_FONT, 16, 18, "MUS o%d p%d v%d pz%d %6.0f rd%d bad%d",
                     st.musicOpen, st.musicPlaying, st.musicVolume, st.musicPaused,
                     st.musicPos, s_ds[0].calls, s_ds[0].bad);
    rdpq_text_printf(NULL, HUD_FONT, 16, 27, "VOX o%d p%d v%d %6.0f rd%d sil%d bad%d",
                     st.voiceOpen, st.voicePlaying, st.voiceVolume, st.voicePos,
                     s_ds[1].calls, s_ds[1].silent, s_ds[1].bad);
    rdpq_text_printf(NULL, HUD_FONT, 16, 36, "ALL vox sil%d bad%d mus bad%d gap%lu",
                     s_ds[1].totSilent, s_ds[1].totBad, s_ds[0].totBad,
                     (unsigned long)s_shownGap);
}

#ifdef __cplusplus
}
#endif
