#include "oos/modem/modem_manager.h"

#include "RadioIndication.h"
#include "RadioResponse.h"

#include <android/hardware/radio/1.0/IRadio.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <mutex>
#include <utility>

namespace android::hardware {
void configureRpcThreadpool(size_t max_threads, bool caller_will_join);
} // namespace android::hardware

namespace oos::modem {
namespace {

namespace radio = ::android::hardware::radio::V1_0;
using ::android::sp;
using ::android::hardware::hidl_string;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;
using ::android::hardware::Void;
using DefaultIndication = radio::implementation::RadioIndication;
using DefaultResponse = radio::implementation::RadioResponse;

struct ResponseRecord {
  radio::RadioResponseInfo info{};
  radio::CardStatus card_status{};
  radio::SignalStrength signal_strength{};
  radio::VoiceRegStateResult voice_registration{};
  radio::DataRegStateResult data_registration{};
  radio::RadioCapability radio_capability{};
  std::string text1;
  std::string text2;
  std::string text3;
  std::string text4;
  int value1 = 0;
  int count = 0;
};

class ResponseStore {
public:
  void complete(ResponseRecord record) {
    const bool needs_acknowledgement =
        record.info.type == radio::RadioResponseType::SOLICITED_ACK_EXP;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      records_[record.info.serial] = std::move(record);
    }
    condition_.notify_all();
    if (needs_acknowledgement)
      acknowledge();
  }

  bool wait(int32_t serial, ResponseRecord &record, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!condition_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] {
          return records_.find(serial) != records_.end();
        }))
      return false;
    auto entry = records_.find(serial);
    record = std::move(entry->second);
    records_.erase(entry);
    return true;
  }

  void setAcknowledger(std::function<void()> acknowledger) {
    std::lock_guard<std::mutex> lock(mutex_);
    acknowledge_ = std::move(acknowledger);
  }

  void acknowledge() {
    std::function<void()> acknowledger;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      acknowledger = acknowledge_;
    }
    if (acknowledger)
      acknowledger();
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    records_.clear();
    acknowledge_ = {};
    radio_state_.store(-1, std::memory_order_relaxed);
  }

  void setRadioState(int state) {
    radio_state_.store(state, std::memory_order_relaxed);
  }

  int radioState() const {
    return radio_state_.load(std::memory_order_relaxed);
  }

private:
  std::mutex mutex_;
  std::condition_variable condition_;
  std::map<int32_t, ResponseRecord> records_;
  std::function<void()> acknowledge_;
  std::atomic<int> radio_state_{-1};
};

class ModemResponse final : public DefaultResponse {
public:
  explicit ModemResponse(std::shared_ptr<ResponseStore> store)
      : store_(std::move(store)) {}

