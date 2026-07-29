#include "oos/web/device_api_service.h"

#include "oos/storage/device_storage.h"
#include "oos/web/device_api_transport.h"

#include <cerrno>
#include <cstring>
#include <vector>

namespace oos::web {
namespace {

constexpr int kReplyTimeoutMs = 30000;

std::string errorText(const char *operation, int result) {
  return std::string(operation) + ": " + std::strerror(-result);
}

void appendJsonString(std::string &output, const std::string &value) {
  static const char hex[] = "0123456789abcdef";
  output.push_back('"');
  for (const unsigned char character : value) {
    switch (character) {
    case '"':
      output += "\\\"";
      break;
    case '\\':
      output += "\\\\";
      break;
    case '\b':
      output += "\\b";
      break;
    case '\f':
      output += "\\f";
      break;
    case '\n':
      output += "\\n";
      break;
    case '\r':
      output += "\\r";
      break;
    case '\t':
      output += "\\t";
      break;
    default:
      if (character < 0x20) {
        output += "\\u00";
        output.push_back(hex[character >> 4]);
        output.push_back(hex[character & 0x0f]);
      } else {
        output.push_back(static_cast<char>(character));
      }
    }
  }
  output.push_back('"');
}

std::string
serializeEntries(const std::vector<storage::DeviceStorageEntry> &entries) {
  std::string output = "[";
  for (size_t index = 0; index < entries.size(); ++index) {
    if (index)
      output.push_back(',');
    output += "{\"path\":";
    appendJsonString(output, entries[index].path);
    output += ",\"size\":" + std::to_string(entries[index].size);
    output +=
        ",\"lastModified\":" + std::to_string(entries[index].last_modified_ms) +
        "}";
  }
  output.push_back(']');
  return output;
}

} // namespace

bool serviceDeviceApi(int socket_fd, storage::DeviceStorageService &service,
                      bool &connected, std::string &error, int timeout_ms) {
  if (!connected)
    return true;
  OosDeviceApiRequest request = {};
  const int received = oos_device_api_receive(socket_fd, &request, timeout_ms);
  if (received == -ETIMEDOUT)
    return true;
  if (received == 0) {
    connected = false;
    return true;
  }
  if (received < 0) {
    error = errorText("receive WPE device API request", received);
    return false;
  }

  const auto volume = static_cast<storage::DeviceStorageVolume>(request.volume);
  int status = 0;
  const void *payload = nullptr;
  uint32_t payload_size = 0;
  std::string serialized;
  std::vector<uint8_t> bytes;
  uint64_t space_bytes = 0;
  if (request.operation == OOS_DEVICE_API_LIST_FILES) {
    std::vector<storage::DeviceStorageEntry> entries;
    if (!service.list(volume, entries)) {
      status = -ENOENT;
    } else {
      serialized = serializeEntries(entries);
      payload = serialized.data();
      payload_size = static_cast<uint32_t>(serialized.size());
    }
  } else if (request.operation == OOS_DEVICE_API_READ_FILE) {
    if (!service.read(volume, request.path, bytes)) {
      status = -ENOENT;
    } else {
      payload = bytes.data();
      payload_size = static_cast<uint32_t>(bytes.size());
    }
  } else if (request.operation == OOS_DEVICE_API_WRITE_FILE) {
    if (!service.write(
            volume, request.path,
            static_cast<storage::DeviceStorageWriteMode>(request.flags),
            static_cast<const uint8_t *>(request.payload),
            request.payload_size))
      status = -EIO;
  } else if (request.operation == OOS_DEVICE_API_DELETE_FILE) {
    bool removed = false;
    if (!service.remove(volume, request.path, removed))
      status = -EIO;
  } else if (request.operation == OOS_DEVICE_API_FREE_SPACE ||
             request.operation == OOS_DEVICE_API_USED_SPACE) {
    const bool success = request.operation == OOS_DEVICE_API_FREE_SPACE
                             ? service.freeSpace(volume, space_bytes)
                             : service.usedSpace(volume, space_bytes);
    if (!success) {
      status = -EIO;
    } else {
      payload = &space_bytes;
      payload_size = sizeof(space_bytes);
    }
  } else {
    status = -ENOSYS;
  }
  const int replied = oos_device_api_reply(socket_fd, status, payload,
                                           payload_size, kReplyTimeoutMs);
  oos_device_api_request_clear(&request);
  if (replied != 0) {
    error = errorText("reply to WPE device API request", replied);
    return false;
  }
  return true;
}

} // namespace oos::web
