#include <jsc/jsc.h>
#include <wpe/webkit-web-process-extension.h>

#include <wasm_export.h>

// wasm_export.h must define WAMR's extended allocator API first.
#include <wasm_c_api.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kWasmPageSize = 64 * 1024;

struct ContextState;
struct InstanceHandle;

struct MemoryHandle {
  std::atomic<uint32_t> references{1};
  ContextState *state = nullptr;
  uint64_t id = 0;
  uint8_t *data = nullptr;
  size_t byte_length = 0;
  size_t capacity = 0;
  uint32_t maximum_pages = 0;
  JSCValue *buffer = nullptr;
};

struct RuntimeState {
  wasm_engine_t *engine = nullptr;
  wasm_store_t *store = nullptr;
  std::mutex memory_mutex;
  std::unordered_map<void *, MemoryHandle *> allocated_memories;
};

struct ContextState {
  std::atomic<uint32_t> references{1};
  JSCContext *context = nullptr;
  JSCClass *module_class = nullptr;
  JSCClass *memory_class = nullptr;
  JSCClass *instance_class = nullptr;
  JSCValue *module_class_anchor = nullptr;
  JSCValue *memory_class_anchor = nullptr;
  JSCValue *instance_class_anchor = nullptr;
  uint64_t next_memory_id = 1;
  std::unordered_map<uint64_t, MemoryHandle *> memories;
  std::unordered_map<void *, MemoryHandle *> memories_by_data;
};

struct ModuleHandle {
  ContextState *state = nullptr;
  wasm_module_t *module = nullptr;
  uint64_t id = 0;
};

struct HostFunction {
  JSCContext *context = nullptr;
  JSCValue *function = nullptr;
  std::vector<wasm_valkind_t> parameters;
  std::vector<wasm_valkind_t> results;
};

struct InstanceHandle {
  std::atomic<uint32_t> references{1};
  ContextState *state = nullptr;
  wasm_instance_t *instance = nullptr;
  wasm_extern_vec_t imports{};
  wasm_extern_vec_t exports{};
  std::vector<std::string> export_names;
};

struct NativeMemory {
  MemoryHandle *memory = nullptr;
  InstanceHandle *instance = nullptr;
};

struct ExportFunction {
  InstanceHandle *instance = nullptr;
  wasm_func_t *function = nullptr;
  std::vector<wasm_valkind_t> parameters;
  std::vector<wasm_valkind_t> results;
};

RuntimeState g_runtime;
std::once_flag g_runtime_once;
std::atomic<uint64_t> g_next_module_id{1};
thread_local MemoryHandle *g_pending_memory = nullptr;
thread_local ContextState *g_pending_context = nullptr;

bool test_diagnostics_enabled() {
  const char *value = std::getenv("OOS_WEB_TEST_MODE");
  return value && value[0] == '1';
}

long long
elapsed_milliseconds(const std::chrono::steady_clock::time_point &started) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now() - started)
      .count();
}

void context_retain(ContextState *state) {
  state->references.fetch_add(1, std::memory_order_relaxed);
}

