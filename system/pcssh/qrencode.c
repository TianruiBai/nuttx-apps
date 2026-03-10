/****************************************************************************
 * apps/system/pcssh/qrencode.c
 *
 * Minimal QR code encoder for NuttX / PicoCalc.
 *
 * Implements QR Code Model 2 versions 1-10, byte mode, ECC level L/M.
 * This is a self-contained implementation with no external dependencies
 * (no libqrencode, etc.) — suitable for embedded use.
 *
 * Based on the QR code specification (ISO/IEC 18004:2015).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "qrencode.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAX_DATA_CODEWORDS  256
#define MAX_EC_CODEWORDS    128

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Capacity table: data codewords per version/ecl */

struct version_info_s
{
  uint8_t total_codewords;
  uint8_t ec_per_block;
  uint8_t num_blocks;
  uint8_t data_codewords;  /* total_codewords - ec_per_block * num_blocks */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Version info for ECL_LOW and ECL_MEDIUM only (versions 1-10).
 * Indexed as [version-1][ecl] where ecl is 0=LOW, 1=MEDIUM.
 */

static const struct version_info_s g_vinfo[10][2] =
{
  /* V1  */ {{ 26,  7, 1, 19}, { 26, 10, 1, 16}},
  /* V2  */ {{ 44, 10, 1, 34}, { 44, 16, 1, 28}},
  /* V3  */ {{ 70, 15, 1, 55}, { 70, 26, 1, 44}},
  /* V4  */ {{100, 20, 1, 80}, {100, 18, 2, 64}},
  /* V5  */ {{134, 26, 1,108}, {134, 24, 2, 86}},
  /* V6  */ {{172, 18, 2,136}, {172, 16, 4,108}},
  /* V7  */ {{196, 20, 2,156}, {196, 18, 4,124}},
  /* V8  */ {{242, 24, 2,194}, {242, 22, 4,154}},
  /* V9  */ {{292, 30, 2,232}, {292, 22, 4,182}},
  /* V10 */ {{346, 18, 4,274}, {346, 26, 4,216}},
};

/* Alignment pattern positions for versions 2-10 */

static const uint8_t g_alignpos[10][7] =
{
  /* V1  */ {0},
  /* V2  */ {6, 18, 0},
  /* V3  */ {6, 22, 0},
  /* V4  */ {6, 26, 0},
  /* V5  */ {6, 30, 0},
  /* V6  */ {6, 34, 0},
  /* V7  */ {6, 22, 38, 0},
  /* V8  */ {6, 24, 42, 0},
  /* V9  */ {6, 26, 46, 0},
  /* V10 */ {6, 28, 50, 0},
};

/* Format info bits for mask patterns 0-7, ECL_LOW */

static const uint16_t g_format_low[8] =
{
  0x77c4, 0x72f3, 0x7daa, 0x789d,
  0x662f, 0x6318, 0x6c41, 0x6976,
};

/* Format info bits for ECL_MEDIUM */

static const uint16_t g_format_med[8] =
{
  0x5412, 0x5125, 0x5e7c, 0x5b4b,
  0x45f9, 0x40ce, 0x4f97, 0x4aa0,
};

/* GF(256) logs and antilogs for Reed-Solomon using polynomial 0x11D */

static uint8_t g_gf_exp[512];
static uint8_t g_gf_log[256];
static bool    g_gf_init = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: gf_init
 ****************************************************************************/

static void gf_init(void)
{
  if (g_gf_init) return;

  int x = 1;
  for (int i = 0; i < 255; i++)
    {
      g_gf_exp[i] = (uint8_t)x;
      g_gf_log[x] = (uint8_t)i;
      x <<= 1;
      if (x >= 256)
        {
          x ^= 0x11d;
        }
    }

  for (int i = 255; i < 512; i++)
    {
      g_gf_exp[i] = g_gf_exp[i - 255];
    }

  g_gf_init = true;
}

/****************************************************************************
 * Name: gf_mul
 ****************************************************************************/

static inline uint8_t gf_mul(uint8_t a, uint8_t b)
{
  if (a == 0 || b == 0) return 0;
  return g_gf_exp[g_gf_log[a] + g_gf_log[b]];
}

/****************************************************************************
 * Name: rs_encode
 *
 * Description:
 *   Reed-Solomon error correction encoding.
 *
 ****************************************************************************/

static void rs_encode(const uint8_t *data, int data_len,
                      uint8_t *ec, int ec_len)
{
  /* Build generator polynomial */

  uint8_t gen[MAX_EC_CODEWORDS + 1];
  memset(gen, 0, sizeof(gen));
  gen[0] = 1;

  for (int i = 0; i < ec_len; i++)
    {
      /* Multiply gen by (x - alpha^i) */

      for (int j = ec_len; j > 0; j--)
        {
          gen[j] = gen[j - 1] ^ gf_mul(gen[j], g_gf_exp[i]);
        }

      gen[0] = gf_mul(gen[0], g_gf_exp[i]);
    }

  /* Division */

  uint8_t remainder[MAX_EC_CODEWORDS];
  memset(remainder, 0, ec_len);

  for (int i = 0; i < data_len; i++)
    {
      uint8_t coef = data[i] ^ remainder[0];
      memmove(remainder, remainder + 1, ec_len - 1);
      remainder[ec_len - 1] = 0;

      if (coef != 0)
        {
          for (int j = 0; j < ec_len; j++)
            {
              remainder[j] ^= gf_mul(gen[j], coef);
            }
        }
    }

  memcpy(ec, remainder, ec_len);
}

/****************************************************************************
 * Name: set_module / get_module
 ****************************************************************************/

static inline void set_module(struct qrcode_s *qr, int x, int y,
                              uint8_t val)
{
  if (x >= 0 && x < qr->size && y >= 0 && y < qr->size)
    {
      qr->modules[y][x] = val;
    }
}

static inline uint8_t get_module(const struct qrcode_s *qr, int x, int y)
{
  if (x >= 0 && x < qr->size && y >= 0 && y < qr->size)
    {
      return qr->modules[y][x];
    }

  return 0;
}

/****************************************************************************
 * Name: place_finder_pattern
 ****************************************************************************/

static void place_finder_pattern(struct qrcode_s *qr, int cx, int cy)
{
  for (int dy = -3; dy <= 3; dy++)
    {
      for (int dx = -3; dx <= 3; dx++)
        {
          bool border = (dx == -3 || dx == 3 || dy == -3 || dy == 3);
          bool inner  = (abs(dx) <= 1 && abs(dy) <= 1);

          set_module(qr, cx + dx, cy + dy,
                     (border || inner) ? 3 : 2);  /* 3=black fixed,
                                                      2=white fixed */
        }
    }
}

/****************************************************************************
 * Name: place_alignment_pattern
 ****************************************************************************/

static void place_alignment_pattern(struct qrcode_s *qr, int cx, int cy)
{
  for (int dy = -2; dy <= 2; dy++)
    {
      for (int dx = -2; dx <= 2; dx++)
        {
          bool border = (abs(dx) == 2 || abs(dy) == 2);
          bool center = (dx == 0 && dy == 0);

          if (qr->modules[cy + dy][cx + dx] >= 2)
            {
              continue;  /* Don't overwrite finder patterns */
            }

          set_module(qr, cx + dx, cy + dy,
                     (border || center) ? 3 : 2);
        }
    }
}

/****************************************************************************
 * Name: place_fixed_patterns
 *
 * Description:
 *   Place finder patterns, separators, timing patterns, alignment,
 *   and dark module.
 *
 ****************************************************************************/

static void place_fixed_patterns(struct qrcode_s *qr)
{
  int sz = qr->size;

  /* Finder patterns + separators */

  place_finder_pattern(qr, 3, 3);
  place_finder_pattern(qr, sz - 4, 3);
  place_finder_pattern(qr, 3, sz - 4);

  /* Separators (white border around finders) */

  for (int i = 0; i < 8; i++)
    {
      /* top-left */

      set_module(qr, i, 7, 2);
      set_module(qr, 7, i, 2);

      /* top-right */

      set_module(qr, sz - 8 + i, 7, 2);
      set_module(qr, sz - 8, i, 2);

      /* bottom-left */

      set_module(qr, i, sz - 8, 2);
      set_module(qr, 7, sz - 8 + i, 2);
    }

  /* Timing patterns */

  for (int i = 8; i < sz - 8; i++)
    {
      uint8_t val = (i % 2 == 0) ? 3 : 2;
      if (qr->modules[6][i] == 0) set_module(qr, i, 6, val);
      if (qr->modules[i][6] == 0) set_module(qr, 6, i, val);
    }

  /* Dark module */

  set_module(qr, 8, sz - 8, 3);

  /* Alignment patterns */

  if (qr->version >= 2)
    {
      const uint8_t *pos = g_alignpos[qr->version - 1];
      int npos = 0;

      while (npos < 7 && pos[npos] != 0)
        {
          npos++;
        }

      for (int i = 0; i < npos; i++)
        {
          for (int j = 0; j < npos; j++)
            {
              /* Skip if overlapping with finder patterns */

              if (i == 0 && j == 0) continue;
              if (i == 0 && j == npos - 1) continue;
              if (i == npos - 1 && j == 0) continue;

              place_alignment_pattern(qr, pos[i], pos[j]);
            }
        }
    }

  /* Reserve format info areas (mark with value 2 = white fixed) */

  for (int i = 0; i < 9; i++)
    {
      if (qr->modules[8][i] == 0) set_module(qr, i, 8, 2);
      if (qr->modules[i][8] == 0) set_module(qr, 8, i, 2);
    }

  for (int i = 0; i < 8; i++)
    {
      if (qr->modules[8][sz - 1 - i] == 0)
        set_module(qr, sz - 1 - i, 8, 2);
      if (qr->modules[sz - 1 - i][8] == 0)
        set_module(qr, 8, sz - 1 - i, 2);
    }
}

/****************************************************************************
 * Name: place_data_bits
 *
 * Description:
 *   Place data + EC codewords in zigzag pattern.
 *
 ****************************************************************************/

static void place_data_bits(struct qrcode_s *qr,
                            const uint8_t *data, int data_len)
{
  int sz = qr->size;
  int bit_idx = 0;
  int total_bits = data_len * 8;

  /* Right-to-left column pairs, bottom-to-top / top-to-bottom */

  bool upward = true;

  for (int col = sz - 1; col >= 1; col -= 2)
    {
      /* Skip timing column */

      if (col == 6)
        {
          col = 5;
        }

      for (int step = 0; step < sz; step++)
        {
          int row = upward ? (sz - 1 - step) : step;

          for (int c = 0; c < 2; c++)
            {
              int x = col - c;

              /* Only place on empty modules (value 0) */

              if (qr->modules[row][x] != 0)
                {
                  continue;
                }

              bool black = false;
              if (bit_idx < total_bits)
                {
                  int byte_idx = bit_idx / 8;
                  int bit_pos  = 7 - (bit_idx % 8);
                  black = ((data[byte_idx] >> bit_pos) & 1) != 0;
                  bit_idx++;
                }

              qr->modules[row][x] = black ? 1 : 0;
            }
        }

      upward = !upward;
    }
}

/****************************************************************************
 * Name: apply_mask
 *
 * Description:
 *   Apply data masking pattern (0-7).
 *   Only data modules (value 0 or 1) are affected, not fixed (2, 3).
 *
 ****************************************************************************/

static void apply_mask(struct qrcode_s *qr, int mask)
{
  for (int y = 0; y < qr->size; y++)
    {
      for (int x = 0; x < qr->size; x++)
        {
          uint8_t v = qr->modules[y][x];
          if (v >= 2)
            {
              continue;  /* fixed pattern, don't mask */
            }

          bool invert = false;
          switch (mask)
            {
              case 0: invert = ((y + x) % 2 == 0); break;
              case 1: invert = (y % 2 == 0); break;
              case 2: invert = (x % 3 == 0); break;
              case 3: invert = ((y + x) % 3 == 0); break;
              case 4: invert = ((y / 2 + x / 3) % 2 == 0); break;
              case 5: invert = ((y * x) % 2 + (y * x) % 3 == 0); break;
              case 6: invert = (((y * x) % 2 + (y * x) % 3) % 2 == 0);
                      break;
              case 7: invert = (((y + x) % 2 + (y * x) % 3) % 2 == 0);
                      break;
            }

          if (invert)
            {
              qr->modules[y][x] = v ^ 1;
            }
        }
    }
}

/****************************************************************************
 * Name: place_format_info
 *
 * Description:
 *   Write the 15-bit format information string.
 *
 ****************************************************************************/

static void place_format_info(struct qrcode_s *qr, int ecl, int mask)
{
  uint16_t bits;

  if (ecl == QR_ECL_MEDIUM)
    {
      bits = g_format_med[mask];
    }
  else
    {
      bits = g_format_low[mask];
    }

  int sz = qr->size;

  /* Around top-left finder */

  for (int i = 0; i < 6; i++)
    {
      set_module(qr, 8, i, (bits >> i) & 1 ? 3 : 2);
    }

  set_module(qr, 8, 7, (bits >> 6) & 1 ? 3 : 2);
  set_module(qr, 8, 8, (bits >> 7) & 1 ? 3 : 2);
  set_module(qr, 7, 8, (bits >> 8) & 1 ? 3 : 2);

  for (int i = 9; i < 15; i++)
    {
      set_module(qr, 14 - i, 8, (bits >> i) & 1 ? 3 : 2);
    }

  /* Along top-right and bottom-left finders */

  for (int i = 0; i < 8; i++)
    {
      set_module(qr, sz - 1 - i, 8, (bits >> i) & 1 ? 3 : 2);
    }

  for (int i = 8; i < 15; i++)
    {
      set_module(qr, 8, sz - 15 + i, (bits >> i) & 1 ? 3 : 2);
    }
}

/****************************************************************************
 * Name: finalize_modules
 *
 * Description:
 *   Convert internal module values to final 0/1.
 *   Fixed values: 2 → 0 (white), 3 → 1 (black)
 *
 ****************************************************************************/

static void finalize_modules(struct qrcode_s *qr)
{
  for (int y = 0; y < qr->size; y++)
    {
      for (int x = 0; x < qr->size; x++)
        {
          uint8_t v = qr->modules[y][x];
          qr->modules[y][x] = (v == 1 || v == 3) ? 1 : 0;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: qrencode
 ****************************************************************************/

int qrencode(const uint8_t *data, size_t data_len, int ecl,
             struct qrcode_s *qr)
{
  int ecl_idx;

  gf_init();

  /* Map ECL to our table index (only LOW and MEDIUM supported) */

  ecl_idx = (ecl >= QR_ECL_MEDIUM) ? 1 : 0;

  /* Find smallest version that fits the data */

  int version = -1;
  const struct version_info_s *vi = NULL;

  for (int v = 1; v <= QR_MAX_VERSION; v++)
    {
      vi = &g_vinfo[v - 1][ecl_idx];

      /* Byte mode overhead: 4 bit mode indicator + 8/16 bit count */

      int overhead = (v <= 9) ? 12 : 20;  /* bits */
      int capacity = vi->data_codewords * 8 - overhead;

      if ((int)(data_len * 8) <= capacity)
        {
          version = v;
          break;
        }
    }

  if (version < 0)
    {
      return -1;  /* Data too large */
    }

  /* Initialize QR structure */

  memset(qr, 0, sizeof(*qr));
  qr->version = version;
  qr->size    = 17 + 4 * version;

  /* Encode data into codewords: byte mode (0100) */

  uint8_t codewords[MAX_DATA_CODEWORDS];
  memset(codewords, 0, sizeof(codewords));

  int bit = 0;

  /* Mode indicator: 0100 (byte mode) */

  codewords[0] = 0x40;
  bit = 4;

  /* Character count indicator */

  if (version <= 9)
    {
      /* 8-bit count */

      codewords[bit / 8] |= (data_len >> (8 - (bit % 8))) & 0xFF;
      bit += 8;
    }
  else
    {
      /* 16-bit count */

      int shift = 16;
      for (int b = 0; b < 16; b++)
        {
          shift--;
          int byte_idx = bit / 8;
          int bit_pos  = 7 - (bit % 8);

          if ((data_len >> shift) & 1)
            {
              codewords[byte_idx] |= (1 << bit_pos);
            }

          bit++;
        }
    }

  /* Data bits */

  for (size_t i = 0; i < data_len; i++)
    {
      for (int b = 7; b >= 0; b--)
        {
          int byte_idx = bit / 8;
          int bit_pos  = 7 - (bit % 8);

          if ((data[i] >> b) & 1)
            {
              codewords[byte_idx] |= (1 << bit_pos);
            }

          bit++;
        }
    }

  /* Terminator (up to 4 zero bits) */

  int total_data_bits = vi->data_codewords * 8;

  if (bit + 4 <= total_data_bits)
    {
      bit += 4;
    }
  else
    {
      bit = total_data_bits;
    }

  /* Pad to byte boundary */

  bit = ((bit + 7) / 8) * 8;

  /* Pad codewords */

  int cw_count = bit / 8;
  int pad_idx = 0;
  static const uint8_t pad_bytes[2] = {0xEC, 0x11};

  while (cw_count < vi->data_codewords)
    {
      codewords[cw_count++] = pad_bytes[pad_idx & 1];
      pad_idx++;
    }

  /* Reed-Solomon ECC */

  uint8_t ec_data[MAX_EC_CODEWORDS];
  int ec_len = vi->ec_per_block;
  int nblocks = vi->num_blocks;
  int data_per_block = vi->data_codewords / nblocks;

  /* Interleave data + EC blocks */

  uint8_t final_data[MAX_DATA_CODEWORDS + MAX_EC_CODEWORDS];
  int final_len = 0;

  /* For simplicity with 1-2 blocks: sequential data then EC */

  uint8_t ec_blocks[4][MAX_EC_CODEWORDS];

  for (int blk = 0; blk < nblocks; blk++)
    {
      rs_encode(&codewords[blk * data_per_block], data_per_block,
                ec_blocks[blk], ec_len);
    }

  /* Interleave data codewords */

  for (int i = 0; i < data_per_block; i++)
    {
      for (int blk = 0; blk < nblocks; blk++)
        {
          final_data[final_len++] =
            codewords[blk * data_per_block + i];
        }
    }

  /* Interleave EC codewords */

  for (int i = 0; i < ec_len; i++)
    {
      for (int blk = 0; blk < nblocks; blk++)
        {
          final_data[final_len++] = ec_blocks[blk][i];
        }
    }

  /* Build the QR code matrix */

  place_fixed_patterns(qr);
  place_data_bits(qr, final_data, final_len);

  /* Apply mask 0 (simplest and usually good enough) */

  apply_mask(qr, 0);
  place_format_info(qr, ecl_idx, 0);
  finalize_modules(qr);

  return 0;
}

/****************************************************************************
 * Name: qr_print_simple
 ****************************************************************************/

void qr_print_simple(FILE *fp, const struct qrcode_s *qr)
{
  /* 4-module quiet zone on each side */

  int qzone = 4;

  for (int y = -qzone; y < qr->size + qzone; y++)
    {
      for (int x = -qzone; x < qr->size + qzone; x++)
        {
          bool black = false;

          if (x >= 0 && x < qr->size && y >= 0 && y < qr->size)
            {
              black = (qr->modules[y][x] != 0);
            }

          fputs(black ? "##" : "  ", fp);
        }

      fputc('\n', fp);
    }
}

/****************************************************************************
 * Name: qr_print_ascii
 *
 * Description:
 *   Compact ASCII art using Unicode half-block characters.
 *   Each character represents 2 rows of modules.
 *
 ****************************************************************************/

void qr_print_ascii(FILE *fp, const struct qrcode_s *qr)
{
  int qzone = 2;

  /* Process 2 rows at a time */

  for (int y = -qzone; y < qr->size + qzone; y += 2)
    {
      for (int x = -qzone; x < qr->size + qzone; x++)
        {
          bool top = false, bot = false;

          if (x >= 0 && x < qr->size)
            {
              if (y >= 0 && y < qr->size)
                {
                  top = (qr->modules[y][x] != 0);
                }

              if (y + 1 >= 0 && y + 1 < qr->size)
                {
                  bot = (qr->modules[y + 1][x] != 0);
                }
            }

          if (top && bot)
            {
              fputs("\xe2\x96\x88", fp);  /* FULL BLOCK */
            }
          else if (top)
            {
              fputs("\xe2\x96\x80", fp);  /* UPPER HALF */
            }
          else if (bot)
            {
              fputs("\xe2\x96\x84", fp);  /* LOWER HALF */
            }
          else
            {
              fputc(' ', fp);
            }
        }

      fputc('\n', fp);
    }
}
