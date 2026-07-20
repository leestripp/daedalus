#include "SystemInfoTool.h"
#include "core/FileIO.h"

#include <fstream>
#include <sstream>
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <sys/utsname.h>
#include <unistd.h>
#include <pwd.h>

using json = nlohmann::json;

// ============================================================================
// Tool Definition
// ============================================================================

json SystemInfoTool::get_definition() const {
    return json::object({
        {"type", "function"},
        {"function", json::object({
            {"name", get_name()},
            {"description", "Gather system information about the current environment: kernel, OS, container (Docker/Podman), date/time, window manager (X11/Wayland), and desktop environment."},
            {"parameters", json::object({
                {"type", "object"},
                {"properties", json::object({})},
                {"required", json::array({})}
            })}
        })}
    });
}

// ============================================================================
// Main Execute
// ============================================================================

json SystemInfoTool::execute(const json& args, FileIO&) {
    (void)args;  // No arguments needed for this tool
    
    
    json result = json::object({
        {"status", "success"}
    });
    
    json kernel = get_kernel_info();
    
    json os = get_os_info();
    
    json container = get_container_info();
    
    json date_time = get_date_time_info();
    
    json display = get_display_info();
    
    json session = get_session_info();
    
    json system = json::object({
        {"kernel", kernel},
        {"os", os},
        {"container", container},
        {"date_time", date_time},
        {"display", display},
        {"session", session}
    });
    
    result["system"] = system;
    
    return result;
}

// ============================================================================
// Kernel Information
// ============================================================================

json SystemInfoTool::get_kernel_info() const {
    json info = json::object();
    
    struct utsname uts;
    if (uname(&uts) == 0) {
        info["name"] = uts.sysname;
        info["version"] = uts.version;
        info["release"] = uts.release;
        info["machine"] = uts.machine;
#if defined(__GNU__)
        info["domain"] = uts.domainname;
#endif
    }
    
    // Alternative: read /proc/version for more details on Linux
    std::string version_content = read_file_content("/proc/version");
    if (!version_content.empty() && info["version"].get<std::string>().empty()) {
        info["version"] = version_content;
    }
    
    return info;
}

// ============================================================================
// OS Information
// ============================================================================

json SystemInfoTool::get_os_info() const {
    json info = json::object();
    
    // Use uname for architecture
    struct utsname uts;
    if (uname(&uts) == 0) {
        info["architecture"] = uts.machine;
        info["kernel_name"] = uts.sysname;
    }
    
    // Parse /etc/os-release for Linux distributions
    std::ifstream os_release("/etc/os-release");
    if (os_release.is_open()) {
        // §FIX-os-release-offsets: locate the OPENING quote first
        // (searching from just after '<key>='), then the closing quote
        // after it. If the value is unquoted (e.g. ID=ubuntu), take it
        // verbatim. The old code searched for a quote at a fixed offset
        // past the opening quote (yielding e.g. "buntu" for
        // NAME="Ubuntu") or, for VERSION=, matched the opening quote as
        // the closing one and underflowed the length.
        auto extract_value = [](const std::string& l, size_t key_len) -> std::string {
            size_t open = l.find('"', key_len);
            if (open != std::string::npos) {
                size_t close = l.find('"', open + 1);
                if (close != std::string::npos) {
                    return l.substr(open + 1, close - open - 1);
                }
                return "";
            }
            // Unquoted value — take the remainder verbatim.
            return l.substr(key_len);
        };
        std::string line;
        while (std::getline(os_release, line)) {
            if (line.rfind("NAME=", 0) == 0) {
                info["name"] = extract_value(line, 5);
            } else if (line.rfind("VERSION=", 0) == 0) {
                info["version"] = extract_value(line, 8);
            } else if (line.rfind("PRETTY_NAME=", 0) == 0) {
                info["pretty_name"] = extract_value(line, 12);
            } else if (line.rfind("ID=", 0) == 0) {
                info["id"] = extract_value(line, 3);
            } else if (line.rfind("VERSION_ID=", 0) == 0) {
                info["version_id"] = extract_value(line, 11);
            }
        }
    }
    
    // Fallback for macOS or other systems
    if (!info.contains("name") || info["name"].get<std::string>().empty()) {
#if defined(__APPLE__)
        info["name"] = "macOS";
#elif defined(_WIN32) || defined(_WIN64)
        info["name"] = "Windows";
#elif defined(__linux__)
        info["name"] = "Linux";
#elif defined(__FreeBSD__)
        info["name"] = "FreeBSD";
#elif defined(__OpenBSD__)
        info["name"] = "OpenBSD";
#elif defined(__NetBSD__)
        info["name"] = "NetBSD";
#endif
    }
    
    return info;
}

