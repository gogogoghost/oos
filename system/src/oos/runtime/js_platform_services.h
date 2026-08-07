#pragma once

#include <quickjs.h>

namespace oos::runtime {

class ApplicationContext;

class JsPlatformHost {
public:
  virtual ~JsPlatformHost() = default;
  virtual ApplicationContext *jsApplicationContext() = 0;
  virtual bool jsRequestExit() = 0;
};

JSModuleDef *loadJsRuntimeModule(JSContext *context);
bool isJsPlatformServiceModule(const char *name);
JSModuleDef *loadJsPlatformServiceModule(JSContext *context, const char *name);

} // namespace oos::runtime
