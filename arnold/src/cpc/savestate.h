/* Arnold for Amiga: exact save states (1.4, 28 Oct 2026). See savestate.c and
 * Docs/agent_notes/savestate.md.
 *
 * A state is a standard SNA v3 file (header, RAM, the CPC+ chunk), so other
 * emulators can read the RAM and registers, plus a private "ARNS" chunk with
 * the exact Arnold state (big-endian, versioned, CRC-32 checked). Loading an
 * ARNS state resumes the emulation exactly; a plain .SNA loads as before
 * (upstream's Snapshot_Insert, lossy).
 *
 * Save and load only between two calls of CPCEmulation_Run(): a host's
 * Host_ProcessSystemEvents returns TRUE to stop the loop at the end of a
 * frame, the host saves or loads, then calls CPCEmulation_Run() again. That
 * is the same point as the menu (Help), and running on from it is exactly the
 * same as never having stopped. */
#ifndef SAVESTATE_H
#define SAVESTATE_H

#define SAVESTATE_FORMAT 1        /* bump on any change to what the sections hold */

/* SaveState_Load results */
#define SAVESTATE_REFUSED 0       /* nothing changed; msg says why */
#define SAVESTATE_EXACT   1       /* an Arnold state, resumed exactly */
#define SAVESTATE_PLAIN   2       /* a plain .SNA (no ARNS chunk), loaded the lossy upstream way */

typedef struct
{
	/* names shown in messages when the media don't match (file part only is kept) */
	const char *cart_name, *disk_name[2], *tape_name;
	/* the host's own data, stored as is (byte order is the host's job) */
	const unsigned char *host;
	unsigned long hostlen;
	/* no longer used: every inserted disk is stored in the state (John, 28 Oct 2026) */
	int embed_disk[2];
} SAVESTATE_INFO;

/* what a load did besides the machine (SaveState_Load fills it) */
typedef struct
{
	const unsigned char *host;    /* the host's data in the state (points into the loaded file) */
	unsigned long hostlen;
	int disk[2];                  /* 0 as it was, 1 emptied (the state's drive was empty), 2 the state's own copy put in */
	int tape;                     /* 0 as it was, 1 taken out (the state's deck was empty) */
	char cart_name[64], disk_name[2][64], tape_name[64];   /* as recorded when saved (file part) */
} SAVESTATE_LOADED;

/* save: *pData is malloc'd (*pLen bytes); 1 = done, 0 = failed (msg) */
int SaveState_Save(unsigned char **pData, unsigned long *pLen, const SAVESTATE_INFO *info, char *msg, int msglen);
/* load: data stays the caller's (out->host points into it); out may be NULL */
int SaveState_Load(const unsigned char *data, unsigned long len, char *msg, int msglen, SAVESTATE_LOADED *out);
/* checks only (no change): 1 = an Arnold state that this machine can load, 2 = a
 * plain .SNA that would load the lossy way, 0 = refused (msg) */
int SaveState_Check(const unsigned char *data, unsigned long len, char *msg, int msglen);
unsigned long SaveState_Crc32(unsigned long crc, const unsigned char *p, unsigned long n);   /* start with 0 */

/* ---- the stream, for each module's state function (SS_xxx at the end of its file) ---- */
typedef struct SS_IO
{
	int load;                     /* 1 reading into the machine, 0 writing it out */
	unsigned char *buf;           /* save: grows (malloc); load: the data read */
	unsigned long pos, len, cap;
	unsigned long end[8];         /* load: end of each open section */
	unsigned long start[8];       /* save: where each open section's length goes */
	int depth;
	int err;                      /* set: stop, msg says why */
	char msg[200];
} SS_IO;

void SS_Int(SS_IO *io, void *p, int size, int sign);       /* an integer of 1, 2, 4 or 8 bytes */
void SS_Raw(SS_IO *io, void *p, unsigned long n);          /* bytes as they are */
void SS_Pack(SS_IO *io, void *p, unsigned long n);         /* bytes, run-length packed */
long SS_Long(SS_IO *io, long v);                           /* save v / return the value read */
void SS_Begin(SS_IO *io, const char *tag);                 /* a section: 4-character tag + length */
void SS_End(SS_IO *io);
void SS_Fail(SS_IO *io, const char *msg);
#define SS_SIGNED(x) ((__typeof__(x))-1 < (__typeof__(x))0)
#define SS_V(io, x) SS_Int((io), &(x), (int)sizeof(x), SS_SIGNED(x))
#define SS_A(io, a) do { int ss_i_; for (ss_i_ = 0; ss_i_ < (int)(sizeof(a) / sizeof((a)[0])); ss_i_++) SS_V((io), (a)[ss_i_]); } while (0)
#define SS_RAWA(io, a) SS_Raw((io), (a), (unsigned long)sizeof(a))

/* the modules (each at the end of its own file; savestate.c calls them in order) */
void SS_Z80(SS_IO *io);
void SS_Cpc(SS_IO *io);
void SS_GateArray(SS_IO *io);
void SS_Pal(SS_IO *io);
void SS_Crtc(SS_IO *io);
void SS_Asic(SS_IO *io);
void SS_Psg(SS_IO *io);
void SS_PsgPlay(SS_IO *io);
void SS_Audio(SS_IO *io);
void SS_TapeAudio(SS_IO *io);
void SS_Fdc(SS_IO *io);
void SS_Fdd(SS_IO *io);
void SS_Fdi(SS_IO *io);
void SS_Printer(SS_IO *io);
void SS_Tape(SS_IO *io);
void SS_Render(SS_IO *io);
void SS_Arnold(SS_IO *io);
void SS_DiskImage(SS_IO *io, int drive);                   /* a drive's whole disk (the MEDI section) */
int  SS_DiskImageFlags(int drive, int *type, int set, int flags, int ntype);   /* the unit's flags/type */

/* machine description for the checks before a load (cpc.c, asic.c, tzxold.c) */
typedef struct
{
	long hardware, disc, ramexp, ramconfig, crtc, extraram;
	unsigned long cart_crc;       /* Plus: the cartridge's ROM (asic.c), 0 none */
	unsigned long rom_crc[3];     /* plain CPC: OS, BASIC, AMSDOS (0 none) */
} SS_MACHINE;
void SS_GetMachine(SS_MACHINE *m);
unsigned long ASIC_CartCrc(void);
int  SS_TapeCrc(unsigned long *crc, unsigned long *size);  /* 1 = a tape is in */
int  SS_TapeSampleInUse(void);
#endif
