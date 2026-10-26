/* SIO0: controller / memory card serial port, with the devices attached to it.
 *
 * The game polls the controllers itself (its own low-level driver talks to these registers
 * byte by byte, driven by the /ACK interrupt), so this models the port at the hardware
 * level: 8-bit full-duplex transfers at the programmed baud rate, the /ACK (DSR) pulse
 * after each byte the device wants to continue, and IRQ7.  Each port has a controller while
 * the platform layer reports one connected: a DualShock (SCPH-1200: digital/analog mode,
 * config commands, two rumble motors) or, with controller=digital, a plain digital pad.
 * Port 1 also has the memory card. */
#include "psx.h"

typedef enum { DEV_NONE, DEV_PAD, DEV_CARD } DevKind;

DualShock g_dualshock[2] = {
    { .type = PAD_TYPE_DUALSHOCK, .axis = { 0x80, 0x80, 0x80, 0x80 } },
    { .type = PAD_TYPE_DUALSHOCK, .axis = { 0x80, 0x80, 0x80, 0x80 } },
};

/* DualShock state kept between transfers (reset at power-up and when the pad is replugged) */
typedef struct {
    bool present;              /* connected at the last access */
    bool config;               /* config mode: ID F3h, commands 40h..4Fh */
    bool locked;               /* 44h: mode locked, the ANALOG button is ignored */
    bool cfg_used;             /* config mode was entered: the old one-motor rumble method is off */
    uint8_t status;            /* 2nd reply byte: 5Ah (00h after a manual mode change, see below) */
    uint8_t map[6];            /* 4Dh: meaning of command bytes 4..9 (00h small, 01h large motor, FFh none) */
} PadState;
static PadState pads[2];

static struct {
    uint16_t mode, ctrl, baud;
    uint32_t stat;
    uint8_t rx[8];
    int rx_n;
    uint8_t tx_byte;
    int phase;                 /* 0 idle, 1 transferring, 2 waiting to assert /ACK, 3 /ACK low */
    bool ack_pending;
    /* device state for the current /CS selection */
    DevKind dev;
    int step;
    /* memory card command state */
    uint8_t mc_cmd, mc_msb, mc_lsb, mc_chk, mc_prev;
    uint8_t mc_buf[128];
    uint8_t mc_flag[2];
    /* DualShock command state: reply[k] goes out with byte k (1 = command byte) */
    uint8_t pad_cmd, pad_len, pad_arg;
    uint8_t pad_reply[9];
} s;

static void pad_reset(int port) {
    memset(&pads[port], 0, sizeof pads[port]);
    pads[port].status = 0x5A;
    memset(pads[port].map, 0xFF, sizeof pads[port].map);
    g_dualshock[port].analog = false;          /* power-up: digital mode, LED off */
    g_dualshock[port].motor_small = g_dualshock[port].motor_large = 0;
}

void sio_init(void) {
    memset(&s, 0, sizeof s);
    s.stat = 0x05;
    s.mc_flag[0] = s.mc_flag[1] = 0x08;   /* "new card" until the first write */
    pad_reset(0);
    pad_reset(1);
}

static int cur_port(void) { return (s.ctrl >> 13) & 1; }
static bool selected(void) { return (s.ctrl & 2) != 0; }

static uint64_t byte_cycles(void) {
    uint32_t factor = (s.mode & 3) == 2 ? 16 : (s.mode & 3) == 3 ? 64 : 1;
    uint32_t bit = (uint32_t)s.baud * factor & ~1u;
    if (bit < 2) bit = 2;
    return (uint64_t)bit * 8u;
}

static void update_irq(bool rising) {
    if (rising) {
        s.stat |= 0x200;
        irq_raise(IRQ_SIO0);
        LOGT("SIO0 IRQ raised: sr=%08X exc=%d I_MASK=%03X cyc=%llu next=%llu", g_cpu.sr, g_in_exception, g_imask,
             (unsigned long long)g_cpu.cycles, (unsigned long long)g_cpu.next_event);
    }
}

/* ------------------------------------------------------------------ devices */
/* plain digital pad (SCPH-1080): knows only 42h - acknowledging 43h/45h makes libpad take
 * it for an analog pad */
static uint8_t digital_exchange(int port, uint8_t in, bool *ack) {
    uint16_t btn = (uint16_t)(~g_pad_buttons[port] | PAD_L3 | PAD_R3);
    switch (s.step) {
    case 1:                                        /* command: digital pads only know 42h */
        if (in != 0x42) { *ack = false; return 0xFF; }
        *ack = true; return 0x41;                  /* ID low: digital pad */
    case 2: *ack = true; return 0x5A;              /* ID high */
    case 3: *ack = true; return (uint8_t)(btn & 0xFF);
    case 4: *ack = false; return (uint8_t)(btn >> 8);
    default: *ack = false; return 0xFF;
    }
}

