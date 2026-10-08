#include "kernel/core/fault.h"
#include "kernel/klib/klib.h"
#include "kernel/core/sched.h"
#include "kernel/core/symbols.h"
#include "kernel/gfx/console.h"
#include "kernel/gfx/compositor.h"
#include "kernel/io.h"


// --- Fault reporter + contract implementation ---

namespace Fault {

// --- The last fault, for userland introspection ---
static Record last = {};
// ...and the one before it: a double fault's own frame says little (its cause
// is usually the stack it is pushed on), so its predecessor is the useful one.
static Record prev = {};

bool last_fault(Record* out) {
    if (!last.valid || out == nullptr) return false;
    *out = last;
    return true;
}

static void record_fault(IDT::Registers* regs, uint64_t cr2) {
    // Keep the one before this: a double fault is a *consequence*, and its own
    // frame says little, so the fault that led to it is what to look at.
    if (last.valid) prev = last;
    last.valid = 1;
    last.task_id = Sched::current_task_id();
    const char* name = Sched::current_task_name();
    for (int i = 0; i < 15 && name[i] != '\0'; i++) last.task_name[i] = name[i];
    last.task_name[15] = '\0';
    last.vector = regs->interrupt_number;
    last.error_code = regs->error_code;
    last.cr2 = cr2;
    last.rip = regs->rip; last.rsp = regs->rsp;
    last.rflags = regs->rflags; last.cs = regs->cs;
    last.rax = regs->rax; last.rbx = regs->rbx;
    last.rcx = regs->rcx; last.rdx = regs->rdx;
    last.rsi = regs->rsi; last.rdi = regs->rdi; last.rbp = regs->rbp;
    last.r8 = regs->r8;   last.r9 = regs->r9;
    last.r10 = regs->r10; last.r11 = regs->r11;
    last.r12 = regs->r12; last.r13 = regs->r13;
    last.r14 = regs->r14; last.r15 = regs->r15;
}

// --- Stack protector guard ---
// Clang on ELF references the global __stack_chk_guard (not the TLS form),
// so a plain global is all the wiring needed. Seeded in init() — before
// that it must be nonzero, or the very first check would pass trivially.
extern "C" uint64_t __stack_chk_guard = 0x00C0FFEE00000000ull;

void init() {
    // Boot-time entropy: TSC mixed with a twist. Not cryptographic —
    // just enough that the canary differs run to run.
    uint64_t tsc = 0;
    asm volatile("rdtsc" : "=a"(tsc) :: "rdx");
    __stack_chk_guard ^= tsc ^ (tsc << 32) ^ 0x9E3779B97F4A7C15ull;
    if (__stack_chk_guard == 0) __stack_chk_guard = 0x00C0FFEE00000000ull;
}

// --- Vector names (Intel SDM order) ---
static const char* vector_name(uint64_t v) {
    switch (v) {
        case 0:  return "#DE Divide-by-Zero";
        case 1:  return "#DB Debug";
        case 2:  return "NMI";
        case 3:  return "#BP Breakpoint";
        case 4:  return "#OF Overflow";
        case 5:  return "#BR BOUND";
        case 6:  return "#UD Invalid Opcode";
        case 7:  return "#NM Device Not Available";
        case 8:  return "#DF Double Fault";
        case 9:  return "Coprocessor Segment Overrun";
        case 10: return "#TS Invalid TSS";
        case 11: return "#NP Segment Not Present";
        case 12: return "#SS Stack Fault";
        case 13: return "#GP General Protection";
        case 14: return "#PF Page Fault";
        case 16: return "#MF x87";
        case 17: return "#AC Alignment Check";
        case 18: return "#MC Machine Check";
        case 19: return "#XM/#SIMD";
        case 20: return "#VE Virtualization";
        case 30: return "#SX Security";
        default: return "Reserved";
    }
}

// --- The dump: everything we know, in one place ---

// Which exceptions the CPU pushes an error code for. The stubs rely on this to
// keep one stack layout; the dump uses it to know whether that slot means what
// it says.
static bool has_error_code(uint64_t v) {
    switch (v) {
        case 8: case 10: case 11: case 12: case 13: case 14:
        case 17: case 21: case 29: case 30:
            return true;
        default:
            return false;
    }
}

static void dump(const char* title, IDT::Registers* regs, uint64_t cr2) {
    Console::ink(Console::LT_RED, Console::BLACK); // faults read red

    kprint("\n=== FAULT: ");
    kprint(title);
    kprint(" in task '");
    kprint(Sched::current_task_name());
    kprint("' (task #");
    kprint_uint(Sched::current_task_id());
    kprint(") ===\n");

    kprint("[fault] vector ");
    kprint_uint(regs->interrupt_number);
    kprint(" ");
    kprint(vector_name(regs->interrupt_number));
    kprint("\n");

    if (regs->interrupt_number == 8 && prev.valid) {
        // What led here: a double fault is a fault *while* handling one, and its
        // own frame (now on the IST stack, at least) describes the handler, not
        // the cause.
        kprint("[fault] the fault before it: vector ");
        kprint_uint(prev.vector);
        kprint(" ");
        kprint(vector_name(prev.vector));
        kprint(" at RIP=");
        kprint_hex(prev.rip);
        kprint(" CS=");
        kprint_hex(prev.cs);
        kprint("\n");
    }

    kprint("[fault] RIP=");
    kprint_hex(regs->rip);
    // Name it if we can: the table covers the kernel and every program, so this
    // works whether the fault is ours or the task's.
    {
        char sym[80];
        if (Symbols::resolve(regs->rip, sym, sizeof(sym))) {
            kprint(" (");
            kprint(sym);
            kprint(")");
        }
    }
    kprint(" CS=");
    kprint_hex(regs->cs);
    kprint(" RFLAGS=");
    kprint_hex(regs->rflags);
    kprint("\n");

    if (regs->interrupt_number == 14) {
        kprint("[fault] CR2=");
        kprint_hex(cr2);
        kprint(" (");
        kprint(regs->error_code & 1 ? "present" : "not-present");
        kprint(regs->error_code & 2 ? " write" : " read");
        kprint(regs->error_code & 4 ? " user" : " kernel");
        if (regs->error_code & 8) kprint(" ifetch");
        kprint(")\n");
    } else if (regs->interrupt_number == 13) {
        // #GP error code: the selector that caused the fault, when nonzero
        if (regs->error_code) {
            kprint("[fault] GP error code=");
            kprint_hex(regs->error_code);
            kprint("\n");
        }
    } else if (has_error_code(regs->interrupt_number)) {
        // Vectors 8 and 10-17 push one. Worth printing even when it is zero:
        // if the frame is shifted (a device interrupt delivered as an exception,
        // which pushes no error code at all), this slot holds the *real* RIP.
        kprint("[fault] error code=");
        kprint_hex(regs->error_code);
        kprint("\n");
    }

    kprint("[fault] RAX=");
    kprint_hex(regs->rax);
    kprint(" RBX=");
    kprint_hex(regs->rbx);
    kprint(" RCX=");
    kprint_hex(regs->rcx);
    kprint(" RDX=");
    kprint_hex(regs->rdx);
    kprint("\n");
    kprint("[fault] RSI=");
    kprint_hex(regs->rsi);
    kprint(" RDI=");
    kprint_hex(regs->rdi);
    kprint(" RBP=");
    kprint_hex(regs->rbp);
    kprint(" RSP=");
    kprint_hex(regs->rsp);
    kprint("\n");
    kprint("[fault] R8 =");
    kprint_hex(regs->r8);
    kprint(" R9 =");
    kprint_hex(regs->r9);
    kprint(" R10=");
    kprint_hex(regs->r10);
    kprint(" R11=");
    kprint_hex(regs->r11);
    kprint("\n");
    kprint("[fault] R12=");
    kprint_hex(regs->r12);
    kprint(" R13=");
    kprint_hex(regs->r13);
    kprint(" R14=");
    kprint_hex(regs->r14);
    kprint(" R15=");
    kprint_hex(regs->r15);
    kprint("\n");

    Console::ink(Console::WHITE, Console::BLACK); // back to normal ink
}

// --- The contract ---
[[noreturn]] void handle_cpu_exception(IDT::Registers* regs, uint64_t cr2) {
    // A fault *while reporting* a fault cannot be reported: the reporter would
    // recurse, and it recurses until the stack is gone, filling the log with
    // garbage instead of the one line that matters. (Seen: an early-boot page
    // fault whose dump itself faulted, and the serial log became thousands of
    // truncated fault headers.) One more fault here stops the machine, and the
    // partial dump is already in the log.
    static bool reporting = false;
    if (reporting) {
        kprint("\n[fault] fault while reporting a fault: halting\n");
        asm volatile("cli; hlt");
        while (true) asm("hlt");
    }
    reporting = true;

    record_fault(regs, cr2);
    bool from_user = (regs->cs & 3) == 3;
    if (from_user) {
        // Show the dump even if a full-screen window owns the console.
        Compositor::set_console_on_top(true);
        dump("USERLAND", regs, cr2);
        // A fault on the unmapped page just below a task's user stack is a
        // stack overflow — the guard page doing its job. Name it clearly.
        if (regs->interrupt_number == 14) {
            uint64_t guard = Sched::current_task_stack_guard();
            if (guard && cr2 >= guard && cr2 < guard + 4096)
                kprint("[fault] stack overflow detected (user stack guard page)\n");
        }
        kprint("[fault] terminating task per the fault contract\n");
        // Same teardown as exit(): fds, surfaces, address space, kstack.
        // The dying task's kernel stack stays valid until the next switch
        // reaps the zombie — exit_current handles all of that, and never
        // returns. (Interrupts are already off: interrupt gate.)
        //
        // This fault was reported and handled, so the next one is a new fault,
        // not a recursion: clear the guard before the task goes away. (A kernel
        // fault below never gets here — it halts, so the guard stays set.)
        reporting = false;
        Sched::exit_current(Sched::EXIT_FAULT);
    }

    // Ring 0: this is a kernel bug. Dump everything and halt — no
    // pretend-recovery, no silent spin.
    dump("KERNEL", regs, cr2);
    kprint("[fault] kernel fault: halting CPU\n");
    asm volatile("cli; hlt");
    while (true) asm("hlt");
}

// --- Kernel stack-protector trap ---
[[noreturn]] void stack_chk_fail_kernel(uint64_t caller_rip) {
    kprint("\n=== STACK SMASHING DETECTED (kernel) ===\n");
    kprint("[fault] caller RIP=");
    kprint_hex(caller_rip);
    kprint("\n");
    kprint("[fault] halting CPU\n");
    asm volatile("cli; hlt");
    while (true) asm("hlt");
}

} // namespace Fault

// --- The symbol clang emits calls to when a kernel canary check fails ---
extern "C" [[noreturn]] void __stack_chk_fail(void) {
    // Caller of the corrupted frame = our return address
    Fault::stack_chk_fail_kernel(
        (uint64_t)__builtin_return_address(0));
}
