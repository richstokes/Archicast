/* Per-instruction tests for src/interp.c: rarely used opcodes, shifter edge cases,
 * flags and the odd addressing forms that the replays may never reach. Each case
 * sets registers and flags, runs a few instructions from RAM at 0x8000 up to a
 * SWI, then checks registers, flags, mode and memory. Expected values follow the
 * ARM2 rules in the Acorn and ARM manuals, worked out by hand.
 *
 * make interp-test builds and runs it on the host. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/interp.c"

cpu_t cpu_state;

/* 64KB of RAM at address 0, mapped for reading and writing */
static u8 ram[0x10000] __attribute__((aligned(4)));
static u8 *rtab[2048], *wtab[2048];

#define CODE 0x8000u
#define SWI0 0xEF000000u

u32 ld32(u32 a)
{
	u8 *page = C->rt[PAGE_INDEX(a)];
	u32 v, s = (a & 3) * 8;
	if (!page)
		return 0;
	v = *(u32 *)(page + (a & 0x7FFCu));
	return s ? (v >> s) | (v << (32 - s)) : v; /* ARM2 rotates an unaligned word */
}
u32 ld8(u32 a) { u8 *page = C->rt[PAGE_INDEX(a)]; return page ? page[a & PAGE_OFFSET] : 0; }
void st32(u32 a, u32 v) { u8 *page = C->wt[PAGE_INDEX(a)]; if (page) *(u32 *)(page + (a & 0x7FFCu)) = v; }
void st8(u32 a, u32 v) { u8 *page = C->wt[PAGE_INDEX(a)]; if (page) page[a & PAGE_OFFSET] = (u8)v; }
u32 ld32T(u32 a, u32 pc) { (void)pc; return ld32(a); }
u32 ld8T(u32 a, u32 pc) { (void)pc; return ld8(a); }
void st32T(u32 a, u32 v, u32 pc) { (void)pc; st32(a, v); }
void st8T(u32 a, u32 v, u32 pc) { (void)pc; st8(a, v); }
const u8 *mach_code_page(u32 pc) { return rtab[PAGE_INDEX(pc)]; }

/* As mach.c, less the register bank switch, which no case here needs */
void psr_write(u32 v)
{
	C->fn = v;
	C->fz = v & 0x40000000u ? 0 : 1;
	C->fc = (v >> 29) & 1;
	C->fv = (v >> 28) & 1;
	if (C->psr & 3)
		C->psr = v & (PSR_I | PSR_F | 3);
	C->budget = 0;
}

u32 usr_get(int reg)
{
	int mode = C->psr & 3;
	if (mode == MODE_USR || (reg < 13 && mode != MODE_FIQ))
		return R(reg);
	return cpu_state.usr[reg - 8];
}

void usr_set(int reg, u32 value)
{
	int mode = C->psr & 3;
	if (mode == MODE_USR || (reg < 13 && mode != MODE_FIQ))
		R(reg) = value;
	else
		cpu_state.usr[reg - 8] = value;
}

/* arm_interp's exits, never reached as the cases call run() */
void swi_insn(u32 pc) { (void)pc; abort(); }
void undef_insn(u32 pc) { (void)pc; abort(); }
void mach_poll(u32 pc) { (void)pc; abort(); }
void dispatch(u32 pc) { (void)pc; abort(); }

typedef struct { int r; u32 v; } reg_t;   /* r is the register plus 1; 0 ends a list */
typedef struct { u32 addr, v; } word_t;   /* addr 0 ends a list */
typedef struct {
	const char *name;
	u32 code[4];           /* runs from 0x8000; a SWI follows, and fills the page */
	reg_t in[6];
	const char *flags_in;  /* NZCV, lower case for clear; null is all clear */
	word_t mem_in[3];
	reg_t out[6];
	const char *flags_out; /* null: not checked */
	word_t mem_out[3];
	u32 exit;              /* where it stops, with EXIT bits; 0 means at the SWI */
	int mode_out;          /* -1: not checked */
} test_t;

#define X(n, v) {(n) + 1, (v)}
#define AT(a, v) {(a), (v)}

