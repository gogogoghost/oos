#include "oos/device/services.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

std::string maskedIdentifier(const std::string &value) {
  if (value.empty())
    return "";
  if (value.size() <= 4)
    return std::string(value.size(), '*');
  return std::string(value.size() - 4, '*') + value.substr(value.size() - 4);
}

void printUsage(const char *program) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s status\n"
               "  %s power on|off\n",
               program, program);
}

int printStatus(oos::modem::ModemManager &modem) {
  oos::modem::ModemSnapshot snapshot;
  if (!modem.querySnapshot(snapshot)) {
    std::fprintf(stderr, "Modem query failed: %s\n", modem.lastError().c_str());
    return 1;
  }

  std::printf("service_connected=%d\n", snapshot.service_connected);
  std::printf("radio_state=%s(%d)\n",
              oos::modem::radioStateName(snapshot.radio_state),
              snapshot.radio_state);
  std::printf("baseband=%s\n", snapshot.baseband_version.c_str());
  std::printf("imei=%s\n", maskedIdentifier(snapshot.identity.imei).c_str());
  std::printf(
      "imeisv=%s\n",
      maskedIdentifier(snapshot.identity.imei_software_version).c_str());
  std::printf("esn=%s\n", maskedIdentifier(snapshot.identity.esn).c_str());
  std::printf("meid=%s\n", maskedIdentifier(snapshot.identity.meid).c_str());
  std::printf("sim_state=%s(%d)\n",
              oos::modem::cardStateName(snapshot.sim.card_state),
              snapshot.sim.card_state);
  std::printf("sim_apps=%d\n", snapshot.sim.application_count);
  std::printf(
      "voice_registration=%s(%d) rat=%s(%d) denial=%d\n",
      oos::modem::registrationStateName(snapshot.voice_registration.state),
      snapshot.voice_registration.state,
      oos::modem::radioTechnologyName(
          snapshot.voice_registration.radio_technology),
      snapshot.voice_registration.radio_technology,
      snapshot.voice_registration.denial_reason);
  std::printf(
      "data_registration=%s(%d) rat=%s(%d) denial=%d max_calls=%d\n",
      oos::modem::registrationStateName(snapshot.data_registration.state),
      snapshot.data_registration.state,
      oos::modem::radioTechnologyName(
          snapshot.data_registration.radio_technology),
      snapshot.data_registration.radio_technology,
      snapshot.data_registration.denial_reason,
      snapshot.data_registration.max_data_calls);
  std::printf("operator_long=%s\noperator_short=%s\noperator_numeric=%s\n",
              snapshot.network_operator.long_name.c_str(),
              snapshot.network_operator.short_name.c_str(),
              snapshot.network_operator.numeric.c_str());
  std::printf("signal_gsm=%d ber=%d\n", snapshot.signal.gsm_strength,
              snapshot.signal.gsm_bit_error_rate);
  std::printf("signal_cdma_dbm=%d ecio=%d\n", snapshot.signal.cdma_dbm,
              snapshot.signal.cdma_ecio);
  std::printf("signal_evdo_dbm=%d ecio=%d snr=%d\n", snapshot.signal.evdo_dbm,
              snapshot.signal.evdo_ecio, snapshot.signal.evdo_snr);
  std::printf("signal_lte=%d rsrp=%d rsrq=%d rssnr=%d cqi=%d ta=%d\n",
              snapshot.signal.lte_strength, snapshot.signal.lte_rsrp,
              snapshot.signal.lte_rsrq, snapshot.signal.lte_rssnr,
              snapshot.signal.lte_cqi, snapshot.signal.lte_timing_advance);
  std::printf("preferred_network_type=%d\n", snapshot.preferred_network_type);
  std::printf("voice_radio_technology=%s(%d)\n",
              oos::modem::radioTechnologyName(snapshot.voice_radio_technology),
              snapshot.voice_radio_technology);
  std::printf("current_calls=%d\ndata_calls=%d\nhardware_configs=%d\n",
              snapshot.current_call_count, snapshot.data_call_count,
              snapshot.hardware_config_count);
  std::printf("radio_access_family=0x%08x\nlogical_modem_uuid=%s\n",
              snapshot.radio_access_family,
              snapshot.logical_modem_uuid.c_str());

  bool timed_out = false;
  for (const auto &request : snapshot.requests) {
    std::printf("request.%s=%s(%d)%s\n", request.operation.c_str(),
                oos::modem::radioErrorName(request.error), request.error,
                request.timed_out ? " timeout" : "");
    timed_out |= request.timed_out;
  }
  if (timed_out) {
    std::fprintf(stderr, "One or more Radio HAL requests timed out\n");
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    printUsage(argv[0]);
    return 2;
  }
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  if (!device) {
    std::fprintf(stderr, "Device factory failed\n");
    return 1;
  }
  oos::modem::ModemManager modem;
  if (!oos::device::initializeService(*device, modem)) {
    std::fprintf(stderr, "Modem initialization failed: %s\n",
                 modem.lastError().c_str());
    return 1;
  }
  if (!std::strcmp(argv[1], "status"))
    return printStatus(modem);
  if (!std::strcmp(argv[1], "power") && argc == 3) {
    const bool on = !std::strcmp(argv[2], "on");
    if (!on && std::strcmp(argv[2], "off")) {
      printUsage(argv[0]);
      return 2;
    }
    oos::modem::ModemRequestStatus status;
    if (!modem.setRadioPower(on, status)) {
      std::fprintf(stderr, "Radio power request failed: %s\n",
                   modem.lastError().c_str());
      return 1;
    }
    std::printf("set_radio_power=%s error=%s(%d)%s\n", on ? "on" : "off",
                oos::modem::radioErrorName(status.error), status.error,
                status.timed_out ? " timeout" : "");
    return status.timed_out || status.error != 0 ? 1 : 0;
  }
  printUsage(argv[0]);
  return 2;
}
