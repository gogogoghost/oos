#include "oos/modem/modem_manager.h"

#include <telephony/ril.h>

#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace oos::modem {
namespace {

struct Response {
  int error = -1;
  std::vector<uint8_t> payload;
};

class Reader {
public:
  explicit Reader(const std::vector<uint8_t> &data) : data_(data) {}

  int32_t integer() {
    if (offset_ + sizeof(int32_t) > data_.size()) {
      valid_ = false;
      return 0;
    }
    int32_t value = 0;
    std::memcpy(&value, data_.data() + offset_, sizeof(value));
    offset_ += sizeof(value);
    return value;
  }

  std::string string16() {
    const int32_t length = integer();
    if (!valid_ || length < 0)
      return {};
    const size_t byte_length = static_cast<size_t>(length) * 2;
    if (offset_ + byte_length + 2 > data_.size()) {
      valid_ = false;
      return {};
    }
    std::string result;
    result.reserve(static_cast<size_t>(length));
    for (int32_t index = 0; index < length; ++index) {
      const uint16_t character = static_cast<uint16_t>(data_[offset_]) |
                                 static_cast<uint16_t>(data_[offset_ + 1] << 8);
      result.push_back(character < 0x80 ? static_cast<char>(character) : '?');
      offset_ += 2;
    }
    offset_ = (offset_ + 2 + 3) & ~size_t(3);
    if (offset_ > data_.size())
      valid_ = false;
    return result;
  }

  std::vector<std::string> strings() {
    const int32_t count = integer();
    std::vector<std::string> result;
    if (count < 0 || count > 64) {
      valid_ = false;
      return result;
    }
    result.reserve(static_cast<size_t>(count));
    for (int32_t index = 0; index < count && valid_; ++index)
      result.push_back(string16());
    return result;
  }

  bool valid() const { return valid_; }

private:
  const std::vector<uint8_t> &data_;
  size_t offset_ = 0;
  bool valid_ = true;
};

bool readFully(int fd, void *data, size_t size, int timeout_ms) {
  auto *bytes = static_cast<uint8_t *>(data);
  size_t offset = 0;
  while (offset < size) {
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = poll(&descriptor, 1, timeout_ms);
    if (ready <= 0) {
      if (ready == 0)
        errno = ETIMEDOUT;
      return false;
    }
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

bool sendParcel(int fd, const std::vector<int32_t> &parcel) {
  const uint32_t length =
      htonl(static_cast<uint32_t>(parcel.size() * sizeof(int32_t)));
  return writeFully(fd, &length, sizeof(length)) &&
         writeFully(fd, parcel.data(), parcel.size() * sizeof(int32_t));
}

bool receiveParcel(int fd, std::vector<uint8_t> &packet, int timeout_ms) {
  uint32_t network_length = 0;
  if (!readFully(fd, &network_length, sizeof(network_length), timeout_ms))
    return false;
  const uint32_t length = ntohl(network_length);
  if (length == 0 || length > 8192) {
    errno = EPROTO;
    return false;
  }
  packet.resize(length);
  return readFully(fd, packet.data(), packet.size(), timeout_ms);
}

int integerString(const std::vector<std::string> &values, size_t index,
                  int fallback = -1) {
  if (index >= values.size() || values[index].empty())
    return fallback;
  char *end = nullptr;
  const long value = std::strtol(values[index].c_str(), &end, 10);
  return end != values[index].c_str() && *end == '\0' ? static_cast<int>(value)
                                                      : fallback;
}

} // namespace

struct ModemManager::Implementation {
  bool request(int code, const std::vector<int32_t> &arguments,
               Response &response, int timeout_ms) {
    const int32_t request_serial = serial++;
    std::vector<int32_t> parcel{code, request_serial};
    parcel.insert(parcel.end(), arguments.begin(), arguments.end());
    if (!sendParcel(fd, parcel)) {
      error = "write RIL request failed: " + std::string(std::strerror(errno));
      return false;
    }
    for (;;) {
      std::vector<uint8_t> packet;
      if (!receiveParcel(fd, packet, timeout_ms)) {
        error = errno == ETIMEDOUT ? "RIL request timed out"
                                   : "read RIL response failed: " +
                                         std::string(std::strerror(errno));
        return false;
      }
      Reader reader(packet);
      const int type = reader.integer();
      if (type == 1) {
        const int event = reader.integer();
        if (event == RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED)
          radio_state = reader.integer();
        continue;
      }
      if (type != 0)
        continue;
      const int32_t response_serial = reader.integer();
      response.error = reader.integer();
      if (!reader.valid()) {
        error = "malformed RIL response header";
        return false;
      }
      if (response_serial != request_serial)
        continue;
      response.payload.assign(packet.begin() + 12, packet.end());
      return true;
    }
  }

  std::string error;
  std::string service_name;
  int fd = -1;
  int32_t serial = 1;
  int radio_state = -1;
};

ModemManager::ModemManager()
    : implementation_(std::make_unique<Implementation>()) {}

ModemManager::~ModemManager() { shutdown(); }

bool ModemManager::initialize(const std::string &service_name) {
  shutdown();
  if (getuid() != 0) {
    implementation_->error = "8110 vendor rild requires uid 0";
    return false;
  }
  const char *socket_path = nullptr;
  if (service_name == "slot1" || service_name == "rild")
    socket_path = "/dev/socket/rild";
  else if (service_name == "slot2" || service_name == "rild2")
    socket_path = "/dev/socket/rild2";
  else {
    implementation_->error = "unknown RIL service: " + service_name;
    return false;
  }
  const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    implementation_->error =
        "create RIL socket failed: " + std::string(std::strerror(errno));
    return false;
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) !=
      0) {
    implementation_->error =
        "connect RIL socket failed: " + std::string(std::strerror(errno));
    close(fd);
    return false;
  }
  implementation_->fd = fd;
  implementation_->service_name = service_name;
  implementation_->error.clear();
  return true;
}

