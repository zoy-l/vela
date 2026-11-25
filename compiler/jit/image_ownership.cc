#include "compiler/jit/image_ownership.h"
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <llvm/ExecutionEngine/Orc/CoreContainers.h>
#include "compiler/jit/code_batch.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "base/declaration.h"
#include "compiler/jit/code_resources.h"
#include "compiler/jit/code_reuse.h"
#include "compiler/native/native_image_data.h"
#include "runtime/native/native_image.h"

namespace vela::compiler::jit {

ImageOwnership::ImageOwnership(std::shared_ptr<CodeResourceCounters> counts,
                               const runtime::NativeImage& image,
                               std::shared_ptr<native::NativeImageData> storage)
    : counters(std::move(counts)), data(std::move(storage)) {
  const auto strings = image.strings.values();
  constant_payload_bytes = strings.size_bytes();
  for (const auto& text : strings) {
    constant_payload_bytes += text.size;
  }
  const auto paths = image.source_paths.values();
  const auto sources = image.source_texts.values();
  debug_payload_bytes = paths.size_bytes() + sources.size_bytes();
  for (const auto& path : paths) {
    debug_payload_bytes += path.size;
  }
  for (const auto& source : sources) {
    debug_payload_bytes += source.size;
  }
  for (const auto& function : image.functions.values()) {
    debug_payload_bytes += function.locations.values().size_bytes() + function.name.size +
                           function.native_symbol.size + sizeof(FunctionIdentity);
  }
  counters->constant_payload_bytes.fetch_add(constant_payload_bytes, std::memory_order_relaxed);
  counters->debug_payload_bytes.fetch_add(debug_payload_bytes, std::memory_order_relaxed);
  counters->images.fetch_add(1, std::memory_order_relaxed);
  image_data_bytes = data ? data->bytes() : 0;
  counters->image_data_bytes.fetch_add(image_data_bytes, std::memory_order_relaxed);
}

ImageOwnership::~ImageOwnership() {
  counters->images.fetch_sub(1, std::memory_order_relaxed);
  counters->image_data_bytes.fetch_sub(image_data_bytes, std::memory_order_relaxed);
  counters->constant_payload_bytes.fetch_sub(constant_payload_bytes, std::memory_order_relaxed);
  counters->debug_payload_bytes.fetch_sub(debug_payload_bytes, std::memory_order_relaxed);
}

absl::flat_hash_map<std::string, ReusableCode> RetainImageCode(
    BatchSymbols& interned,
    const llvm::orc::SymbolMap& resolved,
    const std::shared_ptr<void>& resource,
    CodeReusePlan& plan,
    ImageOwnership& owner) {
  absl::flat_hash_set<const void*> owners;
  absl::flat_hash_map<std::string, ReusableCode> updated;
  for (auto& function : plan.functions) {
    if (!function.resource) {
      function.address = resolved.at(interned.Get(function.symbol)).getAddress().getValue();
      function.resource = resource;
    }
    if (owners.insert(function.resource.get()).second) {
      owner.code.push_back(function.resource);
    }
    updated.emplace(std::move(function.name), ReusableCode{
                                                  .symbol = std::move(function.symbol),
                                                  .resource = function.resource,
                                                  .address = function.address,
                                              });
  }
  return updated;
}

}  // namespace vela::compiler::jit
