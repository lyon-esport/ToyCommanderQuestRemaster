#!/bin/bash
# Toy Commander browser build: our fork as a libretro core for EmulatorJS (WebAssembly + WebGL 2),
# with the SH4 -> WebAssembly JIT from nasomers/flycast-wasm (core/rec-wasm).
#
# Needs (pinned versions, see README.md):
#   - Emscripten SDK 3.1.74, activated (emcc on PATH)
#   - EJS_RA:  EmulatorJS's RetroArch tree, built with
#              emmake make -f Makefile.emulatorjs LIBRETRO=flycast HAVE_OPENGLES3=1 HAVE_AL=1
#              (the final link of that build fails; only its obj-emscripten/ objects are used)
#   - STUBS:   flycast_stubs.o from nasomers/flycast-wasm (build/flycast_stubs.o)
#
# Usage:  EJS_RA=<path> STUBS=<path> tcvr/web/build.sh [out dir]
# Output: <out dir>/tcvr-wasm.data, the EmulatorJS core package (zip of the .js, .wasm, core.json).

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$SRC/build-web}"
OUT_DIR="${1:-$BUILD_DIR/out}"
: "${EJS_RA:?set EJS_RA to the EmulatorJS RetroArch tree}"
: "${STUBS:?set STUBS to flycast-wasm build/flycast_stubs.o}"

echo "=== 1: build the core library ==="
mkdir -p "$BUILD_DIR" && cd "$BUILD_DIR"
emcmake cmake "$SRC" -DCMAKE_BUILD_TYPE=Release -DLIBRETRO=ON -DUSE_GLES=ON \
	-DCMAKE_C_FLAGS="-DJIT_PROD_BUILD=1 -DFLY_RELEASE_BUILD=1" \
	-DCMAKE_CXX_FLAGS="-DJIT_PROD_BUILD=1 -DFLY_RELEASE_BUILD=1"
emmake make -j"$(nproc)"

echo "=== 2: strip objects RetroArch also has ==="
cp libflycast_libretro_emscripten.a libflycast_libretro_emscripten_stripped.a
emar d libflycast_libretro_emscripten_stripped.a \
	CMakeFiles/flycast_libretro.dir/core/deps/libretro-common/file/file_path.c.o 2>/dev/null || true

echo "=== 3: link with RetroArch's objects ==="
# libchdr/lzma come from Flycast; RetroArch's glsym_es3 is replaced by Flycast's (716 GL symbols).
RA_OBJS=$(find "$EJS_RA/obj-emscripten" -name "*.o" -type f | grep -vE \
	"libchdr_chd|libchdr_cdrom|libchdr_lzma|libchdr_bitstream|libchdr_huffman|libchdr_zlib|libchdr_flac|chd_stream|LzmaEnc|LzmaDec|Lzma2Dec|Lzma86Dec|flycast_stubs|glsym_es3" \
	| sort)

