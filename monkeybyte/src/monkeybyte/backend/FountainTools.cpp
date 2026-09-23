#include "monkeybyte/backend/FountainTools.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace monkeybyte::fountain {

namespace {

namespace fs = std::filesystem;

// Single-quote a path for /bin/sh.
std::string shq(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
    }
    return out + "'";
}

bool on_path(const std::string& name) {
    const std::string cmd = "command -v " + shq(name) + " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0;
}

// Runs a converter producing `out` from `in`. Returns false on failure.
bool run(const std::string& converter, const std::string& in,
         const std::string& out, const char* format) {
    std::string cmd;
    if (converter == "wrap") {
        // wrap pdf|html input.fountain — writes beside input; we then move.
        cmd = "wrap " + std::string(format) + " " + shq(in) + " >/dev/null 2>&1";
        if (std::system(cmd.c_str()) != 0) return false;
        const fs::path produced =
            fs::path(in).replace_extension(std::string(".") + format);
        std::error_code ec;
        fs::rename(produced, out, ec);
        return !ec;
    }
    // screenplain input.fountain output.pdf (format inferred from extension;
    // --format for html).
    cmd = "screenplain " + shq(in) + " " + shq(out);
    if (std::string(format) == "html")
        cmd = "screenplain --format html " + shq(in) + " " + shq(out);
    cmd += " >/dev/null 2>&1";
    return std::system(cmd.c_str()) == 0 && fs::exists(out);
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Unique temp workspace for a conversion; cleaned by the caller's scope.
struct TempDir {
    fs::path dir;
    TempDir() {
        dir = fs::temp_directory_path() /
              ("mb-fountain-" + std::to_string(
                  static_cast<unsigned long long>(
                      std::hash<std::string>{}(
                          std::to_string(std::rand()) +
                          std::to_string(reinterpret_cast<uintptr_t>(this))))));
        std::error_code ec;
        fs::create_directories(dir, ec);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

} // namespace

std::string find_converter() {
    if (on_path("wrap")) return "wrap";
    if (on_path("screenplain")) return "screenplain";
    return "";
}

std::string to_html(const std::string& fountain_source) {
    const std::string conv = find_converter();
    if (conv.empty()) return "";
    TempDir tmp;
    if (tmp.dir.empty()) return "";
    const fs::path in = tmp.dir / "script.fountain";
    const fs::path out = tmp.dir / "script.html";
    {
        std::ofstream f(in, std::ios::trunc);
        if (!f) return "";
        f << fountain_source;
    }
    if (!run(conv, in.string(), out.string(), "html")) return "";
    return read_all(out);
}

bool to_pdf(const std::string& fountain_path, const std::string& pdf_path,
            std::string* error) {
    const std::string conv = find_converter();
    if (conv.empty()) {
        if (error) *error = kInstallHint;
        return false;
    }
    if (!run(conv, fountain_path, pdf_path, "pdf")) {
        if (error)
            *error = "Converter '" + conv + "' failed on " + fountain_path;
        return false;
    }
    return true;
}

} // namespace monkeybyte::fountain
