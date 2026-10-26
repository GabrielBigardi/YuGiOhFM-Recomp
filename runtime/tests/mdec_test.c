/*
 * mdec_test.c - MDEC tests: hand-derived cases from the psx-spx MDEC documentation
 * (expected pixel values worked out by hand from the documented formulas, see comments).
 *
 * Build (from the project root):
 *   clang -std=gnu11 -O2 -Wall -Wextra -Iruntime runtime/mdec.c runtime/tests/mdec_test.c -o build/tests/mdec_test.exe
 */
#include "mdec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- runtime stubs ---------------------------------------------------------------- */
static int g_updates;
void dma_mdec_update(void) { g_updates++; }
int g_log_level;
void log_msg(int level, const char *fmt, ...) { (void)level; (void)fmt; }

/* ---- helpers ------------------------------------------------------------------------ */
static int g_pass, g_fail;
#define CHECK(cond, ...) do { if (cond) { g_pass++; } else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)
#define CHECK_EQ(got, want, what) do { uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want); CHECK(g_ == w_, "%s: got %08X want %08X", what, g_, w_); } while (0)

static uint32_t stat(void) { return mdec_read(4); }
static void ctrl(uint32_t v) { mdec_write(4, v); }
static void put(uint32_t v) { mdec_write(0, v); }

#define EOB 0xFE00
static uint16_t hw[2048];
static int nhw;
static void H(uint16_t v) { hw[nhw++] = v; }
static uint16_t DC(int qs, int v) { return (uint16_t)((qs << 10) | (v & 0x3FF)); }
static uint16_t AC(int run, int v) { return (uint16_t)((run << 10) | (v & 0x3FF)); }
static void flat(int dc) { H(DC(1, dc)); H(EOB); }

static uint8_t luma[64], chroma[64];
static void upload_iq(int with_colour) {
    put(0x40000000u | (uint32_t)with_colour);
    for (int t = 0; t < (with_colour ? 2 : 1); t++) {
        const uint8_t *q = t ? chroma : luma;
        for (int i = 0; i < 16; i++)
            put(q[i * 4] | (uint32_t)q[i * 4 + 1] << 8 | (uint32_t)q[i * 4 + 2] << 16 | (uint32_t)q[i * 4 + 3] << 24);
    }
}

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
static void upload_scale(const uint16_t *s) {
    put(0x60000000u);
    for (int i = 0; i < 32; i++) put(s[i * 2] | (uint32_t)s[i * 2 + 1] << 16);
}

/* fresh MDEC with every quant entry = 2 and the standard scale table */
static void setup(void) {
    mdec_init();
    memset(luma, 2, sizeof luma);
    memset(chroma, 2, sizeof chroma);
    upload_iq(1);
    upload_scale(std_scale);
}

static uint32_t out[4096];
static int nout;
static void drain(void) {
    while (!(stat() & 0x80000000u) && nout < 4096) out[nout++] = mdec_read(0);
}

/* run one decode command over hw[0..nhw) through the CPU ports, collecting the output */
static int decode(uint32_t cmd) {
    int words = (nhw + 1) / 2;
    nout = 0;
    put(cmd | (uint32_t)words);
    for (int i = 0; i < words; i++) {
        uint16_t lo = hw[i * 2], hi = (i * 2 + 1 < nhw) ? hw[i * 2 + 1] : EOB;
        drain();
        put(lo | (uint32_t)hi << 16);
    }
    drain();
    return nout;
}

static int all_words(int from, int n, uint32_t v) {
    for (int i = from; i < from + n; i++) if (out[i] != v) return 0;
    return 1;
}

/* signed 8bpp output byte (x,y) of block b */
static int8_t px8(int b, int x, int y) { return (int8_t)(out[b * 16 + y * 2 + x / 4] >> ((x & 3) * 8)); }

#define CMD_DECODE 0x20000000u
#define D4   0x00000000u
#define D8   0x08000000u
#define D24  0x10000000u
#define D15  0x18000000u
#define SGN  0x04000000u
#define STP  0x02000000u

