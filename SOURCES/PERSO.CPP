#include "C_EXTERN.H"

#include "BUILD_INFO.h" /* LBA2_VERSION_STRING (for --version flag) */
#include "INITADEL.H"
#include "DIRECTORIES.H"
#include "RES_DISCOVERY.H"
#include "INPUT.H"
#ifdef LBA2_TARGET_N64
#include "JOYSTICK.H" // N64StickMove
#include "MUSIC.H"    // JingleVolume / CDVolume
#endif
#include "CONSOLE/CONSOLE.H"
#include <SYSTEM/EVENTS.H>
#include <SVGA/VIDEO.H>
#include <FILEIO/SAVEPNG.H>

#include <SVGA/SCREEN.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include "FOLLOWCAM_CFG.H"
#include <SYSTEM/STRING.H>
#include <SYSTEM/WINDOW.H>

#ifdef DEBUG_TOOLS
static inline U32 DosAvailableMem(void) { return 0; }
#endif

#ifdef LBA2_TARGET_N64
// LIB386/SYSTEM/N64_BACKEND.CPP: CRT overscan safe area, percent per side.
extern "C" void N64SetScreenMargin(int pct);
// "DebugStartCube: N" in LBA2.CFG: skip the menus and start a new game in
// cube N (the console's `cube` command is stubbed on N64). -1 = off.
static S32 N64DebugStartCube = -1;
// "DebugSaveTest: 1" in LBA2.CFG: once the first cube is up, exercise the
// SRAM save path without any input (save a slot + current.lba, list the
// saves, read the slot back) and log the outcome. Emulator-side self-test
// for the cartridge filesystem; 0 = off.
static S32 N64DebugSaveTest = 0;
// "DebugFullRedraw: 1": redraw the whole exterior (terrain + horizon) every
// frame, as if the camera never stopped moving — steady-state cost of a
// walking frame measurable from a DebugStartCube boot with no input.
static S32 N64DebugFullRedraw = 0;
// "DebugVoiceWait: 1": once the first cube is up, play a short voice clip and
// wait for it with the GameAskChoice loop (MyGetInput only) — the loop that
// hung on Twinsen's spoken answers until START. Logs [voicewait].
static S32 N64DebugVoiceWait = 0;
extern "C" S32 N64AudioTest_VoiceWait(const char *base, void (*poll)(void), U32 timeoutMs);

static void N64RunDebugVoiceWait(void) {
    static S32 done = FALSE;
    if (!N64DebugVoiceWait OR done) return;
    done = TRUE;
    S32 ms = N64AudioTest_VoiceWait("en_000_0065", MyGetInput, 10000);
    if (ms < 0)
        LogPrintf("[voicewait] FAIL: voice still playing after 10 s of MyGetInput()\n");
    else
        LogPrintf("[voicewait] OK: voice ended after %ld ms\n", (long)ms);
}

