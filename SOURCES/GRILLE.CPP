#include "C_EXTERN.H"
#include "DIRECTORIES.H"

#include <SVGA/SCREEN.H>
#include <SYSTEM/LOGPRINT.H>
#ifdef LBA2_TARGET_WIIU
#include <SYSTEM/wiiu_endian.h>
#  define WIIU_TRACE(s) LogPrintf("[trace] " s)
#  define WIIU_TRACE_FMT(...) LogPrintf(__VA_ARGS__)
#else
#  define WIIU_TRACE(s) ((void)0)
#  define WIIU_TRACE_FMT(...) ((void)0)
#endif

/*══════════════════════════════════════════════════════════════════════════*
		      █▀▀▀▀ █▀▀▀█  █    █     █     █▀▀▀▀
		      ██ ▀█ ██▀█▀  ██   ██    ██    ██▀▀
		      ▀▀▀▀▀ ▀▀  ▀  ▀▀   ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

#define HEADER_BLOCK 3

#define DXBLOCK 0
#define DYBLOCK 1
#define DZBLOCK 2

extern S32 XS0, YS0, XS1, YS1, XS2, YS2, XS3, YS3; // bezier.c

/*-------------------------------------------------------------------------*/

S32 XpOrgw;
S32 YpOrgw;

#define NB_COLON 256  // 28
#define MAX_BRICK 256 // 150

#ifdef LBA2_TARGET_N64
// [renderscale] the brick pipeline is the one part of the engine that works
// directly in PHYSICAL (320x240) space: Map2Screen is halved and the bank is
// pre-downscaled 2x at load (HalveBrickBank), so masks/CopyMask/CalcGraphMsk
// come for free and every screen-space brick constant halves with it. The
// clip rectangle / ScreenXMin.. it exchanges with the rest of the engine are
// virtual (see SVGA/SCREEN.H) and converted at the boundary.
#define BRICK_COL_W 12  /* column stride, was 24 */
#define BRICK_CLIP_X 320
#define BRICK_CLIP_Y 240
#define BRICK_DY 19 /* brick graphic height for overlap tests, was 38 */
#else
#define BRICK_COL_W 24
#define BRICK_CLIP_X 640
#define BRICK_CLIP_Y 480
#define BRICK_DY 38
#endif

S16 NbBrickColon[NB_COLON];

typedef struct
{
    S16 Xm;
    S16 Ym;
    S16 Zm;
    S16 Ys;
    S16 Brick;
    S16 Col;

} T_COLONB;

T_COLONB ListBrickColon[NB_COLON][MAX_BRICK];

U8 *BufferMaskBrick = NULL;
S32 NbBlock;
S32 DxBlock, DyBlock, DzBlock;

S32 ForbidenBrick = 0;

U8 CodeJeu = 0;
S32 XMap = 0, YMap = 0, ZMap = 0;
S32 XScreen = 0, YScreen = 0;
PTR_U8 BufCube = 0;
PTR_U8 BufMap = 0;
PTR_U8 TabBlock = 0;
PTR_U8 BufferBrick = 0;

extern S32 Nxw, Nyw, Nzw;
//void	ReajustPos( U8 col ) ;
/*--------------------------------------------------------------------------*/
typedef struct
{
    U16 Gri_Start;                //	Index of GRI
    U16 Grm_Start;                //	Index of GRM
    U16 Bll_Start;                //	Index of BLL
    U16 Brk_Start;                //	Index of BRK
    U16 Max_Brk;                  //	Nb Brk
    U16 ForbidenBrick;            //	NEW : Brick Interdite
    U32 Max_Size_Gri;             //	Max Size Gri
    U32 Max_Size_Bll;             //	Max Size Bll
    U32 Max_Size_Brick_Cube;      //	Max Ideal Style Size
    U32 Max_Size_Mask_Brick_Cube; //	Max Size Mask

} T_BKG_HEADER;

T_BKG_HEADER BkgHeader;

typedef struct
{
    U8 My_Bll;        //	Index Style ( + Bll_Start )
    U8 My_Grm;        //	Index Grm ( + Grm_Start )
    U8 UsedBlock[32]; //	Bluck Used in Bll ( Bits )

} T_GRI_HEADER;

T_GRI_HEADER *GriHeader;
/*--------------------------------------------------------------------------*/
/*══════════════════════════════════════════════════════════════════════════*
	     █   █ █▀▀▀█ █▀▀▀█       █▀▀█  █▀▀▀█  █    █▀▀▀▀ █  ▄▀
	     ██▀▀█ ██ ▄█ ██▀█▀       ██▀▀█ ██▀█▀  ██   ██    ██▀▄
	     ▀▀  ▀ ▀▀▀▀  ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀  ▀▀   ▀▀▀▀▀ ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/
#define MAX_BRICK_GAME 20000L
#define OFFSET_BUFFER_FLAG 153800L

#ifdef LBA2_TARGET_N64
/* [renderscale] Pre-downscale the whole brick bank 2x right after
 * LoadUsedBrick builds it, so AffGraph/CopyMask/CalcGraphMsk keep working
 * untouched on half-size graphics (masks are derived AFTER this in
 * InitGrille). Rewrite is in-place, front-to-back: each new brick is never
 * larger than its source, so the write cursor never catches unread data;
 * a copy-through fallback keeps the bank valid if a brick can't be scaled. */

#define HB_MAX_W 96
#define HB_MAX_H 96
#define HB_MAX_BRICKS 4096

static U8 s_hbRaw[HB_MAX_W * HB_MAX_H]; /* decoded full-res brick, 0 = transparent */
static U8 s_hbEnc[8192];                /* encoded half-res brick staging */
static U32 s_hbOldTab[HB_MAX_BRICKS + 1];

/* Encode one half-res line (w2 pixels, already sampled) as skip/copy/repeat
 * blocks. Blocks MUST cover exactly w2 pixels (AffGraph advances the screen
 * pointer by pitch-deltaX at line end) and NbBlock must be >= 1 (CalcGraphMsk
 * decodes with do/while). Returns bytes written. */
static U32 HalveBrickEncodeLine(const U8 *pix, S32 w2, U8 *dst) {
    U8 *out = dst + 1; /* dst[0] = NbBlock */
    S32 nbblock = 0;
    S32 x = 0;

    while (x < w2) {
        S32 run = 1;
        U8 c = pix[x];
        while (x + run < w2 AND pix[x + run] == c)
            run++;

        if (c == 0) {
            S32 left = run;
            while (left > 0) {
                S32 n = (left > 64) ? 64 : left;
                *out++ = (U8)(n - 1); /* opcode 00 = skip n */
                nbblock++;
                left -= n;
            }
        } else if (run >= 3) {
            S32 left = run;
            while (left > 0) {
                S32 n = (left > 64) ? 64 : left;
                *out++ = (U8)(0x80 | (n - 1)); /* opcode 10 = repeat n */
                *out++ = c;
                nbblock++;
                left -= n;
            }
        } else {
            /* short opaque runs: accumulate a copy block */
            S32 start = x;
            S32 len = run;
            while (x + len < w2 AND pix[x + len] != 0) {
                S32 r2 = 1;
                while (x + len + r2 < w2 AND pix[x + len + r2] == pix[x + len])
                    r2++;
                if (r2 >= 3)
                    break;
                len += r2;
            }
            S32 done = 0;
            while (done < len) {
                S32 n = (len - done > 64) ? 64 : (len - done);
                *out++ = (U8)(0x40 | (n - 1)); /* opcode 01 = copy n */
                memcpy(out, pix + start + done, n);
                out += n;
                nbblock++;
                done += n;
            }
            x = start + len;
            continue;
        }
        x += run;
    }

    dst[0] = (U8)nbblock;
    return (U32)(out - dst);
}

