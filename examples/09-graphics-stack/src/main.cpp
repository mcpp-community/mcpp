// System-level graphics with no host dependency.
//
// This is the sequence a KMS/DRM program or a Wayland compositor actually
// runs. Every header here is the stock upstream one and every call is the
// stock upstream API — there is nothing mcpp-specific in this file, which is
// the point: code written against these libraries anywhere else compiles here
// unchanged.

#include <gbm.h>              // compat.libgbm
#include <xf86drm.h>          // compat.libdrm
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <EGL/egl.h>          // freedesktop.egl
#include <EGL/eglext.h>
#include <wayland-client.h>   // compat.wayland
#include <wayland-server-core.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include <fcntl.h>
#include <unistd.h>

namespace {

// The two APIs exchange this value across the gbm_bo -> drmModeAddFB2
// boundary. If the packages ever disagreed about it, a display would show the
// wrong colours and nothing would report an error — so it is worth asserting
// rather than assuming.
static_assert(GBM_FORMAT_XRGB8888 == DRM_FORMAT_XRGB8888,
              "libgbm and libdrm must agree on the XRGB8888 fourcc");

void report(const char *label, const void *p)
{
    std::printf("  %-24s %p\n", label, p);
}

} // namespace

int main()
{
    std::puts("== the graphics stack, resolved from the index ==");

    // Where the two LOADERS in this stack find what they dlopen. Nothing in
    // this program and none of the packages sets either: `xim:mesa` declares
    // both into the SubOS through the graphics discovery layer, and mcpp
    // carries SubOS declarations into the processes it launches.
    //
    // GBM_BACKENDS_PATH        -> gbm_create_device() dlopens <path>/<drv>_gbm.so
    // __EGL_VENDOR_LIBRARY_DIRS -> eglInitialize() dlopens what a JSON there names
    for (const char *name : {"GBM_BACKENDS_PATH", "__EGL_VENDOR_LIBRARY_DIRS"}) {
        const char *value = std::getenv(name);
        std::printf("  %-25s = %s\n", name,
                    value ? value : "<unset — the ecosystem did not supply it>");
    }

    // Wayland: build a server-side display. No socket is bound, so this needs
    // no session and no privileges — the cheapest proof the library is live.
    if (wl_display *server = wl_display_create()) {
        report("wl_display_create", server);
        wl_display_destroy(server);
    } else {
        std::puts("  wl_display_create           FAILED");
        return 1;
    }

    // The real chain: a DRM node becomes a GBM device, which becomes an EGL
    // display. This is what "headless GPU rendering" means concretely, and it
    // is the sequence that cannot be expressed without all three packages.
    std::puts("-- DRM node -> GBM device -> EGL display --");
    bool reached_egl = false;

    for (const char *node : {"/dev/dri/renderD128", "/dev/dri/card0"}) {
        const int fd = ::open(node, O_RDWR);
        if (fd < 0) {
            std::printf("  %-24s (not present on this machine)\n", node);
            continue;
        }
        std::printf("  %s\n", node);

        if (drmVersionPtr v = drmGetVersion(fd)) {
            std::printf("  %-24s %s\n", "drm driver", v->name);
            drmFreeVersion(v);
        }

        if (gbm_device *gbm = gbm_create_device(fd)) {
            report("gbm_create_device", gbm);

            // Actually allocate GPU memory. Creating the device only proves the
            // backend loaded; a buffer object is the thing a compositor hands
            // to drmModeAddFB2 for scanout, and its stride and modifier come
            // back from the driver rather than from libgbm.
            if (gbm_bo *bo = gbm_bo_create(gbm, 256, 256, GBM_FORMAT_XRGB8888,
                                           GBM_BO_USE_RENDERING)) {
                std::printf("  %-24s 256x256 stride=%u modifier=0x%llx\n",
                            "gbm_bo_create", gbm_bo_get_stride(bo),
                            (unsigned long long)gbm_bo_get_modifier(bo));
                gbm_bo_destroy(bo);
            } else {
                std::printf("  %-24s (driver declined this format/usage)\n",
                            "gbm_bo_create");
            }

            EGLDisplay dpy =
                eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
            report("eglGetPlatformDisplay", dpy);

            if (dpy != EGL_NO_DISPLAY) {
                EGLint major = 0, minor = 0;
                if (eglInitialize(dpy, &major, &minor)) {
                    std::printf("  %-24s EGL %d.%d, vendor %s\n", "eglInitialize",
                                major, minor, eglQueryString(dpy, EGL_VENDOR));
                    reached_egl = true;
                    eglTerminate(dpy);
                }
            }
            gbm_device_destroy(gbm);
        }
        ::close(fd);
    }

    if (!reached_egl) {
        // Not a failure of the packages: a machine with no DRM node (a
        // container, most CI runners) legitimately gets here. Everything above
        // that does not need hardware has already run.
        std::puts("  (no DRM node reached EGL — expected without a GPU)");
    }

    std::puts("done.");
    return 0;
}