static void N64RunDebugSaveTest(void) {
    static S32 done = FALSE;
    if (!N64DebugSaveTest OR done) return;
    done = TRUE;

    char memoplayername[MAX_SIZE_PLAYER_NAME + 1];
    char memogamepathname[ADELINE_MAX_PATH];
    strcpy(memoplayername, PlayerName);
    strcpy(memogamepathname, GamePathname);

    LogPrintf("[sramtest] cube %d: saving slot 'SRAMTEST'\n", (int)NumCube);
    strcpy(PlayerName, "SRAMTEST");
    GetSavePath(GamePathname, ADELINE_MAX_PATH, "SRAMTEST.LBA");
    NumVersion = NUM_VERSION | SAVE_COMPRESS;
    SaveGame(TRUE);
    LogPrintf("[sramtest] slot on disk: %lu bytes\n", (unsigned long)FileSize(GamePathname));

    CurrentSaveGame(); // current.lba, the "resume game" file

    strcpy(PlayerName, "");
    LoadGamePlayerName();
    LogPrintf("[sramtest] read back: player '%s'%s\n", PlayerName,
              strcmp(PlayerName, "SRAMTEST") ? "  <-- MISMATCH" : "");

    S32 nb = PlayerGameList((char **)BufSpeak, (char *)BufSpeak + 100000L, FALSE);
    LogPrintf("[sramtest] PlayerGameList: %d save(s)\n", (int)nb);
    for (S32 i = 0; i < nb; i++) LogPrintf("[sramtest]   %s\n", ((char **)BufSpeak)[i]);

    // Thumbnail of current.lba, the way the pause menu draws it on every
    // cursor move (GAMEMENU.CPP DoGameMenu). This is the decompression path:
    // its header fields sit at odd offsets, and reading the compressed size
    // wrong is what sent memcpy to a wild address on a tester's console.
    {
        U8 memonumversion = NumVersion;
        S32 memonewcube = NewCube;
        U8 *ptr;

        GetSavePath(GamePathname, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
        ptr = LoadGameScreen();
        LogPrintf("[sramtest] LoadGameScreen(current.lba): %s (version 0x%02x, cube %d, player '%s')\n",
                  ptr ? "ok" : "REFUSED", (unsigned)NumVersion, (int)NewCube, PlayerName);

        NumVersion = memonumversion;
        NewCube = memonewcube;
    }

    if (N64DebugSaveTest >= 2) {
        // Fill the cartridge: how many slots fit, and does a failed write
        // leave the filesystem intact? Extra slots are removed afterwards.
        S32 n = 0;
        for (;;) {
            char fname[32];
            snprintf(PlayerName, MAX_SIZE_PLAYER_NAME + 1, "FILL%02d", (int)n);
            snprintf(fname, sizeof(fname), "%s.LBA", PlayerName);
            GetSavePath(GamePathname, ADELINE_MAX_PATH, fname);
            SaveGame(TRUE);
            if (!FileSize(GamePathname)) break;
            n++;
        }
        LogPrintf("[sramtest] SRAM full after %d extra slot(s)\n", (int)n);
        for (S32 i = 0; i < n; i++) {
            char fname[32];
            snprintf(fname, sizeof(fname), "FILL%02d.LBA", (int)i);
            GetSavePath(GamePathname, ADELINE_MAX_PATH, fname);
            Delete(GamePathname);
        }
        nb = PlayerGameList((char **)BufSpeak, (char *)BufSpeak + 100000L, FALSE);
        LogPrintf("[sramtest] after cleanup: %d save(s)\n", (int)nb);
    }

    strcpy(PlayerName, memoplayername);
    strcpy(GamePathname, memogamepathname);
}
#endif

#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_sdl_compat.h>
#include <SYSTEM/wiiu_endian.h>
#  define WIIU_TRACE(s) LogPrintf("[trace] " s)
#  define WIIU_FIRST(flag, s) do { if (!(flag)) { LogPrintf("[trace] " s); } } while(0)
#  define WIIU_FIRST_FMT(flag, ...) do { if (!(flag)) { LogPrintf(__VA_ARGS__); } } while(0)

// Canary: detects when HQR_Anims->Name[0] (expected '/') is corrupted.
// Logs once at the first observed change so we can pinpoint the culprit.
extern "C" void WiiU_AnimsCanary(const char *where);
#  define WIIU_CANARY(where) WiiU_AnimsCanary(where)

// Blit accumulators (LIB386/SYSTEM/WIIU_BACKEND.CPP): total OSTime ticks
// spent inside WiiU_BlitGameFramebuffer and number of blits since last
// profiler report.
extern "C" OSTime WiiUProf_BlitTicks;
extern "C" OSTime WiiUProf_BlitConvTicks;
extern "C" OSTime WiiUProf_BlitFlushTicks;
extern "C" OSTime WiiUProf_BlitFlipTicks;
extern "C" U32    WiiUProf_BlitCount;

// MainLoop phase profiler. Everything between the previous frame's
// AffScene-end and this frame's AffScene-begin counts as "logic" (input,
// scripts, anims, collisions, camera); AffScene itself is "render"
// (software raster + OSScreen blit — the blit share is split out via the
// backend accumulators). One [prof] log line every 300 sampled frames
// (~5-10 s), cheap enough to leave enabled.
static void WiiUProf_FrameSample(OSTime affBegin, OSTime affEnd) {
    static OSTime s_lastEnd = 0;
    static OSTime s_logic = 0, s_render = 0, s_total = 0;
    static U32 s_n = 0;

    if (s_lastEnd != 0) {
        s_logic += affBegin - s_lastEnd;
        s_render += affEnd - affBegin;
        s_total += affEnd - s_lastEnd;
        s_n++;
    }
    s_lastEnd = affEnd;

    if (s_n < 300)
        return;

    U32 totUs = (U32)(OSTicksToMicroseconds(s_total) / s_n);
    U32 logUs = (U32)(OSTicksToMicroseconds(s_logic) / s_n);
    U32 renUs = (U32)(OSTicksToMicroseconds(s_render) / s_n);
    U32 blitUs = WiiUProf_BlitCount
                     ? (U32)(OSTicksToMicroseconds(WiiUProf_BlitTicks) / WiiUProf_BlitCount)
                     : 0;
    U32 convUs = WiiUProf_BlitCount
                     ? (U32)(OSTicksToMicroseconds(WiiUProf_BlitConvTicks) / WiiUProf_BlitCount)
                     : 0;
    U32 flushUs = WiiUProf_BlitCount
                     ? (U32)(OSTicksToMicroseconds(WiiUProf_BlitFlushTicks) / WiiUProf_BlitCount)
                     : 0;
    U32 flipUs = WiiUProf_BlitCount
                     ? (U32)(OSTicksToMicroseconds(WiiUProf_BlitFlipTicks) / WiiUProf_BlitCount)
                     : 0;
    U32 fps10 = totUs ? (10000000u / totUs) : 0;

    LogPrintf("[prof] %u frames avg: total=%u.%ums (%u.%u fps) logic=%u.%ums affscene=%u.%ums "
              "(blit=%u.%ums x%u: conv=%u.%u flush=%u.%u flip=%u.%u)\n",
              (unsigned)s_n,
              (unsigned)(totUs / 1000u), (unsigned)((totUs % 1000u) / 100u),
              (unsigned)(fps10 / 10u), (unsigned)(fps10 % 10u),
              (unsigned)(logUs / 1000u), (unsigned)((logUs % 1000u) / 100u),
              (unsigned)(renUs / 1000u), (unsigned)((renUs % 1000u) / 100u),
              (unsigned)(blitUs / 1000u), (unsigned)((blitUs % 1000u) / 100u),
              (unsigned)WiiUProf_BlitCount,
              (unsigned)(convUs / 1000u), (unsigned)((convUs % 1000u) / 100u),
              (unsigned)(flushUs / 1000u), (unsigned)((flushUs % 1000u) / 100u),
              (unsigned)(flipUs / 1000u), (unsigned)((flipUs % 1000u) / 100u));

    s_logic = s_render = s_total = 0;
    s_n = 0;
    WiiUProf_BlitTicks = 0;
    WiiUProf_BlitConvTicks = 0;
    WiiUProf_BlitFlushTicks = 0;
    WiiUProf_BlitFlipTicks = 0;
    WiiUProf_BlitCount = 0;
}
#else
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#  define WIIU_TRACE(s) ((void)0)
#  define WIIU_FIRST(flag, s) ((void)0)
#  define WIIU_FIRST_FMT(flag, ...) ((void)0)
#  define WIIU_CANARY(where) ((void)0)
#endif
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*#ifndef DEMO
#define THOMAS  1
#endif
*/

#ifdef DEMO
extern char *Version;
#else
extern const char *Version;
#endif

extern T_OBJ_3D InvObjTwinsen[];

extern S32 PersoFlashTimer; // pour quand le perso eh ben y meurt !!!

S32 EscTimer;

/* FollowCamera refreshes terrain with AFF_ALL_FLIP every movement frame.
 * AFF_ALL_FLIP runs the full AffScene preamble (MemoClipWindow / UnsetClipWindow
 * / ClearImpactRain) before the render, preventing black edges from active clip
 * regions.  The timer lock during render+vsync is compensated by the
 * SaveTimer/RestoreTimer + TimerSystemHR adjustment around AffScene below, so
 * game logic (hero, NPCs, rain) runs at correct wall-clock speed.
 * Vsync naturally caps terrain refreshes at the display rate (~60 Hz).
 * When the hero is idle, all work is skipped entirely. */
/* Default resting distance for the spring arm: midpoint of the two VueCamera
 * presets (DefVueDistance[0]=10500, DefVueDistance[1]=17000). */
#define FOLLOW_CAM_INITIAL_DIST ((DefVueDistance[0] + DefVueDistance[1]) / 2)
static S32 FollowCamTerrainFrame = FALSE;
static S32 FollowCamPrevHeroX = 0;
static S32 FollowCamPrevHeroY = 0;
static S32 FollowCamPrevHeroZ = 0;
static S32 FollowCamPrevHeroBeta = 0;
static S32 FollowCamPrevAddBeta = 0;   /* dirty check: last seen AddBetaCam */
static S32 FollowCamEffectiveDist = 0; /* spring arm: smoothed distance (0 = uninitialised) */
static S32 FollowCamTargetBeta = 0;    /* rotation lag: desired BetaCam */
static S32 FollowCamReengageDelay = 0; /* frames before auto-center resumes after manual pan */
static S32 FollowCamPrevBaseDist = -1; /* dirty: zoom from GereExtKeys (numpad * /) */
static S32 FollowCamPrevAlphaCam = -1; /* dirty: numpad +/- elevation while follow */

/*══════════════════════════════════════════════════════════════════════════*/
char NamePcxSave[ADELINE_MAX_PATH];
S32 NumPcxSave = 0;

#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
/*══════════════════════════════════════════════════════════════════════════*/

U32 MemoMemory;
U32 MemEnd;
U32 MemoDosMemory;
U32 MemoMinDosMemory;

S32 MinNbf = 1000;
S32 MaxNbf = 0;

S32 NbNbf = 0;
S32 TotalNbf = 0;

/*══════════════════════════════════════════════════════════════════════════*/

void AffDebugMenu() {
    S32 lig = 0;

    CoulText(LBAWHITE, 0);

    GraphPrintf(FALSE, 0, lig += 9, "Island: %d", Island);
    GraphPrintf(FALSE, 0, lig += 9, "Cube: %d", NumCube);
    GraphPrintf(FALSE, 0, lig += 9, "Chapter: %d", ListVarGame[FLAG_CHAPTER]);

    GraphPrintf(FALSE, 0, lig += 9, "CmptMemoTimer: %d", CmptMemoTimerRef);
    GraphPrintf(FALSE, 0, lig += 9, "Nb Objs: %d", NbObjets);
    GraphPrintf(FALSE, 0, lig += 9, "Nb Zones: %d", NbZones);
    GraphPrintf(FALSE, 0, lig += 9, "Nb Tracks: %d", NbBrickTrack);

    GraphPrintf(FALSE, 0, lig += 10, "NbFPS: %d", NbFramePerSecond);

    GraphPrintf(FALSE, 0, lig += 9, "Free(K): %d", (AvailableMem()) / 1024);
    GraphPrintf(FALSE, 0, lig += 10, "Memory at start: %d Ko", MemoMemory / 1024);

    GraphPrintf(FALSE, 0, lig += 9, "Total Extend Memory used: %d Ko", (MemoMemory - (AvailableMem())) / 1024);

    lig += 9;

    GraphPrintf(FALSE, 0, lig += 9, "CameraY     : %d", CameraY);

    lig += 9;

    GraphPrintf(FALSE, 0, lig += 9, "AlphaCam     : %d", AlphaCam);
    GraphPrintf(FALSE, 0, lig += 9, "VueDistance  : %d", VueDistance);
}
#endif

/*══════════════════════════════════════════════════════════════════════════*/

// Pour eviter d'ecraser les anciennes copies d'ecran !!!!
void GetNumPcxSave() {
    struct dirent *file;
    DIR *dir;
    S32 num;
    char shootDirPath[ADELINE_MAX_PATH];
    char pathname[ADELINE_MAX_PATH];

    GetShootPath(shootDirPath, ADELINE_MAX_PATH, NULL);
    dir = opendir(shootDirPath);
    if (dir != NULL) {
        while ((file = readdir(dir)) != NULL) {
            bool startsLBA = (ade_strcasestr(file->d_name, "LBA") == file->d_name);
            bool hasPCX = (ade_strcasestr(file->d_name, ".PCX") != NULL);
            if (startsLBA && hasPCX) {
                GetShootPath(pathname, ADELINE_MAX_PATH, file->d_name);

                struct stat st = {};
                stat(pathname, &st);
                if (S_ISREG(st.st_mode)) {
                    num = atoi(&file->d_name[3]);
                    if (num >= NumPcxSave) {
                        NumPcxSave = num + 1;
                    }
                }
            }
        }
        closedir(dir);
    }
}

void CheckSavePcx() {
    if (Key == K_F9) {
        S32 saveflagfade;

        SaveTimer();
        MemoClipWindow();
        UnsetClipWindow();

        BackupScreen(FALSE);
        CopyScreen(Log, Screen);

        GetNumPcxSave();

        char PCXFilename[ADELINE_MAX_PATH];
        snprintf(PCXFilename, sizeof(PCXFilename), "LBA%05d.PCX", NumPcxSave);
        GetShootPath(NamePcxSave, ADELINE_MAX_PATH, PCXFilename);

        CoulText(LBAWHITE, 0);
        GraphPrintf(TRUE, 50, 0, "Saving Screen into %s", NamePcxSave);

        SavePCX(NamePcxSave, Screen, ModeDesiredX, ModeDesiredY, PtrPal);

        WaitNoInput();

        // RestoreScreen without BoxUpdate()
        CopyScreen(ScreenAux, Screen);
        CopyScreen(ScreenAux, Log);

        RestoreTimer();
        SaveTimer();

        saveflagfade = FlagFade;
        FlagFade = FALSE;

        RestoreClipWindow();

        BoxReset();
        BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

        AffScene(AFF_OBJETS_NO_FLIP);
        FixeCinemaMode(FALSE);

        FlagFade = saveflagfade;
        GraphPrintf(FALSE, 50, 0, "Saving Screen into %s", NamePcxSave);
        BoxBlit();
        RestoreTimer();
    }
}

void CheckSaveLogPcx(U8 *pal) {
    if (Key == K_F9) {
        SaveTimer();

        GetNumPcxSave();

        char PCXFilename[ADELINE_MAX_PATH];
        snprintf(PCXFilename, sizeof(PCXFilename), "LBA%05d.PCX", NumPcxSave);
        GetShootPath(NamePcxSave, ADELINE_MAX_PATH, PCXFilename);

        SavePCX(NamePcxSave, Log, ModeDesiredX, ModeDesiredY, pal);

        CoulText(LBAWHITE, 0);
        GraphPrintf(TRUE, 50, 0, "Saving Screen into %s", NamePcxSave);

        WaitNoInput();

        RestoreTimer();
    }
}

//****************************************************************************
/*══════════════════════════════════════════════════════════════════════════*
                          █    ██▄ █  █    ▀▀█▀▀ ██▀▀▀
                          ██   ██▀██  ██     ██  ▀▀▀▀█
                          ▀▀   ▀▀  ▀  ▀▀     ▀▀  ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void LoadListAnim3DS(void) {
    U32 handle, buffer;
    S32 n;
    char tmpFilePath[ADELINE_MAX_PATH];

    // recupere nombre d'anim dans la banque
    handle = OpenRead(HQRPtrAnim3DS->Name);
    if (handle) {
        Read(handle, &buffer, 4);
        Close(handle);
#ifdef LBA2_TARGET_WIIU
        // First 4 bytes of anim3ds.hqr are the leading U32 of the HQR offset
        // table (LE retail), used here as a poor-man's record-count probe:
        // n = (first_offset/4) - 2. Without the swap, n becomes a huge index
        // and LoadMalloc_HQR(_, n) returns NULL -> TheEndCheckFile reports
        // "Not Enough Memory" (misleading: it's a lookup miss, not an OOM).
        buffer = LBA2_LE32(buffer);
#endif
        n = (buffer / 4) - 2;
#ifdef LBA2_TARGET_WIIU
        LogPrintf("[anim3ds] header_first_off=0x%08X -> n=%d\n", buffer, n);
#endif
    }

    // Charge table de description des anims
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, ANIM3DS_HQR_NAME);
    ListAnim3DS = (T_ANIM_3DS *)LoadMalloc_HQR(tmpFilePath, n);
    if (!ListAnim3DS)
        TheEndCheckFile(tmpFilePath);

#ifdef LBA2_TARGET_WIIU
    // T_ANIM_3DS = {char Name[4]; S16 Deb; S16 Fin}, raw LE table consumed
    // as native S16 by GERETRAK/GERELIFE TM_/LM_*_3DS macros and StartInitObj.
    // Unswapped, Deb/Fin become frame indexes like 0x6700 instead of 0x0067:
    // every ANIM_3DS scene object (glass sphere in the Citadel sewers, clam
    // on Desert Island, ...) picks its animation frames from the wrong place
    // in ANIM3DS.HQR. Name[4] must NOT be swapped.
    {
        S32 cnt = LoadMallocFileSize / (S32)sizeof(T_ANIM_3DS);
        S32 i;

        for (i = 0; i < cnt; i++) {
            ListAnim3DS[i].Deb = (S16)LBA2_LE16((U16)ListAnim3DS[i].Deb);
            ListAnim3DS[i].Fin = (S16)LBA2_LE16((U16)ListAnim3DS[i].Fin);
            LogPrintf("[anim3ds] %2d '%c%c%c%c' Deb=%d Fin=%d\n", (int)i,
                      ListAnim3DS[i].Name[0], ListAnim3DS[i].Name[1],
                      ListAnim3DS[i].Name[2], ListAnim3DS[i].Name[3],
                      (int)ListAnim3DS[i].Deb, (int)ListAnim3DS[i].Fin);
        }
    }
#endif
}

/*──────────────────────────────────────────────────────────────────────────*/
void InitGameLists() {
    T_INCRUST_DISP *ptrdisp = ListIncrustDisp;
    S32 i;

    // Peut-etre inutile car fait ailleurs (a verifier ?)
    ClearExtra();

    for (i = 0; i < MAX_INCRUST_DISP; i++, ptrdisp++) {
        ptrdisp->Num = -1;
    }

    memset(ListVarCube, 0, MAX_VARS_CUBE * sizeof(ListVarCube[0]));
    memset(ListVarGame, 0, MAX_VARS_GAME * sizeof(ListVarGame[0]));

    ResetBuggy();
    ResetArdoise();

    // Init VarGames
    InitInventory();

    for (i = 0; i < 4; i++) {
        SampleAmbiance[i] = -1;
        SampleRepeat[i] = 1;
        SampleRnd[i] = 1;
    }

    NbObjets = 0;
    NbZones = 0;
    NbBrickTrack = 0;
}

/*══════════════════════════════════════════════════════════════════════════*/

void InitGame(int argc) {
    /*-------------------------------------------------------------------------*/
    UnsetClip();

    AlphaLight = 896 * MUL_ANGLE;
    BetaLight = 950 * MUL_ANGLE;

    PtrInit3DGame();

    InitGameLists();

    InitPerso();
    InitHoloMap();

    SceneStartX = 16 * SIZE_BRICK_XZ;
    SceneStartY = 24 * SIZE_BRICK_Y;
    SceneStartZ = 16 * SIZE_BRICK_XZ;

    /*-------------------------------------------------------------------------*/
    /* init FIRST scene files */

    NewCube = 0;
    NumCube = -1;

    FlagTheEnd = -1;
    MagicLevel = 0;
    MagicPoint = 0;
    NbGoldPieces = 0;
    NbZlitosPieces = 0;
    NbLittleKeys = 0;
    ListVarGame[FLAG_CHAPTER] = 0;
    Weapon = FLAG_BALLE_MAGIQUE;
    Island = 0;
    ListVarGame[FLAG_CLOVER] = NB_CLOVER_DEFAULT;
    NbCloverBox = NB_CLOVER_BOX_DEFAULT;
    FlagWater = FALSE;
    NumObjFollow = NUM_PERSO;
    SaveComportement = Comportement = C_NORMAL;

    // Mettre a jour ici pour commencer le jeu directement en mode cinema
    CinemaMode = FALSE;

    if (argc == -1) {
        FlagLoadGame = TRUE;
        LoadGameNumCube();
    } else {
        FadeToBlack(PtrPal);
        Cls();
        BoxUpdate();
    }
}

/*──────────────────────────────────────────────────────────────────────────*/
void GamePaused(U32 flags) {
    T_INCRUST_DISP *ptrdisp = NULL;
    S32 n;
    S32 endtimer;
    S32 fin = FALSE;

    SaveTimer();

    if (flags & PAUSE_CLOVER) {
        n = InitIncrustDisp(INCRUST_OBJ | INCRUST_YCLIP,
                            BODY_3D_CLOVER,
                            0, 0,
                            0, 0, 3);

        if (n != -1) {
            ptrdisp = &ListIncrustDisp[n];
        }

        PersoFlashTimer = TimerRefHR;
    }

    if (flags & PAUSE_MESSAGE) {
        ColorFont(LBAWHITE);
        Font(5, ClipWindowYMax - 33, "Pause");

        BoxMovingAdd(5, ClipWindowYMax - 33, 100, ClipWindowYMax);
        BoxBlit();
    }

    InitWaitNoInput(ALL_INPUTS);
    InitWaitNoKey();

    MyGetInput();

    endtimer = TimerRefHR + 1000 * 5; // Wait 5 sec

    // Magouille pour ne pas avoir la gestion des extras, pof ...
    FlagAnimWhoSpeak = TRUE;

    while (!fin) {
        ManageTime();

        if (ptrdisp) {
            ptrdisp->TimerEnd = TimerRefHR + 3 * 1000; // 3 s
            AffScene(AFF_OBJETS_NO_FLIP);
            BoxUpdate();
        }

        MyGetInput();

        if (MyKey OR Input)
            fin = TRUE;

        if (flags & PAUSE_CLOVER AND TimerRefHR > endtimer)
            fin = TRUE;
    }

    FlagAnimWhoSpeak = FALSE;

    RestoreTimer();

    if (ptrdisp) {
        ptrdisp->TimerEnd = TimerRefHR + 1000;
    }

    PersoFlashTimer = 0;

    ClearWaitNoInput();
}

/*══════════════════════════════════════════════════════════════════════════*
             █▄ ▄█ █▀▀▀█  █    ██▄ █       █     █▀▀▀█ █▀▀▀█ █▀▀▀█
             ██▀ █ ██▀▀█  ██   ██▀██       ██    ██  █ ██  █ ██▀▀▀
             ▀▀  ▀ ▀▀  ▀  ▀▀   ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

// en attendant les Input 64 bits (Watcom 11 ?)
U8 WaitNoKey32 = FALSE;
U8 WaitNoKey33 = FALSE;
U8 WaitNoKey34 = FALSE;
U8 WaitNoKey35 = FALSE;

/*──────────────────────────────────────────────────────────────────────────*/

S32 MainLoop() {
    S32 xm, ym, zm;
    T_OBJET *ptrobj;
    U8 i;
    S32 pansample;
#ifdef LBA2_TARGET_WIIU
    // Gates the per-iteration trace markers. Set to 1 at the end of the first
    // while(TRUE) body so subsequent iterations stay silent during gameplay.
    static U8 wiiu_first_iter_done = 0;
#endif

    WIIU_TRACE("MainLoop: SetDetailLevel enter");
    SetDetailLevel(); // prise en compte des différentes options
    WIIU_TRACE("MainLoop: SetDetailLevel exit");

restartloop:

    WIIU_TRACE("MainLoop: restartloop label");
    FirstTime = AFF_ALL_FLIP;
    FlagLoadGame = FALSE;

    WIIU_FIRST(wiiu_first_iter_done, "MainLoop: InitWaitNoKey/InitWaitNoInput enter");
    InitWaitNoKey();
    ClearWaitNoInput();
    InitWaitNoInput(I_FIRE | I_MENUS | I_OPTIONS | I_SAVE | I_LOAD);
    WIIU_FIRST(wiiu_first_iter_done, "MainLoop: InitWaitNoKey/InitWaitNoInput exit");

    /*-------------------------------------------------------------------------*/

    if (EscTimer) {
        SetTimerHR(EscTimer);
        ResumeMusic(FALSE);
    }

    EscTimer = 0;

    /*-------------------------------------------------------------------------*/

    WIIU_FIRST(wiiu_first_iter_done, "MainLoop: InitMove(RealFalling/Shifting/SampleAlways) enter");
    /* vitesse de chute propor pour tous les objets */
    InitMove(&RealFalling, -2560);
    /* vitesse de decalage propor tapis roulant */
    InitMove(&RealShifting, 640);

    /* frequence de paning des samples always */
    InitMove(&SampleAlwaysMove, 2);
    WIIU_FIRST(wiiu_first_iter_done, "MainLoop: InitMove exit");

    /*-------------------------------------------------------------------------*/

    FirstSave = TRUE;

    WIIU_TRACE("MainLoop: entering while(TRUE) loop");
#ifdef LBA2_TARGET_WIIU
    {
        extern volatile U8 wiiu_gameloop_active;
        wiiu_gameloop_active = 1;
    }
#endif
    while (TRUE) {
    startloop:
        WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: while(TRUE) FIRST iter top, NewCube=%d\n", NewCube);

        /*#ifdef  THOMAS
		if( DefKeys[0].Key1==0 )
		{
			PalOne( LBAWHITE, 255, 255, 255 ) ;
			CoulText( LBAWHITE, 0 ) ;
			GraphPrintf( TRUE, 0, 0, "STOP: Patch Inputs trouv\x82 !!!", TRUE ) ;
			while( Key ) ;
			while( !Key ) ;
		}
#endif
*/
        ptrobj = &ListObjet[NUM_PERSO];

        // DEBUG pour trouver bug Alpha

        // debug patch Gamma
        if (ptrobj->Obj.Alpha != 0 OR ptrobj->Obj.Gamma != 0) {
            Message("STOP: 'j'ai trouv\x82 le Patch_Alpha' !", TRUE);
        }

        /*-------------------------------------------------------------------------*/

        if (NewCube != -1) {
#ifdef DEMO
            if (NewDemoSave != CurrentDemoSave) {
                FlagChgCube = 0;

                DemoLogo();

                if (NewDemoSave > NB_DEMOSAVE - 1) {
                    return 1;
                } else {
                    SetDemoSaveGame(NewDemoSave);

                    StopMusic();

                    InitGame(-1);
                    ChangeCube();

                    CopyScreen(Log, ScreenAux);

                    RestoreTimer();
                    SaveTimer();
                    AffScene(AFF_ALL_NO_FLIP);
                    FixeCinemaMode(FALSE);
                    CopyScreen(Log, Screen);

                    // au cas ou le mec meurre avant de faire <ESC>
                    CurrentSaveGame();

                    BoxReset(); // pas de flip

                    ZoomSavedGame(240, 10);

                    FlagFade = FALSE;
                    RestartRainSample = TRUE;
                    HQ_ResumeSamples();

                    ClearPlasmaMenu();

                    RestoreTimer();

                    goto restartloop;
                }
            } else
#endif
            {
                WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: ChangeCube enter (NewCube=%d, NumCube=%d)\n", NewCube, NumCube);
                ChangeCube();
                WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: ChangeCube exit (now NumCube=%d)\n", NumCube);
#ifdef LBA2_TARGET_N64
                N64RunDebugSaveTest();
                N64RunDebugVoiceWait();
#endif
            }

            FirstLoop = TRUE; // premiere boucle d'un cube
                              // ATTENTION: a verifier si
                              // cela ne pose pas de probleme !
            FlagReajustPosTwinsen = TRUE;
        }

/*-------------------------------------------------------------------------*/
#ifdef DEBUG_TOOLS
        UnsetClip();

        if (CmptMemoTimerRef != 0) {
            CoulText(LBAWHITE, 0);
            GraphPrintf(FALSE, 0, 10, "Warning, SaveTimer & RestoreTimer Decale!!!");
            GraphPrintf(FALSE, 0, 20, "Valeur CmptMemoTimerRef = %d", CmptMemoTimerRef);
        }
#endif

        /*-------------------------------------------------------------------------*/

        // en attendant une methode propre de gestion de memoire
        // des samples !
        if (RestartRainSample) {
            WIIU_FIRST(wiiu_first_iter_done, "MainLoop: StartRainSample/LaunchAmbiance enter");
            StartRainSample();
            LaunchAmbiance();
            WIIU_FIRST(wiiu_first_iter_done, "MainLoop: StartRainSample/LaunchAmbiance exit");
        }

        /*-------------------------------------------------------------------------*/

        LastInput = Input;

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: MyGetInput enter");
        MyGetInput();
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: MyGetInput exit");
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: ManageTime enter");
        ManageTime();
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: ManageTime exit");

        if (DemoSlide) {
            if (MyKey
#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
                    AND MyKey != K_T
#endif
            ) {
                HQ_StopSample();
                SavingEnable = FALSE;
                return (MyKey != K_ESC);
            }

            /*                      if( HigTrack>1 AND GetMusicCD()==-1 )
                        {
                                S32     num = MyRnd(HigTrack-1)+1 ;

                                PlayCdTrack( num ) ;
                        }
*/
        }

        /*-------------------------------------------------------------------------*/
        /* gestion clavier */ /* tools */

        // Faut-il le laisser ? (non, à mon avis !)
        if (DemoSlide AND MyKey == K_T) {
            SetTimerHR(TimerRefHR + 200);
        }

#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
        if (MyKey == K_D) {
            AffDebugMenu();
        }

        if (MyKey == K_F) {
            DebugFps ^= 1;
            InitWaitNoKey();
        }
#endif

#ifdef DEBUG_TOOLS
        if (MyKey == K_T) {
            SetTimerHR(TimerRefHR + 200);
        }

        if (MyKey == K_Z) {
            DrawZVObjets ^= 1;
            InitWaitNoKey();
        }

        if (MyKey == K_E) {
            FlagDrawHorizon ^= 1;
            FirstTime = AFF_ALL_FLIP;
            while (MyKey)
                MyGetInput();
        }

        if (MyKey == K_F12) {
            AsciiMode ^= 1;
            while (MyKey)
                MyGetInput();
        }

        if (MyKey == K_F10) {
            if (CubeMode == CUBE_EXTERIEUR) {
                S32 n;
                S32 lasttimer = TimerRefHR;

                SaveTimer();

                CoulText(LBAWHITE, 0);

                for (n = 0; n < 20; n++) {
                    ManageTime();

                    if (FlagDrawHorizon) {
                        DrawHorizon2ZBuf();
                    }

                    // Doit Rajouter sa boite
                    AffichageTerrainZBuf();
                    GraphPrintf(FALSE, 0, 0, "%d Drawn", n);
                    BoxStaticFullflip();
                }

                GraphPrintf(TRUE, 0, 10, "Time for 20 Loops: %d", TimerRefHR - lasttimer);

                while (MyKey)
                    MyGetInput();
                while (!MyKey)
                    MyGetInput();

                RestoreTimer();
            } else {
                LogPrintf("F10 benchmark only works in exterior scenes\n");
            }
            while (MyKey)
                MyGetInput();
        }

        if (MyKey == K_F11) {
            if (CubeMode == CUBE_EXTERIEUR) {
                S32 n;
                S32 lasttimer = TimerRefHR;

                SaveTimer();

                CoulText(LBAWHITE, 0);

                for (n = 0; n < 20; n++) {
                    ManageTime();

                    AffScene(AFF_ALL_NO_FLIP);
                    GraphPrintf(FALSE, 0, 0, "%d Drawn", n);
                    BoxUpdate();
                }

                GraphPrintf(TRUE, 0, 10, "Time for 20 Loops: %d", TimerRefHR - lasttimer);

                while (MyKey)
                    MyGetInput();
                while (!MyKey)
                    MyGetInput();

                RestoreTimer();
            } else {
                LogPrintf("F11 benchmark only works in exterior scenes\n");
            }
            while (MyKey)
                MyGetInput();
        }

        if (MyKey == K_V) {
            if (MagicBall != -1) {
                ptrobj->Obj.X = ListExtra[MagicBall].PosX;
                ptrobj->Obj.Y = ListExtra[MagicBall].PosY;
                ptrobj->Obj.Z = ListExtra[MagicBall].PosZ;
            }
        }

        if (MyKey == K_M) {
            SaveTimer();

            AffPalette();

            FirstTime = AFF_ALL_FLIP;
            RestoreTimer();
        }

        if (MyKey == K_K) {
            ExtraBonus(ptrobj->Obj.X,
                       ptrobj->Obj.Y + 2000,
                       ptrobj->Obj.Z,
                       (256 * MUL_ANGLE), 0,
                       6, 1);
        }

        if (MyKey == K_A) {
            SaveTimer();
            if (!FlagFade)
                FadeToBlackAndSamples(PtrPal);

            FlagFade = TRUE;

            PlayAcf("bu");

            if (CubeMode == CUBE_INTERIEUR) {
                InitGrille(NumCube);
            }
            RazListPartFlow();
            ChoicePalette();

            FlagFade = TRUE;
            FirstTime = AFF_ALL_FLIP;
            RestoreTimer();
        }
#endif // debug tools

#ifdef ENABLE_POLY_RECORDING
        // Scene snapshot / polygon recording capture (Alt+F9)
        if (Key == K_F9 && (SDL_GetModState() & SDL_KMOD_ALT)) {
            SnapshotRequested = 1;
            while (Key)
                MyGetInput();
        }
#endif

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: CheckSavePcx enter");
        CheckSavePcx(); // F9: Sauve Screen
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: CheckSavePcx exit");

        /* Console screenshot (PNG): requested without overlay; perform here where we have Screen + PtrPal. */
        {
            char shotPath[ADELINE_MAX_PATH];
            int is_png = 0;
            if (Console_HasPendingScreenshot(shotPath, sizeof(shotPath), &is_png) && is_png) {
                SavePNG(shotPath, Screen, ModeDesiredX, ModeDesiredY, PtrPal);
                Console_ClearPendingScreenshot();
            }
        }

        /*-------------------------------------------------------------------------*/
        /* gestion clavier */ /* game */

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: !FlagFade input-handling block enter");
        if (!FlagFade) {
            if (Input & I_HELP) {
                SaveTimer();
                StopSpeak();
                BackupScreen(FALSE);
                CopyScreen(Log, Screen);
                BoxReset();
                InitDial(0);

                HQ_StopSample();
                RestartRainSample = TRUE;

                InitPlasmaMenu();
                //                      PauseMusic( TRUE ) ;

                MenuConfig();

                ClearPlasmaMenu();

                InitDial(START_FILE_ISLAND + Island);
                PaletteSync(PtrPal);
                RestoreScreen();
                //                      ResumeMusic( TRUE ) ;
                RestoreTimer();
            }

            if ((MyKey == K_ESC OR Input & I_MENUS)) {
                if (CinemaMode > 1) {
                    ResetCinemaMode();
                } else {
                    StopSpeak();
                    PauseMusic(FALSE);
                    EscTimer = TimerRefHR;

                    if (Comportement == C_PROTOPACK OR Comportement == C_JETPACK) {
                        // pour pouvoir restaurer anim marche protopack
                        if (ptrobj->GenAnim == GEN_ANIM_MARCHE) {
                            TimerProto = EscTimer + TEMPO_PROTO_AUTO; // 1/2 s
                        }
                    }

                    if ((ptrobj->LifePoint > 0)
                            AND(ptrobj->Obj.Body.Num != -1)
                                AND(!(ptrobj->Flags & INVISIBLE))
                                    AND(NumCube != NUM_CUBE_PHANTOM)
                                        AND !DemoSlide
                                            AND !PLAY_THE_END) {
                        SavingEnable = TRUE;
                    } else {
                        SavingEnable = FALSE;
                    }
                    break;
                }
            }

            if (Input & I_OPTIONS) // options menu
            {
                SaveTimer();
                MemoClipWindow();
                UnsetClipWindow();
                GameOptionMenu[5] = 15; // retour au jeu

                StopSpeak();
                InitDial(0); //      SYS

                InitPlasmaMenu();

                PauseMusic(FALSE);

                OptionsMenu(FALSE);

                GameOptionMenu[5] = 26; // retour au menu precedent

                InitDial(START_FILE_ISLAND + Island);

                ClearPlasmaMenu();
                //                        CopyScreen( ScreenAux, Screen ) ;
                //                      RestoreClipWindow() ;// fait dans OptionsMenu
                //                        BoxStaticFullflip() ;
                PaletteSync(PtrPal);

                ResumeMusic(FALSE);

                RestoreTimer();
            }

            if (Input & I_SAVE) // Sauvegarde
            {
#if !defined(DEMO) || defined(DEBUG_TOOLS)
                if ((ptrobj->LifePoint > 0)
                        AND(ptrobj->Obj.Body.Num != -1)
                            AND(!(ptrobj->Flags & INVISIBLE))
                                AND(NumCube != NUM_CUBE_PHANTOM)
                                    AND !PLAY_THE_END) {
                    S32 savenewcube = NewCube;

                    InitDial(0);

                    SaveTimer();
                    MemoClipWindow();
                    UnsetClipWindow();
                    StopSpeak();
                    BackupScreen(TRUE);
                    HQ_PauseSamples();
#ifdef DEMO
                    SetDemoSaveGame(CurrentDemoSave);
                    NumVersion = NUM_VERSION | SAVE_COMPRESS;
                    CopyScreen(Screen, Log);
                    SaveGame(TRUE);
#else
                    InitPlasmaMenu();
                    if (ChoosePlayerName(42, FALSE, TRUE)) {
                        NumVersion = NUM_VERSION | SAVE_COMPRESS;
                        CopyScreen(Screen, Log);
                        if (FileSize(OldGamePathname))
                            Delete(OldGamePathname);
                        SaveGame(TRUE);
                    }
                    ClearPlasmaMenu();
#endif
                    RestoreClipWindow();
                    RestoreScreen();
                    NewCube = savenewcube;
                    InitDial(START_FILE_ISLAND + Island);
                    HQ_ResumeSamples();
                    PaletteSync(PtrPal);
                    RestoreTimer();

                    while (MyKey OR Input)
                        MyGetInput();
                } else
                    PlayErrorSample();
#else // DEMO
                PlayErrorSample();
#endif
            }

#ifndef DEMO
            if (Input & I_LOAD) // Chargement
            {
                S32 savenewcube = NewCube;

                InitDial(0);

                SaveTimer();
                MemoClipWindow();
                UnsetClipWindow();
                StopSpeak();
                BackupScreen(TRUE);
                HQ_PauseSamples();
                InitPlasmaMenu();
                if (ChoosePlayerName(41, FALSE, FALSE)) {
                    RestoreTimer();

                    StopMusic();

                    InitGame(-1);
                    ChangeCube();

                    CopyScreen(Log, ScreenAux);

                    RestoreTimer();
                    SaveTimer();
                    AffScene(AFF_ALL_NO_FLIP);
                    FixeCinemaMode(FALSE);
                    CopyScreen(Log, Screen);

                    // au cas ou le mec meurre avant de faire <ESC>
                    CurrentSaveGame();

                    BoxReset(); // pas de flip

                    ZoomSavedGame(240, 10);

                    FlagFade = FALSE;
                    RestartRainSample = TRUE;
                    HQ_ResumeSamples();

                    ClearPlasmaMenu();

                    RestoreTimer();

                    goto restartloop;
                } else {
                    // on charge pas de nouvelle partie
                    NewCube = savenewcube;
                    ClearPlasmaMenu();
                    RestoreClipWindow();
                    RestoreScreen();
                    InitDial(START_FILE_ISLAND + Island);
                    HQ_ResumeSamples();
                    PaletteSync(PtrPal);
                    RestoreTimer();

                    while (MyKey OR Input)
                        MyGetInput();
                }
            }
#endif

#ifdef DEBUG_TOOLS
            if ((MyKey == K_G) // Sauvegarde d'un bug
                AND(ptrobj->LifePoint > 0)
                    AND(ptrobj->Obj.Body.Num != -1)
                        AND(!(ptrobj->Flags & INVISIBLE))
                            AND(NumCube != NUM_CUBE_PHANTOM)) {
                S32 savenewcube = NewCube;

                InitDial(0);

                SaveTimer();
                MemoClipWindow();
                UnsetClipWindow();
                StopSpeak();
                BackupScreen(TRUE);
                HQ_PauseSamples();
                InitPlasmaMenu();
                if (ChoosePlayerName(42 | 0x40000000, FALSE, TRUE)) {
                    NumVersion = NUM_VERSION | SAVE_COMPRESS;
                    CopyScreen(Screen, Log);
                    SaveGame(TRUE);
                }
                ClearPlasmaMenu();
                RestoreScreen();
                NewCube = savenewcube;
                InitDial(START_FILE_ISLAND + Island);
                RestoreClipWindow();
                HQ_ResumeSamples();
                PaletteSync(PtrPal);
                RestoreTimer();

                while (MyKey OR Input)
                    MyGetInput();
            }
#endif

            if (Input & I_PAUSE) {
                SaveTimer();

                if (Comportement == C_PROTOPACK OR Comportement == C_JETPACK) {
                    // pour pouvoir restaurer anim marche protopack
                    if (ptrobj->GenAnim == GEN_ANIM_MARCHE) {
                        TimerProto = TimerRefHR + TEMPO_PROTO_AUTO; // 1/2 s
                    }
                }

                StopSpeak();
                HQ_PauseSamples();
                PauseMusic(FALSE);

                GamePaused(PAUSE_MESSAGE);

                ResumeMusic(FALSE);
                HQ_ResumeSamples();
                RestoreTimer();
            }

            InventoryAction = -1;

            if ((Input & I_INVENTORY)
                    AND(PtrComportement->Flags & CF_INVENTORY)
                        AND(ptrobj->Obj.Body.Num != -1)
                            AND !(ptrobj->WorkFlags & SKATING)
                                AND(ptrobj->Move == MOVE_MANUAL)) {
                SaveTimer();
                StopSpeak();
                BackupScreen(TRUE);
                MenuInventory(0);

                switch (InventoryAction) {
                case FLAG_HOLOMAP: // holomap
                    if (PtrComportement->Flags & CF_HOLOMAP) {
                        HoloMap();
                    }
                    break;

                case FLAG_BALLE_MAGIQUE: // balle magique
                case FLAG_DART:          // Flechette
                case FLAG_SARBACANE:     // Sarbacane
                case FLAG_GANT:          // Gant (raquette)
                case FLAG_PISTOLASER:    // PistoLaser
                    Weapon = (U8)InventoryAction;
                    if (Comportement > C_DISCRET) {
                        SetComportement(C_NORMAL);
                    }
                    InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    break;

                case FLAG_SABRE: // SABRE MAGIQUE
                    Weapon = (U8)InventoryAction;
                    if (Comportement > C_DISCRET) {
                        SetComportement(C_NORMAL);
                    }

                    // anim degaine sabre
                    InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    InitAnim(GEN_ANIM_DEGAINE, ANIM_THEN, NUM_PERSO);
                    break;

                case FLAG_PROTOPACK: // protopack
                {
                    U8 comp;

                    if (TabInv[FLAG_PROTOPACK].IdObj3D == 1)
                        comp = C_JETPACK;
                    else
                        comp = C_PROTOPACK;

                    if (Comportement == comp)
                        SetComportement(C_NORMAL);
                    else
                        SetComportement(comp);
                } break;

                case FLAG_CONQUE: // Conque
                    if (Comportement == C_CONQUE)
                        SetComportement(C_NORMAL);
                    else
                        SetComportement(C_CONQUE);
                    break;

                case FLAG_MECA_PINGOUIN: // meca pingouin
                    ThrowMecaPingouin();
                    break;

                case FLAG_ARDOISE:
                    GereArdoise();
                    break;

                case FLAG_VISIONNEUSE:
                    PlayAllAcf();

                    if (CubeMode == CUBE_INTERIEUR) {
                        InitGrille(NumCube);
                    }

                    RazListPartFlow();
                    ChoicePalette();
                    FirstTime = AFF_ALL_FLIP;
                    break;

                case FLAG_ANNEAU_FOUDRE:
                    if (Comportement > C_DISCRET) {
                        SetComportement(C_NORMAL);
                    }

                    InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    InitAnim(GEN_ANIM_FOUDRE, ANIM_ALL_THEN, NUM_PERSO);
                    break;

                case FLAG_PROTECTION:
                    ToggleSortProtection();
                    break;

                case FLAG_NOTE_BOURGMESTRE:
                    if (FirstTime != AFF_ALL_FLIP)
                        RestoreScreen();

                    // init incdisp symbol utilisation
                    InitIncrustDisp(INCRUST_INVENTORY | INCRUST_YCLIP,
                                    FLAG_NOTE_BOURGMESTRE,
                                    0, 0,
                                    0, 0, 3);

                    InitDial(2); // FR_GAM.TXT
#ifdef CDROM
                    NumObjSpeak = NUM_PERSO;
#endif
                    NumObjDial = NUM_PERSO;
                    Dial(800, TRUE);
                    InitDial(START_FILE_ISLAND + Island);
                    break;
                }
                if (FirstTime != AFF_ALL_FLIP)
                    RestoreScreen();
                RestoreTimer();
            }

            if ((Input & I_COMPORTEMENT)
                    AND(ptrobj->Obj.Body.Num != -1)
                        AND !(ptrobj->WorkFlags & SKATING)
                            AND((ptrobj->Move == MOVE_MANUAL)
                                    OR(ptrobj->Move == MOVE_BUGGY_MANUAL))) {
                MenuComportement(FALSE);
            }

            // changement d'arme ou de comportement
            if (PtrComportement->Flags & CF_INVENTORY
                                             AND(ptrobj->Obj.Body.Num != -1)
                                                 AND !(ptrobj->WorkFlags & SKATING)
                                                     AND(ptrobj->Move == MOVE_MANUAL)) {
                if (Input & I_WEAPON_1) // Balle magique
                {
                    if (ListVarGame[FLAG_BALLE_MAGIQUE]) {
                        Weapon = FLAG_BALLE_MAGIQUE;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    }
                    InitWaitNoInput(I_WEAPON_1);
                }

                if (Input & I_WEAPON_2) // Fléchettes
                {
                    if (ListVarGame[FLAG_DART]) {
                        Weapon = FLAG_DART;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    }
                    InitWaitNoInput(I_WEAPON_2);
                }

                if (Input & I_WEAPON_3) // SARBACANE/SARBATRON
                {
                    if (ListVarGame[FLAG_SARBACANE]) {
                        Weapon = FLAG_SARBACANE;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    }
                    InitWaitNoInput(I_WEAPON_3);
                }

                if (Input & I_WEAPON_4) // CONQUE
                {
                    if (ListVarGame[FLAG_CONQUE]) {
                        if (Comportement == C_CONQUE)
                            SetComportement(C_NORMAL);
                        else
                            SetComportement(C_CONQUE);
                    }
                    InitWaitNoInput(I_WEAPON_4);
                }

                if (Input & I_WEAPON_5) // GANT
                {
                    if (ListVarGame[FLAG_GANT]) {
                        Weapon = FLAG_GANT;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    }
                    InitWaitNoInput(I_WEAPON_5);
                }

                if (Input & I_WEAPON_6) // PISTOLASER
                {
                    if (ListVarGame[FLAG_PISTOLASER] AND TabInv[FLAG_PISTOLASER].IdObj3D == 2) {
                        Weapon = FLAG_PISTOLASER;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                    }
                    InitWaitNoInput(I_WEAPON_6);
                }

                if (Input & I_WEAPON_7) // SABRE
                {
                    if (ListVarGame[FLAG_SABRE]) {
                        S32 oldweapon = Weapon;

                        Weapon = FLAG_SABRE;

                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }

                        // anim degaine sabre
                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);

                        if (oldweapon != FLAG_SABRE) {
                            InitAnim(GEN_ANIM_DEGAINE, ANIM_THEN, NUM_PERSO);
                        }
                    }
                    InitWaitNoInput(I_WEAPON_7);
                }

                if (CheckKey(DefKeys[33].Key1) // ProtoPack/SuperJetPack
                    OR CheckKey(DefKeys[33].Key2)) {
                    if (!WaitNoKey33
                            AND ListVarGame[FLAG_PROTOPACK]) {
                        U8 comp;

                        if (TabInv[FLAG_PROTOPACK].IdObj3D == 1)
                            comp = C_JETPACK;
                        else
                            comp = C_PROTOPACK;

                        if (Comportement == comp)
                            SetComportement(C_NORMAL);
                        else
                            SetComportement(comp);
                    }

                    WaitNoKey33 = TRUE;
                } else
                    WaitNoKey33 = FALSE;

                if (CheckKey(DefKeys[32].Key1) // NitroMecaPingouin
                    OR CheckKey(DefKeys[32].Key2)) {
                    if (!WaitNoKey32
                            AND ListVarGame[FLAG_MECA_PINGOUIN] > 0) {
                        SaveTimer();
                        InventoryAction = FLAG_MECA_PINGOUIN;
                        ThrowMecaPingouin();
                        RestoreTimer();
                    }

                    WaitNoKey32 = TRUE;
                } else
                    WaitNoKey32 = FALSE;

                if (CheckKey(DefKeys[34].Key1) // Sort de Protection
                    OR CheckKey(DefKeys[34].Key2)) {
                    if (!WaitNoKey34
                            AND ListVarGame[FLAG_PROTECTION] > 0 AND MagicPoint > 0) {
                        SaveTimer();
                        InventoryAction = FLAG_PROTECTION;
                        ToggleSortProtection();
                        RestoreTimer();
                    }

                    WaitNoKey34 = TRUE;
                } else
                    WaitNoKey34 = FALSE;

                if (CheckKey(DefKeys[35].Key1) // Sort de Foudre
                    OR CheckKey(DefKeys[35].Key2)) {
                    if (!WaitNoKey35
                            AND ListVarGame[FLAG_ANNEAU_FOUDRE] > 0 AND MagicPoint >= (MagicLevel * 20)) {
                        SaveTimer();
                        InventoryAction = FLAG_ANNEAU_FOUDRE;
                        if (Comportement > C_DISCRET) {
                            SetComportement(C_NORMAL);
                        }

                        InitBody(GEN_BODY_TUNIQUE, NUM_PERSO);
                        InitAnim(GEN_ANIM_FOUDRE, ANIM_ALL_THEN, NUM_PERSO);
                        RestoreTimer();
                    }

                    WaitNoKey35 = TRUE;
                } else
                    WaitNoKey35 = FALSE;
            }

            if ((Input & (I_NORMAL | I_SPORTIF | I_AGRESSIF | I_DISCRET))
                    AND(PtrComportement->Flags & CF_COMPORTEMENT)
                        AND(ptrobj->Obj.Body.Num != -1)
                            AND !(ptrobj->WorkFlags & SKATING)
                                AND(ptrobj->Move == MOVE_MANUAL)) {
                switch (Comportement) {
                case C_NORMAL:
                case C_SPORTIF:
                case C_AGRESSIF:
                case C_DISCRET:
                case C_PROTOPACK:
                case C_CONQUE:
                case C_JETPACK:
                    SaveTimer();

                    if (Input & I_NORMAL)
                        SetComportement(C_NORMAL);
                    else if (Input & I_SPORTIF)
                        SetComportement(C_SPORTIF);
                    else if (Input & I_AGRESSIF)
                        SetComportement(C_AGRESSIF);
                    else if (Input & I_DISCRET)
                        SetComportement(C_DISCRET);

                    IncrustComportement(Comportement);
                    RestoreTimer();
                    break;

                case C_SCAPH_INT_NORM:
                    if (Input & I_SPORTIF) {
                        SaveTimer();
                        SetComportement((U8)C_SCAPH_INT_SPOR);
                        IncrustComportement(Comportement);
                        RestoreTimer();
                    }
                    break;

                case C_SCAPH_INT_SPOR:
                    if (Input & I_NORMAL) {
                        SaveTimer();
                        SetComportement((U8)C_SCAPH_INT_NORM);
                        IncrustComportement(Comportement);
                        RestoreTimer();
                    }
                    break;

                case C_SCAPH_EXT_NORM:
                    if (Input & I_SPORTIF) {
                        SaveTimer();
                        SetComportement((U8)C_SCAPH_EXT_SPOR);
                        IncrustComportement(Comportement);
                        RestoreTimer();
                    }
                    break;

                case C_SCAPH_EXT_SPOR:
                    if (Input & I_NORMAL) {
                        SaveTimer();
                        SetComportement((U8)C_SCAPH_EXT_NORM);
                        IncrustComportement(Comportement);
                        RestoreTimer();
                    }
                    break;
                }
            }

            if ((Input & I_HOLOMAP)
                    AND(PtrComportement->Flags & (CF_HOLOMAP | CF_INVENTORY))
                        AND(ListVarGame[FLAG_HOLOMAP] == 1)
                            AND(ptrobj->Obj.Body.Num != -1)
                                AND(ptrobj->Move == MOVE_MANUAL OR ptrobj->Move == MOVE_BUGGY_MANUAL)
                                    AND !CinemaMode) {
                SaveTimer();
                StopSpeak();
                //                      PauseMusic( TRUE ) ;
                //			BackupScreen( FALSE ) ;
                InitWaitNoInput(I_HOLOMAP | I_MENUS | I_RETURN | I_ACTION);
                InitWaitNoKey();
                HoloMap();
                //                      ResumeMusic( TRUE ) ;
                //                        RestoreScreen() ;
                RestoreTimer();
            }

            if (Input & I_RETURN) /* recentre sur perso */
            {
                if (!CameraZone /* si pas camera forcée */
                        AND !CinemaMode) {
                    SaveTimer();

                    Rotate(0, DISTANCE_VISEE, ListObjet[NumObjFollow].Obj.Beta);

                    StartXCube = (ListObjet[NumObjFollow].Obj.X + X0) / SIZE_BRICK_XZ;
                    StartYCube = ((ListObjet[NumObjFollow].Obj.Y + SIZE_BRICK_Y) / SIZE_BRICK_Y);
                    StartZCube = (ListObjet[NumObjFollow].Obj.Z + Z0) / SIZE_BRICK_XZ;

                    CameraCenter(1);
                    FirstTime = AFF_ALL_FLIP;

                    /* Re-sync follow camera state so it continues
                     * smoothly from the reset position. */
                    if (FollowCamera) {
                        FollowCamBaseDist = FOLLOW_CAM_INITIAL_DIST;
                        FollowCamEffectiveDist = FollowCamBaseDist;
                        FollowCamTargetBeta = BetaCam;
                        FollowCamReengageDelay = 0;
                        FollowCamPrevBaseDist = FollowCamBaseDist;
                        FollowCamPrevAlphaCam = AlphaCam;
                    }

                    RestoreTimer();

#ifndef DEBUG_TOOL
                    // faut-il le laisser ?
                    InitWaitNoInput(I_RETURN);
#endif
                }
            }

#ifdef DEBUG_TOOLS
            if (MyKey == K_B) {
                if (Sample_Driver_Enabled) {
                    if ((SamplesEnable ^= 1) == 0) {
                        HQ_PauseSamples();
                        Message("Samples OFF", TRUE);
                    } else {
                        HQ_ResumeSamples();
                        Message("Samples ON", TRUE);
                    }
                }
            }
#endif

        } // if !flagfade
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: !FlagFade block exit");

        /*-------------------------------------------------------------------------*/

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GetDeltaMove(Falling/Shifting/SampleAlways) enter");
#ifdef LBA2_TARGET_WIIU
        if (!wiiu_first_iter_done)
            LogPrintf("[twinsen-trace pre-GetDeltaMove] Obj={%d,%d,%d}\n",
                      (int)ListObjet[0].Obj.X, (int)ListObjet[0].Obj.Y, (int)ListObjet[0].Obj.Z);
#endif
        /* vitesse de chute propor pour tous les objets */
        StepFalling = GetDeltaMove(&RealFalling);
        /* vitesse de decalage propor pour tous les objets */
        StepShifting = GetDeltaMove(&RealShifting);

        // doit-on repaner les samples always
        pansample = GetDeltaMove(&SampleAlwaysMove);
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GetDeltaMove exit");
#ifdef LBA2_TARGET_WIIU
        if (!wiiu_first_iter_done)
            LogPrintf("[twinsen-trace post-GetDeltaMove] Obj={%d,%d,%d}\n",
                      (int)ListObjet[0].Obj.X, (int)ListObjet[0].Obj.Y, (int)ListObjet[0].Obj.Z);
#endif

        /*-------------------------------------------------------------------------*/

        CameraZone = FALSE;

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereAmbiance enter");
        GereAmbiance();
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereAmbiance exit");
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: CheckNextMusic enter");
        CheckNextMusic();
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: CheckNextMusic exit");

        /*-------------------------------------------------------------------------*/
        /* gere les objets */

        WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: HitBy reset loop enter (NbObjets=%d)\n", NbObjets);
        // asm
        ptrobj = ListObjet;
        for (i = 0; i < NbObjets; i++, ptrobj++) {
            ptrobj->HitBy = 255;
        }
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: HitBy reset loop exit");

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereExtras enter");
        GereExtras();
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereExtras exit");
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: AnimAllFlow enter");
        AnimAllFlow(); // gere les flows de particules
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: AnimAllFlow exit");

        WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: pre-EXTERIEUR check, CubeMode=%d Island=%d\n", CubeMode, Island);
        if (CubeMode == CUBE_EXTERIEUR) {
            WIIU_FIRST(wiiu_first_iter_done, "MainLoop: in CUBE_EXTERIEUR branch");
            if (Island == 0 AND TEMPETE_ACTIVE AND RainEnable) {
                WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereRain enter");
                GereRain();
                WIIU_FIRST(wiiu_first_iter_done, "MainLoop: GereRain exit");
            }

            WIIU_FIRST(wiiu_first_iter_done, "MainLoop: DoAnimatedPolys enter");
            DoAnimatedPolys();
            WIIU_FIRST(wiiu_first_iter_done, "MainLoop: DoAnimatedPolys exit");
        }

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: object update loop enter");
        ptrobj = ListObjet;
        for (i = 0; i < NbObjets; i++, ptrobj++) {
            if (ptrobj->WorkFlags & OBJ_DEAD)
                continue;

#ifdef LBA2_TARGET_WIIU
            if (!wiiu_first_iter_done && i == NUM_PERSO) {
                LogPrintf("[trace] MainLoop: obj loop iter NUM_PERSO begin (LifePoint=%d Body=%d)\n",
                          ptrobj->LifePoint, ptrobj->Obj.Body.Num);
            }
#endif
            // test mort d'un objet

            if (ptrobj->LifePoint == 0) {
                switch (i) {
                case NUM_PERSO: // twinsen
                    if (ListVarGame[FLAG_CLOVER] > 0) {
                        UseOneClover();
                    } else {
                        InitAnim(GEN_ANIM_MORT, ANIM_FINAL, NUM_PERSO);
                        ptrobj->Move = NO_MOVE;
                    }
                    break;

                case NUM_PINGOUIN:
                    ExplodeMecaPingouin();
                    break;

                default:
                    // sample specifique mort ?
                    HQ_3D_MixSample(SAMPLE_DEAD, 0x1000, 2000, 1,
                                    ptrobj->Obj.X, ptrobj->Obj.Y, ptrobj->Obj.Z);

                    // 'explosion' de l'objet ?
                    if ((ptrobj->OptionFlags & EXTRA_MASK)
                            AND !(ptrobj->OptionFlags & EXTRA_GIVE_NOTHING)) {
                        GiveExtraBonus(ptrobj);
                    }
                }
            }

            // Save Beta pour les objets carrier
            // peut-etre pourrait-on éviter le test
            if (ptrobj->CarryBy == -1)
                ptrobj->OldBeta = ptrobj->Obj.Beta;

#ifdef LBA2_TARGET_WIIU
            if (!wiiu_first_iter_done && i == NUM_PERSO) {
                LogPrintf("[trace] MainLoop: NUM_PERSO DoDir enter\n");
                LogPrintf("[twinsen-trace pre-DoDir] Obj={%d,%d,%d}\n",
                          (int)ptrobj->Obj.X, (int)ptrobj->Obj.Y, (int)ptrobj->Obj.Z);
            }
#endif
            DoDir(i);
#ifdef LBA2_TARGET_WIIU
            if (!wiiu_first_iter_done && i == NUM_PERSO) {
                LogPrintf("[twinsen-trace post-DoDir] Obj={%d,%d,%d}\n",
                          (int)ptrobj->Obj.X, (int)ptrobj->Obj.Y, (int)ptrobj->Obj.Z);
                LogPrintf("[trace] MainLoop: NUM_PERSO DoDir exit\n");
            }
#endif

            ptrobj->OldPosX = ptrobj->Obj.X;
            ptrobj->OldPosY = ptrobj->Obj.Y;
            ptrobj->OldPosZ = ptrobj->Obj.Z;

            if (ptrobj->OffsetTrack != -1) {
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO DoTrack enter\n");
                }
#endif
                DoTrack(i);
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO DoTrack exit\n");
                }
#endif
            }

            if (!FirstLoop) {
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO PtrDoAnim enter\n");
                }