/* Returns the new total bank size (first table entry stays (nbg+1)*4). */
static U32 HalveBrickBank(void) {
    U32 *tab = (U32 *)BufferBrick;
    U32 nbg = (tab[0] - 4) >> 2; /* same convention as CreateMaskGph */
    U32 i, skipped = 0;

    if (nbg == 0 OR nbg > HB_MAX_BRICKS) {
        LogPrintf("[HalveBrickBank] SKIP: nbg=%u out of range\n", (unsigned)nbg);
        return tab[nbg];
    }

    for (i = 0; i <= nbg; i++)
        s_hbOldTab[i] = tab[i];

    U32 newoff = tab[0];

    for (i = 0; i < nbg; i++) {
        const U8 *src = BufferBrick + s_hbOldTab[i];
        U32 oldsize = s_hbOldTab[i + 1] - s_hbOldTab[i];
        S32 w = src[0];
        S32 h = src[1];
        U32 encsize = 0;

        if (w > 0 AND h > 0 AND w <= HB_MAX_W AND h <= HB_MAX_H) {
            /* ---- decode full-res brick to raw pixels (0 = transparent) */
            memset(s_hbRaw, 0, (size_t)(w * h));
            const U8 *s = src + 4;
            for (S32 y = 0; y < h; y++) {
                U8 *row = s_hbRaw + y * w;
                S32 xx = 0;
                U8 nb = *s++;
                while (nb--) {
                    U8 ctl = *s++;
                    S32 n = (ctl & 0x3F) + 1;
                    switch (ctl >> 6) {
                    case 0: /* skip */
                        xx += n;
                        break;
                    case 1: /* copy */
                        while (n--) {
                            if (xx < w)
                                row[xx] = *s;
                            xx++;
                            s++;
                        }
                        break;
                    default: /* repeat */
                    {
                        U8 c = *s++;
                        while (n--) {
                            if (xx < w)
                                row[xx] = c;
                            xx++;
                        }
                    } break;
                    }
                }
            }

            /* ---- sample even pixels/lines and re-encode into staging */
            S32 w2 = (w + 1) >> 1;
            S32 h2 = (h + 1) >> 1;
            U8 line[HB_MAX_W / 2 + 1];
            U8 *enc = s_hbEnc;

            enc[0] = (U8)w2;
            enc[1] = (U8)h2;
            enc[2] = (U8)(((S32)(S8)src[2]) >> 1); /* HotX */
            enc[3] = (U8)(((S32)(S8)src[3]) >> 1); /* HotY */
            enc += 4;

            for (S32 y2 = 0; y2 < h2; y2++) {
                const U8 *row = s_hbRaw + (2 * y2) * w;
                for (S32 x2 = 0; x2 < w2; x2++)
                    line[x2] = row[2 * x2];
                enc += HalveBrickEncodeLine(line, w2, enc);
                if ((U32)(enc - s_hbEnc) > sizeof(s_hbEnc) - 256) {
                    enc = s_hbEnc; /* staging overflow -> copy-through */
                    break;
                }
            }
            if (enc != s_hbEnc)
                encsize = (U32)(enc - s_hbEnc);
        }

        if (encsize == 0 OR newoff + encsize > s_hbOldTab[i + 1]) {
            /* can't scale safely: keep the original graphic (compacted) */
            memmove(BufferBrick + newoff, src, oldsize);
            tab[i] = newoff;
            newoff += oldsize;
            skipped++;
        } else {
            memcpy(BufferBrick + newoff, s_hbEnc, encsize);
            tab[i] = newoff;
            newoff += encsize;
        }
    }

    tab[nbg] = newoff;

    LogPrintf("[HalveBrickBank] %u bricks halved (%u copied through), size %u -> %u\n",
              (unsigned)(nbg - skipped), (unsigned)skipped,
              (unsigned)s_hbOldTab[nbg], (unsigned)newoff);

    return newoff;
}

/* Shape of the bank LoadUsedBrick built, kept to validate every brick draw:
 * a wild offset read from BufferBrick sends AffGraph to an unmapped address
 * (hardware crash, Temple of Bu). */
static U32 s_bankBricks = 0; /* entries in the offset table */
static U32 s_bankSize = 0;   /* tab[s_bankBricks] = end of the pixel data */
#endif /* LBA2_TARGET_N64 */

/*--------------------------------------------------------------------------*/
S32 LoadUsedBrick() {
    U32 i, j, b;
    U8 *pt;
    U8 *ptb;
    U16 *ptw;
    U32 *ptoff;
    U8 *ptdata;
    U32 offset;
    U32 maxbrk;
    U32 brick;
    U32 offseek;
    U32 *ptseek;
    U32 handle;
    U16 *tabflag;
    U16 *ptflag;
    U32 min, max;
    U8 *ptdecomp;
    COMPRESSED_HEADER header;
    S32 nbbrick;
    char tmpFilePath[ADELINE_MAX_PATH];

    tabflag = (U16 *)((U8 *)Screen + OFFSET_BUFFER_FLAG);

    memset(tabflag, 0, MAX_BRICK_GAME * 2L); /* Table de U16 pour NewNumBrick */

    min = 60000;
    max = 0;

    //-------------------------------------- Premiere Passe, Préparation ptflag

    WIIU_TRACE("LoadUsedBrick: pass1 (collect min/max) enter");
    pt = GriHeader->UsedBlock; /* Debut de Used Block */

    nbbrick = 0;

    for (i = 1; i < 256; i++) {
        b = pt[i >> 3] & (1 << (7 - (i & 7))); /*	Recup Bit Block	*/
        if (!b)
            continue;

        // TabBlock starts with a U32 offset table (LE on disk). Without the
        // swap, ptb points into nowhere and the brick scan reads garbage.
        ptb = TabBlock + LBA2_LE32_UA((const void *)(TabBlock + ((i - 1) << 2)));
        ptw = (U16 *)(ptb + 5);                  /* Jump dx dy dz et collis */
        maxbrk = *ptb * *(ptb + 1) * *(ptb + 2); /* dx*dy*dz*/

        for (j = 0; j < maxbrk; j++, ptw += 2) {
            brick = LBA2_LE16(*ptw); // U16 LE brick reference

            if (brick) {
                brick--;

                if (brick < min)
                    min = brick;
                if (brick > max)
                    max = brick;

                if (tabflag[brick] == 0) {
                    nbbrick++;
                    tabflag[brick] = 1;
                }
            }
        }
    }
    WIIU_TRACE_FMT("[trace] LoadUsedBrick: pass1 exit (nbbrick=%d min=%u max=%u)\n",
                   nbbrick, (unsigned)min, (unsigned)max);

    //-------------------------------------- Deuxieme Passe, Load Brick

    WIIU_TRACE("LoadUsedBrick: pass2 (load bricks) enter");
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, BKG_HQR_NAME);
    handle = OpenRead(tmpFilePath);
    if (!handle) {
        WIIU_TRACE("LoadUsedBrick: OpenRead(BKG) FAILED");
        return (0L);
    }

    Seek(handle, BkgHeader.Brk_Start << 2, SEEK_FROM_START); // Begin offsets Brk

    Read(handle, Screen, BkgHeader.Max_Brk << 2); // Read Offsets Brk

    ptflag = tabflag + min;

    ptoff = (U32 *)BufferBrick;

    offset = (nbbrick + 1) * 4L;

    *ptoff++ = offset; /* First Offset        */

    ptdata = BufferBrick + offset; /* Jump nbbrick+1 Offset	*/

    nbbrick = 0;

    ptseek = (U32 *)Screen;

    for (i = min; i <= max; i++, ptflag++) {
        if (*ptflag) {
            nbbrick++;              /*	One More*/
            *ptflag = (S16)nbbrick; /*	Brick+1, for BLL */
            offseek = LBA2_LE32(*(ptseek + i)); // U32 LE seek offset
            Seek(handle, offseek, SEEK_FROM_START);
            Read(handle, &header, sizeof(header));
            // COMPRESSED_HEADER on disk is LE-encoded. SizeFile/CompressedSizeFile
            // are U32, but CompressMethod is S16 (see HQR.H). Using LE32 on a 16-bit
            // field promotes it to U32 then byte-swaps the wrong 4 bytes — for LZSS
            // method=1, that produces 0x10000 (65536) which matches no switch case
            // → brick is never loaded → BufferBrick stays zero → CalcGraphMsk
            // produces 256x256 garbage iterations and overflows BufferMaskBrick.
            U32 hCompressMethod    = LBA2_LE16(header.CompressMethod);
            U32 hSizeFile          = LBA2_LE32(header.SizeFile);
            U32 hCompressedSizeFile = LBA2_LE32(header.CompressedSizeFile);
#ifdef LBA2_TARGET_WIIU
            if (nbbrick <= 3) {
                LogPrintf("[pass2] brick %d (i=%u): offseek=0x%08X method=%u sizeFile=%u compressedSize=%u ptdata=%p\n",
                          (int)nbbrick, (unsigned)i, (unsigned)offseek,
                          (unsigned)hCompressMethod, (unsigned)hSizeFile,
                          (unsigned)hCompressedSizeFile, (void*)ptdata);
                LogPrintf("[pass2] header raw bytes: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
                          ((U8*)&header)[0], ((U8*)&header)[1], ((U8*)&header)[2], ((U8*)&header)[3],
                          ((U8*)&header)[4], ((U8*)&header)[5], ((U8*)&header)[6], ((U8*)&header)[7],
                          ((U8*)&header)[8], ((U8*)&header)[9], ((U8*)&header)[10], ((U8*)&header)[11]);
            }
#endif
            switch (hCompressMethod) {
            case 0: //	No compression
                Read(handle, ptdata, hSizeFile);
                break;
            case 1: //	Compression LZSS
                ptdecomp = ptdata + hSizeFile - hCompressedSizeFile + 500;
                Read(handle, ptdecomp, hCompressedSizeFile);
                //					ExpandLZSS( ptdecomp, ptdata, header.SizeFile )	;
                ExpandLZ(ptdata, ptdecomp, hSizeFile, 2);
                break;
            }
#ifdef LBA2_TARGET_WIIU
            if (nbbrick <= 3) {
                LogPrintf("[pass2] brick %d post-load first 16 bytes: %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X  %02X %02X %02X %02X\n",
                          (int)nbbrick,
                          ptdata[0], ptdata[1], ptdata[2], ptdata[3],
                          ptdata[4], ptdata[5], ptdata[6], ptdata[7],
                          ptdata[8], ptdata[9], ptdata[10], ptdata[11],
                          ptdata[12], ptdata[13], ptdata[14], ptdata[15]);
            }
#endif
            ptdata += hSizeFile;
            offset += hSizeFile;
            *ptoff++ = offset;
        }
    }
    WIIU_TRACE_FMT("[trace] LoadUsedBrick: pass2 exit (loaded %d bricks, total offset=%u)\n",
                   nbbrick, (unsigned)offset);

    //-------------------------------------- Troisieme passe, Rename Block

    WIIU_TRACE("LoadUsedBrick: pass3 (rename block) enter");
    pt = GriHeader->UsedBlock; /* Debut de Used Block */

    for (i = 1; i < 256; i++) {
        b = pt[i >> 3] & (1 << (7 - (i & 7))); /*	Recup Bit Block	*/
        if (!b)
            continue;

        ptb = TabBlock + LBA2_LE32_UA((const void *)(TabBlock + ((i - 1) << 2)));
        ptw = (U16 *)(ptb + 5);                  /* Jump dx dy dz et collis */
        maxbrk = *ptb * *(ptb + 1) * *(ptb + 2); /* dx*dy*dz*/

        for (j = 0; j < maxbrk; j++, ptw += 2) {
            U16 brk = LBA2_LE16(*ptw);
            if (brk) {
                // tabflag is host-order (we wrote it ourselves). Write back as
                // LE so subsequent BLL consumers (which still expect LE in
                // TabBlock) see a consistent layout.
                *ptw = LBA2_LE16(tabflag[brk - 1]);
            }
        }
    }
    WIIU_TRACE("LoadUsedBrick: pass3 exit");

    Close(handle);

    ForbidenBrick = tabflag[BkgHeader.ForbidenBrick];

