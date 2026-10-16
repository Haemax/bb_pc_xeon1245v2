/* CPU compatibility layer, see cpu_compat.h.
 *
 * Built with -mgeneral-regs-only (build.sh): the stubs call into this file from the middle of
 * the game's code, where every SSE/AVX register is live, so nothing here may touch one. */
#define _GNU_SOURCE
#include "cpu_compat.h"
#include <cpuid.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <asm/prctl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#define F_CF 0x0001u
#define F_PF 0x0004u
#define F_AF 0x0010u
#define F_ZF 0x0040u
#define F_SF 0x0080u
#define F_OF 0x0800u

enum { K_TZCNT = 1, K_LZCNT, K_MOVBE_LOAD, K_MOVBE_STORE, K_ANDN, K_BEXTR, K_BLSR, K_BLSMSK, K_BLSI };

typedef struct {
    int kind, opsz, len;
    int reg;        /* ModRM.reg with REX.R / VEX.R */
    int vvvv;       /* VEX.vvvv */
    int isreg, rm;  /* ModRM.rm is a register (number with REX.B) */
    int base, index, scale, riprel, seg;
    int64_t disp;
} Dec;

static uint32_t rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }

/* Decodes one of the handled instructions. Returns its length, or 0. */
static int decode(const unsigned char *p, Dec *d) {
    const unsigned char *s = p;
    int o16 = 0, rep = 0, repne = 0, rexbits = 0, R = 0, X = 0, B = 0, W = 0;
    memset(d, 0, sizeof(*d));
    d->base = d->index = -1;
    for (int n = 0; n < 4; ++n) {
        unsigned char b = *p;
        if (b == 0x66) o16 = 1;
        else if (b == 0xF3) rep = 1;
        else if (b == 0xF2) repne = 1;
        else if (b == 0x64) d->seg = 1;
        else if (b == 0x65) d->seg = 2;
        else if (b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36) { /* ignored in 64-bit mode */ }
        else if (b == 0x67) return 0;
        else break;
        ++p;
    }
    if ((*p & 0xF0) == 0x40) {
        rexbits = *p++;
        W = (rexbits >> 3) & 1; R = (rexbits >> 2) & 1; X = (rexbits >> 1) & 1; B = rexbits & 1;
    }
    int group = 0;
    if (*p == 0xC4) { /* three-byte VEX: BMI1 lives in map 0F38, pp = none, L = 0 */
        if (rexbits || o16 || rep || repne) return 0;
        unsigned v1 = p[1], v2 = p[2], op = p[3];
        if ((v1 & 0x1F) != 2 || (v2 & 7) != 0) return 0;
        R = !(v1 & 0x80); X = !(v1 & 0x40); B = !(v1 & 0x20);
        W = v2 >> 7;
        d->vvvv = (~v2 >> 3) & 15;
        d->opsz = W ? 64 : 32;
        if (op == 0xF2) d->kind = K_ANDN;
        else if (op == 0xF7) d->kind = K_BEXTR;
        else if (op == 0xF3) group = 1;
        else return 0;
        p += 4;
    } else if (*p == 0x0F) {
        d->opsz = W ? 64 : o16 ? 16 : 32;
        if (p[1] == 0xBC || p[1] == 0xBD) {
            if (!rep || repne) return 0; /* without F3 they are BSF/BSR */
            d->kind = p[1] == 0xBC ? K_TZCNT : K_LZCNT;
            p += 2;
        } else if (p[1] == 0x38 && (p[2] == 0xF0 || p[2] == 0xF1)) {
            if (rep || repne) return 0; /* F2 0F 38 F0 is CRC32 */
            d->kind = p[2] == 0xF0 ? K_MOVBE_LOAD : K_MOVBE_STORE;
            p += 3;
        } else return 0;
    } else return 0;
    unsigned m = *p++;
    int mod = m >> 6, ext = (m >> 3) & 7, rm = m & 7;
    if (group) {
        if (ext == 1) d->kind = K_BLSR;
        else if (ext == 2) d->kind = K_BLSMSK;
        else if (ext == 3) d->kind = K_BLSI;
        else return 0;
    }
    d->reg = ext | (R << 3);
    if (mod == 3) {
        if (d->kind == K_MOVBE_LOAD || d->kind == K_MOVBE_STORE) return 0;
        d->isreg = 1;
        d->rm = rm | (B << 3);
    } else {
        if (rm == 4) {
            unsigned sib = *p++;
            int idx = ((sib >> 3) & 7) | (X << 3), bs = sib & 7;
            d->scale = sib >> 6;
            if (idx != 4) d->index = idx;
            if (bs == 5 && mod == 0) { d->disp = (int32_t)rd32(p); p += 4; }
            else d->base = bs | (B << 3);
        } else if (rm == 5 && mod == 0) {
            d->riprel = 1;
            d->disp = (int32_t)rd32(p);
            p += 4;
        } else d->base = rm | (B << 3);
        if (mod == 1) d->disp = (int8_t)*p++;
        else if (mod == 2) { d->disp = (int32_t)rd32(p); p += 4; }
    }
    d->len = (int)(p - s);
    return d->len <= 15 ? d->len : 0;
}

