#include "runtime/native/native_metadata.h"

#include "runtime/isolate/program_layouts.h"
#include "runtime/value/object.h"

namespace vela::runtime {

bool NativeMetadataSet::IsEmpty() const noexcept {
  return classes.empty() && nominals.empty() && arrays.empty() && collections.empty() &&
         signatures.empty() && interfaces.empty();
}

void NativeMetadataTracer::Mark(const ClassLayout* layout) {
  if (layout == nullptr || !reached_.classes.insert(layout).second) {
    return;
  }
  Mark(&layout->nominal_type());
  Mark(layout->callable_type());
}

void NativeMetadataTracer::Mark(const NominalType* nominal) {
  if (nominal == nullptr || !reached_.nominals.insert(nominal).second) {
    return;
  }
  for (const auto* interface : nominal->interfaces()) {
    Mark(interface);
  }
  Mark(nominal->fixed_layout());
}

void NativeMetadataTracer::Mark(const ArrayLayout* layout) {
  if (layout != nullptr && reached_.arrays.insert(layout).second) {
    Mark(layout->element);
  }
}

void NativeMetadataTracer::Mark(const CollectionLayout* layout) {
  if (layout != nullptr && reached_.collections.insert(layout).second) {
    Mark(layout->keys.element);
    if (layout->values) {
      Mark(layout->values->element);
    }
  }
}

void NativeMetadataTracer::Mark(const TypeSignature* signature) {
  if (signature == nullptr || !reached_.signatures.insert(signature).second) {
    return;
  }
  for (const auto& parameter : signature->parameters) {
    Mark(parameter.type);
  }
  if (signature->result) {
    Mark(*signature->result);
  }
}

void NativeMetadataTracer::Mark(const InterfaceType* interface) {
  if (interface != nullptr) {
    reached_.interfaces.insert(interface);
  }
}

void NativeMetadataTracer::Mark(TypeConstraint type) {
  Mark(type.nominal);
  Mark(type.array);
  Mark(type.collection);
  Mark(type.signature);
  Mark(type.interface);
}

void NativeMetadataTracer::Mark(const ProgramLayouts& layouts) {
  for (const auto* layout : layouts.classes) {
    Mark(layout);
  }
  for (const auto* layout : layouts.arrays) {
    Mark(layout);
  }
  for (const auto* layout : layouts.collections) {
    Mark(layout);
  }
  for (const auto* interface : layouts.interfaces) {
    Mark(interface);
  }
  for (const auto type : layouts.types) {
    Mark(type);
  }
}

}  // namespace vela::runtime
