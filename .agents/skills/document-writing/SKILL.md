---
name: document-writing
description: Draft, revise, or review durable project documentation in the user's direct, outcome-first style. Use for technical specifications, design documents, architecture references, and reports; do not use for emails or creative writing.
---

# Document Writing

Write for a reader who needs the document's conclusions and usable contracts, not a record of how the author arrived at them. Follow the user's requested format and the project's local documentation conventions.

## Style

- Lead with the decision, behavior, or current contract. State the document's purpose and scope early.
- Do not narrate research, implementation, or debugging chronologically. Keep investigation steps, abandoned approaches, and progress updates out unless the requested deliverable is specifically a postmortem, work log, or investigation report.
- Keep final decisions, verified facts, and reader-relevant rationale. Separate normative rules, rationale and tradeoffs, implementation status, measurements, and roadmap items when they serve different readers or can change independently.
- Use clear connected prose for explanations. Use lists, tables, and code examples for parallel rules, comparisons, APIs, and actual sequences; do not turn every paragraph into bullets or add headings without a useful distinction.
- Remove repeated claims, conversational history, self-reference, generic filler, and implementation detail that does not affect the reader's decisions or the documented contract.
- For API or language design documents, put a small representative API example near the beginning, then define its types, observable behavior, error cases, ownership, and performance or validation constraints as needed. This is a guide, not a mandatory section template.
- Distinguish existing behavior from a proposed design. Verify existing behavior against source, callers, and tests; do not invent product semantics to fill gaps.
- Include references to support decisions, not to recount the research process. Record dates or status only when readers need them to interpret the document.

## Project Boundaries

- Read the applicable repository rules and nearby canonical documents before editing. Preserve their terminology, structure, and link conventions unless the user asks for a change.
- Follow the user's and repository's constraints for README files and temporary artifacts. Keep plans
  and progress notes in the conversation; place file-based temporary records in the operating
  system's temporary directory, not the project checkout, unless the user explicitly directs
  otherwise.
- When maintaining localized editions, preserve equivalent meaning and contract coverage while writing idiomatically for each language.
- Preserve user changes and edit only files needed for the requested document work. Do not create extra indexes, reports, or companion files unless the deliverable requires them.

## Review Before Delivery

- Check that the opening gives the reader the result or contract without requiring conversation history.
- Check that every section serves the document's purpose and that no implementation diary, stale status, duplicate requirement, unsupported claim, or speculative detail remains.
- Verify local links, code examples, names, units, and cross-section consistency. For existing behavior, compare claims with implementation and tests.
- Use only validation relevant to the document. Do not run unrelated builds or tests.
- Report what changed and how it was checked; do not include a chronological account of the editing process.