/* ---- tests ---------------------------------------------------------------------------- */
static void test_reset_status(void) {
    mdec_init();
    CHECK_EQ(stat(), 0x80040000u, "status after init");
    ctrl(0x60000000u);
    CHECK_EQ(stat(), 0x80040000u, "DMA enabled, no command: no requests");
    put(0x1E001234u);   /* MDEC(0): bits 25-28 -> status 23-26, bits 0-15 shown without -1 */
    CHECK_EQ(stat(), 0x87841234u, "MDEC(0) reflected bits");
    put(0xE0000005u);   /* MDEC(7) acts as MDEC(0) */
    CHECK_EQ(stat(), 0x80040005u, "MDEC(7) like MDEC(0)");
    ctrl(0x80000000u);
    CHECK_EQ(stat(), 0x80040000u, "status after reset");
    CHECK(!mdec_dma_in_request() && !mdec_dma_out_ready(), "no DMA requests after reset (DMA disabled)");

    int u = g_updates;
    (void)stat();
    CHECK(g_updates == u, "status read does not notify the DMA");
    put(0);
    CHECK(g_updates == u + 1, "data write notifies the DMA");
    ctrl(0);
    CHECK(g_updates == u + 2, "control write notifies the DMA");
}

static void test_tables(void) {
    mdec_init();
    ctrl(0x60000000u);
    put(0x40000001u);
    CHECK_EQ(stat(), 0xB004001Fu, "set_iqtab(colour): busy, DREQ0, 31 words left-1");
    CHECK(mdec_dma_in_request(), "DREQ0 while expecting table words");
    put(0x01010101u);
    CHECK_EQ(stat(), 0xB004001Eu, "set_iqtab after one word");
    for (int i = 0; i < 31; i++) put(0x01010101u);
    CHECK_EQ(stat(), 0x8004FFFFu, "set_iqtab complete");
    CHECK(!mdec_dma_in_request(), "no DREQ0 after the last word");

    put(0x40000000u);
    CHECK_EQ(stat(), 0xB004000Fu, "set_iqtab(luma only): 16 words");
    for (int i = 0; i < 16; i++) put(0x02020202u);
    CHECK_EQ(stat(), 0x8004FFFFu, "luma-only table complete");

    put(0x60000000u);
    CHECK_EQ(stat(), 0xB004001Fu, "set_scale: 32 words");
    for (int i = 0; i < 32; i++) put(0);
    CHECK_EQ(stat(), 0x8004FFFFu, "set_scale complete");

    ctrl(0);
    put(0x40000001u);
    CHECK_EQ(stat(), 0xA004001Fu, "DREQ0 needs control bit 30");
    ctrl(0x80000000u);
    CHECK_EQ(stat(), 0x80040000u, "reset aborts the command");

    /* zero scale table -> every pixel 0 whatever the coefficients; then the standard one */
    setup();
    static const uint16_t zero[64];
    upload_scale(zero);
    nhw = 0; flat(300);
    decode(CMD_DECODE | D8 | SGN);
    CHECK(nout == 16 && all_words(0, 16, 0), "zero scale table gives a black block");
    upload_scale(std_scale);
    decode(CMD_DECODE | D8 | SGN);
    CHECK(nout == 16 && !all_words(0, 16, 0), "scale table reloaded");
}

/* DC-only blocks, quant 2.  DC 50 -> coefficient (100*16 - 8) = 1592.
 * pass 1: (1592*2896 + 4000h) >> 15 = 141, pass 2: (141*2896 + 4000h) >> 15 = 12.
 * DC -50: -1592 -> -141 -> -12. */
