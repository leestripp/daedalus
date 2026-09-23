#pragma once

namespace monkeybyte {

// How a file should be opened in the editor area (design §6 preview tabs).
// Text is the default code editor; the rest are read-only WebKit-backed
// viewer tabs (ui/HtmlView). Browser tabs hold a URL in Tab::path instead of
// a workspace file (see EditorArea::open_url).
enum class FileOpenMode { Text, MarkdownPreview, HtmlPreview, FountainPreview, Browser };

} // namespace monkeybyte
