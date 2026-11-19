/* gbtest: the standalone's Game Boy (standalone/adapter) without SDL, to
 * test it under qemu-arm: frames, audio, saves, states and the Cable Link.
 *   gbtest solo ROM FRAMES DIR | host ROM FRAMES DIR | join IP ROM FRAMES DIR
 * DIR: saves and screens (DIR/NNNNN.ppm, SHOT_EVERY / SHOT_AT).
 * KEYS="from-to:key+key;..." (a b select start right left up down), frames
 * counted from the link-up (from the start with solo).
 * STATE_SAVE_AT=n saves state 0 at frame n, STATE_LOAD_AT=n loads it.
 * MENU_AT=n MENU_MS=t: at frame n the menu is open for t ms, as the real
 * menu does it (GB::menuRunBegin, menuDelay, menuRunEnd). */
#include <gambatte.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include "net.h"

extern "C" {
int biosenabled = 0;
std::string homedir = ".";
void printOverlay(const char *t) { printf("OVERLAY %s\n", t); fflush(stdout); }
}

static unsigned sframe;
class In : public gambatte::InputGetter {
public:
   virtual unsigned operator()() {
      static const char *n[8] = { "a", "b", "select", "start", "right", "left", "up", "down" };
      const char *k = getenv("KEYS"); unsigned m = 0;
      while (k && *k) {
         unsigned f, t; char buf[128]; int c = 0;
         if (sscanf(k, "%u-%u:%127[^;]%n", &f, &t, buf, &c) < 3) break;
         if (sframe >= f && sframe <= t)
            for (char *s = strtok(buf, "+"); s; s = strtok(NULL, "+"))
               for (int i = 0; i < 8; i++) if (!strcmp(s, n[i])) m |= 1 << i;
         k += c; if (*k == ';') k++;
      }
      return m;
   }
};

static unsigned long ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000UL + t.tv_nsec / 1000000; }

int main(int argc, char **argv)
{
   const char *role = argv[1];
   int a = !strcmp(role, "join") ? 3 : 2, solo = !strcmp(role, "solo");
   const char *rom = argv[a], *dir = argv[a + 2];
   unsigned frames = atoi(argv[a + 1]), frame, total_audio = 0;
   static gambatte::uint_least32_t video[160 * 144], audio[35112 + 2064];
   In in;
   gambatte::GB gb;
   char path[512];
   unsigned long next;

   gb.setInputGetter(&in);
   gb.setSaveDir(dir);
   if (gb.load(rom, 0, 1) != gambatte::LOADRES_OK) { printf("load failed\n"); return 2; }
   printf("[%s] title '%s' cgb %d rate %ld\n", role, gb.romTitle().c_str(), gb.isCgb(), gambatte::GB::sampleRate());
   if (!strcmp(role, "host") && net_host()) return 3;
   if (!strcmp(role, "join") && net_join(argv[2])) return 3;
   next = ms();
   for (frame = 0; frame < 60 * 600; frame++) {
      std::size_t n = 35112;
      gb.runFor(video, 160, audio, n);
      total_audio += n;
      bool up = solo || (!strcmp(role, "host") ? net_clients() > 0 : net_state() == NET_CONNECTED);
      if (!up) { if (sframe) { printf("[%s] link lost at %u\n", role, sframe); break; } }
      else {
         const char *ev = getenv("SHOT_EVERY"), *at = getenv("SHOT_AT");
         char key[16], list[1024];
         snprintf(key, sizeof(key), ",%u,", sframe);
         snprintf(list, sizeof(list), ",%s,", at ? at : "");
         if ((ev && sframe % atoi(ev) == 0) || strstr(list, key)) {
            snprintf(path, sizeof(path), "%s/%s_%05u.ppm", dir, role, sframe);
            FILE *f = fopen(path, "wb");
            fprintf(f, "P6\n160 144\n255\n");
            for (int i = 0; i < 160 * 144; i++) { unsigned char c[3] = { (unsigned char)(video[i] >> 16), (unsigned char)(video[i] >> 8), (unsigned char)video[i] }; fwrite(c, 1, 3, f); }
            fclose(f);
         }
         if (getenv("STATE_SAVE_AT") && sframe == (unsigned)atoi(getenv("STATE_SAVE_AT")))
            printf("[%s] state save %d\n", role, gb.saveState_NoOsd(video, 160));
         if (getenv("STATE_LOAD_AT") && sframe == (unsigned)atoi(getenv("STATE_LOAD_AT")))
            printf("[%s] state load %d\n", role, gb.loadState_NoOsd());
         if (getenv("MENU_AT") && sframe == (unsigned)atoi(getenv("MENU_AT"))) {
            unsigned long t0 = ms(), len = getenv("MENU_MS") ? atoi(getenv("MENU_MS")) : 3000;
            printf("[%s] menu open at %u\n", role, sframe); fflush(stdout);
            gb.menuRunBegin();
            while (ms() - t0 < len)
               gambatte::GB::menuDelay(10);
            gb.menuRunEnd();
            printf("[%s] menu closed after %lu ms\n", role, ms() - t0); fflush(stdout);
            next = ms();
         }
         if (++sframe >= frames) break;
      }
      if (!getenv("NOPACE")) { next += 17; long w = (long)(next - ms()); if (w > 0) usleep(w * 1000); else if (w < -500) next = ms(); }
   }
   gb.saveSavedata();
   printf("[%s] done: %u frames, %u in session, audio %u samples (%.0f/frame)\n", role, frame, sframe, total_audio, (double)total_audio / (frame + 1));
   net_stop();
   return 0;
}