static void test_mono8_dc(void) {
    setup();
    ctrl(0x60000000u);
    nhw = 0; flat(50);
    put(CMD_DECODE | D8 | 1);
    CHECK_EQ(stat(), 0xB2040000u, "8bpp decode: busy, DREQ0, depth bits, 0 words left-1");
    put(hw[0] | (uint32_t)hw[1] << 16);
    CHECK_EQ(stat(), 0x0204FFFFu, "8bpp block decoded: output, mono block 4, idle");
    nout = 0; drain();
    CHECK(nout == 16 && all_words(0, 16, 0x8C8C8C8Cu), "8bpp unsigned DC 50 -> 128+12");

    CHECK(decode(CMD_DECODE | D8 | SGN) == 16 && all_words(0, 16, 0x0C0C0C0Cu), "8bpp signed DC 50 -> 12");
    nhw = 0; flat(-50);
    CHECK(decode(CMD_DECODE | D8) == 16 && all_words(0, 16, 0x74747474u), "8bpp unsigned DC -50 -> 128-12");
    CHECK(decode(CMD_DECODE | D8 | SGN) == 16 && all_words(0, 16, 0xF4F4F4F4u), "8bpp signed DC -50 -> -12");

    /* 4bpp: (Y + 8) top nibble, first pixel in the low nibble; Y=12 -> 1, Y=-12 -> F */
    nhw = 0; flat(50);
    CHECK(decode(CMD_DECODE | D4) == 8 && all_words(0, 8, 0x99999999u), "4bpp unsigned Y=12");
    CHECK(decode(CMD_DECODE | D4 | SGN) == 8 && all_words(0, 8, 0x11111111u), "4bpp signed Y=12");
    nhw = 0; flat(-50);
    CHECK(decode(CMD_DECODE | D4) == 8 && all_words(0, 8, 0x77777777u), "4bpp unsigned Y=-12");
    CHECK(decode(CMD_DECODE | D4 | SGN) == 8 && all_words(0, 8, 0xFFFFFFFFu), "4bpp signed Y=-12");
    /* DC 508 -> 16248 -> 1436 -> Y=127; 4bpp: 127+8 saturates to 127 -> nibble 7 */
    nhw = 0; flat(508);
    CHECK(decode(CMD_DECODE | D8 | SGN) == 16 && all_words(0, 16, 0x7F7F7F7Fu), "8bpp Y=127");
    CHECK(decode(CMD_DECODE | D4 | SGN) == 8 && all_words(0, 8, 0x77777777u), "4bpp signed Y=127 saturates");
    CHECK(decode(CMD_DECODE | D4) == 8 && all_words(0, 8, 0xFFFFFFFFu), "4bpp unsigned Y=127");
    /* the 0.5 bias matters here: DC -474 -> -15168+8 = -15160 -> -1340 -> -118 (-119 without) */
    nhw = 0; flat(-474);
    CHECK(decode(CMD_DECODE | D8 | SGN) == 16 && all_words(0, 16, 0x8A8A8A8Au), "8bpp DC -474 -> -118 (got %08X)", out[0]);
}

/* One AC coefficient: qscale 4, qt[k]=3, value 10 -> (10*12) >> 3 = 15 -> 15*16 - 8 = 232.
 * Scale rows: row 0 = 2896, row 1 >> 3 = 4017 3405 2275 799 -800 -2276 -3406 -4018.
 * k=1 (horizontal frequency 1): pass 1 gives 21 in column 1 of every row, pass 2
 *   (21*s + 4000h) >> 15 = 3 2 1 1 -1 -1 -2 -3 across each row.
 * k=2 (vertical frequency 1): pass 1 gives 28 24 16 6 -6 -16 -24 -28 down column 0,
 *   pass 2 (t*2896 + 4000h) >> 15 = 2 2 1 1 -1 -1 -2 -2 down each column
 *   (the columns-first rounding makes the two orientations differ). */
static void test_pattern(void) {
    setup();
    memset(luma, 3, sizeof luma);
    upload_iq(0);
    static const int8_t hprof[8] = { 3, 2, 1, 1, -1, -1, -2, -3 };
    static const int8_t vprof[8] = { 2, 2, 1, 1, -1, -1, -2, -2 };

    nhw = 0; H(DC(4, 0)); H(AC(0, 10)); H(EOB);
    CHECK(decode(CMD_DECODE | D8 | SGN) == 16, "pattern block output size");
    int ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (px8(0, x, y) != hprof[x]) ok = 0;
    CHECK(ok, "horizontal frequency 1 profile (row 0: %d %d %d %d %d %d %d %d)",
          px8(0, 0, 0), px8(0, 1, 0), px8(0, 2, 0), px8(0, 3, 0), px8(0, 4, 0), px8(0, 5, 0), px8(0, 6, 0), px8(0, 7, 0));
    CHECK_EQ(out[0], 0x01010203u, "row 0 word 0");
    CHECK_EQ(out[1], 0xFDFEFFFFu, "row 0 word 1");

    nhw = 0; H(DC(4, 0)); H(AC(1, 10)); H(EOB);
    decode(CMD_DECODE | D8 | SGN);
    ok = 1;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) if (px8(0, x, y) != vprof[y]) ok = 0;
    CHECK(ok, "vertical frequency 1 profile (column 0: %d %d %d %d %d %d %d %d)",
          px8(0, 0, 0), px8(0, 0, 1), px8(0, 0, 2), px8(0, 0, 3), px8(0, 0, 4), px8(0, 0, 5), px8(0, 0, 6), px8(0, 0, 7));

    /* negative value: -10 -> (-120) >> 3 = -15 -> -15*16 + 8 = -232: exact negation */
    nhw = 0; H(DC(4, 0)); H(AC(0, -10)); H(EOB);
    decode(CMD_DECODE | D8 | SGN);
    CHECK_EQ(out[0], 0xFFFFFEFDu, "negated pattern row 0 word 0 (-3 -2 -1 -1)");
    CHECK_EQ(out[1], 0x03020101u, "negated pattern row 0 word 1 (1 1 2 3)");
}

