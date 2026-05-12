#pragma once

#include <cstddef>

namespace ens {

struct SdkStub {
    const char* path;
    const unsigned char* data;
    std::size_t size;
};

extern const SdkStub kSdkStubs[];
extern const std::size_t kSdkStubsCount;

}  // namespace ens