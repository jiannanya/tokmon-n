#ifndef TOKMON_LENS_API_H
#define TOKMON_LENS_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#  define TOKMON_LENS_EXPORT __declspec(dllexport)
#else
#  define TOKMON_LENS_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define TOKMON_LENS_ABI_MAJOR 2u
#define TOKMON_LENS_ABI_MINOR 0u

typedef struct TokmonBytes {
  const uint8_t* data;
  size_t size;
} TokmonBytes;

typedef struct TokmonOwnedBytes {
  uint8_t* data;
  size_t size;
  void (*release)(uint8_t* data, size_t size, void* user);
  void* user;
} TokmonOwnedBytes;

typedef struct TokmonLensApi {
  uint32_t abi_major;
  uint32_t abi_minor;
  TokmonBytes manifest_cbor;
  void* (*create)(void);
  int32_t (*view)(void* instance, TokmonBytes optical_input,
                  TokmonOwnedBytes* wavefront_delta,
                  TokmonOwnedBytes* error_frame);
  int32_t (*refract)(void* instance, TokmonBytes photon_window,
                     TokmonBytes act, TokmonOwnedBytes* result,
                     TokmonOwnedBytes* emitted_drafts,
                     TokmonOwnedBytes* error_frame);
  void (*request_stop)(void* instance);
  void (*destroy)(void* instance);
} TokmonLensApi;

typedef TokmonLensApi (*TokmonLensEntry)(void);

TOKMON_LENS_EXPORT TokmonLensApi tokmon_lens_entry(void);

#ifdef __cplusplus
}
#endif

#endif
