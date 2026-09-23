// SPDX-License-Identifier: GPL-2.0-only
// Copyright (C) 2026 Samuele Voltan
//
// Nintendo 64 (libdragon) system backend. Provides the same symbol contract
// as WIIU_BACKEND.CPP: replaces AVAILMEM, DISPOS, EVENTS, KEYBOARD, MOUSE,
// STRING, TIMER, WINDOW and LOGPRINT with libdragon-based equivalents, plus
// the platform main() that hands off to lba2_engine_main().
//
// Video presentation (the N64 counterpart of WiiU_BlitGameFramebuffer) also
// lives here: the engine's 8bpp Phys buffer is pushed to the 16bpp display
// through an RDP CI8+TLUT textured blit — the palette lookup that costs a
// full CPU pass on Wii U is free on N64 hardware.

#include <SYSTEM/ADELINE_TYPES.H>
#include <SYSTEM/EVENTS.H>
#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOGPRINT.H>
#include <SYSTEM/MOUSE.H>
#include <SYSTEM/N64_AUDIODIAG.H>
#include <SYSTEM/N64_SRAMFS.H>
#include <SYSTEM/STRING.H>
#include <SYSTEM/TIMER.H>
#include <SYSTEM/WINDOW.H>

#include <libdragon.h>
#include <malloc.h>

#include <dirent.h> // resolves to LIB386/H/dirent.h (N64 shim, implemented below)

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CRT overscan safe area, percent per side (see N64SetScreenMargin). 6% keeps
// the dialogue text inside the visible raster on the tester's consumer CRT
// (which ate ~4.5%) with headroom for worse sets; 0 = full raster.
#define N64_DEFAULT_SCREEN_MARGIN 6

#ifdef __cplusplus
extern "C" {
#endif

#define N64LBA_BUILD_TAG "N64-V1-SKELETON"
#define N64_LOG_LINE_LEN 256

// =============================================================================
// LOGPRINT.CPP replacement — everything goes to the libdragon debug channels
// (ISViewer window in Ares, USB log on real HW with a UNFLoader-style cart).
// =============================================================================
U32 Log2File = 0; // No SD on a cartridge; debug channel only.
U32 QuietLog = 0;

// Ring buffer of the last log lines, dumped on-screen when the engine exits
// (exit() or return) — visual diagnostics without an ISViewer-capable setup.
#define N64_LOG_RING_LINES 26
#define N64_LOG_RING_WIDTH 64
static char s_logRing[N64_LOG_RING_LINES][N64_LOG_RING_WIDTH];
static int s_logRingPos = 0;
static int s_logRingCount = 0;

static void N64LogRingStore(const char *line) {
    strncpy(s_logRing[s_logRingPos], line, N64_LOG_RING_WIDTH - 1);
    s_logRing[s_logRingPos][N64_LOG_RING_WIDTH - 1] = '\0';
    s_logRingPos = (s_logRingPos + 1) % N64_LOG_RING_LINES;
    if (s_logRingCount < N64_LOG_RING_LINES) s_logRingCount++;
}

void CreateLog(const char *filePath) {
    (void)filePath;
}

void LogPrintf(const char *format, ...) {
    if (QuietLog) return;
    char buf[N64_LOG_LINE_LEN];
    va_list ap;
    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    size_t n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) {
        buf[--n] = '\0';
    }
    if (n > 0) {
        debugf("[LBA2] %s\n", buf);
        N64LogRingStore(buf);
    }
}

void LogPuts(const char *string) {
    if (QuietLog || !string) return;
    LogPrintf("%s", string);
}

// =============================================================================
// Video presentation — N64 counterpart of WiiU_BlitGameFramebuffer.
// Called by SVGA/N64.CPP when the engine releases the frame. `phys` is the
// engine's 8bpp CI framebuffer, `tlut` the 256-entry RGBA5551 palette.
// =============================================================================
// ── [renderprof] frame profiler ─────────────────────────────────────────────
// One presented frame == one N64_BlitGameFramebuffer call, so this is the frame
// anchor. Splits the frame into: terrain software-raster (hooked from the engine
// via N64ProfAddTerrainTicks), cache writeback of the 307 KB 8bpp buffer, RDP
// present (+vsync wait), and "other" (game logic + non-terrain raster + the
// dirty-box Log->Phys copy). Accumulated in raw COP0 ticks, converted to us and
// dumped averaged once per 60 frames. Console build only (LBA2_TARGET_WIIU is
// also defined on N64).
#ifdef LBA2_TARGET_WIIU
#ifndef TICKS_PER_SECOND
#define TICKS_PER_SECOND (93750000 / 2)
#endif
static U32 s_rpWb, s_rpPresent, s_rpTerrain, s_rpFrame, s_rpFrames, s_rpLastEnd;
static U32 s_rpAff, s_rpBox;

extern "C" U32 N64ProfTicks(void) { return (U32)get_ticks(); }
extern "C" U32 N64ProfNowMs(void) { return (U32)get_ticks_ms(); }
U32 g_loadCubeMs = 0, g_loadCubeCount = 0; // [hangprof] LoadCube per frame (LOADISLE.CPP)
extern U32 s_unalignedCount;
// [hangprof] heap snapshot: RDRAM total, newlib arena in use / free
extern "C" void N64ProfHeapLog(const char *tag) {
    struct mallinfo mi = mallinfo();
    LogPrintf("[heap] %s: rdram=%luKB arena=%luKB used=%luKB free=%luKB\n", tag,
              (unsigned long)(get_memory_size() / 1024), (unsigned long)(mi.arena / 1024),
              (unsigned long)(mi.uordblks / 1024), (unsigned long)(mi.fordblks / 1024));
}
extern "C" void N64ProfAddTerrainTicks(U32 t) { s_rpTerrain += t; }
extern "C" void N64ProfAddAffTicks(U32 t) { s_rpAff += t; }   // AffScene (obj raster, incl. terrain)
extern "C" void N64ProfAddBoxTicks(U32 t) { s_rpBox += t; }   // dirty-box Log->Phys CPU copy
// AffScene sub-phases (full-redraw path): which of the full-frame operations dominates.
static U32 s_rpCls, s_rpRefresh, s_rpCopy;
extern "C" void N64ProfAddClsTicks(U32 t) { s_rpCls += t; }         // Cls() clear 640x480
extern "C" void N64ProfAddRefreshTicks(U32 t) { s_rpRefresh += t; } // RefreshGrille() background
extern "C" void N64ProfAddCopyTicks(U32 t) { s_rpCopy += t; }       // CopyScreen(Log,Screen)
static U32 s_rpSort;
extern "C" void N64ProfAddSortTicks(U32 t) { s_rpSort += t; }       // BaseSort(): object render/fill
#endif

