# Source after defining root. Compile the same SDK SHA-256 used by firmware.
sha_sdk="$root/build/a1-sdk/mico-os/MiCO/security/SHAUtils"
if [[ ! -f "$sha_sdk/sha224-256.c" ]]; then
    python3 "$root/tools/prepare_sdk.py" "$root/build/a1-sdk/mico-os"
fi
hash_sources=(-I"$sha_sdk" "$sha_sdk/sha224-256.c")
