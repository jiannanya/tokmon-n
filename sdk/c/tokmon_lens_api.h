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
#define TOKMON_OPTICAL_QUERY_EXTENSION_V1 "org.tokmon.optical-query.v1"

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

typedef struct TokmonOpticalHostV1 {
  uint32_t struct_size;
  void* user;
  int32_t (*get)(void* user, TokmonBytesV1 request,
                 TokmonOwnedBytesV1* result, TokmonOwnedBytesV1* error_frame);
  int32_t (*get_all)(void* user, TokmonBytesV1 request,
                     TokmonOwnedBytesV1* result, TokmonOwnedBytesV1* error_frame);
  int32_t (*query)(void* user, TokmonBytesV1 request,
                   TokmonOwnedBytesV1* result, TokmonOwnedBytesV1* error_frame);
} TokmonOpticalHostV1;

typedef struct TokmonOpticalQueryExtensionV1 {
  uint32_t struct_size;
  uint32_t version;
  int32_t (*derive)(void* instance, TokmonBytesV1 photon_window,
                    TokmonOwnedBytesV1* frozen_state,
                    TokmonOwnedBytesV1* error_frame);
  int32_t (*coordinate)(void* instance, TokmonBytesV1 photon_window,
                        const TokmonOpticalHostV1* optical,
                        TokmonOwnedBytesV1* surface_delta,
                        TokmonOwnedBytesV1* error_frame);
  int32_t (*query)(void* instance, TokmonBytesV1 frozen_state,
                   TokmonBytesV1 request, TokmonOwnedBytesV1* result,
                   TokmonOwnedBytesV1* error_frame);
} TokmonOpticalQueryExtensionV1;

typedef const void* (*TokmonLensGetExtensionV1)(const char* extension_id);

TOKMON_LENS_EXPORT TokmonLensApiV1 tokmon_lens_entry_v1(void);
// Optional symbol. Hosts discover it dynamically; legacy Lens modules need not
// export it and retain the exact v1 ABI above.
TOKMON_LENS_EXPORT const void* tokmon_lens_get_extension_v1(const char* extension_id);

#ifdef __cplusplus
}
#endif

#endif

