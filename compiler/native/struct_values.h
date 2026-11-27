#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <absl/functional/function_ref.h>

#include "base/declaration.h"
#include "compiler/ir/ir.h"
#include "compiler/native/native_values.h"

namespace vela::compiler::native {

// 扁平槽只包含标量、普通引用或 dynamic；嵌套可空值先存独立存在位。
llvm::Value* ExtractStructField(Builder& builder,
                                llvm::Value* value,
                                const SourceType& owner,
                                std::uint32_t field);
llvm::Value* InsertStructField(Builder& builder,
                               llvm::Value* value,
                               const SourceType& owner,
                               std::uint32_t field,
                               llvm::Value* replacement);
std::size_t ManagedSlotCount(const SourceType& type);
llvm::Value* EncodeStructSlot(Builder& builder, llvm::Value* value, const SourceType& type);
void ForEachStructReference(Builder& builder,
                            llvm::Value* value,
                            const SourceType& type,
                            absl::FunctionRef<void(const SourceType&, llvm::Value*)> visit);
void StoreStructBoxFields(Builder& builder,
                          llvm::Value* object,
                          const SourceType& type,
                          llvm::Value* value);
// 装箱入口负责跨分配保根并返回状态；调用者沿所属帧的失败路径处理状态。
llvm::Value* BoxStructValue(Builder& builder,
                            const ir::Program& program,
                            llvm::Value* execution,
                            llvm::Value* value,
                            const SourceType& type,
                            llvm::Value* output);
llvm::Value* UnboxStructValue(Builder& builder, llvm::Value* record, const SourceType& type);

// 描述符及其上下文由调用者原生帧持有，仅在同步调用链中借用。
llvm::Value* LocalStructPlace(Builder& builder,
                              llvm::Value* storage,
                              llvm::Value* roots,
                              const SourceType& type);
llvm::Value* BoxedStructPlace(Builder& builder, llvm::Value* object, const SourceType& type);
llvm::Value* CapturedStructPlace(Builder& builder, llvm::Value* cell, const SourceType& type);
llvm::Value* GlobalStructPlace(Builder& builder,
                               llvm::Value* program,
                               std::uint32_t slot,
                               const SourceType& type);
llvm::Value* BoxSlotStructPlace(Builder& builder, llvm::Value* storage, const SourceType& type);
llvm::Value* ObjectFieldStructPlace(Builder& builder,
                                    llvm::Value* object,
                                    const SourceType& type,
                                    std::uint32_t slot,
                                    std::optional<DeclarationId> field);
llvm::Value* ProjectStructPlace(Builder& builder,
                                llvm::Value* parent,
                                const SourceType& type,
                                std::uint32_t field);
llvm::Value* RequiredStructPlace(Builder& builder, llvm::Value* parent, const SourceType& type);
llvm::Value* ArrayStructPlace(Builder& builder,
                              llvm::Value* array,
                              llvm::Value* index,
                              const SourceType& type);
llvm::Value* ReadStructPlace(Builder& builder,
                             llvm::Value* execution,
                             llvm::Value* place,
                             llvm::Value* output);
llvm::Value* WriteStructPlace(Builder& builder,
                              llvm::Value* execution,
                              llvm::Value* place,
                              llvm::Value* input);

}  // namespace vela::compiler::native
