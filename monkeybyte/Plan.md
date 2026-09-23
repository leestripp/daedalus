# Plan — Open non-text files with system applications (file associations)

Goal: media and other binary formats (.mp4, .mp3, .mov, images, archives, …)
should open in the user's system-default application instead of being dumped
into MonkeyByte's text editor. No new dependencies — giomm (already linked)
does the heavy lifting: MIME detection via the shared-mime-info database and
launching via the freedesktop association DB (in-process equivalent of
`xdg-open`).

## Work items

### ✅ 1. `backend/FileKind.{h,cpp}` — classification + launch helper (new file)
- `bool is_text_like_file(const std::string& path)`:
  - Query GIO content type (`G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE`).
  - Text when: type starts `text/`, `Gio::content_type_is_a(type,
    "text/plain")` (covers application/json, xml, shellscript, etc. via
    shared-mime-info subclassing), or type is `inode/x-empty`.
  - Unknown / `application/octet-stream` → NUL-byte sniff of the first 8 KiB.
  - Query failure → sniff fallback (never throw).
- `bool launch_external(const std::string& path, std::string& error)`:
  - Query content type; `Gio::AppInfo::get_default_for_type()` → friendly
    "no application associated with …" error when absent.
  - `Gio::AppInfo::launch_default_for_uri("file://" + abs_path)`; catch
    `Glib::Error` into `error` instead of throwing.
  - Same API already used in HtmlView.cpp / SettingsPanel.cpp.
- CMake: sources are GLOB_RECURSE'd — no CMakeLists change needed.

### ✅ 2. `WorkspaceTree` — route activation by kind
- Row click handler (~line 111): if `is_text_like_file(path)` emit
  `signal_open_file_` as today; else `launch_external()` and log failures
  via `Logger`. Selection highlight/rebuild unchanged.
- Right-click menu: add "Open with System Application" for files
  (unconditional — useful even for text files, e.g. .md in a real editor,
  .svg in Inkscape). Same launch + error log path.

### ✅ 3. `EditorArea::open_file` — binary guard
- After reading content, check the first 8 KiB for NUL bytes; if binary,
  show "binary file — use Open with System Application" in `header_status_`
  and return without creating a tab. Protects all entry points (Git panel,
  search, future callers).

### ✅ 4. Unit tests (`tests/test_main.cpp` additions)
- `is_text_like_file`: temp .txt/.md → true; temp binary file with NUL
  bytes → false; empty file → true; unknown extension with text content →
  true; unknown extension with binary content → false.
- `launch_external` negative path: type with no handler / nonexistent file
  returns false with an error message, does not throw. (Positive launch
  path is desktop-dependent — verified manually, not in unit tests.)

## Verification
- `cmake --build build -j$(nproc)` — warning-clean under existing flags.
- `./build/unit_tests` green.
- Manual smoke: create a small .mp4/.png in a workspace, click it in the
  tree → system app opens; right-click a .md → "Open with System
  Application" works; binary guard message appears when forcing a binary
  through `open_file`.
