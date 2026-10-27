#!/usr/bin/env bash
# 纯逻辑层宿主单测：gcc 直接编译 pk/ + 测试入口（无引擎依赖）。
# 用法：bash projects/pokego/tests/run_tests.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"   # projects/pokego

CC="${CC:-/d/esp-mo/mingw/w64devkit/bin/gcc.exe}"
mkdir -p "$ROOT/build-tests"
"$CC" -std=c11 -Wall -Wextra -I"$ROOT/main" -I"$ROOT/main/pk" \
    -DPKGO_TEST=1 -DMOSAICO_HOST_SIMULATION=1 \
    "$ROOT/tests/test_pk.c" \
    "$ROOT/main/pk/assets.c" \
    "$ROOT/main/pk/battle.c" \
    "$ROOT/main/pk/battle_escape.c" \
    "$ROOT/main/pk/capture.c" \
    "$ROOT/main/pk/combat.c" \
    "$ROOT/main/pk/crc32.c" \
    "$ROOT/main/pk/encounter.c" \
    "$ROOT/main/pk/evolution.c" \
    "$ROOT/main/pk/exp.c" \
    "$ROOT/main/pk/items.c" \
    "$ROOT/main/pk/nurture.c" \
    "$ROOT/main/pk/party.c" \
    "$ROOT/main/pk/pokemon_names.c" \
    "$ROOT/main/pk/pk_assets_host.c" \
    "$ROOT/main/pk/pk_map.c" \
    "$ROOT/main/pk/pk_world.c" \
    -o "$ROOT/build-tests/test_pk.exe"
cd "$ROOT"
PKGO_ASSETS="$ROOT/main/assets" ./build-tests/test_pk.exe
