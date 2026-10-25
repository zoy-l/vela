"use strict";

// 由 VS Code 的 --extensionTestsPath 加载，验证实际 provider 分发与编辑器位置语义。
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const vscode = require("vscode");

async function runEditorTests() {
  const extension = vscode.extensions.getExtension("vela.vela-language");
  assert.ok(extension, "Vela extension must be available in the real extension host");
  await extension.activate();
  const root = path.resolve(__dirname, "../..");
  const configurationUri = vscode.Uri.file(path.join(__dirname, "language-configuration.json"));
  await vscode.workspace.openTextDocument(configurationUri);
  await vscode.commands.executeCommand("vscode.executeDocumentSymbolProvider", configurationUri);
  // JSON 验证在文档打开后防抖，等待内置语言服务发布对应的 schema 诊断。
  await new Promise(resolve => setTimeout(resolve, 600));
  const configurationDiagnostics = vscode.languages.getDiagnostics(configurationUri)
    .filter(diagnostic => diagnostic.severity <= vscode.DiagnosticSeverity.Warning);
  assert.deepEqual(configurationDiagnostics.map(diagnostic => diagnostic.message), [],
    "language-configuration.json must satisfy the installed VS Code schema");
  const document = await vscode.workspace.openTextDocument(vscode.Uri.file(path.join(root, "examples", "exceptions.vela")));
  assert.equal(document.languageId, "vela");
  const text = document.getText();
  const offset = text.indexOf("validate(-1)");
  const definitions = await vscode.commands.executeCommand("vscode.executeDefinitionProvider",
    document.uri, document.positionAt(offset));
  assert.ok(definitions.length, "validate must resolve through VS Code's definition command");
  assert.equal(definitions[0].uri.fsPath.toLowerCase(), document.uri.fsPath.toLowerCase());
  assert.equal(definitions[0].range.start.line, text.slice(0, text.indexOf("func validate")).split("\n").length - 1);
  console.log("PASS: exceptions.vela definition at identifier start");
  const atEnd = await vscode.commands.executeCommand("vscode.executeDefinitionProvider",
    document.uri, document.positionAt(offset + "validate".length));
  assert.ok(atEnd.length, "F12 at the end of validate must resolve its declaration");
  console.log("PASS: exceptions.vela definition at identifier end");
  const hovers = await vscode.commands.executeCommand("vscode.executeHoverProvider",
    document.uri, document.positionAt(offset));
  assert.ok(hovers.length, "validate must expose a hover in the real extension host");
  assert.ok(hovers[0].contents.some(content => content.value.includes("func validate(")));
  assert.equal(document.getText(hovers[0].range), "validate");
  console.log("PASS: exceptions.vela hover shows validate's signature");
  const editor = await vscode.window.showTextDocument(document);
  const caret = document.positionAt(offset + "validate".length);
  editor.selection = new vscode.Selection(caret, caret);
  await vscode.commands.executeCommand("editor.action.revealDefinition");
  assert.equal(vscode.window.activeTextEditor.selection.active.line, definitions[0].range.start.line,
    "the real F12 command must navigate to validate's declaration");
  console.log("PASS: exceptions.vela F12 command navigates to the declaration");

  const directory = await fs.mkdtemp(path.join(os.tmpdir(), "vela-vscode-"));
  const uri = vscode.Uri.file(path.join(directory, "exceptions.vela"));
  const temporaryDocuments = [];
  try {
    const standardUri = vscode.Uri.file(path.join(directory, "standard.vela"));
    const standardText = "func main(): void { const values = [2, 1]; values.sort((a: int, b: int): int => a - b); print(Math.sqrt(9.0)); }";
    await fs.writeFile(standardUri.fsPath, standardText);
    const standardDocument = await vscode.workspace.openTextDocument(standardUri);
    temporaryDocuments.push(standardDocument);
    const standardCompletion = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider", standardUri,
      standardDocument.positionAt(standardText.indexOf("Math.sqrt") + "Math.".length));
    assert.ok(standardCompletion.items.some(item => item.label === "sqrt"));
    const arrayHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", standardUri,
      standardDocument.positionAt(standardText.indexOf("sort(")));
    assert.ok(arrayHover[0].contents.some(content => content.value.includes("sort(compare: (int, int) => int)")));
    console.log("PASS: default standard objects and array algorithm signatures work without imports");
    const importedUri = vscode.Uri.file(path.join(directory, "imported.vela"));
    const importingUri = vscode.Uri.file(path.join(directory, "imports.vela"));
    const importingSource = "import './imported';\nfunc main(): int { return answer(); }\n";
    await fs.writeFile(importedUri.fsPath, "export func answer(): int { return 42; }\n");
    await fs.writeFile(importingUri.fsPath, importingSource);
    const importingDocument = await vscode.workspace.openTextDocument(importingUri);
    temporaryDocuments.push(importingDocument);
    const importPosition = importingDocument.positionAt(importingSource.indexOf("imported") + 4);
    const importLinks = await vscode.commands.executeCommand("vscode.executeLinkProvider", importingUri);
    assert.equal(importLinks.length, 1, "the native document-link provider must expose the import underline");
    assert.equal(importingDocument.getText(importLinks[0].range), "./imported",
      "the underline must cover the path without its quotes");
    assert.equal(importLinks[0].target.fsPath.toLowerCase(), importedUri.fsPath.toLowerCase());
    const importDefinitions = await vscode.commands.executeCommand("vscode.executeDefinitionProvider",
      importingUri, importPosition);
    assert.equal(importDefinitions.length, 1, "an import path must expose a Ctrl+Click/F12 target");
    assert.equal(importDefinitions[0].uri.fsPath.toLowerCase(), importedUri.fsPath.toLowerCase());
    const importingEditor = await vscode.window.showTextDocument(importingDocument);
    importingEditor.selection = new vscode.Selection(importPosition, importPosition);
    await vscode.commands.executeCommand("editor.action.revealDefinition");
    const openedImport = vscode.window.activeTextEditor.document;
    temporaryDocuments.push(openedImport);
    assert.equal(openedImport.uri.fsPath.toLowerCase(), importedUri.fsPath.toLowerCase(),
      "Go to Definition on the path must open the imported file");
    console.log("PASS: import path navigation opens the actual module");
    const hoverUri = vscode.Uri.file(path.join(directory, "hover.vela"));
    const hoverText = "/// A documented answer.\nfunc answer(): int { return 42; }\nfunc main(): void { print(answer()); }\n";
    await fs.writeFile(hoverUri.fsPath, hoverText);
    const hoverDocument = await vscode.workspace.openTextDocument(hoverUri);
    temporaryDocuments.push(hoverDocument);
    const hoverPosition = hoverDocument.positionAt(hoverText.indexOf("answer());"));
    const originalHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", hoverUri, hoverPosition);
    assert.ok(originalHover[0].contents.some(content => content.value.includes("A documented answer.")));
    const hoverEdit = new vscode.WorkspaceEdit();
    hoverEdit.replace(hoverUri, new vscode.Range(0, 0, hoverDocument.lineCount, 0),
      hoverText.replace("A documented answer.", "An unsaved answer."));
    assert.equal(await vscode.workspace.applyEdit(hoverEdit), true);
    const updatedHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", hoverUri, hoverPosition);
    assert.ok(updatedHover[0].contents.some(content => content.value.includes("An unsaved answer.")),
      "hover must synchronize unsaved text and invalidate the previous semantic snapshot");
    console.log("PASS: hover renders documentation and updates unsaved text");
    await fs.writeFile(uri.fsPath, text);
    const scratch = await vscode.workspace.openTextDocument(uri);
    temporaryDocuments.push(scratch);
    const insertion = scratch.positionAt(text.indexOf("validate(-1)"));
    const edit = new vscode.WorkspaceEdit();
    edit.insert(uri, insertion, "va\n    ");
    assert.equal(await vscode.workspace.applyEdit(edit), true);
    const completions = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider",
      uri, insertion.translate(0, 2));
    assert.ok(completions.items.some(item => item.label === "validate"),
      "an unsaved va prefix before the next statement must suggest validate");
    console.log("PASS: exceptions.vela completion before an existing statement");
    const scratchEditor = await vscode.window.showTextDocument(scratch);
    const completionCaret = insertion.translate(0, 2);
    scratchEditor.selection = new vscode.Selection(completionCaret, completionCaret);
    const beforeCompletion = scratch.version;
    await vscode.commands.executeCommand("editor.action.triggerSuggest");
    for (let attempt = 0; attempt < 50 && scratch.version === beforeCompletion; ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 20));
      await vscode.commands.executeCommand("acceptSelectedSuggestion");
    }
    assert.notEqual(scratch.version, beforeCompletion,
      "the actual suggestion widget must expose an acceptable completion");
    assert.equal(scratch.lineAt(insertion.line).text.trim(), "validate",
      "accepting the suggestion must replace va with validate");
    console.log("PASS: the actual suggestion widget accepts validate");
    const removePrefix = new vscode.WorkspaceEdit();
    removePrefix.delete(uri, new vscode.Range(insertion, insertion.translate(0, "validate".length)));
    assert.equal(await vscode.workspace.applyEdit(removePrefix), true);
    scratchEditor.selection = new vscode.Selection(insertion, insertion);
    await vscode.commands.executeCommand("hideSuggestWidget");
    await vscode.commands.executeCommand("type", { text: "v" });
    await vscode.commands.executeCommand("type", { text: "a" });
    const typedVersion = scratch.version;
    for (let attempt = 0; attempt < 50 && scratch.version === typedVersion; ++attempt) {
      await new Promise(resolve => setTimeout(resolve, 20));
      await vscode.commands.executeCommand("acceptSelectedSuggestion");
    }
    assert.notEqual(scratch.version, typedVersion,
      "typing va must automatically show a suggestion without Trigger Suggest");
    assert.equal(scratch.lineAt(insertion.line).text.trim(), "validate");
    console.log("PASS: automatic suggestions appear while typing va");
    const memberText = scratch.getText();
    const incompleteCall = memberText.indexOf("validate(-1)") + "validate".length;
    const incompleteDefinitions = await vscode.commands.executeCommand("vscode.executeDefinitionProvider",
      uri, scratch.positionAt(incompleteCall));
    assert.ok(incompleteDefinitions.length, "an unfinished preceding statement must not disable F12");
    const dot = memberText.indexOf("error.message") + "error.".length;
    const members = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider",
      uri, scratch.positionAt(dot), ".");
    assert.ok(members.items.some(item => item.label === "message"), "error. must suggest message");
    console.log("PASS: exceptions.vela member completion with unfinished earlier statement");

    const formattingUri = vscode.Uri.file(path.join(directory, "formatting.vela"));
    await fs.writeFile(formattingUri.fsPath,
      'func greet(name:string):string{return "Hello, ${name}!";}func main():void{const label:string=greet(name:"Vela");print(label);}');
    const formatting = await vscode.workspace.openTextDocument(formattingUri);
    temporaryDocuments.push(formatting);
    await vscode.window.showTextDocument(formatting);
    await vscode.commands.executeCommand("editor.action.formatDocument");
    const expected = "func greet(name: string): string {\n  return 'Hello, ${name}!';\n}\n\n" +
      "func main(): void {\n  const label: string = greet(name: 'Vela');\n  print(label);\n}\n";
    assert.equal(formatting.getText(), expected,
      "Format Document must apply the native spacing and single-quote rules");
    await vscode.commands.executeCommand("editor.action.formatDocument");
    assert.equal(formatting.getText(), expected, "Format Document must be idempotent");
    console.log("PASS: Format Document applies spacing and single quotes");

    const commentEditor = await vscode.window.showTextDocument(formatting);
    commentEditor.selection = new vscode.Selection(0, 0, 0, 0);
    await vscode.commands.executeCommand("editor.action.commentLine");
    assert.ok(formatting.lineAt(0).text.startsWith("//"),
      "the current lineComment object must register a working comment token");
    await vscode.commands.executeCommand("editor.action.commentLine");
    assert.equal(formatting.getText(), expected);
    console.log("PASS: language-configuration comment object works");

    const quoteUri = vscode.Uri.file(path.join(directory, "quotes.vela"));
    const quoteSource = "func main():void{print();}";
    await fs.writeFile(quoteUri.fsPath, quoteSource);
    const quoteDocument = await vscode.workspace.openTextDocument(quoteUri);
    temporaryDocuments.push(quoteDocument);
    const quoteEditor = await vscode.window.showTextDocument(quoteDocument);
    const quoteCaret = quoteDocument.positionAt(quoteSource.indexOf("print(") + "print(".length);
    quoteEditor.selection = new vscode.Selection(quoteCaret, quoteCaret);
    await vscode.commands.executeCommand("type", { text: "'" });
    assert.ok(quoteDocument.getText().includes("print('');"), "single quotes must close automatically");
    console.log("PASS: single-quote auto-closing works");
  } finally {
    // 只丢弃测试创建的临时文档；用户工作区文件从未编辑。
    for (const temporary of temporaryDocuments) {
      if (temporary.isClosed) continue;
      await vscode.window.showTextDocument(temporary);
      await vscode.commands.executeCommand("workbench.action.revertAndCloseActiveEditor");
    }
    await fs.rm(directory, { recursive: true, force: true, maxRetries: 10, retryDelay: 50 });
  }
}

async function run() {
  const report = process.env.VELA_EDITOR_TEST_REPORT;
  assert.ok(report, "Run editor tests through vl editor test");
  try {
    await runEditorTests();
    await fs.writeFile(report, JSON.stringify({ passed: true }));
  } catch (error) {
    await fs.writeFile(report, JSON.stringify({ passed: false, error: error.stack }));
    throw error;
  }
}

module.exports = { run };
