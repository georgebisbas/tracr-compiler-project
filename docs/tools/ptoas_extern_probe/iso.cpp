#include <cstdint>
#include <cstdio>
#define __gm__
#define AICORE
static inline void set_mask_norm() {}
static inline void set_vector_mask(int64_t, int64_t) {}

// --- PyPTO-injected prologue (stands in for tracr_aicore_emit.h) ---
static int64_t g_fake_clock = 0;
extern "C" AICORE void tracr_mark(__gm__ int64_t* buf, int32_t chan, int32_t event, int32_t extra) {
  int64_t n = buf[0];
  buf[2 + n * 2]     = (int64_t)((uint32_t)chan | ((uint32_t)event << 16) | ((uint64_t)(uint32_t)extra << 32));
  buf[2 + n * 2 + 1] = ++g_fake_clock;
  buf[0] = n + 1;
}
extern "C" AICORE void tracr_mark(__gm__ int64_t*, int32_t, int32_t, int32_t);
AICORE void probe(__gm__ int64_t* v1) {
  using T = float;

  #if defined(__DAV_VEC__)
  set_mask_norm();
  set_vector_mask(-1, -1);
  // pto: %c0_i32
  const int32_t v2 = 0;
  // pto: %c1_i32
  const int32_t v3 = 1;
  // pto: %c7_i32
  const int32_t v4 = 7;
  tracr_mark(v1, v2, v4, v3);
  #endif // __DAV_VEC__

  return;
}
int main() {
  int64_t buf[16] = {0};
  probe(buf);
  printf("records=%lld word0=0x%llx ts=%lld\n", (long long)buf[0], (long long)buf[2], (long long)buf[3]);
  return 0;
}
