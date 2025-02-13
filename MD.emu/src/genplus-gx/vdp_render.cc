/***************************************************************************************
 *  Genesis Plus
 *  Video Display Processor (Mode 4 & Mode 5 rendering)
 *
 *  Copyright (C) 1998, 1999, 2000, 2001, 2002, 2003  Charles Mac Donald (original code)
 *  Eke-Eke (2007-2011), additional code & fixes for the GCN/Wii port
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 ****************************************************************************************/

#include "shared.h"
#include "vdp_render.h"
#include <imagine/pixmap/Pixmap.hh>

#ifdef NGC
#include "md_ntsc.h"
#include "sms_ntsc.h"

/*** NTSC Filters ***/
extern md_ntsc_t *md_ntsc;
extern sms_ntsc_t *sms_ntsc;
#endif

/* Pixel priority look-up tables information */
#define LUT_MAX     (6)
#define LUT_SIZE    (0x10000)

#if __ARM_ARCH < 6 || (defined __ANDROID__ && __ARM_ARCH < 7)
#define ALIGN_LONG
#endif
#define ALT_RENDERER

// 32-bit type for VDP writes to prevent generating code
// using instructions that assume 4-byte alignment.
// Fixes SIGBUS issues on ARM targets.
using uint32align1 [[gnu::aligned(1)]] = uint32;
using uint32align2 [[gnu::aligned(2)]] = uint32;

#ifdef ALIGN_LONG
/* Or change the names if you depend on these from elsewhere.. */
#undef READ_LONG
#undef WRITE_LONG

static __inline__ uint32 READ_LONG(void *address)
{
  if ((uintptr_t)address & 3)
  {
#ifdef LSB_FIRST  /* little endian version */
    return ( *((uint8 *)address) +
        (*((uint8 *)address+1) << 8)  +
        (*((uint8 *)address+2) << 16) +
        (*((uint8 *)address+3) << 24) );
#else       /* big endian version */
    return ( *((uint8 *)address+3) +
        (*((uint8 *)address+2) << 8)  +
        (*((uint8 *)address+1) << 16) +
        (*((uint8 *)address)   << 24) );
#endif  /* LSB_FIRST */
  }
  else return *(uint32 *)address;
}

static __inline__ void WRITE_LONG(void *address, uint32 data)
{
  if ((uintptr_t)address & 3)
  {
#ifdef LSB_FIRST
      *((uint8 *)address) =  data;
      *((uint8 *)address+1) = (data >> 8);
      *((uint8 *)address+2) = (data >> 16);
      *((uint8 *)address+3) = (data >> 24);
#else
      *((uint8 *)address+3) =  data;
      *((uint8 *)address+2) = (data >> 8);
      *((uint8 *)address+1) = (data >> 16);
      *((uint8 *)address)   = (data >> 24);
#endif /* LSB_FIRST */
    return;
  }
  else *(uint32 *)address = data;
}

#endif  /* ALIGN_LONG */


/* Draw 2-cell column (8-pixels high) */
/*
   Pattern cache base address: VHN NNNNNNNN NNYYYxxx
   with :
      x = Pattern Pixel (0-7)
      Y = Pattern Row (0-7)
      N = Pattern Number (0-2047) from pattern attribute
      H = Horizontal Flip bit from pattern attribute
      V = Vertical Flip bit from pattern attribute
*/
#define GET_LSB_TILE(ATTR, LINE) \
  atex = atex_table[(ATTR >> 13) & 7]; \
  src = (uint32 *)&bg_pattern_cache[(ATTR & 0x00001FFF) << 6 | (LINE)];
#define GET_MSB_TILE(ATTR, LINE) \
  atex = atex_table[(ATTR >> 29) & 7]; \
  src = (uint32 *)&bg_pattern_cache[(ATTR & 0x1FFF0000) >> 10 | (LINE)];

/* Draw 2-cell column (16 pixels high) */
/*
   Pattern cache base address: VHN NNNNNNNN NYYYYxxx
   with :
      x = Pattern Pixel (0-7)
      Y = Pattern Row (0-15)
      N = Pattern Number (0-1023)
      H = Horizontal Flip bit
      V = Vertical Flip bit
*/
#define GET_LSB_TILE_IM2(ATTR, LINE) \
  atex = atex_table[(ATTR >> 13) & 7]; \
  src = (uint32 *)&bg_pattern_cache[((ATTR & 0x000003FF) << 7 | (ATTR & 0x00001800) << 6 | (LINE)) ^ ((ATTR & 0x00001000) >> 6)];
#define GET_MSB_TILE_IM2(ATTR, LINE) \
  atex = atex_table[(ATTR >> 29) & 7]; \
  src = (uint32 *)&bg_pattern_cache[((ATTR & 0x03FF0000) >> 9 | (ATTR & 0x18000000) >> 10 | (LINE)) ^ ((ATTR & 0x10000000) >> 22)];

/*   
   One column = 2 tiles
   Two pattern attributes are written in VRAM as two consecutives 16-bit words:

   P = priority bit
   C = color palette (2 bits)
   V = Vertical Flip bit
   H = Horizontal Flip bit
   N = Pattern Number (11 bits)

   (MSB) PCCVHNNN NNNNNNNN (LSB) (MSB) PCCVHNNN NNNNNNNN (LSB)
              PATTERN1                      PATTERN2

   Both pattern attributes are read from VRAM as one 32-bit word:

   LIT_ENDIAN: (MSB) PCCVHNNN NNNNNNNN PCCVHNNN NNNNNNNN (LSB)
                          PATTERN2          PATTERN1

   BIG_ENDIAN: (MSB) PCCVHNNN NNNNNNNN PCCVHNNN NNNNNNNN (LSB)
                          PATTERN1          PATTERN2


   In line buffers, one pixel = one byte: (msb) 0Pppcccc (lsb)
   with:
      P = priority bit  (from pattern attribute)
      p = color palette (from pattern attribute)
      c = color data (from pattern cache)

   One pattern = 8 pixels = 8 bytes = two 32-bit writes per pattern
*/

#ifdef ALIGN_LONG
#ifdef LSB_FIRST
#define DRAW_COLUMN(ATTR, LINE) \
  GET_LSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++; \
  GET_MSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++;
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++; \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++;
#else
#define DRAW_COLUMN(ATTR, LINE) \
  GET_MSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++; \
  GET_LSB_TILE(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++;
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++; \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  WRITE_LONG(dst, src[0] | atex); \
  dst++; \
  WRITE_LONG(dst, src[1] | atex); \
  dst++;
#endif
#else /* NOT ALIGNED */
#ifdef LSB_FIRST
#define DRAW_COLUMN(ATTR, LINE) \
  GET_LSB_TILE(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex); \
  GET_MSB_TILE(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex);
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex); \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex);
#else
#define DRAW_COLUMN(ATTR, LINE) \
  GET_MSB_TILE(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex); \
  GET_LSB_TILE(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex);
#define DRAW_COLUMN_IM2(ATTR, LINE) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex); \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  *dst++ = (src[0] | atex); \
  *dst++ = (src[1] | atex);
#endif
#endif /* ALIGN_LONG */

#ifdef ALT_RENDERER
/* Draw background tiles directly using priority look-up table */
/* SRC_A = layer A rendered pixel line (4 bytes = 4 pixels at once) */
/* SRC_B = layer B cached pixel line (4 bytes = 4 pixels at once) */
/* Note: cache address is always aligned so no need to use READ_LONG macro */
/* This might be faster or slower than original method, depending on  */
/* architecture (x86, PowerPC), cache size, memory access speed, etc...  */

#ifdef LSB_FIRST 
#define DRAW_BG_TILE(SRC_A, SRC_B) \
  *lb++ = table[((SRC_B << 8) & 0xff00) | (SRC_A & 0xff)]; \
  *lb++ = table[(SRC_B & 0xff00) | ((SRC_A >> 8) & 0xff)]; \
  *lb++ = table[((SRC_B >> 8) & 0xff00) | ((SRC_A >> 16) & 0xff)]; \
  *lb++ = table[((SRC_B >> 16) & 0xff00) | ((SRC_A >> 24) & 0xff)];
#else
#define DRAW_BG_TILE(SRC_A, SRC_B) \
  *lb++ = table[((SRC_B >> 16) & 0xff00) | ((SRC_A >> 24) & 0xff)]; \
  *lb++ = table[((SRC_B >> 8) & 0xff00) | ((SRC_A >> 16) & 0xff)]; \
  *lb++ = table[(SRC_B & 0xff00) | ((SRC_A >> 8) & 0xff)]; \
  *lb++ = table[((SRC_B << 8) & 0xff00) | (SRC_A & 0xff)];
#endif

#ifdef ALIGN_LONG
#ifdef LSB_FIRST 
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#else
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) 
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = READ_LONG((uint32 *)lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#endif
#else /* NOT ALIGNED */
#ifdef LSB_FIRST 
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#else
#define DRAW_BG_COLUMN(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#define DRAW_BG_COLUMN_IM2(ATTR, LINE, SRC_A, SRC_B) \
  GET_MSB_TILE_IM2(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  GET_LSB_TILE_IM2(ATTR, LINE) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[0] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B) \
  SRC_A = *(uint32align2 *)(lb); \
  SRC_B = (src[1] | atex); \
  DRAW_BG_TILE(SRC_A, SRC_B)
#endif
#endif /* ALIGN_LONG */
#endif /* ALT_RENDERER */

#define DRAW_SPRITE_TILE(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i++) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      status |= ((temp & 0x8000) >> 10); \
    } \
  }

