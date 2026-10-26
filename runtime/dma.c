/* DMA controller.  Transfers complete instantly (the CPU is not stalled), then the channel
 * completion interrupt is raised exactly as the hardware would at the end of the transfer.
 * The MDEC channels 0/1 instead follow the MDEC's data requests (see "MDEC channels"). */
#include "psx.h"
#include "gpu.h"
#include "spu.h"
#include "mdec.h"

typedef struct { uint32_t madr, bcr, chcr; } DmaChan;
static DmaChan ch[7];
static uint32_t dpcr = 0x07654321, dicr;

/* MDEC channel transfer state (0 = RAM -> MDEC, 1 = MDEC -> RAM) */
typedef struct {
    bool active;            /* started, words still to move */
    bool completing;        /* DMA1: all words moved, completion IRQ scheduled */
    uint32_t addr;          /* next RAM address */
    uint32_t left;          /* words still to move */
    uint32_t bs, inblk;     /* block size, words moved in the current block */
    uint32_t total;
    uint64_t due;           /* completion cycle (delayed completion) */
} MdecChan;
static MdecChan mc[2];
static bool mdec_servicing;

void dma_init(void) {
    memset(ch, 0, sizeof ch);
    memset(mc, 0, sizeof mc);
    mdec_servicing = false;
    dpcr = 0x07654321;
    dicr = 0;
    mdec_init();            /* the MDEC is only reachable through its ports and DMA0/1 */
}

static void dicr_update(void) {
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    bool master = (dicr & 0x8000) || ((dicr & 0x800000) && (flags & en));
    uint32_t old = dicr & 0x80000000u;
    dicr = (dicr & 0x7FFFFFFFu) | (master ? 0x80000000u : 0);
    if (master && !old) irq_raise(IRQ_DMA);
}

static void finish(int n) {
    ch[n].chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + n))) {
        dicr |= 1u << (24 + n);
    }
    dicr_update();
}

static uint32_t tmpbuf[0x10000 * 2];

static void run_channel(int n) {
    DmaChan *d = &ch[n];
    uint32_t chcr = d->chcr;
    uint32_t sync = (chcr >> 9) & 3;
    bool from_ram = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d->madr & 0x1FFFFC;

    if (n == 2 && sync == 2) {
        /* GPU linked list (ordering table) */
        int guard = 0;
        for (;;) {
            uint32_t hdr = ram_r32(addr);
            uint32_t cnt = hdr >> 24;
            for (uint32_t k = 0; k < cnt; k++) {
                uint32_t w = ram_r32((addr + 4 + k * 4) & 0x1FFFFC);
                gpu_write_gp0(w);
            }
            if (hdr & 0x800000) break;
            addr = hdr & 0x1FFFFC;
            if (++guard > 0x100000) { LOGE("GPU DMA linked list loop"); break; }
        }
        d->madr = 0x00FFFFFF;
        finish(n);
        return;
    }

    uint32_t words;
    if (sync == 0) {
        words = d->bcr & 0xFFFF;
        if (!words) words = 0x10000;
    } else {
        uint32_t bs = d->bcr & 0xFFFF, ba = d->bcr >> 16;
        if (!bs) bs = 0x10000;
        if (!ba) ba = 0x10000;
        words = bs * ba;
        if (words > 0x20000) words = 0x20000;
    }

    LOGT("DMA%d madr=%08X words=%u chcr=%08X", n, addr, words, chcr);
    switch (n) {
    case 6: /* OTC: reverse clear ordering table */
        for (uint32_t k = 0; k < words; k++) {
            uint32_t a = (addr - k * 4) & 0x1FFFFC;
            ram_w32(a, (k == words - 1) ? 0x00FFFFFF : ((a - 4) & 0x1FFFFF));
        }
        break;
    case 2: /* GPU block */
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            gpu_dma_write(tmpbuf, (int)words);
        } else {
            gpu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    case 3: /* CDROM -> RAM */
        LOGD("CD DMA lba=%d -> %08X words=%u", cdrom_data_lba(), addr, words);
        cdrom_dma_read(tmpbuf, (int)words);
        for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        break;
    case 4: /* SPU */
        spu_catch_up();
        if (from_ram) {
            for (uint32_t k = 0; k < words; k++) tmpbuf[k] = ram_r32((addr + (uint32_t)step * k) & 0x1FFFFC);
            spu_dma_write(tmpbuf, (int)words);
        } else {
            spu_dma_read(tmpbuf, (int)words);
            for (uint32_t k = 0; k < words; k++) ram_w32((addr + (uint32_t)step * k) & 0x1FFFFC, tmpbuf[k]);
        }
        break;
    default:
        LOGW("DMA channel %d transfer ignored (chcr=%08X)", n, chcr);
        break;
    }
    if (!(chcr & 1) && n != 6) code_written(step > 0 ? addr : addr - (words - 1) * 4, words * 4);   /* to RAM */
    if (sync != 0) {
        d->madr = (addr + (uint32_t)step * words) & 0xFFFFFF;
        if (sync == 1) d->bcr &= 0xFFFF;   /* block count reaches zero */
    }
    finish(n);
}

