# Nintendo 64 port — development log (condensed)

A chronological summary of how the port came together. The recurring theme
is **budget**: a 93 MHz MIPS with an 8 KB data cache, 8 MB of RAM (with the
Expansion Pak), a 64 MB cartridge, and an engine that was designed to
rasterize 640×480 frames in software on a Pentium 90.

## Why start from the Wii U tree

The N64's VR4300 is big-endian, like the Wii U's PowerPC. The Wii U port had
already fought the whole little-endian campaign (scene loader, bodies,
bricks, islands, sort keys, text banks, saves…), so its tree was cloned and
built with `-DLBA2_TARGET_WIIU -DLBA2_TARGET_N64`: the first define keeps
every endianness fix, the second carves out the few WUT-only paths (heap,
FMV, SDL compatibility shims). The engine is fixed-point almost everywhere
(floats only in joystick and timing code), all x86 assembly had already been
ported to C, and the static footprint is ~2.2 MB — feasible, with the
Expansion Pak.

## Boot and first light

- Docker image with libdragon trunk on top of the official toolchain image;
  `Makefile.n64` mirrors the CMake source lists; `build-n64.sh` generates
  what CMake used to (version header, embedded `LBA2.CFG`) and stages the
  DFS.
- Backends: `N64_BACKEND.CPP` (main, timer, `LogPrintf` → ISViewer, keyboard
  and `dirent` shims over libdragon's `dir_findfirst`), `SVGA/N64.CPP`
  (8bpp `Phys` presented by the RDP as CI8 + TLUT), `JOYSTICK_N64.CPP`.
- Assets: the DFS is case-sensitive and the engine composes lowercase
  names, so everything is staged lowercase at the DFS root; `lba2.cfg` is
  pre-staged because the DFS is read-only.
- `stat()` is unreliable on `rom:/`: existence checks fall back to
  `fopen` / `dir_findfirst`, and only a return of 0 counts as "exists".
- **Unaligned accesses.** `GET_S16`/`GET_U32` and dozens of
  `LBA2_LE16(*(U16*)p)` sites cast unaligned pointers — a trap on MIPS.
  The 32 obvious sites were rewritten to `memcpy` helpers
  (`tools-n64/fix_unaligned_swaps.sh`); the long tail is handled by an
  **unaligned-access emulator in the exception handler** (lh/lhu/lw/lwu/ld/
  sh/sw/sd, full branch-delay-slot handling, sign-extended 64-bit GPRs, a
  BadVAddr/EPC sanity check so a wild pointer reaches the inspector instead
  of double-faulting silently).
- Memory diet: `AvailableMem()` reports 1 MB so the HQR pools clamp to
  their minima; display at 320×240 (a 640×480 16bpp double buffer alone is
  1.2 MB).
- First light: Twinsen's house at 58 fps, dialogues and scripts working.

## Audio

- `AIL/N64/SOUND_N64_BACKEND.CPP` on the libdragon RSP mixer: 12 SFX voices
  (VOC/WAV parsers from the Wii U backend), music channel, voice channel.
- Music: GOG `.ogg` → `.wav64` at staging time. Opus decodes on the CPU and
  was suspected of the frame drops; switching to **VADPCM** (RSP-decoded)
  did not change them — profiling showed the audio pump costs <1 ms/frame
  and the stutter is a *symptom* of the variable frame rate (the mixer is
  fed once per frame). Music is mono 16 kHz VADPCM to make room for voices.
- Two libdragon lessons: `mixer_ch_play` only reconfigures a channel when
  the `waveform_t` *pointer* changes (reusing one descriptor per slot made
  long samples inherit a footstep's length — fixed by alternating two
  descriptors per slot); and an 8-bit waveform played with pitch can make
  the mixer request odd-length chunks, tripping the RSP DMA alignment
  assert — samples are now stored as signed 16-bit.
- Voices: the VOX banks are 83 MB of IMA-ADPCM per language. English only,
  clips re-encoded to Opus at 12 kHz (~18 MB), the `.VOX` HQR replaced by
  placeholders that carry the clip name; the engine's `SPEAK_SAMPLE`
  handle is routed to a dedicated streaming channel.
- Jingle names `TADPCMn` are the CD tracks (`trackn.ogg` on GOG); mapping
  them fixed both the silent main-menu theme and the silent Citadel
  exteriors. `ResumeMusic` no longer stops the stream before resuming it
  (on PC "CD" and "jingle" were two players; here they are one).
- Two playtester reports. *Rain kept falling after the storm*: the engine
  addresses voices either by the handle `PlaySample` returned or by the
  bare sample number (`IsSamplePlaying(SAMPLE_RAIN)`, `StopOneSample(
  SAMPLE_RAIN)`); the N64 lookup only resolved full handles, so every
  number-based query was a no-op — the rain loop could not be stopped, the
  `SAMPLE_TIME_REPEAT` throttle never fired, `SampleAlways` loops stacked.
  Handles now use the MILES layout (`counter<<24 | user<<8 | slot`) and
  numbers resolve by user handle. *Lines cut off mid-sentence*: 20 long
  lines are split across `FlagNextVoc` chains whose continuation clips sit
  physically after the first part with **no index slot**; `vox_repack.py`
  iterated slots and dropped them, so the engine chained into whatever
  entry came next. The repack now follows the chain (1261 clips, up to 5
  parts per line).

## Input

The pad **emulates the PC keyboard**: every control injects the scancode of
the default keyboard binding into `TabKeys`, which is the only path that
reaches the spell shortcuts polled directly in `PERSO.CPP`. The stick is
decoupled from movement (D-pad) and carries the behaviour shortcuts. The
config file's `GamepadDeadzone=8000` killed the stick (raw range ±85): a
local deadzone of 24 is used instead.

## Render scale

Profiling (`[renderprof]`/`[affprof]` in the log, per 60 frames) showed the
frame is **fill-bound**: 75–80% of `AffScene` is pixel writes at 640×480.

A first proof of concept halved the *projection* (iso and perspective
scales, sphere radii, brick coordinates) so the world rendered at 320×240:
interiors went from 30 to 52–60 fps and proved the approach. But the UI,
fonts, menus, sprites and every one of ~700 2D call sites still spoke
640×480, and the clip rectangle, `ScreenXMin..`, `Xp/Yp` are global state
read by both worlds — unfixable site by site.

The final architecture keeps the **whole game logic in a virtual 640×480
space** (`ModeDesiredX/Y`, clip, dirty boxes, projection, all call sites
untouched) while `Log`/`Screen`/`Phys` are **physical 320×240** buffers, and
**only the pixel-writing cores in LIB386 convert** right before touching
memory (`RS_V2P()`, `RS_PushPhysClip()` in `SVGA/SCREEN.H`): `Fill_Poly`
(the single polygon entry point — a private copy of the point list with
halved screen coordinates), `Fill_Sphere`, lines, `AffGraph` (a half-scale
RLE decoder), fonts (`AffMask`, 2×2 OR so thin strokes survive), boxes,
block copies, sprite scalers, the dirty-box lists, shade/flow/rain/z-buffer
overwrite helpers, and full-screen images (downscaled in place after
`Load_HQR`). The brick pipeline is the one part that works natively in
physical space: `Map2Screen` is halved and the brick bank is pre-downscaled
once at load (`HalveBrickBank`, an RLE re-encoder — the masks derived from
it come for free), with `AffGraphNative` for the blit.

## Exterior stalls

Entering a new exterior area froze the game for 6–7 s. Instrumenting HQR
loads, `LoadCube` and the frame phases per slow frame showed six
consecutive ~1 s frames each **reloading the same 9 terrain cubes from ROM
and evicting them again**: with the 1 MB budget the `MapPGround` pool held
a single 32 KB cube while the exterior renderer needs the current cube plus
its 8 neighbours every frame. The island pools are now sized for 9 cubes
(plus more room for decor and character bodies); the RAM came from shrinking
`Phys` to its physical size and dropping the unused Smacker buffer. Cube
loads now hit the cache (9 cubes in 1 ms); what remains is 2–3 frames of
~1 s for the first full render of the new cubes and their ~100 decor
objects — the next target.

## First contact with hardware

A playtester ran the ROM on a real console (flashcart) into a consumer CRT.
Two things the emulator could not tell us: exteriors run noticeably
*smoother* on the silicon than under Ares (which models the VR4300's memory
and exception costs conservatively, and this port takes an exception per
unaligned access), so the profiler numbers are pessimistic; and the CRT's
overscan ate the first and last letters of every dialogue line, since the VI
presets fill the whole raster and the dialogue box sits 8 virtual pixels
from the edge. The fix is in the VI, not the engine: after `display_init`
the active window is shrunk by 6 % per side and the X/Y scales recomputed
from the registers libdragon wrote (so NTSC and PAL presets are handled
alike). The framebuffer and the 320×240 pixel cores are untouched;
emulators show a small black border.

The tester's first real crash came from the Temple of Bu (Desert island):
dropping through the pit of the secret passage into the temple never loaded
the scene — a black screen with the VI still refreshing. Reproduced in Ares
by starting a new game directly in cube 10 (`DebugStartCube`), where the
emulator logs "CPU frozen because of cached access to non-RDRAM area": the
first render read a block offset of ~100 MB out of the block library.
Integrity checks showed the library and the decoded grid were fine after
`InitGrille`, and a software watchpoint hooked into the log calls narrowed
the corruption to Twinsen's life script, opcode `LM_SET_GRM`: `IncrustGrm`
was given a GRM index of `0x800Fxxxx` — DoLife's opcode jump-table pointer,
still sitting in s1 — although the zone's `Info0` bytes were zero. The load
`lw s1,24(s0)` hits an unaligned zone table and is emulated by the port's
address-error handler, which writes the result into `reg_block_t::gpr[17]`…
and libdragon's `inthandler.S`, on the way out of an exception, only reloads
the caller-saved registers: s0–s7 are preserved by the C handler's ABI, so
they are never restored from the frame, and the emulated value is dropped.
Every emulated unaligned load whose destination is an s-register kept a
stale value; it depends on register allocation, which is why it showed up as
rare, scene-specific weirdness (the cellar scene-change heisenbug, where a
zone's `Info3` read as 2048 with zero bytes, has the same signature). Fixed
by patching `inthandler.S` in the toolchain image to reload s0–s7 from the
frame after `__onCriticalException`; a self-test confirmed `lw` into s1 now
returns the loaded value. `AffBrickBlock` additionally skips (and logs) a
cell whose block index is outside the library instead of freezing.

## Saving to the cartridge

The engine saves through plain POSIX calls — `open`/`read`/`write`/`stat`/
`unlink` in `LIB386/SYSTEM/FILES.CPP`, a directory scan for `*.LBA` in the
load menu, `lba2.cfg` rewritten key by key. On N64 the only writable
storage is the 32 KB of battery-backed SRAM declared in the ROM header, so
instead of teaching `SAVEGAME.CPP` about it, the SRAM became a filesystem:
libdragon lets a program register a prefix with a table of callbacks
(`attach_filesystem`), and newlib routes every `sram:/…` path to it — the
engine's user directory simply moved from `rom:/saves/` to `sram:/`. The
image is mirrored in RDRAM (header with magic and CRC32, then packed
records `size, name, data`); reads are served from the mirror, a write is
buffered per handle and, on `close()`, the whole image is rebuilt and DMA'd
to the cartridge at `0x08000000` with the PI domain-2 timings every SRAM
title programs. Name lookup is case-insensitive, because the engine probes
case variations of each path (it grew up on DOS). A blank or foreign part
fails the CRC and is formatted; a write that runs out of room is dropped
whole, so a failed save never leaves a truncated record for the load menu
to trip on.

The diet mattered more than the plumbing. A PC save is ~20 KB, 19,200 of
them the 160×120 thumbnail, and the PC engine stores the automatic
`current.lba` uncompressed. On N64 the thumbnail is drawn at 80×60 physical
pixels anyway (320×240 output), so that is what gets stored (4,800 bytes),
and `current.lba` goes through the same LZSS as the manual slots: a save is
now ~4 KB, and about six slots plus the resume file fit next to `lba2.cfg` —
which also moved into SRAM, so language and volume settings finally stick
(the first-boot default is English rather than the French dev config).
The one place the PC engine assumes a write cannot fail is the save menu;
it now checks the file afterwards and shows "Cartridge memory is full" in
the menu font. An input-free self-test (`DebugSaveTest` in the cfg) saves,
lists, reads back and fills the SRAM from a `DebugStartCube` boot, which
is how the layer was verified in Ares before the ROM went to the tester.

## Anatomy of an outdoor camera jump

The playtester suggested drawing the terrain cubes Twinsen is not standing
on with flat polygons instead of textured ones, to speed up exteriors.
Measuring it first taught us how the exterior actually renders. With the
classic camera (the default; `FollowCamera` is off) the terrain is not
redrawn every frame at all: `AffScene` only refreshes the grid when the
camera jumps — re-centring on Twinsen, entering an area — and the frames in
between draw the objects only. So the "exterior frame rate" is really two
numbers: 20–30 fps between jumps, and one ~850 ms frame (Ares) at every
jump. That single frame is the stall the tester feels.

To see inside it without a controller, `DebugFullRedraw: 1` forces the full
redraw every frame from a `DebugStartCube` boot, and a `[terrprof]` line
splits the terrain: the current cube spends ~35 ms projecting its 65×65
vertices and ~250 ms filling ~2400 triangles — around 100 µs per triangle,
for triangles that cover ~30 pixels at 320×240. That is not fill: it is the
per-scanline perspective setup (divisions, `lrintl` calls, a W queue) and
the 8 KB data cache missing on texture, Z-buffer and fog CLUT for nearly
every pixel — the fillers were written for wide 640×480 spans on a CPU with
a 256 KB L2. The 8 horizon cubes cost ~180 ms, of which ~83 ms is vertex
projection and only ~41 ms the polygon pass; decor objects add ~150 ms.

The flat-horizon idea is in the tree as `TerrainLod` (the terrain already
draws a flat underlay under an incrusted texture, so "flat" means skipping
the texture pass and filling opaque triangles in their palette bank) but
stays off: the horizon's polygon pass is 40 ms with or without textures,
because the cost is per triangle, not per texel. The levers that would
matter are fewer and bigger triangles for the far cubes (a 33×33 grid
would take ~60 ms off the vertex phase alone), a cheaper span setup in the
fillers for small triangles, and the decor objects.

## The same bug, shipped by someone else's toolchain

A second tester — building the ROM themselves rather than running ours —
hit a CPU exception while in the save menu: `Write to invalid memory
address` at `0x004B6FA1`, inside `memcpy` called from `LoadGameScreen`
(SAVEGAME.CPP), itself called from `DoGameMenu`. That call is not the save:
the pause menu redraws the thumbnail of `current.lba` on every cursor move.

The arithmetic gives the whole story. `LoadGameScreen` computes
`ptrdecomp = PtrSave + sizefile + RECOVER_AREA` and moves the compressed
tail there before expanding it. In our own build GCC compiles the header
read as `lw s0,1(a1)`: a save is a packed byte stream, and the compressed
size sits at offset 13 of `current.lba` (version byte, `NumCube`,
`"CURRENT"` and its NUL), so the load is always unaligned and always
emulated. With s0 still holding `lui s0,0x8012` — the high half of
`GamePathname`, set a few instructions earlier — the sum
`0x80396DA1 + 0x80120000 + 0x200` wraps a 32-bit pointer straight into
kuseg and lands on `0x004B6FA1`, the exact address on the tester's screen.
The dropped s-register write again, in a build made from a checkout that
carries the `inthandler.S` patch — but with a toolchain image predating it.
`build-n64.sh` only builds that image when it is missing, so a stale one is
reused in silence.

Two conclusions, both in the tree now. The build refuses to link a ROM
whose libdragon lacks the patch (it disassembles `exception_critical` and
looks for the s0–s7 reload), and `tools-n64/check_rom_unaligned_fix.py`
answers the same question for a ROM someone already has. And the save path
stops depending on the emulator at all: the `LbaRead*`/`LbaWrite*` macros
now go through `memcpy` of a constant size, which GCC turns into byte or
`lwl/lwr` sequences instead of a trapping `lw` — 16 KB of code for a file
format that is read once per menu redraw. The four decompression sites also
validate the header against the end of the load buffer first, so a save
that is corrupt for any other reason is refused (an empty thumbnail, or a
fresh start) instead of writing wherever the arithmetic points.

## Full-motion video: measured, not assumed

The port shipped with the cutscenes off, and the README said why: no room in
the cartridge, and no decoder budget on the CPU. The first half was arithmetic;
the second was an assumption that had never been tested. It turns out to be
wrong.

What is actually in `VIDEO.HQR`: 34 Smacker movies, 320x200 at 15 fps, 14.0
minutes and 223 MB in total, the intro alone 3:53 and 72.8 MB. Their names live
in `RESS.HQR` entry 48 (`RESS_ACFLIST`), and a name's position in that list is
its entry number in the archive — which is how `PlayAcf` finds a movie. Track 0
of each Smacker is the music, tracks 1/2/3 the French, German and English
voices.

The engine side already exists, from the Wii U tree: `SOURCES/PLAYACF.CPP`
streams entries out of the archive through libsmacker. On N64 that path is
compiled out, `InitAcf` tolerates the missing archive, and `PlayAcf` logs a
skip. Nothing was cauterised, so the question was only ever what the console
could decode and what would fit.

The decoder answer came from libdragon's `preview` branch, which carries a
video module the trunk build does not: MPEG-1 *and* H.264 decoders with RSP
ucode, an `fmv_play` that handles audio sync, seeking and frame dropping, and
`videoconv64` to encode on the host. `tools-n64/fmv-probe/` builds a ROM
against that branch which plays the real intro and reports, per frame, how many
frames the player had to drop and how much wall-clock time the stream took.

In Ares, at 320x192:

| encoding | video size | result |
|---|---|---|
| H.264, quality 55 | 3.72 MiB | 377/377 frames, 1.00x realtime, 15.1 fps |
| H.264, quality 80 | 9.05 MiB | 377/377 frames, 1.00x realtime, 15.1 fps |
| MPEG-1, quality 55 @24fps | 14.2 MiB | 600/601 frames, 1.00x realtime, 24.1 fps |

Not a single dropped frame in the two H.264 runs, and the cheapest of the three
is the one that looked flawless on screen. H.264 also keeps the original 15 fps:
MPEG-1 only allows the standard frame rate codes, so the source has to be
resampled to 24 or 25 and then costs more to decode for no gain. Audio is
another 3.66 MiB per copy of the intro as VADPCM — which is what those
measurements include — or 0.95 MiB as Opus, untested here and not to be assumed
free: Opus decodes on the VR4300, and that is exactly why the music went back to
VADPCM.

So the wall is space, not speed. The cartridge today is 60.6 MiB of 64:

| | |
|---|---|
| HQR archives | 24.54 MiB (`lba_bkg` 7.62, `samples` 5.49, `screen` 3.29, `body` 2.10, `holomap` 1.91, `anim` 1.14, rest) |
| voices | 15.83 MiB (1274 Opus clips at 12 kHz) |
| music | 15.44 MiB (25 VADPCM tracks) |
| islands (`.ile`/`.obl`) | 3.22 MiB |
| code | ~0.68 MiB |

That leaves about 3.4 MiB free — not enough for the intro alone in the
configuration that was measured, and the whole set at that quality would be
somewhere around 17-18 MiB (a scaling estimate, not a measurement: bitrate
follows content). Which is a different conversation from the one the README was
having: not "the machine cannot", but "what comes out to make room", or whether
a ROM larger than 64 MiB is acceptable for a port people run off a flashcart.

Three things would have to be settled before any of this becomes work. Whether
real hardware agrees with Ares, which renders through paraLLEl-RDP and makes the
YUV blit far cheaper than it is on a console. Whether the port can move from the
pinned trunk commit to `preview`, carrying the `inthandler.S` patch. And where
the megabytes come from.

## An answer nobody heard

The hardware tester reported that whenever an NPC asks Twinsen a question,
his spoken answer never plays and the game hangs until START. The freeze
itself is the original design: `GameAskChoice` (GAMEMENU.CPP) holds the
game while Twinsen says the chosen line, with
`while (TestSpeak() && !ESC && !(Input & I_MENUS)) MyGetInput();`. On PC
and Wii U the mixer runs on its own thread. Here it only advances inside
`AudioStreamPump()`, which only `ManageTime()` called, and `MyGetInput()`
never reaches `ManageTime()`. The voice channel was started and never
mixed, so it never reached its end: `TestSpeak()` stayed true, and START,
the loop's only other exit, was the one way out. The same pattern waits on
speech in INVENT.CPP and in `MyDial`'s next-page wait, where the voice
simply froze until the page was turned.

It is deterministic, not a hardware quirk. `ManageKeyboard()` (reached by
every `GetInput`) now pumps the mixer too. `DebugVoiceWait: 1` (with
`DebugStartCube`) replays the exact loop on a short clip and logs
`[voicewait]`: 10 s timeout before the fix, 542 ms after, in Ares.

## Diagnostics kept in the tree

- `[renderprof]`/`[affprof]`: per-60-frame breakdown (terrain, object fill,
  dirty-box copy, present, cache writeback, logic).
- `[hangprof]`: any frame ≥80 ms with its phase split, HQR loads/evictions,
  `LoadCube` count/time and unaligned-access exceptions; `ChangeCube`
  timing; heap snapshot after each cube.
- `[terrprof]`: terrain split per frame — current cube vs the 8 horizon
  cubes, vertex projection vs polygon fill, triangles submitted/drawn,
  textures skipped by `TerrainLod`. `DebugFullRedraw` in the cfg makes
  every frame a full redraw so the numbers can be read without input.
- `[scenechg]`: a checksum net around the cube-change zones for a rare
  heisenbug (Twinsen landing on a wall coming back from the cellar: the
  zone's `Info3` read as 2048 instead of 0). It distinguishes an emulator
  misread (`EMU-MISREAD`) from a memory mutation (`ZONE-MUTATED`); so far
  only a deterministic, harmless mutation on an exterior transition has
  been seen.
- `[grille] bad brick`: `N64BrickDrawable` validates every brick draw
  (cell slot in `BufCube`, brick id in `TabBlock`, the `BufferBrick`
  offset table) and skips the brick instead of letting `AffGraph` follow a
  wild offset — a hardware crash in the Temple of Bu, cause still open.
  The retail grids and every GRM a scene references check out offline, so
  the reason logged is what tells a bad id from an overwritten table.
- `tools-n64/run-ares.ps1` redirects Ares' stdout (the ROM's ISViewer
  channel) to `ares_log.txt`: boot trace, engine logs, libdragon asserts
  with symbolic backtraces.
- `tools-n64/check_rom_unaligned_fix.py <rom.z64>` says whether a ROM was
  linked against a patched libdragon (the s0–s7 reload). Worth running on
  any build whose origin is unclear before debugging its crashes.