#define DRAW_SPRITE_TILE_ACCURATE(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i++) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 13) >> 1); \
        status |= 0x20; \
      } \
    } \
  }

#define DRAW_SPRITE_TILE_ACCURATE_2X(WIDTH,ATTR,TABLE)  \
  for (i=0;i<WIDTH;i+=2) \
  { \
    temp = *src++; \
    if (temp & 0x0f) \
    { \
      temp |= (lb[i] << 8); \
      lb[i] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 13) >> 1); \
        status |= 0x20; \
      } \
      temp &= 0x00FF; \
      temp |= (lb[i+1] << 8); \
      lb[i+1] = TABLE[temp | ATTR]; \
      if ((temp & 0x8000) && !(status & 0x20)) \
      { \
        spr_col = (v_counter << 8) | ((xpos + i + 1 + 13) >> 1); \
        status |= 0x20; \
      } \
    } \
  }

/* Pixel conversion */

using Pixel = std::conditional_t<RENDER_BPP == 32, uint32_t, uint16_t>;
static IG::PixelFormat fbRenderFormat{RENDER_BPP == 32 ? IG::PIXEL_RGBA8888 : IG::PIXEL_RGB565};

static Pixel MAKE_PIXEL(int r, int g, int b)
{
	if constexpr(RENDER_BPP == 32)
	{
		auto desc = fbRenderFormat == IG::PIXEL_BGRA8888 ? IG::PIXEL_DESC_BGRA8888.nativeOrder() : IG::PIXEL_DESC_RGBA8888_NATIVE;
		return desc.build(r << 4 | r, g << 4 | g, b << 4 | b, 0);
	}
	else
	{
		return IG::PIXEL_DESC_RGB565.build(r << 1 | (r >> 3), g << 2 | (g >> 2), b << 1 | (b >> 3), 0);
	}
}

/* Window & Plane A clipping */
static struct clip_t
{
  uint8 left;
  uint8 right;
  uint8 enable;
} clip[2];

/* Pattern attribute (priority + palette bits) expansion table */
static const uint32 atex_table[] =
{
  0x00000000,
  0x10101010,
  0x20202020,
  0x30303030,
  0x40404040,
  0x50505050,
  0x60606060,
  0x70707070
};

/* Sprite pattern name offset look-up table (Mode 5) */
static uint8 name_lut[0x400];

/* Bitplane to packed pixel look-up table (Mode 4) */
static uint32 bp_lut[0x10000];

/* Layer priority pixel look-up tables */
static uint8 lut[LUT_MAX][LUT_SIZE];

static Pixel pixel[0x100];
static Pixel pixel_lut[3][0x200];
static Pixel pixel_lut_m4[0x40];

/* Background & Sprite line buffers */
static uint8 linebuf[2][0x200];

/* Sprite limit flag */
static uint8 spr_ovr;

/* Sprites parsing */
static struct 
{
  uint16 ypos;
  uint16 xpos;
  uint16 attr;
  uint16 size;
} object_info[20];

uint8 object_count;

/* Sprite Collision Info */
uint16 spr_col;

/* Function pointers */
void (*render_bg)(int line, int width);
void (*render_obj)(int max_width);
void (*parse_satb)(int line);
void (*update_bg_pattern_cache)(int index);

/*--------------------------------------------------------------------------*/
/* Sprite pattern name offset look-up table function (Mode 5)               */
/*--------------------------------------------------------------------------*/

static void make_name_lut(void)
{
  int vcol, vrow;
  int width, height;
  int flipx, flipy;
  int i;

  for (i = 0; i < 0x400; i += 1)
  {
    /* Sprite settings */
    vcol = i & 3;
    vrow = (i >> 2) & 3;
    height = (i >> 4) & 3;
    width  = (i >> 6) & 3;
    flipx  = (i >> 8) & 1;
    flipy  = (i >> 9) & 1;

    if ((vrow > height) || vcol > width)
    {
      /* Invalid settings (unused) */
      name_lut[i] = -1; 
    }
    else
    {
      /* Adjust column & row index if sprite is flipped */
      if(flipx) vcol = (width - vcol);
      if(flipy) vrow = (height - vrow);

      /* Pattern offset (pattern order is up->down->left->right) */
      name_lut[i] = vrow + (vcol * (height + 1));
    }
  }
}


/*--------------------------------------------------------------------------*/
/* Bitplane to packed pixel look-up table function (Mode 4)                 */
/*--------------------------------------------------------------------------*/

static void make_bp_lut(void)
{
  int x,i,j;
  uint32 out;

  /* ---------------------- */
  /* Pattern color encoding */
  /* -------------------------------------------------------------------------*/
  /* 4 byteplanes are required to define one pattern line (8 pixels)          */
  /* A single pixel color is coded with 4 bits (c3 c2 c1 c0)                  */
  /* Each bit is coming from byteplane bits, as explained below:              */
  /* pixel 0: c3 = bp3 bit 7, c2 = bp2 bit 7, c1 = bp1 bit 7, c0 = bp0 bit 7  */
  /* pixel 1: c3 = bp3 bit 6, c2 = bp2 bit 6, c1 = bp1 bit 6, c0 = bp0 bit 6  */
  /* ...                                                                      */
  /* pixel 7: c3 = bp3 bit 0, c2 = bp2 bit 0, c1 = bp1 bit 0, c0 = bp0 bit 0  */
  /* -------------------------------------------------------------------------*/

  for(i = 0; i < 0x100; i++)
  for(j = 0; j < 0x100; j++)
  {
    out = 0;
    for(x = 0; x < 8; x++)
    {
      /* pixel line data = hh00gg00ff00ee00dd00cc00bb00aa00 (32-bit) */
      /* aa-hh = upper or lower 2-bit values of pixels 0-7 (shifted) */
      out |= (j & (0x80 >> x)) ? (uint32)(8 << (x << 2)) : 0;
      out |= (i & (0x80 >> x)) ? (uint32)(4 << (x << 2)) : 0;
    }

    /* i = low byte in VRAM  (bp0 or bp2) */
    /* j = high byte in VRAM (bp1 or bp3) */
 #ifdef LSB_FIRST
    bp_lut[(j << 8) | (i)] = out;
 #else
    bp_lut[(i << 8) | (j)] = out;
 #endif
   }
}


/*--------------------------------------------------------------------------*/
/* Layers priority pixel look-up tables functions                           */
/*--------------------------------------------------------------------------*/

/* Input (bx):  d5-d0=color, d6=priority, d7=unused */
/* Input (ax):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=zero */
static uint32 make_lut_bg(uint32 bx, uint32 ax)
{
  int bf = (bx & 0x7F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);
  
  int af = (ax & 0x7F);   
  int ap = (ax & 0x40);
  int a  = (ax & 0x0F);

  int c = (ap ? (a ? af : bf) : (bp ? (b ? bf : af) : (a ? af : bf)));

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c);
}

/* Input (bx):  d5-d0=color, d6=priority, d7=unused */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=intensity select (0=half/1=normal) */
static uint32 make_lut_bg_ste(uint32 bx, uint32 ax)
{
  int bf = (bx & 0x7F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);
  
  int af = (ax & 0x7F);   
  int ap = (ax & 0x40);
  int a  = (ax & 0x0F);

  int c = (ap ? (a ? af : bf) : (bp ? (b ? bf : af) : (a ? af : bf)));

  /* Half intensity when both pixels are low priority */
  c |= ((ap | bp) << 1);

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c);
}

/* Input (bx):  d5-d0=color, d6=priority/1, d7=sprite pixel marker */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=priority, d7=sprite pixel marker */
static uint32 make_lut_obj(uint32 bx, uint32 sx)
{
  int bf = (bx & 0x7F);
  int bs = (bx & 0x80);
  int sf = (sx & 0x7F);

  if((sx & 0x0F) == 0) return bx;

  int c = (bs ? bf : sf);

  /* Strip palette bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0xC0;

  return (c | 0x80);
}


/* Input (bx):  d5-d0=color, d6=priority, d7=opaque sprite pixel marker */
/* Input (sx):  d5-d0=color, d6=priority, d7=unused */
/* Output:    d5-d0=color, d6=zero/priority, d7=opaque sprite pixel marker */
static uint32 make_lut_bgobj(uint32 bx, uint32 sx)
{
  int bf = (bx & 0x3F);
  int bs = (bx & 0x80);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);
  
  int sf = (sx & 0x3F);
  int sp = (sx & 0x40);
  int s  = (sx & 0x0F);

  if(s == 0) return bx;

  /* Previous sprite has higher priority */
  if(bs) return bx;

  int c = (sp ? sf : (bp ? (b ? bf : sf) : sf));

  /* Strip palette & priority bits from transparent pixels */
  if((c & 0x0F) == 0x00) c &= 0x80;

  return (c | 0x80);
}

