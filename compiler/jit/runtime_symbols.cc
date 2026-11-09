#include "compiler/jit/runtime_symbols.h"
#include "runtime/native/native_symbols.h"

#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
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
  bind(runtime::kNativeSafepointSymbol, &runtime::vela_native_v3_safepoint);
  bind(runtime::kNativeCanOptimizeSymbol, &runtime::vela_native_v4_can_optimize);
  bind(runtime::kNativeRecoverSymbol, &runtime::vela_native_v4_recover);
  bind(runtime::kNativeFrameStorageSymbol, &runtime::vela_native_v1_frame_storage);
  bind(runtime::kNativeFrameReleaseSymbol, &runtime::vela_native_v1_frame_release);
  bind(runtime::kNativeRootsEnterSymbol, &runtime::vela_native_v1_roots_enter);
  bind(runtime::kNativeRootsExitSymbol, &runtime::vela_native_v1_roots_exit);
  bind(runtime::kNativeLeafFailureSymbol, &runtime::vela_native_v2_leaf_failure);
  bind(runtime::kNativeFailureSymbol, &runtime::vela_native_v1_failure);
  bind(runtime::kNativeHasExceptionSymbol, &runtime::vela_native_v1_has_exception);
  bind(runtime::kNativeCatchSymbol, &runtime::vela_native_v1_catch);
  bind(runtime::kNativeExceptionValueSymbol, &runtime::vela_native_v1_exception_value);
  bind(runtime::kNativeThrowSymbol, &runtime::vela_native_v1_throw);
  bind(runtime::kNativeRethrowSymbol, &runtime::vela_native_v1_rethrow);
  bind(runtime::kNativeNewFutureSymbol, &runtime::vela_native_v1_new_future);
  bind(runtime::kNativeSleepSymbol, &runtime::vela_native_v1_sleep);
  bind(runtime::kNativeAsyncCallSymbol, &runtime::vela_native_v1_async_call);
  bind(runtime::kNativeAsyncReturnSymbol, &runtime::vela_native_v1_async_return);
  bind(runtime::kNativeAsyncErrorSymbol, &runtime::vela_native_v1_async_error);
  bind(runtime::kNativeSuspendSymbol, &runtime::vela_native_v1_suspend);
  bind(runtime::kNativeResuspendSymbol, &runtime::vela_native_v1_resuspend);
  bind(runtime::kNativeResumeReturnSymbol, &runtime::vela_native_v1_resume_return);
  bind(runtime::kNativeResumeOutcomeSymbol, &runtime::vela_native_v1_resume_outcome);
  bind(runtime::kNativeCallIndirectSymbol, &runtime::vela_native_v1_call_indirect);
  bind(runtime::kNativeCallInterfaceV1Symbol, &runtime::vela_native_v1_call_interface);
  bind(runtime::kNativeCallDeclarationSymbol, &runtime::vela_native_v3_call_declaration);
  bind(runtime::kNativeCallInterfaceV3Symbol, &runtime::vela_native_v3_call_interface);
  bind(runtime::kNativeCaptureReceiverSymbol, &runtime::vela_native_v3_capture_receiver);
  bind(runtime::kNativeLoadGlobalSymbol, &runtime::vela_native_v10_load_global);
  bind(runtime::kNativeStoreGlobalSymbol, &runtime::vela_native_v10_store_global);
  bind(runtime::kNativeLoadFieldSymbol, &runtime::vela_native_v3_load_field);
  bind(runtime::kNativeFieldCachesSymbol, &runtime::vela_native_v10_field_caches);
  bind(runtime::kNativeDirectFieldLayoutsSymbol, &runtime::vela_native_v10_direct_field_layouts);
  bind(runtime::kNativeScalarTextLengthSymbol, &runtime::vela_native_v9_scalar_text_length);
  bind(runtime::kNativeScalarToStringSymbol, &runtime::vela_native_v10_scalar_to_string);
  bind(runtime::kNativeMapGetSymbol, &runtime::vela_native_v11_map_get);
  bind(runtime::kNativeMapSetSymbol, &runtime::vela_native_v11_map_set);
  bind(runtime::kNativeStoreFieldSymbol, &runtime::vela_native_v3_store_field);
  bind(runtime::kNativeStoreMaterializedFieldSymbol,
       &runtime::vela_native_v7_store_materialized_field);
  bind(runtime::kNativeImportSymbol, &runtime::vela_native_v1_import);
  bind(runtime::kNativeTypeTestSymbol, &runtime::vela_native_v1_type_test);
  bind(runtime::kNativeInitializeGlobalSymbol, &runtime::vela_native_v2_initialize_global);
  bind(runtime::kNativeNewObjectSymbol, &runtime::vela_native_v10_new_object);
  bind(runtime::kNativeNewCallableSymbol, &runtime::vela_native_v10_new_callable);
  bind(runtime::kNativeNewArraySymbol, &runtime::vela_native_v10_new_array);
  bind(runtime::kNativeNewCollectionSymbol, &runtime::vela_native_v1_new_collection);
  bind(runtime::kNativeWriteBarrierSymbol, &runtime::vela_native_v1_write_barrier);
  bind(runtime::kNativeBuiltinSymbol, &runtime::vela_native_v1_builtin);
  bind(runtime::kNativeConcatSymbol, &runtime::vela_native_v1_concat);
  bind(runtime::kNativeInterpolateSymbol, &runtime::vela_native_v1_interpolate);
  bind(runtime::kNativeCompareStringsSymbol, &runtime::vela_native_v2_compare_strings);
  bind(runtime::kNativeEqualSymbol, &runtime::vela_native_v1_equal);
  bind(runtime::kNativePrintSymbol, &runtime::vela_native_v1_print);
  return jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)));
}

}  // namespace vela::compiler::jit
