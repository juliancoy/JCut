#pragma once

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#else
#include <mutex>
#endif

namespace jcut::projectio {

class ScopedProjectSaveLock {
public:
    ScopedProjectSaveLock() = default;
    ~ScopedProjectSaveLock() { release(); }

    ScopedProjectSaveLock(const ScopedProjectSaveLock&) = delete;
    ScopedProjectSaveLock& operator=(const ScopedProjectSaveLock&) = delete;

    bool acquire(const std::filesystem::path& projectDirectory, std::string* errorOut = nullptr)
    {
        release();
        const std::filesystem::path lockPath = projectDirectory / ".jcut-project-save.lock";
#if defined(_WIN32)
        m_handle = CreateFileW(lockPath.wstring().c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,
                               nullptr,
                               OPEN_ALWAYS,
                               FILE_ATTRIBUTE_HIDDEN,
                               nullptr);
        if (m_handle == INVALID_HANDLE_VALUE) {
            if (errorOut) {
                *errorOut = "failed to lock project files for save";
            }
            return false;
        }
        return true;
#elif defined(__unix__) || defined(__APPLE__)
        m_fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0600);
        if (m_fd < 0) {
            if (errorOut) {
                *errorOut = "failed to open project save lock: " +
                    std::string(std::strerror(errno));
            }
            return false;
        }
        while (::flock(m_fd, LOCK_EX) != 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errorOut) {
                *errorOut = "failed to lock project files for save: " +
                    std::string(std::strerror(errno));
            }
            ::close(m_fd);
            m_fd = -1;
            return false;
        }
        return true;
#else
        m_lock = std::unique_lock<std::mutex>(s_fallbackMutex);
        (void)lockPath;
        (void)errorOut;
        return true;
#endif
    }

    void release()
    {
#if defined(_WIN32)
        if (m_handle != INVALID_HANDLE_VALUE) {
            CloseHandle(m_handle);
            m_handle = INVALID_HANDLE_VALUE;
        }
#elif defined(__unix__) || defined(__APPLE__)
        if (m_fd >= 0) {
            ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
            m_fd = -1;
        }
#else
        if (m_lock.owns_lock()) {
            m_lock.unlock();
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE m_handle = INVALID_HANDLE_VALUE;
#elif defined(__unix__) || defined(__APPLE__)
    int m_fd = -1;
#else
    inline static std::mutex s_fallbackMutex;
    std::unique_lock<std::mutex> m_lock;
#endif
};

} // namespace jcut::projectio