#endif
                PtrDoAnim(i);
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO PtrDoAnim exit\n");
                }
#endif
            }

            if (ptrobj->Flags & CHECK_ZONE) {
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO CheckZoneSce enter\n");
                }
#endif
                CheckZoneSce(ptrobj, i);
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO CheckZoneSce exit\n");
                }
#endif
            }

            // Samples Always
            if (pansample AND ptrobj->SampleAlways) {
                ParmSampleVolume = ptrobj->SampleVolume;

                if (!(ptrobj->SampleAlways & 0xFF000000)
                        OR !IsSamplePlaying(ptrobj->SampleAlways)) {
                    ptrobj->SampleAlways = HQ_3D_MixSample((ptrobj->SampleAlways >> 8) & 0xFFFF,
                                                           0x1000, 0, 0,
                                                           ptrobj->Obj.X, ptrobj->Obj.Y, ptrobj->Obj.Z);

                } else {
                    HQ_3D_ChangePanSample(ptrobj->SampleAlways,
                                          ptrobj->Obj.X, ptrobj->Obj.Y, ptrobj->Obj.Z);
                }
            }

            if (i == NUM_PERSO) {
                // Pour la prochaine boucle
                ptrobj->WorkFlags &= ~(FLY_JETPACK | CHECK_FALLING);

                WIIU_FIRST(wiiu_first_iter_done, "MainLoop: NUM_PERSO CheckDartCol enter");
                CheckDartCol(ptrobj);
                WIIU_FIRST(wiiu_first_iter_done, "MainLoop: NUM_PERSO CheckDartCol exit");

                if (ptrobj->Move == MOVE_BUGGY_MANUAL) {
                    // teste les zones giver pour le
                    // buggy au point devant le buggy
                    CheckBuggyZoneGiver(ptrobj);
                }

                // pour la demo
                if (DemoSlide)
                    ActionNormal = FALSE;
            }

            if (ptrobj->OffsetLife != -1) {
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[trace] MainLoop: NUM_PERSO DoLife enter\n");
                    LogPrintf("[twinsen-trace pre-DoLife] Obj={%d,%d,%d}\n",
                              (int)ptrobj->Obj.X, (int)ptrobj->Obj.Y, (int)ptrobj->Obj.Z);
                }