void N64_BlitGameFramebuffer(const void *phys, U32 resX, U32 resY,
                             const U16 *tlut) {
    if (!phys || resX == 0 || resY == 0) return;

#ifdef LBA2_TARGET_WIIU
    U32 rpT0 = (U32)get_ticks();
#endif
    // RDP reads RDRAM directly; the CPU rendered Phys through the cache.
    data_cache_hit_writeback((void *)phys, (size_t)resX * resY);
#ifdef LBA2_TARGET_WIIU
    U32 rpT1 = (U32)get_ticks();
#endif

    surface_t src = surface_make((void *)phys, FMT_CI8, resX, resY, resX);
    surface_t *disp = display_get(); // blocks until a buffer is free (vsync pacing)

    rdpq_attach(disp, NULL);
    rdpq_set_mode_standard();
    rdpq_mode_tlut(TLUT_RGBA16);
    rdpq_tex_upload_tlut((uint16_t *)tlut, 0, 256);

    // Engine renders 640x480; the display is 320x240 (16bpp double buffer at
    // 640x480 would cost 1.2 MB of the 8 MB RDRAM). The RDP downscales
    // during the blit for free.
    rdpq_blitparms_t parms = {0};
    parms.scale_x = (float)disp->width / (float)resX;
    parms.scale_y = (float)disp->height / (float)resY;
    rdpq_tex_blit(&src, 0, 0, &parms);
    N64AudioDiag_Hud(); // no-op unless the ROM carries rom:/audiodiag
    rdpq_detach_show();

#ifdef LBA2_TARGET_WIIU
    U32 rpT2 = (U32)get_ticks();
    s_rpWb += (U32)(rpT1 - rpT0);
    s_rpPresent += (U32)(rpT2 - rpT1);
    if (s_rpLastEnd)
        s_rpFrame += (U32)(rpT2 - s_rpLastEnd);
    {
        // [hangprof] single slow frames, with the HQR ROM loads they carried
        extern U32 g_hqrLoadCount, g_hqrLoadMs, g_hqrLoadBytes, g_hqrEvictCount;
        extern char g_hqrLoadLast[];
        extern U32 g_loadCubeMs, g_loadCubeCount;
        U32 frameMs = s_rpLastEnd ? (U32)((U64)(rpT2 - s_rpLastEnd) * 1000ULL / TICKS_PER_SECOND) : 0;
        static U32 pAff = 0, pTerr = 0, pBox = 0, pRef = 0, pSort = 0, pCls = 0;
        static U32 pUnal = 0;
        extern U32 g_profDecorsTicks, g_profDecorSwapTicks, g_profDecorsDrawn;
#define RP_DMS(cur, prev) ((unsigned long)((U64)((cur) - (prev)) * 1000ULL / TICKS_PER_SECOND))
        if (frameMs >= 80 || g_hqrLoadMs >= 20) {
            LogPrintf("[hangprof] frame=%lums [aff=%lu terr=%lu (decors=%lu swap=%lu n=%lu) refresh=%lu fill=%lu cls=%lu box=%lu] unaligned=%lu loadcube=%lu(%lums) hqr loads=%lu (%lums, %luB, evict=%lu) last=%s\n",
                      (unsigned long)frameMs, RP_DMS(s_rpAff, pAff), RP_DMS(s_rpTerrain, pTerr),
                      RP_DMS(g_profDecorsTicks, 0), RP_DMS(g_profDecorSwapTicks, 0), (unsigned long)g_profDecorsDrawn,
                      RP_DMS(s_rpRefresh, pRef), RP_DMS(s_rpSort, pSort), RP_DMS(s_rpCls, pCls),
                      RP_DMS(s_rpBox, pBox), (unsigned long)(s_unalignedCount - pUnal),
                      (unsigned long)g_loadCubeCount, (unsigned long)g_loadCubeMs,
                      (unsigned long)g_hqrLoadCount,
                      (unsigned long)g_hqrLoadMs, (unsigned long)g_hqrLoadBytes,
                      (unsigned long)g_hqrEvictCount, g_hqrLoadLast);
        }
#undef RP_DMS
        g_profDecorsTicks = g_profDecorSwapTicks = g_profDecorsDrawn = 0;
        pUnal = s_unalignedCount;
        pAff = s_rpAff; pTerr = s_rpTerrain; pBox = s_rpBox; pRef = s_rpRefresh; pSort = s_rpSort; pCls = s_rpCls;
        if (s_rpFrames + 1 >= 60) // accumulators reset below
            pAff = pTerr = pBox = pRef = pSort = pCls = 0;
        g_hqrLoadCount = g_hqrLoadMs = g_hqrLoadBytes = g_hqrEvictCount = 0;
        g_hqrLoadLast[0] = '\0';
        g_loadCubeMs = g_loadCubeCount = 0;
    }
    s_rpLastEnd = rpT2;
    if (++s_rpFrames >= 60) {
        U32 f = s_rpFrames;
#define RP_US(accum) ((U32)((U64)((accum) / f) * 1000000ULL / TICKS_PER_SECOND))
        U32 frUs = RP_US(s_rpFrame);
        U32 affUs = RP_US(s_rpAff);
        U32 trUs = RP_US(s_rpTerrain);
        U32 boxUs = RP_US(s_rpBox);
        U32 prUs = RP_US(s_rpPresent);
        U32 wbUs = RP_US(s_rpWb);
        U32 clsUs = RP_US(s_rpCls);
        U32 refUs = RP_US(s_rpRefresh);
        U32 cpyUs = RP_US(s_rpCopy);
        U32 sortUs = RP_US(s_rpSort);
#undef RP_US
        // aff (incl. terrain), box, present, wb are all disjoint slices of the
        // frame; whatever is left is game logic + misc.
        U32 accounted = affUs + boxUs + prUs + wbUs;
        U32 logicUs = (frUs > accounted) ? (frUs - accounted) : 0;
        // Within aff: Cls + RefreshGrille (incl. terrain) + CopyScreen are the
        // full-redraw ops; whatever is left is the object/sprite raster loop.
        U32 affSub = clsUs + refUs + cpyUs;
        U32 objUs = (affUs > affSub) ? (affUs - affSub) : 0;
        U32 fpsx10 = frUs ? (U32)(10000000ULL / frUs) : 0;
        LogPrintf("[renderprof] fps=%u.%u frame=%uus | aff=%uus (terr=%uus) box=%uus present=%uus wb=%uus logic=%uus\n",
                  (unsigned)(fpsx10 / 10), (unsigned)(fpsx10 % 10),
                  (unsigned)frUs, (unsigned)affUs, (unsigned)trUs, (unsigned)boxUs,
                  (unsigned)prUs, (unsigned)wbUs, (unsigned)logicUs);
        // objects = sort/fill (BaseSort, render albero = fill-bound) + il resto
        // (pre-pass proiezione/shadow + incrustazioni = transform).
        U32 prepassUs = (objUs > sortUs) ? (objUs - sortUs) : 0;
        LogPrintf("[affprof]    cls=%uus refresh=%uus copy=%uus objects=%uus [fill=%uus prepass=%uus]\n",
                  (unsigned)clsUs, (unsigned)refUs, (unsigned)cpyUs, (unsigned)objUs,
                  (unsigned)sortUs, (unsigned)prepassUs);
        {
            // [terrainlod] neighbour cubes (DrawHorizon2ZBuf, inside refresh) vs
            // current cube (terr): time, polygons submitted/drawn, textures skipped.
            extern U32 g_profHorizTicks, g_profTerrPolys[2], g_profTerrDrawn[2], g_profTerrTexSkipped;
            extern U32 g_profTerrVtxTicks[2], g_profTerrPolyTicks[2];
#define TP_US(accum) ((unsigned)((U64)((accum) / f) * 1000000ULL / TICKS_PER_SECOND))
            if (g_profHorizTicks OR g_profTerrPolys[0])
                LogPrintf("[terrprof]   cur: vtx=%uus poly=%uus %lu/%lu polys | far: total=%uus vtx=%uus poly=%uus %lu/%lu polys texskip=%lu (per frame)\n",
                          TP_US(g_profTerrVtxTicks[0]), TP_US(g_profTerrPolyTicks[0]),
                          (unsigned long)(g_profTerrDrawn[0] / f), (unsigned long)(g_profTerrPolys[0] / f),
                          TP_US(g_profHorizTicks), TP_US(g_profTerrVtxTicks[1]), TP_US(g_profTerrPolyTicks[1]),
                          (unsigned long)(g_profTerrDrawn[1] / f), (unsigned long)(g_profTerrPolys[1] / f),
                          (unsigned long)(g_profTerrTexSkipped / f));
#undef TP_US
            g_profHorizTicks = g_profTerrPolys[0] = g_profTerrPolys[1] = 0;
            g_profTerrDrawn[0] = g_profTerrDrawn[1] = g_profTerrTexSkipped = 0;
            g_profTerrVtxTicks[0] = g_profTerrVtxTicks[1] = g_profTerrPolyTicks[0] = g_profTerrPolyTicks[1] = 0;
        }
        s_rpWb = s_rpPresent = s_rpTerrain = s_rpFrame = s_rpFrames = s_rpAff = s_rpBox = 0;
        s_rpCls = s_rpRefresh = s_rpCopy = s_rpSort = 0;
    }
#endif
}

