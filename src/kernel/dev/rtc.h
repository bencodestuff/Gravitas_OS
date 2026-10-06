#pragma once

#include <stdint.h>

// The CMOS real-time clock: read once at boot to anchor the wall clock. After
// that the clock runs off the PIT (50 Hz), so it drifts slightly; reading the
// CMOS on every call would be slower and could step backwards.
namespace Rtc {

    void init();                  // read the CMOS and set the boot epoch
    uint64_t boot_epoch();        // Unix seconds at boot (0 if unread)
    uint64_t seconds_now();       // wall-clock seconds since the epoch
}