/* the pad's ANALOG button: toggles the mode unless the game locked it (44h); like the
 * watchdog reset it stops and locks the motors, and after config use the 5Ah byte becomes 00h */
void pad_analog_button(int port) {
    PadState *p = &pads[port];
    DualShock *d = &g_dualshock[port];
    if (d->type != PAD_TYPE_DUALSHOCK || !g_pad_connected[port] || p->locked || p->config) return;
    d->analog = !d->analog;
    memset(p->map, 0xFF, sizeof p->map);
    d->motor_small = d->motor_large = 0;
    if (p->cfg_used) p->status = 0x00;
}

/* buttons (L3/R3 only in analog replies) and the four stick bytes RX RY LX LY */
static void ds_inputs(int port, uint8_t *r, bool analog) {
    uint16_t btn = (uint16_t)~g_pad_buttons[port];
    if (!analog) btn |= PAD_L3 | PAD_R3;
    r[3] = (uint8_t)btn;
    r[4] = (uint8_t)(btn >> 8);
    memcpy(r + 5, g_dualshock[port].axis, 4);
}

/* command byte: prepare the reply packet; false = command not known in this mode (no /ACK) */
static bool ds_begin(int port, uint8_t cmd) {
    PadState *p = &pads[port];
    DualShock *d = &g_dualshock[port];
    uint8_t *r = s.pad_reply;
    memset(r, 0, sizeof s.pad_reply);
    s.pad_cmd = cmd;
    s.pad_len = 9;                                 /* bytes incl. the address byte */
    if (p->config) {                               /* config mode: always 9 bytes, ID F3h 5Ah */
        if (cmd < 0x40 || cmd > 0x4F) return false;
        r[1] = 0xF3;
        r[2] = 0x5A;
        switch (cmd) {
        case 0x42: ds_inputs(port, r, true); d->polls++; break;   /* analog bytes even in digital mode */
        case 0x45: r[3] = 0x01; r[4] = 0x02; r[5] = d->analog; r[6] = 0x02; r[7] = 0x01; break;
        case 0x47: r[5] = 0x02; r[7] = 0x01; break;
        case 0x4D: memcpy(r + 3, p->map, 6); break;   /* old mapping; the new one comes in */
        default: break;    /* 43h, 44h and the unused ones answer zeros; 46h/48h/4Ch patch theirs */
        }
        return true;
    }
    if (cmd != 0x42 && cmd != 0x43) return false;   /* normal mode: read, enter config */
    if (cmd == 0x42) d->polls++;
    r[2] = p->status;
    if (d->analog) {
        r[1] = 0x73;
        ds_inputs(port, r, true);
        return true;
    }
    /* digital mode: one halfword, more only when 4Dh mapped a motor to bytes 6..9 */
    int hw = (p->map[4] & p->map[5]) != 0xFF ? 3 : (p->map[2] & p->map[3]) != 0xFF ? 2 : 1;
    r[1] = (uint8_t)(0x40 | hw);
    s.pad_len = (uint8_t)(3 + 2 * hw);
    ds_inputs(port, r, false);
    return true;
}

/* motor byte i (0..5 = command bytes 4..9) of a 42h read */
static void ds_motor(int port, int i, uint8_t in) {
    PadState *p = &pads[port];
    DualShock *d = &g_dualshock[port];
    if (!p->cfg_used) {     /* old method: small motor on with xx = 40h..7Fh and yy bit0 = 1 */
        if (i == 0) s.pad_arg = in;
        else if (i == 1) d->motor_small = (s.pad_arg & 0xC0) == 0x40 && (in & 1);
        return;
    }
    if (p->map[i] == 0x00) d->motor_small = in & 1;
    else if (p->map[i] == 0x01) d->motor_large = in;
}

/* parameter byte k (3..8) arrived */
static void ds_param(int port, int k, uint8_t in) {
    static const uint8_t act0[4] = { 0x01, 0x02, 0x00, 0x0A }, act1[4] = { 0x01, 0x01, 0x01, 0x14 };
    PadState *p = &pads[port];
    DualShock *d = &g_dualshock[port];
    uint8_t *r = s.pad_reply;
    switch (s.pad_cmd) {
    case 0x42: ds_motor(port, k - 3, in); break;
    case 0x43:                                      /* xx: 00h normal mode, 01h config mode */
        if (k == 3 && in <= 1) {
            if (in && !p->config) { p->cfg_used = true; p->status = 0x5A; }
            p->config = in;
        }
        break;
    case 0x44:                                      /* Led (00h digital, 01h analog), Key (3 = lock) */
        if (k == 3 && in <= 1) d->analog = in;
        if (k == 4) p->locked = (in & 3) == 3;
        break;
    case 0x46: if (k == 3 && in <= 1) memcpy(r + 5, in ? act1 : act0, 4); break;
    case 0x48: if (k == 3 && in <= 1) r[7] = 0x01; break;
    case 0x4C: if (k == 3 && in <= 1) r[6] = in ? 0x07 : 0x04; break;
    case 0x4D:
        p->map[k - 3] = in;
        if (!memchr(p->map, 0x00, sizeof p->map)) d->motor_small = 0;   /* unmapped motors stop */
        if (!memchr(p->map, 0x01, sizeof p->map)) d->motor_large = 0;
        break;
    default: break;
    }
}

