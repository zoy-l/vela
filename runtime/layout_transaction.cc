#include "runtime/layout_transaction.h"
#include <cstddef>

#include "runtime/isolate.h"

namespace vela::runtime {

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
}

}  // namespace vela::runtime