// =============================================================================
// AVAILMEM.CPP replacement
// =============================================================================
U64 AvailableMem(void) {
    // Only consumed by AdjustHQRMem (pool sizing as fractions of this) and
    // debug overlays. Total RDRAM (8 MB) made the pools claim ~4.6 MB and
    // OOM the heap; returning a small budget clamps every pool to its MIN_*
    // (~1 MB total), which is the right diet for this console.
    return 1024ULL * 1024ULL;
}

// =============================================================================
// DISPOS.CPP replacement
// =============================================================================
#define OS_NT 3

U32 OSType = OS_NT;

void DisplayOS(void) {
    LogPrintf("Platform: Nintendo 64 (libdragon), RDRAM %lu MB\n",
              (unsigned long)(get_memory_size() / (1024UL * 1024UL)));
}

// =============================================================================
// TIMER.CPP replacement (logic mirrored from WIIU_BACKEND.CPP)
// =============================================================================
volatile U32 TimerSystemHR;
volatile U32 TimerRefHR;
volatile S32 CmptFrame;
volatile S32 NbFramePerSecond;
volatile S32 TimerLock;
volatile S32 CmptMemoTimerRef;
static U32 MemoTimerRefHR;
U32 LastTime;
U32 LastEvaluate;

static U32 N64Ms(void) {
    return (U32)get_ticks_ms();
}

