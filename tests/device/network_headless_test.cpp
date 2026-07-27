#include "oos/device/services.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

using oos::network::BluetoothDevice;
using oos::network::BluetoothManager;
using oos::network::BluetoothProfile;
using oos::network::BluetoothTransport;
using oos::network::IpConfiguration;
using oos::network::IpManager;
using oos::network::WifiAccessPoint;
using oos::network::WifiManager;
using oos::network::WifiSecurity;
using oos::network::WifiStatus;

namespace {

void usage(const char *program) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s wifi status|scan [seconds]|networks\n"
               "  %s wifi cycle\n"
               "  %s wifi connect <ssid> open\n"
               "  %s wifi connect <ssid> wpa <psk>\n"
               "  %s wifi disconnect|reconnect|forget <id>|save\n"
               "  %s ip status|dhcp|cycle\n"
               "  %s ip static <address> <prefix> <gateway> <dns1> [dns2]\n"
               "  %s bluetooth classic-scan|le-scan [seconds]\n"
               "  %s bluetooth pair <address> auto|classic|le [seconds]\n"
               "  %s bluetooth unpair|cancel-pairing <address>\n"
               "  %s bluetooth profile-cycle <address> hid|hfp|a2dp [seconds]\n"
               "  %s bluetooth gatt-cycle <address> [seconds]\n"
               "  %s bluetooth hold [seconds]\n",
               program, program, program, program, program, program, program,
               program, program, program, program, program, program);
}

int seconds(const char *text, int fallback) {
  if (!text)
    return fallback;
  const int value = std::atoi(text);
  return value > 0 && value <= 120 ? value : fallback;
}

void printWifiStatus(const WifiStatus &status) {
  std::printf("state=%s\nssid=%s\nbssid=%s\nnetwork_id=%d\nip=%s\n",
              status.state.c_str(), status.ssid.c_str(), status.bssid.c_str(),
              status.network_id, status.ip_address.c_str());
}

