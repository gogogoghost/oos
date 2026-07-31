#include <wasm_c_api.h>
#include <wasm_c_api_internal.h>

#include <cassert>

int main() {
  // A module with one fixed 32 MiB memory. Repeated construction catches the
  // store-owned instance leak without depending on allocator implementation.
  constexpr wasm_byte_t module_bytes[] = {
      0x00,
      0x61,
      0x73,
      0x6d,
      0x01,
      0x00,
      0x00,
      0x00,
      0x05,
      0x06,
      0x01,
      0x01,
      static_cast<wasm_byte_t>(0x80),
      0x04,
      static_cast<wasm_byte_t>(0x80),
      0x04,
  };

  wasm_engine_t *engine = wasm_engine_new();
  assert(engine);
  wasm_store_t *store = wasm_store_new(engine);
  assert(store);

  wasm_byte_vec_t bytes{};
  wasm_byte_vec_new(&bytes, sizeof(module_bytes), module_bytes);
  wasm_module_t *module = wasm_module_new(store, &bytes);
  wasm_byte_vec_delete(&bytes);
  assert(module);
  assert(store->instances->num_elems == 0);

  for (int i = 0; i < 16; ++i) {
    wasm_trap_t *trap = nullptr;
    wasm_instance_t *instance =
        wasm_instance_new(store, module, nullptr, &trap);
    assert(instance);
    assert(!trap);
    assert(store->instances->num_elems == 1);
    wasm_instance_delete(instance);
    assert(store->instances->num_elems == 0);
  }

  wasm_store_delete(store);
  wasm_engine_delete(engine);
  return 0;
}