// Audio pump hook: the real AIL/N64 backend overrides this weak no-op.
__attribute__((weak)) void AudioStreamPump(void) {}

void InitTimer() {
    LastTime = LastEvaluate = N64Ms();
    ManageTime();
}

void EndTimer() {}

void LockTimer() {
    ManageTime();
    ++TimerLock;
}

void UnlockTimer() {
    if (TimerLock > 0) {
        ManageTime();
        --TimerLock;
    }
}

void SaveTimer() {
    if (!CmptMemoTimerRef++) {
        ManageTime();
        MemoTimerRefHR = TimerRefHR;
    }
}

void RestoreTimer() {
    if (CmptMemoTimerRef) {
        if (!--CmptMemoTimerRef) {
            ManageTime();
            TimerRefHR = MemoTimerRefHR;
        }
    }
}

void SetTimerHR(U32 time) {
    LockTimer();
    TimerRefHR = time;
    UnlockTimer();
}

void ManageTime() {
    TimerSystemHR = N64Ms();
    if (!TimerLock) {
        TimerRefHR += TimerSystemHR - LastTime;
    }
    LastTime = TimerSystemHR;

    AudioStreamPump();

    S32 elapsed = TimerSystemHR - LastEvaluate;
    if (elapsed >= 1000) {
        NbFramePerSecond = (CmptFrame * 1000 + elapsed / 2) / elapsed;
        CmptFrame = 0;
        LastEvaluate += (elapsed / 1000) * 1000;
    }
}

void HandleEventsTimer(const void *event) {
    (void)event;
}

// =============================================================================
// WINDOW.CPP replacement — no window system; display is brought up in main().
// =============================================================================
bool AppActive = true;

static bool s_windowInit = false;
static U32 s_windowResX = 0;
static U32 s_windowResY = 0;
static bool s_windowFullscreen = true;

bool InitWindow(const char *title) {
    (void)title;
    s_windowInit = true;
    return true;
}

void EndWindow() {
    DestroyWindowSurface();
    s_windowInit = false;
}

bool IsWindowInitialized() {
    return s_windowInit;
}

bool CreateWindowSurface(U32 resX, U32 resY) {
    s_windowResX = resX;
    s_windowResY = resY;
    return true; // Engine framebuffer alloc is in SVGA/N64.CPP::CreateVideoSurface.
}

void DestroyWindowSurface() {
    s_windowResX = 0;
    s_windowResY = 0;
}

void WindowToSurfaceCoords(S32 windowX, S32 windowY, S32 *surfaceX, S32 *surfaceY) {
    if (surfaceX) *surfaceX = windowX;
    if (surfaceY) *surfaceY = windowY;
}

void SetWindowFullscreen(bool fullscreen) {
    s_windowFullscreen = fullscreen;
}

bool GetWindowFullscreen() {
    return s_windowFullscreen;
}

void ManageWindow() {
    // No process UI on N64; nothing to pump.
}

void HandleEventsWindow(const void *event) {
    (void)event;
}

// =============================================================================
// EVENTS.CPP replacement
// =============================================================================
static EventFilterFn s_eventFilter = NULL;

bool InitEvents() {
    return true;
}

void EndEvents() {
    s_eventFilter = NULL;
}

void ManageEvents() {
    // Input polling happens in JOYSTICK_N64.CPP::UpdateJoystick.
}

void SetEventFilter(EventFilterFn fn) {
    s_eventFilter = fn;
}

// =============================================================================
// KEYBOARD.CPP replacement — TabKeys[0..255] = keyboard range (unused on N64
// beyond injected virtual keys), TabKeys[256..] = bit-packed gamepad range
// written by JOYSTICK_N64.CPP.
// =============================================================================
S32 AsciiMode = 0;
S32 Key = 0;
U8 TabKeys[TABKEYS_NUM_KEYS];

void InitKeyboard() {
    memset(TabKeys, 0, sizeof(TabKeys));
    AsciiMode = 0;
    Key = 0;
}

void EndKeyboard() {}

S32 GetAscii() {
    return 0; // No keyboard on N64.
}

S32 CheckKey(U32 key) {
    if (key < 256) {
        return TabKeys[key] != 0;
    }
    return (TabKeys[256 - 32 + (key >> 3)] & (U8)(1 << (key & 7))) != 0;
}

void GetKeyScancodeName(char *string, U8 strMaxLen, U32 scanCode) {
    if (!string || strMaxLen == 0) return;
    snprintf(string, strMaxLen, "K%lu", (unsigned long)scanCode);
}

// Defined in SOURCES/JOYSTICK_N64.CPP: re-applies the held pad controls as PC
// keyboard scancodes after we clear the keyboard range, so gameplay actions and
// the direct spell polls (CheckKey(DefKeys[32..35]) in PERSO.CPP) see them.
extern "C" void N64Joy_ReinjectKeys(void);

void ManageKeyboard() {
    memset(TabKeys, 0, 256); // Gamepad bits (256..) are owned by UpdateJoystick.
    N64Joy_ReinjectKeys();   // ...but the injected keyboard keys must survive it.
}

void HandleEventsKeyboard(const void *event) {
    (void)event;
}

// =============================================================================
// MOUSE.CPP replacement — no pointing device on N64 (the N64 mouse exists but
// nobody will play LBA2 with it; all no-ops).
// =============================================================================
bool FlagMouse = false;
volatile S32 Click = 0;
volatile U32 MouseX = 0;
volatile U32 MouseY = 0;
S32 MouseXDep = 0;
S32 MouseYDep = 0;
volatile S32 MouseWheelYAccum = 0;
S32 MouseSpriteGraphicNum = 0;
const void *MouseSpriteGraphic = NULL;

