#include <telephony/ril.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

namespace {

bool readFully(int fd, void *data, size_t size, int timeout_ms) {
  auto *bytes = static_cast<uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    pollfd descriptor{fd, POLLIN, 0};
    if (poll(&descriptor, 1, timeout_ms) <= 0)
      return false;
    const ssize_t count = read(fd, bytes + offset, size - offset);
    if (count <= 0)
      return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

bool writeFully(int fd, const void *data, size_t size) {
  const auto *bytes = static_cast<const uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    const ssize_t count = write(fd, bytes + offset, size - offset);
    if (count <= 0)
      return false;
    offset += static_cast<size_t>(count);
  }
  return true;
}

int32_t readInt(const std::vector<uint8_t> &data, size_t &offset, bool &valid) {
  if (offset + sizeof(int32_t) > data.size()) {
    valid = false;
    return 0;
  }
  int32_t value = 0;
  std::memcpy(&value, data.data() + offset, sizeof(value));
  offset += sizeof(value);
  return value;
}

std::string readString16(const std::vector<uint8_t> &data, size_t &offset,
                         bool &valid) {
  const int32_t length = readInt(data, offset, valid);
  if (!valid || length < 0)
    return {};
  const size_t byte_length = static_cast<size_t>(length) * 2;
  if (offset + byte_length + 2 > data.size()) {
    valid = false;
    return {};
  }
  std::string result;
  result.reserve(static_cast<size_t>(length));
  for (int32_t index = 0; index < length; ++index) {
    const uint16_t character = static_cast<uint16_t>(data[offset]) |
                               static_cast<uint16_t>(data[offset + 1] << 8);
    result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    offset += 2;
  }
  offset += 2;
  offset = (offset + 3) & ~size_t(3);
  if (offset > data.size())
    valid = false;
  return result;
}

bool sendRequest(int fd, int32_t request, int32_t serial) {
  const int32_t payload[] = {request, serial};
  const uint32_t length = htonl(sizeof(payload));
  return writeFully(fd, &length, sizeof(length)) &&
         writeFully(fd, payload, sizeof(payload));
}

bool receivePacket(int fd, std::vector<uint8_t> &packet, int timeout_ms) {
  uint32_t network_length = 0;
  if (!readFully(fd, &network_length, sizeof(network_length), timeout_ms))
    return false;
  const uint32_t length = ntohl(network_length);
  if (length == 0 || length > 8192)
    return false;
  packet.resize(length);
  return readFully(fd, packet.data(), packet.size(), timeout_ms);
}

std::string masked(const std::string &value) {
  if (value.size() <= 4)
    return std::string(value.size(), '*');
  return std::string(value.size() - 4, '*') + value.substr(value.size() - 4);
}

bool waitResponse(int fd, int32_t serial, int request) {
  for (int attempt = 0; attempt < 20; ++attempt) {
    std::vector<uint8_t> packet;
    if (!receivePacket(fd, packet, 1000))
      return false;
    size_t offset = 0;
    bool valid = true;
    const int32_t type = readInt(packet, offset, valid);
    if (!valid)
      return false;
    if (type == 1) {
      const int32_t event = readInt(packet, offset, valid);
      if (event == RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED)
        std::printf("radio_state=%d\n", readInt(packet, offset, valid));
      else if (event == RIL_UNSOL_RIL_CONNECTED)
        std::printf("ril_version=%d\n", readInt(packet, offset, valid));
      continue;
    }
    if (type != 0)
      continue;
    const int32_t response_serial = readInt(packet, offset, valid);
    const int32_t error = readInt(packet, offset, valid);
    if (!valid || response_serial != serial)
      continue;
    std::printf("request.%d.error=%d\n", request, error);
    if (error != RIL_E_SUCCESS)
      return true;
    if (request == RIL_REQUEST_BASEBAND_VERSION) {
      std::printf("baseband=%s\n", readString16(packet, offset, valid).c_str());
    } else if (request == RIL_REQUEST_DEVICE_IDENTITY) {
      const int count = readInt(packet, offset, valid);
      std::printf("identity_count=%d", count);
      for (int index = 0; index < count && valid; ++index)
        std::printf(" id%d=%s", index,
                    masked(readString16(packet, offset, valid)).c_str());
      std::printf("\n");
    } else if (request == RIL_REQUEST_GET_SIM_STATUS) {
      const int card = readInt(packet, offset, valid);
      const int pin = readInt(packet, offset, valid);
      readInt(packet, offset, valid);
      readInt(packet, offset, valid);
      readInt(packet, offset, valid);
      const int apps = readInt(packet, offset, valid);
      std::printf("sim_card_state=%d sim_pin_state=%d sim_apps=%d\n", card, pin,
                  apps);
    } else if (request == RIL_REQUEST_SIGNAL_STRENGTH) {
      int values[13]{};
      for (int &value : values)
        value = readInt(packet, offset, valid);
      std::printf("signal_gsm=%d ber=%d cdma_dbm=%d evdo_dbm=%d "
                  "lte=%d rsrp=%d rsrq=%d rssnr=%d cqi=%d tdscdma=%d\n",
                  values[0], values[1], values[2], values[4], values[7],
                  values[8], values[9], values[10], values[11], values[12]);
    }
    return valid;
  }
  return false;
}

} // namespace

int main() {
  if (getuid() != 0) {
    std::fprintf(stderr, "8110 vendor rild requires uid 0\n");
    return 1;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0)
    return 1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strcpy(address.sun_path, "/dev/socket/rild");
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    std::fprintf(stderr, "connect rild failed: %s\n", std::strerror(errno));
    close(fd);
    return 1;
  }
  const int requests[] = {
      RIL_REQUEST_GET_SIM_STATUS, RIL_REQUEST_BASEBAND_VERSION,
      RIL_REQUEST_DEVICE_IDENTITY, RIL_REQUEST_SIGNAL_STRENGTH};
  int32_t serial = 1;
  for (int request : requests) {
    if (!sendRequest(fd, request, serial) ||
        !waitResponse(fd, serial, request)) {
      std::fprintf(stderr, "RIL request %d failed or timed out\n", request);
      close(fd);
      return 1;
    }
    ++serial;
  }
  close(fd);
  std::printf("ril_probe=ok uid=%u\n", static_cast<unsigned>(getuid()));
  return 0;
}
