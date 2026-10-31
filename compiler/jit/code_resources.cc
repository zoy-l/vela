#include "compiler/jit/code_resources.h"

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <utility>

#include <absl/container/flat_hash_map.h>

#include <llvm/ExecutionEngine/JITLink/JITLink.h>
#include <llvm/ExecutionEngine/Orc/Core.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/LinkGraphLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h>
#include <llvm/ExecutionEngine/Orc/Shared/MemoryFlags.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>

namespace vela::compiler::jit {
namespace {

struct CodeMemory final {
  std::size_t executable = 0;
  std::size_t read_only = 0;
  std::size_t writable = 0;

  void Add(CodeMemory memory) noexcept {
    executable += memory.executable;
    read_only += memory.read_only;
    writable += memory.writable;
  }
};

CodeMemory Measure(const llvm::jitlink::LinkGraph& graph) {
  using llvm::orc::MemProt;
  using llvm::operator&;
  CodeMemory memory;
  for (const auto& section : graph.sections()) {
    if (section.getMemLifetime() != llvm::orc::MemLifetime::Standard) {
      continue;
    }
    std::size_t bytes = 0;
    for (const auto* block : section.blocks()) {
      bytes += block->getSize();
    }
    const auto protection = section.getMemProt();
    if ((protection & MemProt::Exec) != MemProt::None) {
      memory.executable += bytes;
    } else if ((protection & MemProt::Write) != MemProt::None) {
      memory.writable += bytes;
    } else {
      memory.read_only += bytes;
    }
  }
  return memory;
}

class ResourceAccounting final : public llvm::orc::LinkGraphLinkingLayer::Plugin {
 public:
  explicit ResourceAccounting(std::shared_ptr<CodeResourceCounters> counters)
      : counters_(std::move(counters)) {}
  ~ResourceAccounting() override {
    for (const auto& [key, memory] : registered_) {
      Remove(memory);
    }
  }
  ResourceAccounting(const ResourceAccounting&) = delete;
  ResourceAccounting& operator=(const ResourceAccounting&) = delete;
  ResourceAccounting(ResourceAccounting&&) = delete;
  ResourceAccounting& operator=(ResourceAccounting&&) = delete;

  void modifyPassConfig(llvm::orc::MaterializationResponsibility& responsibility,
                        [[maybe_unused]] llvm::jitlink::LinkGraph& graph,
                        llvm::jitlink::PassConfiguration& config) override {
    config.PostFixupPasses.emplace_back([this, &responsibility](llvm::jitlink::LinkGraph& graph) {
      const auto memory = Measure(graph);
      const std::scoped_lock lock(mutex_);
      pending_.emplace(&responsibility, memory);
      return llvm::Error::success();
    });
  }

  llvm::Error notifyEmitted(llvm::orc::MaterializationResponsibility& responsibility) override {
    return responsibility.withResourceKeyDo([&](llvm::orc::ResourceKey key) {
      const std::scoped_lock lock(mutex_);
      const auto entry = pending_.find(&responsibility);
      const auto memory = entry->second;
      registered_.try_emplace(key).first->second.Add(memory);
      counters_->executable_bytes.fetch_add(memory.executable, std::memory_order_relaxed);
      counters_->read_only_bytes.fetch_add(memory.read_only, std::memory_order_relaxed);
      counters_->writable_bytes.fetch_add(memory.writable, std::memory_order_relaxed);
      pending_.erase(entry);
    });
  }

  llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility& responsibility) override {
    const std::scoped_lock lock(mutex_);
    pending_.erase(&responsibility);
    return llvm::Error::success();
  }

  llvm::Error notifyRemovingResources([[maybe_unused]] llvm::orc::JITDylib& library,
                                      llvm::orc::ResourceKey key) override {
    const std::scoped_lock lock(mutex_);
    const auto found = registered_.find(key);
    if (found != registered_.end()) {
      Remove(found->second);
      registered_.erase(found);
    }
    return llvm::Error::success();
  }

  void notifyTransferringResources([[maybe_unused]] llvm::orc::JITDylib& library,
                                   llvm::orc::ResourceKey destination_key,
                                   llvm::orc::ResourceKey source_key) override {
    const std::scoped_lock lock(mutex_);
    const auto found = registered_.find(source_key);
    if (found != registered_.end()) {
      const auto memory = found->second;
      registered_.erase(found);
      registered_.try_emplace(destination_key).first->second.Add(memory);
    }
  }

 private:
  void Remove(CodeMemory memory) noexcept {
    counters_->executable_bytes.fetch_sub(memory.executable, std::memory_order_relaxed);
    counters_->read_only_bytes.fetch_sub(memory.read_only, std::memory_order_relaxed);
    counters_->writable_bytes.fetch_sub(memory.writable, std::memory_order_relaxed);
  }
  std::shared_ptr<CodeResourceCounters> counters_;
  std::mutex mutex_;
  absl::flat_hash_map<const llvm::orc::MaterializationResponsibility*, CodeMemory> pending_;
  absl::flat_hash_map<llvm::orc::ResourceKey, CodeMemory> registered_;
};

}  // namespace

void InstallResourceAccounting(llvm::orc::LLJIT& jit,
                               std::shared_ptr<CodeResourceCounters> counters) {
  llvm::cast<llvm::orc::ObjectLinkingLayer>(jit.getObjLinkingLayer())
      .addPlugin(std::make_shared<ResourceAccounting>(std::move(counters)));
}

}  // namespace vela::compiler::jit