bool MouseSpriteShouldDraw(void) { return false; }
bool MouseConsumeMotionFlag(void) { return false; }
void InitMouse() {}
void EndMouse() {}
void ShowMouse(bool show) { (void)show; }
void ShowMouseAfterFirstMotion(void) {}
void SetMousePos(U32 x, U32 y) { (void)x; (void)y; }
void SetMouseBox(U32 x0, U32 y0, U32 x1, U32 y1) { (void)x0; (void)y0; (void)x1; (void)y1; }
void ClearMouseBox() {}
void ManageMouse() {}
void HandleEventsMouse(const void *event) { (void)event; }
S32 ConsumeMouseWheelY(void) { return 0; }

// =============================================================================
// Profiling globals — PERSO.CPP's [prof] heartbeat (WIIU-gated code) reads and
// resets these. The N64 blit is RDP-side, so they stay zero for now.
// =============================================================================
int64_t WiiUProf_BlitTicks = 0;
int64_t WiiUProf_BlitConvTicks = 0;
int64_t WiiUProf_BlitFlushTicks = 0;
int64_t WiiUProf_BlitFlipTicks = 0;
U32 WiiUProf_BlitCount = 0;

// =============================================================================
// dirent shim (see LIB386/H/dirent.h) — POSIX opendir/readdir/closedir on top
// of libdragon's dir_findfirst/dir_findnext. The engine scans directories for
// *.LBA saves, *.VOX banks and screenshots through this.
// =============================================================================
struct lba2_n64_DIR {
    char path[ADELINE_MAX_PATH];
    dir_t cur;
    struct dirent de;
    int started; // 0 = next readdir does findfirst, 1 = findnext
};

DIR *opendir(const char *path) {
    if (!path || !*path) return NULL;
    DIR *d = (DIR *)calloc(1, sizeof(DIR));
    if (!d) return NULL;
    strncpy(d->path, path, sizeof(d->path) - 1);
    d->started = 0;
    return d;
}

struct dirent *readdir(DIR *dirp) {
    if (!dirp) return NULL;
    int r = dirp->started ? dir_findnext(dirp->path, &dirp->cur)
                          : dir_findfirst(dirp->path, &dirp->cur);
    dirp->started = 1;
    if (r != 0) return NULL;
    strncpy(dirp->de.d_name, dirp->cur.d_name, sizeof(dirp->de.d_name) - 1);
    dirp->de.d_name[sizeof(dirp->de.d_name) - 1] = '\0';
    dirp->de.d_type = dirp->cur.d_type;
    return &dirp->de;
}

int closedir(DIR *dirp) {
    free(dirp);
    return 0;
}

void rewinddir(DIR *dirp) {
    if (dirp) dirp->started = 0;
}

// =============================================================================
// libc gap filler — same as the Wii U build: FILES.CPP::Touch calls utime(),
// which libdragon's newlib does not provide.
// =============================================================================
struct utimbuf;
int utime(const char *path, const struct utimbuf *times) {
    (void)path;
    (void)times;
    return 0;
}

// =============================================================================
// STRING.CPP replacement (UTF-8 -> CP850, ported verbatim from WIIU_BACKEND)
// =============================================================================
char *ade_strcasestr(const char *haystack, const char *needle) {
    size_t length = strlen(needle);
    while (*haystack) {
        if (strncasecmp(haystack, needle, length) == 0) {
            return (char *)haystack;
        }
        ++haystack;
    }
    return NULL;
}

static const U8 kCp850FromLatin1[0x60] = {
    0xFF, 0xAD, 0xBD, 0x9C, 0xCF, 0xBE, 0xDD, 0xF5, // U+00A0..A7
    0xF9, 0xB8, 0xA6, 0xAE, 0xAA, 0xF0, 0xA9, 0xEE, // U+00A8..AF
    0xF8, 0xF1, 0xFD, 0xFC, 0xEF, 0xE6, 0xF4, 0xFA, // U+00B0..B7
    0xF7, 0xFB, 0xA7, 0xAF, 0xAC, 0xAB, 0xF3, 0xA8, // U+00B8..BF
    0xB7, 0xB5, 0xB6, 0xC7, 0x8E, 0x8F, 0x92, 0x80, // U+00C0..C7
    0xD4, 0x90, 0xD2, 0xD3, 0xDE, 0xD6, 0xD7, 0xD8, // U+00C8..CF
    0xD1, 0xA5, 0xE3, 0xE0, 0xE2, 0xE5, 0x99, 0x9E, // U+00D0..D7
    0x9D, 0xEB, 0xE9, 0xEA, 0x9A, 0xED, 0xE8, 0xE1, // U+00D8..DF
    0x85, 0xA0, 0x83, 0xC6, 0x84, 0x86, 0x91, 0x87, // U+00E0..E7
    0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B, // U+00E8..EF
    0xD0, 0xA4, 0x95, 0xA2, 0x93, 0xE4, 0x94, 0xF6, // U+00F0..F7
    0x9B, 0x97, 0xA3, 0x96, 0x81, 0xEC, 0xE7, 0x98, // U+00F8..FF
};

U8 UnicodeToCp850(U32 codePoint) {
    if (codePoint < 0x80) {
        return (U8)codePoint;
    }
    if (codePoint >= 0xA0 && codePoint <= 0xFF) {
        return kCp850FromLatin1[codePoint - 0xA0];
    }
    switch (codePoint) {
    case 0x0131:
        return 0xD5;
    case 0x0192:
        return 0x9F;
    case 0x2018:
    case 0x2019:
        return '\'';
    case 0x201C:
    case 0x201D:
        return '"';
    case 0x2013:
    case 0x2014:
        return '-';
    case 0x2026:
        return '.';
    default:
        return '?';
    }
}

