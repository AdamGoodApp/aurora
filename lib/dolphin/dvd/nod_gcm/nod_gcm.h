#ifndef NOD_GCM_H
#define NOD_GCM_H

#include "nod.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __EMSCRIPTEN__
/**
 * Stream backed by the JS module options `auroraDisc` (FileSystemSyncAccessHandle)
 * and `auroraDiscSize` (number). Calls from pthreads are proxied to the main runtime
 * thread, which owns the handle. `close` is a no-op.
 */
const struct NodDiscStream* nod_gcm_emscripten_stream(void);
#endif

#ifdef __cplusplus
} // extern "C"
#endif

#endif /* NOD_GCM_H */
