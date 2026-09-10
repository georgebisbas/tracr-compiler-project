#include <cstdint>
#ifndef __gm__
#define __gm__
#endif
#define AICORE [aicore]

// The definition PyPTO's prologue would inject, matching ptoas's declaration.
extern "C" AICORE void tracr_mark(__gm__ int64_t* buf, int32_t chan, int32_t event, int32_t extra) {
  int64_t n = buf[0];
  uint64_t w0 = ((uint32_t)chan & 0xFFFFu) | (((uint32_t)event & 0xFFFFu) << 16)
              | ((uint64_t)(uint32_t)extra << 32);
  buf[2 + n * 2]     = (int64_t)w0;
  buf[2 + n * 2 + 1] = (int64_t)get_sys_cnt();
  buf[0] = n + 1;
}

// Exactly what ptoas emits into the kernel TU.
extern "C" AICORE void tracr_mark(__gm__ int64_t*, int32_t, int32_t, int32_t);

extern "C" __global__ AICORE void kernel_entry(__gm__ int64_t* v1) {
  const int32_t v2 = 0;
  const int32_t v3 = 1;
  const int32_t v4 = 7;
  tracr_mark(v1, v2, v4, v3);
}
