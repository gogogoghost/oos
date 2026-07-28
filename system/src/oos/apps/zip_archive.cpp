#include "oos/apps/zip_archive.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

namespace oos::apps {
namespace {

constexpr uint32_t kEndSignature = 0x06054b50;
constexpr uint32_t kCentralSignature = 0x02014b50;
constexpr uint32_t kLocalSignature = 0x04034b50;
constexpr size_t kEndRecordSize = 22;
constexpr size_t kMaximumComment = 65535;
constexpr size_t kMaximumEntries = 4096;
constexpr uint64_t kMaximumExpandedBytes = 256 * 1024 * 1024;

uint16_t little16(const uint8_t *data) {
  return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t little32(const uint8_t *data) {
  return static_cast<uint32_t>(data[0]) |
         (static_cast<uint32_t>(data[1]) << 8) |
         (static_cast<uint32_t>(data[2]) << 16) |
         (static_cast<uint32_t>(data[3]) << 24);
}

bool readExact(int fd, uint64_t offset, void *buffer, size_t size) {
  auto *bytes = static_cast<uint8_t *>(buffer);
  while (size > 0) {
    const ssize_t count = pread(fd, bytes, size, static_cast<off_t>(offset));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    bytes += count;
    offset += static_cast<uint64_t>(count);
    size -= static_cast<size_t>(count);
  }
  return true;
}

} // namespace

bool validPackagePath(const std::string &path) {
  if (path.empty() || path.front() == '/' ||
      path.find('\\') != std::string::npos ||
      path.find('\0') != std::string::npos)
    return false;
  const size_t path_size = path.back() == '/' ? path.size() - 1 : path.size();
  if (path_size == 0)
    return false;
  size_t start = 0;
  while (start < path_size) {
    const size_t found = path.find('/', start);
    const size_t end =
        found == std::string::npos ? path_size : std::min(found, path_size);
    const size_t length = end - start;
    if (length == 0 || (length == 1 && path[start] == '.') ||
        (length == 2 && path[start] == '.' && path[start + 1] == '.'))
      return false;
    start = end == path_size ? path_size : end + 1;
  }
  return true;
}

ZipArchive::~ZipArchive() { close(); }

bool ZipArchive::open(const char *path) {
  close();
  error_.clear();
  if (!path || path[0] == '\0')
    return fail("ZIP path is empty");
  fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
  if (fd_ < 0)
    return fail(std::string("open ZIP: ") + std::strerror(errno));
  struct stat status = {};
  if (fstat(fd_, &status) != 0 || status.st_size < 0) {
    close();
    return fail(std::string("stat ZIP: ") + std::strerror(errno));
  }
  file_size_ = static_cast<uint64_t>(status.st_size);
  if (file_size_ < kEndRecordSize) {
    close();
    return fail("ZIP is too small");
  }

  const size_t tail_size = static_cast<size_t>(
      std::min<uint64_t>(file_size_, kEndRecordSize + kMaximumComment));
  std::vector<uint8_t> tail(tail_size);
  const uint64_t tail_offset = file_size_ - tail_size;
  if (!readExact(fd_, tail_offset, tail.data(), tail.size())) {
    close();
    return fail("cannot read ZIP end record");
  }
  size_t end_offset = std::numeric_limits<size_t>::max();
  for (size_t position = tail.size() - kEndRecordSize + 1; position-- > 0;) {
    if (little32(tail.data() + position) == kEndSignature &&
        position + kEndRecordSize + little16(tail.data() + position + 20) ==
            tail.size()) {
      end_offset = position;
      break;
    }
  }
  if (end_offset == std::numeric_limits<size_t>::max()) {
    close();
    return fail("ZIP end record was not found");
  }
  const uint8_t *end = tail.data() + end_offset;
  if (little16(end + 4) != 0 || little16(end + 6) != 0)
    return fail("multi-disk ZIP is not supported");
  const uint16_t entries_on_disk = little16(end + 8);
  const uint16_t entry_count = little16(end + 10);
  const uint32_t central_size = little32(end + 12);
  const uint32_t central_offset = little32(end + 16);
  if (entries_on_disk != entry_count || entry_count == 0 ||
      entry_count == 0xffff || central_size == 0xffffffff ||
      central_offset == 0xffffffff)
    return fail("invalid or ZIP64 central directory");
  if (entry_count > kMaximumEntries ||
      static_cast<uint64_t>(central_offset) + central_size > file_size_)
    return fail("ZIP central directory exceeds limits");

  std::vector<uint8_t> central(central_size);
  if (!readExact(fd_, central_offset, central.data(), central.size()))
    return fail("cannot read ZIP central directory");
  size_t position = 0;
  uint64_t expanded_bytes = 0;
  entries_.reserve(entry_count);
  for (uint16_t index = 0; index < entry_count; ++index) {
    if (position + 46 > central.size() ||
        little32(central.data() + position) != kCentralSignature)
      return fail("invalid ZIP central directory entry");
    const uint8_t *header = central.data() + position;
    const uint16_t flags = little16(header + 8);
    const uint16_t method = little16(header + 10);
    const uint16_t name_length = little16(header + 28);
    const uint16_t extra_length = little16(header + 30);
    const uint16_t comment_length = little16(header + 32);
    const size_t record_size =
        46u + name_length + extra_length + comment_length;
    if (name_length == 0 || position + record_size > central.size())
      return fail("truncated ZIP central directory entry");
    if ((flags & 0x0001) != 0 || (method != 0 && method != 8))
      return fail("encrypted or unsupported ZIP entry");
    ZipEntry entry;
    entry.name.assign(reinterpret_cast<const char *>(header + 46), name_length);
    entry.compression_method = method;
    entry.crc32 = little32(header + 16);
    entry.compressed_size = little32(header + 20);
    entry.uncompressed_size = little32(header + 24);
    entry.local_header_offset = little32(header + 42);
    if (!validPackagePath(entry.name))
      return fail("unsafe ZIP entry path: " + entry.name);
    if (entry.compressed_size == 0xffffffff ||
        entry.uncompressed_size == 0xffffffff ||
        entry.local_header_offset == 0xffffffff)
      return fail("ZIP64 entries are not supported");
    if (entry.compressed_size > kMaximumExpandedBytes)
      return fail("ZIP compressed entry exceeds limit");
    expanded_bytes += entry.uncompressed_size;
    if (expanded_bytes > kMaximumExpandedBytes)
      return fail("ZIP expanded size exceeds limit");
    if (find(entry.name.c_str()))
      return fail("duplicate ZIP entry: " + entry.name);
    entries_.push_back(std::move(entry));
    position += record_size;
  }
  if (position != central.size())
    return fail("ZIP central directory has trailing data");
  return true;
}

void ZipArchive::close() {
  if (fd_ >= 0)
    ::close(fd_);
  fd_ = -1;
  file_size_ = 0;
  entries_.clear();
}

const ZipEntry *ZipArchive::find(const char *name) const {
  if (!name)
    return nullptr;
  for (const ZipEntry &entry : entries_) {
    if (entry.name == name)
      return &entry;
  }
  return nullptr;
}

bool ZipArchive::read(const char *name, std::vector<uint8_t> &output,
                      size_t maximum_bytes) const {
  error_.clear();
  const ZipEntry *entry = find(name);
  if (!entry)
    return fail(std::string("ZIP entry not found: ") + (name ? name : ""));
  return readEntry(*entry, output, maximum_bytes);
}

bool ZipArchive::readEntry(const ZipEntry &entry, std::vector<uint8_t> &output,
                           size_t maximum_bytes) const {
  if (entry.uncompressed_size > maximum_bytes)
    return fail("ZIP entry exceeds requested size limit: " + entry.name);
  uint8_t local[30] = {};
  if (static_cast<uint64_t>(entry.local_header_offset) + sizeof(local) >
          file_size_ ||
      !readExact(fd_, entry.local_header_offset, local, sizeof(local)) ||
      little32(local) != kLocalSignature)
    return fail("invalid local ZIP header: " + entry.name);
  const uint16_t name_length = little16(local + 26);
  const uint16_t extra_length = little16(local + 28);
  const uint64_t data_offset =
      static_cast<uint64_t>(entry.local_header_offset) + sizeof(local) +
      name_length + extra_length;
  if (data_offset + entry.compressed_size > file_size_)
    return fail("truncated ZIP entry data: " + entry.name);

  std::vector<uint8_t> compressed(entry.compressed_size);
  if (!compressed.empty() &&
      !readExact(fd_, data_offset, compressed.data(), compressed.size()))
    return fail("cannot read ZIP entry data: " + entry.name);
  output.assign(entry.uncompressed_size, 0);
  if (entry.compression_method == 0) {
    if (entry.compressed_size != entry.uncompressed_size)
      return fail("stored ZIP entry has mismatched sizes: " + entry.name);
    output = std::move(compressed);
  } else {
    z_stream stream = {};
    uint8_t empty_output = 0;
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = output.empty() ? &empty_output : output.data();
    stream.avail_out = output.empty() ? 1 : static_cast<uInt>(output.size());
    if (inflateInit2(&stream, -MAX_WBITS) != Z_OK)
      return fail("cannot initialize ZIP inflater");
    const int result = inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
    if (result != Z_STREAM_END || stream.total_out != output.size())
      return fail("cannot inflate ZIP entry: " + entry.name);
  }
  const uLong checksum =
      crc32(0, output.empty() ? Z_NULL : output.data(), output.size());
  if (static_cast<uint32_t>(checksum) != entry.crc32)
    return fail("ZIP entry checksum mismatch: " + entry.name);
  return true;
}

bool ZipArchive::fail(const std::string &message) const {
  error_ = message;
  return false;
}

} // namespace oos::apps
