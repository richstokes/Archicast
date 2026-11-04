/* Run ARM code that was never recompiled, one instruction at a time.
 *
 * The recompiled ROM stays the operating system. This only takes over when control
 * reaches RAM (or a ROM path the static scan missed), so a game loaded from a real
 * disc can run on top of it. It shares all machine state with recompiled code and
 * keeps nothing on the native stack across a return to dispatch, so SWIs, aborts
 * and interrupts all take their normal routes. Semantics follow tools/armxlat.py.
 *
 * ARM_INTERP_ALL keeps ROM code in here too. That build exists to check this file
 * against the replay checksums, which were all recorded from recompiled code.
 *
 * Instructions dispatch on bits 27-20 through one table of label offsets (GCC's
 * labels as values). Data processing works out its operand once per form, then
 * jumps through a second table to a few instructions per opcode. On
 * the SH-4 the 8KB instruction cache decides the speed: a copy of each case per
 * opcode ran no faster than the old switch, as cache misses ate what it saved, so
 * rare paths share code out of line. */
#include "appunit.h"

#define ROM_BASE 0x3800000u
#define EXIT_SWI (1u << 26)
#define EXIT_UNDEF (2u << 26)
#define EXIT_POLL (3u << 26)

#ifdef ARM_INTERP_ALL
#define LEAVES(pc) 0
#else
#define LEAVES(pc) ((pc) >= ROM_BASE)
#endif

const u8 *mach_code_page(u32 pc);

#ifndef __sh__
unsigned long long arm_insns;
u32 arm_hist[0x80000 / 4]; /* RAM below 512KB, by address */
u32 arm_hist_rom[0x200000 / 4]; /* and the 2MB ROM, for the all-interpreted build */
u32 arm_hist_rma[0x200000 / 4]; /* modules, from 0x1800000 */
#define COUNT() (arm_insns++, pc < 0x80000 ? (void)arm_hist[pc >> 2]++ : \
	pc - ROM_BASE < 0x200000u ? (void)arm_hist_rom[(pc - ROM_BASE) >> 2]++ : \
	pc - 0x1800000u < 0x200000u ? (void)arm_hist_rma[(pc - 0x1800000u) >> 2]++ : (void)0)
#elif defined(HW_LOG)
u32 arm_insns; /* read by the heartbeat */
#define COUNT() arm_insns++
#else
#define COUNT()
#endif

/* Byte offsets of r0-r14 in the machine state, so a register is one indexed load */
static const signed char reg_off[16] = {
	SLOT(0) * 4, SLOT(1) * 4, SLOT(2) * 4, SLOT(3) * 4, SLOT(4) * 4, SLOT(5) * 4,
	SLOT(6) * 4, SLOT(7) * 4, SLOT(8) * 4, SLOT(9) * 4, SLOT(10) * 4, SLOT(11) * 4,
	SLOT(12) * 4, SLOT(13) * 4, SLOT(14) * 4, 0,
};
#define REG(n) (*(u32 *)((u8 *)C + reg_off[n]))

static inline int passes(u32 cond)
{
	switch (cond) {
	case 0: return F_Z;
	case 1: return !F_Z;
	case 2: return F_C;
	case 3: return !F_C;
	case 4: return F_N;
	case 5: return !F_N;
	case 6: return F_V;
	case 7: return !F_V;
	case 8: return F_C && !F_Z;
	case 9: return !F_C || F_Z;
	case 10: return F_N == F_V;
	case 11: return F_N != F_V;
	case 12: return !F_Z && F_N == F_V;
	case 13: return F_Z || F_N != F_V;
	case 14: return 1;
	}
	return 0;
}

/* r15 with the flags and mode, as mach.c's R15_FULL, but inline */
static inline u32 r15_full(u32 pc)
{
	return pc | (C->fn & 0x80000000u) | (C->fz ? 0 : 0x40000000u) |
	       (C->fc << 29) | (C->fv << 28) | C->psr;
}

/* Rm or Rs: r15 carries the flags and mode */
static inline u32 reg_psr(int n, u32 pc)
{
	return __builtin_expect(n == 15, 0) ? r15_full(pc & PC_MASK) : REG(n);
}

/* Rn: r15 is the bare PC */
static inline u32 reg_pc(int n, u32 pc)
{
	return __builtin_expect(n == 15, 0) ? pc & PC_MASK : REG(n);
}

