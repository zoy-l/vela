#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>

#include "base/source_location.h"
#include "compiler/frontend/ast.h"
#include "compiler/frontend/parser.h"
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
  std::optional<std::string> insert_text = std::nullopt;
  std::string documentation;
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

enum class SemanticTokenKind : std::uint8_t { Class, Interface, Enum, Type, TypeParameter };
inline constexpr auto kSemanticTokenTypes =
    std::to_array<std::string_view>({"class", "interface", "enum", "type", "typeParameter"});
inline constexpr auto kSemanticTokenModifiers =
    std::to_array<std::string_view>({"declaration", "defaultLibrary"});
inline constexpr std::uint32_t kSemanticDeclaration = 1U;
inline constexpr std::uint32_t kSemanticDefaultLibrary = 2U;

struct SemanticToken final {
  TextPosition position;
  std::size_t length = 0;
  SemanticTokenKind kind = SemanticTokenKind::Type;
  std::uint32_t modifiers = 0;
};

struct ArgumentCompletionQuery final {
  std::string source;
  std::size_t callee_offset = 0;
  std::string prefix;
  bool has_colon = false;
  std::size_t positional_arguments = 0;
  absl::flat_hash_set<std::string> supplied_names;
};

// 使用生产 lexer 定位当前调用，并仅修复查询快照中的实参和末尾分隔符。
[[nodiscard]] std::optional<ArgumentCompletionQuery> PrepareArgumentCompletion(
    std::string_view source,
    std::size_t offset);

// 只补齐光标处的标识符和缺失的末尾分隔符，生成查询用快照；不修改文档或编译语法。
[[nodiscard]] std::string CompletionSource(std::string_view source, std::size_t offset);
[[nodiscard]] std::string DisplaySymbolName(std::string_view name);

// 索引拥有 AST 与 checker 模型；模型中的节点借用始终由同一个索引覆盖。
class SemanticIndex final {
 public:
  SemanticIndex(std::string_view path, std::span<const frontend::SourceInput> overlays);
  SemanticIndex(std::string_view path,
                frontend::ModuleCache& modules,
                std::span<const frontend::SourceInput> overlays);
  [[nodiscard]] std::vector<Completion> Complete(std::size_t offset) const;
  [[nodiscard]] std::optional<Definition> FindDefinition(std::size_t offset,
                                                         PositionEncoding encoding) const;
  [[nodiscard]] std::optional<Hover> FindHover(std::size_t offset, PositionEncoding encoding) const;
  [[nodiscard]] std::optional<Definition> PrepareRename(std::size_t offset,
                                                        PositionEncoding encoding) const;
  [[nodiscard]] std::expected<std::vector<Definition>, std::string>
  RenameSymbol(std::size_t offset, std::string_view name, PositionEncoding encoding) const;
  [[nodiscard]] std::vector<ImportLink> ImportLinks(PositionEncoding encoding) const;
  [[nodiscard]] std::vector<SemanticToken> SemanticTokens(PositionEncoding encoding) const;
  [[nodiscard]] std::vector<Completion> CompleteArguments(
      const ArgumentCompletionQuery& query) const;

