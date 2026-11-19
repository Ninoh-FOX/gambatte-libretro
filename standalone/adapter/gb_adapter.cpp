/*
 * The old libgambatte API the DMS frontend was written for (include/gambatte.h),
 * on top of this core (Gambatte Plus) through the libretro API.
 *
 * Built like the rest of the frontend, with its names renamed (gambatte ->
 * dmsgb, see Makefile.koriki), so this GB is not the core's.
 *
 *  - runFor() runs one frame (retro_run) and hands its video, converted to the
 *    32-bit pixels the frontend uses, and its audio (at the core's rate, see
 *    sampleRate()).
 *  - The color filter of the DMS menu is applied here, to CGB games, as the
 *    old libgambatte did to their palette.
 *  - Saves are RetroArch's: <rom>.srm (save memory) and <rom>.rtc (clock), so
 *    they can be shared with RetroArch's.
 *  - States keep the DMS layout (5 bytes, an 80x72 preview, then the core's
 *    state), so the menu previews still work.
 *  - Netplay: the core's netpacket interface goes to net.c (Cable Link).
 */
#include <gambatte.h>
#include <pakinfo.h>
#include <libretro.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "core_bridge.h"
#include "../net.h"

/* the frontend's (libmenu.cpp, C linkage there) */
extern "C" {
extern int biosenabled;
extern std::string homedir;
void printOverlay(const char *text);
}

namespace gambatte {

static unsigned long lut[65536];        /* RGB565 -> frontend pixels */
static int lut_filter = -1;             /* filter the LUT was made with */
static int filter_on;
static int filter_val[12];
static const uint16_t *frame_px;        /* last frame of the core */
static size_t frame_pitch;
static std::vector<int16_t> audio;      /* this frame's samples (stereo) */
static InputGetter *input;
static bool ui_ready;
static std::string pending_msg;
static std::string save_dir;
static int prefer_cgb;
static bool force_dmg;

static void make_lut(void)
{
   int f = filter_on && core_is_cgb();
   if (f == lut_filter && !f)
      return;
   for (unsigned c = 0; c < 65536; c++) {
      unsigned long r = (c >> 11) << 3, g = ((c >> 5) & 63) << 2, b = (c & 31) << 3;
      if (f)
         lut[c] = ((((r * filter_val[0] + g * filter_val[1] + b * filter_val[2]) >> 8) + filter_val[3]) & 0xff) << 16
                | ((((r * filter_val[4] + g * filter_val[5] + b * filter_val[6]) >> 8) + filter_val[7]) & 0xff) << 8
                | ((((r * filter_val[8] + g * filter_val[9] + b * filter_val[10]) >> 8) + filter_val[11]) & 0xff);
      else
         lut[c] = r << 16 | g << 8 | b;
   }
   lut_filter = f;
}

/* ---------------------------------------------------------------- libretro side */

static const char *option(const char *key)
{
   if (!strcmp(key, "gambatte_gb_colorization"))      return "disabled";   /* the DMS menu sets the palette */
   if (!strcmp(key, "gambatte_gbc_color_correction")) return "disabled";   /* the DMS filter instead */
   if (!strcmp(key, "gambatte_mix_frames"))           return "disabled";   /* DMS ghosting instead */
   if (!strcmp(key, "gambatte_audio_resampler"))      return "sinc";
   if (!strcmp(key, "gambatte_gb_bootloader"))        return biosenabled ? "enabled" : "disabled";
   if (!strcmp(key, "gambatte_gb_link_mode"))         return "Cable Link";
   if (!strcmp(key, "gambatte_up_down_allowed"))      return "disabled";
   if (!strcmp(key, "gambatte_gb_hwmode"))            return force_dmg ? "GB" : "Auto";
   return NULL;
}

static void show_message(const char *text)
{
   printf("gambatte: %s\n", text);
   if (ui_ready)
      printOverlay(text);
   else
      pending_msg = text;
}

}  /* namespace */

/* net.c shows its messages through this */
extern "C" void net_message(const char *text, unsigned ms)
{
   (void)ms;
   gambatte::show_message(text);
}

