#include <stdint.h>
#include <stdlib.h>

typedef struct {
    int64_t refcount;
} EnsHeader;

void* ens_alloc(uint64_t payload_size) {
    EnsHeader* header = (EnsHeader*)malloc(sizeof(EnsHeader) + payload_size);
    if (!header) return NULL;
    header->refcount = 1;
    return (void*)(header + 1);
}

void ens_retain(void* obj) {
    if (!obj) return;
    EnsHeader* header = ((EnsHeader*)obj) - 1;
    __atomic_fetch_add(&header->refcount, 1, __ATOMIC_RELAXED);
}

void ens_release(void* obj) {
    if (!obj) return;
    EnsHeader* header = ((EnsHeader*)obj) - 1;
    int64_t prev = __atomic_fetch_sub(&header->refcount, 1, __ATOMIC_ACQ_REL);
    if (prev == 1) {
        free(header);
    }
}
