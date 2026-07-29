#include "oos/services/system_service.h"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <unistd.h>
#include <vector>

namespace {

int request(oos::services::SystemServiceHub &services, const char *app_id,
            const std::vector<std::string> &permissions, const char *service,
            const char *operation, const char *payload, std::string &response,
            bool system_authority = false) {
  return services.request(app_id, permissions, service, operation, payload,
                          response, system_authority);
}

} // namespace

int main() {
  char root_template[] = "/tmp/oos-system-services.XXXXXX";
  const char *root = mkdtemp(root_template);
  assert(root);

  oos::services::SystemServiceHub services(root);
  assert(services.initialize());
  std::string response;
  const std::vector<std::string> no_permissions;
  const std::vector<std::string> settings = {"settings:read",
                                              "settings:write"};
  const std::vector<std::string> settings_readonly = {"settings",
                                                       "settings:read"};
  const std::vector<std::string> settings_createonly = {"settings",
                                                         "settings:create"};
  assert(request(services, "app.one", no_permissions, "settings", "get",
                 R"({"name":"theme"})", response) == -EACCES);
  assert(request(services, "app.one", settings, "settings", "set",
                 R"({"name":"theme","value":"{\"dark\":true}"})",
                 response) == 0);
  assert(request(services, "app.one", settings, "settings", "get",
                 R"({"name":"theme"})", response) == 0);
  assert(response.find(R"("dark":true)") != std::string::npos);
  assert(request(services, "app.one", settings, "settings", "get-batch",
                 R"({"names":["theme","missing"]})", response) == 0);
  assert(response == R"({"theme":{"dark":true},"missing":null})");
  assert(request(services, "app.readonly", settings_readonly, "settings", "set",
                 R"({"name":"theme","value":"false"})", response) ==
         -EACCES);
  assert(request(services, "app.createonly", settings_createonly, "settings",
                 "set", R"({"name":"first-run","value":"true"})",
                 response) == 0);
  assert(request(services, "app.createonly", settings_createonly, "settings",
                 "get", R"({"name":"first-run"})", response) == -EACCES);
  assert(request(services, "app.one", no_permissions, "system-messages",
                 "subscribe", R"({"topic":"setting:theme"})", response) ==
         -EACCES);
  assert(request(services, "app.one", settings, "system-messages",
                 "subscribe", R"({"topic":"setting:theme"})", response) ==
         0);
  assert(request(services, "system.ui", no_permissions, "settings", "set",
                 R"({"name":"theme","value":"{\"dark\":false}"})",
                 response, true) == 0);
  assert(request(services, "app.one", no_permissions, "system-messages",
                 "poll", R"({"after":0,"limit":8})", response) == 0);
  assert(response.find("setting:theme") != std::string::npos);
  assert(request(services, "app.one", no_permissions, "system-messages",
                 "poll", R"({"after":0,"limit":8})", response) == 0);
  assert(response == "[]");

  const std::vector<std::string> alarms = {"alarms"};
  assert(request(services, "app.one", alarms, "alarms", "add",
                 R"({"dateMs":4102444800000,"ignoreTimezone":false,"data":"{\"kind\":\"wake\"}"})",
                 response) == 0);
  assert(response == "1");
  assert(request(services, "app.one", alarms, "alarms", "list", "{}",
                 response) == 0);
  assert(response.find(R"("kind":"wake")") != std::string::npos);
  assert(request(services, "system.ui", no_permissions, "alarms", "list-all",
                 "{}", response, true) == 0);
  assert(response.find(R"("owner":"app.one")") != std::string::npos);

  const std::vector<std::string> notifications = {"desktop-notification"};
  assert(request(services, "app.one", notifications, "notifications", "add",
                 R"({"value":"{\"title\":\"Hello\"}"})", response) == 0);
  assert(request(services, "system.ui", no_permissions, "notifications",
                 "list-all", "{}", response, true) == 0);
  assert(response.find(R"("title":"Hello")") != std::string::npos);

  const std::vector<std::string> contacts = {"contacts:write"};
  const std::vector<std::string> contacts_readonly = {"contacts",
                                                       "contacts:read"};
  const std::vector<std::string> contacts_createonly = {"contacts",
                                                         "contacts:create"};
  assert(request(services, "app.one", contacts, "contacts", "add",
                 R"({"value":"{\"name\":\"Ada\"}"})", response) == 0);
  assert(request(services, "app.one", contacts, "contacts", "list", "{}",
                 response) == 0);
  assert(response.find(R"("name":"Ada")") != std::string::npos);
  assert(request(services, "app.readonly", contacts_readonly, "contacts", "add",
                 R"({"value":"{\"name\":\"Grace\"}"})", response) ==
         -EACCES);
  assert(request(services, "app.createonly", contacts_createonly, "contacts",
                 "add", R"({"value":"{\"name\":\"Lin\"}"})",
                 response) == 0);
  assert(request(services, "app.createonly", contacts_createonly, "contacts",
                 "list", "{}", response) == -EACCES);

  assert(request(services, "app.one", no_permissions, "activities", "start",
                 R"({"name":"pick","data":"{\"type\":\"image\"}"})",
                 response) == 0);
  const std::string activity_id = response;
  assert(request(services, "app.two", no_permissions, "activities", "status",
                 (std::string("{\"id\":") + activity_id + "}").c_str(),
                 response) == 0);
  assert(response == "null");
  assert(request(services, "system.ui", no_permissions, "activities", "list",
                 "{}", response, true) == 0);
  assert(response.find(R"("owner":"app.one")") != std::string::npos);
  assert(request(services, "system.ui", no_permissions, "activities", "put",
                 (std::string("{\"id\":") + activity_id +
                  ",\"owner\":\"app.one\",\"value\":\"{\\\"state\\\":\\\"resolved\\\",\\\"result\\\":{\\\"path\\\":\\\"photo.jpg\\\"}}\"}")
                     .c_str(),
                 response, true) == 0);
  assert(request(services, "app.one", no_permissions, "activities", "status",
                 (std::string("{\"id\":") + activity_id + "}").c_str(),
                 response) == 0);
  assert(response.find(R"("state":"resolved")") != std::string::npos);

  assert(request(services, "app.one", no_permissions, "audio-policy",
                 "request", R"({"action":"VOLUME_UP"})", response) == 0);
  assert(request(services, "system.ui", no_permissions, "audio-policy",
                 "list-requests", "{}", response, true) == 0);
  assert(response.find("VOLUME_UP") != std::string::npos);

  assert(request(services, "app.one", no_permissions, "system-messages",
                 "subscribe", R"({"topic":"alarm"})", response) ==
         -EACCES);
  const std::vector<std::string> alarm_messages = {"system-message:alarm"};
  assert(request(services, "app.one", alarm_messages, "system-messages",
                 "subscribe", R"({"topic":"alarm"})", response) == 0);
  assert(request(services, "system.ui", no_permissions, "system-messages",
                 "publish",
                 R"({"targetApp":"app.one","topic":"alarm","payload":"{\"id\":7}"})",
                 response, true) == 0);
  assert(request(services, "app.one", no_permissions, "system-messages",
                 "poll", R"({"after":0,"limit":8})", response) == 0);
  assert(response.find(R"("topic":"alarm")") != std::string::npos);

  std::filesystem::remove_all(root);
  std::fprintf(stderr, "PASS: managed system services\n");
  return 0;
}
