#include "tools/FileTool.h"
#include "core/FileIO.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <regex>
#include <string>
#include <stdexcept>

// ========== Helper Functions ==========

// Check if cwd has been initialized (defined in ToolHandler.cpp)
namespace ToolHandler {
    bool is_cwd_initialized();
    std::filesystem::path get_current_working_directory();
}

// ========== ReadFileTool ==========

json ReadFileTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the file to read (relative to Project Root)"}
                    })},
                    {"line_numbers", json::object({
                        {"type", "boolean"},
                        {"description", "Whether to include line numbers in the output (default: false)"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json ReadFileTool::execute(const json& args, FileIO& file_io) {
    return execute(args, file_io, nullptr);
}

json ReadFileTool::execute(const json& args, FileIO& file_io,
                            ToolProgressFn progress) {
    auto emit_progress = [&progress](const std::string& stage,
                                     int percent,
                                     const std::string& message) {
        if (!progress) return;
        json p = {
            {"stage",   stage},
            {"percent", percent},
            {"message", message},
        };
        progress(p);
    };

    std::string path;
    bool line_numbers = false;
    try {
        if (args.is_object() && args.contains("path")) {
            path = args["path"].get<std::string>();
            line_numbers = args.value("line_numbers", false);
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("path")) {
                path = parsed_args["path"].get<std::string>();
                line_numbers = parsed_args.value("line_numbers", false);
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'path' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for read_file"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing path argument: ") + e.what()}});
    }

    try {
        // §BUG-003 fix: resolve relative paths against the tracked project
        // root (ToolHandler cwd if initialized, otherwise FileIO base).
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized()
            ? ToolHandler::get_current_working_directory()
            : file_io.get_project_path();
        std::filesystem::path resolved_path;
        if (std::filesystem::path(path).is_absolute()) {
            resolved_path = std::filesystem::absolute(path);
        } else {
            resolved_path = std::filesystem::absolute(base_dir / path);
        }

        // §Phase C2: emit "reading" before the slurp. Only emit AFTER
        // arg validation passes — matches BashTool's pattern of never
        // firing a "starting" event for a tool that's about to return
        // an error. The 0% mark lets the webui card show an
        // indeterminate shimmer while the rdbuf is in flight.
        emit_progress("reading", 0, "Reading " + path);
        std::string content = file_io.read_file_content(resolved_path);

        // Add line numbers if requested
        if (line_numbers) {
            std::istringstream stream(content);
            std::ostringstream numbered_content;
            std::string line;
            int line_num = 1;
            while (std::getline(stream, line)) {
                numbered_content << line_num << "\t" << line << "\n";
                line_num++;
            }
            content = numbered_content.str();
        }

        return json::object({
            {"status", "success"},
            {"path", path},
            {"content", content}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== ListDirectoryTool ==========

json ListDirectoryTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the directory to list (relative to Project Root)"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json ListDirectoryTool::execute(const json& args, FileIO& file_io) {
    std::string path_str;
    try {
        if (args.is_object() && args.contains("path")) {
            path_str = args["path"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("path")) {
                path_str = parsed_args["path"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'path' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for list_directory"}});
        }

        if (path_str.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing path argument: ") + e.what()}});
    }

    // §Fix: wrap the listing body in try/catch like every sibling tool —
    // std::filesystem calls (absolute/exists/directory_iterator) throw on
    // e.g. permission-denied directories and previously escaped execute().
    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ?
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path path;
        if (std::filesystem::path(path_str).is_absolute()) {
            path = std::filesystem::absolute(path_str);
        } else {
            path = std::filesystem::absolute(base_dir / path_str);
        }

        // Trust OS permissions - any accessible directory can be listed
        if (!std::filesystem::exists(path) || !std::filesystem::is_directory(path)) {
            return json::object({{"status", "error"}, {"message", "Path is not a valid directory: " + path_str}});
        }

        json contents = json::array();
        for (const auto& entry : std::filesystem::directory_iterator(path)) {
            json entry_json = {
                {"name", entry.path().filename().string()},
                {"type", entry.is_directory() ? "directory" : "file"}
            };

            // Add file size
            if (entry.is_regular_file()) {
                try {
                    entry_json["size"] = entry.file_size();
                } catch (...) {
                    // Ignore errors
                }
            }

            // Add modification timestamp (ISO 8601 format)
            try {
                auto time = entry.last_write_time();
                auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());
                std::time_t time_st = std::chrono::system_clock::to_time_t(sctp);
                std::tm* tm = std::localtime(&time_st);
                char buffer[32];
                std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", tm);
                entry_json["modified"] = std::string(buffer);
            } catch (...) {
                // Ignore errors
            }

            // Add permissions (like -rwxr-xr-x)
            try {
                auto status = entry.status();
                auto perms = status.permissions();
                std::string perm_str;
                perm_str += (entry.is_directory() ? 'd' : '-');
                perm_str += ((perms & std::filesystem::perms::owner_read) != std::filesystem::perms::none ? 'r' : '-');
                perm_str += ((perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none ? 'w' : '-');
                perm_str += ((perms & std::filesystem::perms::owner_exec) != std::filesystem::perms::none ? 'x' : '-');
                perm_str += ((perms & std::filesystem::perms::group_read) != std::filesystem::perms::none ? 'r' : '-');
                perm_str += ((perms & std::filesystem::perms::group_write) != std::filesystem::perms::none ? 'w' : '-');
                perm_str += ((perms & std::filesystem::perms::group_exec) != std::filesystem::perms::none ? 'x' : '-');
                perm_str += ((perms & std::filesystem::perms::others_read) != std::filesystem::perms::none ? 'r' : '-');
                perm_str += ((perms & std::filesystem::perms::others_write) != std::filesystem::perms::none ? 'w' : '-');
                perm_str += ((perms & std::filesystem::perms::others_exec) != std::filesystem::perms::none ? 'x' : '-');
                entry_json["permissions"] = perm_str;
            } catch (...) {
                // Ignore errors
            }

            // Add line count for files
            if (entry.is_regular_file()) {
                try {
                    std::ifstream file(entry.path());
                    if (file.is_open()) {
                        size_t line_count = 0;
                        std::string line;
                        while (std::getline(file, line)) {
                            line_count++;
                        }
                        entry_json["lines"] = line_count;
                    }
                } catch (...) {
                    // Ignore errors
                }
            }
            contents.push_back(entry_json);
        }

        return json::object({
            {"status", "success"},
            {"path", path_str},
            {"contents", contents}
        });
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== CreateFileTool ==========

json CreateFileTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path where the file should be created (relative to Project Root)"}
                    })},
                    {"content", json::object({
                        {"type", "string"},
                        {"description", "The content to write to the file"}
                    })}
                })},
                {"required", json::array({"path", "content"})}
            })}
        }}
    });
}

json CreateFileTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    std::string content;
    try {
        if (args.is_object()) {
            path = args.value("path", "");
            content = args.value("content", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            path = parsed_args.value("path", "");
            content = parsed_args.value("content", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for create_file"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ? 
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path resolved_path;
        if (std::filesystem::path(path).is_absolute()) {
            resolved_path = std::filesystem::absolute(path);
        } else {
            resolved_path = std::filesystem::absolute(base_dir / path);
        }

        file_io.write_file_content(resolved_path, content);
        // Return original path (as provided) to reinforce relative path usage
        return json::object({{"status", "success"}, {"path", path}, {"message", "File created successfully."}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== CreateDirectoryTool ==========

json CreateDirectoryTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path of the directory to create (relative to Project Root)"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json CreateDirectoryTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    try {
        if (args.is_object()) {
            path = args.value("path", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            path = parsed_args.value("path", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for create_directory"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ? 
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path resolved_path;
        if (std::filesystem::path(path).is_absolute()) {
            resolved_path = std::filesystem::absolute(path);
        } else {
            resolved_path = std::filesystem::absolute(base_dir / path);
        }

        file_io.create_directory(resolved_path);
        // Return original path (as provided) to reinforce relative path usage
        return json::object({{"status", "success"}, {"path", path}, {"message", "Directory created successfully."}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== ModifyFileTool ==========

json ModifyFileTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the file to modify (relative to Project Root)"}
                    })},
                    {"content", json::object({
                        {"type", "string"},
                        {"description", "The new content to write to the file"}
                    })}
                })},
                {"required", json::array({"path", "content"})}
            })}
        }}
    });
}

json ModifyFileTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    std::string content;
    try {
        if (args.is_object()) {
            path = args.value("path", "");
            content = args.value("content", "");
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            path = parsed_args.value("path", "");
            content = parsed_args.value("content", "");
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for modify_file"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing arguments: ") + e.what()}});
    }

    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ? 
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path resolved_path;
        if (std::filesystem::path(path).is_absolute()) {
            resolved_path = std::filesystem::absolute(path);
        } else {
            resolved_path = std::filesystem::absolute(base_dir / path);
        }

        file_io.write_file_content(resolved_path, content);
        // Return original path (as provided) to reinforce relative path usage
        return json::object({{"status", "success"}, {"path", path}, {"message", "File modified successfully."}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== EditFilePatchTool ==========
// (V2: completely rewritten for better string matching, error messages, and replace_all)
//
// Matching strategies tried in order:
//   1. Exact substring match (fast, common case)
//   2. Line-normalized match (collapse whitespace within lines, ignore trailing)
//   3. Fuzzy match (Levenshtein distance, only if fuzzy=true)
//   4. Regex (only if use_regex=true)
//
// On success: returns status, replacements count, first match line number
// On failure: returns the line context around the search area so the agent
//             can see what was actually in the file and adjust.

json EditFilePatchTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the file to modify (relative to Project Root)"}
                    })},
                    {"old_str", json::object({
                        {"type", "string"},
                        {"description", "The string to find and replace. TIP: copy-paste from read_file output, do not re-type. Whitespace is flexible."}
                    })},
                    {"new_str", json::object({
                        {"type", "string"},
                        {"description", "The new string to replace old_str with"}
                    })},
                    {"line_start", json::object({
                        {"type", "integer"},
                        {"description", "Start line for line-based replacement (1-indexed). Use this for precise edits by line number."}
                    })},
                    {"line_end", json::object({
                        {"type", "integer"},
                        {"description", "End line for line-based replacement (1-indexed, inclusive). Default: same as line_start."}
                    })},
                    {"regex", json::object({
                        {"type", "boolean"},
                        {"description", "Treat old_str as a regex pattern (default: false). Use this for complex patterns."}
                    })},
                    {"replace_all", json::object({
                        {"type", "boolean"},
                        {"description", "Replace ALL occurrences instead of just the first (default: false)"}
                    })},
                    {"fuzzy", json::object({
                        {"type", "boolean"},
                        {"description", "Enable fuzzy matching with small Levenshtein distance tolerance (default: false). Use for typos."}
                    })},
                    {"context_lines", json::object({
                        {"type", "integer"},
                        {"description", "Number of lines to show around the match (default: 5)"}
                    })}
                })}
            })}
        }}
    });
}

// Helper: normalize a single line for comparison
// - Collapses multiple spaces/tabs to single space
// - Trims trailing whitespace (not leading - that's important for indent!)
// - Preserves line content otherwise
static std::string normalize_line(const std::string& line) {
    std::string result;
    result.reserve(line.size());
    bool last_was_space = false;
    for (char c : line) {
        if (c == ' ' || c == '\t') {
            if (!last_was_space) {
                result += ' ';
                last_was_space = true;
            }
        } else {
            result += c;
            last_was_space = false;
        }
    }
    // Trim trailing whitespace
    while (!result.empty() && (result.back() == ' ' || result.back() == '\t')) {
        result.pop_back();
    }
    return result;
}

// Helper: normalize whole content line-by-line (preserves line structure)
static std::vector<std::string> normalize_lines(const std::string& content) {
    std::vector<std::string> result;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        result.push_back(normalize_line(line));
    }
    return result;
}

// Helper: split content into lines (preserving line structure)
static std::vector<std::string> split_lines(const std::string& content) {
    std::vector<std::string> result;
    std::istringstream stream(content);
    std::string line;
    while (std::getline(stream, line)) {
        result.push_back(line);
    }
    // If content ends with newline, add empty line for that
    if (!content.empty() && content.back() == '\n') {
        result.push_back("");
    }
    return result;
}

// Helper: join lines back into a string
static std::string join_lines(const std::vector<std::string>& lines) {
    std::string result;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) result += '\n';
        result += lines[i];
    }
    return result;
}

