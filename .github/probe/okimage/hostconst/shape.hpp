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
// compiler says about it. Two shapes:
//   A: a constexpr function that branches on `if consteval` and calls a
//      non-constexpr marker in the constant branch (C++23 P2448 makes such a
//      function valid while it is not used in a constant expression);
//   B: a plain (non-constexpr) declaration, defined where the host is asked.
#if !defined(SHAPE_B)
platform runtime_host() noexcept;
platform not_known_when_compiling() noexcept;
constexpr platform host_ok() noexcept {
    if consteval { return not_known_when_compiling(); }
    else { return runtime_host(); }
}
#else
platform host_ok() noexcept;
#endif
