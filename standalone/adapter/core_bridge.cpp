/* What the standalone needs from the core's Game Boy that the libretro API
 * has no call for. Built with the core's own headers (unlike the frontend,
 * whose old libgambatte names are renamed, see Makefile.koriki). */
#include <gambatte.h>
#include <string>
#include "core_bridge.h"

gambatte::GB *gambatte_standalone_gb(void);

extern "C" {

int core_is_cgb(void)
{
   return gambatte_standalone_gb()->isCgb();
}

/* rgb32 = 0xRRGGBB; this core (VIDEO_RGB565) takes its palette in RGB565 */
void core_set_dmg_palette_color(unsigned pal, unsigned color, unsigned long rgb32)
{
   unsigned c = (rgb32 & 0x0000F8) >> 3 | (rgb32 & 0x00FC00) >> 5 | (rgb32 & 0xF80000) >> 8;
   gambatte_standalone_gb()->setDmgPaletteColor(pal, color, c);
}

void core_set_game_genie(const char *codes)
{
   gambatte_standalone_gb()->setGameGenie(std::string(codes));
}

void core_set_game_shark(const char *codes)
{
   gambatte_standalone_gb()->setGameShark(std::string(codes));
}

}
