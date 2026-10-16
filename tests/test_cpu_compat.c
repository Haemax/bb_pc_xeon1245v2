/* Tests of src/cpu_compat.c against the real instructions. Needs a CPU that has BMI1, MOVBE and
 * LZCNT: each case runs natively and through cc_emulate() on the same inputs and the registers,
 * the defined flags and memory are compared. Then a patched image (stubs and SIGILL) is run
 * against its unpatched twin. */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "cpu_compat.h"

static void run_native(CcRegs *g, const unsigned char *code);
static unsigned char *exec_buf;

/* Instruction cases: the bytes of an instruction, taken from the assembler. */
#define CASE(name, text) \
    __asm__(".pushsection .text\n" #name "_s:\n" text "\n" #name "_e:\n.popsection\n"); \
    extern const unsigned char name##_s[], name##_e[];
CASE(lz1, "lzcnt %rbx,%rax") CASE(lz2, "lzcnt %ebx,%eax") CASE(lz3, "lzcnt %bx,%ax")
CASE(lz4, "lzcnt %r10,%r9") CASE(lz5, "lzcnt (%rsi),%rax") CASE(lz6, "lzcnt 8(%rsi,%rcx,4),%edx")
CASE(lz7, "lzcnt -8(%r12,%r13,2),%r11d") CASE(lz8, "lzcnt 2(%rsi),%di") CASE(lz9, "lzcnt 0x20(%rsi),%r14")
CASE(tz1, "tzcnt %rbx,%rax") CASE(tz2, "tzcnt %ebx,%eax") CASE(tz3, "tzcnt %bx,%ax")
CASE(tz4, "tzcnt %r10,%r9") CASE(tz5, "tzcnt (%rsi),%rax") CASE(tz6, "tzcnt 8(%rsi,%rcx,4),%edx")
CASE(tz7, "tzcnt -8(%r12,%r13,2),%r11d") CASE(tz8, "tzcnt 2(%rsi),%di")
CASE(mb1, "movbe (%rsi),%rax") CASE(mb2, "movbe 4(%rsi),%edx") CASE(mb3, "movbe 2(%rsi),%dx")
CASE(mb4, "movbe %rax,(%rsi)") CASE(mb5, "movbe %edx,12(%rsi)") CASE(mb6, "movbe %dx,2(%rsi)")
CASE(mb7, "movbe %r9,(%r12,%r13,8)") CASE(mb8, "movbe 0x30(%r12),%r15d") CASE(mb9, "movbe %r10d,-4(%rsi,%rcx,4)")
CASE(an1, "andn %rcx,%rbx,%rax") CASE(an2, "andn (%rsi),%ebx,%eax") CASE(an3, "andn %r10,%r9,%r8")
CASE(an4, "andn 8(%rsi,%rcx,8),%r14d,%r15d")
CASE(be1, "bextr %rcx,%rbx,%rax") CASE(be2, "bextr %edx,(%rsi),%eax") CASE(be3, "bextr %r10,%r9,%r8")
CASE(be4, "bextr %r11d,4(%rsi),%r14d")
CASE(br1, "blsr %rbx,%rax") CASE(br2, "blsr (%rsi),%eax") CASE(br3, "blsr %r9d,%r8d") CASE(br4, "blsr %r10,%r11")
CASE(bm1, "blsmsk %rbx,%rax") CASE(bm2, "blsmsk 8(%rsi),%eax") CASE(bm3, "blsmsk %r9d,%r8d") CASE(bm4, "blsmsk %r10,%r11")
CASE(bi1, "blsi %rbx,%rax") CASE(bi2, "blsi (%rsi),%eax") CASE(bi3, "blsi %r9d,%r8d") CASE(bi4, "blsi %r10,%r11")
/* RIP-relative: the operand is 64 bytes past the instruction start. */
static const unsigned char rip_lz[] = {0xF3, 0x48, 0x0F, 0xBD, 0x05, 0, 0, 0, 0};
static const unsigned char rip_mb[] = {0x48, 0x0F, 0x38, 0xF1, 0x05, 0, 0, 0, 0};

#define F_CF 0x1u
#define F_ZF 0x40u
#define F_SF 0x80u
#define F_OF 0x800u
#define F_ARITH 0x8D5u

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) {
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 7; rng_state ^= rng_state << 17;
    return rng_state;
}
static uint64_t interesting(void) {
    switch (rnd() % 8) {
    case 0: return 0;
    case 1: return ~0ull;
    case 2: return 1ull << (rnd() % 64);
    case 3: return (1ull << (rnd() % 64)) - 1;
    case 4: return rnd() >> (rnd() % 64);
    case 5: return rnd() << (rnd() % 64);
    default: return rnd();
    }
}

