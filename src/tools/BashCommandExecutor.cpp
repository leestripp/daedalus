#include "tools/BashCommandExecutor.h"
#include "core/Subprocess.h"
#include "core/ThreadPool.h"
#include "core/Utf8Utils.h"

#include <thread>
#include <atomic>
#include <sstream>
#include <algorithm>
#include <cstdlib>
#include <limits>
#include <regex>
#include <future>
#include <chrono>
#include <functional>

#include "core/Logger.h"

// §Phase 5.1: helper macro that converts LOG_CERR(INFO,  ...); style
// statements into LOG_INFO/LOG_WARNING/LOG_ERROR calls via the
// existing Logger. Same formatted text routed through the Logger
// instead of writing to stderr directly.
#define LOG_CERR(level, expr) do {                          \
        std::ostringstream _daedalus_msg;                   \
        _daedalus_msg << expr;                               \
        LOG_##level(_daedalus_msg.str());                    \
    } while (0)
// Spinner helper class for consistent animation
// Uses the shared ThreadPool (LOW priority) instead of creating a fresh
// std::thread per command. The thread-pool worker is reused.
class CommandSpinner {
public:
    explicit CommandSpinner(const std::string& cmd)
        : command_done(false), command(cmd) {
        using daedalus::concurrent::ThreadPool;
        using daedalus::concurrent::TaskPriority;
        // Submit to the shared pool - worker threads are reused
        spinner_future = ThreadPool::get_instance().submit(
            TaskPriority::LOW,
            [this]() { run(); }
        );
    }

    ~CommandSpinner() {
        stop();
    }

