#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oos::modem {

struct ModemRequestStatus {
  std::string operation;
  int error = -1;
  bool timed_out = false;
};

struct SimStatus {
  int card_state = -1;
  int universal_pin_state = -1;
  int application_count = 0;
};

struct DeviceIdentity {
  std::string imei;
  std::string imei_software_version;
  std::string esn;
  std::string meid;
};

struct SignalStatus {
  int gsm_strength = 99;
  int gsm_bit_error_rate = 99;
  int cdma_dbm = -1;
  int cdma_ecio = -1;
  int evdo_dbm = -1;
  int evdo_ecio = -1;
  int evdo_snr = -1;
  int lte_strength = 99;
  int lte_rsrp = -1;
  int lte_rsrq = -1;
  int lte_rssnr = -1;
  int lte_cqi = -1;
  int lte_timing_advance = -1;
  int tdscdma_rscp = -1;
};

struct RegistrationStatus {
  int state = -1;
  int radio_technology = -1;
  int denial_reason = 0;
  int max_data_calls = 0;
};

struct OperatorStatus {
  std::string long_name;
  std::string short_name;
  std::string numeric;
};

struct ModemSnapshot {
  bool service_connected = false;
  int radio_state = -1;
  std::string baseband_version;
  DeviceIdentity identity;
  SimStatus sim;
  SignalStatus signal;
  RegistrationStatus voice_registration;
  RegistrationStatus data_registration;
  OperatorStatus network_operator;
  int preferred_network_type = -1;
  int voice_radio_technology = -1;
  int current_call_count = -1;
  int data_call_count = -1;
  int hardware_config_count = -1;
  uint32_t radio_access_family = 0;
  std::string logical_modem_uuid;
  std::vector<ModemRequestStatus> requests;
};

// Minimal read-only state used by latency-sensitive system components such as
// SystemUI. Unlike ModemSnapshot, this does not request identity, call, data
// call, hardware configuration or radio capability diagnostics.
struct NetworkStatus {
  bool service_connected = false;
  int radio_state = -1;
  SignalStatus signal;
  RegistrationStatus voice_registration;
  RegistrationStatus data_registration;
};

class ModemManager {
public:
  ModemManager();
  ~ModemManager();

  ModemManager(const ModemManager &) = delete;
  ModemManager &operator=(const ModemManager &) = delete;

  bool initialize(const std::string &service_name = "slot1");
  void shutdown();
  bool initialized() const;

  // Performs read-only Radio HAL requests. Individual request failures are
  // reported in snapshot.requests; false means the HAL itself was unavailable.
  bool querySnapshot(ModemSnapshot &snapshot, int timeout_ms = 5000);
  bool queryNetworkStatus(NetworkStatus &status, int timeout_ms = 1500);

  // Explicitly mutates RF power. Diagnostics do not call this automatically.
  bool setRadioPower(bool enabled, ModemRequestStatus &status,
                     int timeout_ms = 10000);

  const std::string &lastError() const;

private:
  struct Implementation;
  std::unique_ptr<Implementation> implementation_;
};

const char *cardStateName(int state);
const char *radioStateName(int state);
const char *registrationStateName(int state);
const char *radioTechnologyName(int technology);
const char *radioErrorName(int error);

} // namespace oos::modem
