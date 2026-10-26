/*
 * mdec.c - PlayStation MDEC (macroblock decoder) reimplementation.
 *
 * Words written to 1F801820h (or sent by DMA0) enter a 32-word input FIFO.  The decoder
 * takes a command word from it, then its parameters: quant tables (cmd 2), the IDCT scale
 * table (cmd 3) or run-length coded macroblocks (cmd 1).  Each decoded 8x8 Y block (or the
 * single block of a monochrome macroblock) is encoded into a pixel buffer, which drains
 * into a 32-word output FIFO read through 1F801820h or DMA1.  The pipeline runs until it
 * stalls (input FIFO empty, or output FIFO full), so the FIFO / busy / request status bits
 * and the DMA pacing behave like the chip: DMA0 only finds an empty input FIFO again once
 * enough output has been read.
 *
 * Arithmetic, from the psx-spx MDEC chapter; where psx-spx marks its pseudo code as
 * approximate, the fixed-point details are the hardware-verified ones (Mednafen):
 *   - coefficients carry 4 fractional bits: DC = ci*qt[0], AC = (ci*qt[k]*qscale) >> 3,
 *     times 16 and moved 0.5 towards zero, saturated to -4000h..3FFFh.  qscale = 0 is the
 *     documented special mode (ci*2, no quant table, no zigzag).
 *   - IDCT: scale table entries >> 3 (only the upper 13 bits are used), columns first,
 *     each pass rounded as (sum + 4000h) >> 15; the result is wrapped to 9 bits and
 *     saturated to -128..127.
 *   - YUV->RGB: R = Y + (359*Cr + 80h) >> 8, G = Y + (((-88*Cb) & ~1Fh) + ((-183*Cr) & ~7)
 *     + 80h) >> 8, B = Y + (454*Cb + 80h) >> 8, each wrapped to 9 bits and saturated.
 *   - 15bpp: (c + 4) >> 3 saturated to 31 per component; 4bpp: top nibble of Y + 8.
 */
#include "psx.h"
#include "mdec.h"
#include <string.h>

#define FIFO MDEC_FIFO_WORDS

/* zigzag[raster position] = coefficient order (psx-spx); zagzig is the inverse */
static const uint8_t zigzag[64] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63,
};
static uint8_t zagzig[64];

/* standard scale table (psx-spx); loaded at power-on, games upload it with cmd 3 anyway */
static const uint16_t std_scale[64] = {
    0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82, 0x5A82,
    0x7D8A, 0x6A6D, 0x471C, 0x18F8, 0xE707, 0xB8E3, 0x9592, 0x8275,
    0x7641, 0x30FB, 0xCF04, 0x89BE, 0x89BE, 0xCF04, 0x30FB, 0x7641,
    0x6A6D, 0xE707, 0x8275, 0xB8E3, 0x471C, 0x7D8A, 0x18F8, 0x9592,
    0x5A82, 0xA57D, 0xA57D, 0x5A82, 0x5A82, 0xA57D, 0xA57D, 0x5A82,
    0x471C, 0x8275, 0x18F8, 0x6A6D, 0x9592, 0xE707, 0x7D8A, 0xB8E3,
    0x30FB, 0x89BE, 0x7641, 0xCF04, 0xCF04, 0x7641, 0x89BE, 0x30FB,
    0x18F8, 0xB8E3, 0x6A6D, 0x8275, 0x7D8A, 0x9592, 0x471C, 0xE707,
};

static struct {
    uint32_t in[FIFO];  int in_rd, in_n;            /* input FIFO */
    uint32_t out[FIFO]; int out_rd, out_n;          /* output FIFO */
    uint8_t  out_blk[FIFO];                         /* block number of each output word */
    uint32_t pix[48];   int pix_rd, pix_n;          /* encoded 8x8 block waiting for the FIFO */
    uint8_t  pix_blk;
    uint32_t cmd;                                   /* last command word */
    uint32_t ctrl;                                  /* control bits 29 (DMA1) / 30 (DMA0) */
    bool     busy;                                  /* receiving or processing parameters */
    uint16_t remain;                                /* parameter words remaining - 1 (FFFFh: none) */
    int      tab_pos;                               /* cmd 2/3: next table entry */
    uint8_t  iq[2][64];                             /* quant tables (zigzag order): luminance, colour */
    int16_t  scale[64];                             /* IDCT matrix [frequency*8 + position], >> 3 */
    int16_t  coef[64];                              /* block being decoded, row = vertical frequency */
    int      k;                                     /* next coefficient (zigzag order), 0 = DC */
    int      qscale;
    int      blk;                                   /* 0 Cr, 1 Cb, 2..5 Y1..Y4 (mono: always 2) */
    int8_t   cr[64], cb[64], y[64];
    uint32_t out_pos;                               /* words read since the decode command */
} m;

