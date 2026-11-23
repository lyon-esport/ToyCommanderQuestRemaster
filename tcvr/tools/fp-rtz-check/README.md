# fp-rtz-check

Checks the browser build's software round-to-zero float helpers (core/rec-wasm/rec_wasm.cpp,
`fly_f32_*_rtz`, `fly_inner_rtz`) against the x86 FPU in round-to-zero mode, on random floats.
The helpers are copied in the test; keep them in step with rec_wasm.cpp.

    g++ -O2 -ffp-contract=off -frounding-math -msse2 -o fp_rtz_check fp_rtz_check.cpp && ./fp_rtz_check

Expected: `bad add 0 mul 0 div 0 sqrt 0 inner 0`.

## wasm_rtz_check

Checks the JIT's inline round-to-zero code (core/rec-wasm/wasm_emit.h, `emitRtz*`) the same way:
builds a wasm module of those emitters, runs it in Node on random inputs and compares with the
x86 FPU in both rounding modes (fadd, fsub, fmul, fdiv, fsqrt, fsrra, fmac, int to float, fipr, ftrv).
Run from this folder:

    python3 -c "s=open('../../../core/rec-wasm/wasm_emit.h').read(); a=s.rindex('// =====',0,s.index('// tcvr: online round-to-zero float ops')); open('rtz_block.inc','w').write(s[a:s.index('static bool emitShilOp(WasmModuleBuilder& b, const shil_opcode& op,')])"
    echo > types.h
    g++ -O1 -std=c++17 -I. -Dprivate=public -ffp-contract=off -frounding-math -msse2 -mfpmath=sse -o wasm_rtz_check wasm_rtz_check.cpp
    ./wasm_rtz_check && node wasm_rtz_check.js

Expected: `bad {}` (10,000 cases per op and mode).