namespace gambatte {

static std::string bios_dir;

static bool environment(unsigned cmd, void *data)
{
   switch (cmd) {
   case RETRO_ENVIRONMENT_GET_VARIABLE: {
      struct retro_variable *v = (struct retro_variable *)data;
      v->value = option(v->key);
      return v->value != NULL;
   }
   case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
      *(bool *)data = false;
      return true;
   case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
      return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
   case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
      *(const char **)data = bios_dir.c_str();
      return true;
   case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
      *(const char **)data = save_dir.c_str();
      return true;
   case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
      return true;
   case RETRO_ENVIRONMENT_GET_CAN_DUPE:
      *(bool *)data = true;     /* a repeated frame: runFor() says no frame */
      return true;
   case RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE:
      net_set_interface((const struct retro_netpacket_callback *)data);
      return true;
   case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
      *(unsigned *)data = 1;
      return true;
   case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
      const struct retro_message_ext *m = (const struct retro_message_ext *)data;
      if (m->msg && m->type != RETRO_MESSAGE_TYPE_STATUS)
         show_message(m->msg);
      return true;
   }
   case RETRO_ENVIRONMENT_SET_MESSAGE: {
      const struct retro_message *m = (const struct retro_message *)data;
      if (m->msg)
         show_message(m->msg);
      return true;
   }
   default:
      return false;
   }
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
   (void)w; (void)h;
   if (data) {
      frame_px = (const uint16_t *)data;
      frame_pitch = pitch / 2;
   }
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
   audio.insert(audio.end(), data, data + frames * 2);
   return frames;
}

static void audio_sample(int16_t l, int16_t r)
{
   audio.push_back(l);
   audio.push_back(r);
}

static void input_poll(void) {}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
   static const unsigned map[16] = {
      /* RETRO_DEVICE_ID_JOYPAD_B .. R3 -> InputGetter bits */
      InputGetter::B, 0, InputGetter::SELECT, InputGetter::START,
      InputGetter::UP, InputGetter::DOWN, InputGetter::LEFT, InputGetter::RIGHT,
      InputGetter::A, 0, 0, 0, 0, 0, 0, 0 };
   unsigned keys, i;
   int16_t mask = 0;

   (void)index;
   if (port != 0 || device != RETRO_DEVICE_JOYPAD || !input)
      return 0;
   keys = (*input)();
   if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
      for (i = 0; i < 16; i++)
         if (map[i] && (keys & map[i]))
            mask |= 1 << i;
      return mask;
   }
   return id < 16 && map[id] && (keys & map[id]) ? 1 : 0;
}

/* ---------------------------------------------------------------- GB */

struct GB::Priv {
   bool loaded;
   int state_no;
   std::string rom_path;      /* as given */
   std::string base;          /* save_dir + ROM name without extension */
   std::vector<unsigned char> rom;
   std::vector<unsigned char> saved_sram, saved_rtc;   /* last written */
   Priv() : loaded(false), state_no(0) {}
};

static bool core_inited;

GB::GB() : p_(new Priv)
{
   if (!core_inited) {
      retro_set_environment(environment);
      retro_set_video_refresh(video_refresh);
      retro_set_audio_sample(audio_sample);
      retro_set_audio_sample_batch(audio_batch);
      retro_set_input_poll(input_poll);
      retro_set_input_state(input_state);
      retro_init();
      core_inited = true;
   }
}

GB::~GB()
{
   if (p_->loaded) {
      net_stop();
      saveSavedata();
      retro_unload_game();
   }
   delete p_;
}

static std::string strip_ext(std::string const &s)
{
   std::string::size_type slash = s.find_last_of('/'), dot = s.find_last_of('.');
   std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
   if (dot != std::string::npos && (slash == std::string::npos || dot > slash))
      name = name.substr(0, name.find_last_of('.'));
   return name;
}

static bool read_file(std::string const &path, std::vector<unsigned char> &out)
{
   FILE *f = fopen(path.c_str(), "rb");
   long n;
   if (!f)
      return false;
   fseek(f, 0, SEEK_END);
   n = ftell(f);
   fseek(f, 0, SEEK_SET);
   out.resize(n > 0 ? n : 0);
   if (n > 0 && fread(&out[0], 1, n, f) != (size_t)n) {
      fclose(f);
      return false;
   }
   fclose(f);
   return true;
}

static void load_mem(unsigned id, std::string const &path, std::vector<unsigned char> &copy)
{
   void *mem = retro_get_memory_data(id);
   size_t size = retro_get_memory_size(id);
   std::vector<unsigned char> buf;
   if (!mem || !size || !read_file(path, buf) || buf.empty())
      return;
   memcpy(mem, &buf[0], buf.size() < size ? buf.size() : size);
   copy.assign((unsigned char *)mem, (unsigned char *)mem + size);
}

