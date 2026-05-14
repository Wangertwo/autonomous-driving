#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <sstream>
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

struct PayloadFormat
{
    std::string name_;
    std::string extension_;
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

void ReadExact(int fd, void* dst, std::size_t size)
{
    auto* out = static_cast<std::byte*>(dst);
    std::size_t done = 0;
    while (done < size)
    {
        ssize_t const n = ::read(fd, out + done, size - done);
        if (n == 0)
        {
            throw std::runtime_error("unexpected EOF");
        }
        if (n < 0)
        {
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
}

bool ReadRecordHeader(int fd, std::array<char, kRecordHeaderSize>& bytes)
{
    std::size_t done = 0;
    while (done < bytes.size())
    {
        ssize_t const n = ::read(fd, bytes.data() + done, bytes.size() - done);
        if (n == 0)
        {
            if (done == 0)
            {
                return false;
            }
            throw std::runtime_error("trailing partial record header");
        }
        if (n < 0)
        {
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        done += static_cast<std::size_t>(n);
    }
    return true;
}

std::uint64_t CheckedAdd(std::uint64_t lhs, std::uint64_t rhs)
{
    if (lhs > std::numeric_limits<std::uint64_t>::max() - rhs)
    {
        throw std::runtime_error("record size overflow");
    }
    return lhs + rhs;
}

void VerifyHeader(int fd)
{
    std::array<char, 4> magic{};
    std::uint32_t version = 0;
    ReadExact(fd, magic.data(), magic.size());
    ReadExact(fd, &version, sizeof(version));
    if (magic != kMagic)
    {
        throw std::runtime_error("bad magic");
    }
    if (version != kVersion)
    {
        throw std::runtime_error("bad version");
    }
}

RecordHeader ParseHeader(char const* data)
{
    RecordHeader header{};
    std::memcpy(&header.timestamp_ns_, data, sizeof(header.timestamp_ns_));
    std::memcpy(&header.topic_len_, data + sizeof(header.timestamp_ns_), sizeof(header.topic_len_));
    std::memcpy(&header.payload_len_,
                data + sizeof(header.timestamp_ns_) + sizeof(header.topic_len_),
                sizeof(header.payload_len_));
    return header;
}

PayloadFormat ParsePayloadFormat(std::string const& value)
{
    if (value == "raw")
    {
        return {value, ".bin"};
    }
    if (value == "json")
    {
        return {value, ".json"};
    }
    if (value == "protobuf")
    {
        return {value, ".pb"};
    }
    throw std::runtime_error("format must be raw, json, or protobuf");
}

std::string JsonEscape(std::string_view value)
{
    std::ostringstream out;
    for (unsigned char const ch : value)
    {
        switch (ch)
        {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20)
            {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << static_cast<int>(ch) << std::dec;
            }
            else
            {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

std::string PayloadFileName(std::uint64_t match_index, PayloadFormat const& format)
{
    std::ostringstream out;
    out << "payload_" << std::setw(12) << std::setfill('0') << match_index << format.extension_;
    return out.str();
}

void SkipBytes(int fd, std::uint64_t size, std::vector<char>& buffer)
{
    std::uint64_t remaining = size;
    while (remaining > 0)
    {
        std::size_t const chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        ReadExact(fd, buffer.data(), chunk);
        remaining -= chunk;
    }
}

void CopyBytesToFile(int fd, std::uint64_t size, fs::path const& path, std::vector<char>& buffer)
{
    std::ofstream output(path, std::ios::binary);
    if (!output)
    {
        throw std::runtime_error("create failed: " + path.string());
    }

    std::uint64_t remaining = size;
    while (remaining > 0)
    {
        std::size_t const chunk = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, static_cast<std::uint64_t>(buffer.size())));
        ReadExact(fd, buffer.data(), chunk);
        output.write(buffer.data(), static_cast<std::streamsize>(chunk));
        if (!output)
        {
            throw std::runtime_error("write failed: " + path.string());
        }
        remaining -= chunk;
    }
}

class BufferedRecordScanner
{
public:
    void Consume(std::string_view bytes)
    {
        std::size_t pos = 0;
        while (pos < bytes.size())
        {
            if (payload_remaining_ > 0)
            {
                std::uint64_t const available = bytes.size() - pos;
                std::uint64_t const skipped = std::min(payload_remaining_, available);
                payload_remaining_ -= skipped;
                pos += static_cast<std::size_t>(skipped);
                continue;
            }

            if (!has_header_)
            {
                std::size_t const header_needed = kRecordHeaderSize - header_bytes_.size();
                std::size_t const header_take = std::min(header_needed, bytes.size() - pos);
                header_bytes_.insert(header_bytes_.end(), bytes.begin() + pos,
                                     bytes.begin() + pos + header_take);
                pos += header_take;
                if (header_bytes_.size() < kRecordHeaderSize)
                {
                    break;
                }

                header_ = ParseHeader(header_bytes_.data());
                header_bytes_.clear();
                has_header_ = true;
            }

            std::size_t const topic_needed = header_.topic_len_ - topic_bytes_.size();
            std::size_t const topic_take = std::min(topic_needed, bytes.size() - pos);
            topic_bytes_.insert(topic_bytes_.end(), bytes.begin() + pos,
                                bytes.begin() + pos + topic_take);
            pos += topic_take;
            if (topic_bytes_.size() < header_.topic_len_)
            {
                break;
            }

            ++topic_counts_[std::string(topic_bytes_.begin(), topic_bytes_.end())];
            ++record_count_;
            payload_remaining_ = header_.payload_len_;
            topic_bytes_.clear();
            has_header_ = false;
        }
    }

    void Finish() const
    {
        if (payload_remaining_ != 0 || has_header_ || !header_bytes_.empty() ||
            !topic_bytes_.empty())
        {
            throw std::runtime_error("trailing partial record");
        }
    }

    std::uint64_t RecordCount() const
    {
        return record_count_;
    }

    std::map<std::string, std::uint64_t> const& TopicCounts() const
    {
        return topic_counts_;
    }

private:
    RecordHeader header_{};
    bool has_header_ = false;
    std::vector<char> header_bytes_;
    std::vector<char> topic_bytes_;
    std::uint64_t payload_remaining_ = 0;
    std::uint64_t record_count_ = 0;
    std::map<std::string, std::uint64_t> topic_counts_;
};

void ScanBuffered(fs::path const& path, std::size_t buffer_size)
{
    FileDescriptor file(path);
    VerifyHeader(file.Get());

    std::vector<char> buffer(buffer_size);
    BufferedRecordScanner scanner;
    std::uint64_t bytes_after_header = 0;

    while (true)
    {
        ssize_t const n = ::read(file.Get(), buffer.data(), buffer.size());
        if (n < 0)
        {
            throw std::runtime_error(std::string("read failed: ") + std::strerror(errno));
        }
        if (n == 0)
        {
            break;
        }

        scanner.Consume(std::string_view(buffer.data(), static_cast<std::size_t>(n)));
        bytes_after_header += static_cast<std::uint64_t>(n);
    }

    scanner.Finish();

    std::cout << "scanned_records=" << scanner.RecordCount() << '\n';
    std::cout << "read_bytes_after_header=" << bytes_after_header << '\n';
    for (auto const& [topic, count] : scanner.TopicCounts())
    {
        std::cout << topic << ' ' << count << '\n';
    }
}

void DumpTopicPayloads(fs::path const& path, std::string const& selected_topic,
                       fs::path const& output_dir, PayloadFormat const& format,
                       std::size_t buffer_size)
{
    FileDescriptor file(path);
    VerifyHeader(file.Get());
    fs::create_directories(output_dir);

    fs::path const manifest_path = output_dir / "manifest.jsonl";
    std::ofstream manifest(manifest_path);
    if (!manifest)
    {
        throw std::runtime_error("create failed: " + manifest_path.string());
    }

    std::vector<char> buffer(buffer_size);
    std::array<char, kRecordHeaderSize> header_bytes{};
    std::uint64_t record_index = 0;
    std::uint64_t matched_count = 0;
    while (ReadRecordHeader(file.Get(), header_bytes))
    {
        RecordHeader const header = ParseHeader(header_bytes.data());
        std::vector<char> topic(header.topic_len_);
        ReadExact(file.Get(), topic.data(), topic.size());
        std::string const topic_name(topic.begin(), topic.end());

        if (topic_name == selected_topic)
        {
            std::string const file_name = PayloadFileName(matched_count, format);
            CopyBytesToFile(file.Get(), header.payload_len_, output_dir / file_name, buffer);
            manifest << "{\"record_index\":" << record_index << ",\"match_index\":" << matched_count
                     << ",\"timestamp_ns\":" << header.timestamp_ns_ << ",\"topic\":\""
                     << JsonEscape(topic_name) << "\",\"payload_len\":" << header.payload_len_
                     << ",\"format\":\"" << format.name_ << "\",\"file\":\""
                     << JsonEscape(file_name) << "\"}\n";
            ++matched_count;
        }
        else
        {
            SkipBytes(file.Get(), header.payload_len_, buffer);
        }
        ++record_index;
    }

    std::cout << "dumped_payloads=" << matched_count << '\n';
    std::cout << "output_dir=" << output_dir << '\n';
    std::cout << "manifest=" << manifest_path << '\n';
}

void Usage(char const* argv0)
{
    std::cerr << "usage:\n"
              << "  " << argv0 << " scan <log> [buffer_bytes]\n"
              << "  " << argv0
              << " dump <log> <topic> <output_dir> [raw|json|protobuf] "
                 "[buffer_bytes]\n";
}

int main(int argc, char** argv)
{
    try
    {
        if (argc < 3)
        {
            Usage(argv[0]);
            return 2;
        }

        std::string const command = argv[1];
        if (command == "scan" && (argc == 3 || argc == 4))
        {
            std::size_t const buffer_size = argc == 4 ? std::stoull(argv[3]) : 4ULL * 1024 * 1024;
            if (buffer_size < kRecordHeaderSize)
            {
                throw std::runtime_error("buffer size must be at least one record header");
            }
            ScanBuffered(argv[2], buffer_size);
        }
        else if (command == "dump" && (argc == 5 || argc == 6 || argc == 7))
        {
            PayloadFormat const format = ParsePayloadFormat(argc >= 6 ? argv[5] : "raw");
            std::size_t const buffer_size = argc == 7 ? std::stoull(argv[6]) : 4ULL * 1024 * 1024;
            if (buffer_size == 0)
            {
                throw std::runtime_error("buffer size must be greater than zero");
            }
            DumpTopicPayloads(argv[2], argv[3], argv[4], format, buffer_size);
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
