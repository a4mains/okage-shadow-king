#include "okage/save_store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <system_error>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace okage {
namespace {
constexpr std::uint32_t block_size = static_cast<std::uint32_t>(save_payload_size);
constexpr std::size_t envelope_header_size = 8;
constexpr std::size_t checksum_size = 4;
std::atomic<std::uint64_t> temporary_counter{0};

void put_u32_le(std::uint8_t* dst, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) dst[i] = static_cast<std::uint8_t>(value >> (8 * i));
}

std::uint32_t get_u32_le(const std::uint8_t* src) {
    return std::uint32_t(src[0]) | (std::uint32_t(src[1]) << 8) |
           (std::uint32_t(src[2]) << 16) | (std::uint32_t(src[3]) << 24);
}

std::string format_error(const char* operation, const std::filesystem::path& path, int error) {
    std::ostringstream out;
    out << operation << " '" << path.string() << "': " << std::error_code(error, std::generic_category()).message();
    return out.str();
}

class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(other.release()) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = other.release();
        }
        return *this;
    }
    int get() const { return value_; }
    int release() { int value = value_; value_ = -1; return value; }
private:
    int value_;
};

void write_all(int fd, std::span<const std::uint8_t> bytes, const std::filesystem::path& path) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw SaveIoError("write", path, errno);
        }
        if (count == 0) throw SaveIoError("write returned zero", path, EIO);
        offset += static_cast<std::size_t>(count);
    }
}

void read_all(int fd, std::span<std::uint8_t> bytes, const std::filesystem::path& path) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::read(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw SaveIoError("read", path, errno);
        }
        if (count == 0) throw SaveIoError("unexpected end of file", path, EIO);
        offset += static_cast<std::size_t>(count);
    }
}

Fd open_directory(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) throw SaveIoError("open save directory", path, errno);
    Fd directory(fd);
    struct stat st{};
    if (::fstat(fd, &st) != 0) throw SaveIoError("stat save directory", path, errno);
    if (!S_ISDIR(st.st_mode)) throw SaveIoError("save path is not a directory", path, ENOTDIR);
    return directory;
}

void validate_slot(unsigned slot) {
    if (slot > 2) throw std::invalid_argument("Save slot must be 0, 1, or 2");
}
}

SaveFormatError::SaveFormatError(Reason reason, const char* message)
    : std::runtime_error(message), reason_(reason) {}

std::uint32_t source_save_checksum(std::span<const std::uint8_t> bytes) {
    std::array<std::uint16_t, 256> table{};
    // 0x16d838 iterates while index < 0xff. Entry 255 remains BSS zero.
    for (std::uint32_t index = 0; index < 0xff; ++index) {
        std::uint32_t value = index << 8;
        for (unsigned bit = 0; bit < 8; ++bit)
            value = (value & 0x8000u) ? ((value << 1) ^ 0x1021u) : (value << 1);
        table[index] = static_cast<std::uint16_t>(value);
    }

    std::uint32_t state = 0xffffu;
    for (std::uint8_t byte : bytes) {
        const auto index = static_cast<std::uint8_t>((std::uint32_t(byte) ^ (state >> 8)) & 0xffu);
        state = (state << 8) ^ table[index];
    }
    return ~state;
}

std::vector<std::uint8_t> encode_source_save(const SavePayload& payload) {
    std::vector<std::uint8_t> file(source_save_file_size);
    file[0] = '!'; file[1] = 'c'; file[2] = 'r'; file[3] = 'c';
    put_u32_le(file.data() + 4, block_size);
    put_u32_le(file.data() + envelope_header_size, source_save_checksum(payload));
    std::copy(payload.begin(), payload.end(), file.begin() + envelope_header_size + checksum_size);
    return file;
}

SavePayload decode_source_save(std::span<const std::uint8_t> file) {
    if (file.size() != source_save_file_size)
        throw SaveFormatError(SaveFormatError::Reason::wrong_file_size, "Save file size is not one source 0xd78 block");
    if (file[0] != '!' || file[1] != 'c' || file[2] != 'r' || file[3] != 'c')
        throw SaveFormatError(SaveFormatError::Reason::bad_magic, "Save file header is not !crc");
    if (get_u32_le(file.data() + 4) != block_size)
        throw SaveFormatError(SaveFormatError::Reason::wrong_block_size, "Save file block size is not 0xd78");
    SavePayload payload{};
    std::copy(file.begin() + envelope_header_size + checksum_size, file.end(), payload.begin());
    if (get_u32_le(file.data() + envelope_header_size) != source_save_checksum(payload))
        throw SaveFormatError(SaveFormatError::Reason::checksum_mismatch, "Save file checksum mismatch");
    return payload;
}