static void save_mem(unsigned id, std::string const &path, std::vector<unsigned char> &copy)
{
   unsigned char *mem = (unsigned char *)retro_get_memory_data(id);
   size_t size = retro_get_memory_size(id);
   FILE *f;
   if (!mem || !size || (copy.size() == size && !memcmp(&copy[0], mem, size)))
      return;
   if (!(f = fopen(path.c_str(), "wb")))
      return;
   if (fwrite(mem, 1, size, f) == size)
      copy.assign(mem, mem + size);
   fclose(f);
}

LoadRes GB::load(std::string const &romfile, unsigned flags, int preferCGB)
{
   struct retro_game_info info;

   if (p_->loaded) {
      net_stop();
      saveSavedata();
      retro_unload_game();
      p_->loaded = false;
   }
   if (!read_file(romfile, p_->rom) || p_->rom.size() < 0x150)
      return LOADRES_IO_ERROR;

   /* DMS: a game that also runs on the DMG (CGB flag 0x80) runs as DMG unless
    * "prefer CGB" is on; FORCE_DMG always */
   prefer_cgb = preferCGB;
   force_dmg = (flags & FORCE_DMG) || (!preferCGB && p_->rom[0x143] == 0x80);
   bios_dir = homedir + "/.gambatte/bios";

   memset(&info, 0, sizeof(info));
   info.path = romfile.c_str();
   info.data = &p_->rom[0];
   info.size = p_->rom.size();
   if (!retro_load_game(&info))
      return LOADRES_BAD_FILE_OR_UNKNOWN_MBC;

   p_->loaded = true;
   p_->rom_path = romfile;
   p_->base = save_dir + strip_ext(romfile);
   p_->saved_sram.clear();
   p_->saved_rtc.clear();
   load_mem(RETRO_MEMORY_SAVE_RAM, p_->base + ".srm", p_->saved_sram);
   load_mem(RETRO_MEMORY_RTC, p_->base + ".rtc", p_->saved_rtc);
   lut_filter = -1;
   net_set_game_name(strip_ext(romfile).c_str());
   return LOADRES_OK;
}

std::ptrdiff_t GB::runFor(gambatte::uint_least32_t *videoBuf, std::ptrdiff_t pitch,
                          gambatte::uint_least32_t *audioBuf, std::size_t &samples)
{
   size_t n;

   if (!p_->loaded) {
      samples = 0;
      return -1;
   }
   if (!ui_ready) {
      ui_ready = true;     /* the frontend runs frames once its screens are up */
      if (!pending_msg.empty()) {
         printOverlay(pending_msg.c_str());
         pending_msg.clear();
      }
   }

   audio.clear();
   frame_px = NULL;
   net_poll();
   retro_run();

   if (videoBuf && frame_px) {
      make_lut();
      for (int y = 0; y < 144; y++) {
         const uint16_t *s = frame_px + y * frame_pitch;
         gambatte::uint_least32_t *d = videoBuf + y * pitch;
         for (int x = 0; x < 160; x++)
            d[x] = lut[s[x]];
      }
   }

   n = audio.size() / 2;
   if (audioBuf && n)
      memcpy(audioBuf, &audio[0], n * 4);
   samples = n;
   return frame_px ? (std::ptrdiff_t)n : -1;
}

long GB::sampleRate()
{
   struct retro_system_av_info av;
   retro_get_system_av_info(&av);
   return (long)(av.timing.sample_rate + 0.5);
}

void GB::reset()
{
   if (p_->loaded)
      retro_reset();
}

void GB::setDmgPaletteColor(int palNum, int colorNum, unsigned long rgb32)
{
   if (p_->loaded)
      core_set_dmg_palette_color(palNum, colorNum, rgb32);
}

void GB::setColorFilter(int activated, int filtercolors[12])
{
   filter_on = activated == 1;
   if (filter_on)
      for (int i = 0; i < 12; i++)
         filter_val[i] = filtercolors[i];
   lut_filter = -1;
}

void GB::setInputGetter(InputGetter *getInput) { input = getInput; }

/* the core reads <system dir>/gb_bios.bin and gbc_bios.bin itself */
void GB::setBootloaderGetter(bool (*)(void *, bool, uint8_t *, uint32_t)) {}
void GB::full_init() {}