void CopyUtf8ToCp850(char *dst, size_t dstSize, const char *src) {
    if (!dst || dstSize == 0 || !src) return;
    const U8 *ptr = (const U8 *)src;
    size_t di = 0;

    while (*ptr != '\0' && di + 1 < dstSize) {
        U8 first = *ptr;

        if (first < 0x80) {
            dst[di++] = (char)first;
            ptr++;
            continue;
        }

        if ((first & 0xE0) == 0xC0 && (ptr[1] & 0xC0) == 0x80) {
            U32 codePoint = ((U32)(first & 0x1F) << 6) | (U32)(ptr[1] & 0x3F);
            dst[di++] = (char)UnicodeToCp850(codePoint);
            ptr += 2;
            continue;
        }

        if ((first & 0xF0) == 0xE0 && (ptr[1] & 0xC0) == 0x80 && (ptr[2] & 0xC0) == 0x80) {
            U32 codePoint = ((U32)(first & 0x0F) << 12) |
                            ((U32)(ptr[1] & 0x3F) << 6) |
                            (U32)(ptr[2] & 0x3F);
            dst[di++] = (char)UnicodeToCp850(codePoint);
            ptr += 3;
            continue;
        }

        // Not valid UTF-8: pass the raw byte through (already-CP850 text).
        dst[di++] = (char)first;
        ptr++;
    }

    dst[di] = '\0';
}

void FormatUtf8ToCp850(char *dst, size_t dstSize, const char *fmt, ...) {
    if (!dst || dstSize == 0) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    CopyUtf8ToCp850(dst, dstSize, buf);
}

// =============================================================================
// Unaligned-access emulator.
//
// The engine parses LE byte streams (scenes, tracks, patches) through typed
// pointers — x86 and PPC shrug at unaligned loads, MIPS raises an address
// error. The known swap-site patterns were converted to memcpy-based readers,
// but the CONSUMERS of those in-stream tables (script opcodes and friends)
// also dereference S16/S32 members at odd offsets, and they are legion.
// Same approach as the Linux/MIPS kernel: trap, decode, emulate byte-wise,
// resume. The counter exposes hot offenders worth fixing at the source.
// =============================================================================
#define N64_CAUSE_BD 0x80000000u

U32 s_unalignedCount = 0; // also read by the [hangprof] frame line above