SaveIoError::SaveIoError(std::string operation, const std::filesystem::path& path, int error)
    : std::runtime_error(format_error(operation.c_str(), path, error)), error_(error) {}

NativeSaveStore::NativeSaveStore(std::filesystem::path directory) : directory_(std::move(directory)) {
    if (directory_.empty()) throw std::invalid_argument("Save directory must not be empty");
}

std::string NativeSaveStore::filename(unsigned slot) {
    validate_slot(slot);
    return "bkmo" + std::to_string(slot) + ".dat";
}

std::optional<SavePayload> NativeSaveStore::read(unsigned slot) const {
    const auto name = filename(slot);
    struct stat root_stat{};
    if (::lstat(directory_.c_str(), &root_stat) != 0) {
        if (errno == ENOENT) return std::nullopt;
        throw SaveIoError("stat save directory", directory_, errno);
    }
    if (S_ISLNK(root_stat.st_mode)) throw SaveIoError("save directory is a symlink", directory_, ELOOP);
    auto root = open_directory(directory_);
    const int fd = ::openat(root.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) return std::nullopt;
        throw SaveIoError("open save file", directory_ / name, errno);
    }
    Fd file_fd(fd);
    struct stat st{};
    if (::fstat(fd, &st) != 0) throw SaveIoError("stat save file", directory_ / name, errno);
    if (!S_ISREG(st.st_mode)) throw SaveIoError("save entry is not a regular file", directory_ / name, EINVAL);
    if (st.st_size != static_cast<off_t>(source_save_file_size))
        throw SaveFormatError(SaveFormatError::Reason::wrong_file_size, "Save file size is not one source 0xd78 block");
    std::array<std::uint8_t, source_save_file_size> bytes{};
    read_all(fd, bytes, directory_ / name);
    std::uint8_t extra = 0;
    for (;;) {
        const auto count = ::read(fd, &extra, 1);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) throw SaveIoError("read save file tail", directory_ / name, errno);
        if (count != 0) throw SaveFormatError(SaveFormatError::Reason::wrong_file_size, "Save file has trailing bytes");
        break;
    }
    return decode_source_save(bytes);
}

void NativeSaveStore::write(unsigned slot, const SavePayload& payload) const {
    const auto name = filename(slot);
    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) throw SaveIoError("create save directory", directory_, ec.value());
    struct stat root_stat{};
    if (::lstat(directory_.c_str(), &root_stat) != 0) throw SaveIoError("stat save directory", directory_, errno);
    if (S_ISLNK(root_stat.st_mode)) throw SaveIoError("save directory is a symlink", directory_, ELOOP);
    auto root = open_directory(directory_);

    struct stat existing{};
    if (::fstatat(root.get(), name.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0) {
        if (S_ISLNK(existing.st_mode)) throw SaveIoError("save file is a symlink", directory_ / name, ELOOP);
        if (!S_ISREG(existing.st_mode)) throw SaveIoError("save entry is not a regular file", directory_ / name, EINVAL);
    } else if (errno != ENOENT) {
        throw SaveIoError("stat save file", directory_ / name, errno);
    }

    const auto bytes = encode_source_save(payload);
    std::string temporary;
    Fd temp_fd;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = ".bkmo" + std::to_string(slot) + ".tmp." + std::to_string(::getpid()) + "." +
                    std::to_string(temporary_counter.fetch_add(1, std::memory_order_relaxed));
        const int fd = ::openat(root.get(), temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0) { temp_fd = Fd(fd); break; }
        if (errno != EEXIST) throw SaveIoError("create temporary save file", directory_ / temporary, errno);
    }
    if (temp_fd.get() < 0) throw SaveIoError("create unique temporary save file", directory_, EEXIST);

    bool renamed = false;
    try {
        write_all(temp_fd.get(), bytes, directory_ / temporary);
        if (::fsync(temp_fd.get()) != 0) throw SaveIoError("fsync temporary save file", directory_ / temporary, errno);
        const int fd = temp_fd.release();
        if (::close(fd) != 0) throw SaveIoError("close temporary save file", directory_ / temporary, errno);
        if (::renameat(root.get(), temporary.c_str(), root.get(), name.c_str()) != 0)
            throw SaveIoError("replace save file", directory_ / name, errno);
        renamed = true;
        if (::fsync(root.get()) != 0) throw SaveIoError("fsync save directory", directory_, errno);
    } catch (...) {
        if (!renamed) ::unlinkat(root.get(), temporary.c_str(), 0);
        throw;
    }
}

} // namespace okage