static inline int depth(void) { return (int)(m.cmd >> 27) & 3; }   /* 0 4bpp, 1 8bpp, 2 24bpp, 3 15bpp */
static inline bool colour(void) { return (m.cmd >> 28) & 1; }
static inline bool out_signed(void) { return (m.cmd >> 26) & 1; }

static inline int sext10(int v) { return (v & 0x200) ? v - 0x400 : v; }
/* wrap to signed 9 bits, then saturate to signed 8 bits */
static inline int clamp9(int v) {
    v &= 0x1FF;
    if (v & 0x100) v -= 0x200;
    return v < -128 ? -128 : v > 127 ? 127 : v;
}

/* ---------------------------------------------------------------- IDCT */
static void idct(const int16_t *in, int8_t *out) {
    int16_t t[64];
    for (int h = 0; h < 8; h++)             /* columns: vertical frequency -> row */
        for (int y = 0; y < 8; y++) {
            int32_t s = 0;
            for (int v = 0; v < 8; v++) s += in[v * 8 + h] * m.scale[v * 8 + y];
            t[y * 8 + h] = (int16_t)((s + 0x4000) >> 15);
        }
    for (int y = 0; y < 8; y++)             /* rows: horizontal frequency -> column */
        for (int x = 0; x < 8; x++) {
            int32_t s = 0;
            for (int h = 0; h < 8; h++) s += t[y * 8 + h] * m.scale[h * 8 + x];
            out[y * 8 + x] = (int8_t)clamp9((s + 0x4000) >> 15);
        }
}

/* ---------------------------------------------------------------- output encoding */
static inline uint16_t rgb555(int r, int g, int b) {
    r = (r + 4) >> 3; g = (g + 4) >> 3; b = (b + 4) >> 3;
    if (r > 31) r = 31;
    if (g > 31) g = 31;
    if (b > 31) b = 31;
    return (uint16_t)(r | (g << 5) | (b << 10));
}

/* encode the finished Y block `ybn` (0..3 = Y1..Y4; monochrome: 0) into the pixel buffer */
static void encode(int ybn) {
    uint8_t b[192];
    int words = 0;
    switch (depth()) {
    case 0: {   /* 4bpp: first pixel in the low nibble */
        uint8_t x = out_signed() ? 0x00 : 0x88;
        for (int i = 0; i < 64; i += 2) {
            int p0 = m.y[i] + 8, p1 = m.y[i + 1] + 8;
            if (p0 > 127) p0 = 127;
            if (p1 > 127) p1 = 127;
            b[i / 2] = (uint8_t)((((uint8_t)p0 >> 4) | ((uint8_t)p1 & 0xF0)) ^ x);
        }
        words = 8;
        break;
    }
    case 1: {   /* 8bpp */
        uint8_t x = out_signed() ? 0x00 : 0x80;
        for (int i = 0; i < 64; i++) b[i] = (uint8_t)m.y[i] ^ x;
        words = 16;
        break;
    }
    default: {  /* 24bpp (R,G,B bytes) / 15bpp */
        bool bpp24 = depth() == 2;
        uint8_t  x24 = out_signed() ? 0x80 : 0x00;
        uint16_t x15 = (uint16_t)(((m.cmd >> 25) & 1 ? 0x8000 : 0) | (out_signed() ? 0x4210 : 0));
        int n = 0;
        for (int y = 0; y < 8; y++)
            for (int xx = 0; xx < 8; xx++) {
                int c = ((y >> 1) + (ybn & 2) * 2) * 8 + (xx >> 1) + (ybn & 1) * 4;
                int Y = m.y[y * 8 + xx], cr = m.cr[c], cb = m.cb[c];
                int r = (clamp9(Y + ((359 * cr + 0x80) >> 8)) + 128);
                int g = (clamp9(Y + ((((-88 * cb) & ~0x1F) + ((-183 * cr) & ~0x07) + 0x80) >> 8)) + 128);
                int bl = (clamp9(Y + ((454 * cb + 0x80) >> 8)) + 128);
                if (bpp24) {
                    b[n++] = (uint8_t)r ^ x24;
                    b[n++] = (uint8_t)g ^ x24;
                    b[n++] = (uint8_t)bl ^ x24;
                } else {
                    uint16_t p = rgb555(r, g, bl) ^ x15;
                    b[n++] = (uint8_t)p;
                    b[n++] = (uint8_t)(p >> 8);
                }
            }
        words = n / 4;
        break;
    }
    }
    for (int i = 0; i < words; i++)
        m.pix[i] = b[i * 4] | ((uint32_t)b[i * 4 + 1] << 8) | ((uint32_t)b[i * 4 + 2] << 16) | ((uint32_t)b[i * 4 + 3] << 24);
    m.pix_rd = 0;
    m.pix_n = words;
    m.pix_blk = (uint8_t)(colour() ? ybn : 4);
}

