# ptoas extern-call probe

Proves the mechanism C1–C5 rest on: **ptoas passes a call to an externally-declared
function straight through to its C++ output**, so a TracR marker can be called from
PTO IR even though the PTO dialect has no counter-read op.

See [doc 08 §5.1](../../08-codegen-comm-markers-plan.md).

## Run

```sh
docker run --rm --name ptoas-extern-probe -v "$PWD:/w" pypto3-hw-native-sys:cann9 \
  bash -lc 'cd /w && ptoas probe.pto -o probe.cpp --enable-insert-sync --pto-level=level3 && cat probe.cpp'
```

Expect in `probe.cpp`:

```cpp
extern "C" AICORE void tracr_mark(__gm__ int64_t*, int32_t, int32_t, int32_t);
...
  #if defined(__DAV_VEC__)
  tracr_mark(v1, v2, v4, v3);
  #endif // __DAV_VEC__
```

`iso.cpp` closes the loop on the host: ptoas's emitted declaration + call, against a
stand-in marker definition, with the record decoded.

```sh
g++ -std=c++20 -D__DAV_VEC__ -o iso iso.cpp && ./iso
# records=1 word0=0x100070000 ts=1     (channel 0, event 7, extra 1)
```

## Traps

- **`-std=c++20` is required.** pto-isa's headers use `std::remove_cvref_t`; C++17 fails.
- **Do not include `pto/pto-inst.hpp` in the host check.** Its CPU stub has an unrelated
  `exp(half)` ambiguity under g++-15 that has nothing to do with the mechanism. `iso.cpp`
  stubs the three symbols it needs instead.

## Device-side check

`ccec_probe.cpp` compiles the same shape under the real incore toolchain.

```sh
docker run --rm --name ptoas-extern-ccec -v "$PWD:/w" pypto3-hw-native-sys:cann9 bash -lc '
  /usr/local/Ascend/cann-9.0.0/tools/ccec_compiler/bin/ccec -c -O3 -x cce -Wall -std=c++17 \
    --cce-aicore-only --cce-aicore-arch=dav-c220-vec -mllvm -cce-aicore-addr-transform \
    -DMEMORY_BASE -o /w/ccec_probe.o /w/ccec_probe.cpp && nm /w/ccec_probe.o | grep tracr'
# 0000000000000000 T tracr_mark
```
