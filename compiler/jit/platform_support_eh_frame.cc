#include "compiler/jit/platform_support.h"

#include <memory>
#include <utility>

#include <llvm/ADT/StringRef.h>
#include <llvm/ExecutionEngine/JITSymbol.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include <llvm/ExecutionEngine/Orc/Debugging/DebuggerSupportPlugin.h>
#include <llvm/ExecutionEngine/Orc/Debugging/ELFDebugObjectPlugin.h>
#include <llvm/ExecutionEngine/Orc/EHFrameRegistrationPlugin.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/JITLoaderGDB.h>
#include <llvm/ExecutionEngine/Orc/TargetProcess/RegisterEHFrames.h>
#include <llvm/Support/Error.h>
#include "compiler/jit/engine.h"

namespace vela::compiler::jit {

llvm::Error InstallPlatformSupport(CodeLinker& jit) {
  llvm::orc::SymbolMap symbols;
  const auto bind = [&](llvm::StringRef name, auto* entry) {
    symbols.try_emplace(jit.mangleAndIntern(name), llvm::orc::ExecutorAddr::fromPtr(entry),
                        llvm::JITSymbolFlags::Exported);
  };
  // 静态 SDK 的注册入口不依赖宿主导出其全部符号；同一张表负责强制链接与 ORC 解析。
  bind("llvm_orc_registerJITLoaderGDBAllocAction", &llvm_orc_registerJITLoaderGDBAllocAction);
  bind("llvm_orc_registerEHFrameSectionAllocAction", &llvm_orc_registerEHFrameSectionAllocAction);
  bind("llvm_orc_deregisterEHFrameSectionAllocAction",
       &llvm_orc_deregisterEHFrameSectionAllocAction);
  if (auto error = jit.getMainJITDylib().define(llvm::orc::absoluteSymbols(std::move(symbols)))) {
    return error;
  }
  auto frames = llvm::orc::EHFrameRegistrationPlugin::Create(jit.getExecutionSession());
  if (!frames) {
    return frames.takeError();
  }
  auto& objects = jit.getObjLinkingLayer();
  objects.addPlugin(std::move(*frames));
  if (jit.getTargetTriple().isOSBinFormatMachO()) {
    auto debugger = llvm::orc::GDBJITDebugInfoRegistrationPlugin::Create(
        jit.getExecutionSession(), jit.getMainJITDylib(), jit.getTargetTriple());
    if (!debugger) {
      return debugger.takeError();
    }
    objects.addPlugin(std::move(*debugger));
    return llvm::Error::success();
  }
  auto error = llvm::Error::success();
  objects.addPlugin(std::make_shared<llvm::orc::ELFDebugObjectPlugin>(jit.getExecutionSession(),
                                                                      false, true, error));
  return error;
}

}  // namespace vela::compiler::jit
