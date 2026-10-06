#include "kernel/dev/mouse.h"
#include "kernel/klib/klib.h"
#include "kernel/io.h"
#include "arch/x86_64/pic.h"


// --- PS/2 controller (8042) ports ---
constexpr uint8_t PS2_DATA    = 0x60;
constexpr uint8_t PS2_STATUS  = 0x64;
constexpr uint8_t PS2_COMMAND = 0x64;

// Controller commands
constexpr uint8_t CMD_ENABLE_AUX      = 0xA8; // enable second PS/2 port
constexpr uint8_t CMD_READ_CONFIG     = 0x20;
constexpr uint8_t CMD_WRITE_CONFIG    = 0x60;
constexpr uint8_t CMD_WRITE_AUX       = 0xD4; // next byte goes to the mouse

// Status register flags
constexpr uint8_t ST_OUTPUT_FULL = 0x01;
constexpr uint8_t ST_AUX_DATA    = 0x20; // set when the byte is from the mouse

// Mouse commands
constexpr uint8_t MOUSE_SET_DEFAULTS     = 0xF6;
constexpr uint8_t MOUSE_ENABLE_REPORTING = 0xF4;
constexpr uint8_t MOUSE_IDENTIFY         = 0xF2;
constexpr uint8_t MOUSE_SET_SAMPLE_RATE  = 0xF3;

// --- Packet state machine ---
// The mouse sends 3-byte packets (or 4-byte IntelliMouse packets with a Z
// byte). Byte 0 has bit3 ALWAYS SET (0x08) — that's the sync bit used to
// resynchronize if we ever join mid-packet.
constexpr uint8_t PACKET_FLAGS_ALWAYS = 0x08;
constexpr uint8_t PACKET_X_NEG  = 0x10;
constexpr uint8_t PACKET_Y_NEG  = 0x20;
constexpr uint8_t PACKET_BTN_LEFT   = 0x01;
constexpr uint8_t PACKET_BTN_RIGHT  = 0x02;
constexpr uint8_t PACKET_BTN_MIDDLE = 0x04;

namespace Mouse {

    // --- circular packet buffer (same pattern as the keyboard driver) ---
    struct Packet {
        int16_t dx, dy;
        int8_t  dz;       // wheel: positive = up/away, negative = down/toward
        uint8_t buttons;
    };

    #define BUFFER_SIZE 32
    static Packet packet_buffer[BUFFER_SIZE];
    static volatile int write_ptr = 0;
    static volatile int read_ptr = 0;

    // --- cursor position: deltas accumulate as packets arrive ---
    static int cur_x = 640, cur_y = 400; // start center-ish

    // --- assembly-stream state ---
    static uint8_t cycle = 0;
    static int     pktsize = 3;   // 3 standard, 4 IntelliMouse (with wheel)
    static uint8_t packet[4];

    // --- low-level helpers ---
    static void controller_wait_write() {
        // bit1 of the status = input buffer full (controller busy)
        for (int i = 0; i < 100000; i++) {
            if ((inb(PS2_STATUS) & 0x02) == 0) return;
        }
    }

    static void controller_wait_read() {
        for (int i = 0; i < 100000; i++) {
            if (inb(PS2_STATUS) & ST_OUTPUT_FULL) return;
        }
    }

    // Read one byte, requiring it to be mouse data (aux bit set).
    // Drains keyboard bytes encountered along the way. Returns 0xFF on
    // timeout (0xFF is also "resend", acceptable as a failure signal).
    static uint8_t mouse_read_ack() {
        for (int i = 0; i < 100000; i++) {
            uint8_t st = inb(PS2_STATUS);
            if (st & ST_OUTPUT_FULL) {
                uint8_t data = inb(PS2_DATA);
                if (st & ST_AUX_DATA) return data;
                // keyboard byte — stash it for the keyboard driver? For init
                // simplicity: drop it (no keys are being pressed at boot)
            }
        }
        return 0xFF;
    }

    static void mouse_send(uint8_t cmd) {
        controller_wait_write();
        outb(PS2_COMMAND, CMD_WRITE_AUX);
        controller_wait_write();
        outb(PS2_DATA, cmd);
        uint8_t ack = mouse_read_ack();
        if (ack != 0xFA) {
            kprint("[Mouse] WARN: cmd ");
            kprint_uint(cmd);
            kprint(" ack=");
            kprint_uint(ack);
            kprint(" (want FA)\n");
        }
    }

    // Send a command byte followed by one parameter byte (e.g. set sample
    // rate), ACK-checked at each step.
    static void mouse_send2(uint8_t cmd, uint8_t arg) {
        controller_wait_write();
        outb(PS2_COMMAND, CMD_WRITE_AUX);
        controller_wait_write();
        outb(PS2_DATA, cmd);
        uint8_t ack = mouse_read_ack();
        if (ack != 0xFA) {
            kprint("[Mouse] WARN: cmd ");
            kprint_uint(cmd);
            kprint(" ack=");
            kprint_uint(ack);
            kprint(" (want FA)\n");
        }
        controller_wait_write();
        outb(PS2_COMMAND, CMD_WRITE_AUX);
        controller_wait_write();
        outb(PS2_DATA, arg);
        ack = mouse_read_ack();
        if (ack != 0xFA) {
            kprint("[Mouse] WARN: arg ");
            kprint_uint(arg);
            kprint(" ack=");
            kprint_uint(ack);
            kprint(" (want FA)\n");
        }
    }

