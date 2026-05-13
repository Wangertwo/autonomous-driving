#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;

constexpr std::array<char, 4> kMagic{'A', 'D', 'L', 'G'};
constexpr std::uint32_t kVersion = 1;
constexpr std::uint32_t kRecordHeaderSize =
    sizeof(std::uint64_t) + sizeof(std::uint16_t) + sizeof(std::uint32_t);

struct RecordHeader
{
    std::uint64_t timestamp_ns_;
    std::uint16_t topic_len_;
    std::uint32_t payload_len_;
};

struct RecordMeta
{
    std::uint64_t offset_;
    std::uint64_t timestamp_ns_;
    std::string topic_;
    std::uint32_t payload_len_;
    std::uint64_t total_size_;
};

class FileDescriptor
{
public:
    explicit FileDescriptor(fs::path const& path) : fd_(::open(path.c_str(), O_RDONLY))
    {
        if (fd_ < 0)
        {
            throw std::runtime_error("open failed: " + path.string());
        }
    }

    ~FileDescriptor()
    {
        if (fd_ >= 0)
        {
            ::close(fd_);
        }
    }

    FileDescriptor(FileDescriptor const&) = delete;
    FileDescriptor& operator=(FileDescriptor const&) = delete;

    int Get() const
    {
        return fd_;
    }

private:
    int fd_ = -1;
};

std::uint64_t FileSize(int fd)
{
    struct stat st
    {
    };

    if (::fstat(fd, &st) != 0)
    {
        throw std::runtime_error("fstat failed");
    }
    return static_cast<std::uint64_t>(st.st_size);
}

std::uint64_t CheckedAdd(std::uint64_t lhs, std::uint64_t rhs)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    {
        throw std::runtime_error("offset overflow");
    }
    return lhs + rhs;
}

void CheckPreadOffset(std::uint64_t offset)
{
    if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
    {
        throw std::runtime_error("offset exceeds off_t range; rebuild with large-file support");
    }
}

