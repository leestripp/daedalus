#include "monkeybyte/backend/SpeechStream.h"

#include <utility>

namespace monkeybyte {

namespace {

bool boundary_space(char ch) {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

// Return the byte count of the first complete speakable prefix, or zero when
// more streaming input is needed. Fenced code is held until its closing fence.
std::size_t sentence_end(std::string_view text) {
    bool fenced = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i + 2 < text.size() && text.substr(i, 3) == "```") {
            fenced = !fenced;
            i += 2;
            continue;
        }
        if (fenced) continue;
        if (text[i] == '\\' && i + 1 < text.size()) {
            ++i; // escaped punctuation is not a sentence boundary
            continue;
        }
        if (text[i] == '\n' && i + 1 < text.size() && text[i + 1] == '\n')
            return i + 2;

        std::size_t after = i + 1;
        if (text[i] == '.' || text[i] == '!' || text[i] == '?') {
            after = i + 1;
        } else if (i + 2 < text.size() && text.substr(i, 3) == "…") {
            after = i + 3; // complete three-byte UTF-8 ellipsis
        } else {
            continue;
        }

        // Closing ASCII quotes/brackets belong to the preceding sentence.
        while (after < text.size() &&
               (text[after] == '\'' || text[after] == '"' ||
                text[after] == ')' || text[after] == ']' ||
                text[after] == '*' || text[after] == '_' || text[after] == '~')) {
            ++after;
        }
        if (after >= text.size()) return 0; // boundary not confirmed yet
        if (boundary_space(text[after])) return after;
        if (after > i + 1) i = after - 1;
    }
    return 0;
}

} // namespace

std::vector<std::string> SpeechStreamSegmenter::append(std::string_view delta) {
    if (!delta.empty()) buffer_.append(delta);
    std::vector<std::string> chunks;
    while (const std::size_t end = sentence_end(buffer_)) {
        auto chunk = buffer_.substr(0, end);
        buffer_.erase(0, end);
        // Paragraph separators are boundaries, not utterances.
        if (chunk.find_first_not_of(" \t\r\n") != std::string::npos)
            chunks.push_back(std::move(chunk));
    }
    return chunks;
}

std::string SpeechStreamSegmenter::take_remainder() {
    auto remainder = std::move(buffer_);
    buffer_.clear();
    return remainder;
}

} // namespace monkeybyte
