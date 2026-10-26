/* Controller protocol test: drives sio.c through its registers the way libpad does (select
 * the port with JOY_CTRL, send a packet byte by byte, wait for the /ACK interrupt) and
 * checks reply bytes, IDs, /ACKs, analog bytes and motor values of the DualShock, the plain
 * digital pad and an empty port, plus that the memory card on the same port still answers. */
#include "../psx.h"
#include <stdio.h>

/* stubs for the runtime pieces sio.c uses */
CPU g_cpu;
uint32_t g_istat, g_imask;
int g_in_exception;
int g_log_level = LOG_WARN;
uint16_t g_pad_buttons[2];
bool g_pad_connected[2];
void log_msg(int level, const char *fmt, ...) { (void)level; (void)fmt; }
static uint64_t ev_sio = UINT64_MAX;
static int irq7;
void irq_raise(int n) { if (n == IRQ_SIO0) irq7++; }
void sched_set(int ev, uint64_t when) { if (ev == EV_SIO) ev_sio = when; }
void sched_cancel(int ev) { if (ev == EV_SIO) ev_sio = UINT64_MAX; }
static uint8_t card_img[0x20000];
uint8_t *memcard_data(int slot) { return slot == 0 ? card_img : NULL; }
void memcard_mark_dirty(int slot) { (void)slot; }

static int passed, failed;
#define CHECK(c) do { if (c) passed++; else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- libpad-style port access ---- */
static uint16_t ctrl;

static void run_events(void) {
    while (ev_sio != UINT64_MAX) {
        g_cpu.cycles = ev_sio;
        ev_sio = UINT64_MAX;
        sio_event();
    }
}

static void select_port(int port) {
    sio_write(0x08, 0x0D, 2);                      /* MODE: 8 bits, baud factor 1 */
    sio_write(0x0E, 0x88, 2);                      /* BAUD: ~250 kHz */
    ctrl = (uint16_t)(0x1003 | (port << 13));      /* TX enable, /JOYn, /ACK IRQ enable */
    sio_write(0x0A, ctrl | 0x10, 2);
}

static void deselect(void) { sio_write(0x0A, 0, 2); }

/* one byte; *acked = the device pulsed /ACK (IRQ7) */
static uint8_t xfer(uint8_t out, bool *acked) {
    int before = irq7;
    sio_write(0x00, out, 1);
    run_events();
    uint8_t in = (uint8_t)sio_read(0x00, 1);
    *acked = irq7 > before;
    sio_write(0x0A, ctrl | 0x10, 2);               /* acknowledge the IRQ */
    return in;
}

/* whole packet: reply[i] for send[i]; returns the number of /ACKed bytes and checks that
 * the device acks every byte but the last one of a packet of the expected length */
static int packet(int port, const uint8_t *send, uint8_t *reply, int n) {
    bool ack[160];
    select_port(port);
    for (int i = 0; i < n; i++) reply[i] = xfer(send[i], &ack[i]);
    deselect();
    int acked = 0;
    while (acked < n && ack[acked]) acked++;
    for (int i = acked; i < n; i++) if (ack[i]) acked = -1;   /* /ACK after a missing one */
    return acked;
}

#define SEND(port, reply, ...) packet(port, (const uint8_t[]){ __VA_ARGS__ }, reply, \
                                      (int)sizeof((const uint8_t[]){ __VA_ARGS__ }))

static bool same(const uint8_t *a, const uint8_t *b, int n) { return !memcmp(a, b, (size_t)n); }