void ModemManager::shutdown() {
  if (!implementation_)
    return;
  if (implementation_->fd >= 0)
    close(implementation_->fd);
  implementation_->fd = -1;
  implementation_->service_name.clear();
  implementation_->serial = 1;
  implementation_->radio_state = -1;
}

bool ModemManager::initialized() const {
  return implementation_ && implementation_->fd >= 0;
}

bool ModemManager::querySnapshot(ModemSnapshot &snapshot, int timeout_ms) {
  snapshot = {};
  snapshot.service_connected = initialized();
  if (!initialized()) {
    implementation_->error = "RIL socket is not initialized";
    return false;
  }
  auto run = [&](const char *name, int code, auto apply) {
    Response response;
    ModemRequestStatus status;
    status.operation = name;
    if (!implementation_->request(code, {}, response, timeout_ms)) {
      status.error = -1;
      status.timed_out = implementation_->error == "RIL request timed out";
      snapshot.requests.push_back(status);
      return false;
    }
    status.error = response.error;
    snapshot.requests.push_back(status);
    if (response.error == RIL_E_SUCCESS)
      apply(response.payload);
    return true;
  };

  if (!run("getIccCardStatus", RIL_REQUEST_GET_SIM_STATUS,
           [&](const auto &payload) {
             Reader reader(payload);
             snapshot.sim.card_state = reader.integer();
             snapshot.sim.universal_pin_state = reader.integer();
             reader.integer();
             reader.integer();
             reader.integer();
             snapshot.sim.application_count = reader.integer();
           }) ||
      !run("getBasebandVersion", RIL_REQUEST_BASEBAND_VERSION,
           [&](const auto &payload) {
             Reader reader(payload);
             snapshot.baseband_version = reader.string16();
           }) ||
      !run("getDeviceIdentity", RIL_REQUEST_DEVICE_IDENTITY,
           [&](const auto &payload) {
             Reader reader(payload);
             const auto values = reader.strings();
             if (!values.empty())
               snapshot.identity.imei = values[0];
             if (values.size() > 1)
               snapshot.identity.imei_software_version = values[1];
             if (values.size() > 2)
               snapshot.identity.esn = values[2];
             if (values.size() > 3)
               snapshot.identity.meid = values[3];
           }) ||
      !run("getSignalStrength", RIL_REQUEST_SIGNAL_STRENGTH,
           [&](const auto &payload) {
             Reader reader(payload);
             snapshot.signal.gsm_strength = reader.integer();
             snapshot.signal.gsm_bit_error_rate = reader.integer();
             snapshot.signal.cdma_dbm = reader.integer();
             snapshot.signal.cdma_ecio = reader.integer();
             snapshot.signal.evdo_dbm = reader.integer();
             snapshot.signal.evdo_ecio = reader.integer();
             snapshot.signal.evdo_snr = reader.integer();
             snapshot.signal.lte_strength = reader.integer();
             snapshot.signal.lte_rsrp = reader.integer();
             snapshot.signal.lte_rsrq = reader.integer();
             snapshot.signal.lte_rssnr = reader.integer();
             snapshot.signal.lte_cqi = reader.integer();
             snapshot.signal.tdscdma_rscp = reader.integer();
           }) ||
      !run("getVoiceRegistrationState", RIL_REQUEST_VOICE_REGISTRATION_STATE,
           [&](const auto &payload) {
             Reader reader(payload);
             const auto values = reader.strings();
             snapshot.voice_registration.state = integerString(values, 0);
             snapshot.voice_registration.radio_technology =
                 integerString(values, 3);
             snapshot.voice_registration.denial_reason =
                 integerString(values, 13, 0);
           }) ||
      !run("getDataRegistrationState", RIL_REQUEST_DATA_REGISTRATION_STATE,
           [&](const auto &payload) {
             Reader reader(payload);
             const auto values = reader.strings();
             snapshot.data_registration.state = integerString(values, 0);
             snapshot.data_registration.radio_technology =
                 integerString(values, 3);
             snapshot.data_registration.denial_reason =
                 integerString(values, 4, 0);
             snapshot.data_registration.max_data_calls =
                 integerString(values, 5, 0);
           }) ||
      !run("getOperator", RIL_REQUEST_OPERATOR,
           [&](const auto &payload) {
             Reader reader(payload);
             const auto values = reader.strings();
             if (!values.empty())
               snapshot.network_operator.long_name = values[0];
             if (values.size() > 1)
               snapshot.network_operator.short_name = values[1];
             if (values.size() > 2)
               snapshot.network_operator.numeric = values[2];
           }) ||
      !run("getPreferredNetworkType", RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
           [&](const auto &payload) {
             Reader reader(payload);
             if (reader.integer() > 0)
               snapshot.preferred_network_type = reader.integer();
           }) ||
      !run("getVoiceRadioTechnology", RIL_REQUEST_VOICE_RADIO_TECH,
           [&](const auto &payload) {
             Reader reader(payload);
             if (reader.integer() > 0)
               snapshot.voice_radio_technology = reader.integer();
           }) ||
      !run("getCurrentCalls", RIL_REQUEST_GET_CURRENT_CALLS,
           [&](const auto &payload) {
             Reader reader(payload);
             snapshot.current_call_count = reader.integer();
           }) ||
      !run("getDataCallList", RIL_REQUEST_DATA_CALL_LIST,
           [&](const auto &payload) {
             Reader reader(payload);
             reader.integer();
             snapshot.data_call_count = reader.integer();
           }) ||
      !run("getHardwareConfig", RIL_REQUEST_GET_HARDWARE_CONFIG,
           [&](const auto &payload) {
             Reader reader(payload);
             snapshot.hardware_config_count = reader.integer();
           }) ||
      !run("getRadioCapability", RIL_REQUEST_GET_RADIO_CAPABILITY,
           [&](const auto &payload) {
             Reader reader(payload);
             reader.integer();
             reader.integer();
             reader.integer();
             snapshot.radio_access_family =
                 static_cast<uint32_t>(reader.integer());
             snapshot.logical_modem_uuid = reader.string16();
           }))
    return false;
  snapshot.radio_state = implementation_->radio_state;
  implementation_->error.clear();
  return true;
}

