#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace daikin {

// The first 32 bytes preserve the board descriptor accepted by v0.9 firmware.
// This entire record is part of the signed application payload. Read it only at
// this fixed offset; finding one of these strings elsewhere is not an identity.
struct FirmwareIdentity {
  char target[32];
  char variant[32];
};
static_assert(sizeof(FirmwareIdentity)==64,"Unexpected firmware identity layout");
static constexpr size_t firmwareDescriptorOffset=32;
static constexpr size_t firmwareProjectOffset=firmwareDescriptorOffset+48;
static constexpr size_t firmwareIdentityOffset=firmwareDescriptorOffset+256;
static constexpr size_t firmwarePrefixSize=firmwareIdentityOffset+sizeof(FirmwareIdentity);

// Only the dedicated, authenticated conversion route may select this identity.
// Ordinary OTA always compares against the complete running identity instead.
static inline bool fullToHubIdentity(const FirmwareIdentity &current,
                                     FirmwareIdentity &expected){
  static const char full[32]="PHICOMM-DAIKIN-1:full",hub[32]="PHICOMM-DAIKIN-1:hub";
  if(memcmp(current.variant,full,sizeof(full)))return false;
  expected=current;
  memcpy(expected.variant,hub,sizeof(hub));
  return true;
}

// This checks identity, not authenticity. Signed uploads must also complete the
// trusted-key signature and staged-payload digest checks before activation.
static inline bool matchingFirmwarePrefix(const uint8_t *image,size_t length,
                                         const FirmwareIdentity &expected,
                                         const char project[32]){
  static const uint8_t descriptorMagic[]={0x32,0x54,0xcd,0xab};
  return image&&project&&length>=firmwarePrefixSize&&image[0]==0xe9&&
    !memcmp(image+firmwareDescriptorOffset,descriptorMagic,sizeof(descriptorMagic))&&
    !memcmp(image+firmwareProjectOffset,project,32)&&
    !memcmp(image+firmwareIdentityOffset,&expected,sizeof(expected));
}

} // namespace daikin