int wifiCommand(const oos::device::Device &device, int argc, char **argv) {
  WifiManager wifi = oos::device::createWifiManager(device);
  if (!oos::device::initializeService(device, wifi)) {
    std::fprintf(stderr, "Wi-Fi initialization failed: %s\n",
                 wifi.lastError().c_str());
    return 1;
  }
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  const char *command = argv[2];
  if (!std::strcmp(command, "status")) {
    WifiStatus status;
    if (wifi.status(status)) {
      printWifiStatus(status);
      return 0;
    }
  } else if (!std::strcmp(command, "scan")) {
    std::vector<WifiAccessPoint> results;
    if (wifi.scan(results, seconds(argc > 3 ? argv[3] : nullptr, 3) * 1000)) {
      for (const auto &ap : results)
        std::printf("%s\t%d MHz\t%d dBm\t%s\t%s\n", ap.bssid.c_str(),
                    ap.frequency_mhz, ap.signal_dbm, ap.flags.c_str(),
                    ap.ssid.c_str());
      std::printf("wifi_scan_count=%zu\n", results.size());
      return 0;
    }
  } else if (!std::strcmp(command, "networks")) {
    std::vector<oos::network::WifiNetwork> networks;
    if (wifi.listNetworks(networks)) {
      for (const auto &network : networks)
        std::printf("%d\t%s\t%s\t%s\n", network.id, network.ssid.c_str(),
                    network.bssid.c_str(), network.flags.c_str());
      return 0;
    }
  } else if (!std::strcmp(command, "cycle")) {
    WifiStatus before;
    if (!wifi.status(before) || !wifi.disconnect())
      goto failure;
    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (!wifi.reconnect())
      goto failure;
    for (int attempt = 0; attempt < 40; ++attempt) {
      WifiStatus after;
      if (wifi.status(after) && after.state == "COMPLETED") {
        std::printf("wifi_cycle_restored=1\n");
        printWifiStatus(after);
        return 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    std::fprintf(stderr, "Wi-Fi did not reconnect within 10 seconds\n");
    return 1;
  } else if (!std::strcmp(command, "connect") && argc >= 5) {
    const bool open = !std::strcmp(argv[4], "open");
    const bool wpa = !std::strcmp(argv[4], "wpa");
    if (!open && !wpa) {
      usage(argv[0]);
      return 2;
    }
    if (wpa && argc < 6) {
      usage(argv[0]);
      return 2;
    }
    int network_id = -1;
    if (wifi.connect(argv[3], open ? WifiSecurity::Open : WifiSecurity::WpaPsk,
                     wpa ? argv[5] : "", network_id)) {
      std::printf("network_id=%d\n", network_id);
      return 0;
    }
  } else if (!std::strcmp(command, "disconnect")) {
    if (wifi.disconnect())
      return 0;
  } else if (!std::strcmp(command, "reconnect")) {
    if (wifi.reconnect())
      return 0;
  } else if (!std::strcmp(command, "forget") && argc == 4) {
    if (wifi.forget(std::atoi(argv[3])))
      return 0;
  } else if (!std::strcmp(command, "save")) {
    if (wifi.saveConfiguration())
      return 0;
  } else {
    usage(argv[0]);
    return 2;
  }

failure:
  std::fprintf(stderr, "Wi-Fi command failed: %s\n", wifi.lastError().c_str());
  return 1;
}

int ipCommand(const oos::device::Device &device, int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  IpManager ip = oos::device::createIpManager(device);
  IpConfiguration configuration;
  bool ok = false;
  if (!std::strcmp(argv[2], "status")) {
    ok = ip.status(configuration);
  } else if (!std::strcmp(argv[2], "dhcp")) {
    ok = ip.useDhcp() && ip.status(configuration);
  } else if (!std::strcmp(argv[2], "cycle")) {
    IpConfiguration original;
    if (!ip.status(original) || original.address.empty() ||
        original.gateway.empty()) {
      std::fprintf(stderr, "No active IPv4 configuration to preserve\n");
      return 1;
    }
    std::printf("testing_static=%s/%u\n", original.address.c_str(),
                original.prefix_length);
    if (!ip.useStatic(original)) {
      const std::string static_error = ip.lastError();
      ip.useDhcp();
      std::fprintf(stderr, "Static IPv4 test failed: %s\n",
                   static_error.c_str());
      return 1;
    }
    IpConfiguration static_configuration;
    if (!ip.status(static_configuration) ||
        static_configuration.address != original.address) {
      ip.useDhcp();
      std::fprintf(stderr, "Static IPv4 address was not applied\n");
      return 1;
    }
    std::printf("static_applied=1\n");
    ok = ip.useDhcp() && ip.status(configuration);
    if (ok)
      std::printf("dhcp_restored=1\n");
  } else if (!std::strcmp(argv[2], "static") && argc >= 7) {
    configuration.interface_name = "wlan0";
    configuration.address = argv[3];
    configuration.prefix_length = static_cast<unsigned>(std::atoi(argv[4]));
    configuration.gateway = argv[5];
    configuration.dns1 = argv[6];
    configuration.dns2 = argc > 7 ? argv[7] : "";
    ok = ip.useStatic(configuration) && ip.status(configuration);
  } else {
    usage(argv[0]);
    return 2;
  }
  if (!ok) {
    std::fprintf(stderr, "IP command failed: %s\n", ip.lastError().c_str());
    return 1;
  }
  std::printf("interface=%s\naddress=%s/%u\ngateway=%s\ndns1=%s\ndns2=%s\n",
              configuration.interface_name.c_str(),
              configuration.address.c_str(), configuration.prefix_length,
              configuration.gateway.c_str(), configuration.dns1.c_str(),
              configuration.dns2.c_str());
  return 0;
}

void printBluetoothDevices(const std::vector<BluetoothDevice> &devices) {
  for (const auto &device : devices)
    std::printf("%s\trssi=%d\ttype=%d\tclass=0x%08x\tname=%s\tadv_bytes=%zu\n",
                device.address.c_str(), device.rssi, device.device_type,
                device.device_class, device.name.c_str(),
                device.advertising_data.size());
}

int bluetoothCommand(const oos::device::Device &device, int argc, char **argv) {
  if (argc < 3) {
    usage(argv[0]);
    return 2;
  }
  BluetoothManager bluetooth;
  if (!oos::device::initializeService(device, bluetooth)) {
    std::fprintf(stderr, "Bluetooth initialization failed: %s\n",
                 bluetooth.lastError().c_str());
    return 1;
  }
  const char *command = argv[2];
  bool ok = false;
  if (!std::strcmp(command, "classic-scan") ||
      !std::strcmp(command, "le-scan")) {
    std::vector<BluetoothDevice> devices;
    const int duration = seconds(argc > 3 ? argv[3] : nullptr, 8) * 1000;
    ok = !std::strcmp(command, "classic-scan")
             ? bluetooth.classicScan(devices, duration)
             : bluetooth.leScan(devices, duration);
    if (ok) {
      printBluetoothDevices(devices);
      std::printf("bluetooth_scan_count=%zu\n", devices.size());
    }
  } else if (!std::strcmp(command, "hold")) {
    ok = bluetooth.enable();
    if (ok) {
      std::printf("bluetooth_enabled=1\n");
      std::this_thread::sleep_for(
          std::chrono::seconds(seconds(argc > 3 ? argv[3] : nullptr, 10)));
    }
  } else if (!std::strcmp(command, "pair") && argc == 5) {
    BluetoothTransport transport = BluetoothTransport::Auto;
    if (!std::strcmp(argv[4], "classic"))
      transport = BluetoothTransport::Classic;
    else if (!std::strcmp(argv[4], "le"))
      transport = BluetoothTransport::LowEnergy;
    else if (std::strcmp(argv[4], "auto")) {
      usage(argv[0]);
      return 2;
    }
    ok = bluetooth.pair(argv[3], transport);
    if (ok) {
      const int hold = seconds(argc > 5 ? argv[5] : nullptr, 30);
      std::printf(
          "pairing_request_accepted=1; waiting %d seconds for bond events\n",
          hold);
      std::this_thread::sleep_for(std::chrono::seconds(hold));
    }
  } else if (!std::strcmp(command, "unpair") && argc == 4) {
    ok = bluetooth.unpair(argv[3]);
  } else if (!std::strcmp(command, "cancel-pairing") && argc == 4) {
    ok = bluetooth.cancelPairing(argv[3]);
  } else if (!std::strcmp(command, "profile-cycle") && argc >= 5) {
    BluetoothProfile profile;
    if (!std::strcmp(argv[4], "hid"))
      profile = BluetoothProfile::Hid;
    else if (!std::strcmp(argv[4], "hfp"))
      profile = BluetoothProfile::HandsFree;
    else if (!std::strcmp(argv[4], "a2dp"))
      profile = BluetoothProfile::A2dp;
    else {
      usage(argv[0]);
      return 2;
    }
    ok = bluetooth.profileConnectionCycle(
        argv[3], profile, seconds(argc > 5 ? argv[5] : nullptr, 5) * 1000);
  } else if (!std::strcmp(command, "gatt-cycle") && argc >= 4) {
    ok = bluetooth.leConnectionCycle(
        argv[3], seconds(argc > 4 ? argv[4] : nullptr, 5) * 1000);
  } else {
    usage(argv[0]);
    return 2;
  }
  if (!ok) {
    std::fprintf(stderr, "Bluetooth command failed: %s\n",
                 bluetooth.lastError().c_str());
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  if (argc < 2) {
    usage(argv[0]);
    return 2;
  }
  std::unique_ptr<oos::device::Device> device = oos::device::createDevice();
  if (!device) {
    std::fprintf(stderr, "Device factory failed\n");
    return 1;
  }
  if (!std::strcmp(argv[1], "wifi"))
    return wifiCommand(*device, argc, argv);
  if (!std::strcmp(argv[1], "ip"))
    return ipCommand(*device, argc, argv);
  if (!std::strcmp(argv[1], "bluetooth"))
    return bluetoothCommand(*device, argc, argv);
  usage(argv[0]);
  return 2;
}
