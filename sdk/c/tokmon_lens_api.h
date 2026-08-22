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

#define TOKMON_LENS_ABI_MAJOR 1u
#define TOKMON_LENS_ABI_MINOR 0u

typedef struct TokmonBytesV1 {
  const uint8_t* data;
  size_t size;
} TokmonBytesV1;

typedef struct TokmonOwnedBytesV1 {
  uint8_t* data;
  size_t size;
  void (*release)(uint8_t* data, size_t size, void* user);
  void* user;
} TokmonOwnedBytesV1;

typedef struct TokmonLensApiV1 {
  uint32_t abi_major;
  uint32_t abi_minor;
  TokmonBytesV1 manifest_cbor;
  void* (*create)(void);
  int32_t (*view)(void* instance, TokmonBytesV1 photon_window,
                  TokmonOwnedBytesV1* surface_delta,
                  TokmonOwnedBytesV1* error_frame);
  int32_t (*refract)(void* instance, TokmonBytesV1 photon_window,
                     TokmonBytesV1 act, TokmonOwnedBytesV1* result,
                     TokmonOwnedBytesV1* emitted_drafts,
                     TokmonOwnedBytesV1* error_frame);
  void (*request_stop)(void* instance);
  void (*destroy)(void* instance);
} TokmonLensApiV1;

typedef TokmonLensApiV1 (*TokmonLensEntryV1)(void);

TOKMON_LENS_EXPORT TokmonLensApiV1 tokmon_lens_entry_v1(void);

#ifdef __cplusplus
}
#endif

#endif

