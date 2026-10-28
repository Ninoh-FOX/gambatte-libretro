/* Dual Cable Link: both Game Boys of a link session run on each of the two
 * consoles, joined by a link cable in memory, and only the buttons travel
 * over the network (RetroArch's netpacket interface). The link never waits
 * for the WiFi, however many bytes a game sends.
 *
 * Slot 0 is the netplay host's Game Boy, slot 1 the client's. On each
 * console one of them is the player's own (the one seen and heard) and the
 * other a hidden copy of the other player's. Both consoles compute the very
 * same machines, so everything here must depend on emulated time and on the
 * buttons only, never on how fast a console runs. Ported from gpSP's dual
 * cable (dual.c), for two players. */

#ifndef GAMBATTE_DUAL_H
#define GAMBATTE_DUAL_H

#include <stddef.h>
#include <stdint.h>
#include <libretro.h>
#include <gambatte.h>

namespace dual {

enum { FRAME_WAIT = 0, FRAME_RUN, FRAME_HOLD };

/* The core (libretro.cpp) gives these. */
struct Frontend {
   /* netpacket: send to a client id (host: 0), and read what came. */
   void (*send)(uint16_t to, const void *buf, size_t len);
   void (*poll)(void);
   uint64_t (*time_usec)(void);          /* monotonic, wall clock */
   void (*message)(const char *text);
   /* The player's Game Boy's boot ROM getter (the link resets it without
    * one, and puts this back after). */
   bool (*bootloader)(void *userdata, bool isgbc, uint8_t *data, uint32_t size);
   /* The player's Game Boy is restored to these when the link ends. */
   gambatte::InputGetter *input;
};

void init(const Frontend &fe);
/* A game was loaded into `local` (with these load flags). `cfg` is
 * everything else that makes the machine different (see machine_cfg in
 * libretro.cpp). */
void game_loaded(gambatte::GB *local, const void *rom, size_t size,
                 unsigned flags, uint32_t cfg);
void game_unloaded(void);
void set_cfg(uint32_t cfg);

/* Netplay session (Cable Link mode). */
void session_start(unsigned client_id);
void session_stop(void);
bool peer_connected(unsigned client_id);
void peer_disconnected(unsigned client_id);
bool net_receive(const void *buf, size_t len, uint16_t client_id);

/* Whether the link is being set up (the game waits), running, or may still
 * start in this session. */
bool setting_up(void);
bool running(void);
bool pending(void);
/* The games differ: the real time cable is on (only the bytes go over the
 * network; read it often). */
bool realtime(void);
/* Real time cable: call often (every bit of a frame), it keeps the link
 * alive and notices when the other console is gone. */
void realtime_tick(void);
/* Real time cable: at the start of a frame, whether to wait instead (we
 * are ahead of the other console while bytes go: keep asking, a byte may
 * come; see rt_sync_point in dual.cpp); and each frame the Game Boy ran. */
bool realtime_wait(void);
void realtime_frame_done(void);

/* A frame while running: frame_begin says whether to run it (the other
 * console's buttons are here, and we are not ahead of it), run_frame runs
 * both Game Boys (the player's picture goes to `video`, its sound to
 * `audio`), frame_end finishes it. */
int frame_begin(unsigned local_keys);
void run_frame(gambatte::video_pixel_t *video, int pitch,
               void (*audio)(const int16_t *samples, unsigned frames));
void frame_end(void);
void request_reset(void);

void shutdown(void);

}

#endif