  Return<void>
  getIccCardStatusResponse(const radio::RadioResponseInfo &info,
                           const radio::CardStatus &status) override {
    ResponseRecord record;
    record.info = info;
    record.card_status = status;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void>
  getSignalStrengthResponse(const radio::RadioResponseInfo &info,
                            const radio::SignalStrength &strength) override {
    ResponseRecord record;
    record.info = info;
    record.signal_strength = strength;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getVoiceRegistrationStateResponse(
      const radio::RadioResponseInfo &info,
      const radio::VoiceRegStateResult &registration) override {
    ResponseRecord record;
    record.info = info;
    record.voice_registration = registration;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getDataRegistrationStateResponse(
      const radio::RadioResponseInfo &info,
      const radio::DataRegStateResult &registration) override {
    ResponseRecord record;
    record.info = info;
    record.data_registration = registration;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getOperatorResponse(const radio::RadioResponseInfo &info,
                                   const hidl_string &long_name,
                                   const hidl_string &short_name,
                                   const hidl_string &numeric) override {
    ResponseRecord record;
    record.info = info;
    record.text1 = long_name.c_str();
    record.text2 = short_name.c_str();
    record.text3 = numeric.c_str();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getBasebandVersionResponse(const radio::RadioResponseInfo &info,
                                          const hidl_string &version) override {
    ResponseRecord record;
    record.info = info;
    record.text1 = version.c_str();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getDeviceIdentityResponse(const radio::RadioResponseInfo &info,
                                         const hidl_string &imei,
                                         const hidl_string &imeisv,
                                         const hidl_string &esn,
                                         const hidl_string &meid) override {
    ResponseRecord record;
    record.info = info;
    record.text1 = imei.c_str();
    record.text2 = imeisv.c_str();
    record.text3 = esn.c_str();
    record.text4 = meid.c_str();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void>
  getPreferredNetworkTypeResponse(const radio::RadioResponseInfo &info,
                                  radio::PreferredNetworkType type) override {
    ResponseRecord record;
    record.info = info;
    record.value1 = static_cast<int>(type);
    store_->complete(std::move(record));
    return Void();
  }

  Return<void>
  getVoiceRadioTechnologyResponse(const radio::RadioResponseInfo &info,
                                  radio::RadioTechnology technology) override {
    ResponseRecord record;
    record.info = info;
    record.value1 = static_cast<int>(technology);
    store_->complete(std::move(record));
    return Void();
  }

  Return<void>
  getCurrentCallsResponse(const radio::RadioResponseInfo &info,
                          const hidl_vec<radio::Call> &calls) override {
    ResponseRecord record;
    record.info = info;
    record.count = calls.size();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getDataCallListResponse(
      const radio::RadioResponseInfo &info,
      const hidl_vec<radio::SetupDataCallResult> &calls) override {
    ResponseRecord record;
    record.info = info;
    record.count = calls.size();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getHardwareConfigResponse(
      const radio::RadioResponseInfo &info,
      const hidl_vec<radio::HardwareConfig> &configurations) override {
    ResponseRecord record;
    record.info = info;
    record.count = configurations.size();
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> getRadioCapabilityResponse(
      const radio::RadioResponseInfo &info,
      const radio::RadioCapability &capability) override {
    ResponseRecord record;
    record.info = info;
    record.radio_capability = capability;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void>
  setRadioPowerResponse(const radio::RadioResponseInfo &info) override {
    ResponseRecord record;
    record.info = info;
    store_->complete(std::move(record));
    return Void();
  }

  Return<void> acknowledgeRequest(int32_t) override { return Void(); }

private:
  std::shared_ptr<ResponseStore> store_;
};

class ModemIndication final : public DefaultIndication {
public:
  explicit ModemIndication(std::shared_ptr<ResponseStore> store)
      : store_(std::move(store)) {}

  Return<void> radioStateChanged(radio::RadioIndicationType type,
                                 radio::RadioState state) override {
    acknowledge(type);
    store_->setRadioState(static_cast<int>(state));
    return Void();
  }

  Return<void> currentSignalStrength(radio::RadioIndicationType type,
                                     const radio::SignalStrength &) override {
    acknowledge(type);
    return Void();
  }

  Return<void> networkStateChanged(radio::RadioIndicationType type) override {
    acknowledge(type);
    return Void();
  }

  Return<void> simStatusChanged(radio::RadioIndicationType type) override {
    acknowledge(type);
    return Void();
  }

  Return<void> rilConnected(radio::RadioIndicationType type) override {
    acknowledge(type);
    return Void();
  }

  Return<void> modemReset(radio::RadioIndicationType type,
                          const hidl_string &) override {
    acknowledge(type);
    store_->setRadioState(static_cast<int>(radio::RadioState::UNAVAILABLE));
    return Void();
  }

private:
  void acknowledge(radio::RadioIndicationType type) {
    if (type == radio::RadioIndicationType::UNSOLICITED_ACK_EXP)
      store_->acknowledge();
  }

private:
  std::shared_ptr<ResponseStore> store_;
};

ModemRequestStatus makeStatus(const char *operation,
                              const ResponseRecord *record) {
  ModemRequestStatus status;
  status.operation = operation;
  if (!record) {
    status.timed_out = true;
    return status;
  }
  status.error = static_cast<int>(record->info.error);
  return status;
}

} // namespace

struct ModemManager::Implementation {
  template <typename Sender>
  bool request(const char *operation, Sender sender, ResponseRecord &record,
               ModemRequestStatus &status, int timeout_ms) {
    const int32_t serial = next_serial.fetch_add(1);
    const Return<void> result = sender(serial);
    if (!result.isOk()) {
      error = std::string(operation) +
              " transport failed: " + result.description().c_str();
      status.operation = operation;
      status.error = -1;
      return false;
    }
    if (!store->wait(serial, record, timeout_ms)) {
      status = makeStatus(operation, nullptr);
      return true;
    }
    status = makeStatus(operation, &record);
    return true;
  }

  std::string error;
  std::string service_name;
  sp<radio::IRadio> radio_service;
  sp<ModemResponse> response;
  sp<ModemIndication> indication;
  std::shared_ptr<ResponseStore> store = std::make_shared<ResponseStore>();
  std::atomic<int32_t> next_serial{1};
};

ModemManager::ModemManager()
    : implementation_(std::make_unique<Implementation>()) {}

ModemManager::~ModemManager() { shutdown(); }

bool ModemManager::initialize(const std::string &service_name) {
  shutdown();
  static std::once_flag rpc_threadpool_once;
  std::call_once(rpc_threadpool_once,
                 [] { ::android::hardware::configureRpcThreadpool(1, false); });
  implementation_->radio_service = radio::IRadio::getService(service_name);
  if (!implementation_->radio_service) {
    implementation_->error = "Radio HAL service not found: " + service_name;
    return false;
  }
  implementation_->service_name = service_name;
  implementation_->response = new ModemResponse(implementation_->store);
  implementation_->indication = new ModemIndication(implementation_->store);
  const auto acknowledger = [radio_service = implementation_->radio_service] {
    radio_service->responseAcknowledgement();
  };
  implementation_->store->setAcknowledger(acknowledger);
  const Return<void> result =
      implementation_->radio_service->setResponseFunctions(
          implementation_->response, implementation_->indication);
  if (!result.isOk()) {
    implementation_->error = std::string("setResponseFunctions failed: ") +
                             result.description().c_str();
    shutdown();
    return false;
  }
  implementation_->error.clear();
  return true;
}

void ModemManager::shutdown() {
  if (!implementation_)
    return;
  implementation_->store->reset();
  implementation_->indication.clear();
  implementation_->response.clear();
  implementation_->radio_service.clear();
  implementation_->service_name.clear();
}

bool ModemManager::initialized() const {
  return implementation_->radio_service != nullptr;
}

bool ModemManager::querySnapshot(ModemSnapshot &snapshot, int timeout_ms) {
  snapshot = {};
  snapshot.service_connected = initialized();
  if (!initialized()) {
    implementation_->error = "Radio HAL is not initialized";
    return false;
  }
  snapshot.radio_state = implementation_->store->radioState();
  auto run = [&](const char *name, auto sender, auto apply) {
    ResponseRecord record;
    ModemRequestStatus status;
    if (!implementation_->request(name, sender, record, status, timeout_ms))
      return false;
    snapshot.requests.push_back(status);
    if (!status.timed_out && status.error == 0)
      apply(record);
    return true;
  };

  if (!run(
          "getIccCardStatus",
          [&](int32_t serial) {
            return implementation_->radio_service->getIccCardStatus(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.sim.card_state =
                static_cast<int>(record.card_status.cardState);
            snapshot.sim.universal_pin_state =
                static_cast<int>(record.card_status.universalPinState);
            snapshot.sim.application_count =
                record.card_status.applications.size();
          }) ||
      !run(
          "getBasebandVersion",
          [&](int32_t serial) {
            return implementation_->radio_service->getBasebandVersion(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.baseband_version = record.text1;
          }) ||
      !run(
          "getDeviceIdentity",
          [&](int32_t serial) {
            return implementation_->radio_service->getDeviceIdentity(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.identity = {record.text1, record.text2, record.text3,
                                 record.text4};
          }) ||
      !run(
          "getSignalStrength",
          [&](int32_t serial) {
            return implementation_->radio_service->getSignalStrength(serial);
          },
          [&](const ResponseRecord &record) {
            const auto &value = record.signal_strength;
            snapshot.signal = {
                static_cast<int>(value.gw.signalStrength),
                static_cast<int>(value.gw.bitErrorRate),
                static_cast<int>(value.cdma.dbm),
                static_cast<int>(value.cdma.ecio),
                static_cast<int>(value.evdo.dbm),
                static_cast<int>(value.evdo.ecio),
                static_cast<int>(value.evdo.signalNoiseRatio),
                static_cast<int>(value.lte.signalStrength),
                static_cast<int>(value.lte.rsrp),
                static_cast<int>(value.lte.rsrq),
                value.lte.rssnr,
                static_cast<int>(value.lte.cqi),
                static_cast<int>(value.lte.timingAdvance),
                static_cast<int>(value.tdScdma.rscp),
            };
          }) ||
      !run(
          "getVoiceRegistrationState",
          [&](int32_t serial) {
            return implementation_->radio_service->getVoiceRegistrationState(
                serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.voice_registration = {
                static_cast<int>(record.voice_registration.regState),
                record.voice_registration.rat,
                record.voice_registration.reasonForDenial, 0};
          }) ||
      !run(
          "getDataRegistrationState",
          [&](int32_t serial) {
            return implementation_->radio_service->getDataRegistrationState(
                serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.data_registration = {
                static_cast<int>(record.data_registration.regState),
                record.data_registration.rat,
                record.data_registration.reasonDataDenied,
                record.data_registration.maxDataCalls};
          }) ||
      !run(
          "getOperator",
          [&](int32_t serial) {
            return implementation_->radio_service->getOperator(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.network_operator = {record.text1, record.text2,
                                         record.text3};
          }) ||
      !run(
          "getPreferredNetworkType",
          [&](int32_t serial) {
            return implementation_->radio_service->getPreferredNetworkType(
                serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.preferred_network_type = record.value1;
          }) ||
      !run(
          "getVoiceRadioTechnology",
          [&](int32_t serial) {
            return implementation_->radio_service->getVoiceRadioTechnology(
                serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.voice_radio_technology = record.value1;
          }) ||
      !run(
          "getCurrentCalls",
          [&](int32_t serial) {
            return implementation_->radio_service->getCurrentCalls(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.current_call_count = record.count;
          }) ||
      !run(
          "getDataCallList",
          [&](int32_t serial) {
            return implementation_->radio_service->getDataCallList(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.data_call_count = record.count;
          }) ||
      !run(
          "getHardwareConfig",
          [&](int32_t serial) {
            return implementation_->radio_service->getHardwareConfig(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.hardware_config_count = record.count;
          }) ||
      !run(
          "getRadioCapability",
          [&](int32_t serial) {
            return implementation_->radio_service->getRadioCapability(serial);
          },
          [&](const ResponseRecord &record) {
            snapshot.radio_access_family = record.radio_capability.raf;
            snapshot.logical_modem_uuid =
                record.radio_capability.logicalModemUuid.c_str();
          }))
    return false;

  snapshot.radio_state = implementation_->store->radioState();
  implementation_->error.clear();
  return true;
}

bool ModemManager::queryNetworkStatus(NetworkStatus &status, int timeout_ms) {
  status = {};
  status.service_connected = initialized();
  if (!initialized()) {
    implementation_->error = "Radio HAL is not initialized";
    return false;
  }
  auto run = [&](const char *name, auto sender, auto apply) {
    ResponseRecord record;
    ModemRequestStatus request_status;
    if (!implementation_->request(name, sender, record, request_status,
                                  timeout_ms))
      return false;
    if (!request_status.timed_out && request_status.error == 0)
      apply(record);
    return true;
  };

  if (!run(
          "getSignalStrength",
          [&](int32_t serial) {
            return implementation_->radio_service->getSignalStrength(serial);
          },
          [&](const ResponseRecord &record) {
            const auto &value = record.signal_strength;
            status.signal = {
                static_cast<int>(value.gw.signalStrength),
                static_cast<int>(value.gw.bitErrorRate),
                static_cast<int>(value.cdma.dbm),
                static_cast<int>(value.cdma.ecio),
                static_cast<int>(value.evdo.dbm),
                static_cast<int>(value.evdo.ecio),
                static_cast<int>(value.evdo.signalNoiseRatio),
                static_cast<int>(value.lte.signalStrength),
                static_cast<int>(value.lte.rsrp),
                static_cast<int>(value.lte.rsrq),
                value.lte.rssnr,
                static_cast<int>(value.lte.cqi),
                static_cast<int>(value.lte.timingAdvance),
                static_cast<int>(value.tdScdma.rscp),
            };
          }) ||
      !run(
          "getVoiceRegistrationState",
          [&](int32_t serial) {
            return implementation_->radio_service->getVoiceRegistrationState(
                serial);
          },
          [&](const ResponseRecord &record) {
            status.voice_registration = {
                static_cast<int>(record.voice_registration.regState),
                record.voice_registration.rat,
                record.voice_registration.reasonForDenial, 0};
          }) ||
      !run(
          "getDataRegistrationState",
          [&](int32_t serial) {
            return implementation_->radio_service->getDataRegistrationState(
                serial);
          },
          [&](const ResponseRecord &record) {
            status.data_registration = {
                static_cast<int>(record.data_registration.regState),
                record.data_registration.rat,
                record.data_registration.reasonDataDenied,
                record.data_registration.maxDataCalls};
          }))
    return false;

  status.radio_state = implementation_->store->radioState();
  implementation_->error.clear();
  return true;
}

bool ModemManager::setRadioPower(bool enabled, ModemRequestStatus &status,
                                 int timeout_ms) {
  if (!initialized()) {
    implementation_->error = "Radio HAL is not initialized";
    return false;
  }
  ResponseRecord record;
  return implementation_->request(
      "setRadioPower",
      [&](int32_t serial) {
        return implementation_->radio_service->setRadioPower(serial, enabled);
      },
      record, status, timeout_ms);
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
  switch (state) {
  case 0:
    return "off";
  case 1:
    return "unavailable";
  case 10:
    return "on";
  default:
    return "unknown";
  }
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
  case 4:
    return "unknown";
  case 5:
    return "roaming";
  case 10:
    return "not-registered-emergency";
  case 12:
    return "searching-emergency";
  case 13:
    return "denied-emergency";
  case 14:
    return "unknown-emergency";
  default:
    return "unknown";
  }
}

const char *radioTechnologyName(int technology) {
  static constexpr const char *kNames[] = {
      "unknown", "gprs",  "edge",  "umts",     "is95a", "is95b",  "1xrtt",
      "evdo0",   "evdoa", "hsdpa", "hsupa",    "hspa",  "evdob",  "ehrpd",
      "lte",     "hspap", "gsm",   "td-scdma", "iwlan", "lte-ca",
  };
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
  case 16:
    return "missing-resource";
  case 17:
    return "no-such-element";
  case 38:
    return "internal-error";
  case 39:
    return "system-error";
  case 40:
    return "modem-error";
  case 41:
    return "invalid-state";
  case 42:
    return "no-resources";
  case 43:
    return "sim-error";
  case 44:
    return "invalid-arguments";
  case 45:
    return "invalid-sim-state";
  case 46:
    return "invalid-modem-state";
  case 49:
    return "network-error";
  case 51:
    return "sim-busy";
  case 60:
    return "network-not-ready";
  case 62:
    return "no-subscription";
  case 63:
    return "no-network-found";
  case -1:
    return "transport-error";
  default:
    return "hal-error";
  }
}

} // namespace oos::modem