/* ---------------------------------------------------------------- MDEC channels
 * DMA0 and DMA1 are request driven instead of completing at once: a started transfer stays
 * busy (CHCR bit 24 set) and moves words only while the MDEC asks for them -- DMA0 one block
 * (at most a FIFO's worth) whenever the input FIFO is empty and the current command still
 * expects parameters, DMA1 whenever decoded output is available (written to RAM with the
 * MDEC's 16x16 macroblock re-ordering).  Games usually start DMA1 before the input arrives;
 * it completes, setting its DICR flag and raising the DMA IRQ, as soon as the decoder has
 * produced its last word, i.e. inside the register access (DMA0 start, MDEC write/read)
 * that made the data available.  With MDEC_OUT_DELAY > 0 the DMA1 completion is instead
 * deferred by that many cycles per word (about 16 on hardware at 24bpp: ~512 cycles per
 * 8x8 block); that needs sched.c to call dma_event() for EV_DMA. */
#ifndef MDEC_OUT_DELAY
#define MDEC_OUT_DELAY 0
#endif

static void mdec_chan_end(int n) {
    DmaChan *d = &ch[n];
    MdecChan *m = &mc[n];
    m->active = false;
    LOGT("MDEC DMA%d done", n);
    if (((d->chcr >> 9) & 3) == 1) {        /* SyncMode 1: end address, BA = 0 */
        d->madr = m->addr & 0xFFFFFF;
        d->bcr &= 0xFFFF;
    }
    if (n == 1 && MDEC_OUT_DELAY > 0) {
        m->completing = true;
        m->due = g_cpu.cycles + (uint64_t)MDEC_OUT_DELAY * m->total;
        sched_set(EV_DMA, m->due);
        return;
    }
    finish(n);
}

/* one word moved: SyncMode 1 updates MADR (start of the next block) and BA per block */
static void mdec_chan_step(int n) {
    DmaChan *d = &ch[n];
    MdecChan *m = &mc[n];
    m->addr = (m->addr + ((d->chcr & 2) ? (uint32_t)-4 : 4u)) & 0x1FFFFC;
    m->left--;
    if (++m->inblk < m->bs) return;
    m->inblk = 0;
    if (((d->chcr >> 9) & 3) == 1) {
        d->madr = m->addr & 0xFFFFFF;
        d->bcr = (d->bcr & 0xFFFF) | ((d->bcr - 0x10000u) & 0xFFFF0000u);
    }
}