#endif
                DoLife(i);
#ifdef LBA2_TARGET_WIIU
                if (!wiiu_first_iter_done && i == NUM_PERSO) {
                    LogPrintf("[twinsen-trace post-DoLife] Obj={%d,%d,%d}\n",
                              (int)ptrobj->Obj.X, (int)ptrobj->Obj.Y, (int)ptrobj->Obj.Z);
                    LogPrintf("[trace] MainLoop: NUM_PERSO DoLife exit\n");
                }
#endif
            }

            if (FlagTheEnd != -1)
                return FlagTheEnd; // mmm violent

            // si aprés la vie on à toujours 0 point de vie
            // destruction definitive
            if (ptrobj->LifePoint <= 0) {
                if (i == NUM_PERSO) {
                    if (ptrobj->WorkFlags & ANIM_END) {
                        if (ListVarGame[FLAG_CLOVER] > 0) {
                            UseOneClover();
                            goto startloop;
                        } else // game over
                        {
                            if (FirstTime == AFF_ALL_FLIP) {
                                AffScene(AFF_ALL_FLIP);
                            }

                            GameOver();
                            return 2;
                        }
                    }
                } else // autre obj
                {
                    CheckCarrier(i);
                    ptrobj->WorkFlags |= OBJ_DEAD;
                    ptrobj->Obj.Body.Num = -1;
                    ptrobj->ZoneSce = -1;
                }
            }

            if (NewCube != -1) {
                goto startloop;
            }
        }
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: object update loop exit");
#ifdef LBA2_TARGET_WIIU
        if (!wiiu_first_iter_done)
            LogPrintf("[twinsen-trace post-objLoop] Obj={%d,%d,%d}\n",
                      (int)ListObjet[0].Obj.X, (int)ListObjet[0].Obj.Y, (int)ListObjet[0].Obj.Z);
