#include "compiler/jit/platform_support.h"

#include <memory>
#include <utility>

#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupport.h>
#include <llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>

namespace vela::compiler::jit {

llvm::Error InstallPlatformSupport(llvm::orc::LLJIT& jit) {
  auto frames = llvm::orc::EHFrameRegistrationPlugin::Create(jit.getExecutionSession());
  if (!frames) {
    return frames.takeError();
  }
  llvm::cast<llvm::orc::ObjectLinkingLayer>(jit.getObjLinkingLayer()).addPlugin(std::move(*frames));
  return llvm::orc::enableDebuggerSupport(jit);
}

}  // namespace vela::compiler::jit
