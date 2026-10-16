/* CPU compatibility layer: runs Bloodborne's PS4 (AMD Jaguar) code on x86-64 CPUs that lack
 * BMI1, MOVBE or LZCNT (Intel Sandy Bridge / Ivy Bridge, Xeon E3 v1/v2, ...).
 *
 * The game's code uses these instructions directly. VEX-encoded BMI1 (andn, bextr, blsr, blsmsk,
 * blsi) and MOVBE raise SIGILL on such a CPU; LZCNT/TZCNT silently run as BSR/BSF (a different
 * result), so they cannot be caught at run time and are found offline (scripts/cpu_compat.py).
 *
 * Every listed site is replaced in the loaded image: by a jump to a small stub that emulates the
 * instruction in-process when the site is at least 5 bytes long; a shorter one by a 2-byte jump to
 * a trampoline in nearby padding (listed in the table), and only when there is none by UD2, which
 * SIGILL emulates. BB_CPU_COMPAT_STATS=<seconds> reports the busiest sites on stderr. The emulation itself is cc_emulate(). */
#ifndef CPU_COMPAT_H
#define CPU_COMPAT_H
#include <stddef.h>
#include <stdint.h>

#define CC_BMI1 1u
#define CC_MOVBE 2u
#define CC_LZCNT 4u

typedef struct {
    uint64_t r[16]; /* hardware numbering: rax rcx rdx rbx rsp rbp rsi rdi r8..r15 */
    uint64_t rflags;
    uint64_t rip;
} CcRegs;

/* Executes the instruction at `code` (whose address is `rip`) on `regs`, sets regs->rip to the next
 * instruction and returns its length. Returns 0, changing nothing, when it is not a handled
 * instruction. regs == NULL: only decodes (the length, or 0). */
int cc_emulate(CcRegs *regs, const unsigned char *code, uint64_t rip);

/* Which of CC_BMI1 | CC_MOVBE | CC_LZCNT this CPU lacks (0: it has them all). */
unsigned cc_missing_features(void);

/* In-process LZCNT/TZCNT (register forms, 32/64-bit): writes the code to `out` (48 bytes are enough)
 * and returns its length, 0 if not buildable. Exposed for the tests. */
int cc_build_bitscan(unsigned char *out, int tzcnt, int opsz, int dst, int src);

/* In-process form of any handled instruction (bit-scans and BMI1; register forms, and memory forms of
 * BMI1 without segment prefix or RIP-relative addressing): writes it to `out` (at most `cap` bytes) and
 * returns its length, 0 if there is none. Falls through at its end. Exposed for the tests. */
int cc_build_inline(unsigned char *out, size_t cap, const unsigned char *insn);

typedef struct { uint64_t address, size; } CcRange;

/* Step 1 (right after the image is allocated): reads the site table written by
 * scripts/cpu_compat.py and reserves memory for the stubs next to the image.
 * Returns the number of sites, or -1 on error (a message is printed). */
int64_t cc_load(const char *table_path, size_t image_size);
/* Step 2 (image still writable, patches applied): patches the sites that lie in `code` ranges.
 * Executable stub memory is made so before returning. Prints a summary. */
int cc_apply(unsigned char *image, size_t image_size, const CcRange *code, size_t ncode);
/* SIGILL: emulates the instruction when it is one of ours. Returns 1 if the context was changed
 * and execution can resume. `ucontext` is the third argument of a SA_SIGINFO handler. */
int cc_handle_sigill(void *ucontext);
#endif
