#include "pic.h"
#include "../../kernel/io.h" // Note the path!

// PIC Ports
#define PIC1_COMMAND 0x20
#define PIC1_DATA    0x21
#define PIC2_COMMAND 0xA0
#define PIC2_DATA    0xA1

namespace PIC {

    void remap() {
        // Save current masks (who is allowed to speak)
        uint8_t a1 = inb(PIC1_DATA);
        uint8_t a2 = inb(PIC2_DATA);

        // Start initialization sequence (in cascade mode)
        outb(PIC1_COMMAND, 0x11); io_wait();
        outb(PIC2_COMMAND, 0x11); io_wait();

        // ICW2: Vector Offset
        // Map Master PIC to 32 (0x20)
        outb(PIC1_DATA, 0x20); io_wait();
        // Map Slave PIC to 40 (0x28)
        outb(PIC2_DATA, 0x28); io_wait();

        // ICW3: Tell Master about Slave (at IRQ2)
        outb(PIC1_DATA, 4); io_wait();
        // Tell Slave its cascade identity (2)
        outb(PIC2_DATA, 2); io_wait();

        // ICW4: 8086 mode
        outb(PIC1_DATA, 0x01); io_wait();
        outb(PIC2_DATA, 0x01); io_wait();

        // Restore saved masks
        outb(PIC1_DATA, a1);
        outb(PIC2_DATA, a2);
    }

    void send_eoi(uint8_t irq) {
        if (irq >= 8) outb(PIC2_COMMAND, 0x20);
        outb(PIC1_COMMAND, 0x20);
    }

    void unmask(uint8_t irq) {
        uint16_t port;
        uint8_t value;

        if (irq < 8) {
            port = PIC1_DATA;
        } else {
            port = PIC2_DATA;
            irq -= 8;
        }

        value = inb(port) & ~(1 << irq);
        outb(port, value);
    }
}