void context_release(ContextState *state) {
  if (state->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  if (state->module_class_anchor)
    g_object_unref(state->module_class_anchor);
  if (state->memory_class_anchor)
    g_object_unref(state->memory_class_anchor);
  if (state->instance_class_anchor)
    g_object_unref(state->instance_class_anchor);
  if (state->context)
    g_object_unref(state->context);
  delete state;
}

void instance_retain(InstanceHandle *instance) {
  instance->references.fetch_add(1, std::memory_order_relaxed);
}

void instance_release(InstanceHandle *instance) {
  if (!instance ||
      instance->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  wasm_extern_vec_delete(&instance->exports);
  wasm_extern_vec_delete(&instance->imports);
  if (instance->instance)
    wasm_instance_delete(instance->instance);
  if (instance->state)
    context_release(instance->state);
  delete instance;
}

void memory_retain(MemoryHandle *memory) {
  memory->references.fetch_add(1, std::memory_order_relaxed);
}

void memory_release(MemoryHandle *memory) {
  if (!memory ||
      memory->references.fetch_sub(1, std::memory_order_acq_rel) != 1)
    return;
  memory->state->memories.erase(memory->id);
  if (memory->data) {
    auto found = memory->state->memories_by_data.find(memory->data);
    if (found != memory->state->memories_by_data.end() &&
        found->second == memory)
      memory->state->memories_by_data.erase(found);
  }
  if (memory->buffer)
    g_object_unref(memory->buffer);
  ContextState *state = memory->state;
  delete memory;
  context_release(state);
}

MemoryHandle *allocate_jsc_memory(ContextState *state, size_t size,
                                  uint32_t maximum_pages) {
  JSCValue *typed_array =
      jsc_value_new_typed_array(state->context, JSC_TYPED_ARRAY_UINT8, size);
  if (!typed_array)
    return nullptr;
  JSCValue *buffer = jsc_value_typed_array_get_buffer(typed_array);
  g_object_unref(typed_array);
  if (!buffer)
    return nullptr;

  gsize capacity = 0;
  auto *data = static_cast<uint8_t *>(
      jsc_value_array_buffer_get_data(buffer, &capacity));
  if (capacity < size || (size && !data)) {
    g_object_unref(buffer);
    return nullptr;
  }

  context_retain(state);
  auto *memory = new MemoryHandle{};
  memory->state = state;
  memory->id = state->next_memory_id++;
  memory->data = data;
  memory->byte_length = size;
  memory->capacity = capacity;
  memory->maximum_pages = maximum_pages;
  memory->buffer = buffer;
  state->memories[memory->id] = memory;
  if (data)
    state->memories_by_data[data] = memory;
  return memory;
}

void *runtime_malloc(mem_alloc_usage_t usage, void *, unsigned int size) {
  if (usage == Alloc_For_LinearMemory) {
    if (g_pending_memory) {
      MemoryHandle *memory = g_pending_memory;
      g_pending_memory = nullptr;
      if (size <= memory->capacity) {
        memory_retain(memory);
        std::lock_guard<std::mutex> lock(g_runtime.memory_mutex);
        g_runtime.allocated_memories[memory->data] = memory;
        return memory->data;
      }
    } else if (g_pending_context) {
      uint32_t pages = static_cast<uint32_t>(
          (static_cast<size_t>(size) + kWasmPageSize - 1) / kWasmPageSize);
      MemoryHandle *memory =
          allocate_jsc_memory(g_pending_context, size, pages);
      if (memory) {
        std::lock_guard<std::mutex> lock(g_runtime.memory_mutex);
        g_runtime.allocated_memories[memory->data] = memory;
        return memory->data;
      }
    }
  }
  return std::malloc(size);
}

void *runtime_realloc(mem_alloc_usage_t usage, bool, void *, void *pointer,
                      unsigned int size) {
  if (usage == Alloc_For_LinearMemory) {
    std::lock_guard<std::mutex> lock(g_runtime.memory_mutex);
    auto it = g_runtime.allocated_memories.find(pointer);
    if (it != g_runtime.allocated_memories.end())
      return size <= it->second->capacity ? pointer : nullptr;
  }
  return std::realloc(pointer, size);
}

void runtime_free(mem_alloc_usage_t usage, void *, void *pointer) {
  if (usage == Alloc_For_LinearMemory) {
    MemoryHandle *memory = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_runtime.memory_mutex);
      auto it = g_runtime.allocated_memories.find(pointer);
      if (it != g_runtime.allocated_memories.end()) {
        memory = it->second;
        g_runtime.allocated_memories.erase(it);
      }
    }
    if (memory) {
      memory_release(memory);
      return;
    }
  }
  std::free(pointer);
}

void initialize_runtime() {
  wasm_config_t *config = wasm_config_new();
  MemAllocOption allocator{};
  allocator.allocator.malloc_func = reinterpret_cast<void *>(runtime_malloc);
  allocator.allocator.realloc_func = reinterpret_cast<void *>(runtime_realloc);
  allocator.allocator.free_func = reinterpret_cast<void *>(runtime_free);
  allocator.allocator.user_data = nullptr;
  wasm_config_set_mem_alloc_opt(config, Alloc_With_Allocator, &allocator);
  g_runtime.engine = wasm_engine_new_with_config(config);
  wasm_config_delete(config);
  if (g_runtime.engine)
    g_runtime.store = wasm_store_new(g_runtime.engine);
}

std::string wasm_name_string(const wasm_name_t *name) {
  if (!name || !name->data)
    return {};
  size_t size = name->num_elems;
  while (size && name->data[size - 1] == '\0')
    --size;
  return std::string(reinterpret_cast<const char *>(name->data), size);
}

const char *kind_name(wasm_externkind_t kind) {
  switch (kind) {
  case WASM_EXTERN_FUNC:
    return "function";
  case WASM_EXTERN_GLOBAL:
    return "global";
  case WASM_EXTERN_TABLE:
    return "table";
  case WASM_EXTERN_MEMORY:
    return "memory";
  default:
    return "unknown";
  }
}

JSCValue *undefined_value(JSCContext *context) {
  return jsc_value_new_undefined(context);
}

JSCValue *throw_error(JSCContext *context, const char *name,
                      const std::string &message) {
  jsc_context_throw_with_name(context, name, message.c_str());
  return undefined_value(context);
}

bool byte_span(JSCValue *value, const uint8_t **data, size_t *size) {
  if (jsc_value_is_array_buffer(value)) {
    gsize length = 0;
    *data = static_cast<const uint8_t *>(
        jsc_value_array_buffer_get_data(value, &length));
    *size = length;
    return *data != nullptr;
  }
  if (jsc_value_is_typed_array(value)) {
    gsize ignored = 0;
    *data = static_cast<const uint8_t *>(
        jsc_value_typed_array_get_data(value, &ignored));
    *size = jsc_value_typed_array_get_size(value);
    return *data != nullptr;
  }
  return false;
}

