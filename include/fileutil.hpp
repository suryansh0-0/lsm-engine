#pragma once
// Thin POSIX helpers: full writes and fsync (force data from the OS cache to disk).
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

namespace lsm {

[[noreturn]] inline void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
}

inline void write_all(int fd, const char* p, size_t n) {
    while (n > 0) {
        ssize_t w = ::write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            throw_errno("write");
        }
        p += w;
        n -= static_cast<size_t>(w);
    }
}

// Works for both files and directories (fsync on a directory persists renames).
inline void fsync_path(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) throw_errno("open " + path);
    int rc = ::fsync(fd);
    ::close(fd);
    if (rc != 0) throw_errno("fsync " + path);
}

}  // namespace lsm