// ============================================================================
// Container Information
// ============================================================================

json SystemInfoTool::get_container_info() const {
    json info = json::object();
    
    info["is_container"] = false;
    
    // Check 1: Environment variables
    std::string container_env = get_env_var("container");
    std::string podman_version = get_env_var("PODMAN_VERSION");
    std::string docker_version = get_env_var("DOCKER_VERSION");
    
    if (!podman_version.empty()) {
        info["is_container"] = true;
        info["type"] = "podman";
        info["podman_version"] = podman_version;
    } else if (!docker_version.empty()) {
        info["is_container"] = true;
        info["type"] = "docker";
        info["docker_version"] = docker_version;
    } else if (!container_env.empty()) {
        info["is_container"] = true;
        info["type"] = container_env;
    }
    
    // Check 2: /.dockerenv file
    if (file_exists("/.dockerenv")) {
        info["is_container"] = true;
        if (!info.contains("type") || info["type"].get<std::string>().empty()) {
            info["type"] = "docker";
        }
    }
    
    // Check 3: /run/.containerenv file (Podman)
    if (file_exists("/run/.containerenv")) {
        info["is_container"] = true;
        info["type"] = "podman";
        
        // Try to read container info from .containerenv
        std::string containerenv = read_file_content("/run/.containerenv");
        if (!containerenv.empty()) {
            // Parse for image information
            std::istringstream iss(containerenv);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.substr(0, 6) == "image=") {
                    info["image"] = line.substr(6);
                } else if (line.substr(0, 8) == "imageid=") {
                    info["image_id"] = line.substr(8);
                }
            }
        }
    }
    
    // Check 4: /proc/1/cgroup for docker/podman markers
    std::string cgroup_content = read_file_content("/proc/1/cgroup");
    if (cgroup_content.find("docker") != std::string::npos) {
        info["is_container"] = true;
        if (!info.contains("type") || info["type"].get<std::string>().empty()) {
            info["type"] = "docker";
        }
        
        // Try to get container ID from cgroup
        size_t id_start = cgroup_content.find_last_of('/');
        if (id_start != std::string::npos && id_start + 1 < cgroup_content.length()) {
            std::string container_id = cgroup_content.substr(id_start + 1);
            // Remove newline and extra whitespace
            container_id.erase(container_id.find_last_not_of(" \t\n\r") + 1);
            if (!container_id.empty()) {
                info["container_id"] = container_id;
            }
        }
    } else if (cgroup_content.find("podman") != std::string::npos) {
        info["is_container"] = true;
        if (!info.contains("type") || info["type"].get<std::string>().empty()) {
            info["type"] = "podman";
        }
    } else if (cgroup_content.find("containerd") != std::string::npos) {
        info["is_container"] = true;
        if (!info.contains("type") || !info.contains("type")) {
            info["type"] = "containerd";
        }
    }
    
    // Try to read /etc/hostname for image name hints
    std::string hostname = read_file_content("/etc/hostname");
    if (!hostname.empty()) {
        // Trim whitespace
        hostname.erase(0, hostname.find_first_not_of(" \t\n\r"));
        hostname.erase(hostname.find_last_not_of(" \t\n\r") + 1);
        if (!hostname.empty() && hostname != "localhost" && hostname != "localhost.localdomain") {
            // This might be a container-specific hostname
            if (info["is_container"].get<bool>() && !info.contains("image")) {
                // Could be derived from image name
                // Don't set it as it might be the actual hostname
            }
        }
    }
    
    return info;
}

// ============================================================================
// Date/Time Information
// ============================================================================

