#ifndef LANTERN_PLUGIN_H
#define LANTERN_PLUGIN_H
#include <stdint.h>
#ifdef _WIN32
#define LP_EXPORT __declspec(dllexport)
#else
#define LP_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define LP_ABI_V1 0x00010000u
typedef void* lp_handle;
typedef struct lp_api_v1 {
    uint32_t size, abi;
    const char* id;
    const char* license; /* UTF-8 static lifetime */
    uint64_t capabilities;
    int32_t (*create)(lp_handle* out);
    void (*destroy)(lp_handle h);
    /* Caller-owned output; return 2 with required size on small buffer. Serialized calls per handle. */
    int32_t (*process)(lp_handle h, const uint8_t* input, uint32_t input_size, uint8_t* output,
                       uint32_t* output_size);
} lp_api_v1;
typedef int32_t (*lp_query_fn)(uint32_t abi, uint32_t caller_size, lp_api_v1* out);
LP_EXPORT int32_t lantern_plugin_query(uint32_t abi, uint32_t caller_size, lp_api_v1* out);
#ifdef __cplusplus
}
#endif
#endif
