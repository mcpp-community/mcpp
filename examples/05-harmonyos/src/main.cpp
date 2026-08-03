#include <cstdio>
import device;

int main() {
    std::printf("%s\n", device_banner().c_str());
#if defined(__OHOS__)
    std::printf("__OHOS__ is defined: this really is a HarmonyOS target\n");
#else
    std::printf("NOT built for HarmonyOS\n");
    return 1;
#endif
    return 0;
}