std::vector<wasm_valkind_t> value_kinds(const wasm_valtype_vec_t *types) {
  std::vector<wasm_valkind_t> result;
  if (!types)
    return result;
  result.reserve(types->num_elems);
  for (size_t i = 0; i < types->num_elems; ++i)
    result.push_back(wasm_valtype_kind(types->data[i]));
  return result;
}

JSCValue *wasm_to_js(JSCContext *context, const wasm_val_t &value) {
  switch (value.kind) {
  case WASM_I32:
    return jsc_value_new_number(context, value.of.i32);
  case WASM_I64:
    return jsc_value_new_number(context, static_cast<double>(value.of.i64));
  case WASM_F32:
    return jsc_value_new_number(context, value.of.f32);
  case WASM_F64:
    return jsc_value_new_number(context, value.of.f64);
  default:
    return undefined_value(context);
  }
}

bool js_to_wasm(JSCValue *value, wasm_valkind_t kind, wasm_val_t *output) {
  if (!jsc_value_is_number(value))
    return false;
  output->kind = kind;
  switch (kind) {
  case WASM_I32:
    output->of.i32 = jsc_value_to_int32(value);
    return true;
  case WASM_I64:
    output->of.i64 = static_cast<int64_t>(jsc_value_to_double(value));
    return true;
  case WASM_F32:
    output->of.f32 = static_cast<float>(jsc_value_to_double(value));
    return true;
  case WASM_F64:
    output->of.f64 = jsc_value_to_double(value);
    return true;
  default:
    return false;
  }
}

wasm_trap_t *trap_from_exception(HostFunction *host,
                                 const char *fallback_message) {
  std::string message = fallback_message;
  if (JSCException *exception = jsc_context_get_exception(host->context)) {
    char *description = jsc_exception_to_string(exception);
    if (description) {
      message = description;
      g_free(description);
    }
    jsc_context_clear_exception(host->context);
  }
  wasm_message_t wasm_message{};
  wasm_name_new(&wasm_message, message.size() + 1,
                reinterpret_cast<const wasm_byte_t *>(message.c_str()));
  wasm_trap_t *trap = wasm_trap_new(g_runtime.store, &wasm_message);
  wasm_byte_vec_delete(&wasm_message);
  return trap;
}

wasm_trap_t *call_host_function(void *environment,
                                const wasm_val_vec_t *arguments,
                                wasm_val_vec_t *results) {
  auto *host = static_cast<HostFunction *>(environment);
  const size_t result_count = host->results.size();
  if (results->size < result_count || (result_count && !results->data))
    return trap_from_exception(host,
                               "WAMR provided insufficient result storage");
  std::vector<JSCValue *> js_arguments;
  js_arguments.reserve(arguments->num_elems);
  for (size_t i = 0; i < arguments->num_elems; ++i)
    js_arguments.push_back(wasm_to_js(host->context, arguments->data[i]));

  JSCValue *return_value = jsc_value_function_callv(
      host->function, js_arguments.size(), js_arguments.data());
  for (JSCValue *argument : js_arguments)
    g_object_unref(argument);
  if (!return_value || jsc_context_get_exception(host->context)) {
    if (return_value)
      g_object_unref(return_value);
    return trap_from_exception(host, "JavaScript WebAssembly import failed");
  }

  bool valid = true;
  if (result_count == 1) {
    valid = js_to_wasm(return_value, host->results[0], &results->data[0]);
  } else if (result_count > 1) {
    valid = jsc_value_is_array(return_value);
    for (size_t i = 0; valid && i < result_count; ++i) {
      JSCValue *element =
          jsc_value_object_get_property_at_index(return_value, i);
      valid = js_to_wasm(element, host->results[i], &results->data[i]);
      g_object_unref(element);
    }
  }
  g_object_unref(return_value);
  return valid ? nullptr
               : trap_from_exception(host,
                                     "WebAssembly import returned a bad value");
}

void destroy_host_function(void *environment) {
  auto *host = static_cast<HostFunction *>(environment);
  g_object_unref(host->function);
  g_object_unref(host->context);
  delete host;
}

std::string trap_message(wasm_trap_t *trap) {
  if (!trap)
    return "WebAssembly operation failed";
  wasm_message_t message{};
  wasm_trap_message(trap, &message);
  std::string result = wasm_name_string(&message);
  wasm_byte_vec_delete(&message);
  wasm_trap_delete(trap);
  return result;
}

void destroy_module(void *pointer) {
  auto *module = static_cast<ModuleHandle *>(pointer);
  if (!module)
    return;
  if (module->module)
    wasm_module_delete(module->module);
  if (module->state)
    context_release(module->state);
  delete module;
}

void destroy_native_memory(void *pointer) {
  auto *native = static_cast<NativeMemory *>(pointer);
  if (!native)
    return;
  memory_release(native->memory);
  if (native->instance)
    instance_release(native->instance);
  delete native;
}

void destroy_instance(void *pointer) {
  instance_release(static_cast<InstanceHandle *>(pointer));
}