/* ---------------------------------------------------------------- run-length decoding */
static void block_done(void) {
    if (colour()) {
        idct(m.coef, m.blk == 0 ? m.cr : m.blk == 1 ? m.cb : m.y);
        if (m.blk >= 2) encode(m.blk - 2);
        m.blk = (m.blk + 1) % 6;
    } else {
        idct(m.coef, m.y);
        encode(0);
    }
}

/* store halfword v as coefficient m.k */
static void store(uint16_t v) {
    const uint8_t *qt = m.iq[colour() && m.blk < 2];
    int ci = sext10(v & 0x3FF), val;
    int q = m.k ? qt[m.k] * m.qscale : qt[0];      /* the DC value has no qscale */
    if (m.qscale == 0 || q == 0) {
        val = ci * 2 * 16;                         /* special mode: no quant table */
    } else {
        int p = m.k ? (ci * q) >> 3 : ci * q;
        val = p * 16 + (ci < 0 ? 8 : ci > 0 ? -8 : 0);
    }
    if (val < -0x4000) val = -0x4000;
    if (val > 0x3FFF) val = 0x3FFF;
    m.coef[m.qscale ? zagzig[m.k] : m.k] = (int16_t)val;   /* qscale 0: no zigzag */
    m.k++;
}

static void decode_halfword(uint16_t v) {
    if (m.k == 0) {
        if (v == 0xFE00) return;                   /* padding before a block */
        memset(m.coef, 0, sizeof m.coef);
        m.qscale = v >> 10;
        store(v);
    } else if (v == 0xFE00) {
        m.k = 64;                                  /* end of block */
    } else {
        m.k += v >> 10;                            /* skipped (zero) coefficients */
        if (m.k < 64) store(v);
    }
    if (m.k >= 64) {
        m.k = 0;
        block_done();
    }
}

/* ---------------------------------------------------------------- command processing */
static void start_command(uint32_t w) {
    LOGT("MDEC command %08X", w);
    m.cmd = w;
    switch (w >> 29) {
    case 1:     /* decode macroblocks: drop stale output, restart at the first block */
        m.remain = (uint16_t)w;
        m.out_n = m.out_rd = 0;
        m.pix_n = m.pix_rd = 0;
        m.k = 0;
        m.blk = colour() ? 0 : 2;
        m.out_pos = 0;
        break;
    case 2:     /* quant tables: 64 bytes luminance (+ 64 bytes colour) */
        m.remain = (w & 1) ? 32 : 16;
        m.tab_pos = 0;
        break;
    case 3:     /* scale table: 64 halfwords */
        m.remain = 32;
        m.tab_pos = 0;
        break;
    default:    /* no function: bits 0-15 shown as is, no parameters */
        m.remain = (uint16_t)w;
        return;
    }
    m.remain--;
    m.busy = true;
}

static void push_out(uint32_t w, uint8_t blk) {
    int i = (m.out_rd + m.out_n) % FIFO;
    m.out[i] = w;
    m.out_blk[i] = blk;
    m.out_n++;
}

static uint32_t pop_out(void) {
    uint32_t w = m.out[m.out_rd];
    m.out_rd = (m.out_rd + 1) % FIFO;
    m.out_n--;
    m.out_pos++;
    return w;
}

static void push_in(uint32_t w) {
    if (m.in_n >= FIFO) return;                    /* FIFO full: the word is lost */
    m.in[(m.in_rd + m.in_n) % FIFO] = w;
    m.in_n++;
}

