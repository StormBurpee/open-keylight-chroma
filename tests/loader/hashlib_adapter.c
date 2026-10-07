#include "okl_loader.h"
#include <string.h>

#if defined(_WIN32)
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif

/* Test-only foreign-function adapter: Python hashlib supplies real SHA256 to
 * the same callback slot the embedded mbedTLS adapter uses. No network I/O. */
EXPORTED int loader_prepare_with_sha256(const uint8_t *package, size_t size,
                                       int (*sha256)(void *, const uint8_t *, size_t, uint8_t *)) {
    okl_loader_ops ops;
    okl_loader_image image;
    memset(&ops, 0, sizeof(ops));
    ops.sha256 = sha256;
    return (int)okl_loader_prepare(&image, package, size, &ops);
}