/* Input (bx):  d5-d0=color, d6=priority, d7=intensity (half/normal) */
/* Input (sx):  d5-d0=color, d6=priority, d7=sprite marker */
/* Output:    d5-d0=color, d6=intensity (half/normal), d7=(double/invalid) */
static uint32 make_lut_bgobj_ste(uint32 bx, uint32 sx)
{
  int c;

  int bf = (bx & 0x3F);
  int bp = (bx & 0x40);
  int b  = (bx & 0x0F);
  int bi = (bx & 0x80) >> 1;

  int sf = (sx & 0x3F);
  int sp = (sx & 0x40);
  int s  = (sx & 0x0F);
  int si = sp | bi;

  if(sp)
  {
    if(s)
    {
      if((sf & 0x3E) == 0x3E)
      {
        if(sf & 1)
        {
          c = (bf | 0x00);
        }
        else
        {
          c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
        }
      }
      else
      {
        if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
        {
          c = (sf | 0x40);
        }
        else
        {
          c = (sf | si);
        }
      }
    }
    else
    {
      c = (bf | bi);
    }
  }
  else
  {
    if(bp)
    {
      if(b)
      {
        c = (bf | bi);
      }
      else
      {
        if(s)
        {
          if((sf & 0x3E) == 0x3E)
          {
            if(sf & 1)
            {
              c = (bf | 0x00);
            }
            else
            {
              c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
            }
          }
          else
          {
            if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
            {
              c = (sf | 0x40);
            }
            else
            {
              c = (sf | si);
            }
          }
        }
        else
        {
          c = (bf | bi);
        }
      }
    }
    else
    {
      if(s)
      {
        if((sf & 0x3E) == 0x3E)
        {
          if(sf & 1)
          {
            c = (bf | 0x00);
          }
          else
          {
            c = (bx & 0x80) ? (bf | 0x80) : (bf | 0x40);
          }
        }
        else
        {
          if(sf == 0x0E || sf == 0x1E || sf == 0x2E)
          {
            c = (sf | 0x40);
          }
          else
          {
            c = (sf | si);
          }
        }
      }
      else
      {          
        c = (bf | bi);
      }
    }
  }

  if((c & 0x0f) == 0x00) c &= 0xC0;

  return (c);
}

/* Input (bx):  d3-d0=color, d4=palette, d5=priority, d6=zero, d7=sprite pixel marker */
/* Input (sx):  d3-d0=color, d7-d4=zero */
/* Output:      d3-d0=color, d4=palette, d5=zero/priority, d6=zero, d7=sprite pixel marker */
static uint32 make_lut_bgobj_m4(uint32 bx, uint32 sx)
{
  int bf = (bx & 0x3F);
  int bs = (bx & 0x80);
  int bp = (bx & 0x20);
  int b  = (bx & 0x0F);

  int s  = (sx & 0x0F);
  int sf = (s | 0x10); /* force palette bit */

  if(s == 0) return bx;

  /* Previous sprite has higher priority */
  if(bs) return bx;

  int c = (bp ? (b ? bf : sf) : sf);

  return (c | 0x80);
}


/*--------------------------------------------------------------------------*/
/* Pixel layer merging function                                             */
/*--------------------------------------------------------------------------*/

static inline void merge(uint8 *srca, uint8 *srcb, uint8 *dst, uint8 *table, int width)
{
  do
  {
    *dst++ = table[(*srcb++ << 8) | (*srca++)];
  }
  while (--width);
}

/*--------------------------------------------------------------------------*/
/* Pixel color lookup tables initialization                                 */
/*--------------------------------------------------------------------------*/

static void palette_init(void)
{
  int r, g, b, i;

  /************************************************/
  /* Each R,G,B color channel is 4-bit with a     */
  /* total of 15 different intensity levels.      */
  /*                                              */
  /* Color intensity depends on the mode:         */
  /*                                              */
  /*    normal   : xxx0     (0-14)                */
  /*    shadow   : 0xxx     (0-7)                 */
  /*    highlight: 1xxx - 1 (7-14)                */
  /*    mode4    : xx00 ?   (0-12)                */
  /*                                              */
  /* with x = original 2-bit or 3-bit CRAM value  */
  /************************************************/

  /* Initialize Mode 5 pixel color look-up tables */
  for (i = 0; i < 0x200; i++)
  {
    /* CRAM 9-bit value (BBBGGGRRR) */
    r = (i >> 0) & 7;
    g = (i >> 3) & 7;
    b = (i >> 6) & 7;

    /* Convert to output pixel format */
    pixel_lut[0][i] = MAKE_PIXEL(r,g,b);
    pixel_lut[1][i] = MAKE_PIXEL(r<<1,g<<1,b<<1);
    pixel_lut[2][i] = MAKE_PIXEL(r+7,g+7,b+7);
  }

  /* Initialize Mode 4 pixel color look-up table */
  for (i = 0; i < 0x40; i++)
  {
    /* CRAM 6-bit value (000BBGGRR) */
    r = (i >> 0) & 3;
    g = (i >> 2) & 3;
    b = (i >> 4) & 3;

    /* Convert to output pixel format (expand to 4-bit for brighter colors ?) */
    pixel_lut_m4[i] = MAKE_PIXEL(r << 2,g << 2,b<< 2);
  }
}


/*--------------------------------------------------------------------------*/
/* Color palette update functions                                           */
/*--------------------------------------------------------------------------*/

void color_update_m4(int index, unsigned int data)
{
	//logMsg("setting mode 4 color %d to 0x%X", index, data);
  switch (system_hw)
  {
#if 0
    case SYSTEM_GG:
    {
      /* CRAM value (BBBBGGGGRRRR) */
      int r = (data >> 0) & 0x0F;
      int g = (data >> 4) & 0x0F;
      int b = (data >> 8) & 0x0F;

      /* Convert to output pixel */
      data = MAKE_PIXEL(r,g,b);
      break;
    }

    case SYSTEM_SG:
    {
      /* Fixed TMS9918 palette */
      if (index & 0x0F)
      {
        /* Colors 1-15 */
        data = tms_palette[index & 0x0F];
      }
      else
      {
        /* Backdrop color */
        data = tms_palette[reg[7] & 0x0F];
      }
      break;
    }
#endif

    default:
    {
      /* Test M4 bit */
      if (!(reg[0] & 0x04))
      {
        //if (system_hw & SYSTEM_MD)
        {
          /* Invalid Mode (black screen) */
          data = 0x00;
        }
#if 0
        else if (system_hw != SYSTEM_GGMS)
        {
          /* Fixed CRAM palette */
          if (index & 0x0F)
          {
            /* Colors 1-15 */
            data = tms_crom[index & 0x0F];
          }
          else
          {
            /* Backdrop color */
            data = tms_crom[reg[7] & 0x0F];
          }
        }
#endif
      }

      /* Mode 4 palette */
      data = pixel_lut_m4[data & 0x3F];
      break;
    }
  }


  /* Input pixel: x0xiiiii (normal) or 01000000 (backdrop) */
  if (reg[0] & 0x04)
  {
    /* Mode 4 */
    pixel[0x00 | index] = data;
    pixel[0x20 | index] = data;
    pixel[0x80 | index] = data;
    pixel[0xA0 | index] = data;
  }
  else
  {
    /* TMS9918 modes (palette bit forced to 1 because Game Gear uses CRAM palette #1) */
    if ((index == 0x40) || (index == (0x10 | (reg[7] & 0x0F))))
    {
      /* Update backdrop color */
      pixel[0x40] = data;

      /* Update transparent color */
      pixel[0x10] = data;
      pixel[0x30] = data;
      pixel[0x90] = data;
      pixel[0xB0] = data;
    }

    if (index & 0x0F)
    {
      /* update non-transparent colors */
      pixel[0x00 | index] = data;
      pixel[0x20 | index] = data;
      pixel[0x80 | index] = data;
      pixel[0xA0 | index] = data;
    }
  }
}

void color_update_m5(int index, unsigned int data)
{
	//logMsg("setting mode 5 color %d to 0x%X", index, data);
  /* Palette Mode */
  if (!(reg[0] & 0x04))
  {
    /* Color value is limited to 00X00X00X */
    data &= 0x49;
  }

  if(reg[12] & 0x08)
  {
    /* Mode 5 (Shadow/Normal/Highlight) */
    pixel[0x00 | index] = pixel_lut[0][data];
    pixel[0x40 | index] = pixel_lut[1][data];
    pixel[0x80 | index] = pixel_lut[2][data];
  }
  else
  {
    /* Mode 5 (Normal) */
    data = pixel_lut[1][data];

    /* Input pixel: xxiiiiii */
    pixel[0x00 | index] = data;
    pixel[0x40 | index] = data;
    pixel[0x80 | index] = data;
  }
}

/*--------------------------------------------------------------------------*/
/* Background layers rendering functions                                    */
/*--------------------------------------------------------------------------*/

#if 0

/* Graphics I */
void render_bg_m0(int line, int width)
{
  uint8 color, pattern;
  uint16 name;

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *ct = &vram[((reg[3] <<  6) & 0x3FC0)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + (line & 7)];

  /* 32 x 8 pixels */
  width = 32;

  do
  {
    name = *nt++;
    color = ct[name >> 3];
    pattern = pg[name << 3];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 1) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 0) & 1) << 2)) & 0x0F);
  }
  while (--width);
}