/* several coefficients against a floating point IDCT (basis = scale table / 10000h) */
static void test_float_reference(void) {
    setup();
    for (int i = 0; i < 64; i++) luma[i] = (uint8_t)(1 + i);
    upload_iq(0);
    static const int runs[] = { 0, 2, 5, 10, 20 }, vals[] = { -7, 12, 3, -20, 9 };
    int qs = 2, dcv = 30;
    nhw = 0; H(DC(qs, dcv));
    for (int i = 0; i < 5; i++) H(AC(runs[i], vals[i]));
    H(EOB);
    decode(CMD_DECODE | D8 | SGN);

    double F[64] = { 0 };
    static const uint8_t zz[64] = {
         0,  1,  8, 16,  9,  2,  3, 10, 17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };
    /* documented coefficient values: 4 fractional bits, 0.5 towards zero */
    F[0] = (dcv * luma[0] * 16 - 8) / 16.0;
    for (int i = 0, k = 0; i < 5; i++) {
        k += runs[i] + 1;
        int p = (vals[i] * luma[k] * qs) >> 3;
        F[zz[k]] = (p * 16 + (vals[i] < 0 ? 8 : -8)) / 16.0;
    }
    int ok = 1, worst = 0;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            double s = 0;
            for (int v = 0; v < 8; v++)
                for (int h = 0; h < 8; h++)
                    s += F[v * 8 + h] * (int16_t)std_scale[v * 8 + y] / 65536.0 * (int16_t)std_scale[h * 8 + x] / 65536.0;
            int ref = (int)(s < 0 ? s - 0.5 : s + 0.5);
            if (ref > 127) ref = 127;
            if (ref < -128) ref = -128;
            int d = abs(px8(0, x, y) - ref);
            if (d > worst) worst = d;
            if (d > 1) ok = 0;
        }
    CHECK(ok, "IDCT within 1 of the float reference (worst %d)", worst);
}

/* qscale 0: coefficient = value*2 without quant table, stored without zigzag */
static void test_qscale0(void) {
    setup();
    memset(luma, 7, sizeof luma);
    upload_iq(0);
    /* DC 20 -> 40*16 = 640 -> (640*2896 + 4000h) >> 15 = 57 -> 5 (quant 7 would give 17) */
    nhw = 0; H(DC(0, 20)); H(EOB);
    CHECK(decode(CMD_DECODE | D8 | SGN) == 16 && all_words(0, 16, 0x05050505u), "qscale 0 DC ignores the quant table");
    /* second value lands at raster position 2 (horizontal frequency 2), not zigzag 2 (vertical) */
    nhw = 0; H(DC(0, 0)); H(AC(1, 10)); H(EOB);
    decode(CMD_DECODE | D8 | SGN);
    int rows_equal = 1;
    for (int y = 1; y < 8; y++) for (int x = 0; x < 8; x++) if (px8(0, x, y) != px8(0, x, 0)) rows_equal = 0;
    CHECK(rows_equal && px8(0, 0, 0) != px8(0, 3, 0), "qscale 0 stores without zigzag");
    /* the same with qscale 1 goes to the vertical frequency */
    nhw = 0; H(DC(1, 0)); H(AC(1, 10)); H(EOB);
    decode(CMD_DECODE | D8 | SGN);
    int cols_equal = 1;
    for (int y = 0; y < 8; y++) for (int x = 1; x < 8; x++) if (px8(0, x, y) != px8(0, 0, y)) cols_equal = 0;
    CHECK(cols_equal && px8(0, 0, 0) != px8(0, 0, 7), "qscale 1 uses the zigzag order");
}