#endif

        /*-------------------------------------------------------------------------*/
        /* recentre sur hero (numobjfollow) */

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: camera/follow block enter");
        if (!CameraZone) {
            if (!CinemaMode) {
                // AlphaCam (touches '+' et '-' *)
                GereExtKeys(MyKey);

                /* Any AFF_ALL_FLIP forced by GereExtKeys (e.g. elevation +/-)
                 * while the follow camera is active needs timer compensation
                 * so the vsync lock doesn't bleed into the game clock. */
                if (FollowCamera AND CubeMode == CUBE_EXTERIEUR AND FirstTime == AFF_ALL_FLIP)
                    FollowCamTerrainFrame = TRUE;

                if (Input & I_CAMERA) {
                    SaveTimer();
                    AddBetaCam = (AddBetaCam + 1024) & 4095;
                    /* FollowCamera: do not use CameraCenter(2) — it resets
                     * AlphaCam/VueDistance to VueCamera presets.  Recompute
                     * target offset and BetaCam like case 2, then apply only. */
                    if (FollowCamera AND CubeMode == CUBE_EXTERIEUR) {
                        T_OBJ_3D *ptr3d_cc = &ListObjet[NumObjFollow].Obj;
                        S32 newbeta_cc = AddBetaCam + ptr3d_cc->Beta;
                        newbeta_cc &= 4095;
                        Rotate(0, DISTANCE_VISEE, newbeta_cc);
                        VueOffsetX = ptr3d_cc->X + X0;
                        VueOffsetY = ptr3d_cc->Y;
                        VueOffsetZ = ptr3d_cc->Z + Z0;
                        StartXCube = VueOffsetX / SIZE_BRICK_XZ;
                        StartYCube = VueOffsetY / SIZE_BRICK_Y;
                        StartZCube = VueOffsetZ / SIZE_BRICK_XZ;
                        BetaCam = (2048 - newbeta_cc) & 4095;
                        FollowCamTargetBeta = BetaCam;
                        CameraCenter(3);
                    } else {
                        CameraCenter(2);
                    }
                    FirstTime = AFF_ALL_FLIP;
                    InitWaitNoInput(I_CAMERA);
                    RestoreTimer();
                }

                if (FollowCamera AND CubeMode == CUBE_EXTERIEUR) {
                    T_OBJ_3D *ptr3d = &ListObjet[NumObjFollow].Obj;
                    S32 manualPan = CheckKey(K_LBRACKET) OR CheckKey(K_RBRACKET);
                    S32 heroMoving = (ptr3d->X != FollowCamPrevHeroX OR ptr3d->Z != FollowCamPrevHeroZ);

                    /* First-frame init: sync to current camera state.
                     * FollowCamTargetBeta defaults to 0 but BetaCam starts ~2048
                     * after CameraCenter(1), causing a bogus initial spin. */
                    if (FollowCamBaseDist == 0)
                        FollowCamBaseDist = FOLLOW_CAM_INITIAL_DIST;
                    if (FollowCamEffectiveDist == 0) {
                        FollowCamEffectiveDist = FollowCamBaseDist;
                        FollowCamTargetBeta = BetaCam;
                        FollowCamPrevBaseDist = FollowCamBaseDist;
                        FollowCamPrevAlphaCam = AlphaCam;
                    }

                    /* --- AddBetaCam drift ---
                     * Only drift pan back to centre while the hero is walking.
                     * While standing still, [ / ] pan is preserved. */
                    if (manualPan) {
                        FollowCamReengageDelay = FOLLOW_CAM_REENGAGE_FRAMES;
                    } else if (heroMoving) {
                        if (FollowCamReengageDelay > 0) {
                            FollowCamReengageDelay--;
                        } else if (AddBetaCam != 0) {
                            S32 drift = AddBetaCam;
                            if (drift > 2048)
                                drift -= 4096;
                            S32 step = drift / FOLLOW_CAM_DRIFT_DIVISOR;
                            if (step > 0 AND step < FOLLOW_CAM_DRIFT_MIN_STEP)
                                step = FOLLOW_CAM_DRIFT_MIN_STEP;
                            if (step<0 AND step> - FOLLOW_CAM_DRIFT_MIN_STEP)
                                step = -FOLLOW_CAM_DRIFT_MIN_STEP;
                            if (drift > FOLLOW_CAM_DRIFT_THRESHOLD OR drift < -FOLLOW_CAM_DRIFT_THRESHOLD)
                                AddBetaCam = (AddBetaCam - step) & 4095;
                            else
                                AddBetaCam = 0;
                        }
                    }

                    /* --- Dirty check ---
                     * Triggers on position, facing, pan, or pending BetaCam
                     * catchup.  Hero Beta IS included: the camera always lazily
                     * orbits behind the hero whether running, turning in place,
                     * or standing.  Smoothness comes from the lerp speed, not
                     * from skipping updates. */
                    S32 catchupDiff = FollowCamTargetBeta - BetaCam;
                    if (catchupDiff > 2048)
                        catchupDiff -= 4096;
                    if (catchupDiff < -2048)
                        catchupDiff += 4096;

                    if (ptr3d->X != FollowCamPrevHeroX OR ptr3d->Y != FollowCamPrevHeroY OR ptr3d->Z != FollowCamPrevHeroZ OR ptr3d->Beta != FollowCamPrevHeroBeta OR AddBetaCam != FollowCamPrevAddBeta OR(catchupDiff > FOLLOW_CAM_ROT_THRESHOLD OR catchupDiff < -FOLLOW_CAM_ROT_THRESHOLD) OR FollowCamBaseDist != FollowCamPrevBaseDist OR AlphaCam != FollowCamPrevAlphaCam) {
                        S32 newbeta = AddBetaCam + ptr3d->Beta;
                        newbeta &= 4095;

                        /* Use the camera's current angle for the forward lean,
                         * not the hero's facing.  This keeps the orbit center
                         * stable when the hero turns in place — the lean point
                         * drifts smoothly as BetaCam lerps toward the target,
                         * instead of snapping with the hero's Beta. */
                        S32 leanBeta = (2048 - BetaCam) & 4095;
                        Rotate(0, DISTANCE_VISEE, leanBeta);

                        VueOffsetX = ptr3d->X + X0;
                        VueOffsetY = ptr3d->Y;
                        VueOffsetZ = ptr3d->Z + Z0;

                        StartXCube = VueOffsetX / SIZE_BRICK_XZ;
                        StartYCube = VueOffsetY / SIZE_BRICK_Y;
                        StartZCube = VueOffsetZ / SIZE_BRICK_XZ;

                        /* Rotation lag with distance-based inertia.
                         * The camera always tracks "behind the hero".  The lerp
                         * divisor scales with the spring arm length: closer camera
                         * (arm shortened near terrain) → tighter follow; full
                         * distance → lazy, cinematic drift.  Floor at divisor/3
                         * to avoid jitter when very close. */
                        FollowCamTargetBeta = (2048 - newbeta) & 4095;

                        if (manualPan) {
                            BetaCam = FollowCamTargetBeta;
                        } else if (FollowCamReengageDelay == 0) {
                            S32 diff = FollowCamTargetBeta - BetaCam;
                            if (diff > 2048)
                                diff -= 4096;
                            if (diff < -2048)
                                diff += 4096;
                            if (diff > FOLLOW_CAM_ROT_THRESHOLD OR diff < -FOLLOW_CAM_ROT_THRESHOLD) {
                                S32 dynamicDiv = FOLLOW_CAM_ROT_DIVISOR * FollowCamEffectiveDist / FollowCamBaseDist;
                                if (dynamicDiv < FOLLOW_CAM_ROT_DIVISOR / 3)
                                    dynamicDiv = FOLLOW_CAM_ROT_DIVISOR / 3;
                                S32 step = diff / dynamicDiv;
                                if (step > 0 AND step < FOLLOW_CAM_ROT_MIN_STEP)
                                    step = FOLLOW_CAM_ROT_MIN_STEP;
                                if (step<0 AND step> - FOLLOW_CAM_ROT_MIN_STEP)
                                    step = -FOLLOW_CAM_ROT_MIN_STEP;
                                BetaCam = (BetaCam + step) & 4095;
                            } else {
                                BetaCam = FollowCamTargetBeta;
                            }
                        }

                        /* --- Zoom distance: converge FollowCamEffectiveDist toward
                         * FollowCamBaseDist (numpad * /).  VueDistance must match
                         * FollowCamEffectiveDist before CameraCenter(3) so terrain
                         * render (AffGrilleExt) stays consistent. */
                        if (!CameraZone) {
                            if (FollowCamEffectiveDist < FollowCamBaseDist) {
                                FollowCamEffectiveDist += FOLLOW_CAM_SPRING_RECOVER;
                                if (FollowCamEffectiveDist > FollowCamBaseDist)
                                    FollowCamEffectiveDist = FollowCamBaseDist;
                            } else if (FollowCamEffectiveDist > FollowCamBaseDist) {
                                FollowCamEffectiveDist -= FOLLOW_CAM_SPRING_RECOVER;
                                if (FollowCamEffectiveDist < FollowCamBaseDist)
                                    FollowCamEffectiveDist = FollowCamBaseDist;
                            }

                            VueDistance = FollowCamEffectiveDist;
                            CameraCenter(3);
                        } else {
                            VueDistance = FollowCamEffectiveDist;
                            CameraCenter(3);
                        }

                        FirstTime = AFF_ALL_FLIP;
                        FollowCamTerrainFrame = TRUE;

                        FollowCamPrevHeroX = ptr3d->X;
                        FollowCamPrevHeroY = ptr3d->Y;
                        FollowCamPrevHeroZ = ptr3d->Z;
                        FollowCamPrevHeroBeta = ptr3d->Beta;
                        FollowCamPrevAddBeta = AddBetaCam;
                        FollowCamPrevBaseDist = FollowCamBaseDist;
                        FollowCamPrevAlphaCam = AlphaCam;
                    }
                }
            }

            if (FirstTime == AFF_OBJETS_FLIP) {
                T_OBJET *ptrobj;

                ptrobj = &ListObjet[NumObjFollow];

                PtrProjectPoint(ptrobj->Obj.X,
                                ptrobj->Obj.Y,
                                ptrobj->Obj.Z);

                /* FollowCamera (exterior): the follow camera tracks the hero
                 * every frame — the classic off-screen snap (CameraCenter(0))
                 * would fight it and cause jarring resets near trees / decors. */
                if ((Xp < 80 OR Xp > 539 OR Yp<(80 + ClipWindowYMin) OR Yp>(ClipWindowYMax - 50) /*429*/)
                        AND !(FollowCamera AND CubeMode == CUBE_EXTERIEUR)) {
                    SaveTimer();

                    if (CubeMode == CUBE_INTERIEUR) {
                        xm = (ptrobj->Obj.X) / SIZE_BRICK_XZ;
                        zm = (ptrobj->Obj.Z) / SIZE_BRICK_XZ;

                        StartXCube = xm + ((xm - StartXCube)) / 2;
                        StartZCube = zm + ((zm - StartZCube)) / 2;
                    } else {
                        Rotate(0, DISTANCE_VISEE, ptrobj->Obj.Beta);
                        xm = (ptrobj->Obj.X + X0) / SIZE_BRICK_XZ;
                        zm = (ptrobj->Obj.Z + Z0) / SIZE_BRICK_XZ;

                        StartXCube = xm;
                        StartZCube = zm;
                    }

                    ym = ptrobj->Obj.Y / SIZE_BRICK_Y;
                    StartYCube = ym;

                    if (StartXCube >= SIZE_CUBE_X)
                        StartXCube = SIZE_CUBE_X - 1;
                    if (StartZCube >= SIZE_CUBE_Z)
                        StartZCube = SIZE_CUBE_Z - 1;

                    if (FlagCameraForcee) {
                        FlagCameraForcee = FALSE;
                        AlphaCam = DefAlphaCam[VueCamera];
                        GammaCam = DEFAULT_GAMMA_CAM;
                        VueDistance = DefVueDistance[VueCamera];
                    }

                    CameraCenter(0);
                    FirstTime = AFF_ALL_FLIP;

                    RestoreTimer();
                }
            }
        }

        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: camera/follow block exit");
