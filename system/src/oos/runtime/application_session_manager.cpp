#include "oos/runtime/application_session_manager.h"

#include <functional>
#include <utility>
#include <vector>

namespace oos::runtime {

namespace {

class StoredStatusBarAppearance final
    : public ui::StatusBarAppearanceController {
public:
  StoredStatusBarAppearance(
      ui::StatusBarAppearance initial,
      std::function<void(ui::StatusBarAppearance)> changed)
      : appearance_(initial), changed_(std::move(changed)) {}

  void setStatusBarAppearance(ui::StatusBarAppearance appearance) override {
    appearance.background_rgb &= 0x00ffffffu;
    if (appearance_ == appearance)
      return;
    appearance_ = appearance;
    changed_(appearance_);
  }

  ui::StatusBarAppearance statusBarAppearance() const override {
    return appearance_;
  }

  bool setSurfaceMode(ui::SurfaceMode mode) override {
    if (!surface_changed_ || !surface_changed_(mode))
      return false;
    surface_mode_ = mode;
    return true;
  }

  ui::SurfaceMode surfaceMode() const override { return surface_mode_; }

  void setSurfaceCallback(std::function<bool(ui::SurfaceMode)> callback) {
    surface_changed_ = std::move(callback);
  }

private:
  ui::StatusBarAppearance appearance_;
  std::function<void(ui::StatusBarAppearance)> changed_;
  std::function<bool(ui::SurfaceMode)> surface_changed_;
  ui::SurfaceMode surface_mode_ = ui::SurfaceMode::Normal;
};

} // namespace

class ApplicationSessionManager::Impl {
public:
  struct Entry {
    std::string id;
    Factory factory;
    std::unique_ptr<compositor::LayerSurface> surface;
    std::unique_ptr<ApplicationSession> session;
    std::unique_ptr<StoredStatusBarAppearance> appearance;
  };

  Impl(compositor::Compositor &compositor, int32_t x, int32_t y, uint32_t width,
       uint32_t height, ui::StatusBarAppearanceHost &appearance_host,
       ui::StatusBarAppearance default_appearance)
      : compositor(compositor), appearance_host(appearance_host), x(x), y(y),
        width(width), height(height), default_appearance(default_appearance) {}

  Entry *find(const std::string &id) {
    for (const auto &entry : entries) {
      if (entry->id == id)
        return entry.get();
    }
    return nullptr;
  }

  const Entry *find(const std::string &id) const {
    for (const auto &entry : entries) {
      if (entry->id == id)
        return entry.get();
    }
    return nullptr;
  }

