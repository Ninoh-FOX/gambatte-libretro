/* Dual Cable Link (see dual.h). */

#include "dual.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace dual {

/* ---- protocol ------------------------------------------------------------ */

#define DUAL_HEADER        0x47424431   /* GBD1 (Game Boy, Dual) */
#define DUAL_VERSION       1

/* Packets: header, type | sender's slot << 8 (FF: none yet), words, data. */
#define DPK_HELLO          1   /* version, rom, settings, host */
#define DPK_PING           2   /* time */
#define DPK_PONG           3   /* time (echoed) */
#define DPK_STATE          4   /* offset, total (coded) size, data */
#define DPK_READY          5   /* input delay, one way time (us), clock */
#define DPK_INPUT          6   /* frame, buttons, current frame, lead (us),
                                  times we waited */
#define DPK_HASH           7   /* frame, hash of each slot's Game Boy */
#define DPK_BYE            8
#define DPK_START          10  /* client id of each slot */
#define DPK_GO             11  /* the states can go */
#define DPK_GIVEUP         12  /* games or settings differ */
/* The real time cable (different games): */
#define DPK_XFER           20  /* transfer number, byte, fast (CGB), frame */
#define DPK_REPLY          21  /* transfer number, byte, frame */
#define DPK_CANCEL         22  /* transfer number */
#define DPK_BLOCK          24  /* block number, size, offset, data */
#define DPK_ALIVE          25  /* we have the block routine (see below) */
#define DPK_TICK           26  /* frame (while bytes go, see rt_sync_point) */

#define NP                 2           /* Game Boys on the cable */
#define NO_SLOT            0xFF
#define CHUNK              (16 * 1024)
#define RING               256         /* Frames of buttons kept */
#define HASH_EVERY         60          /* Frames between agreement checks */
#define HASH_SLOTS         4
#define HELLO_US           6000000     /* No answer: the other core is not dual */
#define HELLO_EVERY_US     250000      /* HELLO sent again until the host's comes */
#define SETUP_US           30000000    /* Whole setup */
#define PEER_US            5000000     /* Peer silent while playing: it is gone */
#define STALL_US           12000       /* Longest wait for buttons in a frame */
#define FRAME_US           16743
#define KEY_RESET          0x80000000u /* With the buttons: both games restart */
#define PINGS              8           /* Round trips measured before the states */
#define PING_US            1000000     /* ...for at most this long */
#define PONG_US            200000      /* A ping with no answer: send another */
#define SLICE_US           10000       /* Setup work per frame (it waits anyway) */
#define SYNC_WIN           16          /* Packets the peer's position comes from */
#define SYNC_US            (FRAME_US * 5 / 8)   /* Ahead by more: wait */
#define SYNC_GAP           60          /* Frames between two waits */
#define DELAY_MORE         4           /* Most frames added to the input delay */
#define DELAY_CALM_US      60000000    /* Peer happy this long: one frame less */
#define DELAY_CALM_MAX     600000000   /* (twice as long each time that was wrong) */

/* Emulated time: a frame is 70224 cycles of the 4194304 Hz clock, 35112
 * sound samples. Both Game Boys run a frame in slices of this many samples,
 * one after the other (the host's first), so that what one sends over the
 * cable reaches the other within a slice (a bit, at the normal speed). */
#define FRAME_SAMPLES      35112
#define SLICE_SAMPLES      256
#define SOUND_SPARE        4096        /* runFor can run past what it was asked */

#define MSG_DIFFER "Cable Link: games or settings differ, using the real time cable"
/* Real time cable: a byte from the other end that our game does not take
 * in this long (our time, whatever the network does) is answered with FF,
 * as a cable with nothing ready on this end; at once if our game was not
 * ready for a transfer at all in the last RT_NOT_READY_US (it is not
 * listening: a real cable would not wait either). */
#define RT_HOLD_US         250000
#define RT_NOT_READY_US    50000
/* Ours: the other end always answers (see RT_HOLD_US), this is only in
 * case it is stuck. */
#define RT_REPLY_US        3000000
#define RT_ALIVE_US        1000000     /* A packet at least this often */
#define RT_PEER_US         5000000     /* Nothing for this long: it is gone */
#define RT_BLOCK_US        10000000    /* No block back: blocks byte by byte */
#define RT_BLOCK_MAX       0x1000
/* Frames of the real time cable (see rt_sync_point): ahead of the other
 * console by more than this, a console waits; RT_GAP frames without a byte
 * start a new exchange; RT_ACTIVE frames after a byte both stop keeping
 * pace; a wait longer than RT_WAIT_MAX_US ends the waits until next byte. */
#define RT_SLACK           3
#define RT_GAP             60
#define RT_ACTIVE          120
#define RT_WAIT_MAX_US     1000000
/* The other's frames are only waited for while they keep coming (it stops
 * telling them when it is done with the exchange). */
#define RT_FRESH_US        100000
#define RT_HELD_MAX        8   /* Its transfers waiting for our game */

enum { DS_OFF = 0, DS_HELLO, DS_WAIT, DS_PING, DS_TRANSFER, DS_READY, DS_RUN };

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;

/* The Game Boy's buttons, given by the session. */
class Keys : public gambatte::InputGetter {
public:
   unsigned keys;
   Keys() : keys(0) {}
   unsigned operator()() { return keys; }
};

/* One end of the cable in memory. A transfer on the internal clock (the
 * game wrote 81 to FF02) gets the other end's byte at once if that one
 * waits for a transfer, and FF if not (no clock on its side: the line is
 * high); the other end gets ours when it runs next (within a slice). */
class CableEnd : public gambatte::SerialIO {
public:
   gambatte::GB *gb;
   CableEnd *peer;
   bool has_in;
   unsigned char in;
   bool in_fast;

   CableEnd() : gb(0), peer(0), has_in(false), in(0xFF), in_fast(false) {}
   void clear() { has_in = false; }

   unsigned char send(unsigned char data, bool fastCgb) {
      if (!peer || !peer->gb || peer->has_in || !peer->gb->serialWaiting())
         return 0xFF;
      peer->in = data;
      peer->in_fast = fastCgb;
      peer->has_in = true;
      return peer->gb->serialData();
   }

   bool check(unsigned char, unsigned char &data, bool &fastCgb) {
      if (!has_in)
         return false;
      has_in = false;
      data = in;
      fastCgb = in_fast;
      return true;
   }
};

static Frontend fe;
static void rt_send(u32 type, const u32 *words, u32 nwords);
static void rt_send_data(u32 type, const u32 *words, u32 nwords, const void *data, u32 len);
static void rt_blocks_off(void);
static uint64_t rt_now(void);
static u32 rt_frame(void);
static void rt_sync_point(u32 own, u32 peer);

/* The real time cable, for two different games (a copy of the other's Game
 * Boy cannot be run without its game): only the bytes go over the network.
 *
 * During a transfer of ours our Game Boy stops until the other end's byte
 * comes (a game would otherwise give up on a transfer that takes longer
 * than on a real cable, as Pokémon does while it looks for the other
 * player). One from the other end is answered as soon as it comes if our
 * game is ready for it: our Game Boy is stopped whenever the network is
 * read (between frames, or between the slices of one), so the byte it has
 * prepared is the one a cable would take right then, and the other end's
 * byte goes in before it runs on (check()). If our game is not ready, the
 * byte waits until it is (and is answered then), or is answered with FF if
 * our game is not listening at all or does not take it soon: this end
 * decides, not the network delay. A round trip a byte, but Pokémon (red,
 * blue, yellow, gold, silver, crystal, every language) sends its big
 * blocks (the party, the patch lists, ...) with a routine,
 *   Serial_ExchangeBytes, where what each side sends does not depend on
 *   what it gets: both blocks go over at once instead, and the routine is
 *   done when the other one comes (see GB::setBlockHook). Only when both
 *   consoles found the routine in their game. */
