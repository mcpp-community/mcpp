// Compiled only when the `metrics` feature is active: `[features.metrics]`
// names this file. Under any other feature set it is not in the source set,
// so `counters` is not linked and not resolved.
import counters;

extern "C" void greeter_record_call() { counters::record(); }