// Helper: compute Levenshtein distance (for fuzzy matching)
static size_t levenshtein_distance(const std::string& a, const std::string& b, size_t max_threshold = SIZE_MAX) {
    const size_t m = a.size();
    const size_t n = b.size();
    if (m == 0) return n;
    if (n == 0) return m;
    // §Fix: compare in unsigned arithmetic — the old
    // `(long long)max_threshold` cast turned the default SIZE_MAX
    // into -1, making this short-circuit fire unconditionally.
    if ((m > n ? m - n : n - m) > max_threshold) {
        return max_threshold + 1;  // Short-circuit: too different
    }
    // §Fix(perf): two-row DP instead of a full (m+1)x(n+1) matrix,
    // with early abandon once every cell of a row exceeds the
    // threshold (the distance can only grow from there).
    std::vector<size_t> prev(n + 1), cur(n + 1);
    for (size_t j = 0; j <= n; ++j) prev[j] = j;
    for (size_t i = 1; i <= m; ++i) {
        cur[0] = i;
        size_t row_min = cur[0];
        for (size_t j = 1; j <= n; ++j) {
            size_t cost = (a[i-1] == b[j-1]) ? 0 : 1;
            cur[j] = std::min({
                prev[j] + 1,            // deletion
                cur[j-1] + 1,           // insertion
                prev[j-1] + cost        // substitution
            });
            row_min = std::min(row_min, cur[j]);
        }
        if (row_min > max_threshold) {
            return max_threshold + 1;  // Early abandon: cannot recover
        }
        std::swap(prev, cur);
    }
    return prev[n];
}

// §Fix: returns the exact byte position of the best match (or
// std::string::npos). Previously only {line, col} was returned and the
// caller re-derived the offset of the line START, so a match beginning
// mid-line overwrote unrelated leading text.
// §Fix(perf): only windows starting at line starts are evaluated, so
// the DP runs O(lines) times instead of O(file_size) times.
static size_t find_fuzzy_match(
    const std::string& content,
    const std::string& pattern,
    int max_distance
) {
    if (content.size() < pattern.size()) return std::string::npos;

    // Sliding window over line starts: check each position
    size_t best_pos = std::string::npos;
    size_t best_dist = max_distance + 1;
    for (size_t i = 0; i <= content.size() - pattern.size(); ++i) {
        if (i > 0 && content[i-1] != '\n') continue;  // line starts only
        std::string window = content.substr(i, pattern.size());
        size_t dist = levenshtein_distance(window, pattern, max_distance);
        if (dist < best_dist) {
            best_dist = dist;
            best_pos = i;
            if (dist == 0) break;  // exact match
        }
    }
    return best_pos;
}

// Helper: show context around a line in the file
static std::string show_context(const std::string& content, int center_line, int context_lines) {
    std::vector<std::string> lines = split_lines(content);
    int start = std::max(1, center_line - context_lines);
    int end = std::min((int)lines.size(), center_line + context_lines);

    std::ostringstream out;
    out << "\nFile context (lines " << start << "-" << end << " of " << lines.size() << "):\n";
    int max_line_num = std::to_string(end).size();
    for (int i = start; i <= end; ++i) {
        std::string marker = (i == center_line) ? ">>>" : "   ";
        out << marker << " " << std::setw(max_line_num) << i << ": " << lines[i-1] << "\n";
    }
    return out.str();
}