class RtCable : public gambatte::SerialIO, public gambatte::BlockExchanger {
public:
   /* Ours (internal clock) */
   bool awaiting, has_reply;
   u32 seq_out;
   unsigned char reply_byte;
   u64 t_sent;
   u32 xfer_frame;              /* Our frame when we sent it */
   /* The other end's (it clocks, we answer), oldest first: once one is
    * answered the other end may send its next before our game took it. */
   struct Held {
      u32 seq, frame;           /* frame: its frame when it sent it */
      unsigned char byte;
      bool fast, framed;        /* framed: it said (a core that keeps pace) */
      bool answered;            /* Answered already: our game takes it next */
      u64 t;                    /* When it came */
   } held[RT_HELD_MAX];
   unsigned n_held;
   u64 t_ready;                 /* Our game was last ready for one then */
   /* Blocks */
   u32 blk_out;                 /* Number of our next block */
   bool blk_sent;
   u64 t_blk;
   /* The other's blocks: it may send its next one before we are done
    * with this one (it is done when ours comes), so two. */
   struct PeerBlock {
      u8 data[RT_BLOCK_MAX];
      u32 seq, len, rcvd;
   } peer_blk[2];

   RtCable() { reset(); }
   void reset() {
      awaiting = has_reply = false;
      seq_out = 0;
      reply_byte = 0xFF;
      t_sent = t_ready = 0;
      xfer_frame = 0;
      n_held = 0;
      blk_out = 0;
      blk_sent = false;
      t_blk = 0;
      for (unsigned i = 0; i < 2; i++) {
         peer_blk[i].seq = ~0u;
         peer_blk[i].len = peer_blk[i].rcvd = 0;
      }
   }

   bool async() const { return true; }

   unsigned char send(unsigned char data, bool fastCgb) {
      u32 w[4];
      seq_out++;
      awaiting = true;
      has_reply = false;
      t_sent = rt_now();
      xfer_frame = rt_frame();
      w[0] = seq_out;
      w[1] = data;
      w[2] = fastCgb ? 1 : 0;
      w[3] = xfer_frame;
      rt_send(DPK_XFER, w, 4);
      return 0xFF;
   }

   bool reply(unsigned char &in) {
      if (!awaiting)
         return false;
      if (has_reply) {
         awaiting = has_reply = false;
         in = reply_byte;
         return true;
      }
      if (rt_now() - t_sent > RT_REPLY_US) {
         cancel();
         in = 0xFF;
         return true;
      }
      return false;
   }

   void cancel() {
      if (awaiting) {
         u32 w[1] = { seq_out };
         rt_send(DPK_CANCEL, w, 1);
      }
      awaiting = has_reply = false;
   }

   /* The other end's answer to our transfer `seq`. */
   void answered(u32 seq, unsigned char byte, bool framed, u32 frame) {
      if (!awaiting || seq != seq_out)
         return;                /* (late: one we gave up on) */
      reply_byte = byte;
      has_reply = true;
      if (framed)
         rt_sync_point(xfer_frame, frame);
   }

   /* Our port: when it was last ready for a transfer. */
   void idle(bool ready) {
      if (ready)
         t_ready = rt_now();
   }

   bool holding() const { return n_held != 0; }

   /* Answer the oldest one with `out`. */
   void answer(unsigned char out) {
      Held &h = held[0];
      u32 w[3];
      w[0] = h.seq;
      w[1] = out;
      w[2] = rt_frame();
      rt_send(DPK_REPLY, w, 3);
      if (h.framed)
         rt_sync_point(w[2], h.frame);
      h.answered = true;
   }

   /* The oldest one is done. */
   Held drop() {
      const Held h = held[0];
      unsigned i;
      for (i = 0; i + 1 < n_held; i++)
         held[i] = held[i + 1];
      n_held--;
      return h;
   }

   /* A transfer of the other end came. `ready`: our game is ready for it
    * right now (with `out` prepared). */
   void hold(u32 seq, unsigned char byte, bool fast, bool framed, u32 frame,
             bool ready, unsigned char out) {
      if (n_held == RT_HELD_MAX)
         return;                /* (it never sends that many ahead) */
      held[n_held].seq = seq;
      held[n_held].byte = byte;
      held[n_held].fast = fast;
      held[n_held].framed = framed;
      held[n_held].frame = frame;
      held[n_held].answered = false;
      held[n_held].t = rt_now();
      n_held++;
      if (n_held == 1 && ready)
         answer(out);
   }

   /* The other end gave up on one. */
   void unhold(u32 seq) {
      unsigned i;
      for (i = 0; i < n_held && held[i].seq != seq; i++);
      if (i == n_held || held[i].answered)
         return;
      for (; i + 1 < n_held; i++)
         held[i] = held[i + 1];
      n_held--;
   }

   /* The other end's byte waited too long: our game was not ready for a
    * transfer, so it gets nothing and the other end FF. */
   void hold_expired() {
      const u64 now = rt_now();
      while (n_held && !held[0].answered &&
             (now - held[0].t > RT_HOLD_US || now - t_ready > RT_NOT_READY_US)) {
         answer(0xFF);
         drop();
      }
   }

   /* Our game waits for a transfer the other end clocks. */
   bool check(unsigned char out, unsigned char &data, bool &fastCgb) {
      if (!n_held)
         return false;
      if (!held[0].answered)
         answer(out);
      const Held h = drop();
      data = h.byte;
      fastCgb = h.fast;
      return true;
   }

   int exchange(const unsigned char *data, unsigned len, unsigned char *recv) {
      unsigned i;
      if (!blk_sent) {
         u32 off;
         for (off = 0; off < len; off += CHUNK) {
            u32 w[3] = { blk_out, len, off };
            rt_send_data(DPK_BLOCK, w, 3, &data[off], len - off < CHUNK ? len - off : CHUNK);
         }
         blk_sent = true;
         t_blk = rt_now();
      }
      PeerBlock &p = peer_blk[blk_out & 1];
      if (p.seq == blk_out && p.len && p.rcvd >= p.len) {
         /* What the routine stores: the other block from its second byte
          * (the first preamble byte only ends the waiting for it). */
         for (i = 0; i < len; i++)
            recv[i] = i + 1 < p.len ? p.data[i + 1] : 0xFE;
         blk_out++;
         blk_sent = false;
         p.seq = ~0u;
         p.len = p.rcvd = 0;
         return DONE;
      }
      if (rt_now() - t_blk > RT_BLOCK_US) {
         rt_blocks_off();
         return RUN;
      }
      return WAIT;
   }
};

static struct {
   /* The game */
   gambatte::GB *local;
   const u8 *rom;               /* (our copy) */
   size_t rom_size;
   unsigned flags;
   u32 rom_crc;
   u32 cfg;

   /* The other player's Game Boy, and the cable */
   gambatte::GB *peer;
   bool peer_loaded;
   Keys keys_in[NP];
   CableEnd end[NP];
   u32 count[NP];               /* Samples run past the frame start */