    void init() {
        // 1. Enable the auxiliary (mouse) port
        controller_wait_write();
        outb(PS2_COMMAND, CMD_ENABLE_AUX);

        // 2. Enable the controller's aux interrupt line in the config byte
        controller_wait_write();
        outb(PS2_COMMAND, CMD_READ_CONFIG);
        controller_wait_read();
        uint8_t config = inb(PS2_DATA);
        config |= 0x02;  // enable IRQ12 (aux output buffer full interrupt)
        config &= ~(uint8_t)0x20; // enable the mouse clock line (must be 0)
        controller_wait_write();
        outb(PS2_COMMAND, CMD_WRITE_CONFIG);
        controller_wait_write();
        outb(PS2_DATA, config);


        // 3. Mouse handshake. Drain anything stale first (aux bytes only),
        //    reset the device, then negotiate the IntelliMouse protocol: the
        //    sample-rate magic switches a wheel mouse to 4-byte packets with
        //    a Z byte; the id query tells us whether it took.
        for (int i = 0; i < 16; i++) {
            if (!(inb(PS2_STATUS) & ST_OUTPUT_FULL)) break;
            inb(PS2_DATA);
        }
        mouse_send(MOUSE_SET_DEFAULTS);
        mouse_send2(MOUSE_SET_SAMPLE_RATE, 200);
        mouse_send2(MOUSE_SET_SAMPLE_RATE, 100);
        mouse_send2(MOUSE_SET_SAMPLE_RATE, 80);

        // 4. Device id: 0x00 standard 3-byte, 0x03/0x04 IntelliMouse 4-byte.
        //    This also proves the whole 0xD4 write path works.
        controller_wait_write();
        outb(PS2_COMMAND, CMD_WRITE_AUX);
        controller_wait_write();
        outb(PS2_DATA, MOUSE_IDENTIFY);
        uint8_t a1 = mouse_read_ack();
        uint8_t id = mouse_read_ack();
        pktsize = (id == 3 || id == 4) ? 4 : 3;
        kprint("[Mouse] identify: ack=");
        kprint_uint(a1);
        kprint(" id=");
        kprint_uint(id);
        kprint(pktsize == 4 ? " (wheel, 4-byte)\n" : " (standard, 3-byte)\n");

        // 5. A sane sample rate, then start streaming.
        mouse_send2(MOUSE_SET_SAMPLE_RATE, 100);
        mouse_send(MOUSE_ENABLE_REPORTING);

        kprint("[Mouse] Initialized (IRQ12, streaming enabled).\n");

    }

    void handle_interrupt() {
        // Only touch the data port if this byte is actually mouse data
        if (!(inb(PS2_STATUS) & ST_AUX_DATA)) return; // keyboard byte — leave it
        uint8_t byte = inb(PS2_DATA);

        // Resync guard: byte 0 of a packet always has bit3 set. If we joined
        // mid-stream, skip bytes until we see the sync bit.
        if (cycle == 0 && !(byte & PACKET_FLAGS_ALWAYS)) {
            return;
        }

        packet[cycle++] = byte;

        if (cycle == pktsize) {
            cycle = 0;

            uint8_t flags = packet[0];

            // 9-bit two's-complement deltas: low 8 bits in the data
            // byte, sign extension bit in the flags
            int16_t dx = packet[1];
            int16_t dy = packet[2];
            if (flags & PACKET_X_NEG) dx |= 0xFF00;
            if (flags & PACKET_Y_NEG) dy |= 0xFF00;
            dy = -dy; // PS/2 reports Y screen-inverted: up is positive
                      // in the raw protocol; screen convention (down
                      // positive) needs the flip
            // The wheel byte's low nibble is a 4-bit two's-complement delta
            // and its high nibble is zero, so bit 3 must be sign-extended
            // explicitly. The wire convention (and QEMU) calls a notch *away
            // from the user* negative; the rest of the system uses
            // positive = up/away, so flip it.
            int8_t dz = 0;
            if (pktsize == 4) {
                uint8_t w = packet[3] & 0x0F;
                int8_t d = (int8_t)((w & 0x08) ? (w | 0xF0) : w);
                dz = (int8_t)(-d);
            }

            // The cursor advances with EVERY packet, independent of the
            // ring: it stays live even when no task is draining, and a
            // full ring only drops the queued event, never the pointer.
            cur_x += dx;
            cur_y += dy;
            if (cur_x < 0) cur_x = 0;
            if (cur_y < 0) cur_y = 0;

            int next_write = (write_ptr + 1) % BUFFER_SIZE;
            if (next_write != read_ptr) {
                Packet p;
                p.dx = dx;
                p.dy = dy;
                p.dz = dz;
                p.buttons = (flags & PACKET_BTN_LEFT  ? 1 : 0) |
                            (flags & PACKET_BTN_RIGHT ? 2 : 0) |
                            (flags & PACKET_BTN_MIDDLE? 4 : 0);

                packet_buffer[write_ptr] = p;
                write_ptr = next_write;
            }
            // ring full: drop the queued event (motion is lossy by nature)
        }
    }

    bool read_packet(int16_t* dx, int16_t* dy, uint8_t* buttons, int8_t* dz) {
        if (read_ptr == write_ptr) return false;
        Packet p = packet_buffer[read_ptr];
        read_ptr = (read_ptr + 1) % BUFFER_SIZE;
        if (dx) *dx = p.dx;
        if (dy) *dy = p.dy;
        if (buttons) *buttons = p.buttons;
        if (dz) *dz = p.dz;
        return true;
    }

    int cursor_x() { return cur_x; }
    int cursor_y() { return cur_y; }

    // applied by the compositor after clamping (framebuffer bounds known there)
    void apply_position_clamp(int x, int y) {
        cur_x = x; cur_y = y;
    }
}