// Emulate one integer load/store at an unaligned address. Returns false if
// the opcode is not one we handle (FPU, exotic).
static bool N64EmulateMemOp(reg_block_t *r, uint32_t insn) {
    uint32_t op = insn >> 26;
    uint32_t rt = (insn >> 16) & 31;
    uint32_t base = (insn >> 21) & 31;
    int16_t imm = (int16_t)(insn & 0xFFFF);
    volatile uint8_t *a =
        (volatile uint8_t *)(uintptr_t)((uint32_t)r->gpr[base] + imm);

    switch (op) {
    case 0x21: // lh
        if (rt) r->gpr[rt] = (uint64_t)(int64_t)(int16_t)((a[0] << 8) | a[1]);
        return true;
    case 0x25: // lhu
        if (rt) r->gpr[rt] = (uint64_t)(uint16_t)((a[0] << 8) | a[1]);
        return true;
    case 0x23: // lw
        if (rt) r->gpr[rt] = (uint64_t)(int64_t)(int32_t)
            (((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
             ((uint32_t)a[2] << 8) | a[3]);
        return true;
    case 0x27: // lwu
        if (rt) r->gpr[rt] = (uint64_t)
            (((uint32_t)a[0] << 24) | ((uint32_t)a[1] << 16) |
             ((uint32_t)a[2] << 8) | a[3]);
        return true;
    case 0x37: { // ld
        uint64_t v = 0;
        for (int i = 0; i < 8; i++) v = (v << 8) | a[i];
        if (rt) r->gpr[rt] = v;
        return true;
    }
    case 0x29: { // sh
        uint16_t v = (uint16_t)r->gpr[rt];
        a[0] = (uint8_t)(v >> 8);
        a[1] = (uint8_t)v;
        return true;
    }
    case 0x2B: { // sw
        uint32_t v = (uint32_t)r->gpr[rt];
        a[0] = (uint8_t)(v >> 24);
        a[1] = (uint8_t)(v >> 16);
        a[2] = (uint8_t)(v >> 8);
        a[3] = (uint8_t)v;
        return true;
    }
    case 0x3F: { // sd
        uint64_t v = r->gpr[rt];
        for (int i = 7; i >= 0; i--) {
            a[i] = (uint8_t)v;
            v >>= 8;
        }
        return true;
    }
    default:
        return false;
    }
}

// Side-effect-free branch decode for the delay-slot case. MIPS points EPC at
// the BRANCH when the fault is in its delay slot, and on resume the whole
// pair re-executes; since we emulate the slot ourselves, we must also apply
// the branch: compute the next PC (evaluating the condition on current regs)
// and any link-register write. Branch-LIKELY variants only execute their
// delay slot when taken, so a fault in the slot implies "taken".
typedef struct {
    uint32_t newPc;
    int linkReg;      // 0 = none
    uint32_t linkVal;
    bool ok;
} N64BranchInfo;

static N64BranchInfo N64DecodeBranch(const reg_block_t *r, uint32_t pc) {
    N64BranchInfo b = {0, 0, 0, false};
    uint32_t insn = *(uint32_t *)(uintptr_t)pc;
    uint32_t op = insn >> 26;
    uint32_t rs = (insn >> 21) & 31;
    uint32_t rtn = (insn >> 16) & 31;
    int32_t off = ((int32_t)(int16_t)(insn & 0xFFFF)) << 2;
    uint32_t taken = pc + 4 + (uint32_t)off;
    uint32_t fall = pc + 8;
    int64_t vrs = (int64_t)r->gpr[rs];
    int64_t vrt = (int64_t)r->gpr[rtn];

    b.ok = true;
    switch (op) {
    case 0x00: { // SPECIAL: jr / jalr
        uint32_t funct = insn & 0x3F;
        if (funct == 0x08) {
            b.newPc = (uint32_t)r->gpr[rs];
        } else if (funct == 0x09) {
            uint32_t rd = (insn >> 11) & 31;
            if (rd) { b.linkReg = (int)rd; b.linkVal = fall; }
            b.newPc = (uint32_t)r->gpr[rs];
        } else {
            b.ok = false;
        }
        break;
    }
    case 0x02: b.newPc = (pc & 0xF0000000u) | ((insn & 0x3FFFFFFu) << 2); break; // j
    case 0x03: // jal
        b.linkReg = 31; b.linkVal = fall;
        b.newPc = (pc & 0xF0000000u) | ((insn & 0x3FFFFFFu) << 2);
        break;
    case 0x04: b.newPc = (vrs == vrt) ? taken : fall; break; // beq
    case 0x05: b.newPc = (vrs != vrt) ? taken : fall; break; // bne
    case 0x06: b.newPc = (vrs <= 0) ? taken : fall; break;   // blez
    case 0x07: b.newPc = (vrs > 0) ? taken : fall; break;    // bgtz
    case 0x14: case 0x15: case 0x16: case 0x17:              // b*l (likely)
        b.newPc = taken;
        break;
    case 0x01: // REGIMM
        switch (rtn) {
        case 0x00: b.newPc = (vrs < 0) ? taken : fall; break;  // bltz
        case 0x01: b.newPc = (vrs >= 0) ? taken : fall; break; // bgez
        case 0x02: case 0x03: b.newPc = taken; break;          // bltzl/bgezl
        case 0x10: b.linkReg = 31; b.linkVal = fall;
                   b.newPc = (vrs < 0) ? taken : fall; break;  // bltzal
        case 0x11: b.linkReg = 31; b.linkVal = fall;
                   b.newPc = (vrs >= 0) ? taken : fall; break; // bgezal
        case 0x12: case 0x13: b.linkReg = 31; b.linkVal = fall;
                   b.newPc = taken; break;                     // *al likely
        default: b.ok = false; break;
        }
        break;
    default:
        b.ok = false;
        break;
    }
    return b;
}

// Emulation is only safe for addresses inside mapped RDRAM: a WILD pointer
// (corrupt offset etc.) raises the same address-error code, and touching it
// byte-wise from inside the handler double-faults into a silent hang. Punt
// those to the inspector so they crash loudly with a backtrace instead.
static bool N64EmulAddrOk(uint32_t addr) {
    uint32_t ramEnd = 0x80000000u + get_memory_size();
    return addr >= 0x80000400u && addr < ramEnd - 8;
}

static void N64UnalignedHandler(exception_t *ex) {
    if (ex->code != EXCEPTION_CODE_LOAD_I_ADDRESS_ERROR &&
        ex->code != EXCEPTION_CODE_STORE_ADDRESS_ERROR) {
        exception_default_handler(ex);
        return;
    }
    reg_block_t *r = ex->regs;

    uint32_t bad = C0_BADVADDR();
    if (bad == r->epc || !N64EmulAddrOk(bad) || !N64EmulAddrOk(r->epc)) {
        // Instruction-fetch fault or wild data address: not emulatable.
        exception_default_handler(ex);
        return;
    }

    if (r->cr & N64_CAUSE_BD) {
        // Fault in a branch delay slot: EPC points at the branch.
        N64BranchInfo b = N64DecodeBranch(r, r->epc);
        if (!b.ok) {
            exception_default_handler(ex);
            return;
        }
        uint32_t slotInsn = *(uint32_t *)(uintptr_t)(r->epc + 4);
        if (!N64EmulateMemOp(r, slotInsn)) {
            exception_default_handler(ex);
            return;
        }
        // GPRs are 64-bit: a zero-extended kseg0 address in ra makes the
        // later `jr ra` raise an instruction-fetch address error. Sign-extend.
        if (b.linkReg) r->gpr[b.linkReg] = (uint64_t)(int64_t)(int32_t)b.linkVal;
        r->epc = b.newPc;
    } else {
        uint32_t insn = *(uint32_t *)(uintptr_t)r->epc;
        if (!N64EmulateMemOp(r, insn)) {
            exception_default_handler(ex);
            return;
        }
        r->epc += 4;
    }

    s_unalignedCount++;
    if (s_unalignedCount <= 8 || (s_unalignedCount % 100000) == 0) {
        debugf("[unaligned] #%lu emulated (total so far)\n",
               (unsigned long)s_unalignedCount);
    }
}

// =============================================================================
// Entry point
// =============================================================================
extern int lba2_engine_main(int argc, char *argv[]);

// Exit screen: the engine quits (and errors out) via exit() directly, which
// would leave a black screen. Hooked with atexit() before the engine starts,
// it tears down the game display and shows the last log-ring lines.
static void N64ShowLogOnExit(void) {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;

    debugf("[LBA2] exit hook: dumping last %d log lines\n", s_logRingCount);

    display_close();
    console_init();
    console_set_render_mode(RENDER_MANUAL);
    console_clear();
    printf("=== LBA2 engine exited - last log ===\n\n");
    int start = (s_logRingPos - s_logRingCount + N64_LOG_RING_LINES) % N64_LOG_RING_LINES;
    for (int i = 0; i < s_logRingCount; ++i) {
        printf("%s\n", s_logRing[(start + i) % N64_LOG_RING_LINES]);
    }
    console_render();

    for (;;) {
        wait_ms(1000);
    }
}

// =============================================================================
// CRT safe area
//
// The VI's default active window (libdragon's presets) fills the whole raster,
// and a real CRT overscans it by roughly 4-8% per side: on the tester's TV the
// first and last letters of every dialogue line were gone. Commercial N64
// titles keep a margin for this. We leave the 320x240 framebuffer and the
// engine's layout untouched and shrink the VI output window instead, scaling
// the picture into it (VI resample), so the fix applies to every screen at
// once and costs nothing at runtime. Emulators and upscalers show a thin black
// border; the CRT shows the whole picture. Works off the registers display_init
// wrote, so NTSC/PAL/MPAL presets are all handled the same way.
#define VI_REG(off) (*(volatile uint32_t *)(0xA4400000u + (off)))
#define VI_REG_H_VIDEO 0x24
#define VI_REG_V_VIDEO 0x28
#define VI_REG_X_SCALE 0x30
#define VI_REG_Y_SCALE 0x34

static uint32_t s_viBaseHVideo, s_viBaseVVideo, s_viBaseXScale, s_viBaseYScale;
static int s_viBaseCaptured = 0;
static int s_screenMarginPct = -1;

extern "C" void N64SetScreenMargin(int pct) {
    if (pct < 0) pct = 0;
    if (pct > 15) pct = 15;
    if (!s_viBaseCaptured) {
        s_viBaseHVideo = VI_REG(VI_REG_H_VIDEO);
        s_viBaseVVideo = VI_REG(VI_REG_V_VIDEO);
        s_viBaseXScale = VI_REG(VI_REG_X_SCALE);
        s_viBaseYScale = VI_REG(VI_REG_Y_SCALE);
        s_viBaseCaptured = 1;
    }
    if (pct == s_screenMarginPct) return;
    s_screenMarginPct = pct;

    // H_VIDEO/V_VIDEO: [25:16] start, [9:0] end (dot clocks / half-lines).
    // X_SCALE/Y_SCALE: [11:0] 2.10 fixed "framebuffer units per output unit".
    uint32_t hs = (s_viBaseHVideo >> 16) & 0x3FF, he = s_viBaseHVideo & 0x3FF;
    uint32_t vs = (s_viBaseVVideo >> 16) & 0x3FF, ve = s_viBaseVVideo & 0x3FF;
    uint32_t hw = he - hs, vh = ve - vs;
    uint32_t nhw = hw * (100 - 2 * pct) / 100;
    uint32_t nvh = vh * (100 - 2 * pct) / 100;
    // Keep start/end parity (field/phase) by moving the edges by even amounts.
    uint32_t dh = ((hw - nhw) / 2) & ~1u, dv = ((vh - nvh) / 2) & ~1u;
    nhw = hw - 2 * dh;
    nvh = vh - 2 * dv;
    uint32_t xs = (s_viBaseXScale & 0xFFF) * hw / nhw;
    uint32_t ys = (s_viBaseYScale & 0xFFF) * vh / nvh;

    uint32_t hv = ((hs + dh) << 16) | (he - dh);
    uint32_t vv = ((vs + dv) << 16) | (ve - dv);
    uint32_t xr = (s_viBaseXScale & ~0xFFFu) | (xs & 0xFFF);
    uint32_t yr = (s_viBaseYScale & ~0xFFFu) | (ys & 0xFFF);

    disable_interrupts();
    VI_REG(VI_REG_H_VIDEO) = hv;
    VI_REG(VI_REG_V_VIDEO) = vv;
    VI_REG(VI_REG_X_SCALE) = xr;
    VI_REG(VI_REG_Y_SCALE) = yr;
    enable_interrupts();
    debugf("[LBA2] screen margin %d%%: H %lu..%lu V %lu..%lu xscale 0x%03lx yscale 0x%03lx\n",
           pct, (unsigned long)(hs + dh), (unsigned long)(he - dh),
           (unsigned long)(vs + dv), (unsigned long)(ve - dv),
           (unsigned long)xs, (unsigned long)ys);
}

extern "C" int N64GetScreenMargin(void) {
    return s_screenMarginPct < 0 ? 0 : s_screenMarginPct;
}

int main(int argc, char *argv[]) {
    debug_init_isviewer();
    debug_init_usblog();

    debugf("[LBA2] N64 boot " N64LBA_BUILD_TAG "\n");

    timer_init();
    joypad_init();
    register_exception_handler(N64UnalignedHandler);

    // The engine renders 640x480 8bpp; display at 320x240 (RAM diet: a
    // 640x480 16bpp double buffer would eat 1.2 MB) and let the RDP both
    // downscale and do the CI8->RGBA16 palette lookup during the blit.
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 2, GAMMA_NONE, FILTERS_RESAMPLE);
    rdpq_init();
    N64SetScreenMargin(N64_DEFAULT_SCREEN_MARGIN); // LBA2.CFG "ScreenMargin" re-applies later

    debugf("[LBA2] RDRAM: %lu MB%s\n",
           (unsigned long)(get_memory_size() / (1024UL * 1024UL)),
           get_memory_size() >= 0x800000 ? " (Expansion Pak)" : " - WARNING: 8 MB required");

    int dfsOk = (dfs_init(DFS_DEFAULT_LOCATION) == DFS_ESUCCESS);
    debugf("[LBA2] DFS: %s\n", dfsOk ? "mounted (rom:/)" : "NOT PRESENT (no assets in ROM yet)");

    // Saves and lba2.cfg live in cartridge SRAM (engine user dir = "sram:/").
    N64SramFs_Init();

    // Hardware triage build only (rom:/audiodiag): PI read self-test report.
    N64AudioDiag_BootTest();

    (void)argc;
    (void)argv;
    static char progName[] = "lba2";
    static char *engineArgv[] = {progName, NULL};

    // Engine error paths call exit() directly; make sure they land on the
    // log screen instead of a black display.
    atexit(N64ShowLogOnExit);

    debugf("[LBA2] entering engine main()\n");
    int rc = lba2_engine_main(1, engineArgv);
    debugf("[LBA2] engine returned %d\n", rc);

    N64ShowLogOnExit(); // never returns
    return rc;
}

#ifdef __cplusplus
}
#endif
