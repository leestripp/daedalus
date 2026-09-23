#pragma once

#include <functional>
#include <string>
#include <vector>

namespace monkeybyte {

// A starter-file template for "Add File from Template…" (design §4.2).
// `make_content` receives the filename the user chose (basename only) and
// returns the file contents.
struct FileTemplate {
    std::string label;        // shown in the picker dialog
    std::string default_name; // pre-filled filename
    std::function<std::string(const std::string&)> make_content;
};

// Uppercase alnum run of `filename` with everything else folded to '_'
// (e.g. "foo-bar.h" → "FOO_BAR_H") — used for C++ include guards.
std::string include_guard_name(const std::string& filename);

// The built-in templates, in picker order.
const std::vector<FileTemplate>& file_templates();

} // namespace monkeybyte