    void stop() {
        command_done = true;
        // Wait for the spinner task to complete
        if (spinner_future.valid()) {
            spinner_future.wait();
        }
    }

private:
    void run() {
        // §Phase 5.1: removed the spinner animation that wrote to
        // std::cout ("Executing: ...", "|/-\\", etc.). The library
        // is no longer responsible for terminal animation —
        // hosts that want a progress indicator should observe
        // the agent's sink events and render their own UI.
        // The thread still exists so the destructor/stop() call
        // sites remain valid; it just does nothing visible now.
        while (!command_done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    std::atomic<bool> command_done;
    std::string command;
    std::future<void> spinner_future;
};

// Buffers raw output chunks from a subprocess and forwards them to a host
// callback in a rate-limited, line-friendly way. Chunks are sanitised
// (TTY control sequences stripped) before forwarding so the host TTY is
// never corrupted by streaming display updates.
class OutputChunkBuffer {
public:
    using ChunkCallback = std::function<void(std::string_view)>;

    explicit OutputChunkBuffer(ChunkCallback cb,
                               size_t max_line_buffer = 4096,
                               int min_interval_ms = 100)
        : callback_(std::move(cb)),
          max_line_buffer_(max_line_buffer),
          min_interval_ms_(min_interval_ms),
          last_emit_(std::chrono::steady_clock::now()) {}

    void append(std::string_view chunk) {
        if (!callback_) return;
        buffer_.append(chunk);
        try_emit(false);
    }

    void flush() {
        if (!callback_) return;
        try_emit(true);
    }

private:
    void try_emit(bool force) {
        if (buffer_.empty()) return;

        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - last_emit_).count();

        // Prefer to emit on line boundaries so the streaming display is
        // coherent and we don't split UTF-8 / ANSI sequences.
        size_t emit_end = buffer_.find_last_of('\n');
        bool has_newline = emit_end != std::string::npos;
        if (has_newline) {
            emit_end += 1; // include the newline
        }

        bool threshold_reached = buffer_.size() >= max_line_buffer_;
        bool time_elapsed = elapsed_ms >= min_interval_ms_;

        if (!force && !has_newline && !threshold_reached && !time_elapsed) {
            return;
        }

        // If we have no newline but are emitting for threshold/time reasons,
        // emit the whole current buffer. This is rare (very long single line).
        if (!has_newline) {
            emit_end = buffer_.size();
        }

        if (emit_end == 0) return;

        std::string chunk = BashCommandExecutor::sanitize_tty_sequences(
            buffer_.substr(0, emit_end));
        if (!chunk.empty()) {
            callback_(chunk);
        }
        buffer_.erase(0, emit_end);
        last_emit_ = now;
    }

    ChunkCallback callback_;
    std::string buffer_;
    size_t max_line_buffer_;
    int min_interval_ms_;
    std::chrono::steady_clock::time_point last_emit_;
};

// ========== Public Interface ==========

BashCommandExecutor::Result BashCommandExecutor::execute(const std::string& command,
                                                          const std::string& base_dir,
                                                          int timeout_seconds) {
    return execute(command, base_dir, timeout_seconds, nullptr);
}

BashCommandExecutor::Result BashCommandExecutor::execute(
        const std::string& command,
        const std::string& base_dir,
        int timeout_seconds,
        std::function<void(std::string_view)> output_chunk_cb) {
    // Sanitize pkill command by removing -f flag
    // pkill -f matches ANY part of command line, not just process name
    std::string sanitized_cmd = sanitize_pkill_command(command);
    
    // Escape the base_dir for shell
    std::string escaped_dir = escape_shell_arg(base_dir);
    
    // Build the full command with cd to base_dir.
    // §FIX-heredoc-redirect: wrap the command in a brace group and
    // redirect the group instead of appending ` 2>&1` after the command.
    // Appending corrupted heredoc commands — the trailing `2>&1` landed
    // on the terminator line (`EOF` became `EOF 2>&1`). A trailing
    // newline before `}` terminates the command list (a `;` there would
    // be a syntax error for multi-line input).
    std::string full_command = "cd " + escaped_dir + " && { " + sanitized_cmd + "\n} 2>&1";
    
    return execute_internal(full_command, sanitized_cmd, timeout_seconds,
                            std::move(output_chunk_cb));
}

// ========== Private Implementation ==========

BashCommandExecutor::Result BashCommandExecutor::execute_internal(
        const std::string& full_command,
        const std::string& display_command,
        int timeout_seconds) {
    return execute_internal(full_command, display_command, timeout_seconds, nullptr);
}

BashCommandExecutor::Result BashCommandExecutor::execute_internal(
        const std::string& full_command,
        const std::string& display_command,
        int timeout_seconds,
        std::function<void(std::string_view)> output_chunk_cb) {
    
    CommandSpinner spinner(display_command);

    // Capture whether a chunk callback was supplied before moving it into
    // the buffering wrapper.
    const bool has_chunk_cb = static_cast<bool>(output_chunk_cb);
    OutputChunkBuffer chunk_buffer(std::move(output_chunk_cb));

    // Use Subprocess for crash isolation with specified timeout. If a chunk
    // callback is provided, forward each raw output chunk through the buffer
    // so the host can display sanitised progress in real time.
    SubprocessResult result;
    if (has_chunk_cb) {
        result = Subprocess::execute(full_command, "", timeout_seconds * 1000,
            [&chunk_buffer](std::string_view chunk) {
                chunk_buffer.append(chunk);
            });
    } else {
        result = Subprocess::execute(full_command, "", timeout_seconds * 1000);
    }
    chunk_buffer.flush();
    
    spinner.stop();
    
    if (result.timed_out) {
        return Result(false, 124, "", "Command timed out after " + std::to_string(timeout_seconds) + " seconds");
    }
    
    return Result(result.exit_code == 0, result.exit_code, sanitize_output(result.stdout));
}

std::string BashCommandExecutor::build_spinner_context(const std::string& command) {
    // Extract first line for display
    std::istringstream iss(command);
    std::string first_word;
    iss >> first_word;
    return "Running: " + first_word;
}

// ========== Utilities ==========

std::string BashCommandExecutor::escape_shell_arg(const std::string& input) {
    std::string result;
    result.reserve(input.size() + 16);
    result.push_back('\'');
    for (char c : input) {
        if (c == '\'') {
            result.push_back('\'');
            result.push_back('\\');
            result.push_back('\'');
            result.push_back('\'');
        } else {
            result.push_back(c);
        }
    }
    result.push_back('\'');
    return result;
}

std::string BashCommandExecutor::sanitize_tty_sequences(const std::string& output) {
    if (output.empty()) {
        return output;
    }

    enum class State {
        Normal,
        Escape,          // saw ESC
        EscIntermediate, // ESC + one or more intermediate bytes (0x20-0x2F)
        CSI,             // ESC [ collecting parameter bytes
        CSIIntermediate, // ESC [ params + intermediate bytes
        OSC,             // ESC ] collecting until BEL or ST
        OSC_ST,          // saw ESC inside OSC, expect '\' to terminate
        String,          // ESC P/^/_/X collecting until ST
        String_ST        // saw ESC inside String, expect '\' to terminate
    };

    constexpr unsigned char ESC = 0x1B;
    constexpr unsigned char BEL = 0x07;

    std::string result;
    result.reserve(output.size());

    State state = State::Normal;
    std::string csi_buffer;      // bytes collected for a CSI sequence
    bool csi_has_intermediate = false;

    for (size_t i = 0; i < output.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(output[i]);

        switch (state) {
        case State::Normal:
            if (c == ESC) {
                state = State::Escape;
            } else if (c == '\r') {
                // Normalise CR and CR/LF to a single LF so output cannot
                // overwrite previous lines in the host terminal.
                if (i + 1 < output.size() && static_cast<unsigned char>(output[i + 1]) == '\n') {
                    ++i; // skip the following LF
                }
                result.push_back('\n');
            } else if (c == '\t' || c == '\n') {
                result.push_back(static_cast<char>(c));
            } else if (c < 0x20 || c == 0x7F) {
                // Drop other C0 controls (bell, backspace, vertical tab,
                // form feed) and DEL.
            } else if ((c & 0x80) != 0) {
                // Start of a multi-byte UTF-8 sequence. Copy the whole
                // code point if it is valid and complete.
                const size_t char_len = utf8_char_length_from_first_byte(c);
                if (char_len > 1 && i + char_len <= output.size() &&
                    is_valid_utf8_char(output.data(), i, output.size())) {
                    result.append(output, i, char_len);
                    i += char_len - 1;
                }
                // Invalid high byte: drop it.
            } else {
                // Printable ASCII.
                result.push_back(static_cast<char>(c));
            }
            break;

        case State::Escape:
            if (c == '[') {
                state = State::CSI;
                csi_buffer.clear();
                csi_buffer.push_back(static_cast<char>(ESC));
                csi_buffer.push_back('[');
                csi_has_intermediate = false;
            } else if (c == ']') {
                state = State::OSC;
            } else if (c == 'P' || c == '^' || c == '_' || c == 'X') {
                state = State::String;
            } else if (c >= 0x30 && c <= 0x7E) {
                // ESC + final byte (e.g. ESC c, ESC 7, ESC 8). Drop.
                state = State::Normal;
            } else if (c >= 0x20 && c <= 0x2F) {
                // ESC + intermediate byte; wait for final byte.
                state = State::EscIntermediate;
            } else {
                // Unrecognised ESC sequence: drop ESC and this byte.
                state = State::Normal;
            }
            break;

        case State::EscIntermediate:
            if (c >= 0x30 && c <= 0x7E) {
                // Final byte reached; drop the whole ESC sequence.
                state = State::Normal;
            } else if (c < 0x20 || c > 0x2F) {
                // Invalid byte in the middle of the sequence; recover.
                state = State::Normal;
            }
            // Otherwise remain in EscIntermediate waiting for the final byte.
            break;

        case State::CSI:
            csi_buffer.push_back(static_cast<char>(c));
            if (c >= 0x30 && c <= 0x3F) {
                // Parameter byte: remain in CSI.
            } else if (c >= 0x20 && c <= 0x2F) {
                csi_has_intermediate = true;
                state = State::CSIIntermediate;
            } else if (c >= 0x40 && c <= 0x7E) {
                // Final byte. Keep only Select Graphic Rendition (SGR),
                // which uses final byte 'm' with no intermediate bytes.
                if (c == 'm' && !csi_has_intermediate) {
                    result.append(csi_buffer);
                }
                state = State::Normal;
                csi_buffer.clear();
                csi_has_intermediate = false;
            } else {
                // Malformed CSI; recover to normal state.
                state = State::Normal;
                csi_buffer.clear();
                csi_has_intermediate = false;
            }
            break;

        case State::CSIIntermediate:
            csi_buffer.push_back(static_cast<char>(c));
            if (c >= 0x20 && c <= 0x2F) {
                // Another intermediate byte.
            } else if (c >= 0x40 && c <= 0x7E) {
                // Final byte reached. Not SGR (SGR has no intermediates),
                // so drop the sequence.
                state = State::Normal;
                csi_buffer.clear();
                csi_has_intermediate = false;
            } else {
                // Malformed; recover.
                state = State::Normal;
                csi_buffer.clear();
                csi_has_intermediate = false;
            }
            break;

        case State::OSC:
            if (c == ESC) {
                state = State::OSC_ST;
            } else if (c == BEL) {
                // BEL-terminated OSC (xterm legacy). Drop.
                state = State::Normal;
            }
            // Otherwise remain in OSC, dropping bytes.
            break;

        case State::OSC_ST:
            if (c == '\\') {
                // String terminator (ESC \). Drop.
                state = State::Normal;
            } else {
                // Not a terminator; continue OSC.
                state = State::OSC;
            }
            break;

        case State::String:
            if (c == ESC) {
                state = State::String_ST;
            }
            // Otherwise remain in String, dropping bytes.
            break;

        case State::String_ST:
            if (c == '\\') {
                // String terminator. Drop.
                state = State::Normal;
            } else {
                state = State::String;
            }
            break;
        }
    }

    // If the output ends in the middle of a control sequence, the trailing
    // partial bytes are implicitly dropped by returning `result`.
    return result;
}

std::string BashCommandExecutor::sanitize_output(const std::string& output) {
    if (output.empty()) {
        return output;
    }
    
    // Check first 512 bytes for binary content
    const size_t check_size = std::min(output.size(), static_cast<size_t>(512));
    size_t non_printable = 0;
    for (size_t i = 0; i < check_size; i++) {
        unsigned char c = static_cast<unsigned char>(output[i]);
        // Count non-printable characters (excluding common whitespace)
        if (c < 32 && c != '\t' && c != '\n' && c != '\r') {
            non_printable++;
        }
    }

    // If more than 10% non-printable, it's likely binary
    if (check_size > 0 && non_printable > check_size / 10) {
        return "[Binary output truncated - " + std::to_string(output.size()) + " bytes]";
    }

    // Strip TTY control sequences (cursor movement, screen erase, alternate
    // buffer, OSC, etc.) while preserving UTF-8 text and ANSI SGR colour/style
    // codes. This prevents TUI applications like htop/btop from corrupting the
    // host terminal when their captured output is forwarded to the API or sink.
    std::string sanitized = sanitize_tty_sequences(output);

    // Truncate from middle if too large - keep beginning and end (most important info)
    const size_t max_size = 20000;   // 20KB max
    const size_t head_size = 8000;   // Keep first 8KB
    const size_t tail_size = 8000;    // Keep last 8KB
    
    if (sanitized.size() > max_size) {
        if (sanitized.size() <= head_size + tail_size) {
            return sanitized.substr(0, max_size) + "\n[Output truncated at " + std::to_string(max_size) + " bytes]";
        }
        
        // Keep head and tail, truncate middle
        std::string head = sanitized.substr(0, head_size);
        std::string tail = sanitized.substr(sanitized.size() - tail_size);
        
        // Count lines skipped in middle
        size_t lines_skipped = 0;
        size_t pos = head_size;
        while (pos < sanitized.size() - tail_size) {
            if (sanitized[pos] == '\n') {
                lines_skipped++;
            }
            pos++;
        }
        
        return head + 
            "\n\n[...] " + std::to_string(lines_skipped) + " lines skipped (" + 
            std::to_string(sanitized.size() - head_size - tail_size) + " bytes) [...]\n\n" + 
            tail +
            "\n[Output truncated - showing first " + std::to_string(head_size) + " and last " + 
            std::to_string(tail_size) + " bytes of " + std::to_string(sanitized.size()) + " total]";
    }

    return sanitized;
}

std::string BashCommandExecutor::sanitize_pkill_command(const std::string& command) {
    std::string cmd_lower = command;
    std::transform(cmd_lower.begin(), cmd_lower.end(), cmd_lower.begin(), ::tolower);
    
    // Check if this is a pkill command
    if (cmd_lower.find("pkill") != std::string::npos) {
        std::string sanitized = command;
        
        // Use regex to replace " -f" followed by space, end, or another flag
        std::regex pattern(R"( -f(?= |$|-))");
        sanitized = std::regex_replace(sanitized, pattern, " ");
        
        return sanitized;
    }
    
    return command;
}