void destroy_export_function(void *pointer) {
  auto *function = static_cast<ExportFunction *>(pointer);
  instance_release(function->instance);
  delete function;
}

JSCValue *make_native_memory(ContextState *state, MemoryHandle *memory,
                             InstanceHandle *instance = nullptr) {
  memory_retain(memory);
  if (instance)
    instance_retain(instance);
  auto *native = new NativeMemory{memory, instance};
  return jsc_value_new_object(state->context, native, state->memory_class);
}

JSCValue *memory_buffer(NativeMemory *native) {
  MemoryHandle *memory = native->memory;
  if (!memory->buffer)
    return throw_error(memory->state->context, "WebAssembly.RuntimeError",
                       "WAMR memory has no JSC backing buffer");
  return JSC_VALUE(g_object_ref(memory->buffer));
}

double memory_id(NativeMemory *native) {
  return static_cast<double>(native->memory->id);
}

char *memory_kind(NativeMemory *) { return g_strdup("memory"); }

int memory_grow(NativeMemory *, int) {
  JSCContext *context = jsc_context_get_current();
  jsc_context_throw_with_name(context, "RangeError",
                              "WAMR WebAssembly.Memory growth is not available "
                              "in the fixed-memory profile");
  return -1;
}

JSCValue *module_type_list(ModuleHandle *module, bool imports) {
  JSCContext *context = module->state->context;
  JSCValue *array = jsc_value_new_array(context, G_TYPE_NONE);
  if (imports) {
    wasm_importtype_vec_t types{};
    wasm_module_imports(module->module, &types);
    for (size_t i = 0; i < types.num_elems; ++i) {
      JSCValue *entry = jsc_value_new_object(context, nullptr, nullptr);
      JSCValue *module_name = jsc_value_new_string(
          context,
          wasm_name_string(wasm_importtype_module(types.data[i])).c_str());
      JSCValue *name = jsc_value_new_string(
          context,
          wasm_name_string(wasm_importtype_name(types.data[i])).c_str());
      JSCValue *kind = jsc_value_new_string(
          context,
          kind_name(wasm_externtype_kind(wasm_importtype_type(types.data[i]))));
      jsc_value_object_set_property(entry, "module", module_name);
      jsc_value_object_set_property(entry, "name", name);
      jsc_value_object_set_property(entry, "kind", kind);
      jsc_value_object_set_property_at_index(array, i, entry);
      g_object_unref(module_name);
      g_object_unref(name);
      g_object_unref(kind);
      g_object_unref(entry);
    }
    wasm_importtype_vec_delete(&types);
  } else {
    wasm_exporttype_vec_t types{};
    wasm_module_exports(module->module, &types);
    for (size_t i = 0; i < types.num_elems; ++i) {
      JSCValue *entry = jsc_value_new_object(context, nullptr, nullptr);
      JSCValue *name = jsc_value_new_string(
          context,
          wasm_name_string(wasm_exporttype_name(types.data[i])).c_str());
      JSCValue *kind = jsc_value_new_string(
          context,
          kind_name(wasm_externtype_kind(wasm_exporttype_type(types.data[i]))));
      jsc_value_object_set_property(entry, "name", name);
      jsc_value_object_set_property(entry, "kind", kind);
      jsc_value_object_set_property_at_index(array, i, entry);
      g_object_unref(name);
      g_object_unref(kind);
      g_object_unref(entry);
    }
    wasm_exporttype_vec_delete(&types);
  }
  return array;
}

JSCValue *module_imports(ModuleHandle *module) {
  return module_type_list(module, true);
}

JSCValue *module_exports(ModuleHandle *module) {
  return module_type_list(module, false);
}

JSCValue *export_function_call(GPtrArray *arguments, ExportFunction *function) {
  JSCContext *context = function->instance->state->context;
  if (arguments->len != function->parameters.size())
    return throw_error(context, "TypeError",
                       "WebAssembly function arity mismatch");

  wasm_val_vec_t wasm_arguments{};
  wasm_val_vec_t wasm_results{};
  wasm_val_vec_new_uninitialized(&wasm_arguments, arguments->len);
  wasm_val_vec_new_uninitialized(&wasm_results, function->results.size());
  bool valid = true;
  for (size_t i = 0; i < arguments->len; ++i) {
    valid = js_to_wasm(JSC_VALUE(g_ptr_array_index(arguments, i)),
                       function->parameters[i], &wasm_arguments.data[i]);
    if (!valid)
      break;
  }
  if (!valid) {
    wasm_val_vec_delete(&wasm_arguments);
    wasm_val_vec_delete(&wasm_results);
    return throw_error(context, "TypeError",
                       "WebAssembly function argument has the wrong type");
  }

  wasm_trap_t *trap =
      wasm_func_call(function->function, &wasm_arguments, &wasm_results);
  wasm_val_vec_delete(&wasm_arguments);
  if (trap) {
    std::string message = trap_message(trap);
    wasm_val_vec_delete(&wasm_results);
    return throw_error(context, "WebAssembly.RuntimeError", message);
  }

  JSCValue *result = nullptr;
  if (wasm_results.num_elems == 0) {
    result = undefined_value(context);
  } else if (wasm_results.num_elems == 1) {
    result = wasm_to_js(context, wasm_results.data[0]);
  } else {
    result = jsc_value_new_array(context, G_TYPE_NONE);
    for (size_t i = 0; i < wasm_results.num_elems; ++i) {
      JSCValue *element = wasm_to_js(context, wasm_results.data[i]);
      jsc_value_object_set_property_at_index(result, i, element);
      g_object_unref(element);
    }
  }
  wasm_val_vec_delete(&wasm_results);
  return result;
}

