#include "compiler/tooling/semantic/semantic_references.h"

#include <algorithm>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>

#include "compiler/frontend/syntax/token.h"
#include "compiler/frontend/typing/semantic_model.h"
#include "compiler/tooling/semantic/semantic_index.h"
#include "compiler/tooling/source_text.h"

namespace vela::tooling {

ReferenceIndex::Key ReferenceIndex::KeyFor(const Definition& location) {
  const auto& start = location.range.start;
  return {location.path, start.line, start.character};
}

void ReferenceIndex::Connect(const Definition& first, const Definition& second) {
  const auto first_key = KeyFor(first);
  const auto second_key = KeyFor(second);
  if (first_key != second_key) {
    relations_[first_key].insert(second_key);
    relations_[second_key].insert(first_key);
  }
}

void ReferenceIndex::AddOccurrence(const Definition& declaration,
                                   Definition location,
                                   bool is_declaration) {
  references_[KeyFor(declaration)].push_back(
      {.location = std::move(location), .is_declaration = is_declaration});
}

void SemanticIndex::AddReferences(ReferenceIndex& references, PositionEncoding encoding) const {
  if (sources_.empty()) {
    return;
  }
  const auto& source = sources_.front();
  for (const auto& [key, reference] : references_) {
    if (key.first != 0) {
      continue;
    }
    const auto& token =
        *std::ranges::lower_bound(source.tokens, key.second, {}, &frontend::Token::offset);
    references.AddOccurrence(DefinitionFor(reference.symbol, encoding),
                             LocationFor(key, token.length, encoding), declarations_.contains(key));
  }
  for (const auto& [name, owner] : model_.classes) {
    for (const auto& [selector, target] : owner.virtual_targets) {
      const auto contract = DefinitionFor(
          frontend::MemberReference{
              .owner = std::get<0>(selector),
              .member = std::get<1>(selector),
          },
          encoding);
      const auto implementation = DefinitionFor(target, encoding);
      if (contract && implementation) {
        references.Connect(*contract, *implementation);
      }
    }
  }
}

std::vector<Definition> ReferenceIndex::FindReferences(const Definition& declaration,
                                                       ReferenceDeclaration include) const {
  std::vector<Key> pending{KeyFor(declaration)};
  absl::flat_hash_set<Key> visited;
  std::vector<Definition> result;
  while (!pending.empty()) {
    auto key = std::move(pending.back());
    pending.pop_back();
    if (!visited.insert(key).second) {
      continue;
    }
    if (const auto related = relations_.find(key); related != relations_.end()) {
      pending.insert(pending.end(), related->second.begin(), related->second.end());
    }
    if (const auto references = references_.find(key); references != references_.end()) {
      for (const auto& occurrence : references->second) {
        if (include == ReferenceDeclaration::Include || !occurrence.is_declaration) {
          result.push_back(occurrence.location);
        }
      }
    }
  }
  std::ranges::sort(result, {}, [](const Definition& reference) {
    const auto& start = reference.range.start;
    return std::tie(reference.path, start.line, start.character);
  });
  const auto duplicates = std::ranges::unique(result);
  result.erase(duplicates.begin(), duplicates.end());
  return result;
}

}  // namespace vela::tooling
