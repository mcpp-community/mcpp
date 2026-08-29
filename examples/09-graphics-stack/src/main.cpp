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
#include <EGL/egl.h>          // compat.egl
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

    // Where the GBM backends are found. Nothing in this program and nothing in
    // compat.libgbm sets this: `xim:mesa` declares it into the SubOS through
    // the graphics discovery layer, and mcpp carries SubOS declarations into
    // the processes it launches.
    const char *backends = std::getenv("GBM_BACKENDS_PATH");
    std::printf("  GBM_BACKENDS_PATH = %s\n",
                backends ? backends : "<unset — the ecosystem did not supply it>");

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