/* The shifted register operand of data processing and of LDR and STR */
static inline u32 shifter_reg(u32 i, u32 pc, u32 *carry)
{
	int kind = (i >> 5) & 3;
	if (i & 0x10) {
		u32 m = reg_psr((int)(i & 15), pc + 12);
		u32 s = reg_psr((int)((i >> 8) & 15), pc + 12) & 255;
		return shift_c(kind, m, s, carry);
	}
	{
		u32 m = reg_psr((int)(i & 15), pc + 8), n = (i >> 7) & 31;
		switch (kind) {
		case 0:
			if (!n)
				return m;
			*carry = (m >> (32 - n)) & 1;
			return m << n;
		case 1:
			if (!n) {
				*carry = m >> 31;
				return 0;
			}
			*carry = (m >> (n - 1)) & 1;
			return m >> n;
		case 2:
			if (!n)
				n = 32;
			*carry = (m >> (n - 1)) & 1;
			return (u32)((s32)m >> (n > 31 ? 31 : n));
		}
		if (!n) {
			u32 v = (C->fc << 31) | (m >> 1);
			*carry = m & 1;
			return v;
		}
		*carry = (m >> (n - 1)) & 1;
		return (m >> n) | (m << (32 - n));
	}
}

/* An immediate shift when nothing needs its carry out, as in most code */
static inline u32 shift_imm(u32 i, u32 m)
{
	u32 n = (i >> 7) & 31;
	switch ((i >> 5) & 3) {
	case 0:
		return m << n;
	case 1:
		return n ? m >> n : 0;
	case 2:
		return (u32)((s32)m >> (n ? n : 31));
	}
	return n ? (m >> n) | (m << (32 - n)) : (C->fc << 31) | (m >> 1);
}

/* LDR and STR, one handler per register or immediate offset, byte or word, load
 * or store; pre or post indexing, direction and writeback are tested as they run */
#define LS(reg, byte, load) \
	ls_##reg##byte##load: { \
		u32 base, offset; \
		if (reg) { \
			if (i & 0x10) \
				return pc | EXIT_UNDEF; \
			offset = shift_imm(i, reg_psr((int)(i & 15), pc + 8)); \
		} else { \
			offset = i & 0xFFF; \
		} \
		rn = (i >> 16) & 15; \
		base = reg_pc(rn, pc + 8); \
		ea = i & (1u << 23) ? base + offset : base - offset; \
		addr = i & (1u << 24) ? ea : base; \
		C->cur = pc; \
		if (__builtin_expect((i & 0x01200000u) == 0x00200000u, 0)) \
			goto ls_user; \
		if (load) { \
			r = byte ? ld8i(addr) : ld32i(addr); \
			goto ls_loaded; \
		} \
		if (byte) { \
			st8i(addr, reg_psr((i >> 12) & 15, pc + 12)); \
		} else { \
			if (__builtin_expect((addr & ~3u) == pc + 4, 0) && (pc + 4) & PAGE_OFFSET) \
				goto ls_stale; \
			st32i(addr, reg_psr((i >> 12) & 15, pc + 12)); \
		} \
		goto ls_stored; \
	}

/* Jump table entries, as offsets from the first handler, which halves the table
 * and keeps it position independent */