static int failures;
static void expect(int ok, const char *what, const char *detail) {
    if (ok) return;
    if (failures++ < 30) fprintf(stderr, "FAIL %s: %s\n", what, detail);
}

static void run_case(const char *name, const unsigned char *code, size_t len, unsigned flag_mask, int rounds) {
    unsigned char *twin = exec_buf;
    memcpy(twin, code, len);
    twin[len] = 0xC3;
    for (int i = 0; i < rounds; ++i) {
        _Alignas(16) unsigned char buf_a[128], buf_b[128];
        for (int j = 0; j < 128; j += 8) { uint64_t v = interesting(); memcpy(buf_a + j, &v, 8); }
        memcpy(buf_b, buf_a, 128);
        CcRegs a, b;
        for (int r = 0; r < 16; ++r) a.r[r] = interesting();
        a.r[4] = 0;
        a.r[6] = (uint64_t)(uintptr_t)buf_a + 32; /* rsi: the buffer, with room for negative displacements */
        a.r[12] = (uint64_t)(uintptr_t)buf_a + 64;
        a.r[1] = rnd() % 4; a.r[13] = rnd() % 4;
        a.rflags = (rnd() & F_ARITH) | 0x2;
        a.rip = 0;
        b = a;
        b.r[6] = (uint64_t)(uintptr_t)buf_b + 32;
        b.r[12] = (uint64_t)(uintptr_t)buf_b + 64;
        /* bextr's control register: mostly small starts and lengths */
        if (rnd() % 4) {
            for (int r = 0; r < 16; ++r) if (r != 4 && r != 6 && r != 12 && (rnd() % 3 == 0)) {
                uint64_t c = (rnd() % 70) | ((rnd() % 70) << 8) | (rnd() & ~0xFFFFull);
                if (r != 1 && r != 13) { a.r[r] = c; b.r[r] = c; }
            }
        }
        CcRegs before = a;
        run_native(&a, twin);
        int n = cc_emulate(&b, code, (uint64_t)(uintptr_t)twin);
        char detail[256];
        snprintf(detail, sizeof detail, "length %d, expected %zu", n, len);
        expect(n == (int)len, name, detail);
        for (int r = 0; r < 16; ++r) {
            if (r == 4) continue;
            int pointer = r == 6 || r == 12;
            uint64_t want = a.r[r], got = b.r[r];
            if (pointer) { want -= (uint64_t)(uintptr_t)buf_a; got -= (uint64_t)(uintptr_t)buf_b; }
            if (want != got) {
                snprintf(detail, sizeof detail, "r%d: native %016llx emulated %016llx (inputs rbx=%016llx r9=%016llx rcx=%016llx r10=%016llx)", r,
                         (unsigned long long)want, (unsigned long long)got, (unsigned long long)before.r[3],
                         (unsigned long long)before.r[9], (unsigned long long)before.r[1], (unsigned long long)before.r[10]);
                expect(0, name, detail);
            }
        }
        if ((a.rflags & flag_mask) != (b.rflags & flag_mask) || (flag_mask != F_ARITH && ((a.rflags ^ b.rflags) & F_ARITH & ~flag_mask) && 0)) {
            snprintf(detail, sizeof detail, "flags: native %03llx emulated %03llx (mask %03x)", (unsigned long long)(a.rflags & F_ARITH),
                     (unsigned long long)(b.rflags & F_ARITH), flag_mask);
            expect(0, name, detail);
        }
        if (memcmp(buf_a, buf_b, 128)) expect(0, name, "memory differs");
    }
}

