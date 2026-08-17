// Consuming through the MODULE interface. mcpp compiles the package's published
// `.cppm` to get a BMI — cheap, they are declarations — and links the
// definitions out of the prebuilt artifact.
import std;
import mathkit;

int main() {
    std::println("module : mk::add(2, 3) = {}, mk::scale(2.0) = {}",
                 mk::add(2, 3), mk::scale(2.0));
    return 0;
}