JSCValue *instance_exports(InstanceHandle *instance) {
  ContextState *state = instance->state;
  JSCValue *object = jsc_value_new_object(state->context, nullptr, nullptr);

  for (size_t i = 0; i < instance->exports.num_elems; ++i) {
    wasm_extern_t *external = instance->exports.data[i];
    const std::string &name = instance->export_names[i];
    JSCValue *value = nullptr;
    switch (wasm_extern_kind(external)) {
    case WASM_EXTERN_FUNC: {
      wasm_func_t *function = wasm_extern_as_func(external);
      wasm_functype_t *type = wasm_func_type(function);
      auto *exported = new ExportFunction{
          instance, function, value_kinds(wasm_functype_params(type)),
          value_kinds(wasm_functype_results(type))};
      wasm_functype_delete(type);
      instance_retain(instance);
      value = jsc_value_new_function_variadic(
          state->context, name.c_str(), G_CALLBACK(export_function_call),
          exported, destroy_export_function, JSC_TYPE_VALUE);
      break;
    }
    case WASM_EXTERN_MEMORY: {
      wasm_memory_t *wasm_memory = wasm_extern_as_memory(external);
      void *data = wasm_memory_data(wasm_memory);
      auto existing = state->memories_by_data.find(data);
      MemoryHandle *memory = nullptr;
      if (existing != state->memories_by_data.end()) {
        memory = existing->second;
        wasm_memorytype_t *type = wasm_memory_type(wasm_memory);
        const wasm_limits_t *limits = wasm_memorytype_limits(type);
        memory->byte_length = wasm_memory_data_size(wasm_memory);
        memory->maximum_pages = limits->max;
        wasm_memorytype_delete(type);
      } else {
        wasm_memorytype_t *type = wasm_memory_type(wasm_memory);
        const wasm_limits_t *limits = wasm_memorytype_limits(type);
        context_retain(state);
        memory = new MemoryHandle{};
        memory->state = state;
        memory->id = state->next_memory_id++;
        memory->data = static_cast<uint8_t *>(data);
        memory->byte_length = wasm_memory_data_size(wasm_memory);
        memory->capacity = memory->byte_length;
        memory->maximum_pages = limits->max;
        state->memories[memory->id] = memory;
        state->memories_by_data[data] = memory;
        wasm_memorytype_delete(type);
      }
      value = make_native_memory(state, memory, instance);
      break;
    }
    default:
      value = undefined_value(state->context);
      break;
    }
    jsc_value_object_set_property(object, name.c_str(), value);
    g_object_unref(value);
  }
  return object;
}

