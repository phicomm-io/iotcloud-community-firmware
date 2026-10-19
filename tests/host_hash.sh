# Source after defining root. Use the same pinned SHA-256 implementation as DUTs.
bearssl="$root/build/ota-tls/bearssl-0.6"
if [[ ! -f "$bearssl/inc/bearssl_hash.h" ]]; then
    mkdir -p "$root/build/ota-tls"
    curl -fLsS --retry 3 https://bearssl.org/bearssl-0.6.tar.gz -o "$root/build/ota-tls/source.tar.gz"
    echo "6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14  $root/build/ota-tls/source.tar.gz" | sha256sum -c -
    tar -xzf "$root/build/ota-tls/source.tar.gz" -C "$root/build/ota-tls"
fi
hash_sources=(-I"$bearssl/inc" -I"$bearssl/src" "$bearssl/src/hash/sha2small.c"
    "$bearssl/src/codec/dec32be.c" "$bearssl/src/codec/enc32be.c" "$bearssl/src/codec/enc64be.c")