#ifdef LBA2_TARGET_WIIU
        if (!wiiu_first_iter_done)
            LogPrintf("[twinsen-trace post-camFollow] Obj={%d,%d,%d}\n",
                      (int)ListObjet[0].Obj.X, (int)ListObjet[0].Obj.Y, (int)ListObjet[0].Obj.Z);
#endif

        /*-------------------------------------------------------------------------*/
        /* affiche tout */

        /* For FollowCamera AFF_ALL_FLIP frames: save the timer before the
         * render+vsync and restore it after, then add back the real wall-clock
         * time elapsed (TimerSystemHR is updated by ManageTime inside
         * RestoreTimer).  This makes the full render+vsync transparent to the
         * game clock so hero, NPCs, and rain run at correct speed. */
        U32 FollowCamTimerBefore = 0;
        if (FollowCamTerrainFrame) {
            SaveTimer();
            FollowCamTimerBefore = TimerSystemHR;
        }

        WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: AffScene(%d) enter\n", FirstTime);
#ifdef LBA2_TARGET_N64
        if (N64DebugFullRedraw AND CubeMode == CUBE_EXTERIEUR AND FirstTime == AFF_OBJETS_FLIP)
            FirstTime = AFF_ALL_FLIP;
#endif
#ifdef LBA2_TARGET_WIIU
        const OSTime wiiuProfAffBegin = OSGetSystemTime();
#endif
        AffScene(FirstTime);
#ifdef LBA2_TARGET_WIIU
        WiiUProf_FrameSample(wiiuProfAffBegin, OSGetSystemTime());
#endif
        WIIU_FIRST(wiiu_first_iter_done, "MainLoop: AffScene exit");

        if (FollowCamTerrainFrame) {
            RestoreTimer();
            TimerRefHR += TimerSystemHR - FollowCamTimerBefore;
            FollowCamTerrainFrame = FALSE;
        }

        if ((FirstLoop OR RestartMusic)
                AND !PLAY_THE_END) {
            // on attend une boucle scenarique au cas ou le
            // scenar aurait lancé une musique
            if (CubeJingle != 255) {
                WIIU_FIRST_FMT(wiiu_first_iter_done, "[trace] MainLoop: PlayMusic(CubeJingle=%d) enter\n", CubeJingle);
                PlayMusic(CubeJingle, FALSE);
                WIIU_FIRST(wiiu_first_iter_done, "MainLoop: PlayMusic exit");
            }

            RestartMusic = FALSE;
        }

        FirstTime = AFF_OBJETS_FLIP;
        FirstLoop = FALSE;

        CmptFrame++;

#ifdef DEBUG_TOOLS
        if (NbFramePerSecond < MinNbf)
            MinNbf = NbFramePerSecond;
        if (NbFramePerSecond > MaxNbf)
            MaxNbf = NbFramePerSecond;

        TotalNbf += NbFramePerSecond;
        NbNbf++;
#endif
#ifdef LBA2_TARGET_WIIU
        if (!wiiu_first_iter_done) {
            LogPrintf("[trace] MainLoop: while(TRUE) FIRST iter complete (CmptFrame=%d) — silencing further per-iter trace\n", CmptFrame);
            wiiu_first_iter_done = 1;
        }
        // Heartbeat: every 60 iterations after the first, print a single line
        // so we can tell apart "engine still cycling MainLoop" from "engine
        // hung silently after AffScene".
        {
            static U32 wiiu_heartbeat_n = 0;
            if (wiiu_first_iter_done && ((wiiu_heartbeat_n++ % 60) == 0)) {
                // Sample bytes from Log + Phys to see if rendering pipeline
                // actually populates them. All-zero means AffScene/BoxBlit
                // are no-ops or write to wrong buffer.
                extern void *Log;
                extern void *Phys;
                U8 *l = (U8 *)Log;
                U8 *p = (U8 *)Phys;
                U32 nonzero_log = 0, nonzero_phys = 0;
                if (l && p) {
                    for (U32 i = 0; i < 4096; i += 16) {
                        if (l[i]) nonzero_log++;
                        if (p[i]) nonzero_phys++;
                    }
                }
                LogPrintf("[heartbeat] iter n=%u CmptFrame=%d NewCube=%d NumCube=%d Log_nonzero/256=%u Phys_nonzero/256=%u\n",
                          (unsigned)wiiu_heartbeat_n, (int)CmptFrame, (int)NewCube, (int)NumCube,
                          (unsigned)nonzero_log, (unsigned)nonzero_phys);
            }
        }
#endif
    }

    return 0;
}

/*══════════════════════════════════════════════════════════════════════════*
                      █▀▀▀▀ █▀▀▀█ ██▄ █ █▀▀▀▀  █    █▀▀▀▀
                      ██    ██  █ ██▀██ ██▀▀   ██   ██ ▀█
                      ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀     ▀▀   ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

#ifdef LBA2_TARGET_N64
/*──────────────────────────────────────────────────────────────────────────*/
// WriteConfigFile() only runs at program exit, which never happens on N64
// (the console is switched off), so the options menu persists its settings
// itself: OptionsMenu() calls N64SaveOptionsConfig() on the way out, which
// rewrites only the keys that differ from the last saved state. Every
// DefFileBufferWriteString() rewrites the whole cfg = one SRAM commit (a 32 KB
// PI transfer, a few ms), hence the diff. It works in a private heap buffer:
// ScreenAux, WriteConfigFile()'s buffer, holds the in-game menu backdrop here.

// Same order as N64GetOptions() below.
static const char *const N64OptionKeys[] = {
    "WaveVolume",
    "VoiceVolume",
    "MusicVolume",
    "CDVolume",
    "MasterVolume",
    "Language",
    "LanguageCD",
    "AllCameras",
    "FollowCamera",
    "ReverseStereo",
    "DetailLevel",
    "FullScreen",
    "DisplayFullScreen",
    "FlagDisplayText",
    "N64StickMove",
};
#define N64_OPTION_COUNT (S32)(sizeof(N64OptionKeys) / sizeof(N64OptionKeys[0]))

static S32 N64SavedOptions[N64_OPTION_COUNT];

static void N64GetOptions(S32 *v) {
    S32 n = 0;
    v[n++] = SampleVolume;
    v[n++] = VoiceVolume;
    v[n++] = (S32)JingleVolume;
    v[n++] = (S32)CDVolume;
    v[n++] = MasterVolume;
    v[n++] = Language;
    v[n++] = LanguageCD;
    v[n++] = AllCameras;
    v[n++] = FollowCamera;
    v[n++] = ReverseStereo;
    v[n++] = DetailLevel;
    v[n++] = VideoFullScreen;
    v[n++] = DisplayFullScreen;
    v[n++] = FlagDisplayText;
    v[n++] = N64StickMove;
}

void N64SaveOptionsConfig(void) {
    S32 cur[N64_OPTION_COUNT];
    S32 n, changed = 0;

    N64GetOptions(cur);
    for (n = 0; n < N64_OPTION_COUNT; n++) {
        if (cur[n] != N64SavedOptions[n])
            changed++;
    }
    if (!changed)
        return;

    S32 size = FileSize(PathConfigFile);
    if (!size)
        return;

    size += 1024; // room for keys the cfg doesn't have yet
    void *buf = malloc(size);
    if (!buf) {
        LogPuts("N64SaveOptionsConfig: out of memory, options not saved");
        return;
    }

    if (DefFileBufferInit(PathConfigFile, buf, size)) {
        for (n = 0; n < N64_OPTION_COUNT; n++) {
            if (cur[n] == N64SavedOptions[n])
                continue;

            const char *key = N64OptionKeys[n];
            if (!strcmp(key, "Language") OR !strcmp(key, "LanguageCD")) {
                DefFileBufferWriteString(key, GetLanguageName(cur[n]));
            } else if (!strcmp(key, "FlagDisplayText")) {
                DefFileBufferWriteString(key, cur[n] ? "ON" : "OFF");
            } else {
                DefFileBufferWriteValue(key, cur[n]);
            }
            N64SavedOptions[n] = cur[n];
        }
        LogPrintf("N64SaveOptionsConfig: %d option(s) saved\n", changed);
    }
    free(buf);
}
#endif

void ReadConfigFile(void) {
    const char *ptr;

    ReadInputConfig();    // Lit Config Clavier dans LBA2.CFG
    ReadGamepadConfig();  // Lit Config Gamepad dans LBA2.CFG
    ReadVolumeSettings(); // Lit Volumes dans LBA2.CFG

#ifdef LBA2_TARGET_N64
    // "ScreenMargin: N" (percent per side, 0..15) shrinks the VI output window
    // so CRT overscan doesn't eat the dialogue text; missing key = 6.
    N64SetScreenMargin(DefFileBufferReadValueDefault("ScreenMargin", 6));
    N64DebugStartCube = DefFileBufferReadValueDefault("DebugStartCube", -1);
    N64DebugSaveTest = DefFileBufferReadValueDefault("DebugSaveTest", 0);
    N64DebugFullRedraw = DefFileBufferReadValueDefault("DebugFullRedraw", 0);
    N64DebugVoiceWait = DefFileBufferReadValueDefault("DebugVoiceWait", 0);
    {
        extern U8 N64TerrainLod; // 3DEXT/TERRAIN.CPP
        N64TerrainLod = (U8)DefFileBufferReadValueDefault("TerrainLod", 0);
    }
#endif

    Version_US = DefFileBufferReadValue("Version_US");
    NumVersion = (U8)DefFileBufferReadValue("CompressSave");

    if (NumVersion)
        NumVersion = SAVE_COMPRESS;

    NumVersion |= NUM_VERSION;

    // LastSave
    ptr = DefFileBufferReadString("LastSave");
    if (ptr AND strcmp(ptr, ""))
        strcpy(PlayerName, ptr);

    Shadow = DefFileBufferReadValueDefault("Shadow", 3);
    AllCameras = DefFileBufferReadValueDefault("AllCameras", TRUE);
    /* User-facing name: "Auto camera" (menu/docs); config key FollowCamera; legacy AutoCameraCenter */
    FollowCamera = DefFileBufferReadValueDefault("FollowCamera",
                                                 DefFileBufferReadValueDefault("AutoCameraCenter", FALSE));

    if (FollowCamera < 0 OR FollowCamera > 1)
        FollowCamera = FALSE;

    ReverseStereo = DefFileBufferReadValueDefault("ReverseStereo", FALSE);

    InverseStereoSample(ReverseStereo);

    DetailLevel = DefFileBufferReadValueDefault("DetailLevel", MAX_DETAIL_LEVEL);

    if (DetailLevel < 0)
        DetailLevel = 0;
    if (DetailLevel > MAX_DETAIL_LEVEL)
        DetailLevel = MAX_DETAIL_LEVEL;

    VideoFullScreen = DefFileBufferReadValueDefault("FullScreen", TRUE);

    if (VideoFullScreen < 0 OR VideoFullScreen > 1)
        VideoFullScreen = TRUE;

    DisplayFullScreen = DefFileBufferReadValueDefault("DisplayFullScreen", FALSE);

    if (DisplayFullScreen < 0 OR DisplayFullScreen > 1)
        DisplayFullScreen = FALSE;

    SetWindowFullscreen(DisplayFullScreen != FALSE);

    /* Console toggle scancode (SDL scancode integer); invalid values fall back to F12 in console module. */
    Console_SetToggleScancode(DefFileBufferReadValueDefault("ConsoleToggleKey", K_F12));

    ptr = DefFileBufferReadStringDefault("FlagDisplayText", "ON");

    if (ptr AND !strcasecmp(ptr, "OFF"))
        FlagDisplayText = 0;
    else
        FlagDisplayText = 1; // default

    // version distributeur
    DistribVersion = DefFileBufferReadValueDefault("Version", UNKNOWN_VERSION);

    FlagMenuMouse = DefFileBufferReadValueDefault("MenuMouse", TRUE);
    if (FlagMenuMouse != 0)
        FlagMenuMouse = TRUE;
    else
        FlagMenuMouse = FALSE;

#ifdef LBA2_TARGET_N64
    N64GetOptions(N64SavedOptions); // baseline for N64SaveOptionsConfig()
#endif
}

/*──────────────────────────────────────────────────────────────────────────*/
void WriteConfigFile(void) {
    DefFileBufferInit(PathConfigFile, ScreenAux, (640 * 480 + RECOVER_AREA));

    WriteInputConfig();    // Sauve Config Clavier dans LBA2.CFG
    WriteGamepadConfig();  // Sauve Config Gamepad dans LBA2.CFG
    WriteVolumeSettings(); // Sauve volumes dans LBA2.CFG

    DefFileBufferWriteString("Language", GetLanguageName(Language));
#ifdef CDROM
    DefFileBufferWriteString("LanguageCD", GetLanguageName(LanguageCD));
#endif

    DefFileBufferWriteString("LastSave", PlayerName);

    DefFileBufferWriteValue("Shadow", Shadow);
    DefFileBufferWriteValue("AllCameras", AllCameras);
    DefFileBufferWriteValue("FollowCamera", FollowCamera);
    DefFileBufferWriteValue("ReverseStereo", ReverseStereo);
    DefFileBufferWriteValue("DetailLevel", DetailLevel);
    DefFileBufferWriteValue("FullScreen", VideoFullScreen);
    DefFileBufferWriteValue("DisplayFullScreen", DisplayFullScreen);
    DefFileBufferWriteValue("ConsoleToggleKey", Console_GetToggleScancode());

    if (FlagDisplayText)
        DefFileBufferWriteString("FlagDisplayText", "ON");
    else
        DefFileBufferWriteString("FlagDisplayText", "OFF");

    DefFileBufferWriteValue("MenuMouse", FlagMenuMouse);
}

/*══════════════════════════════════════════════════════════════════════════*
                            █▄ ▄█ █▀▀▀█  █    ██▄ █
                            ██▀ █ ██▀▀█  ██   ██▀██
                            ▀▀  ▀ ▀▀  ▀  ▀▀   ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

static S32 End_Num = -1;
static const char *End_Error;

/*══════════════════════════════════════════════════════════════════════════*/

// Erreur dans le HQRGet() qui appel cette fonction: on sort violemment

HQR_GET_CALLBACK ErrorHQRGet;

#ifdef LBA2_TARGET_WIIU
extern "C" void WiiU_AnimsCanary(const char *where) {
    static unsigned char s_last = 0x2F; // '/'
    static int s_reported = 0;
    if (s_reported || !HQR_Anims) return;
    unsigned char cur = ((unsigned char *)HQR_Anims->Name)[0];
    if (cur != s_last) {
        LogPrintf("[CANARY] HQR_Anims->Name[0] CHANGED at \"%s\" : was=0x%02X now=0x%02X (HQR_Anims=%p)\n",
                  where, s_last, cur, (void *)HQR_Anims);
        s_reported = 1;
    }
}

// Track ListObjet[1].PtrFile3D across ChangeCube to find where it gets clobbered
// to 0x2c010000 between LoadScene exit and SearchBody numobj=1.
extern "C" void WiiU_PtrFile3DCanary(const char *where) {
    static void *s_last = (void *)~(uintptr_t)0;  // sentinel: first call always logs
    void *cur = (void *)ListObjet[1].PtrFile3D;
    if (cur != s_last) {
        LogPrintf("[PTRCANARY] ListObjet[1].PtrFile3D at \"%s\" : was=%p now=%p\n",
                  where, s_last, cur);
        s_last = cur;
    }
}
#endif

