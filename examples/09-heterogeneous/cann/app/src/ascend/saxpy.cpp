// The Ascend island's HOST half: it launches the kernel through ACL.
//
// IT DECLINES WHEN NO NPU IS PRESENT, and that is the contract rather than a
// shortcut. `aclInit` and `aclrtSetDevice` fail on a machine with no Ascend
// device -- which is every machine this example was developed on -- so the
// function returns non-zero and the caller falls back. A device build
// therefore still links and still runs on a machine with no device; it simply
// says so, which is the property that lets one artifact serve both.
//
// The kernel itself was compiled by BiSheng into a Da Vinci object and linked
// into this binary by the ordinary link. Nothing here compiles device code.
#include "saxpy/saxpy.h"

#include <acl/acl.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

// The launcher the `.asc` file exports. `extern "C"` for the reason the seam
// header gives, and for a sharper one measured here: BiSheng's own launcher
// symbol is C++-mangled, so a C++ declaration would make this program depend
// on BiSheng and g++ agreeing about mangling. The wrapper is one function in
// the device translation unit and removes that dependency.
extern "C" void saxpy_launch(std::uint32_t blockDim, void* stream,
                             std::uint8_t* x, std::uint8_t* y, std::uint8_t* out,
                             float a, std::uint32_t n);

namespace {
char g_ran_on[128] = "";

// One place to leave ACL in the state it was found in, whichever step failed.
struct acl_session {
    bool inited = false, device = false;
    aclrtStream stream = nullptr;
    ~acl_session() {
        if (stream) aclrtDestroyStream(stream);
        if (device) aclrtResetDevice(0);
        if (inited) aclFinalize();
    }
};
} // namespace

extern "C" const char* saxpy_device_name(void) { return g_ran_on; }

extern "C" int saxpy_device(float a, const float* x, const float* y,
                            float* out, unsigned n) {
    acl_session s;
    if (aclInit(nullptr) != ACL_SUCCESS) return 1;
    s.inited = true;

    std::uint32_t count = 0;
    if (aclrtGetDeviceCount(&count) != ACL_SUCCESS || count == 0) return 1;
    if (aclrtSetDevice(0) != ACL_SUCCESS) return 1;
    s.device = true;
    if (aclrtCreateStream(&s.stream) != ACL_SUCCESS) return 1;

    const std::size_t bytes = static_cast<std::size_t>(n) * sizeof(float);
    void *dx = nullptr, *dy = nullptr, *dout = nullptr;
    auto release = [&] {
        if (dx)   aclrtFree(dx);
        if (dy)   aclrtFree(dy);
        if (dout) aclrtFree(dout);
    };
    if (aclrtMalloc(&dx,   bytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS
     || aclrtMalloc(&dy,   bytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS
     || aclrtMalloc(&dout, bytes, ACL_MEM_MALLOC_HUGE_FIRST) != ACL_SUCCESS) {
        release();
        return 1;
    }
    if (aclrtMemcpy(dx, bytes, x, bytes, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS
     || aclrtMemcpy(dy, bytes, y, bytes, ACL_MEMCPY_HOST_TO_DEVICE) != ACL_SUCCESS) {
        release();
        return 1;
    }

    // One block: this example is about the build, and a tiling strategy would
    // be the subject of a different one.
    saxpy_launch(1, s.stream,
                 static_cast<std::uint8_t*>(dx), static_cast<std::uint8_t*>(dy),
                 static_cast<std::uint8_t*>(dout), a, n);
    if (aclrtSynchronizeStream(s.stream) != ACL_SUCCESS) { release(); return 1; }

    if (aclrtMemcpy(out, bytes, dout, bytes, ACL_MEMCPY_DEVICE_TO_HOST) != ACL_SUCCESS) {
        release();
        return 1;
    }
    release();

    const char* name = aclrtGetSocName();
    std::snprintf(g_ran_on, sizeof g_ran_on, "%s", name ? name : "ascend");
    return 0;
}