int main(void) {
    uint8_t r[16];
    sio_init();
    g_pad_connected[0] = true;
    g_pad_buttons[0] = PAD_CROSS | PAD_START | PAD_L3;       /* active high */
    uint16_t dig = (uint16_t)~(PAD_CROSS | PAD_START);       /* L3 is not sent in digital mode */
    uint16_t ana = (uint16_t)~(PAD_CROSS | PAD_START | PAD_L3);

    /* ---- DualShock at power-up: digital mode, 5-byte read, ID 41h */
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00) == 4);
    CHECK(r[1] == 0x41 && r[2] == 0x5A && r[3] == (uint8_t)dig && r[4] == (uint8_t)(dig >> 8) && r[5] == 0xFF);
    CHECK(!g_dualshock[0].analog);
    /* config-only commands are not known in normal mode */
    CHECK(SEND(0, r, 0x01, 0x45, 0x00, 0x00, 0x00) == 1);
    CHECK(r[1] == 0xFF);

    /* ---- old one-motor rumble method before any config command: xx 40h..7Fh, yy bit0 */
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x40, 0x01) == 4);
    CHECK(g_dualshock[0].motor_small == 1 && g_dualshock[0].motor_large == 0);
    SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00);
    CHECK(g_dualshock[0].motor_small == 0);

    /* ---- 43h enter config: reply like a 42h read (digital: 5 bytes), then ID F3h */
    CHECK(SEND(0, r, 0x01, 0x43, 0x00, 0x01, 0x00) == 4);
    CHECK(r[1] == 0x41 && r[2] == 0x5A && r[3] == (uint8_t)dig);
    /* 45h status: type 01h (analog pad), LED off */
    CHECK(SEND(0, r, 0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0x01, 0x02, 0x00, 0x02, 0x01, 0x00, 0xFF }, 9));
    /* config-mode 42h: forced analog reply (sticks + L3) even in digital mode */
    g_dualshock[0].axis[0] = 0x11; g_dualshock[0].axis[1] = 0x22;
    g_dualshock[0].axis[2] = 0xE0; g_dualshock[0].axis[3] = 0xFF;
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(r[1] == 0xF3 && r[2] == 0x5A && r[3] == (uint8_t)ana && r[4] == (uint8_t)(ana >> 8));
    CHECK(same(r + 5, (const uint8_t[]){ 0x11, 0x22, 0xE0, 0xFF }, 4));
    /* constant tables 46h / 47h / 4Ch */
    CHECK(SEND(0, r, 0x01, 0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0x00, 0x00, 0x01, 0x02, 0x00, 0x0A }, 8));
    CHECK(SEND(0, r, 0x01, 0x46, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0x00, 0x00, 0x01, 0x01, 0x01, 0x14 }, 8));
    CHECK(SEND(0, r, 0x01, 0x46, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0, 0, 0, 0, 0, 0 }, 8));
    CHECK(SEND(0, r, 0x01, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0x00, 0x00, 0x02, 0x00, 0x01, 0x00 }, 8));
    CHECK(SEND(0, r, 0x01, 0x4C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00 }, 8));
    CHECK(SEND(0, r, 0x01, 0x4C, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(r[6] == 0x07);
    /* unused 40h/41h/4Fh answer zeros (with /ACK); commands outside 40h..4Fh are not acked */
    CHECK(SEND(0, r, 0x01, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8 && r[1] == 0xF3 && !r[3] && !r[8]);
    CHECK(SEND(0, r, 0x01, 0x41, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8 && r[1] == 0xF3 && !r[5]);
    CHECK(SEND(0, r, 0x01, 0x4F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8 && r[2] == 0x5A);
    CHECK(SEND(0, r, 0x01, 0x50, 0x00) == 1);

    /* ---- 44h: analog mode + lock, 45h reports LED on */
    CHECK(SEND(0, r, 0x01, 0x44, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0, 0, 0, 0, 0, 0 }, 8));
    CHECK(g_dualshock[0].analog);
    SEND(0, r, 0x01, 0x45, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(r[5] == 0x01);

    /* ---- 4Dh rumble mapping: returns the old mapping (all FFh at power-up) */
    CHECK(SEND(0, r, 0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }, 8));
    CHECK(SEND(0, r, 0x01, 0x4D, 0x00, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF) == 8);
    CHECK(same(r + 3, (const uint8_t[]){ 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF }, 6));

    /* ---- 43h exit config (config-mode reply: zeros) */
    CHECK(SEND(0, r, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(same(r + 1, (const uint8_t[]){ 0xF3, 0x5A, 0, 0, 0, 0, 0, 0 }, 8));

    /* ---- analog mode read: ID 73h, 9 bytes, sticks, L3; motor bytes per the 4Dh mapping */
    g_dualshock[0].axis[0] = 0x80; g_dualshock[0].axis[1] = 0x80;
    g_dualshock[0].axis[2] = 0x00; g_dualshock[0].axis[3] = 0xFF;   /* left stick down-left */
    uint32_t polls = g_dualshock[0].polls;
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x01, 0xC8, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(r[1] == 0x73 && r[2] == 0x5A && r[3] == (uint8_t)ana && r[4] == (uint8_t)(ana >> 8));
    CHECK(same(r + 5, (const uint8_t[]){ 0x80, 0x80, 0x00, 0xFF, 0xFF }, 5));   /* then end of packet */
    CHECK(g_dualshock[0].motor_small == 1 && g_dualshock[0].motor_large == 0xC8);
    CHECK(g_dualshock[0].polls == polls + 1);
    /* the old method is off now: 40h/01h means small motor = bit0 of 40h = off, large = 01h */
    SEND(0, r, 0x01, 0x42, 0x00, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00);
    CHECK(g_dualshock[0].motor_small == 0 && g_dualshock[0].motor_large == 0x01);
    SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(g_dualshock[0].motor_small == 0 && g_dualshock[0].motor_large == 0);
    /* 43h in analog normal mode: 9-byte button+analog reply, motor bytes not interpreted */
    CHECK(SEND(0, r, 0x01, 0x43, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(r[1] == 0x73 && r[7] == 0x00 && g_dualshock[0].motor_large == 0);
    /* locked: the ANALOG button is ignored */
    pad_analog_button(0);
    CHECK(g_dualshock[0].analog);

    /* ---- port 2 empty: no /ACK, Hi-Z */
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x00, 0x00) == 0);
    CHECK(r[0] == 0xFF && r[1] == 0xFF && r[2] == 0xFF);
    CHECK(SEND(1, r, 0x81, 0x53, 0x00) == 0);                 /* no card in slot 2 either */

    /* ---- port 2 plugged in: own state, powers up in digital mode */
    g_pad_connected[1] = true;
    g_pad_buttons[1] = PAD_TRIANGLE;
    uint16_t dig2 = (uint16_t)~PAD_TRIANGLE;
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00) == 4);
    CHECK(r[1] == 0x41 && r[3] == (uint8_t)dig2 && r[4] == (uint8_t)(dig2 >> 8));
    SEND(1, r, 0x01, 0x43, 0x00, 0x01, 0x00);
    CHECK(SEND(1, r, 0x01, 0x44, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00) == 8);   /* analog, unlocked */
    SEND(1, r, 0x01, 0x4D, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xFF);               /* motors swapped */
    SEND(1, r, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x7F, 0x03, 0x00, 0x00, 0x00, 0x00) == 8);
    CHECK(r[1] == 0x73 && g_dualshock[1].motor_large == 0x7F && g_dualshock[1].motor_small == 1);
    CHECK(g_dualshock[0].motor_large == 0 && g_dualshock[0].analog);   /* port 1 unaffected */
    SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(r[1] == 0x73);
    /* unlocked: the ANALOG button switches to digital mode, stops and locks the motors, and
     * (config commands having been used) the 5Ah byte becomes 00h */
    pad_analog_button(1);
    CHECK(!g_dualshock[1].analog && g_dualshock[1].motor_large == 0 && g_dualshock[1].motor_small == 0);
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x7F, 0x03) == 4);
    CHECK(r[1] == 0x41 && r[2] == 0x00 && g_dualshock[1].motor_large == 0);
    /* unplug + replug = power cycle */
    g_pad_connected[1] = false;
    CHECK(SEND(1, r, 0x01, 0x42, 0x00) == 0);
    g_pad_connected[1] = true;
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x00, 0x00) == 4);
    CHECK(r[1] == 0x41 && r[2] == 0x5A);
    CHECK(SEND(1, r, 0x01, 0x43, 0x00, 0x00, 0x00) == 4);   /* DualShock: 43h is acked */

    /* ---- 4Dh FFh..: motors no longer mapped stop */
    SEND(0, r, 0x01, 0x42, 0x00, 0x01, 0x80, 0x00, 0x00, 0x00, 0x00);
    CHECK(g_dualshock[0].motor_small == 1 && g_dualshock[0].motor_large == 0x80);
    SEND(0, r, 0x01, 0x43, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00);
    SEND(0, r, 0x01, 0x4D, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF);
    CHECK(g_dualshock[0].motor_small == 0 && g_dualshock[0].motor_large == 0);
    /* digital mode with a motor mapped to byte 6: the read grows to 2 halfwords, ID 42h */
    SEND(0, r, 0x01, 0x4D, 0x00, 0x00, 0xFF, 0x01, 0xFF, 0xFF, 0xFF);
    SEND(0, r, 0x01, 0x44, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    SEND(0, r, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00, 0x99, 0x00, 0x00) == 6);
    CHECK(r[1] == 0x42 && g_dualshock[0].motor_large == 0x99);

    /* ---- memory card on port 1 still answers next to the pad */
    card_img[5 * 128 + 7] = 0xAB;
    {
        uint8_t send[140] = { 0x81, 'R', 0x00, 0x00, 0x00, 0x05 }, rep[140];
        int acked = packet(0, send, rep, 140);
        uint8_t chk = 0x00 ^ 0x05;
        for (int i = 10; i < 138; i++) chk ^= rep[i];
        CHECK(acked == 139 && rep[2] == 0x5A && rep[3] == 0x5D && rep[8] == 0x00 && rep[9] == 0x05);
        CHECK(rep[10 + 7] == 0xAB && rep[138] == chk && rep[139] == 0x47);
    }
    CHECK(SEND(0, r, 0x81, 0x53, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00) == 9);
    CHECK(same(r + 2, (const uint8_t[]){ 0x5A, 0x5D, 0x5C, 0x5D, 0x04, 0x00, 0x00, 0x80 }, 8));
    /* PocketStation probe: no /ACK from a normal card */
    CHECK(SEND(0, r, 0x81, 0x58, 0x00) == 1);

    /* ---- controller=digital: only 42h, never 43h/45h; no L3/R3, no analog, no rumble */
    g_dualshock[0].type = g_dualshock[1].type = PAD_TYPE_DIGITAL;
    sio_init();
    CHECK(SEND(0, r, 0x01, 0x43, 0x00, 0x01, 0x00) == 1);
    CHECK(r[1] == 0xFF);
    CHECK(SEND(0, r, 0x01, 0x45, 0x00, 0x00, 0x00) == 1);
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x00, 0x00) == 4);   /* still digital: config was refused */
    CHECK(r[1] == 0x41 && r[2] == 0x5A && r[3] == (uint8_t)dig && r[4] == (uint8_t)(dig >> 8));
    CHECK(SEND(0, r, 0x01, 0x42, 0x00, 0x40, 0x01) == 4);
    CHECK(g_dualshock[0].motor_small == 0 && !g_dualshock[0].analog);
    pad_analog_button(0);
    CHECK(!g_dualshock[0].analog);
    CHECK(SEND(1, r, 0x01, 0x42, 0x00, 0x00, 0x00) == 4 && r[1] == 0x41);
    g_pad_connected[1] = false;
    CHECK(SEND(1, r, 0x01, 0x42, 0x00) == 0);

    printf("%d passed, %d failed\n", passed, failed);
    return failed != 0;
}
