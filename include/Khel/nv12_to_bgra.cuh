#pragma once
#include <cuda_runtime.h>

#ifdef _WIN32
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT
#endif

// Your AVFrame input is CPU memory (use_frame->data[0] and data[1])
// Output will be in BGRA format (CPU buffer provided by caller)
extern "C" DLL_EXPORT bool ConvertAVFrameToBGRA(const uint8_t* y_plane, int y_pitch,
    const uint8_t* uv_plane, int uv_pitch,
    int width, int height,
    uint8_t* out_bgra, int out_bgra_pitch);