# Every symbol must resolve: a missing one would otherwise become a silent null at run time.
emcc -O3 -flto --profiling-funcs \
	-s WASM=1 -s WASM_BIGINT -s MODULARIZE=1 -s EXPORT_NAME=EJS_Runtime \
	-s EXPORTED_FUNCTIONS='["_main","_malloc","_free","_system_restart","_save_state_info","_load_state","_cmd_take_screenshot","_simulate_input","_toggleMainLoop","_get_core_options","_ejs_set_variable","_set_cheat","_reset_cheat","_shader_enable","_get_disk_count","_get_current_disk","_set_current_disk","_save_file_path","_cmd_savefiles","_supports_states","_refresh_save_files","_toggle_fastforward","_set_ff_ratio","_toggle_rewind","_set_rewind_granularity","_toggle_slow_motion","_set_sm_ratio","_get_current_frame_count","_set_vsync","_set_video_rotation","_get_video_dimensions","_ejs_set_keyboard_enabled","_wasm_mem_read8","_wasm_mem_read16","_wasm_mem_read32","_wasm_mem_write8","_wasm_mem_write16","_wasm_mem_write32","_wasm_exec_ifb","_wasm_exec_shil_fb","_wasm_sq_pref","_wasm_div32u","_wasm_div32s","_wasm_div1"]' \
	-s EXPORTED_RUNTIME_METHODS='["callMain","ccall","cwrap","UTF8ToString","stringToUTF8","lengthBytesUTF8","setValue","getValue","writeArrayToMemory","addRunDependency","removeRunDependency","FS","abort","AL","wasmExports"]' \
	-s INITIAL_MEMORY=268435456 -s STACK_SIZE=4194304 -s ALLOW_MEMORY_GROWTH=1 -s ALLOW_TABLE_GROWTH \
	-s ASYNCIFY=1 -s ASYNCIFY_STACK_SIZE=65536 \
	-s 'ASYNCIFY_REMOVE=["Sh4Interpreter::*","i0*","i1*","addrspace::*","mmu_*","aica::*","Pvr*","pvr*","*ReadMem*","*WriteMem*","sh4_sched_tick*","*TA_*Param*"]' \
	-s EXIT_RUNTIME=0 -s FORCE_FILESYSTEM=1 \
	-s ERROR_ON_UNDEFINED_SYMBOLS=1 \
	-s ASSERTIONS=0 -s DISABLE_EXCEPTION_CATCHING=0 -fexceptions \
	-Wl,--wrap=glGetString \
	-s FULL_ES3=1 -s MIN_WEBGL_VERSION=2 -s MAX_WEBGL_VERSION=2 \
	-lopenal -lidbfs.js \
	--js-library "$EJS_RA/emscripten/library_platform_emscripten.js" \
	--js-library "$EJS_RA/emscripten/library_rwebaudio.js" \
	--js-library "$EJS_RA/emscripten/library_rwebcam.js" \
	"$STUBS" \
	$RA_OBJS \
	libflycast_libretro_emscripten_stripped.a \
	libflycast-resources.a \
	core/deps/tinygettext/libtinygettext.a \
	core/deps/nowide/libnowide.a \
	core/deps/libchdr/libchdr-static.a \
	core/deps/libchdr/deps/lzma-24.05/liblzma.a \
	core/deps/libchdr/deps/zstd-1.5.6/build/cmake/lib/libzstd.a \
	core/deps/libchdr/deps/zlib-1.3.1/libz.a \
	core/deps/xxHash/cmake_unofficial/libxxhash.a \
	core/deps/libzip/lib/libzip.a \
	core/deps/libelf/libelf.a \
	core/deps/miniupnpc/libminiupnpc.a \
	-o flycast_libretro.js \
	--js-library "$HERE/gl_override.js" \
	--pre-js "$EJS_RA/emscripten/pre.js"

echo "=== 4: EmulatorJS core package ==="
mkdir -p "$OUT_DIR/pkg"
cp flycast_libretro.js flycast_libretro.wasm "$OUT_DIR/pkg/"
cp "$SRC/LICENSE" "$OUT_DIR/pkg/license.txt"
cat > "$OUT_DIR/pkg/core.json" <<'EOF'
{"name":"tcvr","extensions":["chd","cdi","gdi","cue","elf"],"options":{},"save":"state","license":"GPLv2","repo":"https://github.com/lyon-esport/ToyCommanderQuestRemaster"}
EOF
rm -f "$OUT_DIR/tcvr-wasm.data"
(cd "$OUT_DIR/pkg" && zip -q -9 ../tcvr-wasm.data core.json flycast_libretro.js flycast_libretro.wasm license.txt)
mkdir -p "$OUT_DIR/reports"
echo '{"buildStart":"tcvr","options":{"defaultWebGL2":true}}' > "$OUT_DIR/reports/tcvr.json"
echo "Core package: $OUT_DIR/tcvr-wasm.data and reports/tcvr.json (go to EmulatorJS data/cores/)"
