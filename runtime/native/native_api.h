#pragma once

#ifdef __cplusplus
#include <cstdint>
#else
#include <stdint.h>
#endif

#ifdef __cplusplus
namespace vela::runtime {
class Isolate;
class NativeInstance;
}
using VelaAotIsolate = vela::runtime::Isolate;
using VelaAotInstance = vela::runtime::NativeInstance;
extern "C" {
#endif

struct VelaAotHandle;
#ifdef __cplusplus
using VelaAotStatus = uint8_t;
#else
typedef struct VelaAotIsolate VelaAotIsolate;
typedef struct VelaAotInstance VelaAotInstance;
typedef struct VelaAotHandle VelaAotHandle;
typedef uint8_t VelaAotStatus;
#endif

#define VELA_AOT_RETURNED UINT8_C(0)
#define VELA_AOT_NULL UINT8_C(0)
#define VELA_AOT_BOOLEAN UINT8_C(1)
#define VELA_AOT_INTEGER UINT8_C(2)
#define VELA_AOT_DOUBLE UINT8_C(3)
#define VELA_AOT_HANDLE UINT8_C(4)

// payload 中的 HANDLE 只指向不透明根句柄，不暴露 GC 对象或 C++ 对象布局。
struct VelaAotValue {
  uint64_t payload;
  uint8_t kind;
  uint8_t reserved_byte;
  uint16_t reserved_half;
  uint32_t reserved_word;
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

#ifndef __cplusplus
typedef struct VelaAotValue VelaAotValue;
typedef struct VelaAotTraceFrame VelaAotTraceFrame;
#endif

// Isolate 覆盖全部实例、句柄和 producer 的生命周期；操作与释放均在所属线程执行。
VelaAotIsolate* vela_aot_v1_isolate_create(void);
void vela_aot_v1_isolate_release(VelaAotIsolate* isolate);
VelaAotStatus vela_aot_v1_instantiate(VelaAotIsolate* isolate,
                                      const void* image,
                                      VelaAotInstance** instance,
                                      VelaAotHandle** error);
void vela_aot_v1_instance_release(VelaAotInstance* instance);
VelaAotStatus vela_aot_v1_invoke(VelaAotInstance* instance,
                                 const void* image,
                                 uint32_t function,
                                 const VelaAotValue* arguments,
                                 uint64_t count,
                                 VelaAotValue* result,
                                 VelaAotHandle** error);
VelaAotStatus vela_aot_v1_string_create(VelaAotIsolate* isolate,
                                        const char* text,
                                        uint64_t size,
                                        VelaAotHandle** result);
VelaAotStatus vela_aot_v1_string_view(const VelaAotHandle* handle,
                                      const char** text,
                                      uint64_t* size);
void vela_aot_v1_handle_release(VelaAotHandle* handle);
// wait_for_host 为 0 时只推进可用工作，为 1 时允许等待宿主 producer。
VelaAotStatus vela_aot_v1_future_wait(VelaAotHandle* future,
                                      uint8_t wait_for_host,
                                      VelaAotValue* result,
                                      VelaAotHandle** error);
const char* vela_aot_v1_status_code(VelaAotStatus status);
VelaAotStatus vela_aot_v1_error_message(const VelaAotHandle* error,
                                        const char** text,
                                        uint64_t* size);
VelaAotStatus vela_aot_v1_error_frame(const VelaAotHandle* error,
                                      uint64_t index,
                                      VelaAotTraceFrame* frame);

#ifdef __cplusplus
}

#endif
