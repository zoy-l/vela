#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>

#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/source_loader.h"
#include "compiler/frontend/token.h"
#include "compiler/frontend/type_checker.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {

// 数值与 LSP CompletionItemKind 对齐，由所有编辑器客户端共享。
enum class CompletionKind : std::uint8_t {
  Method = 2,
  Function = 3,
  Field = 5,
  Variable = 6,
  Class = 7,
  Interface = 8,
  Module = 9,
  Property = 10,
  Enum = 13,
  Keyword = 14,
  EnumMember = 20,
  Constant = 21,
  Type = 25,
};

struct Completion final {
  std::string label;
  std::string detail;
  CompletionKind kind = CompletionKind::Variable;
};

struct Definition final {
  std::string path;
  TextRange range;
};

struct ImportLink final {
  std::string path;
  TextRange range;
};

struct Hover final {
  std::string signature;
  std::string documentation;
  TextRange range;
};

// 只补齐光标处的标识符和缺失的末尾分隔符，生成查询用快照；不修改文档或编译语法。
[[nodiscard]] auto CompletionSource(std::string_view source, std::size_t offset) -> std::string;

// 索引拥有 AST 与 checker 模型；模型中的节点借用始终由同一个索引覆盖。
class SemanticIndex final {
 public:
  SemanticIndex(std::string_view path, std::span<const frontend::SourceInput> overlays);
  [[nodiscard]] auto Complete(std::size_t offset) const -> std::vector<Completion>;
  [[nodiscard]] auto FindDefinition(std::size_t offset, PositionEncoding encoding) const
      -> std::optional<Definition>;
  [[nodiscard]] auto FindHover(std::size_t offset, PositionEncoding encoding) const
      -> std::optional<Hover>;
  [[nodiscard]] auto ImportLinks(PositionEncoding encoding) const -> std::vector<ImportLink>;

 private:
  using Key = std::pair<std::uint32_t, std::size_t>;
  struct Source final {
    std::string path;
    SourceText text;
    std::vector<frontend::Token> tokens;
    std::vector<frontend::SourceComment> comments;
    std::vector<std::size_t> closing;
    std::vector<frontend::Program::Import> imports;
  };
  struct Scope final {
    std::uint32_t source = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    std::size_t depth = 0;
  };
  struct Symbol final {
    Completion completion;
    std::string documentation;
    Key key;
    std::size_t length = 0;
    frontend::ExportKind export_kind = frontend::ExportKind::Local;
    frontend::Visibility visibility = frontend::Visibility::Public;
    frontend::MemberStorage storage = frontend::MemberStorage::Instance;
  };
  struct Local final {
    std::size_t symbol = 0;
    Scope scope;
    std::size_t available = 0;
  };
  struct Receiver final {
    frontend::TypeDescriptor type;
    frontend::MemberStorage storage = frontend::MemberStorage::Instance;
    std::string context_class;
  };
  [[nodiscard]] auto TokenAt(SourceLocation location) const -> std::size_t;
  [[nodiscard]] auto QueryToken(std::size_t offset) const -> const frontend::Token*;
  [[nodiscard]] auto DocumentationAt(SourceLocation location) const -> std::string;
  [[nodiscard]] auto BuiltinCompletion(const frontend::Token& token) const
      -> std::optional<Completion>;
  [[nodiscard]] auto KeyAt(SourceLocation location) const -> Key;
  [[nodiscard]] auto Extent(const frontend::Statement& statement) const -> Scope;
  auto AddSymbol(SourceLocation location, Completion completion) -> std::size_t;
  void AddReference(SourceLocation location, std::string_view name);
  void CollectDeclarations();
  void CollectParameters(std::size_t symbol, const frontend::Function& function);
  void CollectImports();
  void VisitType(const frontend::TypeName& type);
  void VisitFunction(const frontend::Function& function, const Scope& scope);
  void VisitStatement(const frontend::Statement* statement, Scope scope);
  void VisitExpression(const frontend::Expression* expression, const Scope& scope);
  void VisitProgram();
  void DeclareLocal(const void* node,
                    SourceLocation location,
                    std::string_view name,
                    std::string detail,
                    Scope scope,
                    std::size_t available);
  [[nodiscard]] auto ReceiverFor(const frontend::Expression& expression) const -> Receiver;
  [[nodiscard]] auto MemberCompletions(const Receiver& receiver) const -> std::vector<Completion>;
  void AddContainerCompletions(std::vector<Completion>& result,
                               const frontend::TypeDescriptor& type) const;
  [[nodiscard]] auto VisibleLocals(std::size_t offset) const
      -> absl::flat_hash_map<std::string, Completion>;
  [[nodiscard]] auto QualifiedCompletions(
      std::size_t token,
      const absl::flat_hash_map<std::string, Completion>& locals) const -> std::vector<Completion>;
  void IndexExpression(const frontend::Expression& expression);
  [[nodiscard]] auto ImportSource(const frontend::Program::Import& imported) const
      -> std::optional<std::uint32_t>;

  frontend::Program program_;
  frontend::SemanticModel model_;
  std::vector<Source> sources_;
  absl::flat_hash_map<std::string, std::uint32_t> source_ids_;
  absl::flat_hash_map<std::string, std::string> resolutions_;
  std::vector<Symbol> symbols_;
  absl::flat_hash_map<Key, std::size_t> declarations_;
  absl::flat_hash_map<Key, std::size_t> references_;
  absl::flat_hash_map<std::string, std::size_t> names_;
  absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, std::size_t>> members_;
  absl::flat_hash_map<std::size_t, absl::flat_hash_map<std::string, std::size_t>> parameters_;
  absl::flat_hash_map<std::uint32_t, std::size_t> bindings_;
  absl::flat_hash_map<Key, Receiver> receivers_;
  absl::flat_hash_map<std::size_t, std::uint32_t> import_targets_;
  absl::flat_hash_map<std::string, std::uint32_t> prefixes_;
  std::vector<std::vector<std::size_t>> exports_;
  std::vector<std::size_t> visible_;
  std::vector<Local> locals_;
  enum class IndexPhase : std::uint8_t { SourceTypes, Checked };
  IndexPhase phase_ = IndexPhase::SourceTypes;
  std::string context_class_;
};

}  // namespace vela::tooling