#ifdef LBA2_TARGET_N64
    // [renderscale] shrink the freshly built bank to half resolution
    // before InitGrille derives the masks from it.
    offset = (U32)HalveBrickBank();
    s_bankBricks = (((U32 *)BufferBrick)[0] - 4) >> 2;
    s_bankSize = offset;
#endif

    return (offset);
}
/*-------------------------------------------------------------------------*/
void InitGrille(U32 numcube) {
    U32 id_gri;
    U32 id_bll;
    char tmpFilePath[ADELINE_MAX_PATH];

    WIIU_TRACE_FMT("[trace] InitGrille(numcube=%u): enter, BkgHeader.Gri_Start=%u Bll_Start=%u\n",
                   (unsigned)numcube,
                   (unsigned)BkgHeader.Gri_Start,
                   (unsigned)BkgHeader.Bll_Start);

    numcube = TabAllCube[numcube].Num; //	Indirection
    WIIU_TRACE_FMT("[trace] InitGrille: TabAllCube indirection -> numcube=%u\n", (unsigned)numcube);

    id_gri = BkgHeader.Gri_Start + numcube;

    GriHeader = (T_GRI_HEADER *)BufMap;

    WIIU_TRACE_FMT("[trace] InitGrille: Load_HQR(BKG, BufMap, id_gri=%u) enter\n", (unsigned)id_gri);
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, BKG_HQR_NAME);
    Load_HQR(tmpFilePath, BufMap, id_gri);
    WIIU_TRACE("InitGrille: Load_HQR(BufMap) exit");

    WIIU_TRACE("InitGrille: ManageSystem #1 enter");
    ManageSystem(); // for streaming
    WIIU_TRACE("InitGrille: ManageSystem #1 exit");

    id_bll = BkgHeader.Bll_Start + GriHeader->My_Bll; //+numcube
    WIIU_TRACE_FMT("[trace] InitGrille: GriHeader->My_Bll=%u id_bll=%u\n",
                   (unsigned)GriHeader->My_Bll, (unsigned)id_bll);

    WIIU_TRACE("InitGrille: Load_HQR(BKG, TabBlock, id_bll) enter");
    Load_HQR(tmpFilePath, TabBlock, id_bll);
    WIIU_TRACE("InitGrille: Load_HQR(TabBlock) exit");

#ifdef LBA2_TARGET_WIIU
    {
        LogPrintf("[BkgHeader] Bll_Start=%u Brk_Start=%u Max_Brk=%u Max_Size_Gri=%u Max_Size_Bll=%u Max_Size_BrickCube=%u Max_Size_MaskBrickCube=%u\n",
                  (unsigned)BkgHeader.Bll_Start, (unsigned)BkgHeader.Brk_Start,
                  (unsigned)BkgHeader.Max_Brk, (unsigned)BkgHeader.Max_Size_Gri,
                  (unsigned)BkgHeader.Max_Size_Bll,
                  (unsigned)BkgHeader.Max_Size_Brick_Cube,
                  (unsigned)BkgHeader.Max_Size_Mask_Brick_Cube);
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[BLL dump] TabBlock first 64 U32 entries (256 bytes):\n");
        for (int k = 0; k < 64; k++) {
            LogPrintf("  [%2d] raw=0x%08X swap=0x%08X\n",
                      k, (unsigned)t[k], (unsigned)LBA2_LE32(t[k]));
        }
        U8 *ub = GriHeader->UsedBlock;
        LogPrintf("[BLL dump] GriHeader->UsedBlock 32 bytes:\n");
        LogPrintf("  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  ub[0], ub[1], ub[2], ub[3], ub[4], ub[5], ub[6], ub[7],
                  ub[8], ub[9], ub[10], ub[11], ub[12], ub[13], ub[14], ub[15]);
        LogPrintf("  %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  ub[16], ub[17], ub[18], ub[19], ub[20], ub[21], ub[22], ub[23],
                  ub[24], ub[25], ub[26], ub[27], ub[28], ub[29], ub[30], ub[31]);
    }
#endif

    //----------------------------------------------	NEW
    WIIU_TRACE("InitGrille: LoadUsedBrick enter");
    if (!LoadUsedBrick()) {
        WIIU_TRACE("InitGrille: LoadUsedBrick returned FALSE -> early return");
        return;
    }
    WIIU_TRACE("InitGrille: LoadUsedBrick exit");

#ifdef LBA2_TARGET_WIIU
    {
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[BLL post-LoadUsedBrick] entry[0]=0x%08X swap=0x%08X entry[16]=0x%08X swap=0x%08X\n",
                  (unsigned)t[0], (unsigned)LBA2_LE32(t[0]),
                  (unsigned)t[16], (unsigned)LBA2_LE32(t[16]));
    }
#endif
    //----------------------------------------------

    WIIU_TRACE("InitGrille: ManageSystem #2 enter");
    ManageSystem(); // for streaming
    WIIU_TRACE("InitGrille: ManageSystem #2 exit");

    WIIU_TRACE("InitGrille: CreateMaskGph enter");
    CreateMaskGph(BufferBrick, BufferMaskBrick);
    WIIU_TRACE("InitGrille: CreateMaskGph exit");

#ifdef LBA2_TARGET_WIIU
    {
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[BLL post-CreateMaskGph] entry[0]=0x%08X swap=0x%08X entry[16]=0x%08X swap=0x%08X\n",
                  (unsigned)t[0], (unsigned)LBA2_LE32(t[0]),
                  (unsigned)t[16], (unsigned)LBA2_LE32(t[16]));
    }
#endif

    WIIU_TRACE("InitGrille: ManageSystem #3 enter");
    ManageSystem(); // for streaming
    WIIU_TRACE("InitGrille: ManageSystem #3 exit");

    // TabBlock starts with a U32 offset table stored little-endian on disk.
    // Without LE32 swap, NbBlock balloons to ~67M and CopyMapToCube blasts
    // gigabytes of garbage into ListObjet, clobbering ptrobj->PtrFile3D for
    // every scene object beyond Twinsen.
    NbBlock = LBA2_LE32_UA((const void *)TabBlock) / 4;
    WIIU_TRACE_FMT("[trace] InitGrille: NbBlock=%u (raw first U32 of TabBlock = 0x%08X swapped=0x%08X)\n",
                   (unsigned)NbBlock,
                   (unsigned)(*(U32 *)TabBlock),
                   (unsigned)LBA2_LE32_UA((const void *)TabBlock));

    WIIU_TRACE("InitGrille: CopyMapToCube enter");
    CopyMapToCube();
    WIIU_TRACE("InitGrille: CopyMapToCube exit");

#ifdef LBA2_TARGET_WIIU
    {
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[BLL post-CopyMapToCube] entry[0]=0x%08X swap=0x%08X entry[16]=0x%08X swap=0x%08X\n",
                  (unsigned)t[0], (unsigned)LBA2_LE32(t[0]),
                  (unsigned)t[16], (unsigned)LBA2_LE32(t[16]));
    }
#endif

    WIIU_TRACE("InitGrille: ManageSystem #4 enter");
    ManageSystem(); // for streaming
    WIIU_TRACE("InitGrille: ManageSystem #4 exit");

    WIIU_TRACE("InitGrille: RedrawGRMs enter");
    RedrawGRMs(); // au cas ou il y ait des GRMs actives
    WIIU_TRACE("InitGrille: RedrawGRMs exit");

#ifdef LBA2_TARGET_WIIU
    {
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[BLL post-RedrawGRMs] entry[0]=0x%08X swap=0x%08X entry[16]=0x%08X swap=0x%08X\n",
                  (unsigned)t[0], (unsigned)LBA2_LE32(t[0]),
                  (unsigned)t[16], (unsigned)LBA2_LE32(t[16]));
    }
#endif

    WIIU_TRACE("InitGrille: end");
    return;
}

/*--------------------------------------------------------------------------*/
void FreeGrille() {}
/*-------------------------------------------------------------------------*/
/*══════════════════════════════════════════════════════════════════════════*
		   █▀▀▀▀ ██▄ █ █▀▀▀▄       █   █ █▀▀▀█ █▀▀▀█
		   ██▀▀  ██▀██ ██  █       ██▀▀█ ██ ▄█ ██▀█▀
		   ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀        ▀▀  ▀ ▀▀▀▀  ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/
/*--------------------------------------------------------------------------*/
/*══════════════════════════════════════════════════════════════════════════*
	     █▀▀▀▀ █   █ ██▄ █ █▀▀▀▀ ▀▀█▀▀  █    █▀▀▀█ ██▄ █ ██▀▀▀
	     ██▀▀  ██  █ ██▀██ ██      ██   ██   ██  █ ██▀██ ▀▀▀▀█
	     ▀▀    ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀   ▀▀   ▀▀   ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

/*--------------------------------------------------------------------------*/
U8 *GetAdrColonneMap(S32 x, S32 z) {
    U32 offset;

    // BufMap U16 offset table is LE on disk; without swap CopyMapToCube
    // dereferences garbage column pointers.
    offset = LBA2_LE16_UA((const void *)(BufMap + sizeof(T_GRI_HEADER) + (x + (z * SIZE_CUBE_Z)) * 2));

    return (BufMap + sizeof(T_GRI_HEADER) + offset);
}
/*--------------------------------------------------------------------------*/
U8 *GetAdrColonneCube(S32 x, S32 z) {
    U32 offset;

    offset = (x * SIZE_CUBE_Y * 2) + (z * SIZE_CUBE_Z * SIZE_CUBE_Y * 2);

    return (BufCube + offset);
}

