#include "compiler/jit/platform_support.h"

#include <windows.h>  // IWYU pragma: keep

#include <dbghelp.h>
#include <errhandlingapi.h>
#include <minwindef.h>
#include <winnt.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <llvm/Support/thread.h>

#include <absl/container/flat_hash_map.h>

#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/ExecutorAddress.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>

namespace vela::compiler::jit {
namespace {

// DbgHelp 的不同符号会话也要求串行访问；此锁只保护该进程级 Windows API。
std::mutex& DebugHelpMutex() {
  static std::mutex mutex;
  return mutex;
}

llvm::Error PlatformError(const char* operation) {
  const auto error = GetLastError();
  return llvm::make_error<llvm::StringError>(
      std::string(operation) + " failed (Windows error " + std::to_string(error) + ')',
      llvm::inconvertibleErrorCode());
}

struct CodeSymbol final {
  std::string name;
  std::uint64_t address = 0;
  std::uint32_t size = 0;
};

struct Registration final {
  std::uint64_t base = 0;
  std::uint32_t size = 0;
  std::vector<RUNTIME_FUNCTION> frames;
  std::vector<CodeSymbol> symbols;
};

class CoffRegistrationPlugin final : public llvm::orc::ObjectLinkingLayer::Plugin {
 public:
  CoffRegistrationPlugin() = default;
  CoffRegistrationPlugin(const CoffRegistrationPlugin&) = delete;
  CoffRegistrationPlugin& operator=(const CoffRegistrationPlugin&) = delete;
  CoffRegistrationPlugin(CoffRegistrationPlugin&&) = delete;
  CoffRegistrationPlugin& operator=(CoffRegistrationPlugin&&) = delete;
  ~CoffRegistrationPlugin() override {
    const std::scoped_lock lock(DebugHelpMutex());
    for (auto& [key, registrations] : registered_) {
      for (auto& registration : registrations) {
        Release(*registration);
      }
    }
    SymCleanup(session());
  }

  llvm::Error Initialize() {
    const std::scoped_lock lock(DebugHelpMutex());
    // 独立的符号会话不接管宿主已有的 DbgHelp 注册或模块集合。
    return (SymInitialize(session(), nullptr, FALSE) != 0) ? llvm::Error::success()
                                                           : PlatformError("SymInitialize");
  }

  void modifyPassConfig(llvm::orc::MaterializationResponsibility& responsibility,
                        llvm::jitlink::LinkGraph& /*G*/,
                        llvm::jitlink::PassConfiguration& config) override {
    config.PostAllocationPasses.emplace_back(ResolveImageBase);
    config.PostFixupPasses.emplace_back([this, &responsibility](llvm::jitlink::LinkGraph& linked) {
      return PrepareRegistration(responsibility, linked);
    });
  }

  static llvm::Error ResolveImageBase(llvm::jitlink::LinkGraph& allocated) {
    auto base = std::numeric_limits<std::uint64_t>::max();
    for (const auto* block : allocated.blocks()) {
      base = std::min(base, block->getAddress().getValue());
    }
    // COFF ADDR32NB 与 .pdata 使用同一批次的基址，不能解析成宿主 EXE 的 __ImageBase。
    llvm::jitlink::Symbol* image_base = nullptr;
    for (auto* symbol : allocated.external_symbols()) {
      if (*symbol->getName() == "__ImageBase") {
        image_base = symbol;
        break;
      }
    }
    if (image_base != nullptr) {
      allocated.makeAbsolute(*image_base, llvm::orc::ExecutorAddr(base));
    }
    return llvm::Error::success();
  }

  llvm::Error PrepareRegistration(llvm::orc::MaterializationResponsibility& responsibility,
                                  llvm::jitlink::LinkGraph& linked) {
    auto registration = std::make_unique<Registration>();
    auto base = std::numeric_limits<std::uint64_t>::max();
    std::uint64_t end = 0;
    for (const auto* block : linked.blocks()) {
      const auto address = block->getAddress().getValue();
      base = std::min(base, address);
      end = std::max(end, address + block->getSize());
    }
    if (end - base > std::numeric_limits<std::uint32_t>::max()) {
      return llvm::make_error<llvm::StringError>("COFF JIT image exceeds the 32-bit RVA range",
                                                 llvm::inconvertibleErrorCode());
    }
    registration->base = base;
    registration->size = static_cast<std::uint32_t>(end - base);
    if (const auto* frames = linked.findSectionByName(".pdata")) {
      for (const auto* block : frames->blocks()) {
        const auto contents = block->getContent();
        if (contents.size() % sizeof(RUNTIME_FUNCTION) != 0) {
          return llvm::make_error<llvm::StringError>("invalid COFF JIT unwind table",
                                                     llvm::inconvertibleErrorCode());
        }
        const auto count = contents.size() / sizeof(RUNTIME_FUNCTION);
        const auto previous = registration->frames.size();
        registration->frames.resize(previous + count);
        std::memcpy(std::span(registration->frames).subspan(previous).data(), contents.data(),
                    contents.size());
      }
      std::ranges::sort(registration->frames, {}, &RUNTIME_FUNCTION::BeginAddress);
    }
    for (const auto* symbol : linked.defined_symbols()) {
      if (symbol->hasName() && symbol->isCallable()) {
        registration->symbols.push_back({
            .name = (*symbol->getName()).str(),
            .address = symbol->getAddress().getValue(),
            .size = static_cast<std::uint32_t>(symbol->getSize()),
        });
      }
    }
    const std::scoped_lock lock(DebugHelpMutex());
    pending_.emplace(&responsibility, std::move(registration));
    return llvm::Error::success();
  }

  llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility& responsibility) override {
    llvm::Error result = llvm::Error::success();
    if (auto error = responsibility.withResourceKeyDo(
            [&](llvm::orc::ResourceKey key) { result = Register(responsibility, key); })) {
      return error;
    }
    return result;
  }

  llvm::Error Register(llvm::orc::MaterializationResponsibility& responsibility,
                       llvm::orc::ResourceKey key) {
    const std::scoped_lock lock(DebugHelpMutex());
    const auto found = pending_.find(&responsibility);
    auto& registration = *found->second;
    const auto name = "vela_jit_" + std::to_string(registration.base);
    if (SymLoadModuleEx(session(), nullptr, name.c_str(), name.c_str(), registration.base,
                        registration.size, nullptr, SLMFLAG_VIRTUAL) == 0) {
      return PlatformError("SymLoadModuleEx");
    }
    for (const auto& symbol : registration.symbols) {
      if (SymAddSymbol(session(), registration.base, symbol.name.c_str(), symbol.address,
                       symbol.size, 0) == 0) {
        auto error = PlatformError("SymAddSymbol");
        SymUnloadModule64(session(), registration.base);
        return error;
      }
    }
    if (!registration.frames.empty() &&
        (RtlAddFunctionTable(registration.frames.data(),
                             static_cast<DWORD>(registration.frames.size()),
                             registration.base) == 0U)) {
      auto error = llvm::make_error<llvm::StringError>(
          "RtlAddFunctionTable rejected the JIT unwind table", llvm::inconvertibleErrorCode());
      SymUnloadModule64(session(), registration.base);
      return error;
    }
    registered_.try_emplace(key).first->second.push_back(std::move(found->second));
    pending_.erase(found);
    return llvm::Error::success();
  }

  llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility& responsibility) override {
    const std::scoped_lock lock(DebugHelpMutex());
    pending_.erase(&responsibility);
    return llvm::Error::success();
  }

  llvm::Error notifyRemovingResources(llvm::orc::JITDylib& /*JD*/,
                                      llvm::orc::ResourceKey key) override {
    const std::scoped_lock lock(DebugHelpMutex());
    const auto found = registered_.find(key);
    if (found != registered_.end()) {
      for (auto& registration : found->second) {
        Release(*registration);
      }
      registered_.erase(found);
    }
    return llvm::Error::success();
  }

  void notifyTransferringResources(llvm::orc::JITDylib& /*JD*/,
                                   llvm::orc::ResourceKey destination,
                                   llvm::orc::ResourceKey source) override {
    const std::scoped_lock lock(DebugHelpMutex());
    const auto found = registered_.find(source);
    if (found != registered_.end()) {
      auto registrations = std::move(found->second);
      registered_.erase(found);
      auto& target = registered_.try_emplace(destination).first->second;
      for (auto& registration : registrations) {
        target.push_back(std::move(registration));
      }
    }
  }

 private:
  HANDLE session() noexcept { return static_cast<HANDLE>(this); }
  void Release(Registration& registration) noexcept {
    if (!registration.frames.empty()) {
      RtlDeleteFunctionTable(registration.frames.data());
    }
    SymUnloadModule64(session(), registration.base);
  }
  absl::flat_hash_map<llvm::orc::MaterializationResponsibility*, std::unique_ptr<Registration>>
      pending_;
  absl::flat_hash_map<llvm::orc::ResourceKey, std::vector<std::unique_ptr<Registration>>>
      registered_;
};

}  // namespace

llvm::Error InstallPlatformSupport(llvm::orc::LLJIT& jit) {
  auto plugin = std::make_shared<CoffRegistrationPlugin>();
  if (auto error = plugin->Initialize()) {
    return error;
  }
  llvm::cast<llvm::orc::ObjectLinkingLayer>(jit.getObjLinkingLayer()).addPlugin(std::move(plugin));
  return llvm::Error::success();
}

}  // namespace vela::compiler::jit
