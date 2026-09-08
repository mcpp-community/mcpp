// The optional backend. A consumer that does not name the `metrics` feature
// never resolves this package, which is the criterion the README states.
export module counters;
import std;

export namespace counters {
inline int calls = 0;
inline void record() { ++calls; }
inline std::string report() { return std::format("{} call(s)", calls); }
}
