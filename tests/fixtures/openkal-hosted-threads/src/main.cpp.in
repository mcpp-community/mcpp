import std;

namespace {
std::atomic<unsigned> arrived{0};
std::atomic<unsigned> handling{0};
std::atomic<unsigned> destroyed{0};
std::atomic<unsigned> unwound{0};
std::atomic<unsigned> passed{0};

struct local_state {
    unsigned value = 0;
    ~local_state() { if (value) destroyed.fetch_or(value); }
};
thread_local local_state local;

struct frame {
    unsigned bit;
    ~frame() { unwound.fetch_or(bit); }
};

void worker(unsigned bit) {
    const bool initially_zero = local.value == 0;
    local.value = bit;
    arrived.fetch_add(1, std::memory_order_release);
    while (arrived.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    bool caught = false;
    try {
        frame guard{bit};
        throw bit;
    } catch (unsigned value) {
        caught = value == bit;
        handling.fetch_add(1, std::memory_order_release);
        // Keep both exception handlers alive together: their TLS exception
        // state must remain independent as each worker observes its own value.
        while (handling.load(std::memory_order_acquire) != 2) std::this_thread::yield();
    }
    if (initially_zero && caught && local.value == bit && (unwound.load() & bit))
        passed.fetch_or(bit);
}
}

int main() {
    local.value = 4;
    std::thread first(worker, 1);
    std::thread second(worker, 2);
    first.join();
    second.join();
    // Joining must complete each worker's TLS teardown, while main's TLS stays
    // alive and independent. A main-thread-only exception cannot prove this.
    if (passed.load() != 3 || unwound.load() != 3 || destroyed.load() != 3
        || local.value != 4) {
        std::println("openkal hosted threads: FAILED ({}, {}, {}, {})",
                     passed.load(), unwound.load(), destroyed.load(), local.value);
        return 1;
    }
    std::println("openkal hosted threads: isolation, destructors and concurrent unwind ok");
}