/* run the decoder until it stalls */
static void run(void) {
    for (;;) {
        while (m.pix_rd < m.pix_n && m.out_n < FIFO) push_out(m.pix[m.pix_rd++], m.pix_blk);
        if (m.pix_rd < m.pix_n) return;            /* output FIFO full */
        if (m.busy && m.remain == 0xFFFF) m.busy = false;
        if (!m.in_n) return;
        uint32_t w = m.in[m.in_rd];
        m.in_rd = (m.in_rd + 1) % FIFO;
        m.in_n--;
        if (!m.busy) {
            start_command(w);
            continue;
        }
        switch (m.cmd >> 29) {
        case 1:
            decode_halfword((uint16_t)w);
            decode_halfword((uint16_t)(w >> 16));
            break;
        case 2:
            for (int i = 0; i < 4; i++, m.tab_pos++)
                m.iq[(m.tab_pos >> 6) & 1][m.tab_pos & 63] = (uint8_t)(w >> (i * 8));
            break;
        case 3:
            for (int i = 0; i < 2; i++, m.tab_pos++)
                m.scale[m.tab_pos & 63] = (int16_t)((int16_t)(uint16_t)(w >> (i * 16)) >> 3);
            break;
        }
        m.remain--;
    }
}

static void reset(void) {
    m.in_rd = m.in_n = 0;
    m.out_rd = m.out_n = 0;
    m.pix_rd = m.pix_n = 0;
    m.cmd = 0;
    m.busy = false;
    m.remain = 0;
    m.tab_pos = 0;
    m.k = 0;
    m.qscale = 0;
    m.blk = 0;
    m.out_pos = 0;
    memset(m.coef, 0, sizeof m.coef);
}

void mdec_init(void) {
    memset(&m, 0, sizeof m);
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
    for (int i = 0; i < 64; i++) m.scale[i] = (int16_t)((int16_t)std_scale[i] >> 3);
    reset();
}

/* ---------------------------------------------------------------- registers */
static uint32_t status(void) {
    uint32_t s = 0;
    int blk = m.out_n ? m.out_blk[m.out_rd] : colour() ? (m.blk + 4) % 6 : 4;
    if (!m.out_n) s |= 1u << 31;
    if (m.in_n == FIFO || (m.busy && m.remain == 0xFFFF)) s |= 1u << 30;   /* full / last word in */
    if (m.busy) s |= 1u << 29;
    if (mdec_dma_in_request()) s |= 1u << 28;
    if ((m.ctrl & (1u << 29)) && m.out_n == FIFO) s |= 1u << 27;
    s |= ((m.cmd >> 25) & 0xF) << 23;
    s |= (uint32_t)blk << 16;
    s |= m.remain;
    return s;
}

uint32_t mdec_read(uint32_t off) {
    if (off & 4) return status();
    if (!m.out_n) return 0;
    uint32_t w = pop_out();     /* CPU reads see the 8x8 blocks unordered (Y1..Y4) */
    run();
    dma_mdec_update();
    return w;
}

void mdec_write(uint32_t off, uint32_t v) {
    if (off & 4) {
        if (v & 0x80000000u) reset();
        m.ctrl = v & 0x7FFFFFFFu;
    } else {
        push_in(v);
        run();
    }
    dma_mdec_update();
}

/* ---------------------------------------------------------------- DMA side */
bool mdec_dma_in_request(void) {
    return (m.ctrl & (1u << 30)) && m.busy && m.remain != 0xFFFF && m.in_n == 0;
}

void mdec_dma_in(uint32_t w) {
    push_in(w);
    run();
}

bool mdec_dma_out_ready(void) {
    return (m.ctrl & (1u << 29)) && m.out_n;
}

uint32_t mdec_dma_out(int32_t *offs) {
    /* DMA1 re-orders the four 8x8 Y blocks into a 16x16 macroblock: row r of Y1/Y3 lands
       r rows further down, Y2/Y4 also move back over the 7 rows of the block before */
    int wws = depth() == 2 ? 6 : depth() == 3 ? 4 : 0;    /* words per 8-pixel row */
    *offs = 0;
    if (!m.out_n) return 0;
    if (wws) {
        uint32_t row = m.out_pos / (uint32_t)wws;
        *offs = (int32_t)(row & 7) * wws - ((row & 8) ? wws * 7 : 0);
    }
    uint32_t w = pop_out();
    run();
    return w;
}
