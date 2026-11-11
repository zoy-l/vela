#pragma once

#include <expected>
#include <memory>
#include <sstream>
#include <string_view>

#include "base/testing/directory.h"
#include "compiler/development/development_files.h"
#include "compiler/development/development_repl.h"
#include "compiler/development/development_session.h"
#include "compiler/development/source_session.h"
#include "compiler/frontend/diagnostic.h"
#include "runtime/isolate/isolate.h"

namespace vela::compiler::test {

[[nodiscard]] DevelopmentResult ApplyUpdate(DevelopmentSession& session,
                                            SourceUpdate update,
                                            DevelopmentOperation operation);
void ExpectAccepted(const DevelopmentResult& result);

class DevelopmentReplTest : public vela::test::DirectoryTest {
 protected:
  void SetUp() override;
  void SetUp(runtime::NativeBindings bindings);
  void TearDown() override;
  void LoadSession(runtime::NativeBindings bindings);
  void Execute(std::string_view input);
  [[nodiscard]] std::expected<DevelopmentResult, frontend::Diagnostics> Reset();
  void ExpectReset(DevelopmentStatus status);
  [[nodiscard]] std::expected<DevelopmentResult, frontend::Diagnostics> UpdateFiles(
      DevelopmentOperation operation);
  [[nodiscard]] DevelopmentRepl& repl() const { return *repl_; }
  [[nodiscard]] DevelopmentSession& session() const { return *session_; }
  [[nodiscard]] std::ostringstream& output() { return output_; }

 private:
  std::ostringstream output_;
  std::unique_ptr<DevelopmentFiles> files_;
  std::unique_ptr<DevelopmentSession> session_;
  std::unique_ptr<DevelopmentRepl> repl_;
};

}  // namespace vela::compiler::test
