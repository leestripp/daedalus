#include <print>
#include <string_view>

int main() {
    constexpr std::string_view message = "Hello, World! (C++23)";
    std::println("{}", message);
    return 0;
}
