#include "runtime/layout_transaction.h"

#include "runtime/isolate.h"

namespace vela::runtime {

LayoutTransaction::~LayoutTransaction() {
  if (!committed_) {
    isolate_->collection_layouts_.resize(initial_.collections);
    isolate_->array_layouts_.resize(initial_.arrays);
    isolate_->class_layouts_.resize(initial_.classes);
  }
}

}  // namespace vela::runtime