static uint8_t dualshock_exchange(int port, uint8_t in, bool *ack) {
    int k = s.step;
    if (k == 1 && !ds_begin(port, in)) { *ack = false; return 0xFF; }
    if (k >= s.pad_len) { *ack = false; return 0xFF; }
    uint8_t out = s.pad_reply[k];                  /* shifted out while byte k comes in */
    if (k >= 3) ds_param(port, k, in);
    *ack = k < s.pad_len - 1;                      /* no /ACK after the last byte */
    return out;
}

/* a controller that was (un)plugged comes back powered up: digital mode, motors off */
static void pad_plug_check(int port) {
    bool on = g_pad_connected[port];
    if (on != pads[port].present) { pad_reset(port); pads[port].present = on; }
}

static uint8_t card_exchange(int port, uint8_t in, bool *ack) {
    uint8_t *card = memcard_data(port);
    int k = s.step;           /* 1 = command byte */
    uint8_t out = 0xFF;
    *ack = true;
    if (k == 1) {
        s.mc_cmd = in;
        if (in != 'R' && in != 'W' && in != 'S') { *ack = false; return s.mc_flag[port]; }
        return s.mc_flag[port];
    }
    switch (s.mc_cmd) {
    case 'R':
        switch (k) {
        case 2: out = 0x5A; break;
        case 3: out = 0x5D; break;
        case 4: s.mc_msb = in; out = 0x00; break;
        case 5: s.mc_lsb = in; out = s.mc_msb; break;
        case 6: out = 0x5C; break;
        case 7: out = 0x5D; break;
        case 8: {
            uint32_t sec = ((uint32_t)s.mc_msb << 8) | s.mc_lsb;
            if (sec > 0x3FF) { out = 0xFF; *ack = false; break; }
            memcpy(s.mc_buf, card + sec * 128, 128);
            s.mc_chk = s.mc_msb ^ s.mc_lsb;
            out = s.mc_msb;
            break;
        }
        case 9: out = s.mc_lsb; break;
        default:
            if (k >= 10 && k < 138) { out = s.mc_buf[k - 10]; s.mc_chk ^= out; }
            else if (k == 138) out = s.mc_chk;
            else if (k == 139) { out = 0x47; *ack = false; }
            else { out = 0xFF; *ack = false; }
            break;
        }
        break;
    case 'W':
        switch (k) {
        case 2: out = 0x5A; break;
        case 3: out = 0x5D; break;
        case 4: s.mc_msb = in; s.mc_chk = in; out = 0x00; break;
        case 5: s.mc_lsb = in; s.mc_chk ^= in; out = s.mc_msb; break;
        default:
            if (k >= 6 && k < 134) { s.mc_buf[k - 6] = in; s.mc_chk ^= in; out = s.mc_prev; }
            else if (k == 134) { out = s.mc_prev; s.mc_chk ^= in; /* chk now 0 if valid */ }
            else if (k == 135) out = 0x5C;
            else if (k == 136) out = 0x5D;
            else if (k == 137) {
                uint32_t sec = ((uint32_t)s.mc_msb << 8) | s.mc_lsb;
                *ack = false;
                if (sec > 0x3FF) out = 0xFF;
                else if (s.mc_chk != 0) out = 0x4E;
                else {
                    memcpy(card + sec * 128, s.mc_buf, 128);
                    memcard_mark_dirty(port);
                    s.mc_flag[port] &= (uint8_t)~0x08;
                    out = 0x47;
                }
            } else { out = 0xFF; *ack = false; }
            break;
        }
        break;
    case 'S': {
        static const uint8_t id[] = { 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80 };
        if (k >= 2 && k < 10) { out = id[k - 2]; *ack = k < 9; }
        else { out = 0xFF; *ack = false; }
        break;
    }
    }
    s.mc_prev = in;
    return out;
}