/* Text */
void render_bg_m1(int line, int width)
{
  uint8 pattern;
  uint8 color = reg[7];

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line >> 3) * 40)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + (line & 7)];

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  /* 40 x 6 pixels */
  width = 40;

  do
  {
    pattern = pg[*nt++];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

/* Text + extended PG */
void render_bg_m1x(int line, int width)
{
  uint8 pattern;
  uint8 color = reg[7];

  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* Unused bits used as a mask on TMS9918 & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    pg_mask |= 0x1800;
  }

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line >> 3) * 40)];
  uint8 *pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + (line & 7)];

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  /* 40 x 6 pixels */
  width = 40;

  do
  {
    pattern = pg[*nt++ << 3];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

/* Graphics II */
void render_bg_m2(int line, int width)
{
  uint8 color, pattern;
  uint16 name;

  uint16 ct_mask = ~0x3FC0 ^ (reg[3] << 6);
  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* Unused bits used as a mask on TMS9918 & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    ct_mask |= 0x1FC0;
    pg_mask |= 0x1800;
  }

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *ct = &vram[((0x2000 + ((line & 0xC0) << 5)) & ct_mask) + (line & 7)];
  uint8 *pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + (line & 7)];

  /* 32 x 8 pixels */
  width = 32;

  do
  {
    name = *nt++ << 3 ;
    color = ct[name & ct_mask];
    pattern = pg[name];

    *lb++ = 0x10 | ((color >> (((pattern >> 7) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 6) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 5) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 4) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 3) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 2) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 1) & 1) << 2)) & 0x0F);
    *lb++ = 0x10 | ((color >> (((pattern >> 0) & 1) << 2)) & 0x0F);
  }
  while (--width);
}

/* Multicolor */
void render_bg_m3(int line, int width)
{
  uint8 color;
  uint16 name;

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *pg = &vram[((reg[4] << 11) & 0x3800) + ((line >> 2) & 7)];

  /* 32 x 8 pixels */
  width = 32;

  do
  {
    name = *nt++;
    color = pg[name << 3];

    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);
}

/* Multicolor + extended PG */
void render_bg_m3x(int line, int width)
{
  uint8 color;
  uint16 name;

  uint16 pg_mask = ~0x3800 ^ (reg[4] << 11);

  /* Unused bits used as a mask on TMS9918 & 315-5124 VDP only */
  if (system_hw > SYSTEM_SMS)
  {
    pg_mask |= 0x1800;
  }

  uint8 *lb = &linebuf[0][0x20];
  uint8 *nt = &vram[((reg[2] << 10) & 0x3C00) + ((line & 0xF8) << 2)];
  uint8 *pg = &vram[((0x2000 + ((line & 0xC0) << 5)) & pg_mask) + ((line >> 2) & 7)];

  /* 32 x 8 pixels */
  width = 32;

  do
  {
    name = *nt++;
    color = pg[name << 3];

    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);
}

/* Invalid (2+3/1+2+3) */
void render_bg_inv(int line, int width)
{
  uint8 color = reg[7];

  uint8 *lb = &linebuf[0][0x20];

  /* Left border (8 pixels) */
  memset (lb, 0x40, 8);
  lb += 8;

  /* 40 x 6 pixels */
  width = 40;

  do
  {
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 4) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
    *lb++ = 0x10 | ((color >> 0) & 0x0F);
  }
  while (--width);

  /* Right borders (8 pixels) */
  memset(lb, 0x40, 8);
}

#endif

void render_bg_m4(int line, int width)
{
  int column;
  uint16 *nt;
  uint32 attr, atex;

  /* Horizontal scrolling */
  int index = ((reg[0] & 0x40) && (line < 0x10)) ? 0x100 : reg[0x08];
  int shift = index & 7;

  /* Background line buffer */
  auto dst = (uint32 *)&linebuf[0][0x20 + shift];

  /* Pattern name table mask */
  uint16 nt_mask = ~0x3C00 ^ (reg[2] << 10);

  /* Unused bits used as a mask on TMS9918 & 315-5124 VDP only */
  //if (system_hw > SYSTEM_SMS)
  {
    nt_mask |= 0x400;
  }

  /* Vertical scrolling */
  int v_line = line + vscroll;

  /* Test for extended modes (Master System II & Game gear VDP only) */
  if (bitmap.viewport.h > 192)
  {
    /* Vertical scroll mask */
    v_line = v_line % 256;

    /* Pattern name Table */
    nt = &vram.getS((0x3700 & nt_mask) + ((v_line >> 3) << 6));
  }
  else
  {
    /* Vertical scroll mask */
    v_line = v_line % 224;

    /* Pattern name Table */
    nt = &vram.getS((0x3800 + ((v_line >> 3) << 6)) & nt_mask);
  }

  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  /* Tile column index */
  index = (0x100 - index) >> 3;

  /* Clip left-most column if required */
  if (shift)
  {
    memset(&linebuf[0][0x20], 0, shift);
    index++;
  }

  /* Number of tiles to draw */
  width >>= 3;

  /* Draw tiles */
  for(column = 0; column < width; column++, index++)
  {
    /* Stop vertical scrolling for rightmost eight tiles */
    if((column == 24) && (reg[0] & 0x80))
    {
      /* Clear Pattern name table start address */
      if (bitmap.viewport.h > 192)
      {
        nt = &vram.getS((0x3700 & nt_mask) + ((line >> 3) << 6));
      }
      else
      {
        nt = &vram.getS((0x3800 + ((line >> 3) << 6)) & nt_mask);
      }

      /* Clear Pattern row index */
      v_line = (line & 7) << 3;
    }

    /* Read name table attribute word */
    attr = nt[index % width];
#ifndef LSB_FIRST
    attr = (((attr & 0xFF) << 8) | ((attr & 0xFF00) >> 8));
#endif

    /* Expand priority and palette bits */
    atex = atex_table[(attr >> 11) & 3];

    /* Cached pattern data line (4 bytes = 4 pixels at once) */
    auto src = (uint32 *)&bg_pattern_cache[((attr & 0x7FF) << 6) | (v_line)];

    /* Copy left & right half, adding the attribute bits in */
#ifdef ALIGN_DWORD
    WRITE_LONG(dst, src[0] | atex);
    dst++;
    WRITE_LONG(dst, src[1] | atex);
    dst++;
#else
    *((uint32align1*)dst++) = (src[0] | atex);
    *((uint32align1*)dst++) = (src[1] | atex);
#endif
  }
}

