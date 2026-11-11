#include "runtime/native/native_metadata.h"

#include "runtime/isolate/program_layouts.h"
#include "runtime/value/object.h"

namespace vela::runtime {

bool NativeMetadataSet::IsEmpty() const noexcept {
  return classes.empty() && nominals.empty() && arrays.empty() && collections.empty() &&
         signatures.empty() && interfaces.empty() && code_identities.empty();
}

void NativeMetadataTracer::Mark(const ClassLayout* layout) {
  if (layout == nullptr || !reached_.classes.insert(layout).second) {
    return;
  }
  Mark(&layout->nominal_type());
  Mark(layout->callable_type());
  Mark(layout->code_identity());
}

void NativeMetadataTracer::Mark(const NominalType* nominal) {
  if (nominal == nullptr || !reached_.nominals.insert(nominal).second) {
    return;
  }
  for (const auto* interface : nominal->interfaces()) {
    Mark(interface);
  }
  Mark(nominal->fixed_layout());
  Mark(nominal->base());
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

void NativeMetadataTracer::Mark(const CodeIdentity* identity) {
  if (identity != nullptr) {
    reached_.code_identities.insert(identity);
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

void NativeMetadataTracer::Mark(const LayoutCache& cache) {
  Mark(cache.code_identity);
  for (const auto& [name, layout] : cache.classes) {
    Mark(layout);
  }
  for (const auto& [name, layout] : cache.arrays) {
    Mark(layout);
  }
  for (const auto& [name, layout] : cache.collections) {
    Mark(layout);
  }
  for (const auto& [name, signature] : cache.signatures) {
    Mark(signature);
  }
  for (const auto& [name, interface] : cache.interfaces) {
    Mark(interface);
  }
}

}  // namespace vela::runtime