   /* Real time cable (when the games differ) */
   bool rt;
   unsigned rt_peer;            /* Its client id */
   RtCable rt_cable;
   unsigned short blk_pc;       /* The game's block routine (0: none) */
   unsigned char blk_flag;      /* ...and its HRAM flag */
   bool rt_blocks;              /* Both have it: blocks on */
   bool rt_blocks_failed;       /* A block got no answer: off for good */
   u64 rt_heard, rt_told, rt_alive;
   /* Keeping pace (see rt_sync_point) */
   u32 rt_frames;               /* Our frames run */
   u32 rt_peer_frames;          /* The other's, as far as we know */
   u64 rt_peer_frames_at;       /* ...told then */
   bool rt_peer_frames_on;      /* It tells them (a core that keeps pace) */
   bool rt_anchored;
   s32 rt_anchor;               /* Ours - its at the exchange's first byte */
   u32 rt_sync_own, rt_sync_peer, rt_sync_blk;   /* At the last byte */
   bool rt_wait_off;            /* Waited too long: no more till next byte */
   u64 rt_wait_since;

   /* Session */
   int state;
   bool is_host;
   bool gave_up;                /* Real time cable for the rest of the session */
   unsigned my_id;              /* Our libretro client id (host: 0) */
   unsigned my_slot;
   int slot_id[NP];             /* libretro client id of each slot's console */
   u64 t_state;                 /* When the current step started */
   u64 t_heard;                 /* Last packet from the other console */

   /* Host, before START */
   int cand;                    /* The client that joined (-1: none) */
   bool cand_hello;             /* Its HELLO came (and matched) */
   u64 t_join;
   u64 early_hello;             /* HELLOs before the frontend told us (bit = id) */
   u64 early_differ;            /* ...that did not match (bit = id): the
                                   answer waits until it joined, RetroArch
                                   drops what we send to it before */

   /* Client, before START */
   bool hello_sent;
   u64 t_hello;
   bool go;

   /* Host: round trips */
   u32 rtt[PINGS], rtt_n;
   bool ping_out;
   u64 t_ping;

   u32 ow_us;                   /* One way time */
   bool peer_ready;             /* Client: READY came */
   u32 delay;                   /* Input delay, frames (agreed at the start) */
   u32 send_delay;              /* Ours now: more after the peer waited */
   u32 stalls, stall_frame;     /* Frames we waited for the peer's buttons */
   u32 peer_stalls;             /* ...and the peer for ours */
   u64 t_peer_stall, t_lowered, calm_us;
   bool reset_wanted;
   u64 clock_base;              /* Cartridge clock: seconds at frame 0 */

   u8 *own_blob, *own_pack;     /* Our Game Boy, and the same run length coded */
   u32 blob_size, own_pack_len;
   u8 *peer_pack;
   u32 peer_pack_len, peer_rcvd;
   bool loaded;

   u32 frame;                   /* Next frame to run */
   u32 sent_upto;
   u32 own_keys[RING];
   u32 peer_keys[RING];
   u32 frame_ok[RING];          /* Peer's buttons valid when this is frame + 1 */
   u32 my_hash[HASH_SLOTS][1 + NP];
   u32 peer_hash[HASH_SLOTS][1 + NP];
   u32 hash_done, hash_bad;

   /* Time sync: where the peer is, from its INPUTs (its frame, in us, at our
    * time 0); the best of the last ones is the least late. */
   s64 peer_pos[SYNC_WIN];
   u32 peer_pos_n;
   s32 peer_lead;
   u32 sync_next;
} ds;

/* ---- helpers --------------------------------------------------------------- */