  compositor::Compositor &compositor;
  ui::StatusBarAppearanceHost &appearance_host;
  int32_t x;
  int32_t y;
  uint32_t width;
  uint32_t height;
  ui::StatusBarAppearance default_appearance;
  std::vector<std::unique_ptr<Entry>> entries;
  Entry *active = nullptr;
  std::string error;
};

ApplicationSessionManager::ApplicationSessionManager(
    compositor::Compositor &compositor, int32_t x, int32_t y, uint32_t width,
    uint32_t height, ui::StatusBarAppearanceHost &appearance_host,
    ui::StatusBarAppearance default_appearance)
    : impl_(std::make_unique<Impl>(compositor, x, y, width, height,
                                   appearance_host, default_appearance)) {}

ApplicationSessionManager::~ApplicationSessionManager() { shutdown(); }

bool ApplicationSessionManager::registerFactory(std::string id,
                                                Factory factory) {
  impl_->error.clear();
  if (id.empty() || !factory) {
    impl_->error = "application session registration is invalid";
    return false;
  }
  if (impl_->find(id)) {
    impl_->error = "application session is already registered: " + id;
    return false;
  }
  auto entry = std::make_unique<Impl::Entry>();
  entry->id = std::move(id);
  entry->factory = std::move(factory);
  Impl::Entry *stored = entry.get();
  entry->appearance = std::make_unique<StoredStatusBarAppearance>(
      impl_->default_appearance, [this, stored](ui::StatusBarAppearance value) {
        if (impl_->active == stored)
          impl_->appearance_host.applyStatusBarAppearance(value);
      });
  entry->appearance->setSurfaceCallback([this, stored](ui::SurfaceMode mode) {
    if (!stored->surface)
      return false;
    const bool immersive = mode == ui::SurfaceMode::Immersive;
    if (!stored->surface->setGeometry(
            impl_->x, immersive ? 0 : impl_->y, impl_->width,
            immersive ? impl_->compositor.height() : impl_->height))
      return false;
    if (impl_->active == stored)
      impl_->appearance_host.setStatusBarVisible(!immersive);
    return true;
  });
  impl_->entries.push_back(std::move(entry));
  return true;
}

bool ApplicationSessionManager::registered(const std::string &id) const {
  return impl_->find(id) != nullptr;
}

bool ApplicationSessionManager::activate(const std::string &id) {
  impl_->error.clear();
  Impl::Entry *entry = impl_->find(id);
  if (!entry) {
    impl_->error = "application session is not registered: " + id;
    return false;
  }
  if (!entry->session) {
    entry->surface = impl_->compositor.createLayer(
        {entry->id, impl_->x, impl_->y, impl_->width, impl_->height, 0});
    if (!entry->surface) {
      impl_->error = "create application layer failed: " + id;
      return false;
    }
    entry->surface->setVisible(false);
    entry->session = entry->factory(*entry->surface, *entry->appearance);
    if (!entry->session || !entry->session->initialize()) {
      impl_->error = entry->session ? entry->session->lastError()
                                    : "application factory returned null";
      if (entry->session)
        entry->session->shutdown();
      entry->session.reset();
      entry->surface.reset();
      return false;
    }
  }
  if (impl_->active && impl_->active != entry) {
    impl_->active->session->onDeactivated();
    impl_->active->surface->setVisible(false);
  }
  entry->surface->setVisible(true);
  impl_->active = entry;
  entry->session->onActivated();
  impl_->appearance_host.applyStatusBarAppearance(
      entry->appearance->statusBarAppearance());
  impl_->appearance_host.setStatusBarVisible(entry->appearance->surfaceMode() !=
                                             ui::SurfaceMode::Immersive);
  return true;
}

bool ApplicationSessionManager::frame(int64_t monotonic_us,
                                      uint32_t &next_delay_ms) {
  if (!impl_->active || !impl_->active->session) {
    impl_->error = "no active application session";
    return false;
  }
  if (!impl_->active->session->frame(monotonic_us, next_delay_ms)) {
    impl_->error = impl_->active->session->lastError();
    return false;
  }
  return true;
}

std::string ApplicationSessionManager::takeLaunchRequest() {
  return impl_->active && impl_->active->session
             ? impl_->active->session->takeLaunchRequest()
             : std::string();
}

bool ApplicationSessionManager::takeExitRequest() {
  return impl_->active && impl_->active->session &&
         impl_->active->session->takeExitRequest();
}

bool ApplicationSessionManager::destroy(const std::string &id) {
  impl_->error.clear();
  Impl::Entry *entry = impl_->find(id);
  if (!entry || !entry->session) {
    impl_->error = "application session is not resident: " + id;
    return false;
  }
  if (impl_->active == entry) {
    impl_->error = "cannot destroy the active application session";
    return false;
  }
  entry->session->shutdown();
  entry->session.reset();
  entry->surface.reset();
  entry->appearance = std::make_unique<StoredStatusBarAppearance>(
      impl_->default_appearance, [this, entry](ui::StatusBarAppearance value) {
        if (impl_->active == entry)
          impl_->appearance_host.applyStatusBarAppearance(value);
      });
  entry->appearance->setSurfaceCallback([this, entry](ui::SurfaceMode mode) {
    if (!entry->surface)
      return false;
    const bool immersive = mode == ui::SurfaceMode::Immersive;
    return entry->surface->setGeometry(
        impl_->x, immersive ? 0 : impl_->y, impl_->width,
        immersive ? impl_->compositor.height() : impl_->height);
  });
  return true;
}

void ApplicationSessionManager::shutdown() {
  if (!impl_)
    return;
  impl_->active = nullptr;
  for (const auto &entry : impl_->entries) {
    if (entry->session)
      entry->session->shutdown();
    entry->session.reset();
    entry->surface.reset();
  }
}

bool ApplicationSessionManager::dispatchKey(const input::KeyEvent &event,
                                            int64_t monotonic_us) {
  if (!impl_->active || !impl_->active->session) {
    impl_->error = "no active application session";
    return false;
  }
  if (!impl_->active->session->dispatchKey(event, monotonic_us)) {
    impl_->error = impl_->active->session->lastError();
    return false;
  }
  return true;
}

const char *ApplicationSessionManager::lastError() const {
  return impl_->error.c_str();
}

const char *ApplicationSessionManager::activeId() const {
  return impl_->active ? impl_->active->id.c_str() : nullptr;
}

size_t ApplicationSessionManager::residentCount() const {
  size_t count = 0;
  for (const auto &entry : impl_->entries) {
    if (entry->session)
      ++count;
  }
  return count;
}

} // namespace oos::runtime