void ErrorHQRGet(char *name, S32 index) {
    char errstring[256];

#ifdef LBA2_TARGET_WIIU
    LogPrintf("[ErrorHQRGet] name_ptr=%p index=%d\n", (void *)name, index);
    if (name) {
        unsigned char hex[16];
        for (int k = 0; k < 16; k++) hex[k] = (unsigned char)name[k];
        LogPrintf("[ErrorHQRGet] name[0..15] hex = %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  hex[0], hex[1], hex[2], hex[3], hex[4], hex[5], hex[6], hex[7],
                  hex[8], hex[9], hex[10], hex[11], hex[12], hex[13], hex[14], hex[15]);
        // Reconstruct alleged T_HQR_HEADER pointer (Name field is at offset 20).
        // Dump the numeric fields so we can tell whether MaxSize/FreeSize/Buffer
        // are sane or completely corrupt.
        T_HQR_HEADER *hdr = (T_HQR_HEADER *)((U8 *)name - 20);
        LogPrintf("[ErrorHQRGet] reconstructed header=%p MaxSize=%d FreeSize=%d MaxIndex=%d NbIndex=%d Buffer=%p\n",
                  (void *)hdr, (int)hdr->MaxSize, (int)hdr->FreeSize,
                  (int)hdr->MaxIndex, (int)hdr->NbIndex, hdr->Buffer);
    }
    LogPrintf("[ErrorHQRGet] HQR_Anims=%p HQR_Bodys=%p HQR_ObjFix=%p HQR_Samples=%p HQRPtrSprite=%p HQRPtrSpriteRaw=%p HQRPtrAnim3DS=%p\n",
              (void *)HQR_Anims, (void *)HQR_Bodys, (void *)HQR_ObjFix,
              (void *)HQR_Samples, (void *)HQRPtrSprite, (void *)HQRPtrSpriteRaw,
              (void *)HQRPtrAnim3DS);
#endif

    snprintf(errstring, sizeof(errstring), "HQR_Get(%s): buffer trop petit (index %d)", name, index);

    TheEnd(NOT_ENOUGH_MEM, errstring);
}

/*══════════════════════════════════════════════════════════════════════════*/

void InitProgram() {
    BoxInit(MAX_BOXES);

    Cls();
    BoxUpdate();

    MemoClipWindow(); // pour caler MemoClipWindow... sur ClipWindow...

#ifdef TRACE
    AsciiMode = TRUE;
#endif

#ifdef CDROM
    InitVoiceFile();
#endif

    ReadConfigFile();
}

/*══════════════════════════════════════════════════════════════════════════*/
void TheEndCheckFile(const char *filename) {
    if (FileSize(filename))
        End_Num = NOT_ENOUGH_MEM;
    else
        End_Num = ERROR_NOT_FOUND_FILE;

    End_Error = filename;

    exit(0);
}

/*══════════════════════════════════════════════════════════════════════════*/
void InitTheEnd(S32 num, char *error) {
    End_Num = num;
    End_Error = error;
}

/*══════════════════════════════════════════════════════════════════════════*/
void TheEnd(S32 num, const char *error) {
    End_Num = num;
    End_Error = error;

    exit(0);
}

/*══════════════════════════════════════════════════════════════════════════*/
#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
void DebugHQR(const char *titre, T_HQR_HEADER *hqr) {
    LogPrintf(titre);

    if (hqr) {
        LogPrintf(": %7ld - used: %7ld - MaxIndex: %4ld - NbIndex: %4ld\n", hqr->MaxSize, hqr->MaxSize - hqr->FreeSize, hqr->MaxIndex, hqr->NbIndex);
    } else {
        LogPrintf(": xxxxxxx - used: xxxxxxx - MaxIndex: xxxx - NbIndex: xxxx\n");
    }
}
#endif

/*══════════════════════════════════════════════════════════════════════════*/

// Trouver le moyen de cleaner cette @#&? de fonction !!!!!!

void V_ColorLine(U8 text, U8 back, U16 ncars) {
    text = text;
    back = back;
    ncars = ncars;
}

/*══════════════════════════════════════════════════════════════════════════*/
void TheEndInfo() {
    S32 err = FALSE;

#ifdef CDROM
    ClearVoiceFile();
#endif
    StopMusic();

    LogPrintf(Version); // dans version.cpp
    LogPrintf("%s\n", BuildInfo ? BuildInfo : "(no build info)");

#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
    LogPrintf("* Start Extended Memory was %ld\n", MemoMemory);
    LogPrintf("* Start Dos Memory was %ld\n", MemoDosMemory);
    LogPrintf("* Min Dos Memory was %ld\n", MemoMinDosMemory);
    LogPrintf("* Size PtrZBuffer: %ld\n", ListMem[MEM_PTR_ZBUFFER].Size);

    DebugHQR("* HQR Sprite     ", HQRPtrSprite);
    DebugHQR("      Sprite raw ", HQRPtrSpriteRaw);
    DebugHQR("    Anim 3DS     ", HQRPtrAnim3DS);
    DebugHQR("      Sample     ", HQR_Samples);
    DebugHQR("        Anim     ", HQR_Anims);
    DebugHQR("        Body     ", HQR_Bodys);
    DebugHQR("      ObjFix     ", HQR_ObjFix);
    DebugHQR("  HQR_Isle_Obj   ", HQR_Isle_Obj);
    DebugHQR("  HQR_CubeInfos  ", HQR_CubeInfos);
    DebugHQR("  HQR_ListDecors ", HQR_ListDecors);
    DebugHQR("  HQR_MapPGround ", HQR_MapPGround);
    DebugHQR("  HQR_ListTexDef ", HQR_ListTexDef);
    DebugHQR("  HQR_MapSommetY ", HQR_MapSommetY);
    DebugHQR("  HQR_MapIntens  ", HQR_MapIntensity);

    LogPrintf("* End Extended Memory was %ld\n", MemEnd);
    LogPrintf("* Total Extended Memory used: %ld\n", MemoMemory - MemEnd);

    LogPrintf("\nNbTexEffects= %d\n", NbTexEffects);
    if (PtrPolySea)
        LogPrintf("Animated Sea Enable.\n");
    else
        LogPrintf("Animated Sea Disable.\n");
#endif

    LogPrintf("\n");

    switch (End_Num) {
    case ERROR_NOT_FOUND_FILE:
        V_ColorLine(14, 0, 130);
        LogPrintf("*** ERROR-> File not found: %s\n\n", End_Error);
        V_ColorLine(7, 0, 130);
        break;

    case NAME_NOT_FOUND:
        V_ColorLine(14, 0, 130);
        LogPrintf("*** ERROR-> Critical error: ident name not found: %s\n\n", End_Error);
        V_ColorLine(7, 0, 130);
        break;

    case NOT_ENOUGH_MEM:
        V_ColorLine(14, 0, 130);
        LogPrintf("*** ERROR-> Not Enough Memory: %s (SEE README.TXT)\n\n", End_Error);
        V_ColorLine(7, 0, 130);
        break;

    case PROGRAM_OK:
#ifdef DEBUG_TOOLS
        if (NbNbf) {
            LogPrintf("* Frame speed status:\n");
            LogPrintf("  Maximal frame rate occured: %d\n", MaxNbf);
            LogPrintf("  Average frame rate was %d\n", TotalNbf / NbNbf);
            LogPrintf("%s\n", End_Error);
        }
#endif
        LogPrintf("%s\n", End_Error);
        LogPrintf("\nOK.\n");
        break;
    }

    WriteConfigFile();

    Clear3dExt();
}

/*══════════════════════════════════════════════════════════════════════════*/

#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
void Message(const char *mess, S32 flag) {
    S32 x;

    CoulText(LBAWHITE, 0);
    PalOne(LBAWHITE, 255, 255, 255);
    x = (strlen(mess) * 8) / 2;
    MemoClip();

    GraphPrintf(FALSE, 320 - x, 236, "%s", mess);
    Rect(320 - x - 4, 234, 320 + x + 4, 248, LBAWHITE);

    BoxStaticAdd(320 - x - 4, 234, 320 + x + 4, 248);

    if (flag) {
        BoxUpdate();
        WaitNoInput();
        WaitInput();
    }
    RestoreClip();
}
#else
void Message(const char *mess, S32 flag) {
    // pour virer warnings !!!! Baaahhhh ! Que j'aime pas ça, c'est pas
    // beau !!! mais ça marche et ça ne génère pas de code !!!

    mess = mess;
    flag = flag;
}
#endif

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/

