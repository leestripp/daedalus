#include "monkeybyte/backend/FileKind.h"

#include <giomm/appinfo.h>
#include <giomm/contenttype.h>
#include <giomm/file.h>
#include <giomm/fileinfo.h>

#include <fstream>

namespace monkeybyte {

namespace {

// Cheap binary sniff: NUL anywhere in the first bytes ⇒ not text.
bool sniffs_as_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    char buf[8192];
    in.read(buf, sizeof(buf));
    const auto n = in.gcount();
    for (std::streamsize i = 0; i < n; ++i)
        if (buf[i] == '\0') return false;
    return true; // empty files count as text
}

std::string content_type_of(const std::string& path) {
    try {
        auto file = Gio::File::create_for_path(path);
        auto info = file->query_info(G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE);
        return info->get_content_type();
    } catch (...) {
        return {};
    }
}

} // namespace

bool is_text_like_file(const std::string& path) {
    const std::string type = content_type_of(path);
    if (type.empty()) return sniffs_as_text(path); // query failed
    if (type.starts_with("text/")) return true;
    // Zero-size files: "application/x-zerosize" on Linux, "inode/x-empty"
    // elsewhere — both are fine to open as an empty text buffer.
    if (type == "inode/x-empty" || type == "application/x-zerosize")
        return true;
    // shared-mime-info subclasses text-ish application types under
    // text/plain (application/json, application/xml, x-shellscript, …).
    if (Gio::content_type_is_a(type, "text/plain")) return true;
    // Unknown / generic binary type: fall back to a NUL sniff so text files
    // with exotic extensions still open in the editor.
    if (type == "application/octet-stream") return sniffs_as_text(path);
    return false;
}

bool launch_external(const std::string& path, std::string& error) {
    error.clear();
    const std::string type = content_type_of(path);
    if (type.empty()) {
        error = "could not determine file type: " + path;
        return false;
    }
    if (!Gio::AppInfo::get_default_for_type(type)) {
        error = "no application associated with " + type + " (" + path + ")";
        return false;
    }
    try {
        const std::string uri =
            Gio::File::create_for_path(path)->get_uri();
        Gio::AppInfo::launch_default_for_uri(uri);
        return true;
    } catch (const Glib::Error& e) {
        error = "failed to launch application for " + path + ": " +
                e.what();
        return false;
    }
}

} // namespace monkeybyte
