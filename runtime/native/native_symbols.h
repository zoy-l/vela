#pragma once

#include <string_view>

namespace vela::runtime {

// 原生 ABI 的外部符号名由代码生成器和 JIT 绑定共同消费。
inline constexpr std::string_view kNativeDirectFieldLayoutsSymbol =
    "vela_native_v10_direct_field_layouts";
inline constexpr std::string_view kNativeFieldCachesSymbol = "vela_native_v10_field_caches";
inline constexpr std::string_view kNativeLoadGlobalSymbol = "vela_native_v10_load_global";
inline constexpr std::string_view kNativeNewArraySymbol = "vela_native_v10_new_array";
inline constexpr std::string_view kNativeNewCallableSymbol = "vela_native_v10_new_callable";
inline constexpr std::string_view kNativeNewObjectSymbol = "vela_native_v10_new_object";
inline constexpr std::string_view kNativeScalarToStringSymbol = "vela_native_v10_scalar_to_string";
inline constexpr std::string_view kNativeStoreGlobalSymbol = "vela_native_v10_store_global";
inline constexpr std::string_view kNativeMapGetSymbol = "vela_native_v11_map_get";
inline constexpr std::string_view kNativeMapSetSymbol = "vela_native_v11_map_set";
inline constexpr std::string_view kNativeStringIntMapGetSymbol =
    "vela_native_v1_string_int_map_get";
inline constexpr std::string_view kNativeStringIntMapSetSymbol =
    "vela_native_v1_string_int_map_set";
inline constexpr std::string_view kNativeAsyncCallSymbol = "vela_native_v1_async_call";
inline constexpr std::string_view kNativeAsyncErrorSymbol = "vela_native_v1_async_error";
inline constexpr std::string_view kNativeAsyncReturnSymbol = "vela_native_v1_async_return";
inline constexpr std::string_view kNativeBuiltinSymbol = "vela_native_v1_builtin";
inline constexpr std::string_view kNativeCallIndirectSymbol = "vela_native_v1_call_indirect";
inline constexpr std::string_view kNativeCallInterfaceV1Symbol = "vela_native_v1_call_interface";
inline constexpr std::string_view kNativeCatchSymbol = "vela_native_v1_catch";
inline constexpr std::string_view kNativeCompareStringsSymbol = "vela_native_v2_compare_strings";
inline constexpr std::string_view kNativeConcatSymbol = "vela_native_v1_concat";
inline constexpr std::string_view kNativeConstantSymbol = "vela_native_v1_constant";
inline constexpr std::string_view kNativeEqualSymbol = "vela_native_v1_equal";
inline constexpr std::string_view kNativeExceptionValueSymbol = "vela_native_v1_exception_value";
inline constexpr std::string_view kNativeFailureSymbol = "vela_native_v1_failure";
inline constexpr std::string_view kNativeUnrootedFailureSymbol = "vela_native_v1_unrooted_failure";
inline constexpr std::string_view kNativeFrameReleaseSymbol = "vela_native_v1_frame_release";
inline constexpr std::string_view kNativeFrameStorageSymbol = "vela_native_v1_frame_storage";
inline constexpr std::string_view kNativeHasExceptionSymbol = "vela_native_v1_has_exception";
inline constexpr std::string_view kNativeImportSymbol = "vela_native_v1_import";
inline constexpr std::string_view kNativeInterpolateSymbol = "vela_native_v1_interpolate";
inline constexpr std::string_view kNativeNewCollectionSymbol = "vela_native_v1_new_collection";
inline constexpr std::string_view kNativeNewFutureSymbol = "vela_native_v1_new_future";
inline constexpr std::string_view kNativePrintSymbol = "vela_native_v1_print";
inline constexpr std::string_view kNativeResumeOutcomeSymbol = "vela_native_v1_resume_outcome";
inline constexpr std::string_view kNativeResumeReturnSymbol = "vela_native_v1_resume_return";
inline constexpr std::string_view kNativeResuspendSymbol = "vela_native_v1_resuspend";
inline constexpr std::string_view kNativeRethrowSymbol = "vela_native_v1_rethrow";
inline constexpr std::string_view kNativeRootsEnterSymbol = "vela_native_v1_roots_enter";
inline constexpr std::string_view kNativeRootsExitSymbol = "vela_native_v1_roots_exit";
inline constexpr std::string_view kNativeReleaseHostHandlesSymbol =
    "vela_native_v1_release_host_handles";
inline constexpr std::string_view kNativeBorrowStringViewSymbol =
    "vela_native_v1_borrow_string_view";
inline constexpr std::string_view kNativeSleepSymbol = "vela_native_v1_sleep";
inline constexpr std::string_view kNativeSuspendSymbol = "vela_native_v1_suspend";
inline constexpr std::string_view kNativeScalarAwaitReturnSymbol =
    "vela_native_v1_scalar_await_return";
inline constexpr std::string_view kNativeThrowSymbol = "vela_native_v1_throw";
inline constexpr std::string_view kNativeTypeTestSymbol = "vela_native_v1_type_test";
inline constexpr std::string_view kNativeWriteBarrierSymbol = "vela_native_v1_write_barrier";
inline constexpr std::string_view kNativeInitializeGlobalSymbol =
    "vela_native_v2_initialize_global";
inline constexpr std::string_view kNativeLeafFailureSymbol = "vela_native_v2_leaf_failure";
inline constexpr std::string_view kNativeCallDeclarationSymbol = "vela_native_v3_call_declaration";
inline constexpr std::string_view kNativeCallInterfaceV3Symbol = "vela_native_v3_call_interface";
inline constexpr std::string_view kNativeCaptureReceiverSymbol = "vela_native_v3_capture_receiver";
inline constexpr std::string_view kNativeInitializationEnterSymbol =
    "vela_native_v3_initialization_enter";
inline constexpr std::string_view kNativeInitializationExitSymbol =
    "vela_native_v3_initialization_exit";
inline constexpr std::string_view kNativeLoadFieldSymbol = "vela_native_v3_load_field";
inline constexpr std::string_view kNativeSafepointSymbol = "vela_native_v3_safepoint";
inline constexpr std::string_view kNativeStoreFieldSymbol = "vela_native_v3_store_field";
inline constexpr std::string_view kNativeCanOptimizeSymbol = "vela_native_v4_can_optimize";
inline constexpr std::string_view kNativeRecoverSymbol = "vela_native_v4_recover";
inline constexpr std::string_view kNativeStoreMaterializedFieldSymbol =
    "vela_native_v7_store_materialized_field";
inline constexpr std::string_view kNativeScalarTextLengthSymbol =
    "vela_native_v9_scalar_text_length";

}  // namespace vela::runtime