JSCValue *module_instantiate(ModuleHandle *module, JSCValue *imports_object) {
  ContextState *state = module->state;
  if (!jsc_value_is_object(imports_object) &&
      !jsc_value_is_undefined(imports_object))
    return throw_error(state->context, "TypeError",
                       "WebAssembly imports must be an object");

  wasm_importtype_vec_t import_types{};
  wasm_module_imports(module->module, &import_types);
  const size_t import_count = import_types.num_elems;
  std::vector<wasm_extern_t *> imports;
  imports.reserve(import_types.num_elems);
  MemoryHandle *pending_memory = nullptr;

  for (size_t i = 0; i < import_types.num_elems; ++i) {
    wasm_importtype_t *import_type = import_types.data[i];
    std::string module_name =
        wasm_name_string(wasm_importtype_module(import_type));
    std::string field_name =
        wasm_name_string(wasm_importtype_name(import_type));
    JSCValue *namespace_object =
        jsc_value_object_get_property(imports_object, module_name.c_str());
    JSCValue *import_value = jsc_value_is_object(namespace_object)
                                 ? jsc_value_object_get_property(
                                       namespace_object, field_name.c_str())
                                 : undefined_value(state->context);
    const wasm_externtype_t *external_type = wasm_importtype_type(import_type);
    wasm_extern_t *external = nullptr;

    if (wasm_externtype_kind(external_type) == WASM_EXTERN_FUNC &&
        jsc_value_is_function(import_value)) {
      const wasm_functype_t *function_type =
          wasm_externtype_as_functype_const(external_type);
      auto *host =
          new HostFunction{JSC_CONTEXT(g_object_ref(state->context)),
                           JSC_VALUE(g_object_ref(import_value)),
                           value_kinds(wasm_functype_params(function_type)),
                           value_kinds(wasm_functype_results(function_type))};
      wasm_functype_t *type_copy = wasm_functype_copy(function_type);
      wasm_func_t *function =
          wasm_func_new_with_env(g_runtime.store, type_copy, call_host_function,
                                 host, destroy_host_function);
      external = wasm_func_as_extern(function);
    } else if (wasm_externtype_kind(external_type) == WASM_EXTERN_MEMORY &&
               jsc_value_is_object(import_value)) {
      JSCValue *native =
          jsc_value_object_get_property(import_value, "__oosWamrNativeMemory");
      JSCValue *id_value = jsc_value_is_object(native)
                               ? jsc_value_object_get_property(native, "id")
                               : undefined_value(state->context);
      uint64_t id = static_cast<uint64_t>(jsc_value_to_double(id_value));
      auto found = state->memories.find(id);
      if (found != state->memories.end()) {
        MemoryHandle *memory = found->second;
        wasm_limits_t limits{
            static_cast<uint32_t>(memory->byte_length / kWasmPageSize),
            memory->maximum_pages};
        wasm_memorytype_t *type = wasm_memorytype_new(&limits);
        wasm_memory_t *wasm_memory = wasm_memory_new(g_runtime.store, type);
        wasm_memorytype_delete(type);
        external = wasm_memory_as_extern(wasm_memory);
        pending_memory = memory;
      }
      g_object_unref(id_value);
      g_object_unref(native);
    }

    g_object_unref(import_value);
    g_object_unref(namespace_object);
    if (!external) {
      for (wasm_extern_t *created : imports)
        wasm_extern_delete(created);
      wasm_importtype_vec_delete(&import_types);
      return throw_error(state->context, "WebAssembly.LinkError",
                         "Missing or unsupported import " + module_name + "." +
                             field_name);
    }
    imports.push_back(external);
  }
  wasm_importtype_vec_delete(&import_types);

  auto *instance = new InstanceHandle{};
  context_retain(state);
  instance->state = state;
  wasm_exporttype_vec_t export_types{};
  wasm_module_exports(module->module, &export_types);
  instance->export_names.reserve(export_types.num_elems);
  for (size_t i = 0; i < export_types.num_elems; ++i) {
    instance->export_names.push_back(
        wasm_name_string(wasm_exporttype_name(export_types.data[i])));
  }
  wasm_exporttype_vec_delete(&export_types);
  wasm_extern_vec_new(&instance->imports, imports.size(), imports.data());
  wasm_trap_t *trap = nullptr;
  const auto started = std::chrono::steady_clock::now();
  g_pending_memory = pending_memory;
  g_pending_context = state;
  instance->instance = wasm_instance_new_with_args(
      g_runtime.store, module->module, &instance->imports, &trap, 32 * 1024, 0);
  g_pending_memory = nullptr;
  g_pending_context = nullptr;
  if (!instance->instance) {
    std::string message = trap_message(trap);
    if (test_diagnostics_enabled()) {
      std::fprintf(stderr,
                   "OOS WAMR Web module=%llu instantiate=fail imports=%zu "
                   "elapsed_ms=%lld error=%s\n",
                   static_cast<unsigned long long>(module->id), import_count,
                   elapsed_milliseconds(started), message.c_str());
    }
    instance_release(instance);
    return throw_error(state->context, "WebAssembly.LinkError", message);
  }
  wasm_instance_exports(instance->instance, &instance->exports);
  if (test_diagnostics_enabled()) {
    std::fprintf(stderr,
                 "OOS WAMR Web module=%llu instantiate=pass imports=%zu "
                 "exports=%zu elapsed_ms=%lld\n",
                 static_cast<unsigned long long>(module->id), import_count,
                 instance->exports.num_elems, elapsed_milliseconds(started));
  }
  return jsc_value_new_object(state->context, instance, state->instance_class);
}

JSCValue *native_compile(JSCValue *bytes, ContextState *state) {
  const uint8_t *data = nullptr;
  size_t size = 0;
  if (!byte_span(bytes, &data, &size))
    return throw_error(
        state->context, "TypeError",
        "WebAssembly source must be an ArrayBuffer or typed array");
  wasm_byte_vec_t binary{};
  wasm_byte_vec_new(&binary, size, reinterpret_cast<const wasm_byte_t *>(data));
  const uint64_t module_id =
      g_next_module_id.fetch_add(1, std::memory_order_relaxed);
  const auto started = std::chrono::steady_clock::now();
  wasm_module_t *compiled = wasm_module_new(g_runtime.store, &binary);
  wasm_byte_vec_delete(&binary);
  if (!compiled) {
    if (test_diagnostics_enabled()) {
      std::fprintf(stderr,
                   "OOS WAMR Web module=%llu compile=fail bytes=%zu "
                   "elapsed_ms=%lld\n",
                   static_cast<unsigned long long>(module_id), size,
                   elapsed_milliseconds(started));
    }
    return throw_error(state->context, "WebAssembly.CompileError",
                       "WAMR rejected the WebAssembly module");
  }
  if (test_diagnostics_enabled()) {
    std::fprintf(stderr,
                 "OOS WAMR Web module=%llu compile=pass bytes=%zu "
                 "elapsed_ms=%lld\n",
                 static_cast<unsigned long long>(module_id), size,
                 elapsed_milliseconds(started));
  }
  context_retain(state);
  auto *handle = new ModuleHandle{state, compiled, module_id};
  return jsc_value_new_object(state->context, handle, state->module_class);
}

