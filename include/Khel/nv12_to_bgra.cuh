#pragma once
#include <cuda_runtime.h>

#ifdef _WIN32
#define DLL_EXPORT __declspec(dllexport)
#else
#define DLL_EXPORT
#endif

// Forward declaration for Npp8u (unsigned char from NPP)
typedef unsigned char Npp8u;

extern "C" DLL_EXPORT void nv12_to_bgra_gpu(uint8_t* d_nv12, size_t nv12_pitch,
    uint8_t* d_bgra, size_t bgra_pitch,
    int width, int height);

extern "C" DLL_EXPORT int bgrToBgra_export(const Npp8u* h_bgr_in, Npp8u* h_bgra_out,
    int width, int height, Npp8u alphaValue);
