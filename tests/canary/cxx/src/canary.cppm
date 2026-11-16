// What the program and its test share: a module of its own over `import std`.
export module canary;
import std;

export namespace canary {

// Threads: the sum of 1..n over four threads.
long long parallel_sum(long long n) {
    std::atomic<long long> total{0};
    std::vector<std::jthread> workers;
    for (int w = 0; w < 4; ++w)
        workers.emplace_back([&total, n, w] {
            long long part = 0;
            for (long long i = w + 1; i <= n; i += 4) part += i;
            total += part;
        });
    workers.clear();
    return total.load();
}

// Exceptions: thrown across a function and caught by type.
std::string thrown_and_caught() {
    try {
        throw std::runtime_error("carried");
    } catch (const std::exception& e) {
        return e.what();
    }
}

} // namespace canary
