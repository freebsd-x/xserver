#pragma once

#include <unistd.h>
#include <utility>
#include <expected>
#include <system_error>

namespace fbx::xserver {

class UniqueFd {
public:
    constexpr UniqueFd() noexcept : fd_(-1) {}
    explicit constexpr UniqueFd(int fd) noexcept : fd_(fd) {}

    ~UniqueFd() noexcept {
        reset();
    }

    constexpr UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

    constexpr UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.fd_, -1));
        }
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] constexpr int get() const noexcept { return fd_; }
    [[nodiscard]] constexpr bool isValid() const noexcept { return fd_ >= 0; }

    explicit constexpr operator bool() const noexcept { return isValid(); }

    void reset(int new_fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = new_fd;
    }

    [[nodiscard]] constexpr int release() noexcept {
        return std::exchange(fd_, -1);
    }

    [[nodiscard]] std::expected<UniqueFd, std::error_code> duplicate() const noexcept {
        if (!isValid()) {
            return std::unexpected(std::make_error_code(std::errc::bad_file_descriptor));
        }
        int new_fd = ::dup(fd_);
        if (new_fd < 0) {
            return std::unexpected(std::error_code(errno, std::generic_category()));
        }
        return UniqueFd(new_fd);
    }

private:
    int fd_;
};

} // namespace fbx::xserver
