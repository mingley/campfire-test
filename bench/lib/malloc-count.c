/* Linux/glibc diagnostic only: count C allocator calls alongside prefixed jemalloc.
 * Build: cc -shared -fPIC -O2 -o tmp/bench/malloc-count.so bench/lib/malloc-count.c
 * LD_PRELOAD this library in allocation runs, never in timing runs.
 * __libc_* avoids allocation/reentrancy in a dlsym-based allocator bootstrap.
 */
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

extern void *__libc_malloc(size_t);
extern void *__libc_calloc(size_t, size_t);
extern void *__libc_realloc(void *, size_t);
extern void __libc_free(void *);

static _Atomic unsigned long long mallocs, callocs, reallocs, frees;

void *malloc(size_t size) {
    atomic_fetch_add_explicit(&mallocs, 1, memory_order_relaxed);
    return __libc_malloc(size);
}

void *calloc(size_t count, size_t size) {
    atomic_fetch_add_explicit(&callocs, 1, memory_order_relaxed);
    return __libc_calloc(count, size);
}

void *realloc(void *pointer, size_t size) {
    atomic_fetch_add_explicit(&reallocs, 1, memory_order_relaxed);
    return __libc_realloc(pointer, size);
}

void free(void *pointer) {
    atomic_fetch_add_explicit(&frees, 1, memory_order_relaxed);
    __libc_free(pointer);
}

__attribute__((destructor)) static void report(void) {
    char buffer[256];
    int length = snprintf(buffer, sizeof buffer,
        "CAMPFIRE_MALLOC_COUNTS {\"malloc\":%llu,\"calloc\":%llu,\"realloc\":%llu,\"free\":%llu}\n",
        atomic_load_explicit(&mallocs, memory_order_relaxed),
        atomic_load_explicit(&callocs, memory_order_relaxed),
        atomic_load_explicit(&reallocs, memory_order_relaxed),
        atomic_load_explicit(&frees, memory_order_relaxed));
    if (length > 0 && length < (int)sizeof buffer) (void)write(STDERR_FILENO, buffer, length);
}