/*--------------------------------------------------------------------------*/
U8 WorldColBrickVisible(S32 oldx, S32 oldy, S32 oldz,
                        S32 xw, S32 yw, S32 zw) {
    U8 col = GetWorldColBrickVisible(xw, yw, zw);

    if (col)
        return (col);

    if (abs(xw - oldx) >= SIZE_BRICK_XZ
                              OR abs(yw - oldy) >= SIZE_BRICK_Y
                                                       OR abs(zw - oldz) >= SIZE_BRICK_XZ) {
        return (WorldColBrickVisible(oldx, oldy, oldz,
                                     oldx + (xw - oldx) / 2,
                                     oldy + (yw - oldy) / 2,
                                     oldz + (zw - oldz) / 2));
    }

    return (0); // Ok, cette fois, on est sûr !:)
}

/*--------------------------------------------------------------------------*/
/*U8	WorldColBrickVisible( S32 xw, S32 yw, S32 zw )
{
	if( GetBlockBrick( xw, yw, zw ) )
	{
		return( WorldColBrick( xw, yw, zw ) ) ;
	}
	else	return 0 ;
}
*/

/*--------------------------------------------------------------------------*/

void GetShadow(S32 xw, S32 yw, S32 zw) {
    S32 xm, ym, zm;
    U8 *ptc;
    S32 y;
    U8 *adr;
    S32 block;

    xm = xw / SIZE_BRICK_XZ;
    ym = yw / SIZE_BRICK_Y;
    zm = zw / SIZE_BRICK_XZ;

    ShadowX = Nxw = xw;
    ShadowZ = Nzw = zw;

    if (ym >= SIZE_CUBE_Y) // au-dessus du plafond de la map
    {
        ym = SIZE_CUBE_Y - 1;
    }

    ptc = BufCube + ym * 2 + (xm * SIZE_CUBE_Y * 2) + (zm * SIZE_CUBE_X * SIZE_CUBE_Y * 2);

    for (y = ym; y > 0; y--) {
        if (*ptc != 0)
            break;
        ptc -= 2;
    }

    XMap = xm;
    YMap = y;
    ZMap = zm;

    Nyw = (y + 1) * SIZE_BRICK_Y;

    ShadowCol = 0;

    if (*ptc != 0) {
        block = (*ptc++) - 1;
        adr = GetAdrBlock(block);
        adr += HEADER_BLOCK;
        adr += (*ptc) << 2; /* 4 Bytes to Jump	*/
        ShadowCol = *adr;
        ReajustPos(ShadowCol);
    }

    ShadowY = AdjustShadowObjects(xw, zw, Nyw, yw);
}

/*──────────────────────────────────────────────────────────────────────────*/
/*--------------------------------------------------------------------------*/
S32 ProjectShadow(T_SORT *ptrs, S32 beta) {
    S32 xmin, zmin, xmax, zmax, xw, yw, zw;
    S32 ys, cy;
    S32 altitude_ref;
    S32 demix, demiz;

    ClearScreenMinMax();

    xw = ptrs->Xw;
    yw = ptrs->Yw;
    zw = ptrs->Zw;

    xmin = ptrs->XMin - xw;
    zmin = ptrs->ZMin - zw;
    xmax = ptrs->XMax - xw;
    zmax = ptrs->ZMax - zw;

    demix = (xmin + xmax) / 2;
    demiz = (zmin + zmax) / 2;

    // inclure ici la gestion des objets du decors
    altitude_ref = AdjustShadowObjects(xw, zw, yw, yw);

    Rotate(demix, zmin, beta);

    X0 += xw;
    Z0 += zw;
    ys = altitude_ref + SIZE_BRICK_Y;

    GetShadow(X0, ys, Z0);
    cy = AdjustShadowObjects(X0, Z0, ShadowY, yw);

    if (cy < altitude_ref - ALTITUDE_MAX)
        cy = altitude_ref - ALTITUDE_MAX;
    else if (cy > altitude_ref + ALTITUDE_MAX)
        cy = altitude_ref + ALTITUDE_MAX;

    if (!LongProjectPoint(X0, cy, Z0))
        return FALSE;
    XS0 = Xp;
    YS0 = Yp;
    AdjustScreenMinMax();

    Rotate(xmin, demiz, beta);

    X0 += xw;
    Z0 += zw;

    GetShadow(X0, ys, Z0);
    cy = AdjustShadowObjects(X0, Z0, ShadowY, yw);

    if (cy < altitude_ref - ALTITUDE_MAX)
        cy = altitude_ref - ALTITUDE_MAX;
    else if (cy > altitude_ref + ALTITUDE_MAX)
        cy = altitude_ref + ALTITUDE_MAX;

    if (!LongProjectPoint(X0, cy, Z0))
        return FALSE;
    XS1 = Xp;
    YS1 = Yp;
    AdjustScreenMinMax();

    Rotate(demix, zmax, beta);

    X0 += xw;
    Z0 += zw;

    GetShadow(X0, ys, Z0);
    cy = AdjustShadowObjects(X0, Z0, ShadowY, yw);

    if (cy < altitude_ref - ALTITUDE_MAX)
        cy = altitude_ref - ALTITUDE_MAX;
    else if (cy > altitude_ref + ALTITUDE_MAX)
        cy = altitude_ref + ALTITUDE_MAX;

    if (!LongProjectPoint(X0, cy, Z0))
        return FALSE;
    XS2 = Xp;
    YS2 = Yp;
    AdjustScreenMinMax();

    Rotate(xmax, demiz, beta);

    X0 += xw;
    Z0 += zw;

    GetShadow(X0, ys, Z0);
    cy = AdjustShadowObjects(X0, Z0, ShadowY, yw);

    if (cy < altitude_ref - ALTITUDE_MAX)
        cy = altitude_ref - ALTITUDE_MAX;
    else if (cy > altitude_ref + ALTITUDE_MAX)
        cy = altitude_ref + ALTITUDE_MAX;

    if (!LongProjectPoint(X0, cy, Z0))
        return FALSE;
    XS3 = Xp;
    YS3 = Yp;
    AdjustScreenMinMax();

    UnsetClip();
    SetClip(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax);

    return TRUE;
}

/*══════════════════════════════════════════════════════════════════════════*
		   █▄ ▄█ █▀▀▀▀ █▄ ▄█ █▀▀▀█  █    █▀▀▀█ █▀▀▀▀
		   ██▀ █ ██▀▀  ██▀ █ ██  █  ██   ██▀█▀ ██▀▀
		   ▀▀  ▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀  ▀▀   ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

U32 CreateMaskGph(U8 *ptsrc, U8 *ptdst) {
    U8 *ptd;
    U32 nbg, off;
    U32 *ptoff;
    U32 size, i;

    ptoff = (U32 *)ptdst;

    off = *(U32 *)ptsrc; /*	First Offset Src	*/

    *ptoff++ = off; /*	First Offset	*/

    ptd = ptdst + off;

    nbg = (off - 4) >> 2; /*	Nombre de Graph	*/

#ifdef LBA2_TARGET_WIIU
    LogPrintf("[CreateMaskGph] entry: ptsrc=%p ptdst=%p first_off=%u nbg=%u BufferMaskBrick.size=%ld\n",
              (void*)ptsrc, (void*)ptdst, (unsigned)off, (unsigned)nbg,
              (long)ListMem[MEM_BUFFER_MASK_BRICK].Size);
    U8 *ptd_initial = ptd;
    U32 max_size_hint = (U32)ListMem[MEM_BUFFER_MASK_BRICK].Size;
#endif

    for (i = 0; i < nbg; i++) {
        size = CalcGraphMsk(i, ptsrc, ptd);

#ifdef LBA2_TARGET_WIIU
        if (i < 8 || (size > 1024)) {
            LogPrintf("[CreateMaskGph] i=%u size=%u total=%u (limit=%u) ptd_offset=%ld\n",
                      (unsigned)i, (unsigned)size, (unsigned)(off + size),
                      max_size_hint, (long)(ptd - ptd_initial));
        }
        if (off + size > max_size_hint) {
            LogPrintf("[CreateMaskGph] OVERFLOW at i=%u: cumulative off=%u > max=%u, ABORTING loop\n",
                      (unsigned)i, (unsigned)(off + size), max_size_hint);
            // Truncate to avoid TabBlock corruption while we figure out the real fix
            return off;
        }
#endif

        off += size;    /*	Maj Offset	*/
        *ptoff++ = off; /*	Write Offset 	*/
        ptd += size;    /*	Maj Pt Dest	*/
    }
#ifdef LBA2_TARGET_WIIU
    LogPrintf("[CreateMaskGph] exit: total_off=%u ptd_advanced=%ld nbg=%u\n",
              (unsigned)off, (long)(ptd - ptd_initial), (unsigned)nbg);
