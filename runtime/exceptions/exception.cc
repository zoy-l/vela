#include "runtime/exceptions/exception.h"

#include <string_view>

#include "runtime/value/object.h"

namespace vela::runtime {

std::string_view ExceptionData::message() const noexcept {
  return {StorageAfter<char>(*this), message_length};
}

ExceptionData& ExceptionData::From(HeapObject& object) noexcept {
  return *StorageAfter<ExceptionData>(object);
}

}  // namespace vela::runtime
