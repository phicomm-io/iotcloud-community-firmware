#pragma once
#include <memory>
#include <new>
extern "C" {
#include "protocol/scene.h"
}
/* The 9520-byte staging area belongs to an active upload, not to idle TLS.
 * An abandoned browser releases it after 30 seconds without a valid chunk. */
class ScreenLayoutUpload {
  std::unique_ptr<scene_transaction> transfer;
  uint32_t touched=0;
public:
  bool active() const {return bool(transfer);}
  void expire(uint32_t now) {
    if(transfer && uint32_t(now-touched)>=30000u) transfer.reset();
  }
  bool receive(screen_scene *scene,const uint8_t *p,size_t n,uint32_t now) {
    expire(now);
    if(n==13&&p[0]==SC_MSG_BEGIN) {
      const uint32_t total=scene_u32(p+5);
      if(total!=SCENE_BYTES) return false;
      if(!transfer) transfer.reset(new(std::nothrow) scene_transaction{});
    }
    if(!transfer) return false;
    bool ok=scene_receive(transfer.get(),scene,p,n);
    if(ok) touched=now;
    if(!transfer->active || (n && p[0]==SC_MSG_COMMIT))transfer.reset();
    return ok;
  }
};