 private:
  explicit SemanticIndex(frontend::ParseResult loaded);
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
    enum class Role : std::uint8_t { Ordinary, Parameter, TypeParameter };
    Completion completion;
    std::string owner;
    Role role = Role::Ordinary;
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
  struct Reference final {
    std::size_t symbol = 0;
    const frontend::Expression* expression = nullptr;
  };
  struct SymbolRename final {
    std::size_t symbol = 0;
    std::vector<Key> offsets;
    std::string_view name;
  };
  struct Receiver final {
    frontend::TypeDescriptor type;
    frontend::MemberStorage storage = frontend::MemberStorage::Instance;
    std::string context_class;
  };
  [[nodiscard]] std::size_t TokenAt(SourceLocation location) const;
  [[nodiscard]] const frontend::Token* QueryToken(std::size_t offset) const;
  [[nodiscard]] std::optional<std::size_t> RenameTarget(std::size_t offset) const;
  [[nodiscard]] Key RenamedKey(Key key,
                               const SemanticIndex& renamed,
                               const SymbolRename& request) const;
  [[nodiscard]] bool CanPreserveReferences(const SemanticIndex& renamed,
                                           const SymbolRename& request) const;
  [[nodiscard]] std::string DocumentationAt(SourceLocation location) const;
  [[nodiscard]] Completion ClassCompletion(const frontend::Class& declaration) const;
  [[nodiscard]] std::string DefaultValue(const frontend::Expression& expression) const;
  void MarkTypeParameters(std::span<const frontend::TypeParameter> parameters);
  [[nodiscard]] std::optional<Completion> BuiltinCompletion(const frontend::Token& token) const;
  [[nodiscard]] static std::string HoverSignature(const Completion& completion,
                                                  const std::string& owner,
                                                  Symbol::Role role);
  [[nodiscard]] Key KeyAt(SourceLocation location) const;
  [[nodiscard]] Scope Extent(const frontend::Statement& statement) const;
  std::size_t AddSymbol(SourceLocation location, CompletionKind kind, std::string detail);
  template <class Declaration>
  std::size_t DeclareSymbol(const Declaration& declaration,
                            CompletionKind kind,
                            const std::string& detail);
  template <class Declaration>
  std::size_t DeclareMember(std::string_view owner,
                            const Declaration& declaration,
                            CompletionKind kind,
                            const std::string& detail);
  void AddReference(SourceLocation location,
                    std::string_view name,
                    const frontend::Expression* expression = nullptr);
  void CollectDeclarations();
  void CollectClass(const frontend::Class& declaration);
  void CollectInterface(const frontend::Interface& declaration);
  void BindIntrinsicSignature(std::size_t symbol,
                              const frontend::Class& owner,
                              const frontend::Function& method);
  void CollectEnumMembers(const frontend::Enum& declaration);
  void CollectParameters(std::size_t symbol, const frontend::Function& function);
  void CollectImports();
  void VisitType(const frontend::TypeName& type);
  void VisitFunction(const frontend::Function& function,
                     const Scope& scope,
                     const frontend::FunctionDescriptor* signature = nullptr);
  void VisitStatement(const frontend::Statement* statement, Scope scope);
  void VisitForEach(const frontend::Statement& statement, const Scope& scope);
  void VisitExpression(const frontend::Expression* expression, const Scope& scope);
  void VisitProgram();
  void VisitClass(const frontend::Class& declaration, const Scope& root);
  void VisitInterface(const frontend::Interface& declaration, const Scope& root);
  void VisitDefinitions(const Scope& root);
  [[nodiscard]] const frontend::NodeFacts& Facts() const;
  [[nodiscard]] const frontend::NodeFacts& FactsFor(const frontend::Expression& expression) const;
  [[nodiscard]] std::string DisplayType(frontend::TypeDescriptor type) const;
  [[nodiscard]] std::string DisplaySignature(const frontend::FunctionDescriptor& signature) const;
  [[nodiscard]] frontend::TypeDescriptor Capability(frontend::TypeDescriptor type) const;
  [[nodiscard]] std::vector<std::string_view> MemberOwners(frontend::TypeDescriptor type) const;
  void DeclareLocal(const void* node,
                    SourceLocation location,
                    std::string detail,
                    Scope scope,
                    std::size_t available);
  [[nodiscard]] Receiver ReceiverFor(const frontend::Expression& expression) const;
  [[nodiscard]] std::vector<Completion> MemberCompletions(const Receiver& receiver) const;
  void AddAsyncCompletions(std::vector<Completion>& result,
                           const frontend::TypeDescriptor& type) const;
  void AddContainerCompletions(std::vector<Completion>& result,
                               const frontend::TypeDescriptor& type) const;
  [[nodiscard]] absl::flat_hash_map<std::string, Completion> VisibleLocals(
      std::size_t offset) const;
  [[nodiscard]] std::vector<Completion> QualifiedCompletions(
      std::size_t token,
      const absl::flat_hash_map<std::string, Completion>& locals) const;
  void IndexExpression(const frontend::Expression& expression);
  [[nodiscard]] std::optional<frontend::FunctionDescriptor> FindCallParameters(
      std::size_t callee_offset) const;
  [[nodiscard]] std::optional<frontend::FunctionDescriptor> FindMemberCallParameters(
      const frontend::Expression& call) const;
  [[nodiscard]] std::optional<std::uint32_t> ImportSource(
      const frontend::Program::Import& imported) const;

  frontend::Program program_;
  frontend::SemanticModel model_;
  std::vector<Source> sources_;
  absl::flat_hash_map<std::string, std::uint32_t> source_ids_;
  absl::flat_hash_map<std::string, std::string> resolutions_;
  std::vector<Symbol> symbols_;
  absl::flat_hash_map<Key, std::size_t> declarations_;
  absl::flat_hash_map<Key, Reference> references_;
  absl::flat_hash_map<std::string, std::size_t> names_;
  absl::flat_hash_map<std::string, absl::flat_hash_map<std::string, std::size_t>> members_;
  absl::flat_hash_map<std::size_t, absl::flat_hash_map<std::string, std::size_t>> parameters_;
  absl::flat_hash_map<std::uint32_t, std::size_t> bindings_;
  absl::flat_hash_map<Key, Receiver> receivers_;
  absl::flat_hash_map<Key, const frontend::Expression*> calls_;
  absl::flat_hash_map<Key, std::uint32_t> type_parameters_;
  absl::flat_hash_map<frontend::TypeParameterId, std::size_t> type_parameter_symbols_;
  absl::flat_hash_map<std::size_t, std::uint32_t> import_targets_;
  absl::flat_hash_map<std::string, std::uint32_t> prefixes_;
  std::vector<std::vector<std::size_t>> exports_;
  std::vector<std::size_t> visible_;
  std::vector<Local> locals_;
  enum class IndexPhase : std::uint8_t { SourceTypes, Checked, Definitions };
  IndexPhase phase_ = IndexPhase::SourceTypes;
  std::string context_class_;
};

}  // namespace vela::tooling
