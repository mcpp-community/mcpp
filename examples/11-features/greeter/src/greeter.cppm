export module greeter;
import std;

export namespace greeter {

// `MCPP_FEATURE_SHOUT` is defined because `shout` is in `[features] default`.
// A consumer that builds with `--no-default-features` gets the other branch,
// and neither branch is named in the manifest.
inline std::string greet(std::string_view who) {
#ifdef MCPP_FEATURE_SHOUT
    std::string s = std::format("HELLO, {}", who);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s + "!";
#else
    return std::format("Hello, {}.", who);
#endif
}

} // namespace greeter
