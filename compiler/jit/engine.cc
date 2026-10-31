#include "compiler/jit/engine.h"

#include <memory>
#include <utility>

#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/Layer.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/TargetParser/Triple.h>

#include "compiler/jit/platform_support.h"
#include "compiler/jit/runtime_symbols.h"
#include "compiler/native/code_generator.h"

namespace vela::compiler::jit {

llvm::Expected<std::unique_ptr<llvm::orc::LLJIT>> CreateEngine(const llvm::Triple& triple) {
  native::InitializeTarget();
  llvm::orc::JITTargetMachineBuilder target(triple);
  target.setCPU("generic");
  llvm::orc::LLJITBuilder builder;
  builder.setJITTargetMachineBuilder(std::move(target));
  builder.setObjectLinkingLayerCreator(
      [](llvm::orc::ExecutionSession& session)
          -> llvm::Expected<std::unique_ptr<llvm::orc::ObjectLayer>> {
        return std::make_unique<llvm::orc::ObjectLinkingLayer>(session);
      });
  auto created = builder.create();
  if (!created) {
    return created.takeError();
  }
  if (auto error = DefineRuntimeSymbols(**created)) {
    return std::move(error);
  }
  if (auto error = InstallPlatformSupport(**created)) {
    return std::move(error);
  }
  return std::move(*created);
}

}  // namespace vela::compiler::jit
