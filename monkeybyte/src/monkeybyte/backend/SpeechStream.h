#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace monkeybyte {

// §CHIMPFLOW-STREAM — incrementally splits live assistant Markdown into
// speakable sentence/paragraph chunks without duplicating earlier text.
// Byte offsets are used only at ASCII punctuation/newline boundaries (or the
// complete UTF-8 ellipsis), never to truncate text at an arbitrary length.
class SpeechStreamSegmenter {
public:
    std::vector<std::string> append(std::string_view delta);
    std::string take_remainder();
    void clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

} // namespace monkeybyte