#endif
    return (off);
}

/*-------------------------------------------------------------------------*/
void CopyMapToCube() {
    U8 *pts, *ptd;
    S32 x, z;

    /*-------------- Copy -------------------------*/
    for (z = 0; z < SIZE_CUBE_Z; z++) {
        for (x = 0; x < SIZE_CUBE_X; x++) {
            pts = GetAdrColonneMap(x, z);
            ptd = GetAdrColonneCube(x, z);

            DecompColonne(pts, ptd);
        }
    }
}
/*──────────────────────────────────────────────────────────────────────────*/
void InitBufferCube() {
    S32 intmem;
    char tmpFilePath[ADELINE_MAX_PATH];

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, BKG_HQR_NAME);

    //	pas de RECOVER_AREA car BkgHeader en stored et pas en LZSS
    Load_HQR(tmpFilePath, &BkgHeader, 0); // HEADER
#ifdef LBA2_TARGET_WIIU
    // T_BKG_HEADER fields stored little-endian in bkg.hqr (x86 retail format).
    // Byte-swap on PPC, otherwise the size fields explode and MainBuffer
    // computation fails. 6x U16 + 4x U32 = 28 bytes packed.
    BkgHeader.Gri_Start                = LBA2_LE16(BkgHeader.Gri_Start);
    BkgHeader.Grm_Start                = LBA2_LE16(BkgHeader.Grm_Start);
    BkgHeader.Bll_Start                = LBA2_LE16(BkgHeader.Bll_Start);
    BkgHeader.Brk_Start                = LBA2_LE16(BkgHeader.Brk_Start);
    BkgHeader.Max_Brk                  = LBA2_LE16(BkgHeader.Max_Brk);
    BkgHeader.ForbidenBrick            = LBA2_LE16(BkgHeader.ForbidenBrick);
    BkgHeader.Max_Size_Gri             = LBA2_LE32(BkgHeader.Max_Size_Gri);
    BkgHeader.Max_Size_Bll             = LBA2_LE32(BkgHeader.Max_Size_Bll);
    BkgHeader.Max_Size_Brick_Cube      = LBA2_LE32(BkgHeader.Max_Size_Brick_Cube);
    BkgHeader.Max_Size_Mask_Brick_Cube = LBA2_LE32(BkgHeader.Max_Size_Mask_Brick_Cube);
#endif

    // 	pas de RECOVER_AREA car TabAllCube en stored et pas en LZSS
    Load_HQR(tmpFilePath, (void *)TabAllCube, BkgHeader.Brk_Start + BkgHeader.Max_Brk);

    // 	Calcul des tailles nécessaires
    intmem = (BkgHeader.Max_Size_Brick_Cube + RECOVER_AREA)    // BufferBrick
             + (SIZE_CUBE_X * SIZE_CUBE_Y * SIZE_CUBE_Z * 2L); // BufCube

    //	PtrZvBuffer sert aussi pour les décors intérieurs
    ListMem[MEM_PTR_ZBUFFER].Size = ade_max(intmem, SIZE_Z_BUFFER);

    ListMem[MEM_BUFFER_MASK_BRICK].Size = BkgHeader.Max_Size_Mask_Brick_Cube;
    ListMem[MEM_BUF_MAP].Size = BkgHeader.Max_Size_Gri + RECOVER_AREA;
    ListMem[MEM_TAB_BLOCK].Size = BkgHeader.Max_Size_Bll + RECOVER_AREA;
}

/*══════════════════════════════════════════════════════════════════════════*
		   █▀▀▀▄  █    ██▀▀▀ █▀▀▀█ █     █▀▀▀█ █  ▄▀
		   ██  █  ██   ▀▀▀▀█ ██▀▀▀ ██    ██▀▀█ ██▀
		   ▀▀▀▀   ▀▀   ▀▀▀▀▀ ▀▀    ▀▀▀▀▀ ▀▀  ▀ ▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void DrawOverBrick(S32 xm, S32 ym, S32 zm) {
    register T_COLONB *ptrlbc;
    register S32 colscreen;
    S32 col, i;
    S32 startcol, endcol;
    S32 xw, yw, zw;
    S32 recouvre;

    // [renderscale] the clip rectangle is virtual, the brick columns physical
    startcol = (RS_V2P(ClipXMin) + BRICK_COL_W) / BRICK_COL_W - 1;
    endcol = (RS_V2P(ClipXMax) + BRICK_COL_W) / BRICK_COL_W;

    for (col = startcol; col <= endcol; col++) {
        ptrlbc = &ListBrickColon[col][0];

        colscreen = col * BRICK_COL_W - BRICK_COL_W;

        for (i = 0; i < NbBrickColon[col]; i++) {
            /* bricks devant */
            if (ptrlbc->Ys + BRICK_DY > RS_V2P(ClipYMin)
                                      AND ptrlbc->Ys <= RS_V2P(ClipYMax)) {
                recouvre = FALSE;

                // essai correction bugs escaliers
                //			if( WorldColBrick(ptrlbc->Xm*SIZE_BRICK_XZ,ptrlbc->Ym*SIZE_BRICK_Y,ptrlbc->Zm*SIZE_BRICK_XZ) > 1 )
                if (ptrlbc->Col > 1) // collis complexe ?
                {
                    if (ptrlbc->Ym >= ym + 1) {
                        recouvre = TRUE;
                    }
                } else if (ptrlbc->Ym >= ym) {
                    recouvre = TRUE;
                }

                if (recouvre) {
                    xw = ptrlbc->Xm * SIZE_BRICK_XZ;
                    yw = ptrlbc->Ym * SIZE_BRICK_Y;
                    zw = ptrlbc->Zm * SIZE_BRICK_XZ;

                    if ((ptrlbc->Xm >= xm AND ptrlbc->Zm >= zm)
                            OR(ptrlbc->Xm > xm
                                                AND WorldColBrick(xw - SIZE_BRICK_XZ, yw, zw) !=
                               1
                                   //				AND WorldColBrick(xw,yw,zw+SIZE_BRICK_XZ)==1 )
                                   AND WorldColBrick(xw, yw, zw + SIZE_BRICK_XZ) >= 1)
                                OR(ptrlbc->Zm > zm
                                                    AND WorldColBrick(xw, yw, zw - SIZE_BRICK_XZ) !=
                                   1
                                       //				AND WorldColBrick(xw+SIZE_BRICK_XZ,yw,zw)==1 ) )
                                       AND WorldColBrick(xw + SIZE_BRICK_XZ, yw, zw) >= 1)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }
                }
            }
            ptrlbc++;
        }
    }
}

/*--------------------------------------------------------------------------*/
/* recouvrement reel pour obj qui ne depassent pas la ZV */

void DrawOverBrick3(S32 xm, S32 ym, S32 zm, S32 ymax) {
    register T_COLONB *ptrlbc;
    register S32 colscreen;
    S32 col, i;
    S32 startcol, endcol;

    // [renderscale] the clip rectangle is virtual, the brick columns physical
    startcol = (RS_V2P(ClipXMin) + BRICK_COL_W) / BRICK_COL_W - 1;
    endcol = (RS_V2P(ClipXMax) + BRICK_COL_W) / BRICK_COL_W;

    for (col = startcol; col <= endcol; col++) {
        ptrlbc = &ListBrickColon[col][0];

        colscreen = col * BRICK_COL_W - BRICK_COL_W;

        for (i = 0; i < NbBrickColon[col]; i++, ptrlbc++) {
            /* bricks devant */
            if (ptrlbc->Ys + BRICK_DY > RS_V2P(ClipYMin)
                                      AND ptrlbc->Ys <= RS_V2P(ClipYMax)) {
                if (ptrlbc->Ym > ymax) {
                    if ((ptrlbc->Zm >= zm)
                            OR(ptrlbc->Xm >= xm)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }
                } else if (ptrlbc->Ym >= ym) {
                    if ((ptrlbc->Zm == zm)
                            AND(ptrlbc->Xm == xm)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }

                    if ((ptrlbc->Zm > zm)
                            OR(ptrlbc->Xm > xm)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }
                }
            }
        }
    }
}

/*--------------------------------------------------------------------------*/
// recouvrement reel pour obj qui ne depassent pas la ZV
// GROSSE RUSTINE: Cage à la fin de LBA2 pour ne pas recouvrir l'objet avec
// les briques du sol en 0

void DrawOverBrickCage(S32 xm, S32 ym, S32 zm) {
    register T_COLONB *ptrlbc;
    register S32 colscreen;
    S32 col, i;
    S32 startcol, endcol;

    // [renderscale] the clip rectangle is virtual, the brick columns physical
    startcol = (RS_V2P(ClipXMin) + BRICK_COL_W) / BRICK_COL_W - 1;
    endcol = (RS_V2P(ClipXMax) + BRICK_COL_W) / BRICK_COL_W;

    for (col = startcol; col <= endcol; col++) {
        ptrlbc = &ListBrickColon[col][0];

        colscreen = col * BRICK_COL_W - BRICK_COL_W;

        for (i = 0; i < NbBrickColon[col]; i++, ptrlbc++) {
            /* bricks devant */
            if (ptrlbc->Ys + BRICK_DY > RS_V2P(ClipYMin)
                                      AND ptrlbc->Ys <= RS_V2P(ClipYMax)) {
                if (ptrlbc->Ym > 0 // diff ici avec DrawOverBrick3
                                 AND ptrlbc->Ym >= ym) {
                    if ((ptrlbc->Zm == zm)
                            AND(ptrlbc->Xm == xm)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }

                    if ((ptrlbc->Zm > zm)
                            OR(ptrlbc->Xm > xm)) {
                        CopyMask(ptrlbc->Brick, colscreen, ptrlbc->Ys, BufferMaskBrick, Screen);
                    }
                }
            }
        }
    }
}