json EditFilePatchTool::execute(const json& args, FileIO& file_io) {
    // === Parse arguments ===
    std::string path, old_str, new_str;
    bool use_regex = false;
    bool replace_all = false;
    bool fuzzy = false;
    int line_start = -1, line_end = -1;
    int context_lines = 5;

    // Declare these outside the try block so they're visible later
    bool has_old_str = false;
    bool has_line_range = false;

    try {
        if (args.is_object()) {
            path = args.value("path", "");
            old_str = args.value("old_str", "");
            new_str = args.value("new_str", "");
            use_regex = args.value("regex", false);
            replace_all = args.value("replace_all", false);
            fuzzy = args.value("fuzzy", false);
            line_start = args.value("line_start", -1);
            line_end = args.value("line_end", -1);
            context_lines = args.value("context_lines", 5);
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            path = parsed_args.value("path", "");
            old_str = parsed_args.value("old_str", "");
            new_str = parsed_args.value("new_str", "");
            use_regex = parsed_args.value("regex", false);
            replace_all = parsed_args.value("replace_all", false);
            fuzzy = parsed_args.value("fuzzy", false);
            line_start = parsed_args.value("line_start", -1);
            line_end = parsed_args.value("line_end", -1);
            context_lines = parsed_args.value("context_lines", 5);
        } else {
            return json::object({
                {"status", "error"},
                {"message", "Invalid arguments format for edit_file_patch"},
                {"permanent_failure", true}
            });
        }

        if (path.empty()) {
            return json::object({
                {"status", "error"},
                {"message", "Missing or empty 'path' argument"},
                {"hint", "Provide a path to the file to modify"},
                {"permanent_failure", true}
            });
        }

        // Determine mode
        has_old_str = !old_str.empty();
        has_line_range = (line_start > 0);
        if (line_end == -1) line_end = line_start;  // single line

        if (!has_old_str && !has_line_range) {
            return json::object({
                {"status", "error"},
                {"message", "Missing search criteria. Provide either 'old_str' or 'line_start'/'line_end'."},
                {"hint", "Use old_str for string search, or line_start for line-based replacement"},
                {"permanent_failure", true}
            });
        }
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error parsing arguments: ") + e.what()},
            {"permanent_failure", true}
        });
    }

    // === Resolve file path ===
    std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ?
        ToolHandler::get_current_working_directory() : file_io.get_project_path();
    std::filesystem::path target_path;
    if (std::filesystem::path(path).is_absolute()) {
        target_path = std::filesystem::absolute(path);
    } else {
        target_path = std::filesystem::absolute(base_dir / path);
    }

    if (!std::filesystem::exists(target_path)) {
        return json::object({
            {"status", "error"},
            {"message", "File does not exist: " + path},
            {"hint", "Use read_file first to verify the file exists"},
            {"permanent_failure", true}
        });
    }

    // === Read file content ===
    std::string content = file_io.read_file_content(target_path);
    int replacements_made = 0;
    int first_match_line = -1;

    try {
        // === Strategy 1: Line-based replacement ===
        if (has_line_range) {
            std::vector<std::string> lines = split_lines(content);
            if (line_start > (int)lines.size() || line_end > (int)lines.size()) {
                return json::object({
                    {"status", "error"},
                    {"message", "Line range " + std::to_string(line_start) + "-" + std::to_string(line_end) +
                                " out of bounds. File has " + std::to_string(lines.size()) + " lines."},
                    {"hint", "Use read_file with line_numbers=true to see line numbers"},
                    {"permanent_failure", true}
                });
            }
            if (line_end < line_start) {
                return json::object({
                    {"status", "error"},
                    {"message", "line_end (" + std::to_string(line_end) + ") must be >= line_start (" + std::to_string(line_start) + ")"},
                    {"permanent_failure", true}
                });
            }

            // Replace lines [line_start, line_end] with new_str
            // new_str can be multi-line (split on \n)
            std::vector<std::string> new_lines;
            std::istringstream new_stream(new_str);
            std::string nl;
            while (std::getline(new_stream, nl)) {
                new_lines.push_back(nl);
            }

            std::vector<std::string> result;
            result.reserve(lines.size() - (line_end - line_start + 1) + new_lines.size());
            for (int i = 1; i < line_start; ++i) result.push_back(lines[i-1]);
            for (const auto& nl : new_lines) result.push_back(nl);
            for (size_t i = line_end + 1; i <= lines.size(); ++i) result.push_back(lines[i-1]);

            // §Fix: only restore a trailing newline if the ORIGINAL
            // content ended with one — previously this fired whenever
            // the edit reached the last line, adding a newline the
            // original file didn't have.
            bool original_had_trailing_newline = !content.empty() && content.back() == '\n';
            content = join_lines(result);
            // Preserve trailing newline if original had one
            if (original_had_trailing_newline && !content.empty() && content.back() != '\n' && lines.size() == (size_t)line_end) {
                content += '\n';
            }
            replacements_made = 1;
            first_match_line = line_start;
        }
        // === Strategy 2: Regex-based search ===
        else if (use_regex) {
            try {
                std::regex pattern(old_str);
                std::string new_content;
                size_t pos = 0;
                std::sregex_iterator it(content.begin(), content.end(), pattern);
                std::sregex_iterator end;
                for (; it != end; ++it) {
                    std::smatch match = *it;
                    size_t match_pos = match.position();
                    size_t match_len = match.length();
                    if (first_match_line == -1) {
                        first_match_line = (int)std::count(content.begin(), content.begin() + match_pos, '\n') + 1;
                    }
                    new_content.append(content, pos, match_pos - pos);
                    new_content.append(new_str);
                    pos = match_pos + match_len;
                    replacements_made++;
                    if (!replace_all) break;
                }
                new_content.append(content, pos, std::string::npos);
                if (replacements_made == 0) {
                    return json::object({
                        {"status", "error"},
                        {"message", "Regex pattern did not match any content in the file."},
                        {"hint", "Test your regex with a tool like regex_search or grep first"},
                        {"context", show_context(content, std::max(1, first_match_line), context_lines)},
                        {"permanent_failure", true}
                    });
                }
                content = new_content;
            } catch (const std::regex_error& e) {
                return json::object({
                    {"status", "error"},
                    {"message", std::string("Invalid regex pattern: ") + e.what()},
                    {"permanent_failure", true}
                });
            }
        }
        // === Strategy 3: Exact substring match (fastest) ===
        else if (!old_str.empty()) {
            size_t pos = 0;
            std::string new_content;
            while (pos < content.size()) {
                size_t found = content.find(old_str, pos);
                if (found == std::string::npos) {
                    new_content.append(content, pos, std::string::npos);
                    break;
                }
                if (first_match_line == -1) {
                    first_match_line = (int)std::count(content.begin(), content.begin() + found, '\n') + 1;
                }
                new_content.append(content, pos, found - pos);
                new_content.append(new_str);
                pos = found + old_str.size();
                replacements_made++;
                if (!replace_all) {
                    new_content.append(content, pos, std::string::npos);
                    break;
                }
            }
            // CRITICAL: assign new_content back to content so the write_file_content call at the
            // end actually writes the patched file. Without this, the file is unchanged!
            content = std::move(new_content);
            if (replacements_made == 0 && !fuzzy) {
                // Try line-normalized match
                std::vector<std::string> orig_lines = split_lines(content);
                std::vector<std::string> norm_orig = normalize_lines(content);
                std::vector<std::string> norm_old = normalize_lines(old_str);

                // Find first line of old_str in normalized orig
                if (!norm_old.empty()) {
                    std::string first_norm = norm_old[0];
                    for (size_t i = 0; i < norm_orig.size(); ++i) {
                        if (norm_orig[i] == first_norm) {
                            // Check if all of norm_old matches starting at i
                            if (i + norm_old.size() <= norm_orig.size()) {
                                bool all_match = true;
                                for (size_t j = 0; j < norm_old.size(); ++j) {
                                    if (norm_orig[i + j] != norm_old[j]) { all_match = false; break; }
                                }
                                if (all_match) {
                                    // Replace original lines [i, i+norm_old.size())
                                    int start_line = (int)(i + 1);
                                    std::vector<std::string> new_lines;
                                    for (size_t k = 0; k < i; ++k) new_lines.push_back(orig_lines[k]);
                                    std::istringstream new_stream(new_str);
                                    std::string nl;
                                    while (std::getline(new_stream, nl)) new_lines.push_back(nl);
                                    for (size_t k = i + norm_old.size(); k < orig_lines.size(); ++k) new_lines.push_back(orig_lines[k]);
                                    content = join_lines(new_lines);
                                    replacements_made = 1;
                                    first_match_line = start_line;
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            if (replacements_made == 0 && fuzzy) {
                // Try fuzzy match
                int max_dist = std::max(2, (int)(old_str.size() / 20));  // 5% tolerance
                // §Fix: find_fuzzy_match returns the exact byte offset of
                // the match; replace there directly instead of re-deriving
                // the start of the line (which ignored the column).
                size_t found = find_fuzzy_match(content, old_str, max_dist);
                if (found != std::string::npos) {
                    first_match_line = (int)std::count(content.begin(), content.begin() + found, '\n') + 1;
                    content.replace(found, old_str.size(), new_str);
                    replacements_made = 1;
                }
            }
            if (replacements_made == 0) {
                return json::object({
                    {"status", "error"},
                    {"message", "Could not find old_str in the file (tried exact match + line-normalized match)."},
                    {"hint", "Common causes: (1) whitespace/indentation mismatch, (2) file has changed since you read it, (3) typo. Use read_file with line_numbers=true to see exact content, then copy-paste old_str exactly."},
                    {"hint2", "If the issue is whitespace, try setting fuzzy=true for Levenshtein-distance matching."},
                    {"old_str_provided", old_str.substr(0, 200) + (old_str.size() > 200 ? "..." : "")},
                    {"old_str_length", old_str.size()},
                    {"file_size", content.size()},
                    {"context", show_context(content, 1, context_lines)},
                    {"permanent_failure", true}
                });
            }
            if (replacements_made > 0 && use_regex == false && !replace_all) {
                // If we did line-normalized match and there's only one match, the new_content
                // from exact-match path was wrong - we need to use the line-based content.
                // (This is handled by setting content above when norm_old matches.)
            }
        }

        // === Write back ===
        file_io.write_file_content(target_path, content);

        return json::object({
            {"status", "success"},
            {"path", path},
            {"message", "Patch applied successfully."},
            {"replacements", replacements_made},
            {"first_match_line", first_match_line}
        });
    } catch (const std::exception& e) {
        return json::object({
            {"status", "error"},
            {"message", std::string("Error applying patch: ") + e.what()},
            {"permanent_failure", true}
        });
    }
}


// ========== DeleteFileTool ==========

json DeleteFileTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the file to delete (relative to Project Root)"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json DeleteFileTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    try {
        if (args.is_object() && args.contains("path")) {
            path = args["path"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("path")) {
                path = parsed_args["path"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'path' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for delete_file"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing path argument: ") + e.what()}});
    }

    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ? 
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path target_path;
        if (std::filesystem::path(path).is_absolute()) {
            target_path = std::filesystem::absolute(path);
        } else {
            target_path = std::filesystem::absolute(base_dir / path);
        }

        // Trust OS permissions - any accessible file can be deleted
        if (!std::filesystem::exists(target_path)) {
            return json::object({{"status", "error"}, {"message", "File does not exist: " + path}});
        }

        // OS handles permissions - if we can reach here, we have access
        std::filesystem::remove(target_path);
        return json::object({{"status", "success"}, {"path", target_path.string()}, {"message", "File deleted successfully."}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}

// ========== DeleteDirectoryTool ==========

json DeleteDirectoryTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", {
            {"name", get_name()},
            {"description", get_description()},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({
                    {"path", json::object({
                        {"type", "string"},
                        {"description", "Path to the directory to delete (relative to Project Root)"}
                    })}
                })},
                {"required", json::array({"path"})}
            })}
        }}
    });
}