#ifndef ALT_RENDERER
void render_bg_m5(int line, int width)
{
  int column;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;

  /* Common data */
  uint32 xscroll = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll = vsram.l[0];

  /* Plane B width */
  int start = 0;
  int end = width >> 4;

  /* Plane B scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = playfield_col_mask + 1 - ((xscroll >> 20) & playfield_col_mask);
  uint32 v_line = (line + ((yscroll >> 16) & 0x3FF)) & playfield_row_mask;
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = playfield_col_mask + 1 - ((xscroll >> 4) & playfield_col_mask);
  uint32 v_line = (line + (yscroll & 0x3FF)) & playfield_row_mask;
#endif

  /* Plane B name table */
  uint32 *nt = (uint32 *)&vram.b[ntbb + (((v_line >> 3) << playfield_shift) & 0x1FC0)];

  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  if(shift)
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & playfield_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    atbuf = nt[index & playfield_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift   = (xscroll & 0x0F);
    index   = playfield_col_mask + start + 1 - ((xscroll >> 4) & playfield_col_mask);
    v_line  = (line + (yscroll & 0x3FF)) & playfield_row_mask;
#else
    shift   = (xscroll >> 16) & 0x0F;
    index   = playfield_col_mask + start + 1 - ((xscroll >> 20) & playfield_col_mask);
    v_line  = (line + ((yscroll >> 16) & 0x3FF)) & playfield_row_mask;
#endif

    /* Plane A name table */
    nt = (uint32 *)&vram.b[ntab + (((v_line >> 3) << playfield_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    if(shift)
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & playfield_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & playfield_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & playfield_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram.b[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = (line & 7) << 3;

    /* Plane A line buffer */
    dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }
}

void render_bg_m5_vs(int line, int width)
{
  int column;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 v_line, *nt;

  /* Common data */
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = &vsram.l[0];

  /* Plane B width */
  int start = 0;
  int end = width >> 4;

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Left-most column vertical scrolling when partially shown horizontally */
  /* Same value for both planes, only in 40-cell mode, verified on PAL MD2 */
  /* See Gynoug, Cutie Suzuki no Ringside Angel, Formula One, Kawasaki Superbike Challenge */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
    yscroll &= 0x3FF;
  }

  if(shift)
  {
    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + ((vs[column] >> 16) & 0x3FF)) & pf_row_mask;
#else
    v_line = (line + (vs[column] & 0x3FF)) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN(atbuf, v_line)
  }
  
  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif 

    if(shift)
    {
      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + (vs[column] & 0x3FF)) & pf_row_mask;
#else
      v_line = (line + ((vs[column] >> 16) & 0x3FF)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = &vram.getL(ntwb | ((line >> 3) << (6 + (reg[12] & 1))));

    /* Pattern row index */
    v_line = (line & 7) << 3;

    /* Plane A line buffer */
    dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }
}

void render_bg_m5_im2(int line, int width)
{
  int column;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;

  /* Common data */
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = vsram.l[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;

  int odd = odd_frame;

  /* Plane B width */
  int start = 0;
  int end = width >> 4;

  /* Plane B scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  uint32 v_line = (line + ((yscroll >> 17) & 0x3FF)) & pf_row_mask;
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  uint32 v_line = (line + ((yscroll >> 1) & 0x3FF)) & pf_row_mask;
#endif

  /* Plane B name table */
  uint32 *nt = (uint32 *)&vram.b[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

  /* Pattern row index */
  v_line = (((v_line & 7) << 1) | odd) << 3;

  if(shift)
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift   = (xscroll & 0x0F);
    index   = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line  = (line + ((yscroll >> 1) & 0x3FF)) & pf_row_mask;
#else
    shift   = (xscroll >> 16) & 0x0F;
    index   = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line  = (line + ((yscroll >> 17) & 0x3FF)) & pf_row_mask;
#endif

    /* Plane A name table */
    nt = (uint32 *)&vram.b[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    if(shift)
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram.b[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = ((line & 7) << 1 | odd) << 3;

    /* Plane A line buffer */
    dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }
}

void render_bg_m5_im2_vs(int line, int width)
{
  int column;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 v_line, *nt;

  /* Common data */
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = &vsram.l[0];

  int odd = odd_frame;

  /* Plane B width */
  int start = 0;
  int end = width >> 4;

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  uint32 shift  = (xscroll >> 16) & 0x0F;
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  uint32 shift  = (xscroll & 0x0F);
  uint32 index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Left-most column vertical scrolling when partially shown horizontally */
  /* Same value for both planes, only in 40-cell mode, verified on PAL MD2 */
  /* See Gynoug, Cutie Suzuki no Ringside Angel, Formula One, Kawasaki Superbike Challenge */
  if (reg[12] & 1)
  {
    /* only in 40-cell mode, verified on MD2 */
    yscroll = (vs[19] >> 1) & (vs[19] >> 17);
    yscroll &= 0x3FF;
  }

  if(shift)
  {
    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = (uint32 *)&vram.b[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x10 + shift];

    atbuf = nt[(index - 1) & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }
  else
  {
    /* Plane B line buffer */
    dst = (uint32 *)&linebuf[0][0x20];
  }

  for(column = 0; column < end; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + ((vs[column] >> 17) & 0x3FF)) & pf_row_mask;
#else
    v_line = (line + ((vs[column] >> 1) & 0x3FF)) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = (uint32 *)&vram.b[ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0)];

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_COLUMN_IM2(atbuf, v_line)
  }

  /* Window & Plane A */
  int a = (reg[18] & 0x1F) << 3;
  int w = (reg[18] >> 7) & 1;

  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    if(shift)
    {
      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = (uint32 *)&vram.b[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x10 + shift + (start << 4)];

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index - 1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }
    else
    {
      /* Plane A line buffer */
      dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + ((vs[column] >> 1) & 0x3FF)) & pf_row_mask;
#else
      v_line = (line + ((vs[column] >> 17) & 0x3FF)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = (uint32 *)&vram.b[ntab + (((v_line >> 3) << pf_shift) & 0x1FC0)];

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }

  /* Window */
  if (w)
  {
    /* Window name table */
    nt = (uint32 *)&vram.b[ntwb | ((line >> 3) << (6 + (reg[12] & 1)))];

    /* Pattern row index */
    v_line = ((line & 7) << 1 | odd) << 3;

    /* Plane A line buffer */
    dst = (uint32 *)&linebuf[1][0x20 + (start << 4)];

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }
}

#else

void render_bg_m5(int line, int width)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 shift, index, v_line, *nt;

  /* Scroll Planes common data */
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = vsram.l[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;

  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Number of columns to draw */
  width >>= 4;

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift  = (xscroll & 0x0F);
    index  = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line = (line + (yscroll & 0x3FF)) & pf_row_mask;
#else
    shift  = (xscroll >> 16) & 0x0F;
    index  = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line = (line + ((yscroll >> 16) & 0x3FF)) & pf_row_mask;
#endif

    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4) + shift];

    /* Plane A name table */
    nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 4;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = &vram.getL(ntwb | ((line >> 3) << (6 + (reg[12] & 1))));

    /* Pattern row index */
    v_line = (line & 7) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* Plane B scroll */
#ifdef LSB_FIRST
  shift  = (xscroll >> 16) & 0x0F;
  index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  v_line = (line + ((yscroll >> 16) & 0x3FF)) & pf_row_mask;
#else
  shift  = (xscroll & 0x0F);
  index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  v_line = (line + (yscroll & 0x3FF)) & pf_row_mask;
#endif

  /* Plane B name table */
  nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));
  
  /* Pattern row index */
  v_line = (v_line & 7) << 3;

  /* Background line buffer */
  uint8 *lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }
 
  for(column = 0; column < width; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_vs(int line, int width)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 shift, index, v_line, *nt;

  /* Scroll Planes common data */
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = &vsram.l[0];

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;

  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Left-most column vertical scrolling when partially shown horizontally */
  /* Same value for both planes, only in 40-cell mode, verified on PAL MD2 */
  /* See Gynoug, Cutie Suzuki no Ringside Angel, Formula One, Kawasaki Superbike Challenge */
  if (reg[12] & 1)
  {
    yscroll = vs[19] & (vs[19] >> 16);
    yscroll &= 0x3FF;
  }

  /* Number of columns to draw */
  width >>= 4;

  /* Plane A*/
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4) + shift];

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 4;

      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + (vs[column] & 0x3FF)) & pf_row_mask;
#else
      v_line = (line + ((vs[column] >> 16) & 0x3FF)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = &vram.getL(ntwb | ((line >> 3) << (6 + (reg[12] & 1))));

    /* Pattern row index */
    v_line = (line & 7) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN(atbuf, v_line)
    }
  }

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Background line buffer */
  uint8 *lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + ((vs[column] >> 16) & 0x3FF)) & pf_row_mask;
#else
    v_line = (line + (vs[column] & 0x3FF)) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (v_line & 7) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_im2(int line, int width)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 shift, index, v_line, *nt;

  /* Scroll Planes common data */
  int odd = odd_frame;
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = vsram.l[0];
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  int a = (reg[18] & 0x1F) << 3;
  
  /* Window position (0=top, 1=bottom) */
  int w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == (line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Number of columns to draw */
  width >>= 4;

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A scroll */
#ifdef LSB_FIRST
    shift  = (xscroll & 0x0F);
    index  = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
    v_line = (line + ((yscroll >> 1) & 0x3FF)) & pf_row_mask;
#else
    shift  = (xscroll >> 16) & 0x0F;
    index  = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
    v_line = (line + ((yscroll >> 17) & 0x3FF)) & pf_row_mask;
#endif

    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4) + shift];

    /* Plane A name table */
    nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 4;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = &vram.getL(ntwb | ((line >> 3) << (6 + (reg[12] & 1))));

    /* Pattern row index */
    v_line = ((line & 7) << 1 | odd) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Plane B scroll */
#ifdef LSB_FIRST
  shift  = (xscroll >> 16) & 0x0F;
  index  = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
  v_line = (line + ((yscroll >> 17) & 0x3FF)) & pf_row_mask;
#else
  shift  = (xscroll & 0x0F);
  index  = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
  v_line = (line + ((yscroll >> 1) & 0x3FF)) & pf_row_mask;
#endif

  /* Plane B name table */
  nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

  /* Pattern row index */
  v_line = (((v_line & 7) << 1) | odd) << 3;

  /* Background line buffer */
  uint8 *lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }
}

