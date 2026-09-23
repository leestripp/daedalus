#pragma once

#include <algorithm>
#include <string>
#include <unordered_map>

namespace monkeybyte {

// Symbolic icon names (data/icons/mb-*.svg) keyed by lowercase extension or
// exact filename (design §4.2). GTK recolors *-symbolic icons from CSS
// foreground, so they follow the active theme.
inline std::string file_icon_name(const std::string& filename, bool is_dir,
                                  bool expanded = false) {
    if (is_dir) return expanded ? "mb-folder-open-symbolic" : "mb-folder-symbolic";

    static const std::unordered_map<std::string, std::string> by_name = {
        {"dockerfile", "mb-file-config-symbolic"},
        {"makefile", "mb-file-config-symbolic"},
        {"cmakelists.txt", "mb-file-config-symbolic"},
        {".gitignore", "mb-file-lock-symbolic"},
        {".env", "mb-file-lock-symbolic"},
        {"readme", "mb-file-doc-symbolic"},
        {"readme.md", "mb-file-doc-symbolic"},
        {"license", "mb-file-doc-symbolic"},
        {"package.json", "mb-file-data-symbolic"},
        {"cargo.toml", "mb-file-data-symbolic"},
        {"go.mod", "mb-file-data-symbolic"},
        {"requirements.txt", "mb-file-data-symbolic"},
    };
    static const std::unordered_map<std::string, std::string> by_ext = {
        {"js", "mb-file-code-symbolic"}, {"mjs", "mb-file-code-symbolic"},
        {"jsx", "mb-file-code-symbolic"}, {"ts", "mb-file-code-symbolic"},
        {"tsx", "mb-file-code-symbolic"},
        {"html", "mb-file-code-symbolic"}, {"htm", "mb-file-code-symbolic"},
        {"css", "mb-file-code-symbolic"}, {"scss", "mb-file-code-symbolic"},
        {"sass", "mb-file-code-symbolic"}, {"less", "mb-file-code-symbolic"},
        {"cpp", "mb-file-code-symbolic"}, {"cc", "mb-file-code-symbolic"},
        {"cxx", "mb-file-code-symbolic"}, {"hpp", "mb-file-code-symbolic"},
        {"h", "mb-file-code-symbolic"}, {"c", "mb-file-code-symbolic"},
        {"rs", "mb-file-code-symbolic"}, {"go", "mb-file-code-symbolic"},
        {"java", "mb-file-code-symbolic"}, {"kt", "mb-file-code-symbolic"},
        {"kts", "mb-file-code-symbolic"}, {"cs", "mb-file-code-symbolic"},
        {"php", "mb-file-code-symbolic"}, {"rb", "mb-file-code-symbolic"},
        {"swift", "mb-file-code-symbolic"}, {"vue", "mb-file-code-symbolic"},
        {"svelte", "mb-file-code-symbolic"}, {"astro", "mb-file-code-symbolic"},
        {"py", "mb-file-script-symbolic"}, {"pyc", "mb-file-script-symbolic"},
        {"sh", "mb-file-script-symbolic"}, {"bash", "mb-file-script-symbolic"},
        {"zsh", "mb-file-script-symbolic"}, {"fish", "mb-file-script-symbolic"},
        {"md", "mb-file-doc-symbolic"}, {"markdown", "mb-file-doc-symbolic"},
        {"fountain", "mb-file-doc-symbolic"}, {"pdf", "mb-file-doc-symbolic"},
        {"txt", "mb-file-doc-symbolic"}, {"pdf", "mb-file-doc-symbolic"},
        {"png", "mb-file-image-symbolic"}, {"jpg", "mb-file-image-symbolic"},
        {"jpeg", "mb-file-image-symbolic"}, {"gif", "mb-file-image-symbolic"},
        {"svg", "mb-file-image-symbolic"}, {"webp", "mb-file-image-symbolic"},
        {"bmp", "mb-file-image-symbolic"}, {"ico", "mb-file-image-symbolic"},
        {"zip", "mb-file-archive-symbolic"}, {"tar", "mb-file-archive-symbolic"},
        {"gz", "mb-file-archive-symbolic"}, {"rar", "mb-file-archive-symbolic"},
        {"7z", "mb-file-archive-symbolic"},
        {"json", "mb-file-data-symbolic"}, {"sql", "mb-file-data-symbolic"},
        {"xml", "mb-file-data-symbolic"},
        {"yaml", "mb-file-config-symbolic"}, {"yml", "mb-file-config-symbolic"},
        {"toml", "mb-file-config-symbolic"}, {"ini", "mb-file-config-symbolic"},
        {"cfg", "mb-file-config-symbolic"}, {"cmake", "mb-file-config-symbolic"},
        {"lock", "mb-file-lock-symbolic"},
    };

    std::string lower = filename;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (auto it = by_name.find(lower); it != by_name.end()) return it->second;

    const auto dot = lower.rfind('.');
    if (dot != std::string::npos && dot + 1 < lower.size()) {
        if (auto it = by_ext.find(lower.substr(dot + 1)); it != by_ext.end())
            return it->second;
    }
    return "mb-file-symbolic";
}

} // namespace monkeybyte
