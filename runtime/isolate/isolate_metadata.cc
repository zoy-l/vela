#include "runtime/isolate/isolate.h"

#include <memory>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "runtime/heap/heap.h"
#include "runtime/native/native_dispatch.h"
#include "runtime/native/native_metadata.h"

namespace vela::runtime {
namespace {

template <class T>
void MarkPersistent(const std::vector<std::unique_ptr<T>>& storage,
                    const absl::flat_hash_set<const T*>& reclaimable,
                    NativeMetadataTracer& tracer) {
  for (const auto& item : storage) {
    if (!reclaimable.contains(item.get())) {
      tracer.Mark(item.get());
    }
  }
}

template <class T>
void SweepMetadata(std::vector<std::unique_ptr<T>>& storage,
                   absl::flat_hash_set<const T*>& reclaimable,
                   const absl::flat_hash_set<const T*>& reached) {
  std::erase_if(storage, [&](const auto& item) {
    const auto* pointer = item.get();
    return !reached.contains(pointer) && reclaimable.erase(pointer) != 0;
  });
}

template <class T>
void SweepInterned(std::vector<std::unique_ptr<T>>& storage,
                   absl::flat_hash_map<T, const T*>& index,
                   absl::flat_hash_set<const T*>& reclaimable,
                   const absl::flat_hash_set<const T*>& reached) {
  for (const auto& item : storage) {
    const auto* pointer = item.get();
    if (reclaimable.contains(pointer) && !reached.contains(pointer)) {
      index.erase(*pointer);
    }
  }
  SweepMetadata(storage, reclaimable, reached);
}

}  // namespace

void Isolate::CollectNativeMetadata() {
  // 正在实例化的事务仍使用容器边界做回滚；在它退出前不能压缩这些容器。
  if (layout_transactions_ != 0) {
    metadata_collection_pending_ = true;
    return;
  }
  metadata_collection_pending_ = false;
  if (native_metadata_.IsEmpty()) {
    return;
  }
  NativeMetadataTracer tracer;
  MarkPersistent(class_layouts_, native_metadata_.classes, tracer);
  MarkPersistent(nominal_types_, native_metadata_.nominals, tracer);
  MarkPersistent(array_layouts_, native_metadata_.arrays, tracer);
  MarkPersistent(collection_layouts_, native_metadata_.collections, tracer);
  MarkPersistent(type_signatures_, native_metadata_.signatures, tracer);
  MarkPersistent(interface_types_, native_metadata_.interfaces, tracer);
  heap_.TraceNativeMetadata(tracer);
  native_dispatch_.TraceMetadata(tracer);
  const auto& reached = tracer.reached();
  SweepInterned(array_layouts_, array_layout_index_, native_metadata_.arrays, reached.arrays);
  SweepInterned(collection_layouts_, collection_layout_index_, native_metadata_.collections,
                reached.collections);
  SweepInterned(type_signatures_, type_signature_index_, native_metadata_.signatures,
                reached.signatures);
  SweepMetadata(class_layouts_, native_metadata_.classes, reached.classes);
  SweepMetadata(nominal_types_, native_metadata_.nominals, reached.nominals);
  SweepMetadata(interface_types_, native_metadata_.interfaces, reached.interfaces);
}

}  // namespace vela::runtime