static uint64_t seg_base(int seg) {
#ifndef _WIN32
    unsigned long v = 0;
    if (syscall(SYS_arch_prctl, seg == 1 ? ARCH_GET_FS : ARCH_GET_GS, &v)) return 0;
    return v;
#else
    (void)seg; return 0;
#endif
}
static uint64_t effective_address(const Dec *d, const CcRegs *g, uint64_t rip) {
    uint64_t a = (uint64_t)d->disp;
    if (d->riprel) a += rip + (uint64_t)d->len;
    if (d->base >= 0) a += g->r[d->base];
    if (d->index >= 0) a += g->r[d->index] << d->scale;
    if (d->seg) a += seg_base(d->seg);
    return a;
}
static uint64_t load(uint64_t a, int bytes) {
    if (bytes == 8) { uint64_t v; memcpy(&v, (void *)(uintptr_t)a, 8); return v; }
    if (bytes == 4) { uint32_t v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
    uint16_t v; memcpy(&v, (void *)(uintptr_t)a, 2); return v;
}
static void store(uint64_t a, uint64_t value, int bytes) {
    if (bytes == 8) memcpy((void *)(uintptr_t)a, &value, 8);
    else if (bytes == 4) { uint32_t v = (uint32_t)value; memcpy((void *)(uintptr_t)a, &v, 4); }
    else { uint16_t v = (uint16_t)value; memcpy((void *)(uintptr_t)a, &v, 2); }
}
/* A 32-bit result clears the upper half, a 16-bit one keeps it. */
static void put(CcRegs *g, int reg, int bits, uint64_t value) {
    if (bits == 64) g->r[reg] = value;
    else if (bits == 32) g->r[reg] = (uint32_t)value;
    else g->r[reg] = (g->r[reg] & ~UINT64_C(0xFFFF)) | (value & 0xFFFF);
}

int cc_emulate(CcRegs *g, const unsigned char *code, uint64_t rip) {
    Dec d;
    if (!decode(code, &d)) return 0;
    if (!g) return d.len;
    const int n = d.opsz;
    const uint64_t mask = n == 64 ? ~UINT64_C(0) : (UINT64_C(1) << n) - 1;
    const uint64_t msb = UINT64_C(1) << (n - 1);
    uint64_t a = 0, src = 0, res = 0, flags = 0;
    if (!d.isreg) a = effective_address(&d, g, rip);
    if (d.kind != K_MOVBE_STORE) src = (d.isreg ? g->r[d.rm] : load(a, n / 8)) & mask;
    switch (d.kind) {
    case K_TZCNT:
    case K_LZCNT:
        res = !src ? (uint64_t)n : d.kind == K_TZCNT ? (uint64_t)__builtin_ctzll(src)
                                                      : (uint64_t)__builtin_clzll(src) - (uint64_t)(64 - n);
        flags = (!src ? F_CF : 0) | (!res ? F_ZF : 0);
        put(g, d.reg, n, res);
        break;
    case K_MOVBE_LOAD:
        res = n == 64 ? __builtin_bswap64(src) : n == 32 ? __builtin_bswap32((uint32_t)src) : __builtin_bswap16((uint16_t)src);
        put(g, d.reg, n, res);
        break;
    case K_MOVBE_STORE:
        src = g->r[d.reg] & mask;
        res = n == 64 ? __builtin_bswap64(src) : n == 32 ? __builtin_bswap32((uint32_t)src) : __builtin_bswap16((uint16_t)src);
        store(a, res, n / 8);
        break;
    case K_ANDN:
        res = ~(g->r[d.vvvv] & mask) & src & mask;
        flags = (!res ? F_ZF : 0) | ((res & msb) ? F_SF : 0);
        put(g, d.reg, n, res);
        break;
    case K_BEXTR: {
        uint64_t ctl = g->r[d.vvvv], start = ctl & 0xFF, len = (ctl >> 8) & 0xFF;
        res = start >= (uint64_t)n ? 0 : src >> start;
        if (len < (uint64_t)n) res &= (UINT64_C(1) << len) - 1;
        res &= mask;
        flags = !res ? F_ZF : 0;
        put(g, d.reg, n, res);
        break;
    }
    case K_BLSR:
        res = src & (src - 1) & mask;
        flags = (!src ? F_CF : 0) | (!res ? F_ZF : 0) | ((res & msb) ? F_SF : 0);
        put(g, d.vvvv, n, res);
        break;
    case K_BLSMSK:
        res = (src ^ (src - 1)) & mask;
        flags = (!src ? F_CF : 0) | ((res & msb) ? F_SF : 0);
        put(g, d.vvvv, n, res);
        break;
    case K_BLSI:
        res = src & (~src + 1) & mask;
        flags = (src ? F_CF : 0) | (!res ? F_ZF : 0) | ((res & msb) ? F_SF : 0);
        put(g, d.vvvv, n, res);
        break;
    default:
        return 0;
    }
    if (d.kind != K_MOVBE_LOAD && d.kind != K_MOVBE_STORE)
        g->rflags = (g->rflags & ~(uint64_t)(F_CF | F_PF | F_AF | F_ZF | F_SF | F_OF)) | flags;
    g->rip = rip + (uint64_t)d.len;
    return d.len;
}

unsigned cc_missing_features(void) {
    unsigned a, b, c, d, missing = 0, leaf1_c = 0, leaf7_b = 0, ext1_c = 0;
    if (__get_cpuid(1, &a, &b, &c, &d)) leaf1_c = c;
    if (__get_cpuid_count(7, 0, &a, &b, &c, &d)) leaf7_b = b;
    if (__get_cpuid(0x80000001u, &a, &b, &c, &d)) ext1_c = c;
    if (!(leaf7_b & (1u << 3))) missing |= CC_BMI1;
    if (!(leaf1_c & (1u << 22))) missing |= CC_MOVBE;
    if (!(ext1_c & (1u << 5))) missing |= CC_LZCNT;
    return missing;
}

#ifndef _WIN32
typedef struct {
    uint32_t off;
    unsigned char len, via;
    unsigned char orig[16];
} Site;
enum { VIA_TRAP, VIA_STUB, VIA_CAVE }; /* SIGILL; jmp to the stub; 2-byte jmp to a padding cave that jumps to the stub */

static Site *sites;
static size_t nsites;
static unsigned char *image_base;
static size_t image_len;
static uint64_t *table;
static uint64_t table_count;
static unsigned char *stub_memory;
static size_t stub_capacity;
static CcRange *caves; /* padding runs (BBCPUC2), sorted */
static size_t ncaves;
static uint64_t *cave_next; /* first byte of each cave not handed out yet */
static unsigned char *entry_len; /* decoded length of each table entry before patching (0: not decodable) */
static uint64_t *hits;      /* BB_CPU_COMPAT_STATS: per-site counters (not kept by in-process stubs) */
static uint64_t *targets;   /* BBCPUC3: direct branch targets that lie just after a listed site, sorted */
static size_t ntargets;
static int reloc_enabled;   /* the table lists the branch targets, so instructions may be relocated */
#define CC_STUB_SIZE 128    /* room for an in-process bit-scan, one relocated instruction and the jump back */

extern void *runtime_low_map(size_t size, int prot) __attribute__((weak));
extern void cc_common(void);
void cc_dispatch(CcRegs *g);

/* Stub entry: the stub pushed the site index over the 128-byte red zone it skipped. The frame is
 * saved in CcRegs order (rax lowest), cc_dispatch emulates in place and leaves the address to
 * continue at in the slot of the index; `ret $128` jumps there and drops the red zone again. */
__asm__(".text\n.globl cc_common\n.type cc_common,@function\ncc_common:\n"
        " pushfq\n push %r15\n push %r14\n push %r13\n push %r12\n push %r11\n push %r10\n push %r9\n push %r8\n"
        " push %rdi\n push %rsi\n push %rbp\n push $0\n push %rbx\n push %rdx\n push %rcx\n push %rax\n"
        " mov %rsp,%rdi\n mov %rsp,%rbp\n and $-16,%rsp\n cld\n call cc_dispatch\n mov %rbp,%rsp\n"
        " pop %rax\n pop %rcx\n pop %rdx\n pop %rbx\n add $8,%rsp\n pop %rbp\n pop %rsi\n pop %rdi\n"
        " pop %r8\n pop %r9\n pop %r10\n pop %r11\n pop %r12\n pop %r13\n pop %r14\n pop %r15\n"
        " popfq\n ret $128\n"
        ".size cc_common,.-cc_common\n");

static const Site *find_site(uint64_t offset) {
    size_t lo = 0, hi = nsites;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (sites[mid].off < offset) lo = mid + 1; else hi = mid;
    }
    return lo < nsites && sites[lo].off == offset ? &sites[lo] : NULL;
}

