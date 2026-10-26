#include "data_archive.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>

#define STBI_SUPPORT_ZLIB
#include "stb_image.h"

namespace {

constexpr std::uint32_t kEndSignature = 0x06054B50;
constexpr std::uint32_t kCentralSignature = 0x02014B50;
constexpr std::uint32_t kLocalSignature = 0x04034B50;
constexpr std::uint32_t kDescriptorSignature = 0x08074B50;
constexpr std::uint16_t kDataDescriptorFlag = 1U << 3;
constexpr std::uint16_t kUtf8Flag = 1U << 11;
constexpr std::uint16_t kAllowedFlags = kDataDescriptorFlag | kUtf8Flag | (1U << 1) | (1U << 2);
constexpr std::uint64_t kMaxTailSize = 22 + 0xFFFF;
constexpr std::size_t   kLocalHeaderSize = 30;
constexpr std::size_t   kCentralHeaderSize = 46;
constexpr std::size_t   kMaxBulkIndexSize = 16 * 1024 * 1024;

std::uint16_t Read16(const std::uint8_t *bytes) {
    return static_cast<std::uint16_t>(bytes[0]) | static_cast<std::uint16_t>(bytes[1] << 8);
}

std::uint32_t Read32(const std::uint8_t *bytes) {
    return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
           (static_cast<std::uint32_t>(bytes[2]) << 16) | (static_cast<std::uint32_t>(bytes[3]) << 24);
}

constexpr std::array<std::uint32_t, 256> MakeCrcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::size_t value = 0; value < table.size(); ++value) {
        std::uint32_t crc = static_cast<std::uint32_t>(value);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320 & (0U - (crc & 1U)));
        }
        table[value] = crc;
    }
    return table;
}

constexpr std::array<std::array<std::uint32_t, 256>, 8> MakeCrcTables() {
    std::array<std::array<std::uint32_t, 256>, 8> tables{};
    tables[0] = MakeCrcTable();
    for (std::size_t slice = 1; slice < tables.size(); ++slice) {
        for (std::size_t value = 0; value < tables[slice].size(); ++value) {
            const std::uint32_t crc = tables[slice - 1][value];
            tables[slice][value] = tables[0][crc & 0xFF] ^ (crc >> 8);
        }
    }
    return tables;
}

constexpr auto        kCrcTables = MakeCrcTables();
constexpr const auto &kCrcTable = kCrcTables[0];

std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFF;
    for (const std::uint8_t byte : bytes) {
        crc = kCrcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

std::uint32_t Crc32Slice8(std::span<const std::uint8_t> bytes) {
    std::uint32_t crc = 0xFFFFFFFF;
    while (bytes.size() >= 8) {
        const std::uint32_t first = crc ^ Read32(bytes.data());
        crc = kCrcTables[7][first & 0xFF] ^ kCrcTables[6][(first >> 8) & 0xFF] ^
              kCrcTables[5][(first >> 16) & 0xFF] ^ kCrcTables[4][first >> 24] ^
              kCrcTables[3][bytes[4]] ^ kCrcTables[2][bytes[5]] ^
              kCrcTables[1][bytes[6]] ^ kCrcTables[0][bytes[7]];
        bytes = bytes.subspan(8);
    }
    for (const std::uint8_t byte : bytes) {
        crc = kCrcTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

bool CrcSlice8Enabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DC_ARCHIVE_CRC_SLICE8");
        return value == nullptr || std::string_view(value) == "1";
    }();
    return enabled;
}

bool BulkIndexEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DC_ARCHIVE_BULK_INDEX");
        return value == nullptr || std::string_view(value) == "1";
    }();
    return enabled;
}

bool SequentialIndexEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DC_ARCHIVE_SEQUENTIAL_INDEX");
        return value == nullptr || std::string_view(value) == "1";
    }();
    return enabled;
}

bool LoadPerfEnabled() {
    static const bool enabled = [] {
        const char *value = std::getenv("DC_PERF_LOAD");
        return value != nullptr && std::string_view(value) == "1";
    }();
    return enabled;
}

using LoadClock = std::chrono::steady_clock;