bool ReadExactAt(int fd, void* dst, std::size_t size, std::uint64_t offset)
{
    auto* out = static_cast<std::byte*>(dst);
    std::size_t done = 0;
    while (done < size)
    {
        std::uint64_t const current_offset = CheckedAdd(offset, done);
        CheckPreadOffset(current_offset);
        ssize_t const n = ::pread(fd, out + done, size - done, static_cast<off_t>(current_offset));
        if (n == 0)
        {
            return false;
        }
        if (n < 0)
        {
            throw std::runtime_error(std::string("pread failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

// This teaching format uses host byte order; production formats should choose a
// fixed byte order.
void WriteU16(std::ofstream& out, std::uint16_t value)
{
    out.write(reinterpret_cast<char const*>(&value), sizeof(value));
}

void WriteU32(std::ofstream& out, std::uint32_t value)
{
    out.write(reinterpret_cast<char const*>(&value), sizeof(value));
}

void WriteU64(std::ofstream& out, std::uint64_t value)
{
    out.write(reinterpret_cast<char const*>(&value), sizeof(value));
}

void GenerateLog(fs::path const& path, std::size_t count)
{
    std::ofstream out(path, std::ios::binary);
    if (!out)
    {
        throw std::runtime_error("create failed: " + path.string());
    }

    out.write(kMagic.data(), kMagic.size());
    WriteU32(out, kVersion);

    std::array<std::string_view, 4> const topics{"/camera/front", "/lidar/points",
                                                 "/planning/trajectory", "/canbus/chassis"};
    for (std::size_t i = 0; i < count; ++i)
    {
        auto const topic = topics[i % topics.size()];
        std::string const payload =
            "payload-" + std::to_string(i) + std::string((i % 97) + 16, 'x');

        WriteU64(out, 1'700'000'000'000'000'000ULL + i * 10'000'000ULL);
        WriteU16(out, static_cast<std::uint16_t>(topic.size()));
        WriteU32(out, static_cast<std::uint32_t>(payload.size()));
        out.write(topic.data(), static_cast<std::streamsize>(topic.size()));
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }
}

std::uint64_t ReadCheckpoint(fs::path const& path)
{
    std::ifstream in(path);
    std::uint64_t offset = 0;
    in >> offset;
    return offset;
}

void WriteAll(int fd, std::string_view data)
{
    std::size_t done = 0;
    while (done < data.size())
    {
        ssize_t const n = ::write(fd, data.data() + done, data.size() - done);
        if (n < 0)
        {
            throw std::runtime_error(std::string("write failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
}

void WriteCheckpoint(fs::path const& path, std::uint64_t offset)
{
    fs::path const temp_path = path.string() + ".tmp";
    int const fd = ::open(temp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
    {
        throw std::runtime_error("checkpoint temp create failed: " + temp_path.string());
    }

    try
    {
        WriteAll(fd, std::to_string(offset) + "\n");
        if (::fsync(fd) != 0)
        {
            throw std::runtime_error(std::string("checkpoint fsync failed: ") +
                                     std::strerror(errno));
        }
        if (::close(fd) != 0)
        {
            throw std::runtime_error(std::string("checkpoint close failed: ") +
                                     std::strerror(errno));
        }
        fs::rename(temp_path, path);
    }
    catch (...)
    {
        ::close(fd);
        fs::remove(temp_path);
        throw;
    }
}

void VerifyHeader(int fd)
{
    std::array<char, 4> magic{};
    std::uint32_t version = 0;
    if (!ReadExactAt(fd, magic.data(), magic.size(), 0) || magic != kMagic)
    {
        throw std::runtime_error("bad magic");
    }
    if (!ReadExactAt(fd, &version, sizeof(version), kMagic.size()) || version != kVersion)
    {
        throw std::runtime_error("bad version");
    }
}

bool ReadRecordMeta(int fd, std::uint64_t offset, std::uint64_t size, RecordMeta& meta)
{
    if (offset > size || CheckedAdd(offset, kRecordHeaderSize) > size)
    {
        return false;
    }

    RecordHeader header{};
    std::uint64_t cursor = offset;
    if (!ReadExactAt(fd, &header.timestamp_ns_, sizeof(header.timestamp_ns_), cursor))
    {
        return false;
    }
    cursor += sizeof(header.timestamp_ns_);
    if (!ReadExactAt(fd, &header.topic_len_, sizeof(header.topic_len_), cursor))
    {
        return false;
    }
    cursor += sizeof(header.topic_len_);
    if (!ReadExactAt(fd, &header.payload_len_, sizeof(header.payload_len_), cursor))
    {
        return false;
    }
    cursor += sizeof(header.payload_len_);

    std::uint64_t const total_size =
        CheckedAdd(CheckedAdd(kRecordHeaderSize, header.topic_len_), header.payload_len_);
    if (CheckedAdd(offset, total_size) > size)
    {
        return false;
    }

    std::string topic(header.topic_len_, '\0');
    if (!ReadExactAt(fd, topic.data(), topic.size(), cursor))
    {
        return false;
    }

    meta =
        RecordMeta{offset, header.timestamp_ns_, std::move(topic), header.payload_len_, total_size};
    return true;
}

std::uint64_t ValidateCheckpoint(int fd, std::uint64_t size, std::uint64_t checkpoint)
{
    std::uint64_t const first_record = kMagic.size() + sizeof(kVersion);
    if (checkpoint <= first_record)
    {
        return first_record;
    }

    std::uint64_t offset = first_record;
    RecordMeta meta{};
    while (offset < checkpoint && ReadRecordMeta(fd, offset, size, meta))
    {
        offset = CheckedAdd(offset, meta.total_size_);
    }
    if (offset == checkpoint)
    {
        return checkpoint;
    }

    std::cerr << "checkpoint is not on a record boundary; restarting from file "
                 "header\n";
    return first_record;
}

void ScanLog(fs::path const& path, fs::path const& checkpoint_path, std::uint64_t max_records)
{
    FileDescriptor file(path);
    VerifyHeader(file.Get());
    std::uint64_t const size = FileSize(file.Get());
    std::uint64_t offset = ValidateCheckpoint(file.Get(), size, ReadCheckpoint(checkpoint_path));
    std::map<std::string, std::uint64_t> topic_counts;

    std::uint64_t scanned = 0;
    RecordMeta meta{};
    while ((max_records == 0 || scanned < max_records) &&
           ReadRecordMeta(file.Get(), offset, size, meta))
    {
        ++topic_counts[meta.topic_];
        offset = CheckedAdd(offset, meta.total_size_);
        ++scanned;
    }

    WriteCheckpoint(checkpoint_path, offset);

    std::cout << "scanned_records=" << scanned << '\n';
    std::cout << "next_offset=" << offset << '\n';
    for (auto const& [topic, count] : topic_counts)
    {
        std::cout << topic << ' ' << count << '\n';
    }
}

std::string CsvEscape(std::string_view value)
{
    bool needs_quotes = false;
    std::string escaped;
    for (char const c : value)
    {
        if (c == '"')
        {
            escaped += "\"\"";
            needs_quotes = true;
        }
        else
        {
            escaped += c;
            needs_quotes = needs_quotes || c == ',' || c == '\n' || c == '\r';
        }
    }
    if (!needs_quotes)
    {
        return escaped;
    }
    return '"' + escaped + '"';
}

void BuildIndex(fs::path const& path, fs::path const& index_path)
{
    FileDescriptor file(path);
    VerifyHeader(file.Get());
    std::uint64_t const size = FileSize(file.Get());
    std::ofstream index(index_path);
    if (!index)
    {
        throw std::runtime_error("create failed: " + index_path.string());
    }

    std::uint64_t offset = kMagic.size() + sizeof(kVersion);
    RecordMeta meta{};
    while (ReadRecordMeta(file.Get(), offset, size, meta))
    {
        index << meta.timestamp_ns_ << ',' << offset << ',' << CsvEscape(meta.topic_) << ','
              << meta.payload_len_ << '\n';
        offset = CheckedAdd(offset, meta.total_size_);
    }
}

void CutWindow(fs::path const& input_path, fs::path const& output_path, std::uint64_t begin_ns,
               std::uint64_t end_ns)
{
    FileDescriptor input(input_path);
    VerifyHeader(input.Get());
    std::uint64_t const size = FileSize(input.Get());
    std::ofstream output(output_path, std::ios::binary);
    output.write(kMagic.data(), kMagic.size());
    WriteU32(output, kVersion);

    std::uint64_t offset = kMagic.size() + sizeof(kVersion);
    RecordMeta meta{};
    std::vector<char> record;
    while (ReadRecordMeta(input.Get(), offset, size, meta))
    {
        if (begin_ns <= meta.timestamp_ns_ && meta.timestamp_ns_ < end_ns)
        {
            record.resize(meta.total_size_);
            if (!ReadExactAt(input.Get(), record.data(), record.size(), offset))
            {
                throw std::runtime_error("record disappeared while cutting");
            }
            output.write(record.data(), static_cast<std::streamsize>(record.size()));
        }
        offset = CheckedAdd(offset, meta.total_size_);
    }
}

void Usage(char const* argv0)
{
    std::cerr << "usage:\n"
              << "  " << argv0 << " generate <log> <record_count>\n"
              << "  " << argv0 << " scan <log> <checkpoint> [max_records]\n"
              << "  " << argv0 << " index <log> <index.csv>\n"
              << "  " << argv0 << " cut <input_log> <output_log> <begin_ns> <end_ns>\n";
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 2)
        {
            Usage(argv[0]);
            return 2;
        }

        std::string const command = argv[1];
        if (command == "generate" && argc == 4)
        {
            GenerateLog(argv[2], std::stoull(argv[3]));
        }
        else if (command == "scan" && (argc == 4 || argc == 5))
        {
            ScanLog(argv[2], argv[3], argc == 5 ? std::stoull(argv[4]) : 0);
        }
        else if (command == "index" && argc == 4)
        {
            BuildIndex(argv[2], argv[3]);
        }
        else if (command == "cut" && argc == 6)
        {
            CutWindow(argv[2], argv[3], std::stoull(argv[4]), std::stoull(argv[5]));
        }
        else
        {
            Usage(argv[0]);
            return 2;
        }
    }
    catch (std::exception const& e)
    {
        std::cerr << "error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
