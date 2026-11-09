// SPDX-License-Identifier: BSL-1.0
// src/core/InterprocessLock.cpp

#include "InterprocessLock.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace daedalus {

namespace {

int open_lock_file(const std::filesystem::path& path)
{
    for (;;) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0640);
        if (fd >= 0 || errno != EINTR) return fd;
    }
}

std::string errno_message(std::string_view prefix,
                          const std::filesystem::path& path)
{
    const int error = errno;
    return std::string(prefix) + " '" + path.string() + "': " +
           std::strerror(error);
}

}  // anonymous namespace

InterprocessFileLock::InterprocessFileLock(
    const std::filesystem::path& path,
    const InterprocessLockMode mode,
    const std::chrono::milliseconds timeout)
{
    fd_ = open_lock_file(path);
    if (fd_ < 0) {
        error_ = errno_message("cannot open inter-process lock", path);
        return;
    }

    const int operation = mode == InterprocessLockMode::exclusive
                              ? LOCK_EX | LOCK_NB
                              : LOCK_SH | LOCK_NB;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (::flock(fd_, operation) == 0) {
            locked_ = true;
            return;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR) {
            error_ = errno_message("cannot acquire inter-process lock", path);
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            error_ = "timed out after " + std::to_string(timeout.count()) +
                     "ms acquiring inter-process lock '" + path.string() + "'";
            return;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        std::this_thread::sleep_for(
            std::min(std::chrono::milliseconds(10),
                     std::max(std::chrono::milliseconds(1), remaining)));
    }
}

InterprocessFileLock::~InterprocessFileLock()
{
    release();
}

InterprocessFileLock::InterprocessFileLock(
    InterprocessFileLock&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      locked_(std::exchange(other.locked_, false)),
      error_(std::move(other.error_))
{
}

InterprocessFileLock& InterprocessFileLock::operator=(
    InterprocessFileLock&& other) noexcept
{
    if (this == &other) return *this;
    release();
    fd_ = std::exchange(other.fd_, -1);
    locked_ = std::exchange(other.locked_, false);
    error_ = std::move(other.error_);
    return *this;
}

void InterprocessFileLock::release() noexcept
{
    if (fd_ >= 0) {
        if (locked_) (void)::flock(fd_, LOCK_UN);
        (void)::close(fd_);
    }
    fd_ = -1;
    locked_ = false;
}

}  // namespace daedalus