double LoadMilliseconds(LoadClock::time_point start, LoadClock::time_point end) {
    return std::chrono::duration<double, std::milli>(end - start).count();
}

bool SafeEntryName(std::string_view name) {
    if (name.empty() || name.front() == '/' || name.find('\\') != std::string_view::npos ||
        name.find(':') != std::string_view::npos || name.find('\0') != std::string_view::npos) {
        return false;
    }
    if (name.back() == '/') {
        name.remove_suffix(1);
    }
    if (name.empty()) {
        return false;
    }
    for (std::size_t start = 0; start < name.size();) {
        const std::size_t      slash = name.find('/', start);
        const std::string_view part = name.substr(start, slash == std::string_view::npos ? name.size() - start : slash - start);
        if (part.empty() || part == "." || part == "..") {
            return false;
        }
        for (const unsigned char character : part) {
            if (character < 0x20 || character == 0x7F) {
                return false;
            }
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return true;
}

[[noreturn]] void Invalid(const std::filesystem::path &path, std::string_view reason) {
    throw std::runtime_error(path.string() + ": " + std::string(reason));
}

} // namespace

DataArchive::DataArchive(const std::filesystem::path &path)
    : path_(path), stream_(path, std::ios::binary) {
    const bool perf_load = LoadPerfEnabled();
    const bool sequential_index = SequentialIndexEnabled();
    const bool bulk_requested = BulkIndexEnabled();
    const auto started = perf_load ? LoadClock::now() : LoadClock::time_point{};
    if (!stream_) {
        Invalid(path_, "cannot open ZIP archive");
    }
    std::error_code     error;
    const std::uint64_t file_size = std::filesystem::file_size(path_, error);
    if (error || file_size < 22) {
        Invalid(path_, "truncated ZIP archive");
    }

    const std::uint64_t       tail_size = std::min(file_size, kMaxTailSize);
    std::vector<std::uint8_t> tail(static_cast<std::size_t>(tail_size));
    stream_.seekg(static_cast<std::streamoff>(file_size - tail_size));
    stream_.read(reinterpret_cast<char *>(tail.data()), static_cast<std::streamsize>(tail.size()));
    if (static_cast<std::size_t>(stream_.gcount()) != tail.size()) {
        Invalid(path_, "cannot read ZIP trailer");
    }

    std::size_t end_at = std::string::npos;
    for (std::size_t at = tail.size() - 22 + 1; at-- > 0;) {
        if (Read32(tail.data() + at) == kEndSignature && at + 22 + Read16(tail.data() + at + 20) == tail.size()) {
            end_at = at;
            break;
        }
    }
    if (end_at == std::string::npos) {
        Invalid(path_, "ZIP end record not found");
    }
    const std::uint8_t *end = tail.data() + end_at;
    const std::uint16_t disk = Read16(end + 4);
    const std::uint16_t central_disk = Read16(end + 6);
    const std::uint16_t disk_entries = Read16(end + 8);
    const std::uint16_t total_entries = Read16(end + 10);
    const std::uint32_t central_size = Read32(end + 12);
    const std::uint32_t central_offset = Read32(end + 16);
    if (disk != 0 || central_disk != 0 || disk_entries != total_entries) {
        Invalid(path_, "multi-disk ZIP archives are unsupported");
    }
    if (disk_entries == 0xFFFF || total_entries == 0xFFFF || central_size == 0xFFFFFFFF ||
        central_offset == 0xFFFFFFFF) {
        Invalid(path_, "ZIP64 archives are unsupported");
    }
    const std::uint64_t trailer_offset = file_size - tail_size + end_at;
    central_directory_offset_ = central_offset;
    if (central_offset > trailer_offset || central_size > trailer_offset - central_offset) {
        Invalid(path_, "ZIP central directory is outside the archive");
    }

    records_.reserve(total_entries);
    stream_.clear();
    stream_.seekg(static_cast<std::streamoff>(central_offset));
    const bool                          bulk_index = bulk_requested && central_size <= kMaxBulkIndexSize;
    std::unique_ptr<std::istringstream> bulk_stream;
    if (bulk_index) {
        std::string storage(central_size, '\0');
        stream_.read(storage.data(), static_cast<std::streamsize>(storage.size()));
        if (static_cast<std::size_t>(stream_.gcount()) != storage.size()) {
            Invalid(path_, "cannot read ZIP central directory");
        }
        bulk_stream = std::make_unique<std::istringstream>(std::move(storage), std::ios::binary);
    }
    std::istream &central_stream = bulk_stream ? static_cast<std::istream &>(*bulk_stream) : stream_;
    std::uint64_t consumed = 0;
    for (std::uint32_t index = 0; index < total_entries; ++index) {
        std::array<std::uint8_t, kCentralHeaderSize> header{};
        central_stream.read(reinterpret_cast<char *>(header.data()), header.size());
        if (static_cast<std::size_t>(central_stream.gcount()) != header.size() || Read32(header.data()) != kCentralSignature) {
            Invalid(path_, "invalid ZIP central directory entry");
        }
        const std::uint16_t flags = Read16(header.data() + 8);
        const std::uint16_t method = Read16(header.data() + 10);
        const std::uint32_t crc = Read32(header.data() + 16);
        const std::uint32_t compressed_size = Read32(header.data() + 20);
        const std::uint32_t size = Read32(header.data() + 24);
        const std::uint16_t name_size = Read16(header.data() + 28);
        const std::uint16_t extra_size = Read16(header.data() + 30);
        const std::uint16_t comment_size = Read16(header.data() + 32);
        const std::uint16_t entry_disk = Read16(header.data() + 34);
        const std::uint32_t local_offset = Read32(header.data() + 42);
        if (entry_disk != 0) {
            Invalid(path_, "multi-disk ZIP entries are unsupported");
        }
        if (compressed_size == 0xFFFFFFFF || size == 0xFFFFFFFF || local_offset == 0xFFFFFFFF) {
            Invalid(path_, "ZIP64 entries are unsupported");
        }
        if ((flags & ~kAllowedFlags) != 0 || (flags & 1U) != 0 || (flags & (1U << 6)) != 0 ||
            (flags & (1U << 13)) != 0) {
            Invalid(path_, "encrypted or unsupported ZIP flags");
        }
        if (method != 0 && method != 8) {
            Invalid(path_, "only stored and deflated ZIP entries are supported");
        }
        if (method == 0 && (flags & ((1U << 1) | (1U << 2))) != 0) {
            Invalid(path_, "deflate flags on a stored ZIP entry");
        }

        std::string name(name_size, '\0');
        central_stream.read(name.data(), name_size);
        std::vector<std::uint8_t> extra(extra_size);
        central_stream.read(reinterpret_cast<char *>(extra.data()), extra_size);
        central_stream.ignore(comment_size);
        if (!central_stream || !SafeEntryName(name)) {
            Invalid(path_, "invalid or unsafe ZIP entry name");
        }
        for (std::size_t at = 0; at < extra.size();) {
            if (extra.size() - at < 4) {
                Invalid(path_, "truncated ZIP extra field");
            }
            const std::uint16_t tag = Read16(extra.data() + at);
            const std::uint16_t length = Read16(extra.data() + at + 2);
            at += 4;
            if (length > extra.size() - at) {
                Invalid(path_, "invalid ZIP extra field");
            }
            if (tag == 1) {
                Invalid(path_, "ZIP64 entries are unsupported");
            }
            at += length;
        }

        consumed += kCentralHeaderSize + name_size + extra_size + comment_size;
        if (consumed > central_size) {
            Invalid(path_, "ZIP central directory size mismatch");
        }
        const bool directory = name.back() == '/';
        Record     record{
            .entry = {.name = std::move(name), .size = size},
            .crc = crc,
            .compressed_size = compressed_size,
            .local_header_offset = local_offset,
            .method = method,
            .flags = flags
        };
        if (!directory) {
            if (local_offset >= central_offset || compressed_size > central_offset - local_offset) {
                Invalid(path_, "ZIP local entry offset is outside the file area");
            }
        }
        records_.push_back(std::move(record));
        if (!sequential_index) {
            central_stream.clear();
            central_stream.seekg(static_cast<std::streamoff>(bulk_index ? consumed : central_offset + consumed));
        }
    }
    if (consumed != central_size) {
        Invalid(path_, "ZIP central directory size mismatch");
    }
    if (perf_load) {
        const auto finished = LoadClock::now();
        std::fprintf(stderr,
                     "perf load archive index entries %zu central_bytes %u sequential_index %d index_ms %.6f "
                     "bulk_index %d bulk_fallback_count %u bulk_fallback_reason %s\n",
                     records_.size(), static_cast<unsigned>(central_size), static_cast<int>(sequential_index),
                     LoadMilliseconds(started, finished), static_cast<int>(bulk_index),
                     static_cast<unsigned>(bulk_requested && !bulk_index),
                     !bulk_requested ? "disabled" : (bulk_index ? "none" : "central_size_cap"));
    }
}

std::size_t DataArchive::Count() const {
    return records_.size();
}

const DataArchive::Entry &DataArchive::GetEntry(std::size_t index) const {
    return records_.at(index).entry;
}

bool DataArchive::Read(std::size_t index, std::span<std::uint8_t> output) const {
    const bool perf_load = LoadPerfEnabled();
    const auto started = perf_load ? LoadClock::now() : LoadClock::time_point{};
    if (index >= records_.size()) {
        return false;
    }
    Record &record = records_[index];
    if (record.entry.name.back() == '/' || record.entry.size != output.size() ||
        output.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        record.compressed_size > static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
        return false;
    }
    std::vector<std::uint8_t> compressed;
    LoadClock::time_point     locked;
    const auto                lock_started = perf_load ? LoadClock::now() : LoadClock::time_point{};
    {
        std::scoped_lock lock(stream_mutex_);
        if (perf_load) {
            locked = LoadClock::now();
        }
        const bool first_read = !record.local_header_checked;
        if (first_read) {
            std::array<std::uint8_t, kLocalHeaderSize> local{};
            stream_.clear();
            stream_.seekg(static_cast<std::streamoff>(record.local_header_offset));
            stream_.read(reinterpret_cast<char *>(local.data()), local.size());
            if (static_cast<std::size_t>(stream_.gcount()) != local.size() || Read32(local.data()) != kLocalSignature ||
                Read16(local.data() + 6) != record.flags || Read16(local.data() + 8) != record.method) {
                return false;
            }
            const std::uint16_t local_name_size = Read16(local.data() + 26);
            const std::uint16_t local_extra_size = Read16(local.data() + 28);
            std::string         local_name(local_name_size, '\0');
            stream_.read(local_name.data(), local_name_size);
            std::vector<std::uint8_t> local_extra(local_extra_size);
            stream_.read(reinterpret_cast<char *>(local_extra.data()), local_extra_size);
            if (!stream_ || local_name != record.entry.name) {
                return false;
            }
            for (std::size_t at = 0; at < local_extra.size();) {
                if (local_extra.size() - at < 4) {
                    return false;
                }
                const std::uint16_t tag = Read16(local_extra.data() + at);
                const std::uint16_t length = Read16(local_extra.data() + at + 2);
                at += 4;
                if (length > local_extra.size() - at || tag == 1) {
                    return false;
                }
                at += length;
            }
            if ((record.flags & kDataDescriptorFlag) == 0 &&
                (Read32(local.data() + 14) != record.crc ||
                 Read32(local.data() + 18) != record.compressed_size ||
                 Read32(local.data() + 22) != record.entry.size)) {
                return false;
            }
            const std::uint64_t data_offset = static_cast<std::uint64_t>(record.local_header_offset) +
                                              kLocalHeaderSize + local_name_size + local_extra_size;
            if (data_offset > central_directory_offset_ ||
                record.compressed_size > central_directory_offset_ - data_offset) {
                return false;
            }
            record.data_offset = data_offset;
            record.local_header_checked = true;
        }

        if (record.method == 0) {
            if (record.compressed_size != record.entry.size) {
                return false;
            }
            if (!first_read) {
                stream_.clear();
                stream_.seekg(static_cast<std::streamoff>(record.data_offset));
            }
            stream_.read(reinterpret_cast<char *>(output.data()), static_cast<std::streamsize>(output.size()));
            if (static_cast<std::size_t>(stream_.gcount()) != output.size()) {
                return false;
            }
        } else {
            compressed.resize(record.compressed_size);
            if (!first_read) {
                stream_.clear();
                stream_.seekg(static_cast<std::streamoff>(record.data_offset));
            }
            stream_.read(reinterpret_cast<char *>(compressed.data()), static_cast<std::streamsize>(compressed.size()));
            if (static_cast<std::size_t>(stream_.gcount()) != compressed.size()) {
                return false;
            }
        }

        if ((record.flags & kDataDescriptorFlag) != 0) {
            const std::uint64_t descriptor_offset = record.data_offset + record.compressed_size;
            if (descriptor_offset > central_directory_offset_ || central_directory_offset_ - descriptor_offset < 12) {
                return false;
            }
            std::array<std::uint8_t, 16> descriptor{};
            stream_.clear();
            const std::uint64_t available = central_directory_offset_ - descriptor_offset;
            const std::size_t   descriptor_size = static_cast<std::size_t>(std::min<std::uint64_t>(available, descriptor.size()));
            stream_.read(reinterpret_cast<char *>(descriptor.data()), static_cast<std::streamsize>(descriptor_size));
            if (static_cast<std::size_t>(stream_.gcount()) != descriptor_size || descriptor_size < 12) {
                return false;
            }
            const bool unsigned_match = Read32(descriptor.data()) == record.crc &&
                                        Read32(descriptor.data() + 4) == record.compressed_size &&
                                        Read32(descriptor.data() + 8) == record.entry.size;
            const bool signed_match = descriptor_size == 16 && Read32(descriptor.data()) == kDescriptorSignature &&
                                      Read32(descriptor.data() + 4) == record.crc &&
                                      Read32(descriptor.data() + 8) == record.compressed_size &&
                                      Read32(descriptor.data() + 12) == record.entry.size;
            if (!unsigned_match && !signed_match) {
                return false;
            }
        }
    }

    const auto io_finished = perf_load ? LoadClock::now() : LoadClock::time_point{};
    auto       inflate_finished = io_finished;
    if (record.method == 8) {
        char      empty_output = 0;
        char     *output_data = output.empty() ? &empty_output : reinterpret_cast<char *>(output.data());
        const int decoded = stbi_zlib_decode_noheader_buffer(output_data,
                                                             static_cast<int>(output.size()),
                                                             reinterpret_cast<const char *>(compressed.data()),
                                                             static_cast<int>(compressed.size()));
        if (decoded != static_cast<int>(output.size())) {
            return false;
        }
    }
    if (perf_load && record.method == 8) {
        inflate_finished = LoadClock::now();
    }
    const bool slice8 = CrcSlice8Enabled();
    const auto crc_started = perf_load ? LoadClock::now() : LoadClock::time_point{};
    const bool valid = (slice8 ? Crc32Slice8(output) : Crc32(output)) == record.crc;
    if (perf_load && valid) {
        const auto finished = LoadClock::now();
        std::fprintf(stderr,
                     "perf load archive entry %zu bytes %llu method %u crc_slice8 %d "
                     "total_wall_ms %.6f lock_wait_ms %.6f IO_wall_ms %.6f "
                     "inflate_wall_ms %.6f crc_wall_ms %.6f name %s\n",
                     index, static_cast<unsigned long long>(output.size()),
                     static_cast<unsigned>(record.method), static_cast<int>(slice8),
                     LoadMilliseconds(started, finished), LoadMilliseconds(lock_started, locked),
                     LoadMilliseconds(locked, io_finished), LoadMilliseconds(io_finished, inflate_finished),
                     LoadMilliseconds(crc_started, finished), record.entry.name.c_str());
    }
    return valid;
}

bool DataArchivePresent(const std::filesystem::path &data_root) {
    std::error_code error;
    return std::filesystem::is_regular_file(data_root.parent_path() / "data.zip", error);
}
