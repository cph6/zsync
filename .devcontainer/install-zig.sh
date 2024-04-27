#!/usr/bin/env sh

set -xe

ZIG_VERSION="$1"
ZIG_URL="https://ziglang.org/download/${ZIG_VERSION}/zig-linux-x86_64-${ZIG_VERSION}.tar.xz"
ZIG_SIGNATURE_URL="https://ziglang.org/download/${ZIG_VERSION}/zig-linux-x86_64-${ZIG_VERSION}.tar.xz.minisig"
ZIG_PUBKEY="RWSGOq2NVecA2UPNdBUZykf1CCb147pkmdtYxgb3Ti+JO/wCYvhbAb/U"
MINISIGN_VERSION="0.11"
MINISIGN_URL="https://github.com/jedisct1/minisign/releases/download/${MINISIGN_VERSION}/minisign-${MINISIGN_VERSION}-linux.tar.gz"
MINISIGN_SIGNATURE_URL="https://github.com/jedisct1/minisign/releases/download/${MINISIGN_VERSION}/minisign-${MINISIGN_VERSION}-linux.tar.gz.minisig"
MINISIGN_PUBKEY="RWQf6LRCGA9i53mlYecO4IzT51TGPpvWucNSCh1CBM0QTaLn73Y7GFO3"

curl -OsL ${MINISIGN_URL}
tar -xzf "minisign-${MINISIGN_VERSION}-linux.tar.gz"
ln -s /home/codespace/minisign-linux/x86_64/minisign /home/codespace/.local/bin/minisign

curl -OsL ${MINISIGN_SIGNATURE_URL}
minisign -Vm minisign-${MINISIGN_VERSION}-linux.tar.gz -P ${MINISIGN_PUBKEY}

curl -OsL ${ZIG_URL}
curl -OsL ${ZIG_SIGNATURE_URL}
minisign -Vm zig-linux-x86_64-${ZIG_VERSION}.tar.xz -P ${ZIG_PUBKEY}

tar -xf "zig-linux-x86_64-${ZIG_VERSION}.tar.xz"
ln -s "/home/codespace/zig-linux-x86_64-${ZIG_VERSION}/zig" /home/codespace/.local/bin/zig