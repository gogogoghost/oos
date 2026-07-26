#include "oos/network/ip_manager.h"

#include <arpa/inet.h>
#include <cutils/properties.h>
#include <ifaddrs.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>

extern "C" {
void ifc_clear_ipv4_addresses(const char *name);
int ifc_remove_default_route(const char *ifname);
int ifc_configure(const char *ifname, in_addr_t address, uint32_t prefix_length,
                  in_addr_t gateway, in_addr_t dns1, in_addr_t dns2);
}

namespace oos::network {
namespace {

std::string ipv4Text(in_addr_t address) {
  char text[INET_ADDRSTRLEN]{};
  in_addr value{address};
  return inet_ntop(AF_INET, &value, text, sizeof(text)) ? text : std::string();
}

bool parseIpv4(const std::string &text, in_addr_t &address,
               bool allow_empty = false) {
  if (text.empty() && allow_empty) {
    address = 0;
    return true;
  }
  in_addr value{};
  if (inet_pton(AF_INET, text.c_str(), &value) != 1)
    return false;
  address = value.s_addr;
  return true;
}

unsigned prefixLength(in_addr_t mask) {
  uint32_t value = ntohl(mask);
  unsigned prefix = 0;
  while (value & 0x80000000U) {
    ++prefix;
    value <<= 1;
  }
  return prefix;
}

std::string getProperty(const std::string &name) {
  char value[PROPERTY_VALUE_MAX]{};
  property_get(name.c_str(), value, "");
  return value;
}

bool addRouteAttribute(nlmsghdr *header, size_t capacity, uint16_t type,
                       const void *value, size_t length) {
  const size_t attribute_length = RTA_LENGTH(length);
  const size_t aligned_message = NLMSG_ALIGN(header->nlmsg_len);
  if (aligned_message + RTA_ALIGN(attribute_length) > capacity)
    return false;
  auto *attribute = reinterpret_cast<rtattr *>(
      reinterpret_cast<uint8_t *>(header) + aligned_message);
  attribute->rta_type = type;
  attribute->rta_len = static_cast<uint16_t>(attribute_length);
  std::memcpy(RTA_DATA(attribute), value, length);
  header->nlmsg_len =
      static_cast<uint32_t>(aligned_message + RTA_ALIGN(attribute_length));
  return true;
}

bool addIpv4Route(uint32_t table, unsigned interface_index, in_addr_t address,
                  unsigned prefix, in_addr_t gateway, bool main_default,
                  std::string &error) {
  struct Request {
    nlmsghdr header;
    rtmsg route;
    uint8_t attributes[128];
  } request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  request.header.nlmsg_type = RTM_NEWROUTE;
  request.header.nlmsg_flags =
      NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE;
  request.header.nlmsg_seq = 1;
  request.route.rtm_family = AF_INET;
  request.route.rtm_dst_len = static_cast<uint8_t>(prefix);
  request.route.rtm_table =
      table <= UINT8_MAX ? static_cast<uint8_t>(table) : RT_TABLE_UNSPEC;
  request.route.rtm_protocol = RTPROT_STATIC;
  request.route.rtm_scope = gateway ? RT_SCOPE_UNIVERSE : RT_SCOPE_LINK;
  request.route.rtm_type = RTN_UNICAST;
  if (main_default)
    request.route.rtm_flags = RTNH_F_ONLINK;

  if (table > UINT8_MAX && !addRouteAttribute(&request.header, sizeof(request),
                                              RTA_TABLE, &table, sizeof(table)))
    return false;
  if (prefix) {
    const uint32_t host_mask =
        prefix == 32 ? UINT32_MAX : UINT32_MAX << (32 - prefix);
    const in_addr_t destination = htonl(ntohl(address) & host_mask);
    if (!addRouteAttribute(&request.header, sizeof(request), RTA_DST,
                           &destination, sizeof(destination)))
      return false;
  }
  if (gateway && !addRouteAttribute(&request.header, sizeof(request),
                                    RTA_GATEWAY, &gateway, sizeof(gateway)))
    return false;
  if (!addRouteAttribute(&request.header, sizeof(request), RTA_OIF,
                         &interface_index, sizeof(interface_index)))
    return false;

  const int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) {
    error = "open route netlink: " + std::string(std::strerror(errno));
    return false;
  }
  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  const ssize_t sent =
      sendto(fd, &request, request.header.nlmsg_len, 0,
             reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel));
  std::array<uint8_t, 512> response{};
  const ssize_t received =
      sent < 0 ? -1 : recv(fd, response.data(), response.size(), 0);
  const int saved_errno = errno;
  close(fd);
  if (sent < 0 ||
      received < static_cast<ssize_t>(NLMSG_LENGTH(sizeof(nlmsgerr)))) {
    error = "route netlink I/O: " + std::string(std::strerror(saved_errno));
    return false;
  }
  const auto *header = reinterpret_cast<const nlmsghdr *>(response.data());
  if (header->nlmsg_type != NLMSG_ERROR) {
    error = "unexpected route netlink response";
    return false;
  }
  const auto *ack = reinterpret_cast<const nlmsgerr *>(NLMSG_DATA(header));
  if (ack->error) {
    error = "add route: " + std::string(std::strerror(-ack->error));
    return false;
  }
  return true;
}