bool ModemManager::queryNetworkStatus(NetworkStatus &status, int timeout_ms) {
  status = {};
  status.service_connected = initialized();
  if (!initialized()) {
    implementation_->error = "RIL socket is not initialized";
    return false;
  }
  auto run = [&](int code, auto apply) {
    Response response;
    if (!implementation_->request(code, {}, response, timeout_ms))
      return false;
    if (response.error == RIL_E_SUCCESS)
      apply(response.payload);
    return true;
  };

  if (!run(RIL_REQUEST_SIGNAL_STRENGTH,
           [&](const auto &payload) {
             Reader reader(payload);
             status.signal.gsm_strength = reader.integer();
             status.signal.gsm_bit_error_rate = reader.integer();
             status.signal.cdma_dbm = reader.integer();
             status.signal.cdma_ecio = reader.integer();
             status.signal.evdo_dbm = reader.integer();
             status.signal.evdo_ecio = reader.integer();
             status.signal.evdo_snr = reader.integer();
             status.signal.lte_strength = reader.integer();
             status.signal.lte_rsrp = reader.integer();
             status.signal.lte_rsrq = reader.integer();
             status.signal.lte_rssnr = reader.integer();
             status.signal.lte_cqi = reader.integer();
             status.signal.tdscdma_rscp = reader.integer();
           }) ||
      !run(RIL_REQUEST_VOICE_REGISTRATION_STATE,
           [&](const auto &payload) {
             Reader reader(payload);
             const auto values = reader.strings();
             status.voice_registration.state = integerString(values, 0);
             status.voice_registration.radio_technology =
                 integerString(values, 3);
             status.voice_registration.denial_reason =
                 integerString(values, 13, 0);
           }) ||
      !run(RIL_REQUEST_DATA_REGISTRATION_STATE, [&](const auto &payload) {
        Reader reader(payload);
        const auto values = reader.strings();
        status.data_registration.state = integerString(values, 0);
        status.data_registration.radio_technology = integerString(values, 3);
        status.data_registration.denial_reason = integerString(values, 4, 0);
        status.data_registration.max_data_calls = integerString(values, 5, 0);
      }))
    return false;

  status.radio_state = implementation_->radio_state;
  implementation_->error.clear();
  return true;
}

