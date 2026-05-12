export module demo.greeter;
import std;
import demo.core;

export namespace demo::greeter {

inline std::string greet() {
    return "Hello, " + demo::core::greet_target() + "!";
}

}