json DeleteDirectoryTool::execute(const json& args, FileIO& file_io) {
    std::string path;
    try {
        if (args.is_object() && args.contains("path")) {
            path = args["path"].get<std::string>();
        } else if (args.is_string()) {
            json parsed_args = json::parse(args.get<std::string>());
            if (parsed_args.contains("path")) {
                path = parsed_args["path"].get<std::string>();
            } else {
                return json::object({{"status", "error"}, {"message", "Missing 'path' in arguments"}});
            }
        } else {
            return json::object({{"status", "error"}, {"message", "Invalid arguments format for delete_directory"}});
        }

        if (path.empty()) {
            return json::object({{"status", "error"}, {"message", "Missing or empty 'path' argument"}});
        }
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string("Error parsing path argument: ") + e.what()}});
    }

    try {
        // Resolve path relative to tracked current working directory
        std::filesystem::path base_dir = ToolHandler::is_cwd_initialized() ? 
            ToolHandler::get_current_working_directory() : file_io.get_project_path();
        std::filesystem::path target_path;
        if (std::filesystem::path(path).is_absolute()) {
            target_path = std::filesystem::absolute(path);
        } else {
            target_path = std::filesystem::absolute(base_dir / path);
        }

        // Trust OS permissions - any accessible directory can be deleted
        // Keep the project folder protection (defense in depth)
        std::filesystem::path project_path = file_io.get_project_path();
        if (std::filesystem::equivalent(target_path, project_path)) {
            return json::object({{"status", "error"}, {"message", "Security: Cannot delete the project folder itself: " + path}});
        }

        if (!std::filesystem::exists(target_path)) {
            return json::object({{"status", "error"}, {"message", "Directory does not exist: " + path}});
        }

        if (!std::filesystem::is_directory(target_path)) {
            return json::object({{"status", "error"}, {"message", "Path is not a directory: " + path}});
        }

        // Remove the directory and all its contents
        std::filesystem::remove_all(target_path);
        return json::object({{"status", "success"}, {"path", target_path.string()}, {"message", "Directory deleted successfully."}});
    } catch (const std::exception& e) {
        return json::object({{"status", "error"}, {"message", std::string(e.what())}});
    }
}
