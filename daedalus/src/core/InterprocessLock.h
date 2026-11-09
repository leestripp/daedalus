// SPDX-License-Identifier: BSL-1.0
// src/core/InterprocessLock.h
//
// Small RAII wrapper around flock(2). A lock is associated with one open
// file description, so independent holders are serialised both across
// processes and across independent instances in the same process.

#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace daedalus {

enum class InterprocessLockMode
{
    shared,
    exclusive,
};

class InterprocessFileLock
{
public:
    static constexpr std::chrono::milliseconds kDefaultTimeout{5000};

    InterprocessFileLock(
        const std::filesystem::path& path,
        InterprocessLockMode mode,
        std::chrono::milliseconds timeout = kDefaultTimeout);
    ~InterprocessFileLock();

    InterprocessFileLock(const InterprocessFileLock&) = delete;
    InterprocessFileLock& operator=(const InterprocessFileLock&) = delete;
    InterprocessFileLock(InterprocessFileLock&& other) noexcept;
    InterprocessFileLock& operator=(InterprocessFileLock&& other) noexcept;

    bool acquired() const noexcept { return locked_; }
    explicit operator bool() const noexcept { return acquired(); }
    const std::string& error() const noexcept { return error_; }

private:
    void release() noexcept;

    int fd_ = -1;
    bool locked_ = false;
    std::string error_;
};

}  // namespace daedalus