json SystemInfoTool::get_date_time_info() const {
    json info = json::object();
    
    std::time_t now = std::time(nullptr);
    char datetime_buffer[100];
    char tz_buffer[100];
    
    // ISO 8601 format with timezone
    std::strftime(datetime_buffer, sizeof(datetime_buffer), "%Y-%m-%dT%H:%M:%S%z", std::localtime(&now));
    info["iso"] = std::string(datetime_buffer);
    
    // Insert colon in timezone offset (e.g., +0200 -> +02:00)
    // §FIX-tz-colon-index: %z appends "+HHMM" right after the
    // 19-char "%Y-%m-%dT%H:%M:%S" prefix, so the sign is at index 19
    // (the old check at index 22 never fired and the colon was never
    // inserted).
    std::string tz_str = info["iso"].get<std::string>();
    if (tz_str.length() == 24 && (tz_str[19] == '+' || tz_str[19] == '-')) {
        // Insert colon between hours and minutes
        std::string formatted = tz_str.substr(0, 22) + ":" + tz_str.substr(22);
        info["iso"] = formatted;
    }
    
    // Timezone name (TZ environment variable or /etc/localtime symlink)
    std::string tz_env = get_env_var("TZ");
    if (!tz_env.empty()) {
        info["timezone"] = tz_env;
    } else {
        // Try to read timezone from /etc/localtime
        char tz_path[256];
        ssize_t len = readlink("/etc/localtime", tz_path, sizeof(tz_path) - 1);
        if (len > 0) {
            tz_path[len] = '\0';
            std::string tz_str(tz_path);
            // Typically /etc/localtime -> ../usr/share/zoneinfo/Region/City
            size_t pos = tz_str.find("zoneinfo/");
            if (pos != std::string::npos) {
                info["timezone"] = tz_str.substr(pos + 9);
            }
        }
    }
    
    // UTC offset
    std::strftime(tz_buffer, sizeof(tz_buffer), "%z", std::localtime(&now));
    info["utc_offset"] = std::string(tz_buffer);
    if (info["utc_offset"].get<std::string>().length() == 5) {
        std::string offset = info["utc_offset"].get<std::string>();
        info["utc_offset"] = offset.substr(0, 3) + ":" + offset.substr(3);
    }
    
    // Human-readable format
    std::strftime(datetime_buffer, sizeof(datetime_buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    info["local"] = std::string(datetime_buffer);
    
    // UTC time
    std::strftime(datetime_buffer, sizeof(datetime_buffer), "%Y-%m-%d %H:%M:%S UTC", std::gmtime(&now));
    info["utc"] = std::string(datetime_buffer);
    
    // Unix timestamp
    info["unix_timestamp"] = static_cast<long long>(now);
    
    return info;
}

// ============================================================================
// Display/Window Manager Information
// ============================================================================

json SystemInfoTool::get_display_info() const {
    json info = json::object();
    
    // Detect display server (X11, Wayland, or other)
    std::string session_type = get_env_var("XDG_SESSION_TYPE");
    std::string wayland_display = get_env_var("WAYLAND_DISPLAY");
    std::string x11_display = get_env_var("DISPLAY");
    
    if (!session_type.empty()) {
        info["server"] = session_type;
    } else if (!wayland_display.empty()) {
        info["server"] = "Wayland";
        info["wayland_socket"] = wayland_display;
    } else if (!x11_display.empty()) {
        info["server"] = "X11";
        info["x11_display"] = x11_display;
    } else if (file_exists("/tmp/.X11-unix")) {
        info["server"] = "X11";
        info["note"] = "X11 socket exists but DISPLAY not set";
    } else {
        info["server"] = "unknown";
    }
    
    // Desktop Environment
    std::string xdg_desktop = get_env_var("XDG_CURRENT_DESKTOP");
    std::string desktop_session = get_env_var("DESKTOP_SESSION");
    std::string gdm_session = get_env_var("GDMSESSION");
    
    if (!xdg_desktop.empty()) {
        info["desktop_environment"] = xdg_desktop;
    } else if (!desktop_session.empty()) {
        info["desktop_environment"] = desktop_session;
    } else if (!gdm_session.empty()) {
        info["desktop_environment"] = gdm_session;
    }
    
    // Window Manager (try to detect)
    std::string wm_name = get_env_var("WM_NAME");
    std::string wm_class = get_env_var("WM_CLASS");
    
    if (!wm_name.empty()) {
        info["window_manager"] = wm_name;
    }
    if (!wm_class.empty() && !info.contains("window_manager")) {
        info["window_manager"] = wm_class;
    }
    
    // Try to read from /proc/[window manager pid]/comm if available
    // Common WMs and their typical process names
    std::string wm_processes[] = {
        "mutter", "gnome-shell", "kwin_x11", "kwin_wayland", 
        "xfwm4", "openbox", "i3", "sway", "compton", "picom",
        "xfwm4", "marco", "metacity", "compiz"
    };
    
    if (!info.contains("window_manager")) {
        // Try to detect from process list (simplified check)
        std::string cmdline = read_file_content("/proc/1/comm");
        for (const auto& wm : wm_processes) {
            if (cmdline.find(wm) != std::string::npos) {
                info["window_manager"] = wm;
                break;
            }
        }
    }
    
    // Desktop version if available
    std::string gnome_version = get_env_var("GNOME_DESKTOP_SESSION_ID");
    if (!gnome_version.empty()) {
        info["gnome_session"] = gnome_version;
    }
    
    return info;
}

// ============================================================================
// Session Information
// ============================================================================

json SystemInfoTool::get_session_info() const {
    json info = json::object();
    
    // Terminal type
    std::string term = get_env_var("TERM");
    if (!term.empty()) {
        info["terminal"] = term;
    } else {
        info["terminal"] = "unknown";
    }
    
    // Terminal program (if available)
    std::string term_program = get_env_var("TERM_PROGRAM");
    if (!term_program.empty()) {
        info["terminal_program"] = term_program;
    }
    
    // User information
    struct passwd* pw = getpwuid(getuid());
    if (pw) {
        info["user"] = pw->pw_name;
        info["uid"] = std::to_string(pw->pw_uid);
        if (pw->pw_gecos && strlen(pw->pw_gecos) > 0) {
            info["gecos"] = pw->pw_gecos;
        }
        info["home"] = pw->pw_dir;
        info["shell"] = pw->pw_shell;
    }
    
    // Effective user if different
    uid_t euid = geteuid();
    if (euid != getuid()) {
        struct passwd* epw = getpwuid(euid);
        if (epw) {
            info["effective_user"] = epw->pw_name;
            info["effective_uid"] = std::to_string(epw->pw_uid);
        }
    }
    
    // Hostname
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';
        info["hostname"] = std::string(hostname);
    }
    
    // Domain name (NIS)
    char domainname[256];
#if defined(__linux__)
    if (getdomainname(domainname, sizeof(domainname)) == 0) {
        domainname[sizeof(domainname) - 1] = '\0';
        std::string domain(domainname);
        if (domain != "(none)" && !domain.empty()) {
            info["domainname"] = domain;
        }
    }
#endif
    
    // Session type (local, SSH, screen, tmux)
    std::string ssh_connection = get_env_var("SSH_CONNECTION");
    std::string ssh_client = get_env_var("SSH_CLIENT");
    std::string ssh_tty = get_env_var("SSH_TTY");
    std::string screen_session = get_env_var("STY");
    std::string tmux_session = get_env_var("TMUX");
    std::string tmux_session_prefix = get_env_var("TMUX_PANE");
    
    if (!ssh_connection.empty() || !ssh_client.empty() || !ssh_tty.empty()) {
        info["session_type"] = "ssh";
        if (!ssh_connection.empty()) {
            info["ssh_connection"] = ssh_connection;
        }
    } else if (!screen_session.empty()) {
        info["session_type"] = "screen";
        info["screen_session"] = screen_session;
    } else if (!tmux_session.empty() || !tmux_session_prefix.empty()) {
        info["session_type"] = "tmux";
        if (!tmux_session.empty()) {
            info["tmux_session"] = tmux_session;
        }
    } else {
        info["session_type"] = "local";
    }
    
    // Process info
    info["pid"] = static_cast<long long>(getpid());
    info["ppid"] = static_cast<long long>(getppid());
    
    return info;
}

// ============================================================================
// Utility Functions
// ============================================================================

std::string SystemInfoTool::read_file_content(const std::string& path) const {
    std::ifstream file(path);
    if (!file.is_open()) {
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

std::string SystemInfoTool::get_env_var(const std::string& name) const {
    const char* value = std::getenv(name.c_str());
    if (value) {
        return std::string(value);
    }
    return "";
}

bool SystemInfoTool::file_exists(const std::string& path) const {
    std::ifstream file(path);
    return file.good();
}
