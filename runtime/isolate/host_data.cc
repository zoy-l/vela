#include "runtime/isolate/host_data.h"

#include "runtime/heap/intrusive_link_internal.h"
#include "runtime/isolate/isolate.h"

namespace vela::runtime {

void HostData::Attach(Isolate& isolate) noexcept {
  internal::PrependIntrusive(*this, isolate.host_data_, &HostData::next_,
                             &HostData::previous_link_);
}

HostData::~HostData() {
  internal::UnlinkIntrusive(*this, &HostData::next_, &HostData::previous_link_);
}

void* HostData::Find(const Isolate& isolate, const void* key) noexcept {
  for (auto* data = isolate.host_data_; data != nullptr; data = data->next_) {
    if (data->key_ == key) {
      return data->value_;
    }
  }
  return nullptr;
}

}  // namespace vela::runtime