void render_bg_m5_im2_vs(int line, int width)
{
  int column, start, end;
  uint32 atex, atbuf, *src;
  uint32align2 *dst;
  uint32 shift, index, v_line, *nt;

  /* common data */
  int odd = odd_frame;
  uint32 xscroll      = vram.getL(hscb + ((line & hscroll_mask) << 2));
  uint32 yscroll      = 0;
  uint32 pf_col_mask  = playfield_col_mask;
  uint32 pf_row_mask  = playfield_row_mask;
  uint32 pf_shift     = playfield_shift;
  uint32 *vs          = &vsram.l[0];

  /* Layer priority table */
  uint8 *table = lut[(reg[12] & 8) >> 2];

  /* Window vertical range (cell 0-31) */
  uint32 a = (reg[18] & 0x1F) << 3;
  
  /* Window position (0=top, 1=bottom) */
  uint32 w = (reg[18] >> 7) & 1;

  /* Test against current line */
  if (w == ((unsigned)line >= a))
  {
    /* Window takes up entire line */
    a = 0;
    w = 1;
  }
  else
  {
    /* Window and Plane A share the line */
    a = clip[0].enable;
    w = clip[1].enable;
  }

  /* Left-most column vertical scrolling when partially shown horizontally */
  /* Same value for both planes, only in 40-cell mode, verified on PAL MD2 */
  /* See Gynoug, Cutie Suzuki no Ringside Angel, Formula One, Kawasaki Superbike Challenge */
  if (reg[12] & 1)
  {
    /* only in 40-cell mode, verified on MD2 */
    yscroll = (vs[19] >> 1) & (vs[19] >> 17);
    yscroll &= 0x3FF;
  }

  /* Number of columns to draw */
  width >>= 4;

  /* Plane A */
  if (a)
  {
    /* Plane A width */
    start = clip[0].left;
    end   = clip[0].right;

    /* Plane A horizontal scroll */
#ifdef LSB_FIRST
    shift = (xscroll & 0x0F);
    index = pf_col_mask + start + 1 - ((xscroll >> 4) & pf_col_mask);
#else
    shift = (xscroll >> 16) & 0x0F;
    index = pf_col_mask + start + 1 - ((xscroll >> 20) & pf_col_mask);
#endif

    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4) + shift];

    if(shift)
    {
      /* Left-most column is partially shown */
      dst -= 4;

      /* Plane A vertical scroll */
      v_line = (line + yscroll) & pf_row_mask;

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      /* Window bug */
      if (start)
      {
        atbuf = nt[index & pf_col_mask];
      }
      else
      {
        atbuf = nt[(index-1) & pf_col_mask];
      }

      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    for(column = start; column < end; column++, index++)
    {
      /* Plane A vertical scroll */
#ifdef LSB_FIRST
      v_line = (line + ((vs[column] >> 1) & 0x3FF)) & pf_row_mask;
#else
      v_line = (line + ((vs[column] >> 17) & 0x3FF)) & pf_row_mask;
#endif

      /* Plane A name table */
      nt = &vram.getL(ntab + (((v_line >> 3) << pf_shift) & 0x1FC0));

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      atbuf = nt[index & pf_col_mask];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }

    /* Window width */
    start = clip[1].left;
    end   = clip[1].right;
  }
  else
  {
    /* Window width */
    start = 0;
    end   = width;
  }

  /* Window Plane */
  if (w)
  {
    /* Background line buffer */
    dst = (uint32 *)&linebuf[0][0x20 + (start << 4)];

    /* Window name table */
    nt = &vram.getL(ntwb | ((line >> 3) << (6 + (reg[12] & 1))));

    /* Pattern row index */
    v_line = ((line & 7) << 1 | odd) << 3;

    for(column = start; column < end; column++)
    {
      atbuf = nt[column];
      DRAW_COLUMN_IM2(atbuf, v_line)
    }
  }

  /* Plane B horizontal scroll */
#ifdef LSB_FIRST
  shift = (xscroll >> 16) & 0x0F;
  index = pf_col_mask + 1 - ((xscroll >> 20) & pf_col_mask);
#else
  shift = (xscroll & 0x0F);
  index = pf_col_mask + 1 - ((xscroll >> 4) & pf_col_mask);
#endif

  /* Background line buffer */
  uint8 *lb = &linebuf[0][0x20];

  if(shift)
  {
    /* Left-most column is partially shown */
    lb -= (0x10 - shift);

    /* Plane B vertical scroll */
    v_line = (line + yscroll) & pf_row_mask;

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    atbuf = nt[(index-1) & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }

  for(column = 0; column < width; column++, index++)
  {
    /* Plane B vertical scroll */
#ifdef LSB_FIRST
    v_line = (line + ((vs[column] >> 17) & 0x3FF)) & pf_row_mask;
#else
    v_line = (line + ((vs[column] >> 1) & 0x3FF)) & pf_row_mask;
#endif

    /* Plane B name table */
    nt = &vram.getL(ntbb + (((v_line >> 3) << pf_shift) & 0x1FC0));

    /* Pattern row index */
    v_line = (((v_line & 7) << 1) | odd) << 3;

    atbuf = nt[index & pf_col_mask];
    DRAW_BG_COLUMN_IM2(atbuf, v_line, xscroll, yscroll)
  }
}
#endif


/*--------------------------------------------------------------------------*/
/* Sprite layer rendering functions                                         */
/*--------------------------------------------------------------------------*/

void render_obj_m4(int max_width)
{
  int i, count, xpos, end;
  uint8 *src, *lb;
  uint16 temp;

  /* Default sprite width */
  int width = 8;

  /* Sprite Generator address mask (LSB is masked for 8x16 sprites) */
  uint16 sg_mask = (~0x1C0 ^ (reg[6] << 6)) & (~((reg[1] & 0x02) >> 1));

  /* Unused bits used as a mask on 315-5124 VDP only */
  //if (system_hw > SYSTEM_SMS)
  {
    sg_mask |= 0xC0;
  }

  /* Set SOVR flag */
  status |= spr_ovr;
  spr_ovr = 0;

  /* Draw sprites in front-to-back order */
  for (count = 0; count < object_count; count++)
  {
    /* Sprite pattern index */
    temp = (object_info[count].attr | 0x100) & sg_mask;

    /* Pointer to pattern cache line */
    src = (uint8 *)&bg_pattern_cache[(temp << 6) | (object_info[count].ypos << 3)];

    /* Sprite X position */
    xpos = object_info[count].xpos;

    /* X position shift */
    xpos -= (reg[0] & 0x08);

    if (xpos < 0)
    {
      /* Clip sprites on left edge */
      src = src - xpos;
      end = xpos + width;
      xpos = 0;
    }
    else if ((xpos + width) > max_width)
    {
      /* Clip sprites on right edge */
      end = max_width - xpos;
    }
    else
    {
      /* Sprite maximal width */
      end = width;
    }

    /* Pointer to line buffer */
    lb = &linebuf[0][0x20 + xpos];

    if (width > 8)
    {
      // Draw sprite pattern (zoomed sprites are rendered at half speed)
      DRAW_SPRITE_TILE_ACCURATE_2X(end,0,lut[5])
    }
    else
    {
      /* Draw sprite pattern */
      DRAW_SPRITE_TILE_ACCURATE(end,0,lut[5])
    }
  }
}

void render_obj_m5(int max_width)
{
  int i, count, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

#ifndef ALT_RENDERER
  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[0], max_width);
#endif

  /* Draw sprites in front-to-back order */
  for (count = 0; count < object_count; count++)
  {
    /* Sprite X position */
    xpos = object_info[count].xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info[count].size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < max_width) && !masked)
    {
      /* Sprite attributes */
      attr = object_info[count].attr;

      /* Sprite vertical offset */
      v_line = object_info[count].ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x07FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pointer into pattern name offset look-up table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[0][0x20 + xpos];

      /* Adjust number of pixels to draw for sprite limit */
      if (pixelcount > max_width)
      {
        width = width - pixelcount + max_width;
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Draw sprite patterns */
      for(column = 0; column < width; column++, lb+=8)
      {
        temp = attr | ((name + s[column]) & 0x07FF);
        src = &bg_pattern_cache[(temp << 6) | (v_line)];
        DRAW_SPRITE_TILE(8,atex,lut[1])
      }
    }

    /* Sprite limit */
    if (pixelcount >= max_width)
    {
      /* Sprite masking will be effective on next line  */
      spr_ovr = 1;

      /* Stop sprite rendering */
      return;
    }
  }

  /* Clear sprite masking for next line  */
  spr_ovr = 0;
}

void render_obj_m5_ste(int max_width)
{
  int i, count, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

#ifndef ALT_RENDERER
  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[2], max_width);
#endif

  /* Clear sprite line buffer */
  memset(&linebuf[1][0], 0, max_width + 0x40);

  /* Draw sprites in front-to-back order */
  for (count = 0; count < object_count; count++)
  {
    /* Sprite X position */
    xpos = object_info[count].xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info[count].size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < max_width) && !masked)
    {
      /* Sprite attributes */
      attr = object_info[count].attr;

      /* Sprite vertical offset */
      v_line = object_info[count].ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x07FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pointer into pattern name offset look-up table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[1][0x20 + xpos];

      /* Adjust number of pixels to draw for sprite limit */
      if (pixelcount > max_width)
      {
        width = width - pixelcount + max_width;
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (v_line & 7) << 3;

      /* Draw sprite patterns */
      for(column = 0; column < width; column++, lb+=8)
      {
        temp = attr | ((name + s[column]) & 0x07FF);
        src = &bg_pattern_cache[(temp << 6) | (v_line)];
        DRAW_SPRITE_TILE(8,atex,lut[3])
      }
    }

    /* Sprite limit */
    if (pixelcount >= max_width)
    {
      /* Sprite masking will be effective on next line  */
      spr_ovr = 1;

      /* Merge background & sprite layers */
      merge(&linebuf[1][0x20],&linebuf[0][0x20],&linebuf[0][0x20],lut[4], max_width);

      /* Stop sprite rendering */
      return;
    }
  }

  /* Clear sprite masking for next line  */
  spr_ovr = 0;

  /* Merge background & sprite layers */
  merge(&linebuf[1][0x20],&linebuf[0][0x20],&linebuf[0][0x20],lut[4], max_width);
}

void render_obj_m5_im2(int max_width)
{
  int i, count, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int odd = odd_frame;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

#ifndef ALT_RENDERER
  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[0], max_width);