static void test_rle(void) {
    setup();
    /* padding before blocks, a block fully defined up to coefficient 63 without EOB,
       a run past the end of the block, EOB + padding after a block */
    nhw = 0;
    H(EOB); H(EOB);
    H(DC(1, 50)); H(AC(62, 1));             /* k = 63: block complete, no EOB */
    H(DC(1, -50)); H(EOB);                  /* starts right away */
    H(DC(1, 50)); H(AC(63, 5));             /* k = 64: value dropped, block complete */
    H(EOB); H(EOB);                         /* padding */
    flat(-50);
    CHECK(decode(CMD_DECODE | D8) == 64, "four blocks decoded (%d words)", nout);
    int near12 = 1;
    for (int i = 0; i < 64; i++) {
        int v = (int8_t)(((const uint8_t *)out)[i] ^ 0x80);
        if (v < 11 || v > 13) near12 = 0;
    }
    CHECK(near12, "block ending at coefficient 63 is DC 50 plus a tiny AC");
    CHECK(all_words(16, 16, 0x74747474u), "block after a block without EOB");
    CHECK(all_words(32, 16, 0x8C8C8C8Cu), "run past coefficient 63 ends the block");
    CHECK(all_words(48, 16, 0x74747474u), "block after EOB padding");
}

/* Colour macroblock, flat.  Luma quant 2, colour quant 4 (luma-only upload must keep it).
 * Y: DC 50 -> 12.  Cr: DC 100 -> 400*16-8 = 6392 -> 565 -> 50.  Cb: DC -60 -> -3832 -> -339 -> -30.
 * R = 12 + ((359*50 + 80h) >> 8) = 12 + 70 = 82
 * G = 12 + ((((-88*-30) & ~1Fh) + ((-183*50) & ~7) + 80h) >> 8) = 12 + ((2624 - 9152 + 128) >> 8) = -13
 * B = 12 + ((454*-30 + 80h) >> 8) = 12 - 53 = -41
 * unsigned: D2 73 57; 15bpp: (210+4)>>3=26, (115+4)>>3=14, (87+4)>>3=11 -> 2DDAh */
static void colour_stream(int ydc[4], int crdc, int cbdc) {
    nhw = 0;
    flat(crdc); flat(cbdc);
    for (int i = 0; i < 4; i++) flat(ydc[i]);
}

static void test_colour(void) {
    setup();
    memset(chroma, 4, sizeof chroma);
    upload_iq(1);
    memset(luma, 2, sizeof luma);
    upload_iq(0);
    int y4[4] = { 50, 50, 50, 50 };
    colour_stream(y4, 100, -60);

    CHECK(decode(CMD_DECODE | D24) == 192, "24bpp macroblock size");
    int ok = 1;
    for (int i = 0; i < 192; i += 3)
        if (out[i] != 0xD25773D2u || out[i + 1] != 0x73D25773u || out[i + 2] != 0x5773D257u) ok = 0;
    CHECK(ok, "24bpp unsigned pixels R=D2 G=73 B=57 (got %08X %08X %08X)", out[0], out[1], out[2]);
    decode(CMD_DECODE | D24 | SGN);
    ok = 1;
    for (int i = 0; i < 192; i += 3)
        if (out[i] != 0x52D7F352u || out[i + 1] != 0xF352D7F3u || out[i + 2] != 0xD7F352D7u) ok = 0;
    CHECK(ok, "24bpp signed pixels R=52 G=F3 B=D7 (got %08X %08X %08X)", out[0], out[1], out[2]);

    CHECK(decode(CMD_DECODE | D15) == 128 && all_words(0, 128, 0x2DDA2DDAu), "15bpp unsigned (got %08X)", out[0]);
    CHECK(decode(CMD_DECODE | D15 | STP) == 128 && all_words(0, 128, 0xADDAADDAu), "15bpp with bit 15 set");
    CHECK(decode(CMD_DECODE | D15 | SGN) == 128 && all_words(0, 128, 0x6FCA6FCAu), "15bpp signed (got %08X)", out[0]);
    CHECK(decode(CMD_DECODE | D15 | SGN | STP) == 128 && all_words(0, 128, 0xEFCAEFCAu), "15bpp signed + bit 15");
    CHECK(decode(CMD_DECODE | D24 | STP) == 192 && out[0] == 0xD25773D2u, "bit 15 flag ignored at 24bpp");

    /* 9-bit wrap: Y = Cr = 127 (DC 508 at quant 2 / 254 at quant 4), Cb = 0:
       R = 127 + ((359*127 + 80h) >> 8) = 305 -> 9-bit -207 -> -128 -> 00h
       G = 127 + ((-23248 + 80h) >> 8) = 36 -> A4h,  B = 127 -> FFh */
    /* green rounding: Y 12, Cr -50 (DC -200 at quant 4 -> -100), Cb -49 (DC -99):
       G = 12 + (((4312 & ~1Fh) + (9150 & ~7) + 80h) >> 8) = 12 + ((4288 + 9144 + 128) >> 8) = 64
       (65 without the Cb mask), R = 12 + (-17822 >> 8) = -58, B = 12 + (-22118 >> 8) = -75 */
    colour_stream(y4, -100, -99);
    decode(CMD_DECODE | D24);
    CHECK(out[0] == 0x4635C046u && out[1] == 0xC04635C0u && out[2] == 0x35C04635u,
          "24bpp green rounding R=46 G=C0 B=35 (got %08X %08X %08X)", out[0], out[1], out[2]);

    int y127[4] = { 508, 508, 508, 508 };
    colour_stream(y127, 254, 0);
    decode(CMD_DECODE | D24);
    CHECK(out[0] == 0x00FFA400u && out[1] == 0xA400FFA4u && out[2] == 0xFFA400FFu,
          "24bpp red overflow wraps through 9 bits (got %08X %08X %08X)", out[0], out[1], out[2]);
}

