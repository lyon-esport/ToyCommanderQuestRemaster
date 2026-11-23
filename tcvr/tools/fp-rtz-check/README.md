# fp-rtz-check

Checks the browser build's software round-to-zero float helpers (core/rec-wasm/rec_wasm.cpp,
`fly_f32_*_rtz`, `fly_inner_rtz`) against the x86 FPU in round-to-zero mode, on random floats.
The helpers are copied in the test; keep them in step with rec_wasm.cpp.

    g++ -O2 -ffp-contract=off -frounding-math -msse2 -o fp_rtz_check fp_rtz_check.cpp && ./fp_rtz_check

Expected: `bad add 0 mul 0 div 0 sqrt 0 inner 0`.