#define H(label) (short)(&&label - &&dp_reg)
#define H4(l) H(l), H(l), H(l), H(l)
#define H16(l) H4(l), H4(l), H4(l), H4(l)
/* bits 23-20 of LDR and STR: U, B, W, L; P is bit 24, above these 16 */
#define LS16(reg) \
	H(ls_##reg##00), H(ls_##reg##01), H(ls_##reg##00), H(ls_##reg##01), \
	H(ls_##reg##10), H(ls_##reg##11), H(ls_##reg##10), H(ls_##reg##11), \
	H(ls_##reg##00), H(ls_##reg##01), H(ls_##reg##00), H(ls_##reg##01), \
	H(ls_##reg##10), H(ls_##reg##11), H(ls_##reg##10), H(ls_##reg##11)

/* Runs until control leaves. Returns the next PC, with EXIT bits saying how to go on.
 * code + pc addresses the instruction while pc - code_lo < code_span: a 32KB page
 * of RAM, or just the one word for ROM, whose pages can move. */
static u32 run(u32 pc)
{
	/* Every instruction by bits 27-20 */
	static const short top[256] = {
		H16(dp_reg), H16(dp_reg), H16(dp_imm), H16(dp_imm),
		LS16(0), LS16(0), LS16(1), LS16(1),
		H16(block), H16(block), H16(branch), H16(branch_link),
		H16(undef), H16(undef), H16(undef), H16(swi),
	};
	/* Data processing ops by opcode and S bit, once the operand is worked out */
	static const short dp_ops[32] = {
		H(and_), H(and_s), H(eor), H(eor_s), H(sub), H(sub_s), H(rsb), H(rsb_s),
		H(add), H(add_s), H(adc), H(adc_s), H(sbc), H(sbc_s), H(rsc), H(rsc_s),
		H(tst), H(tst_s), H(teq), H(teq_s), H(cmp), H(cmp_s), H(cmn), H(cmn_s),
		H(orr), H(orr_s), H(mov), H(mov_s), H(bic), H(bic_s), H(mvn), H(mvn_s),
	};
	uintptr_t code = 0;
	/* Set before use on every path, but GCC can't follow the computed jumps */
	u32 i, code_lo = 0, code_span = 0, a = 0, b = 0, r = 0, carry = 0, ci, ea = 0, addr = 0;
	int rd = 0, rn = 0;
	for (;;) {
		if (__builtin_expect(pc - code_lo >= code_span, 0)) {
			const u8 *page = C->rt[PAGE_INDEX(pc)];
			if (!page || pc >= ROM_BASE)
				page = mach_code_page(pc);
			if (!page)
				return pc; /* dispatch reports it */
			code = (uintptr_t)page - (pc & ~PAGE_OFFSET);
			if (pc >= ROM_BASE) {
				code_lo = pc;
				code_span = 4;
			} else {
				code_lo = pc & ~PAGE_OFFSET;
				code_span = PAGE_OFFSET + 1;
			}
		}
		i = *(const u32 *)(code + pc);
		COUNT();
decode:
		if (__builtin_expect(i >> 28 != 14, 0) && !passes(i >> 28)) {
			pc += 4;
			continue;
		}
		goto *(&&dp_reg + top[(i >> 20) & 0xFF]);

dp_reg: /* data processing with a register operand */
		rd = (i >> 12) & 15;
		carry = C->fc;
		if (LIKELY(!(i & 0xFF0))) {
			/* by far the most common register form */
			b = reg_psr((int)(i & 15), pc + 8);
			a = reg_pc((i >> 16) & 15, pc + 8);
		} else if (!(i & 0x10) && !(i & (1u << 20))) {
			/* no S, so nothing needs the shifter's carry */
			b = shift_imm(i, reg_psr((int)(i & 15), pc + 8));
			a = reg_pc((i >> 16) & 15, pc + 8);
		} else {
			if ((i & 0x90) == 0x90)
				goto mul_or_undef;
			b = shifter_reg(i, pc, &carry);
			a = reg_pc((i >> 16) & 15, pc + (i & 0x10 ? 12 : 8));
		}
		goto *(&&dp_reg + dp_ops[(i >> 20) & 31]);
dp_imm: { /* data processing with an immediate */
		u32 rot = (i >> 7) & 0x1E;
		rd = (i >> 12) & 15;
		b = i & 0xFF;
		carry = C->fc;
		if (rot) {
			b = (b >> rot) | (b << (32 - rot));
			carry = b >> 31;
		}
		a = reg_pc((i >> 16) & 15, pc + 8);
		goto *(&&dp_reg + dp_ops[(i >> 20) & 31]);
	}
		LS(0, 0, 0) LS(0, 0, 1) LS(0, 1, 0) LS(0, 1, 1)
		LS(1, 0, 0) LS(1, 0, 1) LS(1, 1, 0) LS(1, 1, 1)
block: { /* LDM and STM */
		u32 load = i & (1u << 20), wb = i & (1u << 21), sbit = i & (1u << 22);
		u32 up = i & (1u << 23), pre = i & (1u << 24), mask = i & 0xFFFF, base, a, wbv, size;
		int rn = (i >> 16) & 15, r, user_bank = sbit && !(load && (mask & 0x8000));
		u32 *p;
		size = (u32)__builtin_popcount(mask) * 4;
		if (!size)
			size = 4;
		base = reg_pc(rn, pc + 8);
		if (up) {
			a = pre ? base + 4 : base;
			wbv = base + size;
		} else {
			a = pre ? base - size : base - size + 4;
			wbv = base - size;
		}
		a &= ~3u;
		C->cur = pc;
		/* Most transfers sit in one page of RAM, so skip the per-word lookup */
		p = user_bank ? 0 : block_at(load ? C->rt : C->wt, a, size);
		if (load) {
			u32 m = mask & 0x7FFF;
			if (p) {
				for (r = 0; m; r++, m >>= 1)
					if (m & 1)
						REG(r) = *p++;
			} else {
				for (r = 0; m; r++, m >>= 1) {
					if (!(m & 1))
						continue;
					if (user_bank && r >= 8)
						usr_set(r, ld32i(a));
					else
						REG(r) = ld32i(a);
					a += 4;
				}
			}
			if (mask & 0x8000) {
				u32 npc = p ? *p : ld32i(a);
				if (wb && rn != 15 && !(mask >> rn & 1))
					REG(rn) = wbv;
				if (sbit)
					psr_write(npc);
				pc = npc & PC_MASK;
				goto jump;
			}
			if (wb && rn != 15 && (!(mask >> rn & 1) || (user_bank && rn >= 8)))
				REG(rn) = wbv;
		} else {
			int first = 1;
			u32 m = mask;
			for (r = 0; m; r++, m >>= 1) {
				u32 v;
				if (!(m & 1))
					continue;
				if (r == 15)
					v = r15_full((pc + 12) & PC_MASK);
				else if (user_bank && r >= 8)
					v = usr_get(r);
				else if (r == rn && !first && wb)
					v = wbv;
				else
					v = REG(r);
				if (p)
					*p++ = v;
				else
					st32i(a, v);
				a += 4;
				first = 0;
			}
			if (wb && rn != 15)
				REG(rn) = wbv;
		}
		pc += 4;
		continue;
	}
branch_link:
		REG(14) = r15_full((pc + 4) & PC_MASK);
branch:
		pc = (pc + 8 + (u32)((s32)(i << 8) >> 6)) & 0x3FFFFFFu;
		goto jump;
swi:
		return pc | EXIT_SWI;
undef:
		return pc | EXIT_UNDEF;
and_: r = a & b; goto dp_out;
eor: r = a ^ b; goto dp_out;
sub: r = a - b; goto dp_out;
rsb: r = b - a; goto dp_out;
add: r = a + b; goto dp_out;
adc: r = a + b + C->fc; goto dp_out;
sbc: r = a - b - !C->fc; goto dp_out;
rsc: r = b - a - !C->fc; goto dp_out;
orr: r = a | b; goto dp_out;
mov: r = b; goto dp_out;
bic: r = a & ~b; goto dp_out;
mvn: r = ~b; goto dp_out;
tst: r = a & b; goto test_out;
teq: r = a ^ b; goto test_out;
cmp: r = a - b; goto test_out;
cmn: r = a + b; goto test_out;
and_s: r = a & b; goto logic_s;
eor_s: r = a ^ b; goto logic_s;
orr_s: r = a | b; goto logic_s;
mov_s: r = b; goto logic_s;
bic_s: r = a & ~b; goto logic_s;
mvn_s: r = ~b; goto logic_s;
tst_s: r = a & b; goto test_logic_s;
teq_s: r = a ^ b; goto test_logic_s;
/* Arithmetic with S sets C and V, unless the result goes to r15 */
sub_s:
	r = a - b;
	if (rd != 15) {
		C->fc = a >= b;
		C->fv = ((a ^ b) & (a ^ r)) >> 31;
	}
	goto arith_s;
rsb_s:
	r = b - a;
	if (rd != 15) {
		C->fc = b >= a;
		C->fv = ((a ^ b) & (b ^ r)) >> 31;
	}
	goto arith_s;
add_s:
	r = a + b;
	if (rd != 15) {
		C->fc = r < a;
		C->fv = ((a ^ r) & (b ^ r)) >> 31;
	}
	goto arith_s;
adc_s:
	ci = C->fc;
	r = a + b + ci;
	if (rd != 15) {
		C->fc = ci ? r <= a : r < a;
		C->fv = ((a ^ r) & (b ^ r)) >> 31;
	}
	goto arith_s;
sbc_s:
	ci = C->fc;
	r = a - b - !ci;
	if (rd != 15) {
		C->fc = ci ? a >= b : a > b;
		C->fv = ((a ^ b) & (a ^ r)) >> 31;
	}
	goto arith_s;
rsc_s:
	ci = C->fc;
	r = b - a - !ci;
	if (rd != 15) {
		C->fc = ci ? b >= a : b > a;
		C->fv = ((a ^ b) & (b ^ r)) >> 31;
	}
	goto arith_s;
cmp_s:
	r = a - b;
	if (rd != 15) {
		C->fc = a >= b;
		C->fv = ((a ^ b) & (a ^ r)) >> 31;
	}
	goto test_arith_s;
cmn_s:
	r = a + b;
	if (rd != 15) {
		C->fc = r < a;
		C->fv = ((a ^ r) & (b ^ r)) >> 31;
	}
	goto test_arith_s;
logic_s:
	if (rd != 15)
		C->fc = carry;
arith_s:
	if (__builtin_expect(rd == 15, 0)) {
		psr_write(r);
		pc = r & PC_MASK;
		goto jump;
	}
	C->fn = C->fz = r;
	REG(rd) = r;
	pc += 4;
	continue;
dp_out:
	if (__builtin_expect(rd == 15, 0)) {
		pc = r & PC_MASK;
		goto jump;
	}
	REG(rd) = r;
	pc += 4;
	continue;
test_logic_s:
	if (rd != 15)
		C->fc = carry;
test_arith_s:
	if (rd != 15) {
		C->fn = C->fz = r;
		pc += 4;
		continue;
	}
test_out:
	pc += 4;
	if (rd == 15) { /* TEQP and friends */
		psr_write(r);
		goto jump;
	}
	continue;
/* The rest of LDR and STR, shared by their cases */
ls_user: /* post-indexed with W: the user mode forms */
	rd = (i >> 12) & 15;
	if (i & (1u << 20))
		r = i & (1u << 22) ? ld8T(addr, pc) : ld32T(addr, pc);
	else if (i & (1u << 22))
		st8T(addr, reg_psr(rd, pc + 12), pc);
	else
		st32T(addr, reg_psr(rd, pc + 12), pc);
	if (rn != 15)
		REG(rn) = ea;
	if (!(i & (1u << 20))) {
		pc += 4;
		continue;
	}
	goto ls_rd;
ls_stale: {
	/* The pipeline already holds the next instruction, and a store over it takes
	 * effect only on the next pass. Elite's decrypter relies on that, to catch
	 * anyone stepping through it. */
	u32 next = *(const u32 *)(code + pc + 4);
	st32i(addr, reg_psr((i >> 12) & 15, pc + 12));
	if ((i & 0x01200000u) != 0x01000000u && rn != 15)
		REG(rn) = ea;
	pc += 4;
	i = next;
	goto decode;
}
ls_stored:
	if ((i & 0x01200000u) != 0x01000000u && rn != 15)
		REG(rn) = ea;
	pc += 4;
	continue;
ls_loaded:
	if ((i & 0x01200000u) != 0x01000000u && rn != 15)
		REG(rn) = ea;
	rd = (i >> 12) & 15;
ls_rd:
	if (__builtin_expect(rd == 15, 0)) {
		pc = r & PC_MASK;
		goto jump;
	}
	REG(rd) = r;
	pc += 4;
	continue;
mul_or_undef:
		if ((i & 0x0FC000F0u) == 0x90) {
			u32 r = REG(i & 15) * REG((i >> 8) & 15);
			if (i & (1u << 21))
				r += REG((i >> 12) & 15);
			REG((i >> 16) & 15) = r;
			if (i & (1u << 20))
				C->fn = C->fz = r;
			pc += 4;
			continue;
		}
		return pc | EXIT_UNDEF;
jump:
		code_span = 0;
		if (LEAVES(pc))
			return pc;
		if (--C->budget < 0)
			return pc | EXIT_POLL;
	}
}

#ifdef HW_PERF
/* Profiling builds: CPU cycles spent in here, and the SH-4 event HW_PERF names
 * on the second counter, for the heartbeat */
#include <dc/perfctr.h>
u32 interp_cycles, interp_events;
#endif

void arm_interp(u32 pc)
{
#ifdef HW_PERF
	uint64_t c0 = perf_cntr_count(PRFC0), e0 = perf_cntr_count(PRFC1);
#endif
	u32 r = run(pc), next = r & 0x3FFFFFFu;
#ifdef HW_PERF
	interp_cycles += (u32)(perf_cntr_count(PRFC0) - c0);
	interp_events += (u32)(perf_cntr_count(PRFC1) - e0);
#endif
	switch (r >> 26) {
	case 1: TAIL(swi_insn(next));
	case 2: TAIL(undef_insn(next));
	case 3: TAIL(mach_poll(next));
	}
	TAIL(dispatch(next));
}