/* Y1..Y4 = 0, 12, -12, 50 with grey chroma; 15bpp grey c5 = (Y+128+4)>>3: 16, 18, 15, 22 */
static void test_block_order(void) {
    setup();
    int ydc[4] = { 0, 50, -50, 200 };
    static const uint32_t p15[4] = { 0x42104210u, 0x4A524A52u, 0x3DEF3DEFu, 0x5AD65AD6u };
    static const uint32_t p24[4] = { 0x80808080u, 0x8C8C8C8Cu, 0x74747474u, 0xB2B2B2B2u };

    /* CPU reads: unordered 8x8 blocks, status shows the block being read */
    colour_stream(ydc, 0, 0);
    int words = (nhw + 1) / 2;
    put(CMD_DECODE | D15 | (uint32_t)words);
    for (int i = 0; i < words; i++) put(hw[i * 2] | (uint32_t)hw[i * 2 + 1] << 16);
    int ok = 1, blk_ok = 1;
    for (int i = 0; i < 128; i++) {
        if (((stat() >> 16) & 7) != (uint32_t)(i / 32)) blk_ok = 0;
        if (mdec_read(0) != p15[i / 32]) ok = 0;
    }
    CHECK(ok, "CPU reads return Y1, Y2, Y3, Y4 blocks in turn");
    CHECK(blk_ok, "status current block follows the output block");
    CHECK_EQ(stat(), 0x8604FFFFu, "idle after the macroblock (current block = Cr)");

    /* DMA reads: re-ordered into a 16x16 macroblock (15bpp: 8 words per row) */
    for (int pass = 0; pass < 2; pass++) {
        int wpr = pass ? 12 : 8, n = 16 * wpr;
        const uint32_t *p = pass ? p24 : p15;
        static uint32_t img[512];
        memset(img, 0xEE, sizeof img);
        colour_stream(ydc, 0, 0);
        words = (nhw + 1) / 2;
        ctrl(0x60000000u);
        put(CMD_DECODE | (pass ? D24 : D15) | (uint32_t)words);
        int fed = 0, got = 0;
        for (int progress = 1; progress;) {
            progress = 0;
            if (mdec_dma_in_request())
                for (int k = 0; k < MDEC_FIFO_WORDS && fed < words; k++, fed++, progress = 1)
                    mdec_dma_in(hw[fed * 2] | (uint32_t)hw[fed * 2 + 1] << 16);
            while (mdec_dma_out_ready() && got < n) {
                int32_t offs;
                uint32_t w = mdec_dma_out(&offs);
                img[got + offs] = w;
                got++;
                progress = 1;
            }
        }
        CHECK(fed == words && got == n, "DMA moved all words (%d/%d in, %d/%d out)", fed, words, got, n);
        ok = 1;
        for (int y = 0; y < 16; y++)
            for (int w = 0; w < wpr; w++)
                if (img[y * wpr + w] != p[(y >= 8) * 2 + (w >= wpr / 2)]) ok = 0;
        CHECK(ok, "DMA1 re-orders the %s macroblock (row 0: %08X .. %08X)", pass ? "24bpp" : "15bpp", img[0], img[wpr - 1]);
        ctrl(0x80000000u);
    }
}

