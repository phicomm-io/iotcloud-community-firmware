#pragma once
#ifdef __cplusplus
extern "C" {
#endif
#include "resources.h"
#define WALLPAPER_WIDTH 240u
#define WALLPAPER_HEIGHT 427u
#define WALLPAPER_PIXELS (WALLPAPER_WIDTH*WALLPAPER_HEIGHT)
#define WALLPAPER_BYTES (WALLPAPER_PIXELS*2u)
#define WALLPAPER_HEADER 32u
#define WALLPAPER_BASE 0xa00000u
#define WALLPAPER_BANK 0x40000u
#define SCREEN_MSG_WALLPAPER 0x31u
#define SCREEN_MSG_WALLPAPER_STATUS 0x32u
#define WALLPAPER_PREPARING 7u
#define WALLPAPER_IO_DEADLINE 60000u
enum { WP_QUERY, WP_BEGIN, WP_CHUNK, WP_COMMIT, WP_CANCEL };
typedef struct {
 void *context;
 bool (*read)(void *,uint32_t,void *,size_t);
 bool (*write)(void *,uint32_t,const void *,size_t);
 bool (*erase)(void *,uint32_t,size_t);
 uint32_t (*now)(void *);
 void (*service)(void *);
 void (*activity)(void *,bool);
 uint16_t *pixels; /* Native 480x854 RGB565; never read NOR during drawing. */
 uint32_t crc, generation, bank, target, received, running, expected, deadline, erased;
 uint32_t prepare_at, prepare_started, load_at, load_started, load_crc;
 uint8_t header[WALLPAPER_HEADER], state, wanted, kind;
 bool ready, active, pending, initialized, loading, preparing, reading;
} screen_wallpaper;
extern screen_wallpaper wallpaper;
bool wallpaper_header(const uint8_t *,size_t);
void wallpaper_init(screen_wallpaper *);
void wallpaper_poll(screen_wallpaper *);
bool wallpaper_receive(screen_wallpaper *,const uint8_t *,size_t,uint8_t[20]);
/* Shared integer artwork, also used by the browser preview. */
uint32_t wallpaper_preset(unsigned,unsigned,unsigned);

#ifdef __cplusplus
}
#endif
