#pragma once
// Allocation shim the Helix decoder expects (see buffers.c). Internal RAM:
// the decoder state (~24 KB) is hit on every frame and WiFi is down while
// music plays from the SD.
#ifdef __cplusplus
extern "C" {
#endif
void* helix_malloc(int size);
void helix_free(void* ptr);
#ifdef __cplusplus
}
#endif
