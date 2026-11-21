#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <ranges>
#include <regex>
#include <sstream>
#include <string>
int main() {
    std::regex re("a+b");
    std::map<std::string, int> m{{"a", 1}};
    return std::regex_match(std::format("{}ab", "a"), re) ? 0 : 1;
}