bool ModemManager::setRadioPower(bool enabled, ModemRequestStatus &status,
                                 int timeout_ms) {
  status = {};
  status.operation = "setRadioPower";
  if (!initialized()) {
    implementation_->error = "RIL socket is not initialized";
    return false;
  }
  Response response;
  if (!implementation_->request(RIL_REQUEST_RADIO_POWER, {1, enabled ? 1 : 0},
                                response, timeout_ms)) {
    status.error = -1;
    status.timed_out = implementation_->error == "RIL request timed out";
    return false;
  }
  status.error = response.error;
  implementation_->error.clear();
  return true;
}

const std::string &ModemManager::lastError() const {
  return implementation_->error;
}

const char *cardStateName(int state) {
  switch (state) {
  case 0:
    return "absent";
  case 1:
    return "present";
  case 2:
    return "error";
  case 3:
    return "restricted";
  default:
    return "unknown";
  }
}

const char *radioStateName(int state) {
  return state == 0    ? "off"
         : state == 1  ? "unavailable"
         : state == 10 ? "on"
                       : "unknown";
}

const char *registrationStateName(int state) {
  switch (state) {
  case 0:
    return "not-registered";
  case 1:
    return "home";
  case 2:
    return "searching";
  case 3:
    return "denied";
  case 5:
    return "roaming";
  default:
    return "unknown";
  }
}

const char *radioTechnologyName(int technology) {
  static constexpr const char *kNames[] = {
      "unknown", "gprs",  "edge",  "umts",     "is95a", "is95b", "1xrtt",
      "evdo0",   "evdoa", "hsdpa", "hsupa",    "hspa",  "evdob", "ehrpd",
      "lte",     "hspap", "gsm",   "td-scdma", "iwlan", "lte-ca"};
  return technology >= 0 &&
                 technology < static_cast<int>(sizeof(kNames) / sizeof(*kNames))
             ? kNames[technology]
             : "unknown";
}

const char *radioErrorName(int error) {
  switch (error) {
  case 0:
    return "none";
  case 1:
    return "radio-not-available";
  case 2:
    return "generic-failure";
  case 6:
    return "request-not-supported";
  case 11:
    return "sim-absent";
  case -1:
    return "transport-error";
  default:
    return "ril-error";
  }
}

} // namespace oos::modem
