/* Development harness: runs the recompiled ROM on the Mac with deterministic
 * time and writes PPM screenshots. Not part of the Dreamcast build. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../cpu.h"

static uint64_t virtual_us;
static u32 us_step = 40, us_jitter, jitter = 12345;
static uint64_t stop_us;
static const char *shot_path;
static int frames;
static screen_t last;

uint64_t plat_time_us(void)
{
		jitter = jitter * 1103515245u + 12345u;
	return virtual_us += us_step + (jitter >> 16) % (us_jitter + 1);
}

/* The clock the chip in assets/cmos.bin was stopped at, 09 Nov 1992 14:16:19,
 * so replays stay deterministic. DREAM_RTC gives another, in Unix seconds. */
int64_t plat_rtc_secs(void)
{
	return getenv("DREAM_RTC") ? atoll(getenv("DREAM_RTC")) : 721318579;
}

void plat_rtc_set(int64_t secs)
{
	fprintf(stderr, "rtc set to %lld\n", (long long)secs);
}

void plat_log(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static void write_shot(void);

void plat_fatal(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fprintf(stderr, "virtual time %.2fs, frames %d\n", virtual_us / 1e6, frames);
	for (int i = 0; i < 15; i++)
		fprintf(stderr, "r%-2d %08X%s", i, R(i), i % 4 == 3 ? "\n" : "  ");
	fprintf(stderr, "\n");
	write_shot();
	{ void mach_dump(void); mach_dump(); }
	{
		extern u32 mach_miss_pc;
		u32 mach_peek(u32 addr);
		FILE *f = fopen("build/miss.txt", "w");
		for (u32 a = mach_miss_pc > 0x800 ? (mach_miss_pc & ~3u) - 0x800 : 0; f && mach_miss_pc && a < mach_miss_pc + 0x800; a += 4)
			fprintf(f, "%08X\n", mach_peek(a));
		if (f)
			fclose(f);
	}
	exit(1);
}

static u32 rgb(u32 entry)
{
	u32 r = (entry & 15) * 17, g = ((entry >> 4) & 15) * 17, b = ((entry >> 8) & 15) * 17;
	return r | (g << 8) | (b << 16);
}

static void write_shot(void)
{
	const screen_t *s = &last;
	FILE *f;
	if (!shot_path || s->width <= 0 || s->height <= 0)
		return;
	f = fopen(shot_path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", s->width, s->height);
	u32 addr = s->start, line = (u32)(s->width * s->bpp / 8);
	for (int y = 0; y < s->height; y++) {
		for (int x = 0; x < s->width; x++) {
			u32 bit = (u32)x * (u32)s->bpp, a = addr + bit / 8;
			if (a >= s->end)
				a = s->base + (a - s->end);
			u32 v = (s->ram[a] >> (bit & 7)) & ((1u << s->bpp) - 1);
			u32 e = (s->split_line >= 0 && y >= s->split_line ? s->palette2 : s->palette)[v & 15];
			if (s->bpp == 8)
				e = (e & 0x737) | ((v & 0x10) >> 1) | ((v & 0x60) << 1) | ((v & 0x80) << 4);
			u32 c = rgb(e);
			int cx = x - s->cursor_x, cy = y - s->cursor_y;
			if (cx >= 0 && cx < 32 && cy >= 0 && cy < s->cursor_h) {
				u32 p = (s->ram[s->cursor_addr + (u32)cy * 8 + (u32)(cx >> 2)] >> ((cx & 3) * 2)) & 3;
				if (p)
					c = rgb(s->cursor_palette[p - 1]);
			}
			putc(c & 255, f);
			putc((c >> 8) & 255, f);
			putc((c >> 16) & 255, f);
		}
		addr += line;
		if (addr >= s->end)
			addr = s->base + (addr - s->end);
	}
	fclose(f);
}

u32 plat_frame(const screen_t *screen)
{
	last = *screen;
	frames++;
	if (virtual_us >= stop_us) {
		write_shot();
		mach_stop();
	}
	/* Virtual time: presenting is instant, unless DREAM_PRESENT_US asks for it to
	 * take that long, as it does on a console, where the machine stands still */
	{
		static int cost = -1;
		if (cost < 0)
			cost = getenv("DREAM_PRESENT_US") ? atoi(getenv("DREAM_PRESENT_US")) : 0;
		u32 took = (u32)cost;
		if (cost && getenv("DREAM_PRESENT_RAND")) {
			jitter = jitter * 1103515245u + 12345u;
			took = (jitter >> 8) % (u32)cost;
		}
		virtual_us += took;
		return took;
	}
}

/* DREAM_WAV records what the platform would play, at the last rate in use */
static FILE *wav;
static u32 wav_frames, wav_rate;

static void wav_header(void)
{
	u32 h[11] = {0x46464952, 36 + wav_frames * 4, 0x45564157, 0x20746D66, 16, 0x00020001,
		wav_rate, wav_rate * 4, 0x00100004, 0x61746164, wav_frames * 4};
	fseek(wav, 0, SEEK_SET);
	fwrite(h, 4, 11, wav);
}

void plat_sound(const s16 *stereo, int frames, int rate)
{
	const char *path = getenv("DREAM_WAV");
	if (!wav && path)
		wav = fopen(path, "wb");
	if (!wav)
		return;
	if (!wav_rate)
		fwrite(&wav_frames, 4, 11, wav); /* room for the header, written at the end */
	wav_rate = (u32)rate;
	fwrite(stereo, 4, (size_t)frames, wav);
	wav_frames += (u32)frames;
}

/* Scripted input: "<seconds> move <dx> <dy>", "<seconds> button <mask>",
 * "<seconds> key <row> <col> <0|1>", "<seconds> shot <path>",
 * "<seconds> type <text>" (\\n for Return). */
static FILE *replay;
static double next_time = -1;
static char next_verb[32], next_arg[256];
static int pending_dx, pending_dy, button_mask;

/* Unshifted key positions by ASCII, and shifted ones below */
static const struct { char c; u8 row, col, shift; } key_map[] = {
	{'a',3,12,0},{'b',5,2,0},{'c',5,0,0},{'d',3,14,0},{'e',2,9,0},{'f',3,15,0},{'g',4,0,0},
	{'h',4,1,0},{'i',2,14,0},{'j',4,2,0},{'k',4,3,0},{'l',4,4,0},{'m',5,4,0},{'n',5,3,0},
	{'o',2,15,0},{'p',3,0,0},{'q',2,7,0},{'r',2,10,0},{'s',3,13,0},{'t',2,11,0},{'u',2,13,0},
	{'v',5,1,0},{'w',2,8,0},{'x',4,15,0},{'y',2,12,0},{'z',4,14,0},{'0',1,10,0},{'1',1,1,0},
	{'2',1,2,0},{'3',1,3,0},{'4',1,4,0},{'5',1,5,0},{'6',1,6,0},{'7',1,7,0},{'8',1,8,0},
	{'9',1,9,0},{' ',5,15,0},{'.',5,6,0},{'-',1,11,0},{'/',5,7,0},{',',5,5,0},{'\n',4,7,0},
	{':',4,5,1},{'$',1,4,1},{'*',1,8,1},{'"',4,6,1},{'_',1,11,1},{'!',1,1,1},{'=',1,12,0},
	{'(',1,9,1},{')',1,10,1},{'&',1,7,1},{'[',3,1,0},{']',3,2,0},
	{'<',5,5,1},{'>',5,6,1},{'+',1,12,1},{';',4,5,0},{'?',5,7,1},{'%',1,5,1},
};
static char typing[256];
static int type_pos, type_phase;

static void type_step(void)
{
	char c = typing[type_pos];
	int upper = c >= 'A' && c <= 'Z';
	unsigned i;
	if (!c)
		return;
	if (upper)
		c = (char)(c - 'A' + 'a');
	for (i = 0; i < sizeof(key_map) / sizeof(key_map[0]); i++) {
		if (key_map[i].c != c)
			continue;
		int shift = upper || key_map[i].shift;
		switch (type_phase) {
		case 0: if (shift) mach_key(4, 12, 1); break;
		case 1: mach_key(key_map[i].row, key_map[i].col, 1); break;
		case 2: break;
		case 3: mach_key(key_map[i].row, key_map[i].col, 0); break;
		case 4: if (shift) mach_key(4, 12, 0); break;
		}
		break;
	}
	if (++type_phase == 8) {
		type_phase = 0;
		type_pos++;
	}
}

static void replay_read(void)
{
	char line[320];
	next_time = -1;
	while (replay && fgets(line, sizeof(line), replay)) {
		next_arg[0] = 0;
		if (line[0] != '#' && sscanf(line, "%lf %31s %255[^\n]", &next_time, next_verb, next_arg) >= 2)
			return;
		next_time = -1;
	}
}

void plat_poll_input(void)
{
	int dx, dy;
	while (next_time >= 0 && virtual_us / 1e6 >= next_time) {
		int a = 0, b = 0, c = 0;
		sscanf(next_arg, "%d %d %d", &a, &b, &c);
		if (!strcmp(next_verb, "move")) {
			pending_dx += a;
			pending_dy += b;
		} else if (!strcmp(next_verb, "button")) {
			button_mask = a;
		} else if (!strcmp(next_verb, "type")) {
			int o = 0;
			for (char *t = next_arg; *t && o < 255; t++, o++)
				typing[o] = (t[0] == '\\' && t[1] == 'n') ? (t++, '\n') : t[0];
			typing[o] = 0;
			type_pos = type_phase = 0;
		} else if (!strcmp(next_verb, "key")) {
			mach_key(a, b, c);
		} else if (!strcmp(next_verb, "shot")) {
			const char *saved = shot_path;
			shot_path = next_arg;
			write_shot();
			shot_path = saved;
			fprintf(stderr, "sum=%08X ", mach_screen_sum(&last));
			fprintf(stderr, "shot %s at %.2fs cursor %d,%d h=%d addr=%X\n", next_arg, virtual_us / 1e6,
				last.cursor_x, last.cursor_y, last.cursor_h, last.cursor_addr);
		}
		replay_read();
	}
	type_step();
	dx = pending_dx > 16 ? 16 : pending_dx < -16 ? -16 : pending_dx;
	dy = pending_dy > 16 ? 16 : pending_dy < -16 ? -16 : pending_dy;
	pending_dx -= dx;
	pending_dy -= dy;
	mach_mouse(dx, dy, button_mask);
}

static u8 rom[0x400000];
static u32 pages_touched[4];
static u32 page_loads[128];

void plat_rom_read(u32 page, u8 *dest)
{
	memcpy(dest, rom + page * 0x8000, 0x8000);
	pages_touched[page >> 5] |= 1u << (page & 31);
	page_loads[page]++;
}

static FILE *disc_file;

int plat_disc_open(void)
{
	const char *path = getenv("DREAM_ADF");
	if (!path)
		return 0;
	disc_file = fopen(path, "rb"); /* missing means a blank disc */
	return 1;
}

int plat_disc_read(u8 *sector)
{
	return disc_file && fread(sector, 1, 1024, disc_file) == 1024;
}

void plat_disc_close(void)
{
	if (disc_file)
		fclose(disc_file);
	disc_file = NULL;
}

static void disc_save(void)
{
	const char *path = getenv("DREAM_ADF_OUT");
	FILE *f = path && fdc_present() ? fopen(path, "wb") : NULL;
	if (f) {
		static u8 sector[1024];
		for (int i = 0; i < 80 * 2 * 5; i++) {
			fdc_copy_sector(i, sector);
			fwrite(sector, 1, sizeof(sector), f);
		}
		fclose(f);
	}
}

static void cmos_save(void)
{
	const char *path = getenv("DREAM_CMOS_OUT");
	FILE *f = path ? fopen(path, "wb") : NULL;
	if (f) {
		fwrite(mach_cmos(), 1, 256, f);
		fclose(f);
	}
}

/* A sparse file (DREAM_HD) stands in for the SD card, and a read-only image
 * (DREAM_HD_RO) for the archive streamed off the CD. The card is unit 0 when
 * both are given, as on the console. */
static FILE *hd_file[HD_UNITS];
static u32 hd_sectors[HD_UNITS];
static int hd_ro[HD_UNITS];
static int hd_opened;

static void hd_open(void)
{
	const char *rw = getenv("DREAM_HD"), *ro = getenv("DREAM_HD_RO");
	int unit = 0;
	hd_opened = 1;
	if (rw) {
		FILE *f = fopen(rw, "r+b");
		if (!f) {
			f = fopen(rw, "w+b");
			fseek(f, 8LL * 1000 * 1000 * 1000 - 1, SEEK_SET);
			fputc(0, f);
		}
		hd_file[unit++] = f;
	}
	if (ro) {
		hd_file[unit] = fopen(ro, "rb");
		if (!hd_file[unit])
			plat_fatal("cannot open %s\n", ro);
		hd_ro[unit++] = 1;
	}
	while (unit--) {
		long long bytes;
		fseek(hd_file[unit], 0, SEEK_END);
		bytes = ftell(hd_file[unit]);
		hd_sectors[unit] = bytes / 512 > 0xF000000 ? 0xF000000 : (u32)(bytes / 512);
	}
}

u32 plat_hd_sectors(int unit)
{
	if (!hd_opened)
		hd_open();
	return hd_sectors[unit];
}

int plat_hd_writable(int unit)
{
	if (!hd_opened)
		hd_open();
	return hd_file[unit] && !hd_ro[unit];
}

int plat_hd_read(int unit, u32 lba, u8 *dest)
{
	fseek(hd_file[unit], (long long)lba * 512, SEEK_SET);
	return fread(dest, 1, 512, hd_file[unit]) == 512 ? 0 : -1;
}

int plat_hd_write(int unit, u32 lba, const u8 *src)
{
	if (hd_ro[unit])
		return -1;
	fseek(hd_file[unit], (long long)lba * 512, SEEK_SET);
	return fwrite(src, 1, 512, hd_file[unit]) == 512 ? 0 : -1;
}

int plat_hd_prepare(void)
{
	int state;
	if (!plat_hd_writable(0))
		return 1;
	state = hd_is_formatted();
	if (state)
		return state > 0;
	fprintf(stderr, "formatting hard disc\n");
	return !hd_format(plat_hd_sectors(0));
}

const ov_header_t *plat_overlay_load(int unit)
{
	extern const ov_header_t *const ov_headers[];
	fprintf(stderr, "overlay %d loaded at %.2fs\n", unit, virtual_us / 1e6);
	return ov_headers[unit];
}

static u8 *read_file(const char *path, size_t want, u8 *dest)
{
	FILE *f = fopen(path, "rb");
	if (!f || (fread(dest, 1, want, f) != want && want != sizeof(rom))) {
		fprintf(stderr, "cannot read %s\n", path);
		exit(1);
	}
	fclose(f);
	return dest;
}

int main(int argc, char **argv)
{
	static u8 cmos[256];
	if (argc < 4) {
		fprintf(stderr, "usage: %s rom.bin cmos.bin seconds [shot.ppm]\n", argv[0]);
		return 2;
	}
	read_file(argv[1], sizeof(rom), rom);
	read_file(argv[2], 256, cmos);
	stop_us = (uint64_t)(atof(argv[3]) * 1e6);
	shot_path = argc > 4 ? argv[4] : NULL;
	if (getenv("DREAM_US_STEP"))
		us_step = (u32)atoi(getenv("DREAM_US_STEP"));
	if (getenv("DREAM_SEED"))
		jitter = (u32)atoi(getenv("DREAM_SEED"));
	if (getenv("DREAM_US_JITTER"))
		us_jitter = (u32)atoi(getenv("DREAM_US_JITTER"));
	if (getenv("DREAM_REPLAY")) {
		replay = fopen(getenv("DREAM_REPLAY"), "r");
		replay_read();
	}
	mach_init(cmos);
	mach_run(0);
	{
		extern u32 mach_pc_ring[256];
		void mach_dump(void);
		mach_dump();
		for (int i = 0; i < 256; i += 16)
			fprintf(stderr, "%07X ", mach_pc_ring[i]);
		fprintf(stderr, "\nregs:");
		for (int i = 0; i < 15; i++)
			fprintf(stderr, " %08X", R(i));
		fprintf(stderr, "\n");
	}
	{
		u32 mach_peek(u32 addr);
		for (u32 v = 0; v < 0x20; v += 4)
			fprintf(stderr, "vector %02X %08X\n", v, mach_peek(v));
	}
	disc_save();
	cmos_save();
	if (wav) {
		wav_header();
		fclose(wav);
	}
	if (hd_file[0])
		fflush(hd_file[0]);
	fprintf(stderr, "overlay loads %u\n", mach_overlay_loads());
	fprintf(stderr, "rom misses %u, pages touched %d, pack pages touched %d\n", mach_rom_misses(),
		__builtin_popcount(pages_touched[0]) + __builtin_popcount(pages_touched[1]),
		__builtin_popcount(pages_touched[2]) + __builtin_popcount(pages_touched[3]));
	fprintf(stderr, "page loads:");
	for (int i = 0; i < 72; i++)
		fprintf(stderr, " %u", page_loads[i]);
	fprintf(stderr, "\n");
	fprintf(stderr, "screen sum %08X\n", mach_screen_sum(&last));
	fprintf(stderr, "stopped at %.2fs virtual, %d frames, %dx%d %dbpp\n", virtual_us / 1e6,
		frames, last.width, last.height, last.bpp);
	return 0;
}
