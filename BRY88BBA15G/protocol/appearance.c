#include "appearance.h"
#include "scene.h"
#include <string.h>
static const uint32_t palettes[6][AP_ROLES] = {
 {0x101611,0x1b251d,0xd9e5d0,0xf0f3e8,0xb1bbab,0xe1e9db,0xb7dca5,0x354335,0xb7dca5,0x8ebfcd,0xdcb981,0xefab91},
 {0x101216,0x20242b,0xd8dce4,0xf4f5f7,0xa7afbd,0xe0e4ec,0xc5d3e8,0x3b424f,0xc5d3e8,0x96c9c0,0xd8b79a,0xf1a6a6},
 {0x0c1721,0x162d3d,0xc5e1ed,0xedf8ff,0xa1bdcf,0xd5e8f2,0x8fd9ef,0x2e4e61,0x8fd9ef,0xa8cf9d,0xecc393,0xffb4a0},
 {0x211811,0x35271d,0xe9d4bd,0xfff2df,0xc5ae98,0xecdfce,0xefc28c,0x554232,0xefc28c,0xa6c4b2,0xb8b5df,0xf4a59c},
 {0x1b1523,0x2d233a,0xe0d1ed,0xf8efff,0xb9a8c8,0xe8dcee,0xd5b5f0,0x4b3b5b,0xd5b5f0,0x99cbd2,0xe4c297,0xf5a4b5},
 {0xeeeee6,0xfcfcf6,0x394536,0x1c2a1c,0x5c6857,0x34412f,0x426c3a,0xcbd1c1,0x426c3a,0x346f86,0x9c682c,0xa13828}
};
void appearance_default(screen_appearance *a) {
 memset(a,0,sizeof(*a));a->revision=1;a->opacity=80;a->dim=24;
}
unsigned appearance_rail_height(const screen_appearance *a) {return a->page_rail?a->page_rail:12;}
static bool valid(const screen_appearance *a) {
 if(!a->revision||a->theme>=6||a->wallpaper>4||a->mask>0xfff||
    a->language>1||a->opacity>100||a->dim>70||a->count>APPEARANCE_ITEMS||
    (a->page_rail&&a->page_rail!=6&&a->page_rail!=12&&a->page_rail!=20)) return false;
 for(unsigned i=0;i<AP_ROLES;i++)if(a->colors[i]>0xffffff)return false;
 for(unsigned i=0;i<a->count;i++) {
  const appearance_item *it=&a->items[i];
  if(!it->id||it->mask>31||(it->opacity>100&&it->opacity!=255))return false;
  for(unsigned j=0;j<i;j++)if(a->items[j].id==it->id)return false;
  for(unsigned j=0;j<5;j++)if(it->colors[j]>0xffffff)return false;
 }
 return true;
}
static void rgb(uint8_t *p,uint32_t v) {p[0]=v>>16;p[1]=v>>8;p[2]=v;}
static uint32_t color(const uint8_t *p) {return (uint32_t)p[0]<<16|(uint32_t)p[1]<<8|p[2];}
bool appearance_encode(const screen_appearance *a,uint8_t *p,size_t n) {
 if(n!=APPEARANCE_BYTES||!valid(a))return false;
 memset(p,0,n);memcpy(p,"APP1",4);scene_put32(p+4,a->revision);
 p[8]=a->mask;p[9]=a->mask>>8;p[10]=a->theme;p[11]=a->wallpaper;
 p[12]=a->opacity;p[13]=a->dim;p[14]=a->count;p[56]=a->language;p[57]=a->page_rail;
 for(unsigned i=0;i<AP_ROLES;i++)rgb(p+16+3*i,a->colors[i]);
 scene_put32(p+52,a->custom_crc);
 for(unsigned i=0;i<a->count;i++) {
  const appearance_item *it=&a->items[i];uint8_t *q=p+64+20*i;
  q[0]=it->id;q[1]=it->id>>8;q[2]=it->mask;q[3]=it->opacity;
  for(unsigned j=0;j<5;j++)rgb(q+4+3*j,it->colors[j]);
 }
 return true;
}
bool appearance_decode(screen_appearance *a,const uint8_t *p,size_t n) {
 if(n!=APPEARANCE_BYTES||memcmp(p,"APP1",4)||p[14]>APPEARANCE_ITEMS||p[15])return false;
 if(p[56]>1)return false;
 for(unsigned i=58;i<64;i++)if(p[i])return false;
 screen_appearance v={0};v.revision=scene_u32(p+4);v.mask=p[8]|p[9]<<8;
 v.theme=p[10];v.wallpaper=p[11];v.opacity=p[12];v.dim=p[13];v.count=p[14];
 v.custom_crc=scene_u32(p+52);v.language=p[56];v.page_rail=p[57];
 for(unsigned i=0;i<AP_ROLES;i++)v.colors[i]=color(p+16+3*i);
 for(unsigned i=0;i<v.count;i++) {
  appearance_item *it=&v.items[i];const uint8_t *q=p+64+20*i;
  if(q[19])return false;
  it->id=q[0]|q[1]<<8;it->mask=q[2];it->opacity=q[3];
  for(unsigned j=0;j<5;j++)it->colors[j]=color(q+4+3*j);
 }
 if(!valid(&v))return false;
 *a=v;return true;
}
uint32_t appearance_color(const screen_appearance *a,unsigned role) {
 if(role>=AP_ROLES)return 0;
 return a->mask&(1u<<role)?a->colors[role]:palettes[a->theme<6?a->theme:0][role];
}
const appearance_item *appearance_find(const screen_appearance *a,unsigned id) {
 for(unsigned i=0;i<a->count;i++)if(a->items[i].id==id)return &a->items[i];
 return NULL;
}
uint32_t appearance_widget_color(const screen_appearance *a,const appearance_item *it,unsigned role) {
 return it&&role<5&&(it->mask&(1u<<role))?it->colors[role]:appearance_color(a,AP_BACKGROUND+role);
}
unsigned appearance_opacity(const screen_appearance *a,const appearance_item *it,unsigned type,unsigned height,bool wallpaper) {
 if(it&&it->opacity!=255)return it->opacity;
 if(wallpaper)return a->opacity;
 return (it&&(it->mask&1))||(type!=SC_CLOCK&&height<4)?100:0;
}
