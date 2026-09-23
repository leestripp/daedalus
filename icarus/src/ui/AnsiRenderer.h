// SPDX-License-Identifier: BSL-1.0
// src/ui/AnsiRenderer.h
//
// ANSI terminal rendering helpers for the icarus CLI. Provides color/style
// management, box drawing, text wrapping, progress bars, and simple code
// highlighting. All output is plain-text compatible: when color or Unicode
// is disabled, the renderer falls back to ASCII-safe equivalents.

#pragma once

#include <cstdint>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace icarus::ui {

// ---------------------------------------------------------------------------
// Runtime capability flags. Caller populates these from CLI args / env.
// ---------------------------------------------------------------------------
struct RenderConfig {
    bool color_enabled = true;   // false == strip all SGR escapes
    bool unicode_enabled = true; // false == use ASCII box chars
    bool hud_enabled = true;     // false == disable status HUD / spinner
    int terminal_width = 80;     // 0 == no wrapping

    static RenderConfig detect(bool force_no_color = false,
                               bool force_no_unicode = false,
                               bool force_no_hud = false);
};

// Live terminal width at render time: re-queries the OS (TIOCGWINSZ, then
// $COLUMNS) so boxes/tables follow terminal resizes mid-session instead of
// sticking to the width detected at startup. Falls back to
// cfg.terminal_width when no live value is available (e.g. piped output).
[[nodiscard]] int effective_terminal_width(const RenderConfig& cfg);

// ---------------------------------------------------------------------------
// Semantic style builder. Each method returns an ANSI string segment.
// When color is disabled, methods return the plain text unchanged.
// ---------------------------------------------------------------------------
class Style {
public:
    explicit Style(const RenderConfig& cfg) : cfg_(cfg) {}

    [[nodiscard]] std::string reset() const;
    [[nodiscard]] std::string bold(std::string_view text) const;
    [[nodiscard]] std::string dim(std::string_view text) const;
    [[nodiscard]] std::string italic(std::string_view text) const;

    [[nodiscard]] std::string primary(std::string_view text) const;
    [[nodiscard]] std::string success(std::string_view text) const;
    [[nodiscard]] std::string warning(std::string_view text) const;
    [[nodiscard]] std::string error(std::string_view text) const;
    [[nodiscard]] std::string info(std::string_view text) const;
    [[nodiscard]] std::string muted(std::string_view text) const;

    // 256-color and true-color fallbacks.
    [[nodiscard]] std::string fg256(std::string_view text, std::uint8_t color) const;
    [[nodiscard]] std::string rgb(std::string_view text,
                                  std::uint8_t r,
                                  std::uint8_t g,
                                  std::uint8_t b) const;

private:
    const RenderConfig& cfg_;

    [[nodiscard]] std::string wrap(std::string_view text,
                                   std::string_view prefix,
                                   std::string_view suffix) const;
};

// ---------------------------------------------------------------------------
// Pure text utilities (ANSI-aware).
// ---------------------------------------------------------------------------

// Visible width of a string, ignoring ANSI escape sequences.
[[nodiscard]] std::size_t visible_width(std::string_view text);

// Strip ANSI escape sequences from text.
[[nodiscard]] std::string strip_ansi(std::string_view text);

// Wrap text to fit within `width` visible columns. Preserves existing ANSI
// sequences in each chunk (does not reset/re-emit them).
[[nodiscard]] std::vector<std::string> wrap_lines(std::string_view text,
                                                   std::size_t width);

// Truncate to `width` visible columns, adding an ellipsis if truncated.
[[nodiscard]] std::string truncate(std::string_view text,
                                   std::size_t width,
                                   std::string_view ellipsis = "…");

// ---------------------------------------------------------------------------
// Drawing primitives.
// ---------------------------------------------------------------------------

// Horizontal rule.
[[nodiscard]] std::string hrule(std::size_t width,
                                char ascii = '-',
                                const RenderConfig& cfg = RenderConfig::detect());

// Bordered box. Title is optional. Content lines are left as-is except for
// being padded/truncated to the inner width.
enum class Alignment { Left, Center, Right, Auto };
struct BoxResult {
    std::vector<std::string> lines;
    std::size_t inner_width = 0;
};
[[nodiscard]] BoxResult make_box(const std::vector<std::string>& content,
                                 std::optional<std::string_view> title = std::nullopt,
                                 const RenderConfig& cfg = RenderConfig::detect(),
                                 Alignment align = Alignment::Left);

// Table with auto-sized or fixed-width columns.
struct TableColumn {
    std::string header;
    std::size_t min_width = 0;
    std::size_t max_width = 0; // 0 == unbounded
    bool flex = false;         // grow to fill available width
};
[[nodiscard]] std::vector<std::string> make_table(
    const std::vector<TableColumn>& columns,
    const std::vector<std::vector<std::string>>& rows,
    const RenderConfig& cfg = RenderConfig::detect());

// Fluent front end for make_table — the single entry point every table in
// the app should be built through, so sizing/shrink/truncation behaviour
// stays consistent and only ever needs fixing in one place.
//
//   DrawTable t(cfg);
//   t.column("Name", 8, 20).column("Path", 20, 80, true);
//   t.row({"daedalus", "/home/ubuntu/.local/share/daedalus"});
//   t.print();
class DrawTable {
public:
    explicit DrawTable(RenderConfig cfg = RenderConfig::detect())
        : cfg_(std::move(cfg)) {}

    DrawTable& column(std::string header, std::size_t min_width,
                      std::size_t max_width = 0, bool flex = false) {
        columns_.push_back({std::move(header), min_width, max_width, flex});
        return *this;
    }

    DrawTable& row(std::vector<std::string> cells) {
        rows_.push_back(std::move(cells));
        return *this;
    }

    // Rendered lines (borders included), fit to the live terminal width.
    [[nodiscard]] std::vector<std::string> render() const {
        return make_table(columns_, rows_, cfg_);
    }

    // Leading blank line + one line per row, matching the historical
    // print_table() behaviour.
    void print(std::ostream& os = std::cout) const {
        os << "\n";
        for (const auto& line : render()) os << line << "\n";
    }

private:
    RenderConfig cfg_;
    std::vector<TableColumn> columns_;
    std::vector<std::vector<std::string>> rows_;
};

// ---------------------------------------------------------------------------
// Progress bar.
// ---------------------------------------------------------------------------

[[nodiscard]] std::string progress_bar(double fraction,
                                       std::size_t width = 20,
                                       const RenderConfig& cfg = RenderConfig::detect());

// Compact inline progress: "label ░░░░░░░░░░░░░░░░░░  34%"
[[nodiscard]] std::string compact_progress(std::string_view label,
                                           double fraction,
                                           std::size_t bar_width = 15,
                                           const RenderConfig& cfg = RenderConfig::detect());

// ---------------------------------------------------------------------------
// Simple code highlighting.
// ---------------------------------------------------------------------------

// Detect a code fence start line and extract the language tag.
// Returns std::nullopt if the line is not a fence start.
[[nodiscard]] std::optional<std::string_view> detect_fence(std::string_view line);

// Highlight a single line of code. Language hint controls keyword/comment
// rules. Very lightweight: keyword, string, comment, number classes.
[[nodiscard]] std::string highlight_code_line(std::string_view line,
                                              std::string_view language,
                                              const RenderConfig& cfg = RenderConfig::detect());

} // namespace icarus::ui