/* The native runner: a generated thunk keeps the register shuffling out of the C compiler's hands. */
static void run_native(CcRegs *g, const unsigned char *code) {
    extern void test_call(CcRegs *, const void *);
    test_call(g, code);
}
__asm__(".text\n.globl test_call\ntest_call:\n"
        " push %rbx\n push %rbp\n push %r12\n push %r13\n push %r14\n push %r15\n"
        " push %rsi\n push %rdi\n"     /* [rsp] = regs, 8(%rsp) = code */
        " push 128(%rdi)\n popfq\n"
        " mov 0(%rdi),%rax\n mov 8(%rdi),%rcx\n mov 16(%rdi),%rdx\n mov 24(%rdi),%rbx\n mov 40(%rdi),%rbp\n"
        " mov 48(%rdi),%rsi\n mov 64(%rdi),%r8\n mov 72(%rdi),%r9\n mov 80(%rdi),%r10\n mov 88(%rdi),%r11\n"
        " mov 96(%rdi),%r12\n mov 104(%rdi),%r13\n mov 112(%rdi),%r14\n mov 120(%rdi),%r15\n mov 56(%rdi),%rdi\n"
        " call *8(%rsp)\n"
        " pushfq\n push %rax\n mov 16(%rsp),%rax\n"
        " mov %rcx,8(%rax)\n mov %rdx,16(%rax)\n mov %rbx,24(%rax)\n mov %rbp,40(%rax)\n mov %rsi,48(%rax)\n"
        " mov %rdi,56(%rax)\n mov %r8,64(%rax)\n mov %r9,72(%rax)\n mov %r10,80(%rax)\n mov %r11,88(%rax)\n"
        " mov %r12,96(%rax)\n mov %r13,104(%rax)\n mov %r14,112(%rax)\n mov %r15,120(%rax)\n"
        " pop %rcx\n mov %rcx,0(%rax)\n pop %rcx\n mov %rcx,128(%rax)\n"
        " add $16,%rsp\n pop %r15\n pop %r14\n pop %r13\n pop %r12\n pop %rbp\n pop %rbx\n ret\n");

/* ---- end-to-end: a patched image against its unpatched twin ---- */
extern const unsigned char e2e_s[], e2e_e[];
extern const unsigned char e2e_site0[], e2e_site1[], e2e_site2[], e2e_site3[], e2e_site4[], e2e_site5[], e2e_site6[], e2e_site7[], e2e_site8[], e2e_cave0[];
/* rdi, rsi: inputs; rdx: scratch buffer. Leaf function that keeps a value in the red zone and an
 * xmm register live across the emulated instructions; results go to the buffer. */