void GB::setSaveDir(std::string const &sdir)
{
   save_dir = sdir;
   if (!save_dir.empty() && save_dir[save_dir.size() - 1] != '/')
      save_dir += '/';
}

bool GB::isCgb() const { return p_->loaded && core_is_cgb(); }
bool GB::isLoaded() const { return p_->loaded; }

void GB::saveSavedata()
{
   if (!p_->loaded)
      return;
   save_mem(RETRO_MEMORY_SAVE_RAM, p_->base + ".srm", p_->saved_sram);
   save_mem(RETRO_MEMORY_RTC, p_->base + ".rtc", p_->saved_rtc);
}

std::string GB::getSaveStatePath(int statenum)
{
   char n[16];
   snprintf(n, sizeof(n), "_%d.gqp", statenum);
   return p_->base + n;
}

/* ---------------------------------------------------------------- states */

static const char state_magic[5] = { 'G', 'Q', 'P', 1, 0 };

bool GB::saveState(gambatte::uint_least32_t const *videoBuf, std::ptrdiff_t pitch,
                   std::string const &filepath)
{
   size_t size = retro_serialize_size();
   std::vector<unsigned char> st(size);
   gambatte::uint_least32_t preview[80 * 72];
   FILE *f;

   if (!p_->loaded || !size || !retro_serialize(&st[0], size))
      return false;
   for (int y = 0; y < 72; y++)
      for (int x = 0; x < 80; x++)
         preview[y * 80 + x] = videoBuf ? videoBuf[y * 2 * pitch + x * 2] : 0;
   if (!(f = fopen(filepath.c_str(), "wb")))
      return false;
   bool ok = fwrite(state_magic, 1, 5, f) == 5
          && fwrite(preview, 1, sizeof(preview), f) == sizeof(preview)
          && fwrite(&st[0], 1, size, f) == size;
   fclose(f);
   return ok;
}

bool GB::loadState(std::string const &filepath)
{
   std::vector<unsigned char> buf;
   const size_t skip = 5 + 80 * 72 * 4;
   if (!p_->loaded || !read_file(filepath, buf) || buf.size() <= skip
       || memcmp(&buf[0], state_magic, 3))
      return false;
   return retro_unserialize(&buf[skip], buf.size() - skip);
}

bool GB::saveState(gambatte::uint_least32_t const *videoBuf, std::ptrdiff_t pitch)
{
   bool ok = saveState(videoBuf, pitch, getSaveStatePath(p_->state_no));
   char msg[32];
   snprintf(msg, sizeof(msg), ok ? "State %d saved" : "State %d failed", p_->state_no);
   show_message(msg);
   return ok;
}

bool GB::saveState_NoOsd(gambatte::uint_least32_t const *videoBuf, std::ptrdiff_t pitch)
{
   return saveState(videoBuf, pitch, getSaveStatePath(p_->state_no));
}

bool GB::loadState()
{
   bool ok = loadState(getSaveStatePath(p_->state_no));
   char msg[32];
   snprintf(msg, sizeof(msg), ok ? "State %d loaded" : "State %d empty", p_->state_no);
   show_message(msg);
   return ok;
}

bool GB::loadState_NoOsd() { return loadState(getSaveStatePath(p_->state_no)); }

void GB::selectState(int n)
{
   char msg[32];
   selectState_NoOsd(n);
   snprintf(msg, sizeof(msg), "State %d selected", p_->state_no);
   show_message(msg);
}

void GB::selectState_NoOsd(int n)
{
   p_->state_no = n < 0 ? 9 : n > 9 ? 0 : n;
}

int GB::currentState() const { return p_->state_no; }

std::string const GB::romTitle() const
{
   char t[17] = { 0 };
   if (p_->rom.size() >= 0x144)
      memcpy(t, &p_->rom[0x134], 16);
   return std::string(t);
}

void GB::setGameGenie(std::string const &codes) { if (p_->loaded) core_set_game_genie(codes.c_str()); }
void GB::setGameShark(std::string const &codes) { if (p_->loaded) core_set_game_shark(codes.c_str()); }

std::string const to_string(LoadRes r)
{
   return r == LOADRES_OK ? "ok" : r == LOADRES_IO_ERROR ? "cannot read the file" : "bad ROM";
}

}  /* namespace */
