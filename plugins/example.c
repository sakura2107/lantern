#include "lantern/plugin.h"
#include <stdlib.h>
#include <string.h>
/* A byte preview provider, not a pretend video codec. */
static int32_t create(lp_handle* out) {
    if (!out)
        return 1;
    *out = malloc(1);
    return *out ? 0 : 3;
}
static void destroy(lp_handle h) {
    free(h);
}
static int32_t process(lp_handle h, const uint8_t* in, uint32_t n, uint8_t* out, uint32_t* size) {
    if (!h || !size || (!in && n))
        return 1;
    if (*size < n || (!out && n)) {
        *size = n;
        return 2;
    }
    if (n)
        memcpy(out, in, n);
    *size = n;
    return 0;
}
LP_EXPORT int32_t lantern_plugin_query(uint32_t abi, uint32_t size, lp_api_v1* out) {
    lp_api_v1 api = {sizeof(lp_api_v1), LP_ABI_V1, "org.lantern.byte-preview", "MIT", 1, create,
                     destroy,           process};
    if (abi != LP_ABI_V1 || size < sizeof(api) || !out)
        return 4;
    *out = api;
    return 0;
}
