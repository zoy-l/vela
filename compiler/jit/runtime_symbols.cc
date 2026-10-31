#include "compiler/jit/runtime_symbols.h"

#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h>
#include <llvm/Support/Error.h>

#include "runtime/native/native_abi.h"
#include "runtime/native/native_async.h"
#include "runtime/native/native_call.h"
#include "runtime/native/native_control.h"
#include "runtime/native/native_deoptimization.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_function.h"
#include "runtime/native/native_operations.h"

namespace vela::compiler::jit {

llvm::Error DefineRuntimeSymbols(llvm::orc::LLJIT& jit) {
  llvm::orc::SymbolMap symbols;
  const auto bind = [&](llvm::StringRef name, auto* entry) {
    symbols.try_emplace(jit.mangleAndIntern(name), llvm::orc::ExecutorAddr::fromPtr(entry),
                        llvm::JITSymbolFlags::Exported);
  };
  // 静态 SDK 的注册入口不依赖宿主导出其全部符号；同一张表负责强制链接与 ORC 解析。
  bind("llvm_orc_registerJITLoaderGDBAllocAction", &llvm_orc_registerJITLoaderGDBAllocAction);
  bind("llvm_orc_registerJITLoaderGDBWrapper", &llvm_orc_registerJITLoaderGDBWrapper);
  bind("llvm_orc_registerEHFrameSectionAllocAction", &llvm_orc_registerEHFrameSectionAllocAction);
  bind("llvm_orc_deregisterEHFrameSectionAllocAction",
       &llvm_orc_deregisterEHFrameSectionAllocAction);
  bind("vela_native_v3_safepoint", &runtime::vela_native_v3_safepoint);
  bind("vela_native_v4_can_optimize", &runtime::vela_native_v4_can_optimize);
  bind("vela_native_v4_recover", &runtime::vela_native_v4_recover);
  bind("vela_native_v3_host_enter", &runtime::vela_native_v3_host_enter);
  bind("vela_native_v3_host_exit", &runtime::vela_native_v3_host_exit);
  bind("vela_native_v3_initialization_enter", &runtime::vela_native_v3_initialization_enter);
  bind("vela_native_v3_initialization_exit", &runtime::vela_native_v3_initialization_exit);
  bind("vela_native_v1_frame_storage", &runtime::vela_native_v1_frame_storage);
  bind("vela_native_v1_frame_release", &runtime::vela_native_v1_frame_release);
  bind("vela_native_v1_roots_enter", &runtime::vela_native_v1_roots_enter);
  bind("vela_native_v1_roots_exit", &runtime::vela_native_v1_roots_exit);
  bind("vela_native_v2_leaf_failure", &runtime::vela_native_v2_leaf_failure);
  bind("vela_native_v1_failure", &runtime::vela_native_v1_failure);
  bind("vela_native_v1_has_exception", &runtime::vela_native_v1_has_exception);
  bind("vela_native_v1_catch", &runtime::vela_native_v1_catch);
  bind("vela_native_v1_exception_value", &runtime::vela_native_v1_exception_value);
  bind("vela_native_v1_throw", &runtime::vela_native_v1_throw);
  bind("vela_native_v1_rethrow", &runtime::vela_native_v1_rethrow);
  bind("vela_native_v1_new_future", &runtime::vela_native_v1_new_future);
  bind("vela_native_v1_sleep", &runtime::vela_native_v1_sleep);
  bind("vela_native_v1_async_call", &runtime::vela_native_v1_async_call);
  bind("vela_native_v1_async_return", &runtime::vela_native_v1_async_return);
  bind("vela_native_v1_async_error", &runtime::vela_native_v1_async_error);
  bind("vela_native_v1_suspend", &runtime::vela_native_v1_suspend);
  bind("vela_native_v1_resuspend", &runtime::vela_native_v1_resuspend);
  bind("vela_native_v1_resume_return", &runtime::vela_native_v1_resume_return);
  bind("vela_native_v1_resume_outcome", &runtime::vela_native_v1_resume_outcome);
  bind("vela_native_v1_call_indirect", &runtime::vela_native_v1_call_indirect);
  bind("vela_native_v1_call_interface", &runtime::vela_native_v1_call_interface);
  bind("vela_native_v3_call_declaration", &runtime::vela_native_v3_call_declaration);
  bind("vela_native_v3_call_interface", &runtime::vela_native_v3_call_interface);
  bind("vela_native_v3_capture_receiver", &runtime::vela_native_v3_capture_receiver);
  bind("vela_native_v3_load_global", &runtime::vela_native_v3_load_global);
  bind("vela_native_v3_store_global", &runtime::vela_native_v3_store_global);
  bind("vela_native_v3_load_field", &runtime::vela_native_v3_load_field);
  bind("vela_native_v3_store_field", &runtime::vela_native_v3_store_field);
  bind("vela_native_v7_store_materialized_field",
       &runtime::vela_native_v7_store_materialized_field);
  bind("vela_native_v1_import", &runtime::vela_native_v1_import);
  bind("vela_native_v1_constant", &runtime::vela_native_v1_constant);
  bind("vela_native_v1_type_test", &runtime::vela_native_v1_type_test);
  bind("vela_native_v2_initialize_global", &runtime::vela_native_v2_initialize_global);
  bind("vela_native_v1_new_instance", &runtime::vela_native_v1_new_instance);
  bind("vela_native_v1_new_array", &runtime::vela_native_v1_new_array);
  bind("vela_native_v1_new_collection", &runtime::vela_native_v1_new_collection);
  bind("vela_native_v1_write_barrier", &runtime::vela_native_v1_write_barrier);
  bind("vela_native_v1_array_grow", &runtime::vela_native_v1_array_grow);
  bind("vela_native_v1_builtin", &runtime::vela_native_v1_builtin);
  bind("vela_native_v1_concat", &runtime::vela_native_v1_concat);
  bind("vela_native_v1_interpolate", &runtime::vela_native_v1_interpolate);
  bind("vela_native_v1_compare_strings", &runtime::vela_native_v1_compare_strings);
  bind("vela_native_v1_equal", &runtime::vela_native_v1_equal);
  bind("vela_native_v1_print", &runtime::vela_native_v1_print);
  return jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)));
}

}  // namespace vela::compiler::jit