bool configureAndroidRoutes(const std::string &interface_name,
                            in_addr_t address, unsigned prefix,
                            in_addr_t gateway, std::string &error) {
  const unsigned interface_index = if_nametoindex(interface_name.c_str());
  if (!interface_index) {
    error = "unknown interface " + interface_name;
    return false;
  }
  // Android netd assigns per-interface tables as 1000 + ifindex on this
  // platform. Rules select this table before the legacy main table.
  const uint32_t interface_table = 1000 + interface_index;
  if (!addIpv4Route(interface_table, interface_index, address, prefix, 0, false,
                    error) ||
      (gateway && !addIpv4Route(interface_table, interface_index, 0, 0, gateway,
                                false, error)))
    return false;
  // Keep legacy tools and /proc/net/route consistent with the policy table.
  return !gateway || addIpv4Route(RT_TABLE_MAIN, interface_index, 0, 0, gateway,
                                  true, error);
}

} // namespace

IpManager::IpManager(std::string interface_name)
    : interface_name_(std::move(interface_name)),
      service_name_("dhcpcd_" + interface_name_) {}

bool IpManager::status(IpConfiguration &configuration) {
  configuration = {};
  configuration.interface_name = interface_name_;
  ifaddrs *interfaces = nullptr;
  if (getifaddrs(&interfaces) < 0) {
    last_error_ = std::strerror(errno);
    return false;
  }
  for (const ifaddrs *entry = interfaces; entry; entry = entry->ifa_next) {
    if (!entry->ifa_addr || interface_name_ != entry->ifa_name ||
        entry->ifa_addr->sa_family != AF_INET)
      continue;
    const auto *address =
        reinterpret_cast<const sockaddr_in *>(entry->ifa_addr);
    const auto *netmask =
        reinterpret_cast<const sockaddr_in *>(entry->ifa_netmask);
    configuration.address = ipv4Text(address->sin_addr.s_addr);
    if (netmask)
      configuration.prefix_length = prefixLength(netmask->sin_addr.s_addr);
    break;
  }
  freeifaddrs(interfaces);

  std::ifstream routes("/proc/net/route");
  std::string line;
  std::getline(routes, line);
  while (std::getline(routes, line)) {
    std::istringstream fields(line);
    std::string interface;
    std::string destination;
    std::string gateway;
    if (!(fields >> interface >> destination >> gateway) ||
        interface != interface_name_ || destination != "00000000")
      continue;
    char *end = nullptr;
    errno = 0;
    const unsigned long raw = std::strtoul(gateway.c_str(), &end, 16);
    if (!errno && end && !*end)
      configuration.gateway = ipv4Text(static_cast<in_addr_t>(raw));
    break;
  }
  configuration.dns1 = getProperty("net." + interface_name_ + ".dns1");
  configuration.dns2 = getProperty("net." + interface_name_ + ".dns2");
  last_error_.clear();
  return true;
}

bool IpManager::setServiceState(const char *action, int timeout_ms) {
  if (property_set(action, service_name_.c_str()) < 0) {
    last_error_ = std::string("property_set ") + action + " failed";
    return false;
  }
  const bool starting = std::strcmp(action, "ctl.start") == 0;
  const std::string state_property = "init.svc." + service_name_;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  do {
    const std::string state = getProperty(state_property);
    if ((starting && state == "running") || (!starting && state == "stopped"))
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (std::chrono::steady_clock::now() < deadline);
  last_error_ = service_name_ + (starting ? " did not start" : " did not stop");
  return false;
}

bool IpManager::useDhcp(int timeout_ms) {
  setServiceState("ctl.stop", 5000);
  ifc_remove_default_route(interface_name_.c_str());
  ifc_clear_ipv4_addresses(interface_name_.c_str());
  if (!setServiceState("ctl.start", 5000))
    return false;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  do {
    IpConfiguration current;
    if (status(current) && !current.address.empty()) {
      const std::string gateway_text =
          getProperty("dhcp." + interface_name_ + ".gateway");
      in_addr_t address = 0;
      in_addr_t gateway = 0;
      if (!parseIpv4(current.address, address) ||
          !parseIpv4(gateway_text, gateway) ||
          !configureAndroidRoutes(interface_name_, address,
                                  current.prefix_length, gateway,
                                  last_error_)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        continue;
      }
      IpConfiguration complete;
      if (status(complete) && !complete.gateway.empty())
        return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  } while (std::chrono::steady_clock::now() < deadline);
  last_error_ = "DHCP did not assign an address";
  return false;
}

bool IpManager::useStatic(const IpConfiguration &configuration) {
  in_addr_t address = 0;
  in_addr_t gateway = 0;
  in_addr_t dns1 = 0;
  in_addr_t dns2 = 0;
  if (configuration.prefix_length > 32 ||
      !parseIpv4(configuration.address, address) ||
      !parseIpv4(configuration.gateway, gateway, true) ||
      !parseIpv4(configuration.dns1, dns1, true) ||
      !parseIpv4(configuration.dns2, dns2, true)) {
    last_error_ = "invalid static IPv4 configuration";
    return false;
  }
  if (!setServiceState("ctl.stop", 5000))
    return false;
  ifc_remove_default_route(interface_name_.c_str());
  ifc_clear_ipv4_addresses(interface_name_.c_str());
  // libnetutils sets the address and prefix before its legacy default-route
  // call. That call fails on Android's per-interface policy table, so complete
  // the route setup through rtnetlink and program properties explicitly.
  ifc_configure(interface_name_.c_str(), address, configuration.prefix_length,
                gateway, dns1, dns2);
  if (!configureAndroidRoutes(interface_name_, address,
                              configuration.prefix_length, gateway,
                              last_error_)) {
    return false;
  }
  property_set(("net." + interface_name_ + ".gw").c_str(),
               configuration.gateway.c_str());
  property_set(("net." + interface_name_ + ".dns1").c_str(),
               configuration.dns1.c_str());
  property_set(("net." + interface_name_ + ".dns2").c_str(),
               configuration.dns2.c_str());
  last_error_.clear();
  return true;
}

const std::string &IpManager::lastError() const { return last_error_; }

} // namespace oos::network
