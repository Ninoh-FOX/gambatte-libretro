#ifndef CORE_BRIDGE_H
#define CORE_BRIDGE_H
#ifdef __cplusplus
extern "C" {
#endif
int core_is_cgb(void);
void core_set_dmg_palette_color(unsigned pal, unsigned color, unsigned long rgb32);
void core_set_game_genie(const char *codes);
void core_set_game_shark(const char *codes);
#ifdef __cplusplus
}
#endif
#endif
