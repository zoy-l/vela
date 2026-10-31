#include "runtime/external_resources.h"

#include <atomic>
#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

#include "runtime/intrusive_link_internal.h"

namespace vela::runtime {

std::string_view ExternalResourceKindName(ExternalResourceKind kind) noexcept {
  switch (kind) {
    case ExternalResourceKind::RootRange:
      return "RootRange";
    case ExternalResourceKind::RootedValue:
      return "RootedValue";
    case ExternalResourceKind::NativeInstance:
      return "NativeInstance";
    case ExternalResourceKind::NativeHandle:
      return "NativeHandle";
    case ExternalResourceKind::NativeEntry:
      return "NativeNamedEntry";
    case ExternalResourceKind::NativeCandidate:
      return "NativePreparedPublication";
    case ExternalResourceKind::Program:
      return "Program";
    case ExternalResourceKind::Future:
      return "Future";
    case ExternalResourceKind::Execution:
      return "Execution";
    case ExternalResourceKind::HostAccess:
      return "HostAccess";
  }
  std::unreachable();
}

ExternalResource::ExternalResource(ExternalResources& owner, ExternalResourceKind kind) noexcept {
  Attach(owner, kind);
  Retain();
}

ExternalResource::~ExternalResource() {
  internal::UnlinkIntrusive(*this, &ExternalResource::next_, &ExternalResource::previous_link_);
}

void ExternalResource::Attach(ExternalResources& owner, ExternalResourceKind kind) noexcept {
  kind_ = kind;
  identity_ = owner.next_identity_++;
  internal::PrependIntrusive(*this, owner.first_, &ExternalResource::next_,
                             &ExternalResource::previous_link_);
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
