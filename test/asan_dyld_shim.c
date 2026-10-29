/* macOS 26.4+ calls malloc from dyld_shared_cache_iterate_text while
   AddressSanitizer is still initializing, and the runtime deadlocks.
   Hand ASAN the dyld header through the non-allocating getter instead.
   https://github.com/llvm/llvm-project/issues/182943 */
#include <mach-o/dyld.h>
#include <stdint.h>
#include <string.h>
#include <uuid/uuid.h>

struct dyld_shared_cache_dylib_text_info {
    uint64_t version;
    uint64_t loadAddressUnslid;
    uint64_t textSegmentSize;
    uuid_t dylibUuid;
    const char *path;
    uint64_t textSegmentOffset;
};

typedef void (^dsc_callback)(const struct dyld_shared_cache_dylib_text_info *);

extern int dyld_shared_cache_iterate_text(const uuid_t uuid, dsc_callback cb);
extern const void *_dyld_get_shared_cache_range(size_t *length);
extern const struct mach_header *_dyld_get_dyld_header(void) __attribute__((weak_import));

static int shim_iterate(const uuid_t uuid, dsc_callback cb) {
    if (_dyld_get_dyld_header && cb) {
        const struct mach_header *hdr = _dyld_get_dyld_header();
        size_t len = 0;
        const void *cache = _dyld_get_shared_cache_range(&len);
        if (hdr && cache) {
            struct dyld_shared_cache_dylib_text_info info;
            memset(&info, 0, sizeof info);
            info.version = 2;
            info.textSegmentOffset = (uint64_t)((uintptr_t)hdr - (uintptr_t)cache);
            info.path = "/usr/lib/dyld";
            cb(&info);
            return 0;
        }
    }
    return dyld_shared_cache_iterate_text(uuid, cb);
}

__attribute__((used, section("__DATA,__interpose")))
static struct {
    const void *replacement;
    const void *original;
} interpose_tbl[] = {
    { (const void *)(uintptr_t)&shim_iterate,
      (const void *)(uintptr_t)&dyld_shared_cache_iterate_text },
};
