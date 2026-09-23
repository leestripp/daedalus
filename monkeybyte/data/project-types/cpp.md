---
id: cpp
name: C++ (C++23)
description: Modern C++ code project (CMake, C++23)
detect: CMakeLists.txt, *.cpp, *.h, *.cc, *.hpp
---

This is a modern C++ project. Default to C++23 unless the build files say
otherwise. Conventions:

- Read CMakeLists.txt before assuming target names, standards, or options.
- Prefer standard-library facilities over new dependencies; ask before
  adding any dependency or FetchContent entry.
- Match the existing style: naming, include order, formatting (clang-format
  config if present), header vs. implementation split.
- Warnings are errors in spirit: code you write must compile warning-clean
  under the project's existing warning flags.
- After code changes, build and run the project's tests to verify; treat a
  red build as your bug to fix, not something to hand back.
- Use const-correctness, RAII, std::string_view/span where appropriate, and
  avoid raw new/delete.