gboolean native_validate(JSCValue *bytes, ContextState *) {
  const uint8_t *data = nullptr;
  size_t size = 0;
  if (!byte_span(bytes, &data, &size))
    return FALSE;
  wasm_byte_vec_t binary{};
  wasm_byte_vec_new(&binary, size, reinterpret_cast<const wasm_byte_t *>(data));
  bool valid = wasm_module_validate(g_runtime.store, &binary);
  wasm_byte_vec_delete(&binary);
  return valid;
}

JSCValue *native_create_memory(JSCValue *descriptor, ContextState *state) {
  if (!jsc_value_is_object(descriptor))
    return throw_error(state->context, "TypeError",
                       "WebAssembly.Memory descriptor must be an object");
  JSCValue *initial_value =
      jsc_value_object_get_property(descriptor, "initial");
  JSCValue *maximum_value =
      jsc_value_object_get_property(descriptor, "maximum");
  double initial_number = jsc_value_to_double(initial_value);
  double maximum_number = jsc_value_to_double(maximum_value);
  g_object_unref(initial_value);
  g_object_unref(maximum_value);
  if (initial_number < 0 || maximum_number < initial_number ||
      maximum_number > 65536 || initial_number != maximum_number)
    return throw_error(
        state->context, "RangeError",
        "The fixed-memory WAMR profile requires maximum to equal initial");

  uint32_t pages = static_cast<uint32_t>(initial_number);
  size_t size = static_cast<size_t>(pages) * kWasmPageSize;
  MemoryHandle *memory = allocate_jsc_memory(state, size, pages);
  if (!memory)
    return throw_error(state->context, "RangeError",
                       "Cannot allocate WebAssembly linear memory");
  JSCValue *native = make_native_memory(state, memory);
  memory_release(memory);
  return native;
}

void install_classes(ContextState *state) {
  state->module_class = jsc_context_register_class(
      state->context, "OOSWamrModule", nullptr, nullptr, destroy_module);
  jsc_class_add_method(state->module_class, "instantiate",
                       G_CALLBACK(module_instantiate), nullptr, nullptr,
                       JSC_TYPE_VALUE, 1, JSC_TYPE_VALUE);
  jsc_class_add_method(state->module_class, "imports",
                       G_CALLBACK(module_imports), nullptr, nullptr,
                       JSC_TYPE_VALUE, 0, G_TYPE_NONE);
  jsc_class_add_method(state->module_class, "exports",
                       G_CALLBACK(module_exports), nullptr, nullptr,
                       JSC_TYPE_VALUE, 0, G_TYPE_NONE);
  state->module_class_anchor = jsc_value_new_object(
      state->context, new ModuleHandle{}, state->module_class);

  state->memory_class = jsc_context_register_class(
      state->context, "OOSWamrMemory", nullptr, nullptr, destroy_native_memory);
  jsc_class_add_property(state->memory_class, "buffer", JSC_TYPE_VALUE,
                         G_CALLBACK(memory_buffer), nullptr, nullptr, nullptr);
  jsc_class_add_property(state->memory_class, "id", G_TYPE_DOUBLE,
                         G_CALLBACK(memory_id), nullptr, nullptr, nullptr);
  jsc_class_add_property(state->memory_class, "kind", G_TYPE_STRING,
                         G_CALLBACK(memory_kind), nullptr, nullptr, nullptr);
  jsc_class_add_method(state->memory_class, "grow", G_CALLBACK(memory_grow),
                       nullptr, nullptr, G_TYPE_INT, 1, G_TYPE_INT);
  state->memory_class_anchor = jsc_value_new_object(
      state->context, new NativeMemory{}, state->memory_class);

  state->instance_class = jsc_context_register_class(
      state->context, "OOSWamrInstance", nullptr, nullptr, destroy_instance);
  jsc_class_add_property(state->instance_class, "exports", JSC_TYPE_VALUE,
                         G_CALLBACK(instance_exports), nullptr, nullptr,
                         nullptr);
  state->instance_class_anchor = jsc_value_new_object(
      state->context, new InstanceHandle{}, state->instance_class);
}

