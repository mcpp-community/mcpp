import std;
import greeter;

#ifdef MCPP_FEATURE_METRICS
extern "C" void greeter_record_call();
#endif

int main() {
    std::println("{}", greeter::greet("world"));
#ifdef MCPP_FEATURE_METRICS
    greeter_record_call();
    std::println("metrics: on");
#else
    std::println("metrics: off");
#endif
    return 0;
}