#endif

  /* Draw sprites in front-to-back order */
  for (count = 0; count < object_count; count++)
  {
    /* Sprite X position */
    xpos = object_info[count].xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info[count].size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < max_width) && !masked)
    {
      /* Sprite attributes */
      attr = object_info[count].attr;

      /* Sprite y offset */
      v_line = object_info[count].ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x03FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pattern name offset lookup table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[0][0x20 + xpos];

      /* Adjust width for sprite limit */
      if (pixelcount > max_width)
      {
        width = width - pixelcount + max_width;
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      /* Render sprite patterns */
      for(column = 0; column < width; column ++, lb+=8)
      {
        temp = attr | (((name + s[column]) & 0x3ff) << 1);
        src = &bg_pattern_cache[((temp << 6) | (v_line)) ^ ((attr & 0x1000) >> 6)];
        DRAW_SPRITE_TILE(8,atex,lut[1])
      }
    }

    /* Sprite Limit */
    if (pixelcount >= max_width)
    {
      /* Enable sprite masking for next line */
      spr_ovr = 1;

      /* Stop sprite rendering */
      return;
    }
  }

  /* Clear sprite masking for next line */
  spr_ovr = 0;
}

void render_obj_m5_im2_ste(int max_width)
{
  int i, count, column;
  int xpos, width;
  int pixelcount = 0;
  int masked = 0;
  int odd = odd_frame;

  uint8 *src, *s, *lb;
  uint32 temp, v_line;
  uint32 attr, name, atex;

#ifndef ALT_RENDERER
  /* Merge background layers */
  merge(&linebuf[1][0x20], &linebuf[0][0x20], &linebuf[0][0x20], lut[2], max_width);
#endif

  /* Clear sprite line buffer */
  memset(&linebuf[1][0], 0, max_width + 0x40);

  /* Draw sprites in front-to-back order */
  for (count = 0; count < object_count; count++)
  {
    /* Sprite X position */
    xpos = object_info[count].xpos;

    /* Sprite masking  */
    if (xpos)
    {
      /* Requires at least one sprite with xpos > 0 */
      spr_ovr = 1;
    }
    else if (spr_ovr)
    {
      /* Remaining sprites are not drawn */
      masked = 1;
    }

    /* Display area offset */
    xpos = xpos - 0x80;

    /* Sprite size */
    temp = object_info[count].size;

    /* Sprite width */
    width = 8 + ((temp & 0x0C) << 1);

    /* Update pixel count (off-screen sprites are included) */
    pixelcount += width;

    /* Is sprite across visible area ? */
    if (((xpos + width) > 0) && (xpos < max_width) && !masked)
    {
      /* Sprite attributes */
      attr = object_info[count].attr;

      /* Sprite y offset */
      v_line = object_info[count].ypos;

      /* Sprite priority + palette bits */
      atex = (attr >> 9) & 0x70;

      /* Pattern name base */
      name = attr & 0x03FF;

      /* Mask vflip/hflip */
      attr &= 0x1800;

      /* Pattern name offset lookup table */
      s = &name_lut[((attr >> 3) & 0x300) | (temp << 4) | ((v_line & 0x18) >> 1)];

      /* Pointer into line buffer */
      lb = &linebuf[1][0x20 + xpos];

      /* Adjust width for sprite limit */
      if (pixelcount > max_width)
      {
        width = width - pixelcount + max_width;
      }

      /* Number of tiles to draw */
      width = width >> 3;

      /* Pattern row index */
      v_line = (((v_line & 7) << 1) | odd) << 3;

      /* Render sprite patterns */
      for(column = 0; column < width; column ++, lb+=8)
      {
        temp = attr | (((name + s[column]) & 0x3ff) << 1);
        src = &bg_pattern_cache[((temp << 6) | (v_line)) ^ ((attr & 0x1000) >> 6)];
        DRAW_SPRITE_TILE(8,atex,lut[3])
      }
    }

    /* Sprite Limit */
    if (pixelcount >= max_width)
    {
      /* Enable sprite masking for next line */
      spr_ovr = 1;

      /* Merge background & sprite layers */
      merge(&linebuf[1][0x20],&linebuf[0][0x20],&linebuf[0][0x20],lut[4], max_width);

      /* Stop sprite rendering */
      return;
    }
  }

  /* Clear sprite masking for next line */
  spr_ovr = 0;

  /* Merge background & sprite layers */
  merge(&linebuf[1][0x20],&linebuf[0][0x20],&linebuf[0][0x20],lut[4], max_width);
}


/*--------------------------------------------------------------------------*/
/* Sprites Parsing functions                                                */
/*--------------------------------------------------------------------------*/

void parse_satb_m4(int line)
{
  int i = 0;

  /* Sprite counter (64 max.) */
  int count = 0;

  /* Y position */
  int ypos;

  /* Sprite height (8x8 by default) */
  int height = 8;

  /* Adjust height for 8x16 sprites */
  height <<= ((reg[1] & 0x02) >> 1);

  /* Sprite attribute table address mask */
  uint16 st_mask = ~0x3F80 ^ (reg[5] << 7);

  /* Unused bits used as a mask on 315-5124 VDP only */
  //if (system_hw > SYSTEM_SMS)
  {
    st_mask |= 0x80;
  }

  /* Pointer to sprite attribute table */
  uint8 *st = &vram.b[st_mask & 0x3F00];

  /* Parse Sprite Table (64 entries) */
  do
  {
    /* Sprite Y position */
    ypos = st[i];

    /* Check end of sprite list marker */
    if(ypos == (bitmap.viewport.h + 16))
    {
      break;
    }

    /* Wrap Y coordinate for sprites > 256-16 */
    if(ypos >= 240)
    {
      ypos -= 256;
    }

    /* Y range */
    ypos = line - ypos;

    /* Sprite is visble on this line ? */
    if((ypos >= 0) && (ypos < height))
    {
      /* Sprite overflow */
      if(count == 8)
      {
      	/* Flag is set only during active area */
				if ((line >= 0) && (line < bitmap.viewport.h))
				{
					spr_ovr = 0x40;
				}
        break;
      }

      /* Store sprite attributes for later processing */
      object_info[count].ypos = ypos;
      object_info[count].xpos = st[(0x80 + (i << 1)) & st_mask];
      object_info[count].attr = st[(0x81 + (i << 1)) & st_mask];

      /* Increment Sprite count */
      ++count;
    }
  }
  while (++i < 64);

  /* Update sprite count for next line */
  object_count = count;
}

void parse_satb_m5(int line)
{
  /* Y position */
  int ypos;

  /* Sprite height (8,16,24,32 pixels)*/
  int height;

  /* Sprite size data */
  int size;

  /* Sprite link data */
  int link = 0;

  /* Sprite counter */
  int count = 0;

  /* 16 or 20 sprites max. per line */
  int max = 16 + ((reg[12] & 1) << 2);

  /* 64 or 80 sprites max. */
  int total = max << 2;

  /* Pointer to sprite attribute table */
  uint16 *p = (uint16 *) &vram.b[satb];

  /* Pointer to internal RAM */
  uint16 *q = &sat.s[0];

  /* Adjust line offset */
  line += 0x81;

  do
  {
    /* Read Y position & size from internal SAT */
    ypos = (q[link] >> im2_flag) & 0x1FF;
    size = q[link + 1] >> 8;

    /* Sprite height */
    height = 8 + ((size & 3) << 3);

    /* Y range */
    ypos = line - ypos;

    /* Sprite is visble on this line ? */
    if ((ypos >= 0) && (ypos < height))
    {
      /* Sprite overflow */
      if(count == max)
      {
        status |= 0x40;
        break;
      }

      /* Update sprite list */
      /* name, attribute & xpos are parsed from VRAM */ 
      object_info[count].attr  = p[link + 2];
      object_info[count].xpos  = p[link + 3] & 0x1ff;
      object_info[count].ypos  = ypos;
      object_info[count].size  = size & 0x0f;
      ++count;
    }

    /* Read link data from internal SAT */ 
    link = (q[link + 1] & 0x7F) << 2;

    /* Last sprite */
    if(link == 0) break;
  }
  while (--total);

  /* Update sprite count for next line */
  object_count = count;
}


/*--------------------------------------------------------------------------*/
/* Pattern cache update function                                            */
/*--------------------------------------------------------------------------*/