static uint8_t exchange(uint8_t in, bool *ack) {
    int port = cur_port();
    *ack = false;
    if (!selected()) return 0xFF;
    if (s.step == 0) {
        s.step = 1;
        pad_plug_check(port);
        if (in == 0x01 && g_pad_connected[port]) { s.dev = DEV_PAD; *ack = true; return 0xFF; }
        if (in == 0x81 && memcard_data(port)) { s.dev = DEV_CARD; *ack = true; return 0xFF; }
        s.dev = DEV_NONE;
        return 0xFF;
    }
    uint8_t r = 0xFF;
    if (s.dev == DEV_PAD) r = g_dualshock[port].type == PAD_TYPE_DIGITAL ? digital_exchange(port, in, ack)
                                                                         : dualshock_exchange(port, in, ack);
    else if (s.dev == DEV_CARD) r = card_exchange(port, in, ack);
    s.step++;
    if (!*ack) s.dev = DEV_NONE;
    return r;
}

/* ------------------------------------------------------------------ transfer timing */
void sio_event(void) {
    switch (s.phase) {
    case 1: { /* byte shifted out/in */
        bool ack;
        uint8_t r = exchange(s.tx_byte, &ack);
        if (s.rx_n < 8) s.rx[s.rx_n++] = r;
        else s.rx[7] = r;
        s.stat |= 0x07;   /* TX ready, RX not empty, TX idle */
        if ((s.ctrl & 0x800) && s.rx_n >= (1 << ((s.ctrl >> 8) & 3))) update_irq(true);
        if (s.ctrl & 0x400) update_irq(true);
        if (ack) { s.phase = 2; sched_set(EV_SIO, g_cpu.cycles + 120); }
        else s.phase = 0;
        break;
    }
    case 2: /* device pulls /ACK low */
        s.stat |= 0x80;
        if (s.ctrl & 0x1000) update_irq(true);
        s.phase = 3;
        sched_set(EV_SIO, g_cpu.cycles + 100);
        break;
    case 3: /* /ACK released */
        s.stat &= ~0x80u;
        s.phase = 0;
        break;
    default: break;
    }
}

static void start_tx(void) {
    s.stat &= ~0x05u;
    s.stat &= ~0x80u;
    s.phase = 1;
    sched_set(EV_SIO, g_cpu.cycles + byte_cycles());
}

/* ------------------------------------------------------------------ registers */
uint32_t sio_read(uint32_t off, int width) {
    switch (off & 0x1F) {
    case 0x00: { /* RX data */
        uint32_t v = s.rx_n ? s.rx[0] : 0xFF;
        if (width >= 2) for (int i = 1; i < 4; i++) v |= (uint32_t)(i < s.rx_n ? s.rx[i] : s.rx[0]) << (8 * i);
        int pop = width == 4 ? 4 : 1;
        for (int i = 0; i < pop && s.rx_n; i++) { memmove(s.rx, s.rx + 1, 7); s.rx_n--; }
        if (!s.rx_n) s.stat &= ~0x02u;
        LOGT("SIO0 rx %02X (w%d)", v & 0xFF, width);
        return v;
    }
    case 0x04: return s.stat;
    case 0x08: return s.mode;
    case 0x0A: return s.ctrl;
    case 0x0E: return s.baud;
    default: return 0;
    }
}

void sio_write(uint32_t off, uint32_t v, int width) {
    (void)width;
    switch (off & 0x1F) {
    case 0x00:
        s.tx_byte = (uint8_t)v;
        LOGT("SIO0 tx %02X (ctrl=%04X step=%d)", v & 0xFF, s.ctrl, s.step);
        if (s.ctrl & 1) start_tx();
        break;
    case 0x08: s.mode = (uint16_t)(v & 0x1FF); break;
    case 0x0A: {
        uint16_t old = s.ctrl;
        LOGT("SIO0 ctrl %04X -> %04X (step=%d dev=%d)", old, v & 0xFFFF, s.step, s.dev);
        if (v & 0x40) {   /* reset */
            s.mode = 0; s.ctrl = 0; s.baud = 0; s.rx_n = 0; s.stat = 0x05; s.phase = 0;
            s.step = 0; s.dev = DEV_NONE;
            sched_cancel(EV_SIO);
            return;
        }
        if (v & 0x10) s.stat &= ~0x208u;   /* acknowledge */
        s.ctrl = (uint16_t)(v & ~0x50u);
        /* /CS deasserted or port changed: devices reset their transaction */
        if (!(s.ctrl & 2) || ((old ^ s.ctrl) & 0x2000)) { s.step = 0; s.dev = DEV_NONE; }
        if ((s.ctrl & 2) && !(old & 2)) { s.step = 0; s.dev = DEV_NONE; }
        break;
    }
    case 0x0E: s.baud = (uint16_t)v; break;
    default: break;
    }
}