/*--------------------------------------------------------------------------*/

#ifdef LBA2_TARGET_N64
/* TRUE when brick `numbrick` (1-based, as stored in the block) of cell
 * (block, brick) can be drawn from BufferBrick. Otherwise logs which link of
 * the chain is broken and the caller skips the brick: the cell's brick slot
 * (BufCube), the brick id (TabBlock rename) or the bank offset table
 * (BufferBrick itself overwritten). */
static S32 N64BrickDrawable(S32 block, S32 brick, S32 numbrick) {
    static int s_logged = 0;
    const U32 *tab = (const U32 *)BufferBrick;
    const U8 *blk = GetAdrBlock(block);
    U32 slots = (U32)blk[0] * blk[1] * blk[2];
    U32 idx = (U32)(numbrick - 1);
    const char *why = NULL;
    U32 off = 0;

    if ((U32)brick >= slots)
        why = "cell brick slot past the block (BufCube)";
    else if (idx >= s_bankBricks)
        why = "brick id past the bank (TabBlock)";
    else if (tab[0] != (s_bankBricks + 1) * 4 OR tab[s_bankBricks] != s_bankSize)
        why = "bank table header overwritten (BufferBrick)";
    else {
        off = tab[idx];
        if (off < tab[0] OR off >= s_bankSize)
            why = "bank table entry overwritten (BufferBrick)";
    }

    if (why == NULL)
        return TRUE;

    if (s_logged < 16) {
        LogPrintf("[grille] bad brick: %s | cube=%ld block=%ld slot=%ld/%lu numbrick=%ld bank=%lu/%lu tab0=%lu off=%lu\n",
                  why, (long)NumCube, (long)block, (long)brick, (unsigned long)slots, (long)numbrick,
                  (unsigned long)s_bankBricks, (unsigned long)s_bankSize, (unsigned long)tab[0], (unsigned long)off);
        s_logged++;
    }
    return FALSE;
}
#endif

void AffBrickBlock(S32 block, S32 brick, S16 x, S16 y, S16 z) {
    U8 *adr;
    S32 nb, col;
    register T_COLONB *ptrlbc;
    S16 numbrick;
    U8 brickcol;
#ifdef LBA2_TARGET_WIIU
    static int s_abb_logged = 0;
#endif

#ifdef LBA2_TARGET_N64
    // A block index past the library would read block data as an offset and
    // freeze the CPU on a wild address (no bus error recovery on N64). Seen
    // once when a mis-emulated unaligned load fed IncrustGrm a wild GRM index
    // (fixed in docker/Dockerfile.n64); kept as a guard: log and skip the cell.
    if ((U32)block >= (U32)NbBlock) {
        static int s_badLogged = 0;
        if (s_badLogged < 8) {
            LogPrintf("[grille] AffBrickBlock: block=%ld >= NbBlock=%ld at x=%d y=%d z=%d brick=%ld -> skipped\n",
                      (long)block, (long)NbBlock, (int)x, (int)y, (int)z, (long)brick);
            s_badLogged++;
        }
        return;
    }
#endif
    adr = GetAdrBlock(block);
    adr += HEADER_BLOCK;

    adr += (brick << 2); /* 4 Bytes to Jump	*/
    brickcol = *adr;
    adr += 2;

    // numbrick is LE on disk inside TabBlock; without swap AffGraph gets a
    // garbage index and overruns BufferBrick header → RefreshGrille hang.
    numbrick = LBA2_LE16_UA((const void *)adr); /* & 32767 */

#ifdef LBA2_TARGET_WIIU
    if (s_abb_logged < 5) {
        U8 *adr_block = GetAdrBlock(block);
        LogPrintf("[AffBrickBlock] block=%d brick=%d adr_block=%p numbrick=%d brickcol=%d ForbidenBrick=%d\n",
                  (int)block, (int)brick, (void*)adr_block, (int)numbrick,
                  (int)brickcol, (int)ForbidenBrick);
        s_abb_logged++;
    }
#endif

#ifdef LBA2_TARGET_N64
    if (numbrick AND numbrick != ForbidenBrick AND !N64BrickDrawable(block, brick, numbrick))
        return;
#endif

    if (numbrick AND numbrick != ForbidenBrick) {
        Map2Screen(x - StartXCube, y - StartYCube, z - StartZCube);

#ifdef LBA2_TARGET_WIIU
        {
            static int s_m2s_log = 0;
            if (s_m2s_log < 6) {
                LogPrintf("[Map2Screen] in_x=%d y=%d z=%d StartCube={%d,%d,%d} -> XScreen=%d YScreen=%d\n",
                          (int)x, (int)y, (int)z,
                          (int)StartXCube, (int)StartYCube, (int)StartZCube,
                          (int)XScreen, (int)YScreen);
                s_m2s_log++;
            }
        }
#endif

        if (XScreen >= -BRICK_COL_W AND XScreen < BRICK_CLIP_X AND YScreen >= -BRICK_DY AND YScreen < BRICK_CLIP_Y) {
#ifdef LBA2_TARGET_WIIU
            static int s_affgraph_logged = 0;
            if (s_affgraph_logged < 3) {
                U32 *bbtab = (U32*)BufferBrick;
                LogPrintf("[AffBrickBlock] calling AffGraph numgraph=%d xs=%d ys=%d bb_tab[0..3]=0x%08X 0x%08X 0x%08X 0x%08X\n",
                          (int)(numbrick-1), (int)XScreen, (int)YScreen,
                          (unsigned)bbtab[0], (unsigned)bbtab[1],
                          (unsigned)bbtab[2], (unsigned)bbtab[3]);
                s_affgraph_logged++;
            }
#endif
            AffGraphNative(numbrick - 1, XScreen, YScreen, BufferBrick); // [renderscale] physical bank+coords

            col = (XScreen + BRICK_COL_W) / BRICK_COL_W; /* 48 / 2 colonne intercalée */

            nb = NbBrickColon[col];

            ptrlbc = &ListBrickColon[col][nb];

            if (nb < MAX_BRICK) {
                ptrlbc->Xm = x;
                ptrlbc->Ym = y;
                ptrlbc->Zm = z;
                ptrlbc->Ys = (S16)YScreen;
                ptrlbc->Brick = (S16)(numbrick - 1);
                ptrlbc->Col = brickcol;
                NbBrickColon[col]++;
            } else
                Message("Arg MAX_BRICK Z BUFFER atteint", TRUE);
        }
    }
}

/*--------------------------------------------------------------------------*/
/*ptc = BufCube + y*2 + (x*SIZE_CUBE_Y*2) + (z*SIZE_CUBE_X*SIZE_CUBE_Y*2) ;*/

void AffGrille() {
    S32 block;
    U8 *ptc;
    S16 z, y, x;
#ifdef LBA2_TARGET_WIIU
    static U8 affgrille_first_done = 0;
    int total_cells = 0;
    int blocks_seen = 0;
    int affbrickcalls = 0;
    if (!affgrille_first_done) {
        LogPrintf("[trace] AffGrille: entry, Twinsen.Obj={%d,%d,%d} StartCube={%d,%d,%d}\n",
                  (int)ListObjet[0].Obj.X, (int)ListObjet[0].Obj.Y, (int)ListObjet[0].Obj.Z,
                  (int)StartXCube, (int)StartYCube, (int)StartZCube);
        LogPrintf("[trace] AffGrille: entry, BufCube=%p TabBlock=%p BufferBrick=%p ForbidenBrick=%d\n",
                  (void*)BufCube, (void*)TabBlock, (void*)BufferBrick, (int)ForbidenBrick);
        // dump TabBlock state AT this point to detect post-init corruption
        U32 *t = (U32 *)TabBlock;
        LogPrintf("[AffGrille] TabBlock first 32 U32 NOW (in AffGrille):\n");
        for (int k = 0; k < 32; k++) {
            LogPrintf("  [%2d] raw=0x%08X swap=0x%08X\n",
                      k, (unsigned)t[k], (unsigned)LBA2_LE32(t[k]));
        }
        // Also dump first 32 bytes of BufCube to see what blocks are stored
        U8 *bc = BufCube;
        LogPrintf("[AffGrille] BufCube first 32 bytes (z=0,x=0,y=0..15 pairs):\n");
        for (int k = 0; k < 32; k += 16) {
            LogPrintf("  +%02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      k, bc[k], bc[k+1], bc[k+2], bc[k+3], bc[k+4], bc[k+5], bc[k+6], bc[k+7],
                      bc[k+8], bc[k+9], bc[k+10], bc[k+11], bc[k+12], bc[k+13], bc[k+14], bc[k+15]);
        }
    }
#endif

    PtrProjectPoint(0, 0, 0);
    XpOrgw = Xp;
    YpOrgw = Yp;

    memset(NbBrickColon, 0, sizeof(S16) * NB_COLON);

    ptc = BufCube;
    for (z = 0; z < SIZE_CUBE_Z; z++) {
        for (x = 0; x < SIZE_CUBE_X; x++) {
            for (y = 0; y < SIZE_CUBE_Y; y++) {
                block = *ptc++;
#ifdef LBA2_TARGET_WIIU
                total_cells++;
                if (block) {
                    blocks_seen++;
                    if (!affgrille_first_done && affbrickcalls < 5) {
                        LogPrintf("[AffGrille] cell #%d at z=%d x=%d y=%d block=%d brick=%d\n",
                                  total_cells, (int)z, (int)x, (int)y,
                                  (int)block, (int)*ptc);
                        affbrickcalls++;
                    }
                }
#endif

                if (block) {
                    AffBrickBlock(block - 1, *ptc, x, y, z);
                }
                ptc++;
            }
        }
    }

#ifdef LBA2_TARGET_WIIU
    if (!affgrille_first_done) {
        LogPrintf("[trace] AffGrille: exit, total_cells=%d blocks_seen=%d\n",
                  total_cells, blocks_seen);
        affgrille_first_done = 1;
    }
#endif
}

