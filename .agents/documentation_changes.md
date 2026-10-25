# Documentation Changes

These rules apply to documentation work inside the project checkout.

- Treat the project-root `README.md` and `README.zh-CN.md` as read-only unless the user explicitly
  asks to change them. Do not edit them incidentally to synchronize links or descriptions during
  other documentation work.
- Keep task plans and progress notes in the conversation. Store file-based temporary execution
  documents or records, including command logs, scratch reports, and intermediate write-ups, only in
  the operating system's temporary directory (`%TEMP%` on Windows and `$TMPDIR` on macOS/Linux), not
  in the project checkout. Create an in-project document only when it is a requested, durable project
  artifact; create a temporary execution document in the project only when the user explicitly asks
  for that location.