static const test_t tests[] = {
	/* Arithmetic with carry in and out */
	{"ADCS wraps to zero with carry in", {0xE0B10002}, {X(1, 0xFFFFFFFF), X(2, 0)}, "nzCv",
	 {{0}}, {X(0, 0)}, "nZCv", {{0}}, 0, -1},
	{"ADCS carries when the sum wraps back to the first operand", {0xE0B10002},
	 {X(1, 5), X(2, 0xFFFFFFFF)}, "nzCv", {{0}}, {X(0, 5)}, "nzCv", {{0}}, 0, -1},
	{"SBCS of equal operands with C clear borrows", {0xE0D10002}, {X(1, 5), X(2, 5)}, "nzcv",
	 {{0}}, {X(0, 0xFFFFFFFF)}, "Nzcv", {{0}}, 0, -1},
	{"SBCS borrows when C is clear", {0xE0D10002}, {X(1, 5), X(2, 3)}, "nzcv",
	 {{0}}, {X(0, 1)}, "nzCv", {{0}}, 0, -1},
	{"RSCS with C set", {0xE0F10002}, {X(1, 3), X(2, 5)}, "nzCv",
	 {{0}}, {X(0, 2)}, "nzCv", {{0}}, 0, -1},
	{"SUBS signed overflow", {0xE0510002}, {X(1, 0x80000000), X(2, 1)}, "nzcv",
	 {{0}}, {X(0, 0x7FFFFFFF)}, "nzCV", {{0}}, 0, -1},
	{"CMN signed overflow", {0xE1710002}, {X(0, 0x55), X(1, 0x7FFFFFFF), X(2, 1)}, "nzcv",
	 {{0}}, {X(0, 0x55)}, "NzcV", {{0}}, 0, -1},
	{"RSB", {0xE0610002}, {X(1, 3), X(2, 10)}, "nzcv",
	 {{0}}, {X(0, 7)}, "nzcv", {{0}}, 0, -1},
	{"BICS leaves V alone", {0xE1D10002}, {X(1, 0xF0), X(2, 0xF0)}, "nzcV",
	 {{0}}, {X(0, 0)}, "nZcV", {{0}}, 0, -1},

	/* The shifter, and its carry out into logical ops */
	{"TEQ takes C from LSR #1", {0xE13100A2}, {X(1, 1), X(2, 0x80000001)}, "nzcV",
	 {{0}}, {X(1, 1)}, "nzCV", {{0}}, 0, -1},
	{"MOVS RRX", {0xE1B00062}, {X(2, 3)}, "nzCv",
	 {{0}}, {X(0, 0x80000001)}, "NzCv", {{0}}, 0, -1},
	{"MOV RRX without S", {0xE1A00062}, {X(2, 3)}, "nzCv",
	 {{0}}, {X(0, 0x80000001)}, "nzCv", {{0}}, 0, -1},
	{"LDR register offset RRX", {0xE7910062}, {X(1, 0x1000), X(2, 8)}, "nzcv",
	 {AT(0x1004, 0x1234)}, {X(0, 0x1234)}, NULL, {{0}}, 0, -1},
	{"MOV LSR #32 without S", {0xE1A00022}, {X(2, 0x80000000)}, "nzcv",
	 {{0}}, {X(0, 0)}, "nzcv", {{0}}, 0, -1},
	{"MOV ASR #32 without S", {0xE1A00042}, {X(2, 0x80000000)}, "nzcv",
	 {{0}}, {X(0, 0xFFFFFFFF)}, "nzcv", {{0}}, 0, -1},
	{"MOVS LSR #32", {0xE1B00022}, {X(2, 0x80000000)}, "nzcv",
	 {{0}}, {X(0, 0)}, "nZCv", {{0}}, 0, -1},
	{"MOVS ASR #32", {0xE1B00042}, {X(2, 0x80000000)}, "nzcv",
	 {{0}}, {X(0, 0xFFFFFFFF)}, "NzCv", {{0}}, 0, -1},
	{"MOV ROR #4, no flags", {0xE1A00262}, {X(2, 0x12345678)}, "nzcv",
	 {{0}}, {X(0, 0x81234567)}, "nzcv", {{0}}, 0, -1},
	{"MOVS LSL by register 32", {0xE1B00312}, {X(2, 1), X(3, 32)}, "nzcv",
	 {{0}}, {X(0, 0)}, "nZCv", {{0}}, 0, -1},
	{"MOVS LSR by register 33", {0xE1B00332}, {X(2, 0x80000000), X(3, 33)}, "nzCv",
	 {{0}}, {X(0, 0)}, "nZcv", {{0}}, 0, -1},
	{"MOVS ROR by register 32", {0xE1B00372}, {X(2, 0x80000001), X(3, 32)}, "nzcv",
	 {{0}}, {X(0, 0x80000001)}, "NzCv", {{0}}, 0, -1},
	{"MOVS by register 0 keeps C", {0xE1B00312}, {X(2, 5), X(3, 0x100)}, "nzCv",
	 {{0}}, {X(0, 5)}, "nzCv", {{0}}, 0, -1},
	{"MOVS immediate with rotation sets C", {0xE3B004FF}, {{0}}, "nzcv",
	 {{0}}, {X(0, 0xFF000000)}, "NzCv", {{0}}, 0, -1},

	/* r15 as an operand */
	{"ADD from pc, register shift reads pc+12", {0xE08F0211}, {{0}}, "nzcv",
	 {{0}}, {X(0, CODE + 12)}, NULL, {{0}}, 0, -1},
	{"ADD from pc, immediate reads pc+8", {0xE28F0000}, {{0}}, "nzcv",
	 {{0}}, {X(0, CODE + 8)}, NULL, {{0}}, 0, -1},
	{"MOV from pc carries flags and mode", {0xE1A0000F}, {{0}}, "Nzcv",
	 {{0}}, {X(0, 0x80000000 | PSR_I | 3 | (CODE + 8))}, NULL, {{0}}, 0, -1},

	/* Multiply */
	{"MLAS truncates and keeps C", {0xE0303291}, {X(1, 0x10000), X(2, 0x10000), X(3, 5)}, "nzCv",
	 {{0}}, {X(0, 5)}, "nzCv", {{0}}, 0, -1},
	{"MULS to zero", {0xE0100291}, {X(1, 0x10000), X(2, 0x10000)}, "nzcv",
	 {{0}}, {X(0, 0)}, "nZcv", {{0}}, 0, -1},
	{"SWP is undefined on an ARM2", {0xE1010092}, {{0}}, NULL,
	 {{0}}, {{0}}, NULL, {{0}}, CODE | EXIT_UNDEF, -1},
	{"coprocessor op is undefined", {0xEE000010}, {{0}}, NULL,
	 {{0}}, {{0}}, NULL, {{0}}, CODE | EXIT_UNDEF, -1},

	/* LDR and STR */
	{"LDR unaligned rotates", {0xE5910000}, {X(1, 0x1001)}, NULL,
	 {AT(0x1000, 0x44332211)}, {X(0, 0x11443322)}, NULL, {{0}}, 0, -1},
	{"LDRB post-indexed", {0xE4D10001}, {X(1, 0x1002)}, NULL,
	 {AT(0x1000, 0x44332211)}, {X(0, 0x33), X(1, 0x1003)}, NULL, {{0}}, 0, -1},
	{"STRB pre-indexed down with writeback", {0xE5610001}, {X(0, 0x1AB), X(1, 0x1004)}, NULL,
	 {AT(0x1000, 0x44332211)}, {X(1, 0x1003)}, NULL, {AT(0x1000, 0xAB332211)}, 0, -1},
	{"LDR register offset ASR #3", {0xE79101C2}, {X(1, 0x1000), X(2, 0x20)}, NULL,
	 {AT(0x1004, 0x55667788)}, {X(0, 0x55667788), X(1, 0x1000)}, NULL, {{0}}, 0, -1},
	{"LDRT post-indexed", {0xE4B10004}, {X(1, 0x1000)}, NULL,
	 {AT(0x1000, 0xCAFE)}, {X(0, 0xCAFE), X(1, 0x1004)}, NULL, {{0}}, 0, -1},
	{"LDR into pc jumps", {0xE591F000}, {X(1, 0x1000)}, NULL,
	 {AT(0x1000, 0x8100)}, {{0}}, NULL, {{0}}, 0x8100 | EXIT_SWI, -1},
	{"STR pc stores pc+12 with flags", {0xE581F000}, {X(1, 0x1000)}, "nzcv",
	 {{0}}, {{0}}, NULL, {AT(0x1000, PSR_I | 3 | (CODE + 12))}, 0, -1},
	{"LDR pc-relative", {0xE59F0000}, {{0}}, NULL,
	 {{0}}, {X(0, SWI0)}, NULL, {{0}}, 0, -1},
	{"STR over the next instruction runs the old one", {0xE50F0004, 0xE3A02001},
	 {X(0, 0xE3A02002)}, NULL, {{0}}, {X(2, 1)}, NULL, {AT(CODE + 4, 0xE3A02002)}, 0, -1},

	/* LDM and STM */
	{"STMIA with writeback, base second", {0xE8A10003}, {X(0, 0xA), X(1, 0x1010)}, NULL,
	 {{0}}, {X(1, 0x1018)}, NULL, {AT(0x1010, 0xA), AT(0x1014, 0x1018)}, 0, -1},
	{"STMDB with writeback, base first", {0xE9210006}, {X(1, 0x1020), X(2, 7)}, NULL,
	 {{0}}, {X(1, 0x1018)}, NULL, {AT(0x1018, 0x1020), AT(0x101C, 7)}, 0, -1},
	{"LDMIA base in the list wins over writeback", {0xE8B10003}, {X(1, 0x1000)}, NULL,
	 {AT(0x1000, 0x11), AT(0x1004, 0x22)}, {X(0, 0x11), X(1, 0x22)}, NULL, {{0}}, 0, -1},
	{"LDMDB with pc and ^ restores flags and mode", {0xE9518001}, {X(1, 0x1000)}, "nzcv",
	 {AT(0xFF8, 0x99), AT(0xFFC, 0xF0008200)}, {X(0, 0x99), X(1, 0x1000)}, "NZCV", {{0}},
	 0x8200 | EXIT_SWI, MODE_USR},
	{"STM pc stores pc+12 with flags", {0xE8818000}, {X(1, 0x1000)}, "nZcv",
	 {{0}}, {{0}}, NULL, {AT(0x1000, 0x40000000 | PSR_I | 3 | (CODE + 12))}, 0, -1},
	{"STM ^ stores user r13", {0xE8C12100}, {X(1, 0x1000), X(8, 0x88)}, NULL,
	 {{0}}, {{0}}, NULL, {AT(0x1000, 0x88), AT(0x1004, 0xAAAA0005)}, 0, -1},

	/* Changing the PSR */
	{"TEQP sets flags and mode", {0xE331F000}, {X(1, 0xF0000002)}, "nzcv",
	 {{0}}, {{0}}, "NZCV", {{0}}, 0, MODE_IRQ},
	{"MOVS pc, lr returns with the PSR", {0xE1B0F00E}, {X(14, 0x30008304)}, "Nzcv",
	 {{0}}, {{0}}, "nzCV", {{0}}, 0x8304 | EXIT_SWI, MODE_USR},

	/* Branches and conditions */
	{"BL sets lr with flags and mode", {0xEB000002}, {{0}}, "nzcv",
	 {{0}}, {X(14, PSR_I | 3 | (CODE + 4))}, NULL, {{0}}, (CODE + 16) | EXIT_SWI, -1},
	{"B backwards", {0xE1A00000, 0xEAFFFFFC}, {{0}}, NULL,
	 {{0}}, {{0}}, NULL, {{0}}, (CODE - 4) | EXIT_SWI, -1},
	{"SWINE skipped when Z set", {0x1F000000, 0xE3A00007}, {{0}}, "nZcv",
	 {{0}}, {X(0, 7)}, NULL, {{0}}, 0, -1},
};