/*══════════════════════════════════════════════════════════════════════════*
	  █     █▀▀▀█ █▀▀█  █  ▄▀ █▀▀▀█  █    ██▄ █ ▀▀█▀▀ █   █ █▀▀▀▀
	  ██    ██▀▀█ ██▀▀█ ██▀   ██▀█▀  ██   ██▀██   ██  ██▀▀█ ██▀▀
	  ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀    ▀▀  ▀  ▀▀   ▀▀  ▀   ▀▀  ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

/*--------------------------------------------------------------------------*/
// Calcule les masques du Z-Buffer pour DrawOverBrick sans dessiner la
// brick
void AffBrickBlockColon(S32 block, S32 brick, S16 x, S16 y, S16 z) {
    U8 *adr;
    S32 nb, col;
    register T_COLONB *ptrlbc;
    S16 numbrick;
    U8 brickcol;

    adr = GetAdrBlock(block);
    adr += HEADER_BLOCK;

    adr += (brick << 2); /* 4 Bytes to Jump	*/
    brickcol = *adr;
    adr += 2;

    numbrick = LBA2_LE16_UA((const void *)adr); /* & 32767 */

#ifdef LBA2_TARGET_N64
    if (numbrick AND numbrick != ForbidenBrick AND !N64BrickDrawable(block, brick, numbrick))
        return;
#endif

    if (numbrick AND numbrick != ForbidenBrick) {
        Map2Screen(x - StartXCube, y - StartYCube, z - StartZCube);

        if (XScreen >= -BRICK_COL_W AND XScreen < BRICK_CLIP_X AND YScreen >= -BRICK_DY AND YScreen < BRICK_CLIP_Y) {
            col = (XScreen + BRICK_COL_W) / BRICK_COL_W; /* 48 / 2 colonne intercalée */

            nb = NbBrickColon[col];

            ptrlbc = &ListBrickColon[col][nb];

            if (nb < MAX_BRICK) {
                ptrlbc->Xm = x;
                ptrlbc->Ym = y;
                ptrlbc->Zm = z;
                ptrlbc->Ys = (S16)YScreen;
                ptrlbc->Brick = (S16)(numbrick - 1);
                ptrlbc->Col = brickcol;
                NbBrickColon[col]++;

            } else
                Message("Arg MAX_BRICK Z BUFFER atteint", TRUE);
        }
    }
}

// Calcule le Z-Buffer pour le DrawOverBrick de toute la grille sans
// afficher les bricks
void AffGrilleOnly() {
    S32 block;
    U8 *ptc;
    U8 *adr;
    S16 z, y, x;
    U8 codejeu;

    PtrProjectPoint(0, 0, 0);
    XpOrgw = Xp;
    YpOrgw = Yp;

    memset(NbBrickColon, 0, sizeof(S16) * NB_COLON);

    ptc = BufCube;

    for (z = 0; z < SIZE_CUBE_Z; z++) {
        for (x = 0; x < SIZE_CUBE_X; x++) {
            for (y = 0; y < SIZE_CUBE_Y; y++) {
                block = *ptc++;

                if (block) {
                    block--;

                    //-------------------
                    adr = GetAdrBlock(block);
                    adr += (HEADER_BLOCK + 1);

                    codejeu = (U8)((*adr) >> 4);

                    if (codejeu != CJ_LABYRINTHE)
                    //-------------------
                    {
                        AffBrickBlockColon(block, *ptc, x, y, z);
                    }
                }
                ptc++;
            }
        }
    }
}
/*--------------------------------------------------------------------------*/
// Affiche la brick sans calculer le Z-Buffer pour DrawOverBrick
void AffBrickBlockOnly(S32 block, S32 brick, S16 x, S16 y, S16 z) {
    U8 *adr;
    S32 numbrick;

    adr = GetAdrBlock(block);
    adr += HEADER_BLOCK;

    adr += (brick << 2); /* 4 Bytes to Jump	*/
    adr += 2;

    numbrick = LBA2_LE16_UA((const void *)adr); /* & 32767 */

#ifdef LBA2_TARGET_N64
    if (numbrick AND numbrick != ForbidenBrick AND !N64BrickDrawable(block, brick, numbrick))
        return;
#endif

    if (numbrick AND numbrick != ForbidenBrick) {
        Map2Screen(x - StartXCube, y - StartYCube, z - StartZCube);

        if (XScreen >= -BRICK_COL_W AND XScreen < BRICK_CLIP_X AND YScreen >= -BRICK_DY AND YScreen < BRICK_CLIP_Y) {
            AffGraphNative(numbrick - 1, XScreen, YScreen, BufferBrick); // [renderscale] physical bank+coords
        }
    }
}

// Redessine uniquement les block n'ayant pas le code jeu Invisible
void AffGrilleLabyrinthe() {
    S32 block;
    U8 *ptc;
    U8 *adr;
    S16 z, y, x;
    U8 codejeu;

    PtrProjectPoint(0, 0, 0);
    XpOrgw = Xp;
    YpOrgw = Yp;

    ptc = BufCube;

    for (z = 0; z < SIZE_CUBE_Z; z++) {
        for (x = 0; x < SIZE_CUBE_X; x++) {
            for (y = 0; y < SIZE_CUBE_Y; y++) {
                block = *ptc++;

                if (block) {
                    block--;

                    adr = GetAdrBlock(block);
                    adr += (HEADER_BLOCK + 1);

                    codejeu = (U8)((*adr) >> 4);

                    if (codejeu != CJ_LABYRINTHE) {
                        AffBrickBlockOnly(block, *ptc, x, y, z);
                    }
                }
                ptc++;
            }
        }
    }
}

// Affiche un cube de block (3*3*3) autour du point passe (coordonnees world)
void AffOneBrick(S32 x, S32 y, S32 z) {
    S32 block;
    U8 *ptc;
    U8 *adr;
    S32 xmin, ymin, xmax, ymax;
    S16 xs, ys, zs;
    S16 zdeb;
    U8 codejeu;

    xmin = ModeDesiredX - 1; // virtual: compared with ScreenXMin.. below
    ymin = ModeDesiredY - 1;
    xmax = 0;
    ymax = 0;

    PtrProjectPoint(0, 0, 0);
    XpOrgw = Xp;
    YpOrgw = Yp;

    x /= SIZE_BRICK_XZ;
    y /= SIZE_BRICK_Y;
    z /= SIZE_BRICK_XZ;

    zdeb = (S16)(z - 1);
    if (zdeb < 0)
        zdeb = 0;

    for (zs = zdeb; zs <= z + 1; zs++) {
        for (xs = (S16)(x - 1); xs <= (S16)(x + 1); xs++) {
            ptc = GetAdrColonneCube(xs, zs);
            ptc += (y - 1) * 2;
            for (ys = (S16)(y - 1); ys <= (S16)(y + 1); ys++) {
                block = *ptc++;
                if (block) {
                    block--;

                    adr = GetAdrBlock(block);
                    adr += (HEADER_BLOCK + 1);

                    codejeu = (U8)((*adr) >> 4);

                    if (codejeu == CJ_LABYRINTHE) {
                        AffBrickBlockOnly(block, *ptc, xs, ys, zs);
                        if (ScreenXMin < xmin)
                            xmin = ScreenXMin;
                        if (ScreenXMax > xmax)
                            xmax = ScreenXMax;
                        if (ScreenYMin < ymin)
                            ymin = ScreenYMin;
                        if (ScreenYMax > ymax)
                            ymax = ScreenYMax;
                    }
                }
                ptc++;
            }
        }
    }

    SetClip(xmin, ymin, xmax, ymax);
    DrawRecover((x + 1) * SIZE_BRICK_XZ, y * SIZE_BRICK_Y, (z + 1) * SIZE_BRICK_XZ, 0, 0);
}

// Fin Labyrinthe

/*--------------------------------------------------------------------------*/

void IncrustGrm(T_ZONE *ptrz) {
    S32 numgrm, xo, yo, zo;
    U8 *pts, *ptd;
    S32 x, z;
    S32 dx, dy, dz;
    char tmpFilePath[ADELINE_MAX_PATH];

    numgrm = ptrz->Info0;

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, BKG_HQR_NAME);
    Load_HQR(tmpFilePath, Screen,
             BkgHeader.Grm_Start + GriHeader->My_Grm + numgrm);

    xo = ptrz->X0 / SIZE_BRICK_XZ;
    yo = ptrz->Y0 / SIZE_BRICK_Y;
    zo = ptrz->Z0 / SIZE_BRICK_XZ;

    pts = (U8 *)Screen;

    dx = *pts++;
    dy = *pts++;
    dz = *pts++;

    yo <<= 1; //	Cause S16
    dy <<= 1; //	Cause S16

    for (z = zo; z < zo + dz; z++) {
        for (x = xo; x < xo + dx; x++) {
            ptd = GetAdrColonneCube(x, z);
            ptd += yo;
            memcpy(ptd, pts, dy);
            pts += dy;
        }
    }

    FirstTime = AFF_ALL_FLIP;
}

