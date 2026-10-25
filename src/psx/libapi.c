#if defined(_WIN32)
/* This file implements the PSX open/read/write/lseek/close. Newer mingw-w64
 * headers reach <io.h> through <wchar.h> (via SDL), whose legacy POSIX
 * aliases then conflict with them; NO_OLDNAMES withholds those aliases. */
#define NO_OLDNAMES
#endif
#include "psx/libapi.h"
#include "psx/libmcrd.h"
#include "psx/kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
/* Forward-declare _mkdir directly instead of including <direct.h>.
 * <direct.h> drags in <io.h> which prototypes open / lseek / read /
 * write with stdio signatures and conflicts with this file's own
 * PSX-shim implementations of those names. */
int _mkdir(const char* dirname);
#else
#include <sys/stat.h>
#endif
#include "../PsyX_main.h"

/* This TU is compiled with -fvisibility=hidden on ELF so its libc-colliding
 * names (open/read/write/close/lseek/ioctl/rename) don't interpose libc for
 * other loaded modules (see PsyCross/CMakeLists.txt). SetSp is part of the PSX
 * API that map overlay .so's call, and on Linux those overlays now import the
 * single PsyCross instance from the host exe instead of linking their own copy,
 * so SetSp must stay default-visibility to be exported via -rdynamic. */
#if defined(__GNUC__) && !defined(_WIN32)
#  define PSX_API_EXPORT __attribute__((visibility("default")))
#else
#  define PSX_API_EXPORT
#endif

long sp = 0;

int dword_300[] = { 0x20, 0xD,  0x0,  0x0 };
int dword_308[] = { 0x10, 0x20, 0x40, 0x1 };

/* ---- Simple PSX Event System ---- */
#define MAX_EVENTS 16

typedef struct {
    int used;
    int enabled;
    int pending;            /* set by DeliverEvent, consumed by TestEvent */
    unsigned int ev_class;  /* e.g. RCntCNT2, SwCARD, HwCARD */
    int ev_spec;
    int ev_mode;
    long (*callback)(void);
} PsxEvent;

static PsxEvent g_events[MAX_EVENTS] = { 0 };

/* RCnt2 timer callback — called from PsyX interrupt thread */
static long (*g_rcnt2_callback)(void) = NULL;
int g_rcnt2_timer_active = 0;  /* exposed to PsyX_main.cpp */

#define CTR_RUNNING (0)
#define CTR_STOPPED (1)

#define CTR_MODE_TO_FFFF (0)
#define CTR_MODE_TO_TARG (1)

#define CTR_CLOCK_SYS (0)
#define CTR_CLOCK_PIXEL (1)
#define CTR_HORIZ_RETRACE (1)

#define CTR_CLOCK_SYS_ONE (0)
#define CTR_CLOCK_SYS_ONE_EIGHTH (1)

typedef struct
{
	unsigned int i_cycle;

	union
	{
		unsigned short cycle;
		unsigned short unk00;
	};

	unsigned int i_value;

	union
	{
		unsigned short value;
		unsigned short unk01;
	};

	unsigned int i_target;

	union
	{
		unsigned short target;
		unsigned short unk02;
	};


	unsigned int padding00;
	unsigned int padding01;
} SysCounter;

extern SysCounter counters[3];

SysCounter counters[3] = { 0 };

int SetRCnt(int spec, unsigned short target, int mode)//(F)
{
	int value = 0x48;

	spec &= 0xFFFF;
	if (spec > 2)
	{
		return 0;
	}

	counters[spec].value = 0;
	counters[spec].target = target;

	if (spec < 2)
	{
		if ((mode & 0x10))
		{
			value = 0x49;
		}
		else if ((mode & 0x1))//loc_148
		{
			value |= 0x100;
		}
	}
	else
	{
		//loc_158
		if (spec == 2 && !(mode & 1))
		{
			value = 0x248;
		}//loc_174
	}
	//loc_174
	if ((mode & 0x1000))
	{
		value |= 0x10;
	}//loc_180

	counters[spec].value = value;

	return 1;
}

int GetRCnt(int spec)//(F)
{
	spec &= 0xFFFF;

	if (spec > 2)
	{
		return 0;
	}

	return counters[spec].cycle;
}

int ResetRCnt(int spec)//(F)
{
	spec &= 0xFFFF;

	if (spec > 2)
	{
		return 0;
	}

	counters[spec].cycle = 0;

	return 1;
}

int StartRCnt(int spec)//(F)
{
	spec &= 0xFFFF;
	dword_300[1] |= dword_308[spec];
	return spec < 3 ? 1 : 0;
}

