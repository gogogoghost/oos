#include "oos/device/nokia8110/permission_controller.h"

#include <binder/IPCThreadState.h>
#include <binder/IPermissionController.h>
#include <binder/IServiceManager.h>
#include <binder/ProcessState.h>
#include <private/android_filesystem_config.h>
#include <utils/String16.h>
#include <utils/Vector.h>

#include <mutex>

namespace oos::device::nokia8110 {
namespace {

class OosPermissionController final : public android::BnPermissionController {
public:
  bool checkPermission(const android::String16 &, int32_t, int32_t uid) final {
    switch (uid) {
    case AID_ROOT:
    case AID_SYSTEM:
    case AID_MEDIA:
    case AID_AUDIO:
    case AID_CAMERA:
      return true;
    default:
      return false;
    }
  }

  void getPackagesForUid(uid_t uid,
                         android::Vector<android::String16> &packages) final {
    if (checkPermission(android::String16(), 0, static_cast<int32_t>(uid)))
      packages.add(android::String16("org.orangeos.system"));
  }

  bool isRuntimePermission(const android::String16 &permission) final {
    return permission == android::String16("android.permission.RECORD_AUDIO") ||
           permission == android::String16("android.permission.CAMERA");
  }
};

std::mutex service_mutex;
android::sp<OosPermissionController> service;

} // namespace

bool ensurePermissionController(std::string &error) {
  std::lock_guard<std::mutex> lock(service_mutex);
  error.clear();
  android::sp<android::IServiceManager> manager =
      android::defaultServiceManager();
  if (manager == nullptr) {
    error = "Android service manager is unavailable";
    return false;
  }
  android::sp<android::IBinder> existing =
      manager->checkService(android::String16("permission"));
  if (existing != nullptr && existing->isBinderAlive())
    return true;

  if (service == nullptr)
    service = new OosPermissionController();
  const android::status_t status =
      manager->addService(android::String16("permission"), service, false);
  if (status != android::OK) {
    error = "register permission service failed: " + std::to_string(status);
    return false;
  }
  android::ProcessState::self()->startThreadPool();
  return true;
}

} // namespace oos::device::nokia8110