static void test_fifo_status(void) {
    setup();
    ctrl(0x60000000u);
    /* 15bpp: Y1 fills the output FIFO exactly, Y2 then waits in the pixel buffer */
    int ydc[4] = { 0, 50, -50, 200 };
    colour_stream(ydc, 0, 0);
    put(CMD_DECODE | D15 | 64);
    for (int i = 0; i < 6; i++) put(hw[i * 2] | (uint32_t)hw[i * 2 + 1] << 16);
    /* 4 words consumed (Cr, Cb, Y1, Y2), Y3/Y4 words wait in the input FIFO */
    CHECK_EQ(stat(), 0x2E00003Bu, "stalled: busy, DREQ1, Y1 at the output, 59 left-1");
    CHECK(!mdec_dma_in_request() && mdec_dma_out_ready(), "DMA0 waits for an empty FIFO, DMA1 may read");
    for (int i = 0; i < 30; i++) put(0xFE00FE00u);
    CHECK_EQ(stat(), 0x6E00003Bu, "input FIFO full");
    put(0xFE00FE00u);                            /* lost: FIFO full */
    nout = 0;
    for (int i = 0; i < 32; i++) out[nout++] = mdec_read(0);
    CHECK_EQ((stat() >> 16) & 7, 1, "Y2 at the output after 32 words");
    drain();
    CHECK(nout == 128, "whole macroblock read (%d)", nout);
    /* 4 + 32 words consumed so far, the lost word did not count */
    CHECK_EQ(stat(), 0xB6040000u | 27u, "waiting for the last 28 words (DREQ0 again)");
    for (int i = 0; i < 28; i++) put(0xFE00FE00u);
    CHECK_EQ(stat(), 0x8604FFFFu, "command complete");

    /* busy stays set until the last block has left the pixel buffer */
    nhw = 0; flat(50); flat(50); flat(50);       /* mono 8bpp: 16 words per block */
    put(CMD_DECODE | D8 | 3);
    for (int i = 0; i < 3; i++) put(hw[i * 2] | (uint32_t)hw[i * 2 + 1] << 16);
    CHECK_EQ(stat(), 0x6A04FFFFu, "last word received, third block pending");
    for (int i = 0; i < 16; i++) mdec_read(0);
    CHECK_EQ(stat(), 0x0A04FFFFu, "not busy once the last block is in the FIFO");
    for (int i = 0; i < 32; i++) mdec_read(0);
    CHECK_EQ(stat(), 0x8204FFFFu, "output FIFO empty");
    CHECK_EQ(mdec_read(0), 0, "empty data register reads 0");

    /* reset in the middle of a command; the next word is a new command */
    put(CMD_DECODE | D15 | 64);
    put(hw[0] | (uint32_t)hw[1] << 16);
    ctrl(0xE0000000u);
    CHECK_EQ(stat(), 0x80040000u, "reset mid-command");
    put(0x40000000u);
    CHECK_EQ(stat(), 0xB004000Fu, "new command after reset (DMA enables kept from the reset write)");

    /* DMA1 request needs control bit 29 */
    setup();
    ctrl(0x40000000u);
    nhw = 0; flat(50);
    put(CMD_DECODE | D8 | 1);
    put(hw[0] | (uint32_t)hw[1] << 16);
    CHECK(!mdec_dma_out_ready(), "no DMA1 without control bit 29");
    ctrl(0x60000000u);
    CHECK(mdec_dma_out_ready(), "DMA1 ready once enabled");
}

int main(void) {
    test_reset_status();
    test_tables();
    test_mono8_dc();
    test_pattern();
    test_float_reference();
    test_qscale0();
    test_rle();
    test_colour();
    test_block_order();
    test_fifo_status();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
