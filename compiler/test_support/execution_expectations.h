#pragma once

namespace vela::runtime {
class NativeInstance;
}

namespace vela::compiler::test {

void ExpectAnswer(runtime::NativeInstance& instance);

}  // namespace vela::compiler::test