constexpr const char kWebAssemblyShim[] = R"JS(
(() => {
  'use strict';
  const native = globalThis.__oosWamrNative;
  const moduleHandle = Symbol('oos.wamr.module');
  const memoryHandle = Symbol('oos.wamr.memory');
  const memoryCache = new Map();

  class CompileError extends Error { constructor(message) { super(message); this.name = 'WebAssembly.CompileError'; } }
  class LinkError extends Error { constructor(message) { super(message); this.name = 'WebAssembly.LinkError'; } }
  class RuntimeError extends Error { constructor(message) { super(message); this.name = 'WebAssembly.RuntimeError'; } }

  class Module {
    constructor(bytes) { this[moduleHandle] = native.compile(bytes); }
    static imports(module) { return module[moduleHandle].imports(); }
    static exports(module) { return module[moduleHandle].exports(); }
    static customSections() { return []; }
  }

  class Memory {
    constructor(descriptor) {
      this[memoryHandle] = native.createMemory(descriptor);
      memoryCache.set(this[memoryHandle].id, this);
      Object.defineProperty(this, '__oosWamrNativeMemory', { value: this[memoryHandle] });
    }
    static fromNative(value) {
      let wrapper = memoryCache.get(value.id);
      if (!wrapper) {
        wrapper = Object.create(Memory.prototype);
        wrapper[memoryHandle] = value;
        memoryCache.set(value.id, wrapper);
        Object.defineProperty(wrapper, '__oosWamrNativeMemory', { value });
      }
      return wrapper;
    }
    get buffer() { return this[memoryHandle].buffer; }
    grow(delta) { return this[memoryHandle].grow(delta); }
  }

  class Instance {
    constructor(module, imports = {}) {
      if (!(module instanceof Module)) throw new TypeError('Expected a WebAssembly.Module');
      const raw = module[moduleHandle].instantiate(imports).exports;
      const result = {};
      for (const name of Object.keys(raw)) {
        const value = raw[name];
        result[name] = value && value.kind === 'memory' ? Memory.fromNative(value) : value;
      }
      this.exports = Object.freeze(result);
    }
  }

  const compile = bytes => Promise.resolve().then(() => new Module(bytes));
  const instantiate = (source, imports = {}) => Promise.resolve().then(() => {
    if (source instanceof Module) return new Instance(source, imports);
    const module = new Module(source);
    return { module, instance: new Instance(module, imports) };
  });
  const responseBytes = source => Promise.resolve(source).then(response => response.arrayBuffer());
  const api = {
    Module, Instance, Memory,
    CompileError, LinkError, RuntimeError,
    validate: bytes => native.validate(bytes),
    compile,
    instantiate,
    compileStreaming: source => responseBytes(source).then(bytes => new Module(bytes)),
    instantiateStreaming: (source, imports = {}) => responseBytes(source).then(bytes => instantiate(bytes, imports))
  };
  Object.defineProperty(globalThis, 'WebAssembly', { value: Object.freeze(api), configurable: false });
  delete globalThis.__oosWamrNative;
})();
)JS";

void window_object_cleared(WebKitScriptWorld *world, WebKitWebPage *,
                           WebKitFrame *frame, gpointer) {
  std::call_once(g_runtime_once, initialize_runtime);
  JSCContext *context =
      webkit_frame_get_js_context_for_script_world(frame, world);
  if (!context || !g_runtime.store)
    return;

  auto *state = new ContextState{};
  state->context = JSC_CONTEXT(g_object_ref(context));
  install_classes(state);

  JSCValue *native = jsc_value_new_object(context, nullptr, nullptr);
  context_retain(state);
  JSCValue *compile = jsc_value_new_function(
      context, "compile", G_CALLBACK(native_compile), state,
      reinterpret_cast<GDestroyNotify>(context_release), JSC_TYPE_VALUE, 1,
      JSC_TYPE_VALUE);
  context_retain(state);
  JSCValue *validate = jsc_value_new_function(
      context, "validate", G_CALLBACK(native_validate), state,
      reinterpret_cast<GDestroyNotify>(context_release), G_TYPE_BOOLEAN, 1,
      JSC_TYPE_VALUE);
  context_retain(state);
  JSCValue *create_memory = jsc_value_new_function(
      context, "createMemory", G_CALLBACK(native_create_memory), state,
      reinterpret_cast<GDestroyNotify>(context_release), JSC_TYPE_VALUE, 1,
      JSC_TYPE_VALUE);
  jsc_value_object_set_property(native, "compile", compile);
  jsc_value_object_set_property(native, "validate", validate);
  jsc_value_object_set_property(native, "createMemory", create_memory);
  jsc_context_set_value(context, "__oosWamrNative", native);
  g_object_unref(compile);
  g_object_unref(validate);
  g_object_unref(create_memory);
  g_object_unref(native);

  JSCValue *result = jsc_context_evaluate(context, kWebAssemblyShim, -1);
  if (result)
    g_object_unref(result);
  context_release(state);
  g_object_unref(context);
}

} // namespace

extern "C" __attribute__((visibility("default"))) void
webkit_web_process_extension_initialize(WebKitWebProcessExtension *) {
  g_signal_connect(webkit_script_world_get_default(), "window-object-cleared",
                   G_CALLBACK(window_object_cleared), nullptr);
}