static u32 be32(u32 v) {
   const u8 *b = (const u8*)&v;
   return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static u32 fnv(u32 h, const void *data, size_t len) {
   const u8 *p = (const u8*)data;
   while (len--)
      h = (h ^ *p++) * 16777619u;
   return h;
}

static void dual_send(unsigned to, u32 type, const u32 *words, u32 nwords,
                      const void *data, u32 len) {
   static u8 buf[8 + 16 * 4 + CHUNK];
   u32 *w = (u32*)buf;
   u32 i;
   w[0] = be32(DUAL_HEADER);
   w[1] = be32(type | ((ds.state >= DS_PING ? ds.my_slot : NO_SLOT) << 8));
   for (i = 0; i < nwords; i++)
      w[2 + i] = be32(words[i]);
   if (len)
      memcpy(&buf[8 + nwords * 4], data, len);
   fe.send(to, buf, 8 + nwords * 4 + len);
}

/* To the other console of the session (host: the client; client: the host). */
static void dual_send_peer(u32 type, const u32 *words, u32 nwords,
                           const void *data, u32 len) {
   dual_send(ds.is_host ? (unsigned)ds.slot_id[1] : 0, type, words, nwords, data, len);
}

static void dual_msg(const char *text) {
   if (fe.message)
      fe.message(text);
}

static uint64_t rt_now(void) {
   return fe.time_usec();
}

static void rt_send(u32 type, const u32 *words, u32 nwords) {
   if (ds.rt) {
      dual_send(ds.rt_peer, type, words, nwords, NULL, 0);
      ds.rt_told = rt_now();
   }
}

static void rt_send_data(u32 type, const u32 *words, u32 nwords, const void *data, u32 len) {
   if (ds.rt) {
      dual_send(ds.rt_peer, type, words, nwords, data, len);
      ds.rt_told = rt_now();
   }
}

/* A block was not answered: blocks go byte by byte for the rest of the
 * session (the other console gives up on its own too). */
static void rt_blocks_off(void) {
   ds.rt_blocks = false;
   ds.rt_blocks_failed = true;
   if (ds.local)
      ds.local->setBlockHook(0, 0, NULL);
}

/* Keeping pace. On a cable both Game Boys run at the same speed, and games
 * count on it: Pokémon, waiting for the other player's move, sends it once
 * a frame for a number of frames, and so does the other (both loops count
 * frames, not bytes). Here a transfer of ours stops our Game Boy for a
 * network round trip, while the other one runs on: it would be done with
 * its loops long before ours, and our late bytes would be taken as its
 * next move. So, while bytes go, each console tells the other how many
 * frames it ran, and the one ahead (from where both were at the first byte
 * of the exchange, which a real cable has at the same moment) by more than
 * RT_SLACK frames waits, as if its Game Boy were slower. A byte from the
 * other end always lets it run (it is waiting for our answer). */
static u32 rt_frame(void) {
   return ds.rt_frames;
}

static void rt_peer_frame(u32 f) {
   if (!ds.rt_peer_frames_on || (s32)(f - ds.rt_peer_frames) > 0)
      ds.rt_peer_frames = f;
   ds.rt_peer_frames_at = rt_now();
   ds.rt_peer_frames_on = true;
}

/* A byte went over: our frame and the other's when it did (both consoles
 * get the very same pair, so they take the same decisions). */
static void rt_sync_point(u32 own, u32 peer) {
   if (!ds.rt_anchored || (s32)(own - ds.rt_sync_own) > RT_GAP ||
       (s32)(peer - ds.rt_sync_peer) > RT_GAP || ds.rt_cable.blk_out != ds.rt_sync_blk) {
      /* A new exchange (or a block went over, which has its own waits). */
      ds.rt_anchored = true;
      ds.rt_anchor = (s32)(own - peer);
   }
   ds.rt_sync_own = own;
   ds.rt_sync_peer = peer;
   ds.rt_sync_blk = ds.rt_cable.blk_out;
   ds.rt_wait_off = false;
   rt_peer_frame(peer);
}

/* Pokémon's Serial_ExchangeBytes in ROM bank 0 (the same code in every
 * game of the first two generations): its address and its HRAM flag. */
static void find_block_routine(void) {
   static const short sig[] = {
      0x3E,0x01,0xE0,-1,0x7E,0xE0,-1,0xCD,-1,-1,0xC5,0x47,0x23,0x3E,0x30,0x3D,
      0x20,0xFD,0xF0,-1,0xA7,0x78,0xC1,0x28,-1,0x2B,0xFE,0xFD,0x20,-1,0xAF,0xE0,
      -1,0x18,-1,0x12,0x13,0x0B,0x78,0xB1,0x20,-1,0xC9 };
   const size_t n = sizeof(sig) / sizeof(sig[0]);
   const size_t end = ds.rom_size < 0x4000 ? ds.rom_size : 0x4000;
   size_t i, j, found = 0, at = 0;
   ds.blk_pc = 0;
   ds.blk_flag = 0;
   for (i = 0x100; i + n <= end; i++) {
      for (j = 0; j < n && (sig[j] < 0 || ds.rom[i + j] == sig[j]); j++);
      if (j == n) {
         found++;
         at = i;
      }
   }
   /* Only one, and its flag the same in all three places. */
   if (found == 1 && ds.rom[at + 3] == ds.rom[at + 19] && ds.rom[at + 3] == ds.rom[at + 32]) {
      ds.blk_pc = (unsigned short)at;
      ds.blk_flag = ds.rom[at + 3];
   }
}

/* The real time cable, with the console of this client id. */
static void rt_start(unsigned peer) {
   u32 w[1];
   if (!ds.local)
      return;
   ds.rt = true;
   ds.rt_peer = peer;
   ds.rt_cable.reset();
   ds.rt_blocks = ds.rt_blocks_failed = false;
   ds.rt_heard = ds.rt_alive = rt_now();
   ds.rt_frames = ds.rt_peer_frames = 0;
   ds.rt_peer_frames_at = 0;
   ds.rt_peer_frames_on = ds.rt_anchored = ds.rt_wait_off = false;
   ds.rt_anchor = 0;
   ds.rt_sync_own = ds.rt_sync_peer = ds.rt_sync_blk = 0;
   ds.rt_wait_since = 0;
   ds.local->setSerialIO(&ds.rt_cable);
   w[0] = ds.blk_pc ? 1 : 0;
   rt_send(DPK_ALIVE, w, 1);
}

static void rt_stop(void) {
   if (!ds.rt)
      return;
   ds.rt = false;
   ds.rt_cable.reset();
   ds.rt_blocks = false;
   if (ds.local) {
      ds.local->setSerialIO(NULL);
      ds.local->setBlockHook(0, 0, NULL);
   }
}

static u32 rle_pack(u8 *dst, const u8 *src, u32 len) {
   /* Runs of zeros: 00 count(4); anything else as is (a 00 alone: 00 01). */
   u32 i = 0, o = 0;
   while (i < len) {
      if (!src[i]) {
         u32 n = 1;
         while (i + n < len && !src[i + n] && n < 0xFFFFFFFFu)
            n++;
         dst[o++] = 0;
         dst[o++] = n >> 24; dst[o++] = n >> 16; dst[o++] = n >> 8; dst[o++] = n;
         i += n;
      }
      else
         dst[o++] = src[i++];
   }
   return o;
}

static bool rle_unpack(u8 *dst, u32 len, const u8 *src, u32 slen) {
   u32 i = 0, o = 0;
   while (i < slen) {
      if (!src[i]) {
         u32 n;
         if (slen - i < 5)
            return false;
         n = ((u32)src[i + 1] << 24) | ((u32)src[i + 2] << 16) |
             ((u32)src[i + 3] << 8) | src[i + 4];
         if (n > len - o)
            return false;
         memset(&dst[o], 0, n);
         o += n;
         i += 5;
      }
      else {
         if (o >= len)
            return false;
         dst[o++] = src[i++];
      }
   }
   return o == len;
}

static void free_blobs(void) {
   free(ds.own_blob);
   free(ds.own_pack);
   free(ds.peer_pack);
   ds.own_blob = ds.own_pack = ds.peer_pack = NULL;
}

/* The cartridge clock while linked: the same for every copy of a Game Boy,
 * from emulated time (seconds since the host's clock at the start). */
static uint64_t session_clock(void) {
   return ds.clock_base + (u64)ds.frame * 70224 / 4194304;
}

/* Hash of a Game Boy: its whole state, but for what only the picture
 * uses (the DMG palette colours, from the display options of each
 * console). The state is: version (2 bytes), thumbnail (24-bit size and
 * data), then fields: label, NUL, 24-bit size, data. */
static u32 hash_state(const u8 *buf, size_t size) {
   u32 h = 2166136261u;
   size_t i = 2, len;
   if (size < 5)
      return 0;
   len = ((size_t)buf[i] << 16) | ((size_t)buf[i + 1] << 8) | buf[i + 2];
   i += 3 + len;
   while (i < size) {
      const u8 *label = &buf[i];
      while (i < size && buf[i])
         i++;
      if (size - i < 4)
         break;
      len = ((size_t)buf[i + 1] << 16) | ((size_t)buf[i + 2] << 8) | buf[i + 3];
      i += 4;
      if (len > size - i)
         break;
      if (strcmp((const char*)label, "dmgpal")) {
         h = fnv(h, label, strlen((const char*)label));
         h = fnv(h, &buf[i], len);
      }
      i += len;
   }
   return h;
}

/* Hash of a Game Boy. */
static u32 hash_gb(gambatte::GB *gb) {
   static u8 *buf;
   static size_t buf_size;
   const size_t size = gb->stateSize();
   if (size > buf_size) {
      free(buf);
      buf = (u8*)malloc(size);
      buf_size = buf ? size : 0;
   }
   if (!buf)
      return 0;
   memset(buf, 0, size);
   gb->saveState(buf);
   return hash_state(buf, size);
}

static gambatte::GB *slot_gb(unsigned s) {
   return s == ds.my_slot ? ds.local : ds.peer;
}

/* ---- session ------------------------------------------------------------- */

static void dual_stop(const char *why) {
   unsigned s;
   if (ds.state == DS_OFF)
      return;
   if (why)
      dual_msg(why);
   ds.state = DS_OFF;
   free_blobs();
   if (ds.loaded) {
      /* The player's Game Boy goes on alone, as it was. */
      gambatte::setTimeSource(NULL);
      ds.local->setInputGetter(fe.input);
      ds.local->setSerialIO(NULL);
      for (s = 0; s < NP; s++)
         ds.end[s].clear();
      ds.loaded = false;
   }
}

static void give_up(const char *why) {
   const bool differ = why && !strcmp(why, MSG_DIFFER);
   if (ds.is_host) {
      if (ds.cand >= 0)
         dual_send(ds.cand, DPK_GIVEUP, NULL, 0, NULL, 0);
   }
   else if (ds.state == DS_HELLO || ds.state == DS_WAIT)
      dual_send(0, differ ? DPK_GIVEUP : DPK_BYE, NULL, 0, NULL, 0);
   dual_stop(NULL);
   ds.gave_up = true;
   /* Both consoles have this core, with other games: the real time cable. */
   if (differ)
      rt_start(ds.is_host ? (unsigned)ds.cand : 0);
   if (why)
      dual_msg(why);
}

static void send_hello(unsigned to) {
   u32 w[4];
   w[0] = DUAL_VERSION;
   w[1] = ds.rom_crc;
   w[2] = ds.cfg;
   w[3] = ds.is_host ? 1 : 0;
   dual_send(to, DPK_HELLO, w, 4, NULL, 0);
   ds.hello_sent = true;
   ds.t_hello = fe.time_usec();
}

static bool hello_matches(const u32 *w) {
   return w[0] == DUAL_VERSION && w[1] == ds.rom_crc && w[2] == ds.cfg &&
          w[3] == (ds.is_host ? 0u : 1u);
}

static void send_ping(void) {
   const u64 now = fe.time_usec();
   u32 w[2] = { (u32)now, (u32)(now >> 32) };
   dual_send(ds.slot_id[1], DPK_PING, w, 2, NULL, 0);
   ds.ping_out = true;
   ds.t_ping = now;
}

static u32 rtt_min(void) {
   u32 i, m = ~0u;
   for (i = 0; i < ds.rtt_n && i < PINGS; i++)
      m = ds.rtt[i] < m ? ds.rtt[i] : m;
   return ds.rtt_n ? m : 0;
}

/* The other player's Game Boy, with this very game. */
static bool load_peer(void) {
   if (ds.peer_loaded)
      return true;
   if (!ds.peer)
      ds.peer = new gambatte::GB();
   ds.peer->setBootloaderGetter(NULL);   /* It never runs one (see reset_all) */
   if (ds.peer->load(ds.rom, ds.rom_size, ds.flags) != 0)
      return false;
   ds.peer_loaded = true;
   return true;
}

/* Our Game Boy starts over, and the copy sent to the other console is made
 * of it then (without the boot ROM, which the state does not hold: the
 * other console may have another one, or none). */
static bool capture(void) {
   ds.local->setBootloaderGetter(NULL);
   ds.local->reset();
   ds.local->setBootloaderGetter(fe.bootloader);
   ds.blob_size = ds.local->stateSize();
   ds.own_blob = (u8*)calloc(1, ds.blob_size);
   ds.own_pack = (u8*)malloc(ds.blob_size * 5 + 16);
   if (!ds.own_blob || !ds.own_pack)
      return false;
   ds.local->saveState(ds.own_blob);
   ds.own_pack_len = rle_pack(ds.own_pack, ds.own_blob, ds.blob_size);
   return true;
}

static void send_blob(void) {
   u32 off;
   for (off = 0; off < ds.own_pack_len; off += CHUNK) {
      u32 w[2] = { off, ds.own_pack_len };
      const u32 len = ds.own_pack_len - off < CHUNK ? ds.own_pack_len - off : CHUNK;
      dual_send_peer(DPK_STATE, w, 2, &ds.own_pack[off], len);
   }
   free(ds.own_pack);
   ds.own_pack = NULL;
}

static bool have_state(void) {
   return ds.peer_pack_len && ds.peer_rcvd >= ds.peer_pack_len;
}

/* Both Game Boys start from the states that were exchanged: ours from the
 * copy we sent (so that both copies of it start the same way). */
static bool load_machines(void) {
   u8 *blob;
   unsigned s;
   if (!load_peer())
      return false;
   if (!ds.local->loadState(ds.own_blob, ds.blob_size))
      return false;
   blob = (u8*)malloc(ds.blob_size);
   if (!blob)
      return false;
   if (!rle_unpack(blob, ds.blob_size, ds.peer_pack, ds.peer_pack_len) ||
       !ds.peer->loadState(blob, ds.blob_size)) {
      free(blob);
      return false;
   }
   free(blob);

   for (s = 0; s < NP; s++) {
      gambatte::GB *gb = slot_gb(s);
      ds.end[s].gb = gb;
      ds.end[s].peer = &ds.end[s ^ 1];
      ds.end[s].clear();
      ds.keys_in[s].keys = 0;
      ds.count[s] = 0;
      gb->setSerialIO(&ds.end[s]);
      gb->setInputGetter(&ds.keys_in[s]);
   }
   free_blobs();
   ds.loaded = true;
   return true;
}

static void session_reset(void) {
   const u64 now = fe.time_usec();
   free_blobs();
   ds.gave_up = false;
   ds.cand = -1;
   ds.cand_hello = false;
   ds.early_hello = ds.early_differ = 0;
   ds.hello_sent = ds.go = ds.peer_ready = false;
   ds.peer_pack_len = ds.peer_rcvd = 0;
   ds.rtt_n = 0;
   ds.ping_out = false;
   ds.ow_us = 0;
   memset(ds.frame_ok, 0, sizeof(ds.frame_ok));
   memset(ds.my_hash, 0, sizeof(ds.my_hash));
   memset(ds.peer_hash, 0, sizeof(ds.peer_hash));
   ds.hash_done = ds.hash_bad = 0;
   ds.peer_pos_n = 0;
   ds.peer_lead = 0;
   ds.slot_id[0] = 0;
   ds.slot_id[1] = -1;
   ds.t_heard = now;
   ds.t_state = now;
}

static void start_running(void) {
   u32 i;
   char msg[96];
   ds.state = DS_RUN;
   ds.frame = 0;
   /* Nobody presses anything during the first frames (the delay). */
   for (i = 0; i < ds.delay; i++) {
      ds.own_keys[i] = ds.peer_keys[i] = 0;
      ds.frame_ok[i] = i + 1;
   }
   ds.sent_upto = ds.delay;
   ds.send_delay = ds.delay;
   ds.stalls = ds.stall_frame = ds.peer_stalls = 0;
   ds.t_peer_stall = ds.t_heard = fe.time_usec();
   ds.t_lowered = 0;
   ds.calm_us = DELAY_CALM_US;
   ds.sync_next = SYNC_GAP;
   ds.reset_wanted = false;
   gambatte::setTimeSource(session_clock);
   snprintf(msg, sizeof(msg), "Cable Link: both Game Boys connected (%u frames of delay)",
            (unsigned)ds.delay);
   dual_msg(msg);
}

/* Input delay from the round trip: the buttons must get to the other
 * console before it needs them. The time sync keeps the consoles within
 * SYNC_US of each other; on top of that the network takes one way, and a
 * frame more covers it having a bad moment. */
static u32 pick_delay(void) {
   const u32 d = (ds.ow_us + SYNC_US + FRAME_US + FRAME_US - 1) / FRAME_US;
   return d < 2 ? 2 : d > 30 ? 30 : d;
}

static void begin_transfer(void) {
   if (!load_peer() || !capture()) {
      dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: could not start the other Game Boy");
      return;
   }
   ds.state = DS_TRANSFER;
   send_blob();
}

static void host_start(void) {
   u32 w[NP];
   ds.my_slot = 0;
   ds.slot_id[0] = 0;
   ds.slot_id[1] = ds.cand;
   w[0] = 0;
   w[1] = (u32)ds.cand;
   ds.state = DS_PING;
   ds.t_state = fe.time_usec();
   dual_send(ds.cand, DPK_START, w, NP, NULL, 0);
   send_ping();
}

static void setup_step(void) {
   const u64 now = fe.time_usec();

   if (ds.state == DS_HELLO) {
      /* Client. The host may not be listening yet (it only is once the
       * frontend tells it about us): say it again until it answers. */
      if (!ds.hello_sent || now - ds.t_hello > HELLO_EVERY_US)
         send_hello(0);
      if (now - ds.t_state > HELLO_US)
         give_up(NULL);         /* The host does not have this core */
      return;
   }

   if (ds.state == DS_WAIT) {
      if (!ds.is_host || ds.cand < 0)
         return;                /* Waiting (the game goes on) */
      if (ds.cand_hello)
         host_start();
      else if (now - ds.t_join > HELLO_US)
         give_up("Cable Link: the other console does not have this core, using the real time cable");
      return;
   }

   if (ds.state == DS_PING) {
      if (ds.is_host) {
         if (ds.rtt_n < PINGS && (!ds.ping_out || now - ds.t_ping > PONG_US))
            send_ping();
         if (ds.rtt_n >= PINGS || now - ds.t_state > PING_US) {
            ds.ow_us = rtt_min() / 2;
            ds.delay = pick_delay();
            dual_send_peer(DPK_GO, NULL, 0, NULL, 0);
            begin_transfer();
         }
      }
      else if (ds.go)
         begin_transfer();
   }

   if (ds.state == DS_TRANSFER && have_state()) {
      if (!load_machines()) {
         dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
         dual_stop("Cable Link: could not start the other Game Boy");
         return;
      }
      if (ds.is_host) {
         u32 w[4];
         ds.clock_base = gambatte::timeNow();
         w[0] = ds.delay;
         w[1] = ds.ow_us;
         w[2] = (u32)ds.clock_base;
         w[3] = (u32)(ds.clock_base >> 32);
         dual_send_peer(DPK_READY, w, 4, NULL, 0);
      }
      ds.state = DS_READY;
   }

   if (ds.state == DS_READY && (ds.is_host || ds.peer_ready))
      start_running();

   if (ds.state >= DS_PING && ds.state < DS_RUN && now - ds.t_state > SETUP_US) {
      dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: the other console did not answer");
   }
}

/* ---- frontend interface ---------------------------------------------------- */

void init(const Frontend &f) {
   fe = f;
}

void game_loaded(gambatte::GB *local, const void *rom, size_t size,
                 unsigned flags, uint32_t cfg) {
   game_unloaded();
   ds.local = local;
   ds.rom = (const u8*)malloc(size);
   if (ds.rom)
      memcpy((void*)ds.rom, rom, size);
   ds.rom_size = ds.rom ? size : 0;
   ds.flags = flags;
   ds.rom_crc = fnv(2166136261u ^ (u32)size, rom, size);
   ds.cfg = cfg;
   find_block_routine();
}

void game_unloaded(void) {
   dual_stop(NULL);
   rt_stop();
   delete ds.peer;
   ds.peer = NULL;
   ds.peer_loaded = false;
   free((void*)ds.rom);
   ds.rom = NULL;
   ds.rom_size = 0;
   ds.local = NULL;
}

/* The settings, again, as a netplay session starts: the other Game Boy is
 * loaded again if they changed (it takes them when it is loaded). */
void set_cfg(uint32_t cfg) {
   if (cfg == ds.cfg)
      return;
   dual_stop(NULL);
   ds.cfg = cfg;
   ds.peer_loaded = false;
}

void session_start(unsigned client_id) {
   dual_stop(NULL);
   rt_stop();
   session_reset();
   ds.is_host = (client_id == 0);
   ds.my_id = client_id;
   ds.my_slot = ds.is_host ? 0 : 1;
   if (!ds.local || !ds.rom) {
      ds.state = DS_OFF;
      ds.gave_up = true;
      return;
   }
   if (ds.is_host)
      ds.state = DS_WAIT;
   else {
      ds.state = DS_HELLO;
      send_hello(0);
   }
   load_peer();   /* (if it fails, the setup says so) */
   ds.t_state = fe.time_usec();
}

void session_stop(void) {
   dual_stop(NULL);
   rt_stop();
}

bool peer_connected(unsigned client_id) {
   const u64 bit = client_id < 64 ? 1ull << client_id : 0;
   if (!ds.is_host || ds.gave_up || ds.state == DS_OFF)
      return true;
   if (ds.state != DS_WAIT || ds.cand >= 0)
      return false;               /* The cable has two ends */
   ds.cand = (int)client_id;
   if (ds.early_differ & bit) {
      /* Its HELLO came first and the games differ: tell it now. */
      ds.early_differ &= ~bit;
      give_up(MSG_DIFFER);
      return true;
   }
   ds.cand_hello = (ds.early_hello & bit) != 0;
   ds.early_hello &= ~bit;
   ds.t_join = fe.time_usec();
   return true;
}

void peer_disconnected(unsigned client_id) {
   if (!ds.is_host || ds.state == DS_OFF)
      return;
   if (ds.state == DS_WAIT) {
      if (client_id < 64) {
         ds.early_hello &= ~(1ull << client_id);
         ds.early_differ &= ~(1ull << client_id);
      }
      if (ds.cand == (int)client_id) {
         ds.cand = -1;
         ds.cand_hello = false;
      }
      return;
   }
   if (ds.slot_id[1] == (int)client_id)
      dual_stop(ds.state == DS_RUN ? "Cable Link: the other console left" : NULL);
}

bool setting_up(void) {
   const u64 t0 = fe.time_usec();
   if (ds.state == DS_WAIT) {
      setup_step();
      if (ds.state == DS_WAIT || ds.state == DS_OFF)
         return false;
   }
   while (ds.state != DS_OFF && ds.state != DS_RUN && ds.state != DS_WAIT) {
      setup_step();
      if (ds.state == DS_OFF || ds.state == DS_RUN || ds.state == DS_WAIT ||
          fe.time_usec() - t0 > SLICE_US)
         break;
      fe.poll();
      usleep(250);
   }
   return ds.state != DS_OFF && ds.state != DS_RUN && ds.state != DS_WAIT;
}

bool running(void) {
   return ds.state == DS_RUN;
}

bool pending(void) {
   return ds.state != DS_OFF && ds.state != DS_RUN;
}

bool realtime(void) {
   return ds.rt;
}

void realtime_tick(void) {
   u64 now;
   if (!ds.rt || !ds.local)
      return;
   now = rt_now();
   ds.rt_cable.hold_expired();
   /* Alive (and whether we have the block routine), once a second. */
   if (now - ds.rt_alive > RT_ALIVE_US) {
      u32 w[1] = { ds.blk_pc ? 1u : 0u };
      ds.rt_alive = now;
      rt_send(DPK_ALIVE, w, 1);
   }
   if (now - ds.rt_heard > RT_PEER_US) {
      rt_stop();
      dual_msg("Cable Link: the other console stopped answering");
   }
}

bool realtime_wait(void) {
   u64 now;
   s32 lead;
   if (!ds.rt || !ds.local || !ds.rt_peer_frames_on || !ds.rt_anchored || ds.rt_wait_off)
      return false;
   /* A byte to answer, or one of ours on its way: run. */
   if (ds.rt_cable.holding() || ds.rt_cable.awaiting || ds.local->serialAwaiting() ||
       ds.local->blockAwaiting())
      return false;
   now = rt_now();
   lead = (s32)(ds.rt_frames - ds.rt_peer_frames) - ds.rt_anchor;
   if ((s32)(ds.rt_frames - ds.rt_sync_own) > RT_ACTIVE || lead <= RT_SLACK ||
       now - ds.rt_peer_frames_at > RT_FRESH_US) {
      ds.rt_wait_since = 0;
      return false;
   }
   if (!ds.rt_wait_since)
      ds.rt_wait_since = now;
   else if (now - ds.rt_wait_since > RT_WAIT_MAX_US) {
      ds.rt_wait_off = true;
      ds.rt_wait_since = 0;
      return false;
   }
   return true;
}

void realtime_frame_done(void) {
   if (!ds.rt)
      return;
   ds.rt_frames++;
   if (ds.rt_anchored && (s32)(ds.rt_frames - ds.rt_sync_own) <= RT_ACTIVE) {
      u32 w[1] = { ds.rt_frames };
      rt_send(DPK_TICK, w, 1);
   }
}

void request_reset(void) {
   if (ds.state == DS_RUN)
      ds.reset_wanted = true;
}

/* How far ahead of the peer we are (us, at `now`). */
static s32 lead(u64 now) {
   s64 best;
   u32 i;
   if (!ds.peer_pos_n)
      return 0;
   best = ds.peer_pos[0];
   for (i = 1; i < ds.peer_pos_n && i < SYNC_WIN; i++)
      best = ds.peer_pos[i] > best ? ds.peer_pos[i] : best;
   return (s32)((s64)ds.frame * FRAME_US - (s64)now - best);
}

/* Without the boot ROM: each console has its own (or none, or has it
 * off), and both must compute the very same machines. */
static void reset_all(void) {
   unsigned s;
   for (s = 0; s < NP; s++) {
      slot_gb(s)->setBootloaderGetter(NULL);
      slot_gb(s)->reset();
      ds.end[s].clear();
   }
   ds.local->setBootloaderGetter(fe.bootloader);
}

int frame_begin(unsigned local_keys) {
   const u32 f = ds.frame;
   const u32 fi = f % RING;
   const u64 t0 = fe.time_usec();
   const s32 my_lead = lead(t0);
   const s32 ahead = (my_lead - ds.peer_lead) / 2;

   /* Ahead of the other console (both work it out the same way, so only one
    * of the two can be): stay a frame where we are. */
   if (f >= ds.sync_next && ds.peer_pos_n >= SYNC_WIN && ahead > SYNC_US) {
      ds.sync_next = f + SYNC_GAP;
      return FRAME_HOLD;
   }

   /* The other console waited for our buttons: they go out earlier. Back
    * down once all is calm. */
   if (ds.send_delay > ds.delay && t0 - ds.t_peer_stall > ds.calm_us) {
      ds.send_delay--;
      ds.t_peer_stall = ds.t_lowered = t0;
   }

   /* Ours for frame f + delay. */
   while (ds.sent_upto <= f + ds.send_delay) {
      u32 w[5];
      w[0] = ds.sent_upto;
      w[1] = (local_keys & 0xFF) | (ds.reset_wanted ? KEY_RESET : 0);
      ds.reset_wanted = false;
      w[2] = f;
      w[3] = (u32)my_lead;
      w[4] = ds.stalls;
      ds.own_keys[ds.sent_upto % RING] = w[1];
      dual_send_peer(DPK_INPUT, w, 5, NULL, 0);
      ds.sent_upto++;
   }

   /* The other one's for frame f. */
   while (ds.frame_ok[fi] != f + 1) {
      const u64 now = fe.time_usec();
      if (ds.state != DS_RUN)
         return FRAME_WAIT;
      if (now - ds.t_heard > PEER_US) {
         dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
         dual_stop("Cable Link: the other console stopped answering");
         return FRAME_WAIT;
      }
      if (now - t0 > STALL_US) {
         if (ds.stall_frame != f + 1) {
            ds.stall_frame = f + 1;
            ds.stalls++;
         }
         return FRAME_WAIT;
      }
      fe.poll();
      usleep(250);
   }

   /* A player pressed reset: both consoles restart both games in this very
    * frame. */
   if ((ds.own_keys[fi] | ds.peer_keys[fi]) & KEY_RESET)
      reset_all();

   ds.keys_in[ds.my_slot].keys = ds.own_keys[fi] & ~KEY_RESET;
   ds.keys_in[ds.my_slot ^ 1].keys = ds.peer_keys[fi] & ~KEY_RESET;
   return FRAME_RUN;
}

void run_frame(gambatte::video_pixel_t *video, int pitch,
               void (*audio)(const int16_t *samples, unsigned frames)) {
   static gambatte::video_pixel_t work[NP][160 * 144];
   static gambatte::uint_least32_t sound[SLICE_SAMPLES + SOUND_SPARE];
   u32 target = 0;
   unsigned s;

   while (target < FRAME_SAMPLES) {
      target = target + SLICE_SAMPLES < FRAME_SAMPLES ? target + SLICE_SAMPLES : FRAME_SAMPLES;
      for (s = 0; s < NP; s++) {
         gambatte::GB *gb = slot_gb(s);
         while (ds.count[s] < target) {
            unsigned n = target - ds.count[s];
            const long blit = gb->runFor(work[s], 160, sound,
                                         sizeof(sound) / sizeof(sound[0]), n);
            if (s == ds.my_slot) {
               audio((const int16_t*)sound, n);
               if (blit >= 0) {
                  unsigned y;
                  for (y = 0; y < 144; y++)
                     memcpy(&video[y * pitch], &work[s][y * 160],
                            160 * sizeof(gambatte::video_pixel_t));
               }
            }
            ds.count[s] += n;
         }
      }
   }
   /* (What a Game Boy ran past the frame counts for the next one.) */
   for (s = 0; s < NP; s++)
      ds.count[s] -= FRAME_SAMPLES;
}

/* Both copies of each Game Boy must be the same: the link is cut when two
 * checks in a row differ. */
static void check_hash(u32 f) {
   const u32 i = (f / HASH_EVERY) % HASH_SLOTS;
   const u32 *mine = ds.my_hash[i], *theirs = ds.peer_hash[i];
   unsigned g;
   if (mine[0] != f + 1 || theirs[0] != f + 1 || ds.hash_done >= f + 1)
      return;
   ds.hash_done = f + 1;
   for (g = 0; g < NP; g++)
      if (mine[1 + g] != theirs[1 + g])
         break;
   if (g == NP)
      return;
   if (f >= HASH_EVERY && ds.hash_bad == f + 1 - HASH_EVERY) {
      dual_send_peer(DPK_BYE, NULL, 0, NULL, 0);
      dual_stop("Cable Link: the consoles went out of sync");
      return;
   }
   ds.hash_bad = f + 1;
}

void frame_end(void) {
   const u32 f = ds.frame++;
   if (f % HASH_EVERY == 0) {
      u32 w[1 + NP], *mine = ds.my_hash[(f / HASH_EVERY) % HASH_SLOTS];
      unsigned s;
      w[0] = f;
      mine[0] = f + 1;
      for (s = 0; s < NP; s++)
         mine[1 + s] = w[1 + s] = hash_gb(slot_gb(s));
      dual_send_peer(DPK_HASH, w, 1 + NP, NULL, 0);
      check_hash(f);
   }
}

bool net_receive(const void *buf, size_t len, uint16_t client_id) {
   const u32 *pkt = (const u32*)buf;
   const u64 now = fe.time_usec();
   u32 type, from;
   if (len < 8 || be32(pkt[0]) != DUAL_HEADER)
      return false;

   type = be32(pkt[1]) & 0xFF;
   if (type >= DPK_XFER) {
      /* The real time cable (only with the console it runs with). */
      RtCable &c = ds.rt_cable;
      if (!ds.rt || client_id != ds.rt_peer || len < 12)
         return true;
      ds.rt_heard = now;
      if (type == DPK_XFER && len >= 8 + 3 * 4) {
         const bool framed = len >= 8 + 4 * 4;
         c.hold(be32(pkt[2]), (unsigned char)be32(pkt[3]), be32(pkt[4]) != 0,
                framed, framed ? be32(pkt[5]) : 0,
                ds.local->serialWaiting(), ds.local->serialData());
         if (framed)
            rt_peer_frame(be32(pkt[5]));
      }
      else if (type == DPK_REPLY && len >= 8 + 2 * 4) {
         const bool framed = len >= 8 + 3 * 4;
         c.answered(be32(pkt[2]), (unsigned char)be32(pkt[3]), framed,
                    framed ? be32(pkt[4]) : 0);
      }
      else if (type == DPK_TICK && len >= 8 + 4) {
         rt_peer_frame(be32(pkt[2]));
      }
      else if (type == DPK_CANCEL) {
         c.unhold(be32(pkt[2]));
      }
      else if (type == DPK_BLOCK && len > 8 + 3 * 4) {
         const u32 seq = be32(pkt[2]), total = be32(pkt[3]), off = be32(pkt[4]);
         const u32 dlen = (u32)len - (8 + 3 * 4);
         if (total <= RT_BLOCK_MAX && off <= total && dlen <= total - off) {
            RtCable::PeerBlock &p = c.peer_blk[seq & 1];
            if (seq != p.seq) {
               p.seq = seq;
               p.len = total;
               p.rcvd = 0;
            }
            memcpy(&p.data[off], &pkt[5], dlen);
            p.rcvd += dlen;
         }
      }
      else if (type == DPK_ALIVE && len >= 8 + 4) {
         /* Both have the block routine: blocks on (never again after one
          * went unanswered). */
         if (be32(pkt[2]) && ds.blk_pc && !ds.rt_blocks && !ds.rt_blocks_failed) {
            ds.rt_blocks = true;
            ds.local->setBlockHook(ds.blk_pc, ds.blk_flag, &c);
         }
      }
      return true;
   }
   /* The client says HELLO again after we went for the real time cable:
    * our answer was lost, it goes again. */
   if (type == DPK_HELLO && ds.is_host && ds.rt && client_id == ds.rt_peer) {
      dual_send(client_id, DPK_GIVEUP, NULL, 0, NULL, 0);
      return true;
   }
   if (ds.state == DS_OFF)
      return true;

   from = (be32(pkt[1]) >> 8) & 0xFF;
   if (type == DPK_START)
      from = NO_SLOT;
   /* Packets with a slot: only from the other console of this session. */
   if (from != NO_SLOT) {
      if (ds.state < DS_PING || from >= NP || from == ds.my_slot ||
          ds.slot_id[from] != (int)client_id)
         return true;
      ds.t_heard = now;
   }

   switch (type) {
   case DPK_HELLO:
      if (len >= 8 + 4 * 4) {
         u32 w[4], i;
         for (i = 0; i < 4; i++)
            w[i] = be32(pkt[2 + i]);
         if (ds.is_host && ds.state == DS_WAIT) {
            if (!hello_matches(w)) {
               /* Other games: the real time cable, once it joined (what we
                * send to it before is dropped by the frontend). */
               if (ds.cand == (int)client_id)
                  give_up(MSG_DIFFER);
               else if (client_id < 64)
                  ds.early_differ |= 1ull << client_id;
               break;
            }
            /* Answer right away, even if the frontend has not told us
             * about it yet: otherwise it would think we lack this core. */
            send_hello(client_id);
            if (ds.cand == (int)client_id)
               ds.cand_hello = true;
            else if (client_id < 64)
               ds.early_hello |= 1ull << client_id;
         }
         else if (!ds.is_host && ds.state == DS_HELLO && client_id == 0) {
            if (!hello_matches(w))
               give_up(MSG_DIFFER);
            else
               ds.state = DS_WAIT;
         }
      }
      break;

   case DPK_START:
      if (len >= 8 + NP * 4 && !ds.is_host && client_id == 0 &&
          (ds.state == DS_WAIT || ds.state == DS_HELLO)) {
         ds.slot_id[0] = (int)be32(pkt[2]);
         ds.slot_id[1] = (int)be32(pkt[3]);
         if (ds.slot_id[1] != (int)ds.my_id) {
            give_up(NULL);
            break;
         }
         ds.my_slot = 1;
         ds.t_heard = now;
         ds.state = DS_PING;
         ds.t_state = now;
      }
      break;

   case DPK_PING:
      if (len >= 16) {
         u32 w[2] = { be32(pkt[2]), be32(pkt[3]) };
         dual_send(client_id, DPK_PONG, w, 2, NULL, 0);
      }
      break;

   case DPK_PONG:
      if (len >= 16 && ds.is_host && from == 1) {
         const u64 t = be32(pkt[2]) | ((u64)be32(pkt[3]) << 32);
         ds.rtt[ds.rtt_n % PINGS] = (u32)(now - t);
         ds.rtt_n++;
         ds.ping_out = false;
      }
      break;

   case DPK_GO:
      if (!ds.is_host && from == 0)
         ds.go = true;
      break;

   case DPK_STATE:
      if (len > 16 && from != NO_SLOT && ds.state >= DS_PING && ds.state <= DS_TRANSFER) {
         const u32 off = be32(pkt[2]);
         const u32 total = be32(pkt[3]);
         const u32 dlen = (u32)len - 16;
         const u32 max = ds.local ? (u32)ds.local->stateSize() * 5 + 16 : 0;
         if (!ds.peer_pack && total <= max)
            ds.peer_pack = (u8*)malloc(max);
         /* (Checked so that no sum can wrap around.) */
         if (ds.peer_pack && total <= max && off <= total && dlen <= total - off) {
            memcpy(&ds.peer_pack[off], &pkt[4], dlen);
            ds.peer_pack_len = total;
            ds.peer_rcvd += dlen;
         }
      }
      break;

   case DPK_READY:
      if (len >= 8 + 4 * 4 && !ds.is_host && from == 0) {
         const u32 d = be32(pkt[2]);
         ds.delay = d < 1 ? 1 : d > 30 ? 30 : d;
         ds.ow_us = be32(pkt[3]);
         ds.clock_base = be32(pkt[4]) | ((u64)be32(pkt[5]) << 32);
         ds.peer_ready = true;
      }
      break;

   case DPK_INPUT:
      if (len >= 8 + 5 * 4 && from != NO_SLOT && ds.state >= DS_TRANSFER) {
         const u32 f = be32(pkt[2]);
         const u32 their_stalls = be32(pkt[6]);
         ds.peer_keys[f % RING] = be32(pkt[3]);
         ds.frame_ok[f % RING] = f + 1;
         /* It was starting this frame one way ago. */
         ds.peer_pos[ds.peer_pos_n++ % SYNC_WIN] =
            (s64)be32(pkt[4]) * FRAME_US + ds.ow_us - (s64)now;
         ds.peer_lead = (s32)be32(pkt[5]);
         /* It waited for our buttons: they go out a frame earlier. */
         if (their_stalls != ds.peer_stalls && ds.state == DS_RUN) {
            ds.peer_stalls = their_stalls;
            ds.send_delay = ds.send_delay + 1 < ds.delay + DELAY_MORE ?
                            ds.send_delay + 1 : ds.delay + DELAY_MORE;
            if (ds.t_lowered && now - ds.t_lowered < ds.calm_us)
               ds.calm_us = ds.calm_us * 2 < DELAY_CALM_MAX ? ds.calm_us * 2 : DELAY_CALM_MAX;
            ds.t_peer_stall = now;
         }
      }
      break;

   case DPK_HASH:
      if (len >= 8 + (1 + NP) * 4 && from != NO_SLOT && ds.state == DS_RUN) {
         const u32 f = be32(pkt[2]);
         u32 *theirs = ds.peer_hash[(f / HASH_EVERY) % HASH_SLOTS];
         unsigned s;
         theirs[0] = f + 1;
         for (s = 0; s < NP; s++)
            theirs[1 + s] = be32(pkt[3 + s]);
         check_hash(f);
      }
      break;

   case DPK_GIVEUP:
      if (!ds.is_host && client_id == 0) {
         dual_stop(NULL);
         ds.gave_up = true;
         rt_start(0);
         dual_msg(MSG_DIFFER);
      }
      else if (ds.is_host && ds.state == DS_WAIT) {
         dual_stop(NULL);
         ds.gave_up = true;
         rt_start(client_id);
         dual_msg(MSG_DIFFER);
      }
      break;

   case DPK_BYE:
      if (ds.is_host && ds.state == DS_WAIT && from == NO_SLOT) {
         /* The client cannot take part: the real time cable. */
         dual_stop(NULL);
         ds.gave_up = true;
         dual_msg(MSG_DIFFER);
         break;
      }
      if (from != NO_SLOT || client_id == 0)
         dual_stop(ds.state == DS_RUN ? "Cable Link: the other console left" : NULL);
      break;
   }
   return true;
}

void shutdown(void) {
   game_unloaded();
}

}
