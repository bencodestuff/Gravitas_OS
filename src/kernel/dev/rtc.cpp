#include "rtc.h"
#include "kernel/io.h"
#include "kernel/core/timer.h"

extern "C" void kprint(const char* str);
extern "C" void kprint_uint(uint64_t value);

// The CMOS RTC. Registers are indexed through port 0x70 and read from 0x71.
// Two format bits matter (status register B): BCD vs binary, and 12h vs 24h.
namespace Rtc {

    static uint64_t epoch = 0;

    static uint8_t cmos_read(uint8_t reg) {
        outb(0x70, reg);
        return inb(0x71);
    }

    static int update_in_progress() {
        return cmos_read(0x0A) & 0x80;
    }

    static uint8_t bcd_to_bin(uint8_t v) {
        return (uint8_t)((v & 0x0F) + (v >> 4) * 10);
    }

    // Days from 1970-01-01 (Hinnant's algorithm; the kernel has no libc).
    static int64_t days_from_civil(int y, int m, int d) {
        y -= (m <= 2);
        int64_t era = (y >= 0 ? y : y - 399) / 400;
        unsigned yoe = (unsigned)(y - era * 400);
        unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
        unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
        return era * 146097 + (int64_t)doe - 719468;
    }

    void init() {
        uint8_t sec = 0, min = 0, hour = 0, day = 1, mon = 1, year = 0, cent = 0;

        // Wait out an update, read the fields, and re-check the seconds: if
        // they moved, the read straddled an update and must be redone.
        int tries = 0;
        for (;;) {
            while (update_in_progress()) { }
            sec   = cmos_read(0x00);
            min   = cmos_read(0x02);
            hour  = cmos_read(0x04);
            day   = cmos_read(0x07);
            mon   = cmos_read(0x08);
            year  = cmos_read(0x09);
            cent  = cmos_read(0x32);
            while (update_in_progress()) { }
            if (cmos_read(0x00) == sec || ++tries > 8) break;
        }

        uint8_t status_b = cmos_read(0x0B);
        int binary = status_b & 0x04;
        int is24 = status_b & 0x02;

        uint8_t pm = 0;
        if (!is24) { pm = hour & 0x80; hour &= 0x7F; }

        if (!binary) {
            sec  = bcd_to_bin(sec);
            min  = bcd_to_bin(min);
            hour = bcd_to_bin(hour);
            day  = bcd_to_bin(day);
            mon  = bcd_to_bin(mon);
            year = bcd_to_bin(year);
            cent = bcd_to_bin(cent);
        }

        if (!is24) {
            hour = (uint8_t)(hour % 12);
            if (pm) hour = (uint8_t)(hour + 12);
        }

        // The century register is optional; without it, guess from the year.
        int y = (cent ? cent * 100 : (year >= 70 ? 1900 : 2000)) + year;
        if (mon < 1 || mon > 12 || day < 1 || day > 31) {
            kprint("[RTC] CMOS returned an invalid date; wall clock disabled.\n");
            return;
        }

        int64_t days = days_from_civil(y, mon, day);
        epoch = (uint64_t)(days * 86400 + (int64_t)hour * 3600
                           + (int64_t)min * 60 + sec);

        kprint("[RTC] boot epoch: ");
        kprint_uint(epoch);
        kprint("\n");
    }

    uint64_t boot_epoch() { return epoch; }

    uint64_t seconds_now() {
        // Anchored at boot, advanced by the PIT.
        return epoch + Timer::get_ticks() / 50;
    }
}