static int flag(const char *f, char c) { return f && strchr(f, c) != NULL; }

static void set_flags_str(const char *f)
{
	C->fn = flag(f, 'N') ? 0x80000000u : 0;
	C->fz = flag(f, 'Z') ? 0 : 1;
	C->fc = flag(f, 'C');
	C->fv = flag(f, 'V');
}

static void get_flags_str(char *out)
{
	out[0] = (s32)C->fn < 0 ? 'N' : 'n';
	out[1] = C->fz == 0 ? 'Z' : 'z';
	out[2] = C->fc ? 'C' : 'c';
	out[3] = C->fv ? 'V' : 'v';
	out[4] = 0;
}

static void reset(void)
{
	u32 a;
	memset(&cpu_state, 0, sizeof(cpu_state));
	memset(ram, 0, sizeof(ram));
	for (a = 0x7F00; a < 0x10000; a += 4)
		*(u32 *)(ram + a) = SWI0; /* stray jumps stop at once */
	for (a = 0; a < 8; a++)
		cpu_state.usr[a < 7 ? a : 6] = 0xAAAA0000u + a;
	C->rt = rtab;
	C->wt = wtab;
	C->psr = PSR_I | MODE_SVC;
}

/* Runs to the first SWI or undefined instruction, through polls as mach_poll would */
static u32 go(u32 pc)
{
	for (;;) {
		u32 r;
		C->budget = 1000;
		r = run(pc);
		if (r >> 26 != 3)
			return r;
		pc = r & 0x3FFFFFFu;
	}
}