int StopRCnt(int spec)//TODO
{
	return 0;
}
#undef OpenEvent
int OpenEvent(unsigned int ev_class, int ev_spec, int ev_mode, long(*func)())
{
	/* Find a free slot */
	for (int i = 0; i < MAX_EVENTS; i++) {
		if (!g_events[i].used) {
			g_events[i].used = 1;
			g_events[i].enabled = 0;
			g_events[i].pending = 0;
			g_events[i].ev_class = ev_class;
			g_events[i].ev_spec = ev_spec;
			g_events[i].ev_mode = ev_mode;
			g_events[i].callback = func;
			return i + 1; /* 1-based handle */
		}
	}
	return -1;
}

int CloseEvent(unsigned int event)
{
	int idx = (int)event - 1;
	if (idx >= 0 && idx < MAX_EVENTS) {
		/* If this was the RCnt2 callback, clear it */
		if (g_events[idx].callback == g_rcnt2_callback && g_rcnt2_callback != NULL) {
			g_rcnt2_callback = NULL;
			g_rcnt2_timer_active = 0;
		}
		g_events[idx].used = 0;
		g_events[idx].enabled = 0;
		g_events[idx].callback = NULL;
	}
	return 0;
}

int EnableEvent(unsigned int event)
{
	int idx = (int)event - 1;
	if (idx >= 0 && idx < MAX_EVENTS && g_events[idx].used) {
		g_events[idx].enabled = 1;

		/* If this is an RCnt2 event, register the callback for the timer */
		if ((g_events[idx].ev_class & 0xFFFF) == 2) { /* RCntCNT2 */
			g_rcnt2_callback = g_events[idx].callback;
			g_rcnt2_timer_active = 1;
		}
	}
	return 0;
}

int DisableEvent(unsigned int event)
{
	int idx = (int)event - 1;
	if (idx >= 0 && idx < MAX_EVENTS) {
		g_events[idx].enabled = 0;
		if ((g_events[idx].ev_class & 0xFFFF) == 2) {
			g_rcnt2_timer_active = 0;
		}
	}
	return 0;
}

int WaitEvent(unsigned int event)
{
	/* Not needed for BGM — just return */
	return 0;
}

int TestEvent(unsigned int event)
{
	int idx = (int)event - 1;
	if (idx >= 0 && idx < MAX_EVENTS && g_events[idx].used && g_events[idx].pending) {
		g_events[idx].pending = 0;
		return 1;
	}
	return 0;
}

void DeliverEvent(unsigned int ev1, int ev2)
{
	/* Mark all matching events as pending; if EvMdINTR, also fire callback. */
	for (int i = 0; i < MAX_EVENTS; i++) {
		if (!g_events[i].used) continue;
		if (g_events[i].ev_class != ev1) continue;
		if (g_events[i].ev_spec != ev2) continue;
		g_events[i].pending = 1;
		if ((g_events[i].ev_mode & EvMdINTR) && g_events[i].callback) {
			g_events[i].callback();
		}
	}
}

void UnDeliverEvent(unsigned int ev1, int ev2)
{
	for (int i = 0; i < MAX_EVENTS; i++) {
		if (!g_events[i].used) continue;
		if (g_events[i].ev_class != ev1) continue;
		if (g_events[i].ev_spec != ev2) continue;
		g_events[i].pending = 0;
	}
}

/* Called from PsyX interrupt thread to pump RCnt2 timer */
void PsyX_PumpRCnt2Timer(void)
{
	if (g_rcnt2_timer_active && g_rcnt2_callback) {
		g_rcnt2_callback();
	}
}