void update_bg_pattern_cache_m4(int index)
{
  int i;
  uint8 x, y, c;
  uint8 *dst;
  uint16 name, bp01, bp23;
  uint32 bp;

  for(i = 0; i < index; i++)
  {
    /* Get modified pattern name index */
    name = bg_name_list[i];

    /* Check modified lines */
    for(y = 0; y < 8; y++)
    {
      if(bg_name_dirty[name] & (1 << y))
      {
        /* Pattern cache base address */
        dst = &bg_pattern_cache[name << 6];

        /* Byteplane data */
        bp01 = vram.getS((name << 5) | (y << 2) | (0));
        bp23 = vram.getS((name << 5) | (y << 2) | (2));

        /* Convert to pixel line data (4 bytes = 8 pixels)*/
        /* (msb) p7p6 p5p4 p3p2 p1p0 (lsb) */
        bp = (bp_lut[bp01] >> 2) | (bp_lut[bp23]);

        /* Update cached line (8 pixels = 8 bytes) */
        for(x = 0; x < 8; x++)
        {
          /* Extract pixel data */
          c = bp & 0x0F;

          /* Pattern cache data (one pattern = 8 bytes) */
          /* byte0 <-> p0 p1 p2 p3 p4 p5 p6 p7 <-> byte7 (hflip = 0) */
          /* byte0 <-> p7 p6 p5 p4 p3 p2 p1 p0 <-> byte7 (hflip = 1) */
          dst[0x00000 | (y << 3) | (x)] = (c);            /* vflip=0 & hflip=0 */
          dst[0x08000 | (y << 3) | (x ^ 7)] = (c);        /* vflip=0 & hflip=1 */
          dst[0x10000 | ((y ^ 7) << 3) | (x)] = (c);      /* vflip=1 & hflip=0 */
          dst[0x18000 | ((y ^ 7) << 3) | (x ^ 7)] = (c);  /* vflip=1 & hflip=1 */

          /* Next pixel */
          bp = bp >> 4;
        }
      }
    }

    /* Clear modified pattern flag */
    bg_name_dirty[name] = 0;
  }
}

void update_bg_pattern_cache_m5(int index)
{
  int i;
  uint8 x, y, c;
  uint8 *dst;
  uint16 name;
  uint32 bp;

  for(i = 0; i < index; i++)
  {
    /* Get modified pattern name index */
    name = bg_name_list[i];

    /* Check modified lines */
    for(y = 0; y < 8; y ++)
    {
      if(bg_name_dirty[name] & (1 << y))
      {
        /* Pattern cache base address */
        dst = &bg_pattern_cache[name << 6];

        /* Byteplane data (one pattern = 4 bytes) */
        /* LIT_ENDIAN: byte0 (lsb) p2p3 p0p1 p6p7 p4p5 (msb) byte3 */
        /* BIG_ENDIAN: byte0 (msb) p0p1 p2p3 p4p5 p6p7 (lsb) byte3 */
        bp = vram.getL((name << 5) | (y << 2));

        /* Update cached line (8 pixels = 8 bytes) */
        for(x = 0; x < 8; x ++)
        {
          /* Extract pixel data */
          c = bp & 0x0F;

          /* Pattern cache data (one pattern = 8 bytes) */
          /* byte0 <-> p0 p1 p2 p3 p4 p5 p6 p7 <-> byte7 (hflip = 0) */
          /* byte0 <-> p7 p6 p5 p4 p3 p2 p1 p0 <-> byte7 (hflip = 1) */
#ifdef LSB_FIRST
          /* Byteplane data = (msb) p4p5 p6p7 p0p1 p2p3 (lsb) */
          dst[0x00000 | (y << 3) | (x ^ 3)] = (c);        /* vflip=0, hflip=0 */
          dst[0x20000 | (y << 3) | (x ^ 4)] = (c);        /* vflip=0, hflip=1 */
          dst[0x40000 | ((y ^ 7) << 3) | (x ^ 3)] = (c);  /* vflip=1, hflip=0 */
          dst[0x60000 | ((y ^ 7) << 3) | (x ^ 4)] = (c);  /* vflip=1, hflip=1 */
#else
          /* Byteplane data = (msb) p0p1 p2p3 p4p5 p6p7 (lsb) */
          dst[0x00000 | (y << 3) | (x ^ 7)] = (c);        /* vflip=0, hflip=0 */
          dst[0x20000 | (y << 3) | (x)] = (c);            /* vflip=0, hflip=1 */
          dst[0x40000 | ((y ^ 7) << 3) | (x ^ 7)] = (c);  /* vflip=1, hflip=0 */
          dst[0x60000 | ((y ^ 7) << 3) | (x)] = (c);      /* vflip=1, hflip=1 */
#endif
          /* Next pixel */
          bp = bp >> 4;
        }
      }
    }

    /* Clear modified pattern flag */
    bg_name_dirty[name] = 0;
  }
}


/*--------------------------------------------------------------------------*/
/* Window & Plane A clipping update function (Mode 5)                       */
/*--------------------------------------------------------------------------*/

void window_clip(unsigned int data, unsigned int sw)
{
  /* Window size and invert flags */
  unsigned int hp = (data & 0x1f);
  int hf = (data >> 7) & 1;

  /* Display width (16 or 20 columns) */
  sw = 16 + (sw << 2);

  /* Perform horizontal clipping; the results are applied in reverse
     if the horizontal inversion flag is set
   */
  int a = hf;
  int w = hf ^ 1;

  if(hp)
  {
    if(hp > sw)
    {
      /* Plane W takes up entire line */
      clip[w].left = 0;
      clip[w].right = sw;
      clip[w].enable = 1;
      clip[a].enable = 0;
    }
    else
    {
      /* Plane W takes left side, Plane A takes right side */
      clip[w].left = 0;
      clip[a].right = sw;
      clip[a].left = clip[w].right = hp;
      clip[0].enable = clip[1].enable = 1;
    }
  }
  else
  {
    /* Plane A takes up entire line */
    clip[a].left = 0;
    clip[a].right = sw;
    clip[a].enable = 1;
    clip[w].enable = 0;
  }
}


/*--------------------------------------------------------------------------*/
/* Init, reset routines                                                     */
/*--------------------------------------------------------------------------*/

void render_init(void)
{
  int bx, ax;

  /* Initialize layers priority pixel look-up tables */
  uint16 index;
  for (bx = 0; bx < 0x100; bx++)
  {
    for (ax = 0; ax < 0x100; ax++)
    {
      index = (bx << 8) | (ax);

      lut[0][index] = make_lut_bg(bx, ax);
      lut[1][index] = make_lut_bgobj(bx, ax);
      lut[2][index] = make_lut_bg_ste(bx, ax);
      lut[3][index] = make_lut_obj(bx, ax);
      lut[4][index] = make_lut_bgobj_ste(bx, ax);
      lut[5][index] = make_lut_bgobj_m4(bx,ax);
    }
  }

  /* Initialize pixel color look-up tables */
  palette_init();

  /* Make sprite pattern name index look-up table (Mode 5) */
  make_name_lut();

  /* Make bitplane to pixel look-up table (Mode 4) */
  make_bp_lut();
}

void render_reset(void)
{
  /* Clear line buffers */
  memset(linebuf, 0, sizeof(linebuf));

  /* Clear color palettes */
  memset(pixel, 0, sizeof(pixel));

  /* Reset Sprite infos */
  spr_ovr = spr_col = object_count = 0;
}


/*--------------------------------------------------------------------------*/
/* Line rendering functions                                                 */
/*--------------------------------------------------------------------------*/

void render_line(int line, IG::MutablePixmapView pix)
{
  int width = bitmap.viewport.w;

  /* Check display status */
  if (reg[1] & 0x40)
  {
    /* Update pattern cache */
    if (bg_list_index)
    {
      update_bg_pattern_cache(bg_list_index);
      bg_list_index = 0;
    }

    /* Render BG layer(s) */
    render_bg(line, width);

    /* Render sprite layer */
    render_obj(width);

    /* Left-most column blanking */
    if(reg[0] & 0x20)
    {
      memset(&linebuf[0][0x20], 0x40, 8);
    }

    /* Parse sprites for next line */
    if (line < (bitmap.viewport.h - 1))
    {
      parse_satb(line);
    }
  }
  else
  {
    /* Display disabled */
    memset(&linebuf[0][0x20], 0x40, width);
  }

  /* Horizontal borders */
  int x_offset = bitmap.viewport.x;
  if (x_offset > 0)
  {
    memset(&linebuf[0][0x20 - x_offset], 0x40, x_offset);
    memset(&linebuf[0][0x20 + width], 0x40, x_offset);
  }

  /* Pixel color remapping */
  if(pix)
  	remap_line(line, pix);
}

void blank_line(int line, int offset, int width)
{
  memset(&linebuf[0][0x20 + offset], 0x40, width);
  //remap_line(line);
}

void remap_line(int line, IG::MutablePixmapView pix)
{
  /* Line width */
  int x_offset = bitmap.viewport.x;
  int width = bitmap.viewport.w + (x_offset << 1);

  /* Adjust line offset in framebuffer */
  line = (line + bitmap.viewport.y) % lines_per_frame;

  /* Adjust for interlaced output */
  if (interlaced && config_render)
  {
    line = (line << 1) + odd_frame;
  }

  /* Pixel line buffer */
  uint8 *src = &linebuf[0][0x20 - x_offset];
	auto *dst = (Pixel*)&pix[0, line];
	do
	{
		*dst++ = pixel[*src++];
	}
	while (--width);
}

static bool isValidPixelFormat(IG::PixelFormat fmt)
{
	if constexpr(RENDER_BPP == 32)
	{
		return fmt == IG::PIXEL_RGBA8888 || fmt == IG::PIXEL_BGRA8888;
	}
	else
	{
		return fmt == IG::PIXEL_RGB565;
	}
}

void setFramebufferRenderFormat(IG::PixelFormat fmt)
{
	if(!isValidPixelFormat(fmt))
		return;
	fbRenderFormat = fmt;
	palette_init();
}

IG::PixelFormat framebufferRenderFormat()
{
	assumeExpr(isValidPixelFormat(fbRenderFormat));
	return fbRenderFormat;
}
