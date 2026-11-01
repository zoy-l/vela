#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "compiler/jit/code_inputs.h"
#include "compiler/native/native_roots.h"
#include "runtime/native/native_image.h"

namespace llvm {
class Module;
}

namespace vela::compiler::jit {

struct ReusableCode final {
  std::string symbol;
  std::weak_ptr<void> resource;
  std::uintptr_t address = 0;
};

// 开发会话拥有弱代码索引；实际映像与直接调用边持有代码，不因缓存延长旧版本生命周期。
struct CodeReuseCache final {
  struct Source final {
    std::string fingerprint;
    std::vector<std::string> symbols;
    native::NativeFunctionMetrics metrics;
    runtime::NativeEntry entry = nullptr;
    runtime::NativeTypedEntry typed_entry = nullptr;
    std::vector<std::size_t> body_dependencies;
  };
  std::mutex mutex;
  absl::flat_hash_map<std::string, ReusableCode> functions;
  std::vector<Source> sources;
};

struct ReuseFunction final {
  std::string name;
  std::string symbol;
  std::shared_ptr<void> resource;
  std::uintptr_t address = 0;
};

struct CodeReusePlan final {
  std::vector<FunctionCodeInputs> inputs;
  std::vector<std::uint8_t> reused;
  std::vector<ReuseFunction> functions;
  // 从规划开始就固定弱索引命中的资源，覆盖后台生成期间旧候选被释放的窗口。
  std::vector<std::shared_ptr<void>> retained;
  // 新机器码真正直接引用的旧代码。映像表中的入口指针另由映像所有者统一保活。
  std::vector<std::shared_ptr<void>> dependencies;
  std::size_t compiled_functions = 0;
};

CodeReusePlan PlanCodeReuse(std::vector<FunctionCodeInputs> inputs, const CodeReuseCache& cache);
void BindReusableCode(llvm::Module& module,
                      const CodeReuseCache& cache,
                      const std::vector<std::vector<std::string>>& symbols,
                      std::uint64_t batch,
                      CodeReusePlan& plan);

}  // namespace vela::compiler::jit
