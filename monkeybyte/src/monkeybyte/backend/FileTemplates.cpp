#include "monkeybyte/backend/FileTemplates.h"

#include <cctype>
#include <ctime>

namespace monkeybyte {

std::string include_guard_name(const std::string& filename) {
    std::string guard;
    guard.reserve(filename.size());
    for (const unsigned char c : filename)
        guard += std::isalnum(c) ? static_cast<char>(std::toupper(c)) : '_';
    return guard;
}

namespace {

std::string cpp_header(const std::string& name) {
    const std::string guard = include_guard_name(name);
    return "#ifndef " + guard + "\n#define " + guard +
           "\n\n\n\n#endif // " + guard + "\n";
}

std::string cpp_source(const std::string& name) {
    return "// " + name + "\n\n";
}

std::string cmake_lists(const std::string&) {
    return
        "cmake_minimum_required(VERSION 3.28)\n"
        "project(my_project LANGUAGES CXX)\n"
        "\n"
        "set(CMAKE_CXX_STANDARD 23)\n"
        "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n"
        "\n"
        "add_executable(my_project main.cpp)\n";
}

std::string gitignore(const std::string&) {
    return
        "# CMake / build output\n"
        "build/\n"
        "build-*/\n"
        "\n"
        "# Compiled artifacts\n"
        "*.o\n"
        "*.obj\n"
        "*.a\n"
        "*.so\n"
        "*.exe\n"
        "\n"
        "# Editor\n"
        ".cache/\n"
        "*.swp\n";
}

std::string readme(const std::string&) {
    return "# Project Title\n\nShort description.\n";
}

std::string mit_license(const std::string&) {
    std::time_t t = std::time(nullptr);
    char year[8] = "????";
    if (const std::tm* tm = std::localtime(&t))
        std::strftime(year, sizeof(year), "%Y", tm);
    return std::string(
        "MIT License\n"
        "\n"
        "Copyright (c) ") + year + R"( [Your Name]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
)";
}

const std::vector<FileTemplate> kTemplates = {
    {"C++ Header (.h)", "header.h", cpp_header},
    {"C++ Source (.cpp)", "main.cpp", cpp_source},
    {"CMakeLists.txt", "CMakeLists.txt", cmake_lists},
    {".gitignore", ".gitignore", gitignore},
    {"README.md", "README.md", readme},
    {"LICENSE (MIT)", "LICENSE", mit_license},
};

} // namespace

const std::vector<FileTemplate>& file_templates() { return kTemplates; }

} // namespace monkeybyte