/*--------------------------------------------------------------------------*/
void DesIncrustGrm(T_ZONE *ptrz) {
    U8 *pts, *ptd;
    U8 col[50]; //	1 Colonne decompressee
    S32 x, y, z, dy;
    S32 z0, z1, x0, x1;
    S32 y0, y1;

    x0 = ptrz->X0 / SIZE_BRICK_XZ;
    x1 = ptrz->X1 / SIZE_BRICK_XZ;

    y0 = ptrz->Y0 / SIZE_BRICK_Y;
    y1 = ptrz->Y1 / SIZE_BRICK_Y;

    z0 = ptrz->Z0 / SIZE_BRICK_XZ;
    z1 = ptrz->Z1 / SIZE_BRICK_XZ;

    y = y0 << 1; //	Cause S16

    dy = ((y1 - y0) + 1) << 1;

    /*-------------- Copy -------------------------*/
    for (z = z0; z <= z1; z++) {
        for (x = x0; x <= x1; x++) {
            pts = GetAdrColonneMap(x, z);
            DecompColonne(pts, col);

            ptd = GetAdrColonneCube(x, z);
            memcpy(ptd + y, col + y, dy);
        }
    }

    FirstTime = AFF_ALL_FLIP;
}

/*--------------------------------------------------------------------------*/
void RedrawGRMs(void) {
    S32 n;
    T_ZONE *ptrz = ListZone;

    for (n = 0; n < NbZones; n++, ptrz++) {
        switch (ptrz->Type) {
        case 3: // GRM
            if (ptrz->Info2) {
                IncrustGrm(ptrz);
            }
            break;
        }
    }
}
/*--------------------------------------------------------------------------*/

#include <stdio.h>

static S32 logical_shift_right_s32(S32 value, U32 bits) {
    return (S32)(((U32)value) >> bits);
}

static S32 arithmetic_shift_right_8(S32 value) {
    if (value >= 0) {
        return value >> 8;
    }

    return (S32)(-(((-(long long)value) + 255LL) >> 8));
}

U8 WorldColBrick(S32 xw, S32 yw, S32 zw) {
    XMap = logical_shift_right_s32(xw, 9);
    YMap = logical_shift_right_s32(yw, 8);
    ZMap = logical_shift_right_s32(zw, 9);

    if (XMap < 0 || XMap >= 64) {
        return 0;
    }

    int cubeIndex = (XMap * 50);

    if (YMap <= -1) {
        YMap = 0;
        return 1;
    } else if (YMap < 0 || YMap >= 25) {
        return 0;
    }

    cubeIndex += YMap * 2;

    if (ZMap < 0 || ZMap >= 64) {
        return 0;
    }

    cubeIndex += ZMap * 3200;

    U8 value = BufCube[cubeIndex];

    if (value == 0) {
        return BufCube[cubeIndex + 1];
    }

    U32 numBlock = value - 1;
    U8 *addr = GetAdrBlock(numBlock);
    addr += HEADER_BLOCK;
    addr += BufCube[cubeIndex + 1] * 4;

    return *addr;
}

//-------------------------------------------------------------------
U8 WorldColBrickFull(S32 xw, S32 yw, S32 zw, S32 ymax) {
    XMap = logical_shift_right_s32(xw, 9);
    YMap = arithmetic_shift_right_8(yw);
    ZMap = logical_shift_right_s32(zw, 9);

    if (XMap < 0 || XMap >= SIZE_CUBE_X)
        return 0;

    if (YMap <= -1) {
        return 1;
    }

    if (YMap >= SIZE_CUBE_Y)
        return 0;

    if (ZMap < 0 || ZMap >= SIZE_CUBE_Z)
        return 0;

    U32 colIdx = XMap * 50 + YMap * 2 + ZMap * 3200;

    U8 col = BufCube[colIdx];
    if (col == 0) {
        // eventuelle col transparente
        col = BufCube[colIdx + 1];

        // check full height
        U32 something = ((U32)(ymax + 255)) >> 8;
        S32 scanY = YMap;
        do {
            if (scanY >= SIZE_CUBE_Y - 1) {
                return col;
            }

            colIdx += 2;
            ++scanY;

            if (BufCube[colIdx] != 0 || BufCube[colIdx + 1] != 0) {
                return 1;
            }
            something--;
        } while (something != 0);

        return col;
    }

    S32 block = BufCube[colIdx] - 1;
    U8 *adr = GetAdrBlock(block);
    adr += HEADER_BLOCK;
    adr += BufCube[colIdx + 1] * 4; // 4 Bytes to Jump
    col = *adr;

    // check full height
    U32 something = ((U32)(ymax + 255)) >> 8;
    S32 scanY = YMap;
    do {
        if (scanY >= SIZE_CUBE_Y - 1) {
            return col;
        }

        colIdx += 2;
        ++scanY;

        if (BufCube[colIdx] != 0 || BufCube[colIdx + 1] != 0) {
            return 1;
        }
        something--;
    } while (something != 0);

    return col;
}

//-------------------------------------------------------------------
U8 WorldCodeBrick(S32 xw, S32 yw, S32 zw) {
    XMap = logical_shift_right_s32(xw, 9);

    U8 code = 0;

    if (yw > -1) {
        U8 *ptc = BufCube;

        YMap = logical_shift_right_s32(yw, 8);
        ZMap = logical_shift_right_s32(zw, 9);

        ptc += XMap * 50 + YMap * 2 + ZMap * 3200;

        U8 col = *ptc;
        if (col) {
            U8 *adr = GetAdrBlock(col - 1);
            adr += HEADER_BLOCK;
            code = *(adr + 1);
        }
    }
    return code;
}

//-------------------------------------------------------------------
U8 GetBlockBrick(S32 xw, S32 yw, S32 zw) {
    XMap = logical_shift_right_s32(xw, 9);
    YMap = logical_shift_right_s32(yw, 8);
    ZMap = logical_shift_right_s32(zw, 9);

    if (XMap < 0 || XMap >= 64) {
        return 0; // ovfw
    }

    if (YMap <= -1) {
        YMap = 0;
        return 1; // col_1
    }

    if (YMap < 0 || YMap >= 25) {
        return 0; // ovfw
    }

    if (ZMap < 0 || ZMap >= 64) {
        return 0; // ovfw
    }

    U32 offset = XMap * 50 + YMap * 2 + ZMap * 3200;
    U8 *ptc = BufCube + offset;

    return *ptc;
}

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
U8 GetWorldColBrickVisible(S32 x, S32 y, S32 z) {
    XMap = logical_shift_right_s32(x, 9);
    YMap = logical_shift_right_s32(y, 8);
    ZMap = logical_shift_right_s32(z, 9);

    if (XMap >= 0 && XMap < 64) {
        if (YMap <= -1) {
            YMap = 0;
            return 1;
        }
        if (YMap >= 0 && YMap < 25 && ZMap >= 0 && ZMap < 64) {
            U8 *ptc = 3200 * ZMap + 50 * XMap + BufCube + 2 * YMap;

            if (*ptc != 0) {
                U8 block = (*ptc++) - 1;
                U8 *adr = GetAdrBlock(block);
                adr += HEADER_BLOCK;
                adr += (*ptc) << 2; /* 4 Bytes to Jump	*/
                return *adr;
            }
        }
    }

    return 0;
}

//-------------------------------------------------------------------
void DecompColonne(U8 *pts, U8 *ptd) {
    U8 *dstData = ptd;
    U8 count = 0;
    U8 *srcData = pts;

    // Get Nb Entity
    U8 nbEntity = *srcData;
    srcData++;

    // For Every Entity
    do {
        // NewEntity:
        while (1) {
            U8 opCode = 0;
            while (1) {
                opCode = *srcData++;
                count = (opCode & 0x3F) + 1;
                // Incrust?
                if (opCode & 0xC0) {
                    break;
                }
                // Incrust ( Full off Nothing )
                opCode = 0;
                while (count) {
                    dstData[0] = 0;
                    dstData[1] = 0;
                    dstData += 2;
                    --count;
                }
                --nbEntity;
                if (!nbEntity) {
                    return;
                }
            }
            // Diff brick ?
            if (opCode & 0x40) {
                break;
            }
            U8 numBlock = srcData[0];
            U8 posBlock = srcData[1];
            srcData += 2;
            while (count) {
                dstData[0] = numBlock;
                dstData[1] = posBlock;
                dstData += 2;
                --count;
            }
            if (!--nbEntity) {
                return;
            }
        }
        while (count) {
            dstData[0] = srcData[0];
            dstData[1] = srcData[1];
            srcData += 2;
            dstData += 2;
            --count;
        }
        --nbEntity;
    } while (nbEntity);
}

//-------------------------------------------------------------------
void Map2Screen(S32 x, S32 y, S32 z) {
#ifdef LBA2_TARGET_N64
    // [renderscale] half-res iso layer: floor(full/2). X stays a multiple
    // of 12 so the column quantization in AffBrickBlock/DrawOverBrick still
    // reconstructs XScreen exactly (col*12-12).
    XScreen = 12 * (x - z) + 144;
    YScreen = (12 * (z + x) - 15 * y + 226) >> 1;
#else
    XScreen = 24 * (x - z) + 288;
    YScreen = 12 * (z + x) - 15 * y + 226;
#endif
}

//-------------------------------------------------------------------
U8 *GetAdrBlock(S32 numblock) {
    // TabBlock leading U32 offset table is LE on disk.
    return TabBlock + LBA2_LE32(((U32 *)TabBlock)[numblock]);
}
