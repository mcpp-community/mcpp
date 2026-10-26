// The two shapes os::host() takes (design §6.1 R3, §8.6), reduced to what the
// compilers must accept.
//   native target: host() is the compile target, usable in constant expressions;
//   ok target:     host() is a constexpr function whose constant evaluation is
//                  ill-formed (it is not known when compiling) and whose call at
//                  run time asks the environment.
enum class platform { linux, macos, windows, openkal };
#if defined(__MCPP_TARGET_WINDOWS__) || defined(_WIN32)
inline constexpr platform PLATFORM = platform::windows;
#elif defined(__APPLE__)
inline constexpr platform PLATFORM = platform::macos;
#else
inline constexpr platform PLATFORM = platform::linux;
#endif
constexpr platform host() noexcept { return PLATFORM; }

// The ok-target form, compiled here on a native target to see what each
// compiler says about it.
platform runtime_host() noexcept;                 // asks the environment (G8)
platform not_known_when_compiling() noexcept;     // never defined: a marker
constexpr platform host_ok() noexcept {
    if consteval { return not_known_when_compiling(); }
    else { return runtime_host(); }
}