void cc_dispatch(CcRegs *g) {
    const Site *s = &sites[g->rip];
    if (hits) ++hits[g->rip];
    g->r[4] = (uint64_t)(uintptr_t)&g->rip + 8 + 128; /* the guest's rsp before the stub */
    if (!cc_emulate(g, s->orig, (uint64_t)(uintptr_t)image_base + s->off)) __builtin_trap();
}

int64_t cc_load(const char *path, size_t image_size) {
    FILE *f = fopen(path, "rb");
    unsigned char head[16];
    int v2 = 0, v3 = 0;
    uint64_t ncave_entries = 0, ntarget_entries = 0;
    if (!f || fread(head, 1, 16, f) != 16 || (memcmp(head, "BBCPUC1", 8) && !(v2 = !memcmp(head, "BBCPUC2", 8)) && !(v3 = !memcmp(head, "BBCPUC3", 8)))) {
        fprintf(stderr, "ERROR: invalid CPU compatibility table %s (scripts/cpu_compat.py writes it)\n", path);
        if (f) fclose(f);
        return -1;
    }
    memcpy(&table_count, head + 8, 8);
    if (v3) v2 = 1;
    if (v2 && fread(&ncave_entries, 8, 1, f) != 1) ncave_entries = UINT64_MAX;
    if (v3 && fread(&ntarget_entries, 8, 1, f) != 1) ntarget_entries = UINT64_MAX;
    if (table_count > 4000000 || ncave_entries > 4000000 || ntarget_entries > 40000000) { fclose(f); fprintf(stderr, "ERROR: CPU compatibility table too large\n"); return -1; }
    free(table);
    free(caves);
    caves = NULL;
    ncaves = 0;
    free(targets);
    targets = NULL;
    ntargets = 0;
    table = malloc((table_count + 1) * 8);
    CcRange *list = malloc((ncave_entries + 1) * sizeof(CcRange));
    uint64_t *tlist = malloc((ntarget_entries + 1) * 8);
    if (!table || !list || !tlist || fread(table, 8, table_count, f) != table_count ||
        fread(list, sizeof(CcRange), ncave_entries, f) != ncave_entries ||
        fread(tlist, 8, ntarget_entries, f) != ntarget_entries || fgetc(f) != EOF) {
        free(tlist);
        fclose(f);
        free(list);
        fprintf(stderr, "ERROR: truncated CPU compatibility table\n");
        return -1;
    }
    fclose(f);
    caves = list;
    ncaves = (size_t)ncave_entries;
    /* Without a BBCPUC3 table nothing is known about branch targets: no instruction is relocated. */
    targets = v3 ? tlist : (free(tlist), NULL);
    ntargets = v3 ? (size_t)ntarget_entries : 0;
    reloc_enabled = v3;
    stub_capacity = (size_t)(table_count + 1) * CC_STUB_SIZE;
    stub_capacity = (stub_capacity + 4095) & ~(size_t)4095;
    /* Next to the image: a jump rel32 has to reach it. */
    stub_memory = runtime_low_map ? runtime_low_map(stub_capacity, PROT_READ | PROT_WRITE | PROT_EXEC) : NULL;
    if (!stub_memory) {
        void *p = mmap(NULL, stub_capacity, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        stub_memory = p == MAP_FAILED ? NULL : p;
    }
    (void)image_size;
    return (int64_t)table_count;
}

static int cmp_u64(const void *x, const void *y) {
    uint64_t a = *(const uint64_t *)x, b = *(const uint64_t *)y;
    return a < b ? -1 : a > b;
}


/* In-process LZCNT/TZCNT on registers: no call, no stack, only the flags change. CF and ZF are the
 * architectural ones (CF = source is zero, ZF = result is zero); the flags AMD leaves undefined are
 * whatever TEST/XOR/INC leave. Writes the code to `out` and returns its length, or 0 when the
 * instruction is not a 32/64-bit register form. */
int cc_build_bitscan(unsigned char *out, int tzcnt, int opsz, int dst, int src) {
    if ((opsz != 32 && opsz != 64) || dst < 0 || dst > 15 || src < 0 || src > 15) return 0;
    unsigned char *p = out, *jz, *jmp;
    int w = opsz == 64 ? 8 : 0;
#define REX(r, b) do { unsigned x_ = 0x40 | (unsigned)w | ((r) >= 8 ? 4u : 0u) | ((b) >= 8 ? 1u : 0u); if (x_ != 0x40) *p++ = (unsigned char)x_; } while (0)
    REX(src, src); *p++ = 0x85; *p++ = (unsigned char)(0xC0 | (src & 7) << 3 | (src & 7));      /* test src,src */
    *p++ = 0x74; jz = p++;                                                                          /* jz zero */
    REX(dst, src); *p++ = 0x0F; *p++ = tzcnt ? 0xBC : 0xBD; *p++ = (unsigned char)(0xC0 | (dst & 7) << 3 | (src & 7)); /* bsf/bsr dst,src */
    if (tzcnt) { REX(dst, dst); *p++ = 0x85; *p++ = (unsigned char)(0xC0 | (dst & 7) << 3 | (dst & 7)); } /* test dst,dst */
    else { REX(0, dst); *p++ = 0x83; *p++ = (unsigned char)(0xF0 | (dst & 7)); *p++ = (unsigned char)(opsz - 1); } /* xor $w-1,dst */
    *p++ = 0xEB; jmp = p++;                                                                         /* jmp done */
    *jz = (unsigned char)(p - (jz + 1));
    *p++ = 0xF9;                                                                                    /* zero: stc */
    { int saved = w; w = 0; REX(0, dst); w = saved; }
    *p++ = (unsigned char)(0xB8 | (dst & 7)); { uint32_t v = (uint32_t)(opsz - 1); memcpy(p, &v, 4); p += 4; } /* mov $w-1,dst32 */
    REX(0, dst); *p++ = 0xFF; *p++ = (unsigned char)(0xC0 | (dst & 7));                             /* inc dst: ZF = 0, CF kept */
    *jmp = (unsigned char)(p - (jmp + 1));
#undef REX
    return (int)(p - out);
}


/* ---- In-process BMI1 (andn, bextr, blsr, blsmsk, blsi) -------------------------------------------
 * The same idea as the bit-scan stub: plain x86 instead of the call into cc_emulate (a full register
 * save and a decode, ~150+ cycles). Flags are the architectural ones (CF/ZF/SF/OF); PF/AF, undefined
 * for these instructions, are whatever the replacement leaves. A scratch register is saved on the
 * stack below the 128-byte red zone only where one is needed. Memory operands with a segment
 * prefix or RIP-relative addressing are left to the slow path. */
typedef struct { unsigned char *p; } Em;
static void e1(Em *e, unsigned v) { *e->p++ = (unsigned char)v; }
static void erex(Em *e, int w, int r, int x, int b) {
    unsigned v = 0x40u | (w ? 8u : 0u) | (r >= 8 ? 4u : 0u) | (x >= 8 ? 2u : 0u) | (b >= 8 ? 1u : 0u);
    if (v != 0x40) e1(e, v);
}
static void emodrm(Em *e, int reg, int rm) { e1(e, 0xC0u | (unsigned)(reg & 7) << 3 | (unsigned)(rm & 7)); }
/* op r/m(rm), r(reg) with a one-byte opcode */
static void err(Em *e, unsigned op, int w, int rm, int reg) { erex(e, w, reg, 0, rm); e1(e, op); emodrm(e, reg, rm); }
static void emov(Em *e, int w, int dst, int src) { if (dst != src || !w) err(e, 0x89, w, dst, src); }
static void eunary(Em *e, unsigned op, int ext, int w, int r) { erex(e, w, 0, 0, r); e1(e, op); emodrm(e, ext, r); }          /* F7 /2 not, F7 /3 neg, FF /0 inc, FF /1 dec */
static void eimm8(Em *e, int ext, int w, int r, int v) { erex(e, w, 0, 0, r); e1(e, 0x83); emodrm(e, ext, r); e1(e, (unsigned)v & 0xFF); } /* 83 /0 add, /7 cmp */
static void epush(Em *e, int r) { if (r >= 8) e1(e, 0x41); e1(e, 0x50u | (unsigned)(r & 7)); }
static void epop(Em *e, int r) { if (r >= 8) e1(e, 0x41); e1(e, 0x58u | (unsigned)(r & 7)); }
static void elea_rsp(Em *e, int delta) { /* lea delta(%rsp),%rsp: leaves the flags alone */
    e1(e, 0x48); e1(e, 0x8D);
    if (delta >= -128 && delta <= 127) { e1(e, 0x64); e1(e, 0x24); e1(e, (unsigned)delta & 0xFF); }
    else { e1(e, 0xA4); e1(e, 0x24); int32_t v = delta; memcpy(e->p, &v, 4); e->p += 4; }
}
/* [REX] opcode modrm for the instruction's own memory operand, `extra` added to an rsp-based displacement. */
static int emem(Em *e, unsigned op, int w, int reg, const Dec *d, int extra, int two_byte) {
    int64_t disp = d->disp + (d->base == 4 ? extra : 0);
    if (disp < INT32_MIN || disp > INT32_MAX) return 0;
    erex(e, w, reg, d->index, d->base);
    if (two_byte) e1(e, 0x0F);
    e1(e, op);
    int32_t v = (int32_t)disp;
    if (d->base < 0) {
        e1(e, (unsigned)(reg & 7) << 3 | 4);
        e1(e, (unsigned)d->scale << 6 | (unsigned)(d->index < 0 ? 4 : (d->index & 7)) << 3 | 5);
    } else if (d->index >= 0 || (d->base & 7) == 4) {
        e1(e, 0x80u | (unsigned)(reg & 7) << 3 | 4);
        e1(e, (unsigned)d->scale << 6 | (unsigned)(d->index < 0 ? 4 : (d->index & 7)) << 3 | (unsigned)(d->base & 7));
    } else e1(e, 0x80u | (unsigned)(reg & 7) << 3 | (unsigned)(d->base & 7));
    memcpy(e->p, &v, 4); e->p += 4;
    return 1;
}
static int pick_scratch(unsigned used) {
    static const int order[] = {0, 2, 3, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 5};
    for (size_t i = 0; i < sizeof order / sizeof *order; ++i) if (!(used & 1u << order[i])) return order[i];
    return -1;
}
static unsigned used_regs(const Dec *d) {
    unsigned u = 1u << 4;
    if (d->base >= 0) u |= 1u << d->base;
    if (d->index >= 0) u |= 1u << d->index;
    if (d->isreg) u |= 1u << d->rm;
    u |= 1u << d->reg | 1u << d->vvvv;
    return u;
}

/* Builds the replacement code for the instruction at `insn` into `out` (cap bytes); returns its
 * length, or 0 when the instruction has no in-process form. The code falls through at its end. */
int cc_build_inline(unsigned char *out, size_t cap, const unsigned char *insn) {
    Dec d;
    unsigned char buf[160];
    if (!decode(insn, &d)) return 0;
    if (d.kind == K_TZCNT || d.kind == K_LZCNT) {
        if (!d.isreg) return 0;
        int n = cc_build_bitscan(buf, d.kind == K_TZCNT, d.opsz, d.reg, d.rm);
        if (!n || (size_t)n > cap) return 0;
        memcpy(out, buf, (size_t)n);
        return n;
    }
    if ((d.kind == K_MOVBE_LOAD || d.kind == K_MOVBE_STORE) && d.opsz != 16 && !d.riprel && !d.seg && !d.isreg &&
        d.reg != d.base && d.reg != d.index) {
        /* movbe = mov + bswap, no flags and no stack: the store swaps the register, writes, swaps it back */
        Em M = {buf};
        const int mw = d.opsz == 64;
        if (d.kind == K_MOVBE_LOAD) {
            if (!emem(&M, 0x8B, mw, d.reg, &d, 0, 0)) return 0;
            erex(&M, mw, 0, 0, d.reg); e1(&M, 0x0F); e1(&M, 0xC8u | (unsigned)(d.reg & 7));
        } else if (mw) {
            erex(&M, 1, 0, 0, d.reg); e1(&M, 0x0F); e1(&M, 0xC8u | (unsigned)(d.reg & 7));
            if (!emem(&M, 0x89, 1, d.reg, &d, 0, 0)) return 0;
            erex(&M, 1, 0, 0, d.reg); e1(&M, 0x0F); e1(&M, 0xC8u | (unsigned)(d.reg & 7));
        } else {
            /* 32-bit: bswap on the register would clear its upper half, so swap a copy */
            int t = pick_scratch(used_regs(&d));
            if (t < 0) return 0;
            elea_rsp(&M, -128); epush(&M, t);
            emov(&M, 0, t, d.reg);
            erex(&M, 0, 0, 0, t); e1(&M, 0x0F); e1(&M, 0xC8u | (unsigned)(t & 7));
            if (!emem(&M, 0x89, 0, t, &d, 136, 0)) return 0;
            epop(&M, t); elea_rsp(&M, 128);
        }
        size_t mn = (size_t)(M.p - buf);
        if (mn > cap) return 0;
        memcpy(out, buf, mn);
        return (int)mn;
    }
    if (d.kind < K_ANDN || d.kind > K_BLSI) return 0;
    if (!d.isreg && (d.riprel || d.seg)) return 0;
    Em E = {buf}, *e = &E;
    const int w = d.opsz == 64, mem = !d.isreg;
    int dst, a = -1, src = d.rm;       /* andn: dst = ~a & src; bextr: ctrl in a; blsX: dst = f(src) */
    dst = (d.kind >= K_BLSR) ? d.vvvv : d.reg;
    a = d.vvvv;
    int t = -1;
    unsigned char *fix[8]; int nfix = 0;
#define JCC(opc) do { e1(e, opc); fix[nfix++] = e->p; e1(e, 0); } while (0)
#define HERE(k) (*fix[k] = (unsigned char)(e->p - (fix[k] + 1)))

    if (d.kind == K_ANDN) {
        if (!mem) {
            if (dst != src) { emov(e, w, dst, a); eunary(e, 0xF7, 2, w, dst); err(e, 0x21, w, dst, src); }
            else { err(e, 0x09, w, dst, a); err(e, 0x31, w, dst, a); }
        } else if (dst != a) {
            if (!emem(e, 0x8B, w, dst, &d, 0, 0)) return 0;
            err(e, 0x09, w, dst, a); err(e, 0x31, w, dst, a);
        } else {
            t = pick_scratch(used_regs(&d));
            if (t < 0) return 0;
            elea_rsp(e, -128); epush(e, t);
            if (!emem(e, 0x8B, w, t, &d, 136, 0)) return 0;
            eunary(e, 0xF7, 2, w, dst); err(e, 0x21, w, dst, t);
            epop(e, t); elea_rsp(e, 128);
        }
    } else if (d.kind == K_BEXTR) {
        unsigned used = used_regs(&d) | 2u;
        t = pick_scratch(used);
        if (t < 0) return 0;
        const int ctrl = a;
        elea_rsp(e, -128); epush(e, 1); epush(e, t);
        if (mem) { if (!emem(e, 0x8B, w, t, &d, 144, 0)) return 0; } else emov(e, w, t, src);
        if (ctrl != 1) emov(e, 0, 1, ctrl);                        /* mov ctrl32,%ecx */
        e1(e, 0x80); e1(e, 0xF9); e1(e, d.opsz);                    /* cmp $opsz,%cl */
        JCC(0x73);                                                  /* jae zero */
        erex(e, w, 0, 0, t); e1(e, 0xD3); emodrm(e, 5, t);          /* shr %cl,t */
        e1(e, 0x0F); e1(e, 0xB6); e1(e, 0xCD);                      /* movzbl %ch,%ecx */
        e1(e, 0x84); e1(e, 0xC9);                                   /* test %cl,%cl */
        JCC(0x74);                                                  /* jz zero */
        e1(e, 0x80); e1(e, 0xF9); e1(e, d.opsz);                    /* cmp $opsz,%cl */
        JCC(0x73);                                                  /* jae final */
        e1(e, 0xF6); e1(e, 0xD9);                                   /* neg %cl */
        e1(e, 0x80); e1(e, 0xC1); e1(e, d.opsz);                    /* add $opsz,%cl */
        erex(e, w, 0, 0, t); e1(e, 0xD3); emodrm(e, 4, t);          /* shl %cl,t */
        erex(e, w, 0, 0, t); e1(e, 0xD3); emodrm(e, 5, t);          /* shr %cl,t */
        e1(e, 0xEB); fix[nfix++] = e->p; e1(e, 0);                  /* jmp final */
        HERE(0); HERE(1);                                           /* zero: */
        err(e, 0x31, 0, t, t);                                      /* xor t32,t32 */
        HERE(2); HERE(3);                                           /* final: */
        err(e, 0x85, w, t, t);                                      /* test t,t: ZF, SF; CF = OF = 0 */
        if (dst == 1) {                                             /* the result goes into rcx's saved slot */
            erex(e, 1, t, 0, 0); e1(e, 0x89); e1(e, 0x44u | (unsigned)(t & 7) << 3); e1(e, 0x24); e1(e, 8); /* mov %t,8(%rsp): 64-bit, t is zero-extended */
        } else emov(e, w, dst, t);
        epop(e, t); epop(e, 1); elea_rsp(e, 128);
        goto done;
    } else {
        /* blsr / blsmsk / blsi: dst = f(src); a source in memory goes through a scratch register */
        int s = src;
        if (mem) {
            t = pick_scratch(used_regs(&d));
            if (t < 0) return 0;
            elea_rsp(e, -128); epush(e, t);
            if (!emem(e, 0x8B, w, t, &d, 136, 0)) return 0;
            s = t;
        }
        int sc = -1;
        if (!mem && dst == s) { sc = pick_scratch(used_regs(&d)); if (sc < 0) return 0; elea_rsp(e, -128); epush(e, sc); }
        if (d.kind == K_BLSR) {
            if (dst != s) {
                erex(e, w, dst, 0, s); e1(e, 0x8D); e1(e, 0x40u | (unsigned)(dst & 7) << 3 | (unsigned)(s & 7)); /* lea -1(s),dst */
                if ((s & 7) == 4) return 0;
                e1(e, 0xFF);
                err(e, 0x21, w, dst, s);                              /* and s,dst */
                JCC(0x75);                                            /* jnz done */
                eimm8(e, 7, w, s, 1);                                 /* cmp $1,s: CF = (s == 0) */
                eunary(e, 0xFF, 1, w, dst); eunary(e, 0xFF, 0, w, dst); /* dec/inc: ZF = 1, CF kept */
                HERE(0);
            } else {
                erex(e, w, sc, 0, s); e1(e, 0x8D); e1(e, 0x40u | (unsigned)(sc & 7) << 3 | (unsigned)(s & 7));
                if ((s & 7) == 4) return 0;
                e1(e, 0xFF);                                          /* lea -1(s),sc */
                err(e, 0x21, w, dst, sc);                             /* and sc,dst */
                JCC(0x75);
                eimm8(e, 0, w, sc, 1);                                /* add $1,sc: CF = (s was 0) */
                eunary(e, 0xFF, 1, w, dst); eunary(e, 0xFF, 0, w, dst);
                HERE(0);
            }
        } else if (d.kind == K_BLSMSK) {
            err(e, 0x85, w, s, s);                                    /* test s,s */
            JCC(0x74);                                                /* jz zero */
            if (dst != s) {
                erex(e, w, dst, 0, s); e1(e, 0x8D); e1(e, 0x40u | (unsigned)(dst & 7) << 3 | (unsigned)(s & 7));
                if ((s & 7) == 4) return 0;
                e1(e, 0xFF);
                err(e, 0x31, w, dst, s);                              /* xor s,dst */
            } else {
                erex(e, w, sc, 0, s); e1(e, 0x8D); e1(e, 0x40u | (unsigned)(sc & 7) << 3 | (unsigned)(s & 7));
                if ((s & 7) == 4) return 0;
                e1(e, 0xFF);
                err(e, 0x31, w, dst, sc);
            }
            e1(e, 0xEB); fix[nfix++] = e->p; e1(e, 0);                /* jmp done */
            HERE(0);                                                  /* zero: s == 0 -> all ones, CF = 1, SF = 1, ZF = 0 */
            eimm8(e, 7, w, s, 1);                                     /* cmp $1,s */
            if (w) { erex(e, 1, 0, 0, dst); e1(e, 0xC7); emodrm(e, 0, dst); { int32_t m1 = -1; memcpy(e->p, &m1, 4); e->p += 4; } }
            else { if (dst >= 8) e1(e, 0x41); e1(e, 0xB8u | (unsigned)(dst & 7)); { uint32_t m1 = 0xFFFFFFFFu; memcpy(e->p, &m1, 4); e->p += 4; } }
            HERE(1);
        } else { /* K_BLSI */
            if (dst != s) { emov(e, w, dst, s); eunary(e, 0xF7, 3, w, dst); err(e, 0x21, w, dst, s); }
            else { emov(e, w, sc, s); eunary(e, 0xF7, 3, w, sc); err(e, 0x21, w, dst, sc); }
            JCC(0x74);                                                /* jz done: CF = 0 */
            e1(e, 0xF9);                                              /* stc */
            HERE(0);
        }
        if (sc >= 0) { epop(e, sc); elea_rsp(e, 128); }
        if (mem) { epop(e, t); elea_rsp(e, 128); }
    }
done:
#undef JCC
#undef HERE
    {
        size_t n = (size_t)(e->p - buf);
        if (n > cap || n > sizeof buf) return 0;
        memcpy(out, buf, n);
        return (int)n;
    }
}

/* Length of the instruction at `p` when it can be copied elsewhere unchanged: mov $imm32,r; the
 * plain register/memory ALU forms; lea; ALU $imm8 - none of them RIP-relative, none a branch.
 * 0 otherwise. */
static int reloc_len(const unsigned char *p) {
    const unsigned char *s = p;
    if ((*p & 0xF0) == 0x40) ++p;
    unsigned rex = (p > s) ? s[0] : 0, op = *p++;
    if (op >= 0xB8 && op <= 0xBF) return rex & 8 ? 0 : (int)(p + 4 - s);
    static const unsigned char modrm_ops[] = {0x01, 0x03, 0x09, 0x0B, 0x21, 0x23, 0x29, 0x2B, 0x31, 0x33, 0x39, 0x3B, 0x85, 0x89, 0x8B, 0x8D, 0x83};
    int known = 0;
    for (size_t i = 0; i < sizeof modrm_ops; ++i) if (modrm_ops[i] == op) known = 1;
    if (!known) return 0;
    unsigned m = *p++, mod = m >> 6, rm = m & 7;
    if (op == 0x8D && mod == 3) return 0;
    if (mod != 3) {
        if (mod == 0 && rm == 5) return 0; /* RIP-relative */
        if (rm == 4) { unsigned sib = *p++; if (mod == 0 && (sib & 7) == 5) p += 4; }
        if (mod == 1) p += 1; else if (mod == 2) p += 4;
    }
    if (op == 0x83) p += 1;
    return (int)(p - s);
}

static int is_target(uint64_t lo, uint64_t hi) { /* any recorded branch target in [lo, hi) */
    size_t a = 0, b = ntargets;
    while (a < b) { size_t mid = (a + b) / 2; if (targets[mid] < lo) a = mid + 1; else b = mid; }
    return a < ntargets && targets[a] < hi;
}

static int in_code(uint64_t off, uint64_t len, const CcRange *code, size_t ncode, size_t image_size) {
    for (size_t c = 0; c < ncode; ++c)
        if (off >= code[c].address && off + len <= code[c].address + code[c].size && code[c].address + code[c].size <= image_size) return 1;
    return 0;
}

/* A 5-byte slot of padding that a 2-byte jmp at `site` reaches (rel8: -126..+129 bytes from the
 * site's start), or 0. The slot never overlaps a listed instruction. */
static uint64_t take_cave(uint64_t site, const CcRange *code, size_t ncode, size_t image_size) {
    uint64_t lo = site + 2 > 128 ? site + 2 - 128 : 0, hi = site + 2 + 127;
    size_t a = 0, b = ncaves;
    while (a < b) { /* first cave that ends after lo */
        size_t mid = (a + b) / 2;
        if (caves[mid].address + caves[mid].size <= lo) a = mid + 1; else b = mid;
    }
    for (size_t i = a; i < ncaves && caves[i].address <= hi; ++i) {
        uint64_t slot = cave_next[i] > lo ? cave_next[i] : lo;
        if (slot > hi || slot + 5 > caves[i].address + caves[i].size) continue;
        if (!in_code(slot, 5, code, ncode, image_size)) continue;
        size_t x = 0, y = (size_t)table_count; /* never on top of a listed instruction */
        while (x < y) { size_t mid = (x + y) / 2; if (table[mid] + 15 <= slot) x = mid + 1; else y = mid; }
        int clash = 0;
        for (; x < table_count && table[x] < slot + 5; ++x)
            if (table[x] + (entry_len[x] ? entry_len[x] : 15u) > slot) clash = 1;
        if (clash) continue;
        cave_next[i] = slot + 5;
        return slot;
    }
    return 0;
}

static const char *site_name(const Dec *d) {
    static const char *const names[] = {"?", "tzcnt", "lzcnt", "movbe(load)", "movbe(store)", "andn", "bextr", "blsr", "blsmsk", "blsi"};
    return d->kind >= 0 && d->kind <= K_BLSI ? names[d->kind] : "?";
}

typedef struct { uint64_t delta; uint32_t index; } StatRow;
static int cmp_rows(const void *x, const void *y) {
    const StatRow *a = x, *b = y;
    return a->delta < b->delta ? 1 : a->delta > b->delta ? -1 : 0;
}

/* BB_CPU_COMPAT_STATS=<seconds>: every interval, the busiest sites of that interval go to stderr. */
static void *stats_thread(void *arg) {
    unsigned interval = (unsigned)(uintptr_t)arg;
    uint64_t *previous = calloc(nsites + 1, 8);
    StatRow *rows = malloc((nsites + 1) * sizeof(StatRow));
    if (!previous || !rows) return NULL;
    for (;;) {
        sleep(interval);
        uint64_t total = 0, by_via[3] = {0, 0, 0};
        size_t n = 0;
        for (size_t i = 0; i < nsites; ++i) {
            uint64_t now = hits[i], delta = now - previous[i];
            previous[i] = now;
            if (!delta) continue;
            total += delta;
            by_via[sites[i].via] += delta;
            rows[n].delta = delta;
            rows[n++].index = (uint32_t)i;
        }
        if (!total) continue;
        qsort(rows, n, sizeof(StatRow), cmp_rows);
        fprintf(stderr, "CPU compatibility stats (%us): %" PRIu64 "/s emulated: SIGILL %" PRIu64 "/s, stubs %" PRIu64 "/s, via padding %" PRIu64 "/s; %zu sites hit\n",
                interval, total / interval, by_via[VIA_TRAP] / interval, by_via[VIA_STUB] / interval, by_via[VIA_CAVE] / interval, n);
        uint64_t top = 0;
        for (size_t r = 0; r < n && r < 16; ++r) {
            const Site *s = &sites[rows[r].index];
            Dec d;
            decode(s->orig, &d);
            top += rows[r].delta;
            fprintf(stderr, "  %2zu. +0x%08" PRIx32 " %-5s %" PRIu64 "/s  %s/%d %s  bytes", r + 1, s->off, s->via == VIA_TRAP ? "SIGILL" : s->via == VIA_CAVE ? "cave" : "stub",
                    rows[r].delta / interval, site_name(&d), d.opsz, d.isreg ? "reg" : "mem");
            for (int k = 0; k < s->len; ++k) fprintf(stderr, " %02x", s->orig[k]);
            fputc('\n', stderr);
        }
        fprintf(stderr, "  top %zu sites = %" PRIu64 "%% of all\n", n < 16 ? n : 16, top * 100 / total);
    }
}

int cc_apply(unsigned char *image, size_t image_size, const CcRange *code, size_t ncode) {
    image_base = image;
    image_len = image_size;
    qsort(table, table_count, 8, cmp_u64);
    free(sites);
    sites = calloc(table_count + 1, sizeof(*sites));
    if (!sites) return -1;
    size_t jumps = 0, traps = 0, skipped = 0, tramps = 0, inlined = 0, relocated = 0;
    uint64_t previous_end = 0;
    free(cave_next);
    free(entry_len);
    cave_next = calloc(ncaves + 1, 8);
    entry_len = calloc(table_count + 1, 1);
    if (!cave_next || !entry_len) return -1;
    for (uint64_t i = 0; i < table_count; ++i) { /* lengths before anything is patched */
        uint64_t off = table[i];
        unsigned char window[16] = {0};
        Dec d;
        if (off >= image_size) continue;
        memcpy(window, image + off, image_size - off < 15 ? image_size - off : 15);
        if (decode(window, &d)) entry_len[i] = (unsigned char)d.len;
    }
    for (size_t i = 0; i < ncaves; ++i) cave_next[i] = caves[i].address;
    for (uint64_t i = 0; i < table_count; ++i) {
        uint64_t off = table[i];
        int inside = 0;
        for (size_t c = 0; c < ncode; ++c)
            if (off >= code[c].address && off < code[c].address + code[c].size && code[c].address + code[c].size <= image_size) inside = 1;
        unsigned char window[16] = {0};
        if (inside) memcpy(window, image + off, image_size - off < 15 ? image_size - off : 15);
        Dec d = {0};
        if (!inside || off < previous_end || !decode(window, &d)) { ++skipped; continue; }
        Site *s = &sites[nsites];
        s->off = (uint32_t)off;
        s->len = (unsigned char)d.len;
        memcpy(s->orig, window, 15);
        unsigned char *site = image + off;
        unsigned char *stub = stub_memory ? stub_memory + nsites * CC_STUB_SIZE : NULL;
        intptr_t rel = stub ? (intptr_t)(stub - (site + 5)) : 0;
        int bs_len = 0;
        unsigned char body[CC_STUB_SIZE];
        if (stub) bs_len = cc_build_inline(body, CC_STUB_SIZE - 5, window);
        int rl = 0; /* instruction after a short site that moves into the stub, making room for a jmp rel32 */
        if (bs_len && d.len < 5 && reloc_enabled && off + 4 + 15 <= image_size && d.len == 4) {
            rl = reloc_len(image + off + 4);
            if (rl && (!in_code(off, (uint64_t)(4 + rl), code, ncode, image_size) || is_target(off + 1, off + 4 + (uint64_t)rl) ||
                       (i + 1 < table_count && table[i + 1] < off + 4 + (uint64_t)rl) || bs_len + rl + 5 > CC_STUB_SIZE)) rl = 0;
        }
        intptr_t stub_end = (intptr_t)(stub + bs_len + rl);
        if (bs_len && (d.len >= 5 || rl) && rel > INT32_MIN && rel < INT32_MAX) {
            /* In-process bit-scan: [scan][relocated instruction][jmp back]. */
            uint64_t back = off + (d.len >= 5 ? (uint64_t)d.len : 4u + (uint64_t)rl);
            memcpy(stub, body, (size_t)bs_len);
            if (rl) memcpy(stub + bs_len, image + off + 4, (size_t)rl);
            intptr_t brel = (intptr_t)(image + back) - (stub_end + 5);
            if (brel > INT32_MIN && brel < INT32_MAX) {
                stub[bs_len + rl] = 0xE9; { int32_t b32 = (int32_t)brel; memcpy(stub + bs_len + rl + 1, &b32, 4); }
                site[0] = 0xE9; { int32_t r32 = (int32_t)rel; memcpy(site + 1, &r32, 4); }
                memset(site + 5, 0x90, (size_t)(back - off - 5));
                s->via = VIA_STUB;
                s->len = (unsigned char)(back - off > 15 ? 15 : back - off);
                ++jumps; ++inlined;
                if (rl) ++relocated;
                previous_end = back;
                ++nsites;
                continue;
            }
        }
        if (d.len >= 5 && stub && rel > INT32_MIN && rel < INT32_MAX) {
            static const unsigned char lea_red_zone[5] = {0x48, 0x8D, 0x64, 0x24, 0x80}; /* lea -128(%rsp),%rsp */
            uint32_t index = (uint32_t)nsites;
            uintptr_t common = (uintptr_t)cc_common;
            memcpy(stub, lea_red_zone, 5);
            stub[5] = 0x68; memcpy(stub + 6, &index, 4);       /* push index */
            stub[10] = 0xFF; stub[11] = 0x25; memset(stub + 12, 0, 4); /* jmp *0(%rip) */
            memcpy(stub + 16, &common, 8);
            site[0] = 0xE9; { int32_t r32 = (int32_t)rel; memcpy(site + 1, &r32, 4); }
            memset(site + 5, 0x90, (size_t)d.len - 5);
            s->via = VIA_STUB;
            ++jumps;
        } else if (d.len >= 2 && d.len < 5 && stub && ncaves) {
            /* Too short for a jmp rel32: a 2-byte jmp to padding nearby, which jumps to the stub. */
            uint64_t slot = take_cave(off, code, ncode, image_size);
            unsigned char *cave = slot ? image + slot : NULL;
            intptr_t crel = cave ? (intptr_t)(stub - (cave + 5)) : 0;
            if (cave && crel > INT32_MIN && crel < INT32_MAX) {
                static const unsigned char lea_red_zone[5] = {0x48, 0x8D, 0x64, 0x24, 0x80};
                uint32_t index = (uint32_t)nsites;
                uintptr_t common = (uintptr_t)cc_common;
                memcpy(stub, lea_red_zone, 5);
                stub[5] = 0x68; memcpy(stub + 6, &index, 4);
                stub[10] = 0xFF; stub[11] = 0x25; memset(stub + 12, 0, 4);
                memcpy(stub + 16, &common, 8);
                cave[0] = 0xE9; { int32_t r32 = (int32_t)crel; memcpy(cave + 1, &r32, 4); }
                site[0] = 0xEB; site[1] = (unsigned char)(int8_t)((int64_t)slot - (int64_t)(off + 2));
                memset(site + 2, 0x90, (size_t)d.len - 2);
                s->via = VIA_CAVE;
                ++jumps;
                ++tramps;
            } else {
                site[0] = 0x0F; site[1] = 0x0B;
                memset(site + 2, 0x90, (size_t)d.len - 2);
                ++traps;
            }
        } else {
            site[0] = 0x0F; site[1] = 0x0B; /* ud2: SIGILL emulates it */
            memset(site + 2, 0x90, (size_t)d.len - 2);
            ++traps;
        }
        previous_end = off + (uint64_t)d.len;
        ++nsites;
    }
    if (stub_memory) mprotect(stub_memory, stub_capacity, PROT_READ | PROT_EXEC);
    free(table);
    table = NULL;
    free(targets);
    targets = NULL;
    ntargets = 0;
    free(entry_len);
    entry_len = NULL;
    printf("CPU compatibility: %zu instructions emulated (%zu through stubs, %zu through SIGILL), %zu table entries skipped\n",
           nsites, jumps, traps, skipped);
    printf("CPU compatibility: %zu of the stubs are reached through padding trampolines, %zu padding caves available\n", tramps, ncaves);
    printf("CPU compatibility: %zu instructions run in-process without a call (%zu of them by relocating the next instruction)%s\n", inlined, relocated,
           reloc_enabled ? "" : "; relocation off: the table has no branch targets");
    const char *stats = getenv("BB_CPU_COMPAT_STATS");
    if (stats && *stats && strcmp(stats, "0") && nsites) {
        unsigned interval = (unsigned)strtoul(stats, NULL, 10);
        if (interval < 1 || interval > 3600) interval = 10;
        hits = calloc(nsites + 1, 8);
        pthread_t t;
        if (hits && !pthread_create(&t, NULL, stats_thread, (void *)(uintptr_t)interval)) {
            pthread_detach(t);
            printf("CPU compatibility: per-site statistics every %u s on stderr (BB_CPU_COMPAT_STATS)\n", interval);
        } else { free(hits); hits = NULL; }
    }
    return 0;
}

int cc_handle_sigill(void *context) {
    ucontext_t *uc = context;
    greg_t *gr = uc->uc_mcontext.gregs;
    static const int map[16] = {REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
                                REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15};
    CcRegs g;
    for (int i = 0; i < 16; ++i) g.r[i] = (uint64_t)gr[map[i]];
    g.rflags = (uint64_t)gr[REG_EFL];
    uint64_t rip = (uint64_t)gr[REG_RIP];
    const unsigned char *code = (const unsigned char *)(uintptr_t)rip;
    if (image_base && rip >= (uintptr_t)image_base && rip - (uintptr_t)image_base < image_len) {
        const Site *s = find_site(rip - (uintptr_t)image_base);
        if (s) {
            code = s->orig;
            if (hits) ++hits[s - sites];
        }
    }
    if (!cc_emulate(&g, code, rip)) return 0;
    for (int i = 0; i < 16; ++i) gr[map[i]] = (greg_t)g.r[i];
    gr[REG_EFL] = (greg_t)g.rflags;
    gr[REG_RIP] = (greg_t)g.rip;
    return 1;
}
#else
int64_t cc_load(const char *p, size_t s) { (void)p; (void)s; return -1; }
int cc_apply(unsigned char *i, size_t s, const CcRange *c, size_t n) { (void)i; (void)s; (void)c; (void)n; return -1; }
int cc_handle_sigill(void *c) { (void)c; return 0; }
#endif