static int ref_cond(int c, int n, int z, int cf, int v)
{
	int t;
	switch (c >> 1) {
	case 0: t = z; break;
	case 1: t = cf; break;
	case 2: t = n; break;
	case 3: t = v; break;
	case 4: t = cf && !z; break;
	case 5: t = n == v; break;
	case 6: t = !z && n == v; break;
	default: return c == 14; /* AL; NV never runs on an ARM2 */
	}
	return c & 1 ? !t : t;
}

int main(void)
{
	int failed = 0, n = 0, c, f;
	unsigned k;
	rtab[0] = wtab[0] = ram;
	rtab[1] = wtab[1] = ram + 0x8000;
	for (k = 0; k < sizeof(tests) / sizeof(tests[0]); k++) {
		const test_t *t = &tests[k];
		u32 r, want, insns = 0;
		int j, bad = 0;
		char flags[5];
		reset();
		for (j = 0; j < 4 && t->code[j]; j++, insns++)
			*(u32 *)(ram + CODE + 4 * j) = t->code[j];
		for (j = 0; j < 6 && t->in[j].r; j++)
			R(t->in[j].r - 1) = t->in[j].v;
		for (j = 0; j < 3 && t->mem_in[j].addr; j++)
			*(u32 *)(ram + t->mem_in[j].addr) = t->mem_in[j].v;
		set_flags_str(t->flags_in);
		r = go(CODE);
		want = t->exit ? t->exit : (CODE + 4 * insns) | EXIT_SWI;
		if (r != want) {
			printf("FAIL %s: stopped at %07X kind %u, expected %07X kind %u\n", t->name,
				r & 0x3FFFFFF, r >> 26, want & 0x3FFFFFF, want >> 26);
			bad = 1;
		}
		for (j = 0; j < 6 && t->out[j].r; j++)
			if (R(t->out[j].r - 1) != t->out[j].v) {
				printf("FAIL %s: r%d=%08X, expected %08X\n", t->name, t->out[j].r - 1,
					R(t->out[j].r - 1), t->out[j].v);
				bad = 1;
			}
		get_flags_str(flags);
		if (t->flags_out && strcmp(flags, t->flags_out)) {
			printf("FAIL %s: flags %s, expected %s\n", t->name, flags, t->flags_out);
			bad = 1;
		}
		if (t->mode_out >= 0 && (int)(C->psr & 3) != t->mode_out) {
			printf("FAIL %s: mode %u, expected %d\n", t->name, C->psr & 3, t->mode_out);
			bad = 1;
		}
		for (j = 0; j < 3 && t->mem_out[j].addr; j++)
			if (*(u32 *)(ram + t->mem_out[j].addr) != t->mem_out[j].v) {
				printf("FAIL %s: [%04X]=%08X, expected %08X\n", t->name, t->mem_out[j].addr,
					*(u32 *)(ram + t->mem_out[j].addr), t->mem_out[j].v);
				bad = 1;
			}
		failed += bad;
		n++;
	}
	/* Every condition code against every combination of flags */
	for (c = 0; c < 16; c++)
		for (f = 0; f < 16; f++) {
			int nf = f >> 3 & 1, zf = f >> 2 & 1, cf = f >> 1 & 1, vf = f & 1, ran;
			reset();
			*(u32 *)(ram + CODE) = ((u32)c << 28) | 0x03A00001u; /* MOVcc r0, #1 */
			C->fn = nf ? 0x80000000u : 0;
			C->fz = !zf;
			C->fc = (u32)cf;
			C->fv = (u32)vf;
			go(CODE);
			ran = R(0) == 1;
			if (ran != ref_cond(c, nf, zf, cf, vf)) {
				printf("FAIL condition %X with NZCV=%d%d%d%d: %s\n", c, nf, zf, cf, vf,
					ran ? "ran" : "skipped");
				failed++;
			}
			n++;
		}
	printf("%d of %d interpreter cases passed\n", n - failed, n);
	return failed != 0;
}
