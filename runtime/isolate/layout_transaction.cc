#include "runtime/isolate/layout_transaction.h"
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "base/declaration.h"
#include "runtime/heap/heap.h"
#include "runtime/isolate/isolate.h"
#include "runtime/native/native_program_state.h"
#include "runtime/value/object.h"

namespace vela::runtime {

LayoutTransaction::LayoutTransaction(Isolate& isolate, LayoutLifetime lifetime) noexcept
    : isolate_(&isolate),
      initial_{
          .classes = isolate.class_layouts_.size(),
          .arrays = isolate.array_layouts_.size(),
          .collections = isolate.collection_layouts_.size(),
          .signatures = isolate.type_signatures_.size(),
          .interfaces = isolate.interface_types_.size(),
          .code_identities = isolate.code_identities_.size(),
          .nominal_types = isolate.nominal_types_.size(),
      },
      lifetime_(lifetime) {
  ++isolate.layout_transactions_;
  if (lifetime == LayoutLifetime::Reloadable) {
    ++isolate.native_layout_transactions_;
  }
}

void LayoutTransaction::CommitNative(HeapObject& owner) {
  auto& state = Heap::NativeProgram(owner);
  auto& metadata = isolate_->native_metadata_;
  const auto retain_new = [](auto& target, const auto& storage, std::size_t first) {
    for (auto index = first; index < storage.size(); ++index) {
      target.insert(storage.at(index).get());
    }
  };
  retain_new(metadata.arrays, isolate_->array_layouts_, initial_.arrays);
  retain_new(metadata.collections, isolate_->collection_layouts_, initial_.collections);
  retain_new(metadata.signatures, isolate_->type_signatures_, initial_.signatures);
  retain_new(metadata.interfaces, isolate_->interface_types_, initial_.interfaces);
  auto& layouts = isolate_->class_layouts_;
  auto retained = initial_.classes;
  for (auto index = initial_.classes; index < layouts.size(); ++index) {
    auto& layout = layouts.at(index);
    if (layout->IsEnum()) {
      metadata.classes.insert(layout.get());
      if (retained != index) {
        layouts.at(retained) = std::move(layout);
      }
      ++retained;
    } else {
      layout->program_owner_ = &owner;
      state.owned_classes.push_back(std::move(layout));
    }
  }
  layouts.resize(retained);
  auto& nominals = isolate_->nominal_types_;
  retained = initial_.nominal_types;
  for (auto index = initial_.nominal_types; index < nominals.size(); ++index) {
    auto& nominal = nominals.at(index);
    if (nominal->declaration() != DeclarationId{}) {
      metadata.nominals.insert(nominal.get());
      if (retained != index) {
        nominals.at(retained) = std::move(nominal);
      }
      ++retained;
    } else {
      state.owned_nominals.push_back(std::move(nominal));
    }
  }
  nominals.resize(retained);
  auto& identities = isolate_->code_identities_;
  for (auto index = initial_.code_identities; index < identities.size(); ++index) {
    state.owned_code_identities.push_back(std::move(identities.at(index)));
  }
  identities.resize(initial_.code_identities);
  committed_ = true;
}

LayoutTransaction::~LayoutTransaction() {
  if (!committed_) {
    for (std::size_t index = initial_.signatures; index < isolate_->type_signatures_.size();
         ++index) {
      isolate_->type_signature_index_.erase(*isolate_->type_signatures_.at(index));
    }
    for (std::size_t index = initial_.collections; index < isolate_->collection_layouts_.size();
         ++index) {
      isolate_->collection_layout_index_.erase(*isolate_->collection_layouts_.at(index));
    }
    for (std::size_t index = initial_.arrays; index < isolate_->array_layouts_.size(); ++index) {
      isolate_->array_layout_index_.erase(*isolate_->array_layouts_.at(index));
    }
    isolate_->collection_layouts_.resize(initial_.collections);
    isolate_->array_layouts_.resize(initial_.arrays);
    isolate_->class_layouts_.resize(initial_.classes);
    isolate_->nominal_types_.resize(initial_.nominal_types);
    isolate_->type_signatures_.resize(initial_.signatures);
    isolate_->interface_types_.resize(initial_.interfaces);
    isolate_->code_identities_.resize(initial_.code_identities);
  }
  --isolate_->layout_transactions_;
  if (lifetime_ == LayoutLifetime::Reloadable) {
    --isolate_->native_layout_transactions_;
  }
  if (isolate_->layout_transactions_ == 0 && isolate_->metadata_collection_pending_) {
    isolate_->heap().FinishCollection();
    if (isolate_->metadata_collection_pending_) {
      isolate_->CollectNativeMetadata();
    }
  }
}

}  // namespace vela::runtime
