import std;
import demo.greeter;
import demo.core;

int main() {
    std::println("{}", demo::greeter::greet());
    std::println("core version: {}", demo::core::version());
    return 0;
}
