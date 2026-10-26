#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

/**
 * Reads a conventional ZIP archive of game data, or the game data on a disc in place.
 *
 * Stored and deflated ZIP entries are supported; malformed archives fail when opened or read. A disc
 * (an image, or a directory holding DATA.DAT and DATA.HD2) lists the files DATA.HD2 indexes, read
 * from DATA.DAT, with the loose files of a data directory over them: what dcdata writes beside the
 * disc's files (normalized/, data.hd2, languages.json and the aliases), which a disc lacks.
 */
class DataArchive {
public:
    /**
     * One file name and its uncompressed size from the archive.
     */
    struct Entry {
        std::string   name;     /**< Slash-separated path, in archive case. */
        std::uint64_t size = 0; /**< Uncompressed bytes. */
    };

    /**
     * Opens the archive and validates its central directory; local headers are checked when read.
     */
    explicit DataArchive(const std::filesystem::path &path);

    /**
     * Opens a disc's index; loose files under overlay, by their path relative to it, replace the
     * disc's file of the same folded name or add to them.
     */
    static std::unique_ptr<DataArchive> OpenDisc(const std::filesystem::path &disc,
                                                 const std::filesystem::path &overlay);

    ~DataArchive();

    /**
     * Returns the number of files listed by the archive.
     */
    std::size_t Count() const;

    /**
     * Returns the name and uncompressed size for an archive entry.
     */
    const Entry &GetEntry(std::size_t index) const;

    /**
     * Reads one entry and verifies its uncompressed size and, from a ZIP archive, its CRC-32.
     */
    bool Read(std::size_t index, std::span<std::uint8_t> output) const;

private:
    enum class Source : std::uint8_t { kZip, kDisc, kLoose };

    DataArchive() = default;

    struct Record {
        Entry                 entry;
        std::uint32_t         crc = 0;
        std::uint32_t         compressed_size = 0;
        std::uint32_t         local_header_offset = 0;
        std::uint16_t         method = 0;
        std::uint16_t         flags = 0;
        mutable std::uint64_t data_offset = 0;
        mutable bool          local_header_checked = false;
        Source                source = Source::kZip;
        std::uint64_t         disc_offset = 0; /**< Byte offset in the disc's DATA.DAT file. */
        std::filesystem::path loose_path;      /**< A loose file's path. */
    };

    std::filesystem::path       path_;
    mutable std::vector<Record> records_;
    mutable std::mutex          stream_mutex_;
    mutable std::ifstream       stream_;
    std::uint64_t               central_directory_offset_ = 0;
    int                         disc_file_ = -1; /**< DATA.DAT's descriptor, read with pread. */
};

/**
 * Reads size bytes at offset with as few system calls as the file system allows. An ifstream reads in its
 * buffer's small steps, each a round trip to the browser's storage on the web.
 */
bool DataArchiveReadAt(int file, std::uint64_t offset, void *output, std::size_t size);

/**
 * Reads a whole file the same way; false if it cannot be opened or is not size bytes.
 */
bool DataArchiveReadFile(const std::filesystem::path &path, void *output, std::size_t size);

/**
 * Reports whether the data root has a sibling data.zip archive or, on the web, a disc
 * (DataArchiveDisc).
 */
bool DataArchivePresent(const std::filesystem::path &data_root);

/**
 * The disc stored beside the data root, read in place: on the web, disc.iso or disc/ (the page's
 * port/web/site/shell.js stores them there) unless data.zip is there too. Empty otherwise.
 */
std::filesystem::path DataArchiveDisc(const std::filesystem::path &data_root);