__asm__(".pushsection .text\n"
        "e2e_s:\n"
        " movq %rdi,%xmm0\n"
        " mov %rsi,-8(%rsp)\n mov %rdi,-16(%rsp)\n"
        "e2e_site0: lzcnt %rdi,%rax\n"          /* 5 bytes: stub */
        " mov %rax,16(%rdx)\n pushfq\n pop %rcx\n mov %rcx,24(%rdx)\n"
        "e2e_site1: tzcnt %esi,%ecx\n"          /* 4 bytes: 2-byte jmp to the padding below */
        " mov %rcx,32(%rdx)\n"
        " jmp 1f\n e2e_cave0: .fill 16,1,0xCC\n 1:\n"
        "e2e_site7: movbe (%rdx),%ecx\n"        /* 4 bytes: through the padding as well */
        " mov %rcx,104(%rdx)\n"
        "e2e_site2: movbe (%rdx),%r8\n"         /* 5 bytes */
        " mov %r8,40(%rdx)\n"
        "e2e_site3: andn %rsi,%rdi,%r9\n"       /* 5 bytes VEX */
        " mov %r9,48(%rdx)\n pushfq\n pop %rcx\n mov %rcx,56(%rdx)\n"
        "e2e_site4: blsr %rdi,%r10\n"
        " mov %r10,64(%rdx)\n"
        "e2e_site5: movbe %rsi,8(%rdx)\n"
        "e2e_site6: bextr %rsi,%rdi,%r11\n"
        " mov %r11,72(%rdx)\n"
        " .fill 300,1,0x90\n"                  /* out of reach of any padding */
        "e2e_site8: lzcnt %ecx,%eax\n"          /* 4 bytes: SIGILL */
        " mov %rax,112(%rdx)\n"
        " movq %xmm0,%rax\n mov %rax,80(%rdx)\n mov -8(%rsp),%rax\n mov %rax,88(%rdx)\n mov -16(%rsp),%rax\n mov %rax,96(%rdx)\n"
        " ret\n"
        "e2e_e:\n.popsection\n");