#ifdef LBA2_TARGET_WIIU
// On Wii U the real entry point lives in LIB386/SYSTEM/WIIU_BACKEND.CPP, which
// brings up ProcUI/OSScreen and waits for foreground before calling into us.
// Renaming here avoids the symbol clash with the wrapper's main().
extern "C" int lba2_engine_main(int argc, char *argv[]);
int lba2_engine_main(int argc, char *argv[]) {
#else
int main(int argc, char *argv[]) {
#endif
    /* --version: print resolved engine version (from VERSION file at the
     * repo root, optionally suffixed with -dirty) and exit. Handled before
     * any other init so it works without retail data, without SDL, and on
     * stripped-down environments (CI, AppImage builders). The same string
     * is written to ${CMAKE_BINARY_DIR}/VERSION for release pipelines to
     * cat directly — see cmake/write_version.cmake. */
    {
        int i;
        for (i = 1; i < argc; ++i) {
            if (strcmp(argv[i], "--version") == 0) {
                printf("%s\n", LBA2_VERSION_STRING);
                return 0;
            }
        }
    }

    S32 load = FALSE;
    U8 *ptrdos4gw;
    char tmpFilePath[ADELINE_MAX_PATH];

#ifdef DEBUG_TOOLS
    MemoMemory = AvailableMem();       // memory at start
    MemoDosMemory = DosAvailableMem(); // Dos memory at start
#endif

    /* SDL filesystem APIs (GetDefaultUserDir, ResolveGameDataDir) require SDL
     * to be initialized before use. */
    if (!SDL_Init(0)) {
        LogPrintf("SDL_Init failed: %s\n", SDL_GetError());
        exit(EXIT_FAILURE);
    }

    {
        char resFolderPath[ADELINE_MAX_PATH] = "";
        char userFolderPath[ADELINE_MAX_PATH] = "";

        GetDefaultUserDir(userFolderPath);

        if (!ResolveGameDataDir(resFolderPath, ADELINE_MAX_PATH, &argc, argv)) {
            /* Do not SDL_Quit() here: SDL3 can double-free in SDL_QuitFilesystem
             * when shutting down after early filesystem API use; exiting is enough. */
            exit(EXIT_FAILURE);
        }

        InitDirectories(userFolderPath, resFolderPath, userFolderPath, "",
                        DistribVersion);
    }

    HQRGetErrorFunc = ErrorHQRGet;

    InitMainBuffer(); // Alloue gros buffer pour tous les pointeurs

    End_Error = PathConfigFile;

    InitAdeline(argc, argv);

    /* Console hooks: keyboard toggle/input interception + final-frame overlay compositing. */
    SetEventFilter(Console_FeedEvent);
    SetPrePresentCallback(Console_PrePresent);

    atexit(TheEndInfo);

    End_Error = "";

    WIIU_TRACE("InitProgram enter");
    InitProgram();
    WIIU_TRACE("InitProgram exit");

    WIIU_TRACE("InitMemory enter");
    InitMemory(); // Alloue petits buffers
    WIIU_TRACE("InitMemory exit");

#ifdef ONE_GAME_DIRECTORY
    WIIU_TRACE("CreateLbaDirectories enter");
    CreateLbaDirectories();
    WIIU_TRACE("CreateLbaDirectories exit");
#endif

    //-------------------   buffer samples

    if (Sample_Driver_Enabled) {
        WIIU_TRACE("HQR_Samples init enter");
        // déclare ressource samples buffer
        GetResPath(tmpFilePath, ADELINE_MAX_PATH, SAMPLES_HQR_NAME);
        HQR_Samples = HQR_Init_Ressource(
            tmpFilePath,
            SampleMem,
            SampleMem / 5000);

        SamplesEnable = TRUE;

        if (!HQR_Samples) {
            TheEndCheckFile(tmpFilePath);
        }

        // pour Windows
        HQR_Lock_Ressource(HQR_Samples);
        WIIU_TRACE("HQR_Samples init exit");
    }

    //-------------------

    WIIU_TRACE("InitLanguage enter");
    InitLanguage(); // multilangue
    WIIU_TRACE("InitLanguage exit");

    WIIU_TRACE("BufText alloc enter");
    BufText = (U8 *)SmartMalloc(BIG_FILE_DIA);
    if (!BufText)
        TheEnd(NOT_ENOUGH_MEM, "BufText");
    WIIU_TRACE("BufText alloc exit");

    BufOrder = (U16 *)SmartMalloc(BIG_FILE_ORD);
    if (!BufOrder)
        TheEnd(NOT_ENOUGH_MEM, "BufOrder");

    WIIU_TRACE("InitDial(0) enter");
    InitDial(0);
    WIIU_TRACE("InitDial(0) exit");

    WIIU_TRACE("GetMultiText 49 enter");
    GetMultiText(49, PleaseWait);
    WIIU_TRACE("GetMultiText 49 exit");

    GetMultiText(71, NewGameTxt);

    // chargement palettes ici car le InitDial(2) ou le AskForCD()
    // peuvent en avoir besoin...
    WIIU_TRACE("InitPalette enter");
    InitPalette();
    WIIU_TRACE("InitPalette exit");

    WIIU_TRACE("ChoicePalette enter");
    ChoicePalette();
    WIIU_TRACE("ChoicePalette exit");

#ifdef CDROM
#if defined(DEBUG_TOOLS) || defined(TEST_TOOLS)
    WIIU_TRACE("InitCD(\"\") enter");
    InitCD("");
    WIIU_TRACE("InitCD(\"\") exit");
#else
    WIIU_TRACE("CDROM block enter");
    SetBlackPal(); // pour etre sur ...

    {
        char volname[10];
        S32 cd = TRUE;

        switch (DistribVersion) {
        case UNKNOWN_VERSION:
        case EA_VERSION:
            strcpy(volname, "LBA2");
            strcpy(MessageNoCD, MESSAGE_NO_CD);
            break;

        default:
            strcpy(volname, "TWINSEN");
            strcpy(MessageNoCD, MESSAGE_NO_CD_US);
            break;
        }

        do {
            // pour Windows car ca prend du temps a scaner le CD
            WIIU_TRACE("CDROM Load_HQR(SCREEN,Log,PCR_CDROM) enter");
            GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
            Load_HQR_Image(tmpFilePath, Log, PCR_CDROM);
            WIIU_TRACE("CDROM Load_HQR(SCREEN) exit");
            WIIU_TRACE("BoxStaticFullflip enter");
            BoxStaticFullflip();
            WIIU_TRACE("BoxStaticFullflip exit");

            // premiere fois
            if (cd) {
                WIIU_TRACE("FadeToPal(PtrPalNormal) enter");
                FadeToPal(PtrPalNormal);
                WIIU_TRACE("FadeToPal(PtrPalNormal) exit");
            }

            WIIU_TRACE("InitCD(volname) enter");
            if (!InitCD(volname)) {
                WIIU_TRACE("InitCD(volname) returned FALSE");
                // premiere fois
                if (cd)
                    PlayErrorSample();

                if (!AskForCD()) {
                    TheEnd(PROGRAM_OK, MessageNoCD);
                }

                cd = FALSE;
            } else {
                WIIU_TRACE("InitCD(volname) returned TRUE");
                cd = TRUE;
            }
        } while (!cd);
    }
    WIIU_TRACE("FadeToBlack(PtrPalNormal) enter");
    FadeToBlack(PtrPalNormal);
    WIIU_TRACE("FadeToBlack(PtrPalNormal) exit");
    WIIU_TRACE("CDROM block exit");

#endif

    //-------------------   divers malloc
    WIIU_TRACE("BufMemoSeek SmartMalloc enter");
    BufMemoSeek = (U32 *)SmartMalloc(2048L);
    if (!BufMemoSeek)
        TheEnd(NOT_ENOUGH_MEM, "BufMemoSeek");
    WIIU_TRACE("BufMemoSeek SmartMalloc exit");
#endif

    WIIU_TRACE("InitJingle enter");
    InitJingle();
    WIIU_TRACE("InitJingle exit");

    WIIU_TRACE("BufferAnim SmartMalloc enter");
    BufferAnim = (U8 *)SmartMalloc(SIZE_BUFFER_ANIM);
    if (!BufferAnim)
        TheEnd(NOT_ENOUGH_MEM, "Buffer Anim");
    WIIU_TRACE("BufferAnim SmartMalloc exit");
    //-------------------

    WIIU_TRACE("LbaFont LoadMalloc_HQR(ress,FONT_GPM) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    LbaFont = (U8 *)LoadMalloc_HQR(tmpFilePath, RESS_FONT_GPM);
    if (!LbaFont)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("LbaFont LoadMalloc_HQR exit");

#ifdef LBA2_TARGET_WIIU
    // GPM mask bank = U32 offset table (one per char) + RLE glyph bytes.
    // Only the table is endian-sensitive (AffMask/GetDxDyMask index it as
    // U32); glyph data is pure bytes. Entry 0 points at the end of the
    // table, so table_entries = table[0]/4. Unswapped the offsets are
    // garbage → menu/dialog text renders as nothing.
    {
        U32 *tab = (U32 *)LbaFont;
        U32 first = LBA2_LE32(tab[0]);
        U32 n = first / 4;
        if (n >= 1 && n <= 4096 && (first & 3) == 0) {
            for (U32 i = 0; i < n; i++) tab[i] = LBA2_LE32(tab[i]);
            LogPrintf("[Font] offset table swapped: %lu entries (first glyph @%lu)",
                      (unsigned long)n, (unsigned long)first);
        } else {
            LogPrintf("[Font] UNEXPECTED table[0]=0x%08lx — table NOT swapped",
                      (unsigned long)first);
        }
    }
#endif

    SetFont(LbaFont, 2, 8);
    ColorFont(14);

    WIIU_TRACE("InitAcf enter");
    InitAcf();
    WIIU_TRACE("InitAcf exit");

#ifndef DEBUG_TOOLS
    //-------------------   logo adeline (affiche)

    WIIU_TRACE("DistribLogo enter");
    DistribLogo();
    WIIU_TRACE("DistribLogo exit");

    Cls();
    BoxUpdate();

    WIIU_TRACE("IsFirstGameLaunched enter");
    if (!IsFirstGameLaunched()) {
        WIIU_TRACE("AdelineLogo enter");
        AdelineLogo();
        WIIU_TRACE("AdelineLogo exit");

        Cls();
        BoxUpdate();
    }
    WIIU_TRACE("IsFirstGameLaunched exit");

#ifdef DEMO
    DemoBumper();

    Cls();
    BoxUpdate();
#endif
#else
    Cls();
#endif

    WIIU_TRACE("InitDial(2) enter");
    InitDial(2); // on s'assure que FR_GAM.VOX soit sur le Disque Dur
    WIIU_TRACE("InitDial(2) exit");

    WIIU_TRACE("Init3DExt enter");
    Init3DExt();
    WIIU_TRACE("Init3DExt exit");

    // a Faire Imperativement avant LoadFicPerso() !!!!!!!!
    WIIU_TRACE("Load_HQR(ress,BufferFile3D,FILE3D) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    if (!Load_HQR(tmpFilePath, BufferFile3D, RESS_FILE3D))
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("Load_HQR(FILE3D) exit");

#ifdef LBA2_TARGET_WIIU
    // BufferFile3D begins with a dense U32 offset table (one per perso/comportement
    // file). LoadFile3D(i) macro reads tab[i] raw — fix endian once here so the
    // 14 LoadFile3D calls in LoadFicPerso() return real pointers instead of wild
    // ones that would make SearchAnim spin in *ptrc++ command parsing.
    {
        unsigned int n = WiiU_SwapHqrSubOffsetTable(BufferFile3D);
        LogPrintf("[file3d] swapped %u offset entries\n", n);
    }
#endif

    WIIU_TRACE("LoadFicPerso enter");
    LoadFicPerso();
    WIIU_TRACE("LoadFicPerso exit");
    WIIU_TRACE("InitObjects enter");
    InitObjects(BufferAnim, SIZE_BUFFER_ANIM, GivePtrBody, GivePtrAnim);
    WIIU_TRACE("InitObjects exit");

    //-------------------   load ressources diverses

    // Textures inventaire, médaillon, etc... (256x256)
    WIIU_TRACE("BufferTexture LoadMalloc_HQR(ress,TEXTURES) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    BufferTexture = (U8 *)LoadMalloc_HQR(tmpFilePath, RESS_TEXTURES);
    if (!BufferTexture)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("BufferTexture LoadMalloc_HQR exit");

    WIIU_TRACE("ScanTextureAnimation enter");
    ScanTextureAnimation(BufferTexture, 256);
    WIIU_TRACE("ScanTextureAnimation exit");
    WIIU_TRACE("InitTextureAnimation enter");
    InitTextureAnimation(); // pour initialiser l'affaire
    WIIU_TRACE("InitTextureAnimation exit");

    WIIU_TRACE("PtrZvExtra LoadMalloc_HQR(ress,GOODIES_GPC) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    PtrZvExtra = (S16 *)LoadMalloc_HQR(tmpFilePath, RESS_GOODIES_GPC);
    if (!PtrZvExtra)
        TheEndCheckFile(tmpFilePath);
#ifdef LBA2_TARGET_WIIU
    // *_GPC = flat S16 LE array (8 per sprite: hotXY, dXY, ZV box). Left raw,
    // PtrProjectSprite adds garbage hotspots and every sprite clips off-screen.
    WiiU_SwapU16Array(PtrZvExtra, LoadMallocFileSize);
#endif
    WIIU_TRACE("PtrZvExtra LoadMalloc_HQR exit");

    WIIU_TRACE("PtrZvExtraRaw LoadMalloc_HQR(ress,GOODRAW_GPC) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    PtrZvExtraRaw = (S16 *)LoadMalloc_HQR(tmpFilePath, RESS_GOODRAW_GPC);
    if (!PtrZvExtraRaw)
        TheEndCheckFile(tmpFilePath);
#ifdef LBA2_TARGET_WIIU
    WiiU_SwapU16Array(PtrZvExtraRaw, LoadMallocFileSize);
#endif
    WIIU_TRACE("PtrZvExtraRaw LoadMalloc_HQR exit");

#ifndef DEMO
    WIIU_TRACE("PtrZvAnim3DS LoadMalloc_HQR(ress,ANIM3DS_GPC) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    PtrZvAnim3DS = (S16 *)LoadMalloc_HQR(tmpFilePath, RESS_ANIM3DS_GPC);
    if (!PtrZvAnim3DS)
        TheEndCheckFile(tmpFilePath);
#ifdef LBA2_TARGET_WIIU
    WiiU_SwapU16Array(PtrZvAnim3DS, LoadMallocFileSize);
#endif
    WIIU_TRACE("PtrZvAnim3DS LoadMalloc_HQR exit");
#endif

    //-------------------   buffer sprites

    WIIU_TRACE("HQRPtrSprite HQR_Init_Ressource(SPRITES) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SPRITES_HQR_NAME);
    HQRPtrSprite = HQR_Init_Ressource(
        tmpFilePath,
        SpriteMem,
        SpriteMem / 1000);

    if (!HQRPtrSprite)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("HQRPtrSprite HQR_Init_Ressource exit");

    WIIU_TRACE("HQRPtrSpriteRaw HQR_Init_Ressource(SPRIRAW) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SPRIRAW_HQR_NAME);
    HQRPtrSpriteRaw = HQR_Init_Ressource(
        tmpFilePath,
        SpriteRawMem,
        SpriteRawMem / 100);

    if (!HQRPtrSpriteRaw)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("HQRPtrSpriteRaw HQR_Init_Ressource exit");

//-------------------   Anims 3DS
#ifndef DEMO
    WIIU_TRACE("HQRPtrAnim3DS HQR_Init_Ressource(ANIM3DS) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, ANIM3DS_HQR_NAME);
    HQRPtrAnim3DS = HQR_Init_Ressource(
        tmpFilePath,
        Anim3DSMem,
        100);

    if (!HQRPtrAnim3DS)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("HQRPtrAnim3DS HQR_Init_Ressource exit");

    WIIU_TRACE("LoadListAnim3DS enter");
    LoadListAnim3DS();
    WIIU_TRACE("LoadListAnim3DS exit");
#endif
    //-------------------   Pof
    WIIU_TRACE("Load_HQR(ress,BufferPof,POF) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    Load_HQR(tmpFilePath, BufferPof, RESS_POF);
    WIIU_TRACE("Load_HQR(POF) exit");

    //-------------------   buffer Impacts
    WIIU_TRACE("Load_HQR(ress,BufferImpact,IMPACT) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    Load_HQR(tmpFilePath, BufferImpact, RESS_IMPACT);
    WIIU_TRACE("Load_HQR(IMPACT) exit");

    //-------------------   buffer animations

    WIIU_TRACE("HQR_Anims HQR_Init_Ressource(ANIM) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, ANIM_HQR_NAME);
    HQR_Anims = HQR_Init_Ressource(
        tmpFilePath,
        AnimMem,
        AnimMem / 800);

    if (!HQR_Anims)
        TheEndCheckFile(tmpFilePath);
#ifdef LBA2_TARGET_WIIU
    {
        unsigned char *nm = (unsigned char *)HQR_Anims->Name;
        LogPrintf("[init-check] HQR_Anims=%p Buffer=%p MaxSize=%d FreeSize=%d MaxIndex=%d NbIndex=%d\n",
                  (void *)HQR_Anims, HQR_Anims->Buffer,
                  (int)HQR_Anims->MaxSize, (int)HQR_Anims->FreeSize,
                  (int)HQR_Anims->MaxIndex, (int)HQR_Anims->NbIndex);
        LogPrintf("[init-check] HQR_Anims->Name as string: \"%s\"\n", HQR_Anims->Name);
        LogPrintf("[init-check] HQR_Anims->Name first 16 hex: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  nm[0], nm[1], nm[2], nm[3], nm[4], nm[5], nm[6], nm[7],
                  nm[8], nm[9], nm[10], nm[11], nm[12], nm[13], nm[14], nm[15]);
    }
#endif
    WIIU_TRACE("HQR_Anims HQR_Init_Ressource exit");
    WIIU_CANARY("after HQR_Anims init");

    //-------------------   buffer bodys

    WIIU_TRACE("HQR_Bodys HQR_Init_Ressource(BODY) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, BODY_HQR_NAME);
    HQR_Bodys = HQR_Init_Ressource(
        tmpFilePath,
        BodyMem,
        BodyMem / 1000);

    if (!HQR_Bodys)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("HQR_Bodys HQR_Init_Ressource exit");
    WIIU_CANARY("after HQR_Bodys init");

    WIIU_TRACE("HQR_ObjFix HQR_Init_Ressource(OBJFIX) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, OBJFIX_HQR_NAME);
    HQR_ObjFix = HQR_Init_Ressource(
        tmpFilePath,
        ObjFixMem,
        ObjFixMem / 1000);

    if (!HQR_ObjFix)
        TheEndCheckFile(tmpFilePath);
    WIIU_TRACE("HQR_ObjFix HQR_Init_Ressource exit");
    WIIU_CANARY("after HQR_ObjFix init");

    //-------------------   buffer flows de particules

    WIIU_TRACE("InitPartFlow enter");
    InitPartFlow();
    WIIU_TRACE("InitPartFlow exit");
    WIIU_TRACE("InitDarts enter");
    InitDarts();
    WIIU_TRACE("InitDarts exit");
    WIIU_TRACE("InitRain enter");
    InitRain();
    WIIU_TRACE("InitRain exit");
    WIIU_TRACE("InitSysText enter");
    InitSysText();
    WIIU_TRACE("InitSysText exit");

    // doit etre la derniere allocation faite
    WIIU_TRACE("PtrPolySea Malloc enter");
    PtrPolySea = (S_ANI_POLY *)Malloc(SIZE_BUFFER_POLY_SEA);
    if (!PtrPolySea)
        MaxPolySea = 0; // Animation de l'eau non dispo
    WIIU_TRACE("PtrPolySea Malloc exit");
    WIIU_CANARY("after PtrPolySea Malloc");

#ifdef DEBUG_TOOLS
    MemoMinDosMemory = DosAvailableMem(); // Dos memory after inits
    MemEnd = AvailableMem();              // Extend memory at the end
#endif

#ifndef DEBUG_TOOLS
    // Pour virer warning !!!!!!
    ArgC = ArgC;
#else
    // temporaire pour virer Warning
    ArgC = ArgC;

    Palette(PtrPalBlack);
#endif

    //-------------------   main game menu

    WIIU_TRACE("FadeToBlack(PalettePcx) enter");
    FadeToBlack(PalettePcx);
    WIIU_TRACE("FadeToBlack(PalettePcx) exit");

    // Pour un SlideShow sur un salon, il ne faut pas que les p'tits gars
    // puissent jouer avec le clavier !!!!!
    /*      if( ArgC==2 AND !strcasecmp(ArgV[1],"/NOKEY") )
        {
                KeyOff = TRUE ;
        }
*/

#ifdef DEBUG_TOOLS
    // Command-line cube selection: ./lba2 <cube_number>
    if (ArgC > 1) {
        char *p = ArgV[1];
        S32 isNumber = TRUE;
        while (*p) {
            if (!isdigit((unsigned char)*p++)) {
                isNumber = FALSE;
                break;
            }
        }

        if (isNumber) {
            S32 cubeNum = atoi(ArgV[1]);
            LogPrintf("\nStarting at cube %d\n", cubeNum);

            InitGame(1);
            NewCube = cubeNum; // Set AFTER InitGame since it resets NewCube to 0
            FlagFade = TRUE;
            MainLoop();
            TheEnd(PROGRAM_OK, "");
            return 0;
        }
    }
#endif

#ifdef LBA2_TARGET_N64
    if (N64DebugStartCube >= 0) {
        LogPrintf("\nStarting at cube %d (DebugStartCube)\n", (int)N64DebugStartCube);
        InitGame(1);
        NewCube = N64DebugStartCube; // after InitGame, which resets NewCube to 0
        FlagFade = TRUE;
        MainLoop();
        TheEnd(PROGRAM_OK, "");
        return 0;
    }
#endif

    // Load save game from command line argument
    if (ArgC > 1) {
#ifdef DEMO
#ifdef DEBUG_TOOLS
        load = atoi(ArgV[1]);
#endif
#else
        LogPrintf("\nTry to load %s\n", ArgV[1]);

        if (FileSize(ArgV[1])) {
            strcpy(GamePathname, ArgV[1]);
            load = TRUE;
        } else {
            GetSavePath(GamePathname, ADELINE_MAX_PATH, ArgV[1]);

            if (FileSize(GamePathname)) {
                load = TRUE;
            } else {
                char tmpFileName[ADELINE_MAX_PATH];
                snprintf(tmpFileName, ADELINE_MAX_PATH, "%s.lba", ArgV[1]);
                GetSavePath(GamePathname, ADELINE_MAX_PATH, tmpFileName);

                if (FileSize(GamePathname)) {
                    load = TRUE;
                } else
                    strcpy(GamePathname, "");
            }
        }

        if (load) {
            Palette(PtrPal);
        }
#endif
    }

#ifdef DEBUG_TOOLS
    HERCULE(HCls());
#endif

    WIIU_TRACE("MainGameMenu enter");
    MainGameMenu(load);
    WIIU_TRACE("MainGameMenu exit");

#ifdef DEMO
    if (!FlagShowEndDemo) {
        SlideShow();
    }

    DemoBumper();
#endif

    TheEnd(PROGRAM_OK, "");

    return 0;
}

/*══════════════════════════════════════════════════════════════════════════*/
