#pragma once

#ifdef __cplusplus
#include <cstdint>
namespace vela {
class HandleScope;
namespace runtime {
class Isolate;
class NativeInstance;
}
}
using VelaAotIsolate = vela::runtime::Isolate;
using VelaAotInstance = vela::runtime::NativeInstance;
using VelaAotScope = vela::HandleScope;
struct VelaAotPersistent;
struct VelaAotError;
using VelaAotStatus = uint8_t;
extern "C" {
#else
#include <stdint.h>
typedef struct VelaAotIsolate VelaAotIsolate;
typedef struct VelaAotInstance VelaAotInstance;
typedef struct VelaAotScope VelaAotScope;
typedef struct VelaAotPersistent VelaAotPersistent;
typedef struct VelaAotError VelaAotError;
typedef struct VelaAotLocal VelaAotLocal;
typedef struct VelaAotValue VelaAotValue;
typedef struct VelaAotTraceFrame VelaAotTraceFrame;
typedef uint8_t VelaAotStatus;
#endif

#define VELA_AOT_RETURNED UINT8_C(0)
#define VELA_AOT_NULL UINT8_C(0)
#define VELA_AOT_BOOLEAN UINT8_C(1)
#define VELA_AOT_INTEGER UINT8_C(2)
#define VELA_AOT_DOUBLE UINT8_C(3)
#define VELA_AOT_LOCAL UINT8_C(4)

// 不透明借用引用；所属作用域必须存活，复制引用不创建根。
struct VelaAotLocal {
  const VelaAotScope* scope;
  void* slot;
};

struct VelaAotValue {
  uint64_t payload;
  uint8_t kind;
  uint8_t reserved_byte;
  uint16_t reserved_half;
  uint32_t reserved_word;
  VelaAotLocal local;
};

struct VelaAotTraceFrame {
  const char* function;
  uint64_t function_size;
  const char* source_path;
  uint64_t source_path_size;
  uint64_t instruction;
  uint32_t line;
  uint32_t column;
};

// 所有操作及释放都在 Isolate 所属线程执行。
VelaAotIsolate* vela_aot_v2_isolate_create(void);
void vela_aot_v2_isolate_release(VelaAotIsolate* isolate);
VelaAotStatus vela_aot_v2_scope_create(VelaAotIsolate* isolate, VelaAotScope** scope);
void vela_aot_v2_scope_release(VelaAotScope* scope);
VelaAotStatus vela_aot_v2_persist(VelaAotLocal local, VelaAotPersistent** persistent);
VelaAotStatus vela_aot_v2_localize(VelaAotScope* scope,
                                   const VelaAotPersistent* persistent,
                                   VelaAotLocal* local);
void vela_aot_v2_persistent_release(VelaAotPersistent* persistent);
VelaAotStatus vela_aot_v2_instantiate(VelaAotIsolate* isolate,
                                      const void* image,
                                      VelaAotInstance** instance,
                                      VelaAotError** error);
void vela_aot_v2_instance_release(VelaAotInstance* instance);
VelaAotStatus vela_aot_v2_invoke(VelaAotScope* scope,
                                 VelaAotInstance* instance,
                                 const void* image,
                                 uint32_t function,
                                 const VelaAotValue* arguments,
                                 uint64_t count,
                                 VelaAotValue* result,
                                 VelaAotError** error);
VelaAotStatus vela_aot_v2_string_create(VelaAotScope* scope,
                                        const char* text,
                                        uint64_t size,
                                        VelaAotLocal* result);
VelaAotStatus vela_aot_v2_string_view(VelaAotLocal string, const char** text, uint64_t* size);
VelaAotStatus vela_aot_v2_future_wait(VelaAotScope* scope,
                                      VelaAotLocal future,
                                      uint8_t wait_for_host,
                                      VelaAotValue* result,
                                      VelaAotError** error);
const char* vela_aot_v2_status_code(VelaAotStatus status);
void vela_aot_v2_error_release(VelaAotError* error);
VelaAotStatus vela_aot_v2_error_payload(VelaAotScope* scope,
                                        const VelaAotError* error,
                                        VelaAotLocal* payload);
VelaAotStatus vela_aot_v2_error_message(const VelaAotError* error,
                                        const char** text,
                                        uint64_t* size);
VelaAotStatus vela_aot_v2_error_frame(const VelaAotError* error,
                                      uint64_t index,
                                      VelaAotTraceFrame* frame);

#ifdef __cplusplus
}
#endif