static unsigned char *e2e_image;
static size_t e2e_image_size = 1 << 20;
void *runtime_low_map(size_t size, int prot) {
    /* Right after the image, as the loader does. */
    void *p = mmap(e2e_image + e2e_image_size + 4096, size, prot, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static void on_sigill(int sig, siginfo_t *info, void *context) {
    (void)info;
    if (!cc_handle_sigill(context)) { dprintf(2, "unhandled SIGILL (%d)\n", sig); _exit(3); }
}
typedef void (*E2E)(uint64_t, uint64_t, uint64_t *);

static void test_end_to_end(void) {
    size_t length = (size_t)(e2e_e - e2e_s);
    e2e_image = mmap(NULL, e2e_image_size + (1 << 20) * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (e2e_image == MAP_FAILED) { perror("mmap"); exit(1); }
    munmap(e2e_image + e2e_image_size, (1 << 20) * 2); /* the stubs go here */
    memset(e2e_image, 0xCC, e2e_image_size);
    memcpy(e2e_image + 0x1000, e2e_s, length);
    unsigned char *twin = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(twin, e2e_s, length);
    const unsigned char *sites[] = {e2e_site0, e2e_site1, e2e_site2, e2e_site3, e2e_site4, e2e_site5, e2e_site6, e2e_site7, e2e_site8};
    uint64_t offsets[9], count = 0, ncaves = 1, cave[2] = {0x1000 + (uint64_t)(e2e_cave0 - e2e_s), 16};
    for (size_t i = 0; i < 9; ++i) offsets[count++] = 0x1000 + (uint64_t)(sites[i] - e2e_s);
    char path[] = "/tmp/cc_table_XXXXXX";
    int fd = mkstemp(path);
    unsigned char magic[8] = "BBCPUC2";
    if (write(fd, magic, 8) != 8 || write(fd, &count, 8) != 8 || write(fd, &ncaves, 8) != 8 ||
        write(fd, offsets, count * 8) != (ssize_t)(count * 8) || write(fd, cave, 16) != 16) exit(1);
    close(fd);
    expect(cc_load(path, e2e_image_size) == 9, "e2e", "cc_load");
    unlink(path);
    CcRange code = {0x1000, 4096};
    expect(cc_apply(e2e_image, e2e_image_size, &code, 1) == 0, "e2e", "cc_apply");
    mprotect(e2e_image, e2e_image_size, PROT_READ | PROT_EXEC);
    /* The 4-byte sites: two reach the padding (jmp rel8), the one out of reach traps (ud2). */
    expect(e2e_image[0x1000 + (e2e_site1 - e2e_s)] == 0xEB && e2e_image[0x1000 + (e2e_site7 - e2e_s)] == 0xEB, "e2e", "4-byte sites not patched with jmp rel8");
    expect(e2e_image[0x1000 + (e2e_site8 - e2e_s)] == 0x0F && e2e_image[0x1000 + (e2e_site8 - e2e_s) + 1] == 0x0B, "e2e", "far site is not ud2");
    expect(e2e_image[0x1000 + (e2e_cave0 - e2e_s)] == 0xE9 && e2e_image[0x1000 + (e2e_cave0 - e2e_s) + 5] == 0xE9 &&
           e2e_image[0x1000 + (e2e_cave0 - e2e_s) + 10] == 0xCC, "e2e", "padding slots");
    struct sigaction sa = {0};
    sa.sa_sigaction = on_sigill; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGILL, &sa, NULL);
    E2E patched = (E2E)(e2e_image + 0x1000), original = (E2E)twin;
    for (int i = 0; i < 20000; ++i) {
        uint64_t a = interesting(), b = interesting();
        _Alignas(16) uint64_t x[16], y[16];
        for (int j = 0; j < 16; ++j) x[j] = y[j] = interesting();
        if (rnd() % 2) b = (rnd() % 70) | ((rnd() % 70) << 8);
        original(a, b, x);
        patched(a, b, y);
        for (int j = 0; j < 15; ++j) {
            uint64_t mx = x[j], my = y[j];
            if (j == 3 || j == 7 || j == 11) { mx &= F_ARITH & ~0x10 /* AF */; my &= F_ARITH & ~0x10; mx &= ~4ull; my &= ~4ull; }
            if (j == 3) { mx &= F_CF | F_ZF; my &= F_CF | F_ZF; }
            if (j == 7 || j == 11) { /* 11: the red-zone slot the guest's pushfq left behind (andn's flags) */ mx &= F_CF | F_ZF | F_SF | F_OF; my &= F_CF | F_ZF | F_SF | F_OF; }
            if (mx != my) {
                char d[200];
                snprintf(d, sizeof d, "slot %d: original %016llx patched %016llx (a=%016llx b=%016llx)", j, (unsigned long long)mx,
                         (unsigned long long)my, (unsigned long long)a, (unsigned long long)b);
                expect(0, "e2e", d);
            }
        }
    }
}

/* The in-process lzcnt/tzcnt (cc_build_bitscan) against the real instructions: every register pair,
 * 32 and 64 bits, registers and CF/ZF compared. */
static void test_bitscan(void) {
    unsigned char *native = exec_buf + 512, *stub = exec_buf + 1024;
    for (int tz = 0; tz < 2; ++tz)
        for (int sz = 32; sz <= 64; sz += 32)
            for (int dst = 0; dst < 16; ++dst)
                for (int src = 0; src < 16; ++src) {
                    if (dst == 4 || src == 4) continue; /* rsp */
                    unsigned char *p = native;
                    unsigned rex = 0x40u | (sz == 64 ? 8u : 0u) | (dst >= 8 ? 4u : 0u) | (src >= 8 ? 1u : 0u);
                    *p++ = 0xF3;
                    if (rex != 0x40) *p++ = (unsigned char)rex;
                    *p++ = 0x0F; *p++ = tz ? 0xBC : 0xBD; *p++ = (unsigned char)(0xC0 | (dst & 7) << 3 | (src & 7)); *p++ = 0xC3;
                    int n = cc_build_bitscan(stub, tz, sz, dst, src);
                    expect(n > 0 && n <= 48, "bitscan", "build failed");
                    if (n <= 0 || n > 48) continue;
                    stub[n] = 0xC3;
                    for (int r = 0; r < 400; ++r) {
                        CcRegs a, b;
                        for (int i = 0; i < 16; ++i) a.r[i] = interesting();
                        a.r[4] = 0;
                        a.rflags = (rnd() & F_ARITH) | 0x2;
                        a.rip = 0;
                        b = a;
                        run_native(&a, native);
                        run_native(&b, stub);
                        int bad = (a.rflags & (F_CF | F_ZF)) != (b.rflags & (F_CF | F_ZF));
                        for (int i = 0; i < 16; ++i) if (i != 4 && a.r[i] != b.r[i]) bad = 1;
                        char detail[96];
                        snprintf(detail, sizeof detail, "%s%d r%d,r%d src=%016llx", tz ? "tzcnt" : "lzcnt", sz, dst, src, (unsigned long long)a.r[src]);
                        expect(!bad, "bitscan", detail);
                    }
                }
}

/* The in-process forms (cc_build_inline) of the BMI1 and MOVBE instructions against the real ones: every
 * register role combination, memory operands [rsi+d], [rsp+d] and [rsi+r9*4+d], 32 and 64 bits. */
static unsigned char *enc_bmi(unsigned char *p, int kind, int w, int reg, int vvvv, int rm, int base, int index, int mem, int disp) {
    int R = reg >= 8, X = mem && index >= 8, B = mem ? base >= 8 : rm >= 8;
    int regf = kind < 2 ? reg : kind - 0; /* blsr /1, blsmsk /2, blsi /3 */
    *p++ = 0xC4; *p++ = (unsigned char)((!R) << 7 | (!X) << 6 | (!B) << 5 | 2);
    *p++ = (unsigned char)(w << 7 | ((~vvvv) & 15) << 3);
    *p++ = kind == 0 ? 0xF2 : kind == 1 ? 0xF7 : 0xF3;
    if (!mem) *p++ = (unsigned char)(0xC0 | (regf & 7) << 3 | (rm & 7));
    else {
        int sib = index >= 0 || (base & 7) == 4;
        *p++ = (unsigned char)(0x80 | (regf & 7) << 3 | (sib ? 4 : (base & 7)));
        if (sib) *p++ = (unsigned char)(2 << 6 | (index >= 0 ? index & 7 : 4) << 3 | (base & 7));
        memcpy(p, &disp, 4); p += 4;
    }
    return p;
}
static void test_inline(void) {
    static uint64_t area[64];
    unsigned char *native = exec_buf + 512, *stub = exec_buf + 1024;
    static const char *const names[] = {"andn", "bextr", "blsr", "blsmsk", "blsi", "movbe-load", "movbe-store"};
    for (int kind = 0; kind < 7; ++kind)
        for (int w = 0; w < 2; ++w)
            for (int mm = 0; mm < 4; ++mm)
                for (int x = 0; x < 16; ++x)
                    for (int y = 0; y < 16; ++y)
                        for (int z = 0; z < (mm ? 1 : 16); ++z) {
                            int reg = x, vvvv = y, rm = z, base = -1, index = -1, disp = 0, ismem = mm != 0;
                            if (mm == 1) { base = 6; disp = 0x20; }
                            if (mm == 2) { base = 4; disp = 0x20; }
                            if (mm == 3) { base = 6; index = 9; disp = 8; }
                            if (reg == 4 || vvvv == 4 || (!ismem && rm == 4)) continue;
                            if (ismem && (mm == 1 || mm == 3) && (reg == 6 || vvvv == 6)) continue;
                            if (ismem && mm == 3 && (reg == 9 || vvvv == 9)) continue;
                            unsigned char *p = native;
                            if (kind >= 5) {
                                if (!ismem || vvvv != 0 || (kind == 6 && mm == 2)) continue;
                                unsigned rex = 0x40u | (w ? 8u : 0u) | (reg >= 8 ? 4u : 0u) | (index >= 8 ? 2u : 0u) | (base >= 8 ? 1u : 0u);
                                if (rex != 0x40) *p++ = (unsigned char)rex;
                                *p++ = 0x0F; *p++ = 0x38; *p++ = kind == 5 ? 0xF0 : 0xF1;
                                int sib = index >= 0 || (base & 7) == 4;
                                *p++ = (unsigned char)(0x80 | (reg & 7) << 3 | (sib ? 4 : (base & 7)));
                                if (sib) *p++ = (unsigned char)(2 << 6 | (index >= 0 ? index & 7 : 4) << 3 | (base & 7));
                                memcpy(p, &disp, 4); p += 4;
                            } else p = enc_bmi(p, kind, w, reg, vvvv, rm, base, index, ismem, disp);
                            unsigned char insn[16] = {0};
                            memcpy(insn, native, (size_t)(p - native));
                            *p++ = 0xC3;
                            int n = cc_build_inline(stub, 120, insn);
                            if (!n) continue;
                            stub[n] = 0xC3;
                            for (int r = 0; r < 40; ++r) {
                                CcRegs a, b;
                                for (int i = 0; i < 16; ++i) a.r[i] = interesting();
                                a.r[4] = 0;
                                if (kind == 1 && (rnd() & 3) && vvvv != 4) a.r[vvvv] = (rnd() % 70) | ((rnd() % 70) << 8) | (rnd() & ~0xFFFFull);
                                a.r[6] = (uint64_t)(uintptr_t)area;
                                if (mm == 3) a.r[9] = rnd() % 3;
                                for (int i = 0; i < 64; ++i) area[i] = interesting();
                                a.rflags = (rnd() & F_ARITH) | 0x2;
                                a.rip = 0;
                                b = a;
                                uint64_t before[64], after_native[64];
                                memcpy(before, area, sizeof area);
                                run_native(&a, native);
                                memcpy(after_native, area, sizeof area);
                                memcpy(area, before, sizeof area);
                                run_native(&b, stub);
                                unsigned mask = kind >= 5 ? 0u : kind == 1 ? (F_CF | F_ZF | F_OF) : (F_CF | F_ZF | F_SF | F_OF);
                                int bad = (a.rflags & mask) != (b.rflags & mask) || memcmp(after_native, area, sizeof area);
                                for (int i = 0; i < 16; ++i) if (i != 4 && a.r[i] != b.r[i]) bad = 1;
                                char detail[96];
                                snprintf(detail, sizeof detail, "%s%d mode %d reg %d vvvv %d rm %d", names[kind], w ? 64 : 32, mm, reg, vvvv, rm);
                                expect(!bad, "inline", detail);
                            }
                        }
}

int main(void) {
    unsigned missing = cc_missing_features();
    if (missing) {
        printf("cpu_compat test skipped: this CPU lacks the instructions it compares against (missing mask %u)\n", missing);
        return 0;
    }
    exec_buf = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    test_bitscan();
    test_inline();
    struct { const char *name; const unsigned char *s, *e; unsigned mask; } cases[] = {
#define C(n, m) {#n, n##_s, n##_e, m}
        C(lz1, F_CF | F_ZF), C(lz2, F_CF | F_ZF), C(lz3, F_CF | F_ZF), C(lz4, F_CF | F_ZF), C(lz5, F_CF | F_ZF),
        C(lz6, F_CF | F_ZF), C(lz7, F_CF | F_ZF), C(lz8, F_CF | F_ZF), C(lz9, F_CF | F_ZF),
        C(tz1, F_CF | F_ZF), C(tz2, F_CF | F_ZF), C(tz3, F_CF | F_ZF), C(tz4, F_CF | F_ZF), C(tz5, F_CF | F_ZF),
        C(tz6, F_CF | F_ZF), C(tz7, F_CF | F_ZF), C(tz8, F_CF | F_ZF),
        C(mb1, F_ARITH), C(mb2, F_ARITH), C(mb3, F_ARITH), C(mb4, F_ARITH), C(mb5, F_ARITH), C(mb6, F_ARITH),
        C(mb7, F_ARITH), C(mb8, F_ARITH), C(mb9, F_ARITH),
        C(an1, F_CF | F_ZF | F_SF | F_OF), C(an2, F_CF | F_ZF | F_SF | F_OF), C(an3, F_CF | F_ZF | F_SF | F_OF),
        C(an4, F_CF | F_ZF | F_SF | F_OF),
        C(be1, F_CF | F_ZF | F_OF), C(be2, F_CF | F_ZF | F_OF), C(be3, F_CF | F_ZF | F_OF), C(be4, F_CF | F_ZF | F_OF),
        C(br1, F_CF | F_ZF | F_SF | F_OF), C(br2, F_CF | F_ZF | F_SF | F_OF), C(br3, F_CF | F_ZF | F_SF | F_OF), C(br4, F_CF | F_ZF | F_SF | F_OF),
        C(bm1, F_CF | F_ZF | F_SF | F_OF), C(bm2, F_CF | F_ZF | F_SF | F_OF), C(bm3, F_CF | F_ZF | F_SF | F_OF), C(bm4, F_CF | F_ZF | F_SF | F_OF),
        C(bi1, F_CF | F_ZF | F_SF | F_OF), C(bi2, F_CF | F_ZF | F_SF | F_OF), C(bi3, F_CF | F_ZF | F_SF | F_OF), C(bi4, F_CF | F_ZF | F_SF | F_OF),
#undef C
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(*cases); ++i)
        run_case(cases[i].name, cases[i].s, (size_t)(cases[i].e - cases[i].s), cases[i].mask, 3000);
    /* RIP-relative operands: the data lives 64 bytes into the code buffer. */
    {
        unsigned char code[9];
        int32_t disp = 64 - 9;
        memcpy(code, rip_lz, 9); memcpy(code + 5, &disp, 4);
        for (int i = 0; i < 500; ++i) {
            uint64_t v = interesting(), want, got;
            memcpy(exec_buf, code, 9); exec_buf[9] = 0xC3; memcpy(exec_buf + 64, &v, 8);
            CcRegs a = {{0}, 0, 0}, b;
            a.rflags = 2;
            b = a;
            run_native(&a, exec_buf);
            int n = cc_emulate(&b, code, (uint64_t)(uintptr_t)exec_buf);
            want = a.r[0]; got = b.r[0];
            expect(n == 9 && want == got && (a.rflags & (F_CF | F_ZF)) == (b.rflags & (F_CF | F_ZF)), "rip lzcnt", "mismatch");
        }
        memcpy(code, rip_mb, 9); memcpy(code + 5, &disp, 4);
        for (int i = 0; i < 500; ++i) {
            uint64_t v = interesting();
            CcRegs a = {{0}, 0, 0}, b;
            a.r[0] = v; a.rflags = 2; b = a;
            memcpy(exec_buf, code, 9); exec_buf[9] = 0xC3;
            run_native(&a, exec_buf);
            uint64_t native_mem; memcpy(&native_mem, exec_buf + 64, 8);
            memset(exec_buf + 64, 0, 8);
            int n = cc_emulate(&b, code, (uint64_t)(uintptr_t)exec_buf);
            uint64_t emu_mem; memcpy(&emu_mem, exec_buf + 64, 8);
            expect(n == 9 && native_mem == emu_mem, "rip movbe", "mismatch");
        }
    }
    /* Things that must not be handled. */
    {
        CcRegs g = {{0}, 0, 0};
        const unsigned char bsf[] = {0x0F, 0xBC, 0xC3};                 /* bsf: plain */
        const unsigned char crc[] = {0xF2, 0x0F, 0x38, 0xF0, 0xC3};     /* crc32 */
        const unsigned char pop[] = {0xF3, 0x0F, 0xB8, 0xC3};           /* popcnt */
        const unsigned char mulx[] = {0xC4, 0xE2, 0xFB, 0xF6, 0xC3};    /* mulx (BMI2, not handled) */
        expect(!cc_emulate(&g, bsf, 0) && !cc_emulate(&g, crc, 0) && !cc_emulate(&g, pop, 0) && !cc_emulate(&g, mulx, 0), "reject", "decoded a foreign instruction");
    }
    test_end_to_end();
    if (failures) { printf("cpu_compat tests: %d failures\n", failures); return 1; }
    printf("cpu_compat tests passed\n");
    return 0;
}