void dma_mdec_update(void) {
    if (mdec_servicing) return;
    mdec_servicing = true;
    for (bool progress = true; progress;) {
        progress = false;
        MdecChan *m = &mc[0];
        if (m->active && mdec_dma_in_request()) {
            uint32_t n = m->bs - m->inblk;
            if (n > MDEC_FIFO_WORDS) n = MDEC_FIFO_WORDS;
            if (n > m->left) n = m->left;
            for (uint32_t k = 0; k < n; k++) {
                mdec_dma_in(ram_r32(m->addr));
                mdec_chan_step(0);
            }
            progress = true;
            if (!m->left) mdec_chan_end(0);
        }
        m = &mc[1];
        if (m->active) {
            while (m->left && mdec_dma_out_ready()) {
                int32_t offs;
                uint32_t w = mdec_dma_out(&offs);
                ram_w32(m->addr + (uint32_t)offs * 4u, w);
                code_written(m->addr + (uint32_t)offs * 4u, 4);
                mdec_chan_step(1);
                progress = true;
            }
            if (!m->left) mdec_chan_end(1);
        }
    }
    mdec_servicing = false;
}

void dma_event(void) {
    MdecChan *m = &mc[1];
    if (!m->completing) return;
    if (g_cpu.cycles < m->due) { sched_set(EV_DMA, m->due); return; }
    m->completing = false;
    finish(1);
}

/* CHCR written: (re)start or stop the channel */
static void mdec_chan_control(int n) {
    DmaChan *d = &ch[n];
    MdecChan *m = &mc[n];
    memset(m, 0, sizeof *m);
    if (!(d->chcr & 0x01000000u)) return;
    uint32_t sync = (d->chcr >> 9) & 3;
    if (!(dpcr & (8u << (n * 4))))
        LOGD("DMA%d started while disabled in DPCR", n);
    if (sync > 1) {
        LOGW("DMA%d: sync mode %u is not usable with the MDEC (chcr=%08X)", n, sync, d->chcr);
        finish(n);
        return;
    }
    if ((d->chcr & 1) != (n == 0 ? 1u : 0u))
        LOGW("DMA%d: unexpected transfer direction (chcr=%08X)", n, d->chcr);
    uint32_t bs = d->bcr & 0xFFFF, ba = sync ? d->bcr >> 16 : 1;
    if (!bs) bs = 0x10000;
    if (!ba) ba = 0x10000;
    uint64_t words = (uint64_t)bs * ba;
    m->left = words > 0x80000 ? 0x80000u : (uint32_t)words;    /* at most all of RAM */
    m->total = m->left;
    m->bs = sync ? bs : m->left;
    m->addr = d->madr & 0x1FFFFC;
    m->active = true;
    LOGT("MDEC DMA%d start madr=%08X words=%u", n, m->addr, m->left);
    dma_mdec_update();
}

uint32_t dma_read(uint32_t off) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: return ch[n].madr;
        case 4: return ch[n].bcr;
        case 8: return ch[n].chcr;
        default: return 0;
        }
    }
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    return 0;
}

void dma_write(uint32_t off, uint32_t v) {
    int n = (int)(off >> 4);
    if (n < 7) {
        switch (off & 0xC) {
        case 0: ch[n].madr = v & 0xFFFFFF; break;
        case 4: ch[n].bcr = v; break;
        case 8:
            if (n == 6) v = (v & 0x51000000u) | 2;   /* OTC: only start/trigger bits writable, step -4 */
            ch[n].chcr = v;
            if (n < 2) { mdec_chan_control(n); break; }
            if ((v & 0x01000000u) && (((v >> 9) & 3) != 0 || (v & 0x10000000u))) {
                if (!(dpcr & (8u << (n * 4))))
                    LOGD("DMA%d started while disabled in DPCR", n);
                run_channel(n);
            }
            break;
        }
        return;
    }
    if (off == 0x70) { dpcr = v; return; }
    if (off == 0x74) {
        uint32_t ack = v & 0x7F000000u;
        dicr = (dicr & 0xFF000000u & ~ack) | (v & 0x00FF803Fu);
        dicr_update();
        return;
    }
}
