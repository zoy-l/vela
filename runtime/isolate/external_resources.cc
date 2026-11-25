#include "runtime/isolate/external_resources.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace vela::runtime {

std::string_view ExternalResourceKindName(ExternalResourceKind kind) noexcept {
  switch (kind) {
    case ExternalResourceKind::RootRange:
      return "RootRange";
    case ExternalResourceKind::NativeInstance:
      return "NativeInstance";
    case ExternalResourceKind::NativeHandle:
      return "NativeHandle";
    case ExternalResourceKind::Persistent:
      return "Persistent";
    case ExternalResourceKind::Type:
      return "Type";
    case ExternalResourceKind::NativeNamedEntry:
      return "NativeNamedEntry";
    case ExternalResourceKind::NativePreparedPublication:
      return "NativePreparedPublication";
    case ExternalResourceKind::Future:
      return "Future";
    case ExternalResourceKind::Execution:
      return "Execution";
    case ExternalResourceKind::HostAccess:
      return "HostAccess";
  }
  std::unreachable();
}

void ExternalResource::Retain() noexcept {
  references_.fetch_add(1, std::memory_order_relaxed);
}

void ExternalResource::Release() noexcept {
  references_.fetch_sub(1, std::memory_order_release);
}

std::size_t ExternalResource::references() const noexcept {
  return references_.load(std::memory_order_acquire);
}

void ExternalResources::AppendTo(std::vector<ExternalResourceInfo>& result) const {
  for (auto* entry = first_; entry != nullptr; entry = entry->next_) {
    const auto references = entry->references();
    if (references != 0) {
      result.push_back({
          .kind = entry->kind_,
          .identity = entry->identity_,
          .references = references,
      });
    }
  }
}

}  // namespace vela::runtime