int OpenTh(int(*func)(), unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int CloseTh(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int ChangeTh(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

/*
int open(char* unk00, unsigned int unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int close(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int lseek(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int read(int unk00, void* unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int write(int unk00, void* unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int ioctl(int unk00, int unk01, int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

struct DIRENTRY* firstfile(char* unk00, struct DIRENTRY* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

struct DIRENTRY* nextfile(struct DIRENTRY* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int erase(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int undelete(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int format(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
int rename(char* unk00, char* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int cd(char* unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
*/

int LoadTest(char*  unk00, struct EXEC* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Load(char * unk00, struct EXEC* unk01)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Exec(struct EXEC * unk00, int unk01, char** unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int LoadExec(char * unk00, unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int InitPAD(char * unk00, int unk01, char* unk02, int unk03)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int StartPAD()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void StopPAD()
{
	PSYX_UNIMPLEMENTED();
}

void EnablePAD()
{
	PSYX_UNIMPLEMENTED();
}

void DisablePAD()
{
	PSYX_UNIMPLEMENTED();
}

void FlushCache()
{
	PSYX_UNIMPLEMENTED();
}

void ReturnFromException()
{
	PSYX_UNIMPLEMENTED();
}
/*
int EnterCriticalSection()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void ExitCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}
*/
void Exception()
{
	PSYX_UNIMPLEMENTED();
}

void SwEnterCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}
void SwExitCriticalSection()
{
	PSYX_UNIMPLEMENTED();
}

PSX_API_EXPORT unsigned long SetSp(unsigned long newsp)//(F)
{
	unsigned long old_sp = sp;
	sp = newsp;
	return old_sp;
}

unsigned long GetSp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetGp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetCr()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetSr()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

unsigned long GetSysSp()
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int SetConf(unsigned int unk00, unsigned int unk01, unsigned int unk02)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void GetConf(unsigned int* unk00, unsigned int* unk01, unsigned int* unk02)
{
	PSYX_UNIMPLEMENTED();
}

/*
int _get_errno(void)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int _get_error(int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}
*/
void SystemError(char unk00, int unk01)
{
	PSYX_UNIMPLEMENTED();
}

void SetMem(int unk00)
{
	PSYX_UNIMPLEMENTED();
}

int Krom2RawAdd(unsigned int unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

int Krom2RawAdd2(unsigned short unk00)
{
	PSYX_UNIMPLEMENTED();
	return 0;
}

void _96_init(void)
{
	PSYX_UNIMPLEMENTED();
}

void _96_remove(void)
{
	PSYX_UNIMPLEMENTED();
}

void _boot(void)
{
	PSYX_UNIMPLEMENTED();
}

void ChangeClearPAD(int unk00)
{
	PSYX_UNIMPLEMENTED();
}

/* ============================================================================
 * PSX Memory Card backing storage (0.MCD / 1.MCD).
 *
 * Layout (PSX standard, 128KB per card):
 *   block 0 (frames 0..63, 8192 bytes) = directory
 *     frame 0      = "MC" header (0x4D43...)
 *     frames 1..15 = 15 directory entries (128 bytes each)
 *     frames 16..35 = broken-sector list (32 entries)
 *     frame 63     = test write frame (last)
 *   blocks 1..15 = 15 file data slots (8192 bytes each)
 *
 * Frame size = 128 bytes. Block size = 64 frames = 8192 bytes.
 *
 * This implementation routes:
 *   - _card_load/_clear/_info/_status/_wait/_chan/_format -> open/poke 0.MCD
 *   - _card_read/_card_write -> read/write 128-byte frames at frame index
 *   - open/lseek/read/write/close of "buXX:NAME" -> dir-entry lookup + block I/O
 *   - firstfile/nextfile -> directory scan
 *   - erase / format -> dir entry zero / full card wipe
 *
 * Each operation deliveres SwCARD/HwCARD EvSpIOE on success so the game's
 * memcard state machine (s_MemCard_Work) advances.
 * ============================================================================
 */

#define MC_FRAME_SIZE   128
#define MC_BLOCK_SIZE   8192
#define MC_FRAMES_PER_BLOCK 64
#define MC_NUM_BLOCKS   16
#define MC_TOTAL_SIZE   (MC_NUM_BLOCKS * MC_BLOCK_SIZE)
#define MC_DIR_ENTRY_COUNT 15

/* Directory entry attr (frame 0..15). Lower 3 bits = block-link / count.
 * Upper nibble: 0x50 = first/only, 0x51 = middle, 0x52 = last, 0xA0 = free,
 * 0xF0 = unusable. Game writes 0x51 with blockCount in low bits when creating. */
#define MC_DIR_ATTR_FREE         0xA0
#define MC_DIR_ATTR_FIRST_OR_ONLY 0x50

#pragma pack(push,1)
typedef struct {
	unsigned int   attr;        /* state | block-count */
	unsigned int   size;        /* bytes */
	unsigned short unknown;
	char           name[21];    /* PSX memcard name field is 21 bytes
	                             * (max 20 chars + null). The Silent
	                             * Hill save filename "BASLUS-00707SILENT00"
	                             * is exactly 20 chars long so the trailing
	                             * null lives in the 21st byte. A 20-byte
	                             * field truncates by one character which
	                             * makes the subsequent open() fail to
	                             * find the file. */
	char           padding[97]; /* pad to 128 bytes (4+4+2+21+97) */
} McDirEntry;
#pragma pack(pop)

/* Card files are numbered like the "buXY:" device paths, X*8+Y: 0..3 = port 1
 * slots A-D, 8..11 = port 2 slots A-D. The low-level _card_* calls take the
 * hardware channel instead, port*16+slot, and go through mc_card_from_channel. */
#define MC_CARD_FILES 12

static int  s_currentChannel = 0;
static int  s_lastCardOk[MC_CARD_FILES];

/* PSX file-handle table for open()/read()/write()/lseek()/close().
 * We use small positive ints as handles (1..MC_DIR_ENTRY_COUNT). */
typedef struct {
	int  used;
	int  channel;
	int  dirIndex;       /* 1..15 */
	int  flags;          /* O_RDONLY/O_WRONLY/O_CREAT */
	long offset;         /* current seek pos (0..size) */
	long size;           /* file size in bytes (block-aligned) */
} McFileHandle;
static McFileHandle s_handles[16] = { 0 };

/* Locate the gamedata/save directory; create it on first use. Returns
 * the static buffer (or NULL if creation fails — caller falls back to
 * cwd-relative path). On Windows mkdir(dirname) succeeds idempotently
 * via _mkdir which returns 0 on create / -1 + errno=EEXIST otherwise. */
static const char* mc_save_dir(void)
{
	static char dir[64];
	static int  s_initialised = 0;
	if (!s_initialised) {
		snprintf(dir, sizeof(dir), "gamedata/save");
#if defined(_WIN32)
		_mkdir(dir);
#else
		mkdir(dir, 0755);
#endif
		s_initialised = 1;
	}
	return dir;
}

static const char* mc_path_for_channel(int chan)
{
	static char buf[96];
	const char* dir = mc_save_dir();
	if (dir) {
		snprintf(buf, sizeof(buf), "%s/%d.MCD", dir, chan);
	} else {
		snprintf(buf, sizeof(buf), "%d.MCD", chan);
	}
	return buf;
}

static FILE* mc_fopen(int chan, const char* mode)
{
	return fopen(mc_path_for_channel(chan), mode);
}

static int mc_card_from_channel(int chan)
{
	return ((chan >> 4) & 1) * 8 + (chan & 3);
}

/* Directory frames carry an XOR of their first 127 bytes in byte 127. The
 * console BIOS and DuckStation's card editor expect it, so cards written here
 * stay usable outside the port. */
static void mc_seal_frame(unsigned char* frame)
{
	unsigned char x = 0;
	for (int i = 0; i < MC_FRAME_SIZE - 1; i++) x ^= frame[i];
	frame[MC_FRAME_SIZE - 1] = x;
}

/* Fields are written byte-wise so the image is little-endian on every host. */
static void mc_make_dir_frame(unsigned char* frame, unsigned int attr, unsigned int size, const char* name)
{
	memset(frame, 0, MC_FRAME_SIZE);
	frame[0] = (unsigned char)attr;
	frame[4] = (unsigned char)size;
	frame[5] = (unsigned char)(size >> 8);
	frame[6] = (unsigned char)(size >> 16);
	frame[7] = (unsigned char)(size >> 24);
	frame[8] = 0xFF;
	frame[9] = 0xFF;
	if (name) strncpy((char*)frame + 10, name, 20);
	mc_seal_frame(frame);
}

/* A standard formatted card, as the console BIOS writes it. */
static void mc_format_buffer(unsigned char* buf)
{
	memset(buf, 0, MC_BLOCK_SIZE);
	memset(buf + MC_BLOCK_SIZE, 0xFF, MC_TOTAL_SIZE - MC_BLOCK_SIZE);
	buf[0] = 'M';
	buf[1] = 'C';
	mc_seal_frame(buf);
	for (int i = 1; i <= MC_DIR_ENTRY_COUNT; i++)
		mc_make_dir_frame(buf + i * MC_FRAME_SIZE, MC_DIR_ATTR_FREE, 0, NULL);
	/* Frames 16..35: empty bad-sector relocation list. */
	for (int i = 16; i < 36; i++) {
		unsigned char* fr = buf + i * MC_FRAME_SIZE;
		memset(fr, 0xFF, 4);
		fr[8] = 0xFF;
		fr[9] = 0xFF;
		mc_seal_frame(fr);
	}
	memset(buf + 36 * MC_FRAME_SIZE, 0xFF, 27 * MC_FRAME_SIZE);
	memcpy(buf + 63 * MC_FRAME_SIZE, buf, MC_FRAME_SIZE);
}

/* Only port 1 and port 2 slot A are plugged in: the PC has no multitap. A
 * multitap slot still counts as present when an older build left saves on it
 * (those builds reported all eight slots present), so no save goes missing. */
static int mc_card_present(int card)
{
	if (card < 0 || card >= MC_CARD_FILES || (card & 4)) return 0;
	if ((card & 3) == 0) return 1;
	{
		unsigned char dir[(MC_DIR_ENTRY_COUNT + 1) * MC_FRAME_SIZE];
		FILE* f = mc_fopen(card, "rb");
		size_t n;
		if (!f) return 0;
		n = fread(dir, 1, sizeof(dir), f);
		fclose(f);
		if (n != sizeof(dir) || dir[0] != 'M' || dir[1] != 'C') return 0;
		for (int i = 1; i <= MC_DIR_ENTRY_COUNT; i++) {
			const unsigned char* e = dir + i * MC_FRAME_SIZE;
			if ((e[0] & 0xF0) == MC_DIR_ATTR_FIRST_OR_ONLY && e[10] != 0) return 1;
		}
	}
	return 0;
}

/* Creates a formatted card file on first use; fails for a slot with no card. */
static int mc_ensure_card(int chan)
{
	FILE* f;
	if (!mc_card_present(chan)) return 0;
	f = mc_fopen(chan, "rb");
	if (f) { fclose(f); return 1; }
	f = mc_fopen(chan, "wb");
	if (!f) return 0;
	{
		unsigned char* fresh = (unsigned char*)malloc(MC_TOTAL_SIZE);
		if (!fresh) { fclose(f); return 0; }
		mc_format_buffer(fresh);
		fwrite(fresh, 1, MC_TOTAL_SIZE, f);
		free(fresh);
	}
	fclose(f);
	return 1;
}

/* Read N bytes at byte offset; returns 1 on success. */
static int mc_read_at(int chan, long ofs, void* buf, int bytes)
{
	FILE* f = mc_fopen(chan, "rb");
	if (!f) return 0;
	if (fseek(f, ofs, SEEK_SET) != 0) { fclose(f); return 0; }
	size_t n = fread(buf, 1, bytes, f);
	fclose(f);
	return (int)n == bytes;
}

/* Write N bytes at byte offset; returns 1 on success. */
static int mc_write_at(int chan, long ofs, const void* buf, int bytes)
{
	FILE* f = mc_fopen(chan, "rb+");
	if (!f) {
		/* card missing — auto-create then retry */
		if (!mc_ensure_card(chan)) return 0;
		f = mc_fopen(chan, "rb+");
		if (!f) return 0;
	}
	if (fseek(f, ofs, SEEK_SET) != 0) { fclose(f); return 0; }
	size_t n = fwrite(buf, 1, bytes, f);
	fflush(f);
	fclose(f);
	return (int)n == bytes;
}

/* Find a directory entry by name. Returns 1..15 if found, 0 otherwise. */
static int mc_find_dir(int chan, const char* name, McDirEntry* outEntry)
{
	for (int i = 1; i <= MC_DIR_ENTRY_COUNT; i++) {
		McDirEntry e;
		if (!mc_read_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
		if ((e.attr & 0xF0) == MC_DIR_ATTR_FREE) continue;
		if (e.name[0] == '\0') continue;
		if (strncmp(e.name, name, sizeof(e.name)) == 0) {
			if (outEntry) *outEntry = e;
			return i;
		}
	}
	return 0;
}

/* Allocate a dir entry + contiguous data blocks for a new file.
 * blocks = block count requested. Returns dir index 1..15 or 0 on failure. */
static int mc_alloc_dir(int chan, const char* name, int blocks)
{
	/* read()/write() address a file as consecutive blocks from its directory
	 * slot, so the whole run must be free. */
	unsigned char dir[(MC_DIR_ENTRY_COUNT + 1) * MC_FRAME_SIZE];
	if (!mc_read_at(chan, 0, dir, sizeof(dir))) return 0;
	for (int i = 1; i + blocks - 1 <= MC_DIR_ENTRY_COUNT; i++) {
		int b;
		for (b = 0; b < blocks; b++)
			if ((dir[(i + b) * MC_FRAME_SIZE] & 0xF0) != MC_DIR_ATTR_FREE) break;
		if (b < blocks) continue;
		for (b = 0; b < blocks; b++) {
			unsigned char* fr = dir + (i + b) * MC_FRAME_SIZE;
			unsigned int attr = (b == 0) ? 0x51 : (b == blocks - 1 ? 0x53 : 0x52);
			mc_make_dir_frame(fr, attr, b == 0 ? (unsigned int)(blocks * MC_BLOCK_SIZE) : 0, b == 0 ? name : NULL);
			if (b < blocks - 1) {
				/* Link = next block index - 1, which is this slot's index. */
				fr[8] = (unsigned char)(i + b);
				fr[9] = 0;
				mc_seal_frame(fr);
			}
		}
		if (!mc_write_at(chan, i * MC_FRAME_SIZE, dir + i * MC_FRAME_SIZE, blocks * MC_FRAME_SIZE)) return 0;
		return i;
	}
	return 0;
}

/* Strip "buXX:" prefix if present, return remaining name. */
static const char* mc_strip_prefix(const char* path, int* outChan)
{
	if (!path) return NULL;
	if (path[0] == 'b' && path[1] == 'u' && path[3] != '\0' && path[4] == ':') {
		if (outChan) *outChan = (path[2] - '0') * 8 + (path[3] - '0');
		return path + 5;
	}
	if (outChan) *outChan = 0;
	return path;
}

/* Deliver SwCARD + HwCARD EvSpIOE so the game's state machine advances. */
static void mc_deliver_iod(void)
{
	DeliverEvent(SwCARD, EvSpIOE);
	DeliverEvent(HwCARD, EvSpIOE);
}

/* What the BIOS raises for an empty slot: the request is accepted, then times out. */
static void mc_deliver_timeout(void)
{
	DeliverEvent(SwCARD, EvSpTIMOUT);
	DeliverEvent(HwCARD, EvSpTIMOUT);
}

/* ----- BIOS-level memcard funcs ----- */

void InitCARD(int val)
{
	(void)val;
	MemCardInit(0);
	mc_ensure_card(0);
}

int StartCARD()
{
	MemCardStart();
	return 1;
}

int StopCARD()
{
	MemCardStop();
	return 1;
}

void _bu_init()
{
	mc_ensure_card(0);
}

/* Common front of the low-level calls: 1 = card there (event left to the
 * caller), 0 = no card (timeout already delivered). */
static int mc_card_request(int chan)
{
	int card = mc_card_from_channel(chan);
	s_currentChannel = chan;
	if (!mc_ensure_card(card)) {
		s_lastCardOk[card] = 0;
		mc_deliver_timeout();
		return 0;
	}
	s_lastCardOk[card] = 1;
	return 1;
}

int _card_info(int chan)
{
	if (mc_card_request(chan)) mc_deliver_iod();
	return 1;
}

int _card_clear(int chan)
{
	if (mc_card_request(chan)) mc_deliver_iod();
	return 1;
}

int _card_load(int chan)
{
	if (mc_card_request(chan)) mc_deliver_iod();
	return 1;
}

int _card_auto(int val)
{
	(void)val;
	return 1;
}

void _new_card()
{
	/* PSX BIOS: marks card as "newly inserted" — clear cached state. */
	memset(s_lastCardOk, 0, sizeof(s_lastCardOk));
}

int _card_status(int drv)
{
	return s_lastCardOk[mc_card_from_channel(drv)] ? 1 : 0;
}

int _card_wait(int drv)
{
	(void)drv;
	return 1;  /* sync mode — operation already complete */
}

unsigned int _card_chan(void)
{
	return (unsigned int)s_currentChannel;
}

int _card_write(int chan, int frameIdx, unsigned char *buf)
{
	if (frameIdx < 0 || frameIdx >= MC_TOTAL_SIZE / MC_FRAME_SIZE) return 0;
	if (!mc_card_request(chan)) return 1;
	if (!mc_write_at(mc_card_from_channel(chan), (long)frameIdx * MC_FRAME_SIZE, buf, MC_FRAME_SIZE)) return 0;
	mc_deliver_iod();
	return 1;
}

int _card_read(int chan, int frameIdx, unsigned char *buf)
{
	if (frameIdx < 0 || frameIdx >= MC_TOTAL_SIZE / MC_FRAME_SIZE) return 0;
	if (!mc_card_request(chan)) return 1;
	if (!mc_read_at(mc_card_from_channel(chan), (long)frameIdx * MC_FRAME_SIZE, buf, MC_FRAME_SIZE)) return 0;
	mc_deliver_iod();
	return 1;
}

/* Takes a card file number (format() passes the "buXY:" path's X*8+Y). */
static int mc_format_card(int c)
{
	unsigned char* fresh;
	if (!mc_card_present(c)) return 0;
	fresh = (unsigned char*)malloc(MC_TOTAL_SIZE);
	if (!fresh) return 0;
	mc_format_buffer(fresh);
	{
		FILE* f = mc_fopen(c, "wb");
		if (!f) { free(fresh); return 0; }
		fwrite(fresh, 1, MC_TOTAL_SIZE, f);
		fflush(f);
		fclose(f);
	}
	free(fresh);
	mc_deliver_iod();
	return 1;
}

int _card_format(int chan)
{
	return mc_format_card(mc_card_from_channel(chan));
}

/* ----- POSIX-style file ops on "buXX:NAME" paths ----- */

int open(char* path, unsigned int flags)
{
	if (!path) return -1;
	int chan = 0;
	const char* name = mc_strip_prefix(path, &chan);
	if (name == path) {
		/* Not a memcard path (e.g. "sim:..."). Not supported here. */
		return -1;
	}
	if (!mc_ensure_card(chan)) return -1;

	int blockCount = (int)((flags >> 16) & 0xFFFF);
	int isCreate = (flags & 0x0200) != 0;  /* O_CREAT/FCREAT */

	int dirIdx = mc_find_dir(chan, name, NULL);
	if (dirIdx == 0) {
		if (!isCreate) return -1;
		if (blockCount <= 0 || blockCount > 15) blockCount = 1;
		dirIdx = mc_alloc_dir(chan, name, blockCount);
		if (dirIdx == 0) return -1;
	}

	for (int h = 1; h < (int)(sizeof(s_handles)/sizeof(s_handles[0])); h++) {
		if (!s_handles[h].used) {
			McDirEntry e;
			if (!mc_read_at(chan, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return -1;
			s_handles[h].used = 1;
			s_handles[h].channel = chan;
			s_handles[h].dirIndex = dirIdx;
			s_handles[h].flags = (int)flags;
			s_handles[h].offset = 0;
			s_handles[h].size = (long)e.size;
			if (s_handles[h].size <= 0) s_handles[h].size = MC_BLOCK_SIZE;
			return h;
		}
	}
	return -1;
}

int close(int handle)
{
	if (handle < 1 || handle >= (int)(sizeof(s_handles)/sizeof(s_handles[0]))) return -1;
	if (!s_handles[handle].used) return -1;
	s_handles[handle].used = 0;
	return 0;
}

int lseek(int handle, int offset, int whence)
{
	if (handle < 1 || handle >= (int)(sizeof(s_handles)/sizeof(s_handles[0]))) return -1;
	if (!s_handles[handle].used) return -1;
	long base = 0;
	if (whence == 0) base = 0;
	else if (whence == 1) base = s_handles[handle].offset;
	else if (whence == 2) base = s_handles[handle].size;
	long p = base + offset;
	if (p < 0) return -1;
	s_handles[handle].offset = p;
	return (int)p;
}

int read(int handle, void* buf, int bytes)
{
	if (handle < 1 || handle >= (int)(sizeof(s_handles)/sizeof(s_handles[0]))) return -1;
	if (!s_handles[handle].used) return -1;
	int chan = s_handles[handle].channel;
	int dataBlock = s_handles[handle].dirIndex;  /* file occupies data blocks dirIndex..dirIndex+N-1 */
	long fileBase = (long)dataBlock * MC_BLOCK_SIZE;
	long ofs = fileBase + s_handles[handle].offset;
	if (!mc_read_at(chan, ofs, buf, bytes)) return -1;
	s_handles[handle].offset += bytes;
	mc_deliver_iod();
	return bytes;
}

int write(int handle, void* buf, int bytes)
{
	if (handle < 1 || handle >= (int)(sizeof(s_handles)/sizeof(s_handles[0]))) return -1;
	if (!s_handles[handle].used) return -1;
	int chan = s_handles[handle].channel;
	int dataBlock = s_handles[handle].dirIndex;
	long fileBase = (long)dataBlock * MC_BLOCK_SIZE;
	long ofs = fileBase + s_handles[handle].offset;
	if (!mc_write_at(chan, ofs, buf, bytes)) return -1;
	s_handles[handle].offset += bytes;
	mc_deliver_iod();
	return bytes;
}

int ioctl(int unk00, int unk01, int unk02)
{
	(void)unk00; (void)unk01; (void)unk02;
	return 0;
}

/* ----- Directory enumeration ----- */

/* Forward declaration: firstfile bootstraps the cursor and tail-calls
 * nextfile, which is defined just below. */
struct DIRENTRY* nextfile(struct DIRENTRY* dir);

/* Internal cursor for firstfile/nextfile. We embed it in DIRENTRY's `system[]`. */
struct DIRENTRY* firstfile(char* path, struct DIRENTRY* dir)
{
	if (!path || !dir) return NULL;
	int chan = 0;
	const char* pattern = mc_strip_prefix(path, &chan);
	(void)pattern;  /* "*" matches all; we don't support patterns here */
	if (!mc_ensure_card(chan)) return NULL;
	memset(dir, 0, sizeof(*dir));
	dir->system[0] = (char)chan;
	dir->system[1] = 0;  /* next dirIdx to scan */
	return nextfile(dir);
}

struct DIRENTRY* nextfile(struct DIRENTRY* dir)
{
	if (!dir) return NULL;
	int chan = (int)(unsigned char)dir->system[0];
	int start = (int)(unsigned char)dir->system[1] + 1;
	for (int i = (start < 1 ? 1 : start); i <= MC_DIR_ENTRY_COUNT; i++) {
		McDirEntry e;
		if (!mc_read_at(chan, i * MC_FRAME_SIZE, &e, sizeof(e))) return NULL;
		if ((e.attr & 0xF0) == MC_DIR_ATTR_FREE) continue;
		if (e.name[0] == '\0') continue;
		dir->system[1] = (char)i;
		/* Copy full 20-char name; null-terminate at byte 20.
		 * DIRENTRY.name is now 21 bytes (kernel.h) so the null fits. */
		memcpy(dir->name, e.name, 20);
		dir->name[20] = '\0';
		dir->attr = e.attr;
		dir->size = e.size;
		dir->head = 0;
		dir->next = NULL;
		return dir;
	}
	return NULL;
}

int erase(char* path)
{
	if (!path) return 0;
	int chan = 0;
	const char* name = mc_strip_prefix(path, &chan);
	McDirEntry e;
	int dirIdx = mc_find_dir(chan, name, &e);
	if (dirIdx == 0) return 0;
	{
		/* Follow the block links (a file copied onto a real card need not be
		 * contiguous). Older port builds wrote link 0 on single-block files, so
		 * the size decides how many blocks to free, not the link. */
		unsigned char fr[MC_FRAME_SIZE];
		int blocks = (int)((e.size + MC_BLOCK_SIZE - 1) / MC_BLOCK_SIZE);
		int cur = dirIdx;
		if (blocks < 1) blocks = 1;
		for (int b = 0; b < blocks && cur >= 1 && cur <= MC_DIR_ENTRY_COUNT; b++) {
			int next;
			if (!mc_read_at(chan, cur * MC_FRAME_SIZE, fr, MC_FRAME_SIZE)) return 0;
			next = fr[8] | (fr[9] << 8);
			mc_make_dir_frame(fr, MC_DIR_ATTR_FREE, 0, NULL);
			if (!mc_write_at(chan, cur * MC_FRAME_SIZE, fr, MC_FRAME_SIZE)) return 0;
			cur = (next == 0xFFFF) ? -1 : next + 1;
		}
	}
	return 1;
}

int undelete(char* unk00)
{
	(void)unk00;
	return 0;
}

int format(char* path)
{
	int card = 0;
	if (!path || mc_strip_prefix(path, &card) == path) return 0;
	return mc_format_card(card);
}

/* Signature uses const char* to match the stdio.h `rename` decl
 * (we include <stdio.h> for fopen/fread above). The libapi.h PSX
 * decl `int rename(char*, char*)` is commented out in our headers,
 * so the only in-scope declaration is stdio's. Game callers pass
 * char* which converts implicitly to const char*; we never write
 * to the input strings here so const is correct semantically too. */
int rename(const char* oldpath, const char* newpath)
{
	if (!oldpath || !newpath) return 0;
	int chanA = 0, chanB = 0;
	const char* oldname = mc_strip_prefix(oldpath, &chanA);
	const char* newname = mc_strip_prefix(newpath, &chanB);
	if (chanA != chanB) return 0;
	int dirIdx = mc_find_dir(chanA, oldname, NULL);
	if (dirIdx == 0) return 0;
	McDirEntry e;
	if (!mc_read_at(chanA, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
	memset(e.name, 0, sizeof(e.name));
	strncpy(e.name, newname, sizeof(e.name) - 1);
	mc_seal_frame((unsigned char*)&e);
	if (!mc_write_at(chanA, dirIdx * MC_FRAME_SIZE, &e, sizeof(e))) return 0;
	return 1;
}

int cd(char* unk00)
{
	(void)unk00;
	return 0;
}
