"use strict";

// 由 VS Code 的 --extensionTestsPath 加载，验证实际 provider 分发与编辑器位置语义。
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const os = require("node:os");
const path = require("node:path");
const vscode = require("vscode");

async function runVelaTask(file, stop = false) {
  let listener, started, timer;
  const matches = event => event.execution.task.source === "Vela"
    && event.execution.task.execution.args[1] === file;
  const finished = new Promise((resolve, reject) => {
    timer = setTimeout(() => reject(new Error("Vela run task did not finish")), 20000);
    listener = vscode.tasks.onDidEndTaskProcess(event => { if (matches(event)) resolve(event); });
    if (stop) started = vscode.tasks.onDidStartTaskProcess(event => {
      if (matches(event)) event.execution.terminate();
    });
  });
  try {
    const [execution, event] = await Promise.all([vscode.commands.executeCommand("vela.runFile"), finished]);
    assert.ok(execution, "Run File must return an actual task execution");
    assert.equal(event.execution, execution);
    assert.ok(execution.task.execution instanceof vscode.ProcessExecution);
    return event.exitCode;
  } catch (error) {
    for (const execution of vscode.tasks.taskExecutions) {
      if (matches({ execution })) execution.terminate();
    }
    throw error;
  } finally {
    clearTimeout(timer);
    listener.dispose();
    started?.dispose();
  }
}

async function readSemanticTokens(document) {
  const uri = document.uri;
  const legend = await vscode.commands.executeCommand("vscode.provideDocumentSemanticTokensLegend", uri);
  const tokens = await vscode.commands.executeCommand("vscode.provideDocumentSemanticTokens", uri);
  assert.ok(legend && tokens, "Vela must provide semantic tokens in the extension host");
  const result = [];
  let line = 0, character = 0;
  for (let index = 0; index < tokens.data.length; index += 5) {
    const [deltaLine, deltaCharacter, length, kind, modifiers] = tokens.data.slice(index, index + 5);
    line += deltaLine;
    character = deltaLine ? deltaCharacter : character + deltaCharacter;
    const range = new vscode.Range(line, character, line, character + length);
    result.push({
      offset: document.offsetAt(range.start),
      spelling: document.getText(range),
      kind: legend.tokenTypes[kind],
      modifiers: legend.tokenModifiers.filter((_, bit) => modifiers & (1 << bit))
    });
  }
  return result;
}

async function checkTypeHighlighting(document, source) {
  const syntax = await vscode.commands.executeCommand("_workbench.captureSyntaxTokens", document.uri);
  const syntaxAt = new Map();
  let syntaxOffset = 0;
  for (const token of syntax) {
    syntaxAt.set(syntaxOffset, token);
    syntaxOffset += token.c.length;
  }
  assert.equal(syntaxOffset, source.replaceAll("\n", "").length,
    "captured syntax tokens must cover the document apart from its line breaks");
  const semantic = new Map((await readSemanticTokens(document)).map(token => [token.offset, token]));
  for (const [context, spelling, scope, kind] of [
    ["as lib;", "lib", "entity.name.namespace"],
    ["callback<T extends", "T", "entity.name.type.parameter", "typeParameter"],
    ["T extends payload", "payload", "entity.name.type", "class"],
    ["extends payload implements", "payload", "entity.name.type", "class"],
    ["implements reader<payload>", "reader", "entity.name.type", "interface"],
    [", lib.measurable", "measurable", "entity.name.type", "interface"],
    ["field: payload?", "payload", "entity.name.type", "class"],
    ["handler: callback<payload>", "callback", "entity.name.type", "type"],
    ["entries: Map<string", "Map", "support.type"],
    ["Map<string, lib.remote?[]>", "remote", "entity.name.type", "class"],
    ["constructor(initial: payload)", "payload", "entity.name.type", "class"],
    ["get current(): payload?", "payload", "entity.name.type", "class"],
    ["set current(value: payload?)", "payload", "entity.name.type", "class"],
    ["const variable:\n    payload", "payload", "entity.name.type", "class"],
    ["(input: payload):", "payload", "entity.name.type", "class"],
    ["): payload => input", "payload", "entity.name.type", "class"],
    ["value is\n    lib.remote", "remote", "entity.name.type", "class"],
    ["value is payload?", "payload", "entity.name.type", "class"],
    ["value is payload?", "?", "keyword.operator.type.nullable"],
    ["value as payload;", "payload", "entity.name.type", "class"],
    ["lib.identity<lib.remote>", "remote", "entity.name.type", "class"],
    ["catch (error: lib.remote)", "remote", "entity.name.type", "class"],
    ["consume(value: fallback)", "fallback", "variable.other.readwrite"],
    ["'fallback': fallback", "fallback", "variable.other.readwrite"],
    ["flag ? variable : fallback", "fallback", "variable.other.readwrite"],
    ["count < other && other > count", "other", "variable.other.readwrite"]
  ]) {
    const start = source.indexOf(context);
    assert.ok(start >= 0, context);
    const offset = start + context.lastIndexOf(spelling);
    const token = syntaxAt.get(offset - document.positionAt(offset).line);
    assert.equal(token?.c, spelling, context);
    assert.ok(token.t.split(" ").includes(scope + ".vela"), context + ": " + token.t);
    if (scope.startsWith("entity.name.type") || scope.startsWith("support.type")) {
      for (const theme of ["dark_modern", "light_modern", "dark_plus", "light_plus"]) {
        assert.ok(token.r[theme] && !token.r[theme].startsWith("default:"), context + " in " + theme);
      }
    }
    if (kind) {
      assert.equal(semantic.get(offset)?.kind, kind, context);
    }
  }

  const field = document.positionAt(source.indexOf("payload? = null"));
  const edit = new vscode.WorkspaceEdit();
  edit.replace(document.uri, new vscode.Range(field, field.translate(0, "payload".length)), "lib.remote");
  assert.equal(await vscode.workspace.applyEdit(edit), true);
  const edited = (await readSemanticTokens(document)).find(token =>
    token.offset === document.offsetAt(field) + "lib.".length);
  assert.equal(edited?.spelling, "remote", "unsaved changes must update the field's semantic type");
  assert.equal(edited.kind, "class");
}

async function runEditorTests() {
  const extension = vscode.extensions.getExtension("vela.vela-language");
  assert.ok(extension, "Vela extension must be available in the real extension host");
  await extension.activate();
  const language = extension.packageJSON.contributes.languages.find(item => item.id === "vela");
  for (const variant of ["light", "dark"]) {
    const svg = await fs.readFile(path.join(extension.extensionPath, language.icon[variant]), "utf8");
    assert.ok(svg.includes('viewBox="0 0 16 16"'), "language file icons must be bundled for both themes");
  }
  const root = path.resolve(__dirname, "../..");
  const structUri = vscode.Uri.file(path.join(root, "examples", "struct.vela"));
  const structDocument = await vscode.workspace.openTextDocument(structUri);
  const structSyntax = await vscode.commands.executeCommand("_workbench.captureSyntaxTokens", structUri);
  for (const [spelling, scope] of [["struct", "storage.type.vela"], ["Point", "entity.name.type.vela"]]) {
    const token = structSyntax.find(token => token.c === spelling && token.t.split(" ").includes(scope));
    assert.ok(token, `${spelling} must receive ${scope}`);
    for (const theme of ["dark_modern", "light_modern", "dark_plus", "light_plus"]) {
      assert.ok(token.r[theme] && !token.r[theme].startsWith("default:"),
        `${spelling} must have a syntax color in ${theme}`);
    }
  }
  const structSource = structDocument.getText();
  const structTypes = (await readSemanticTokens(structDocument)).filter(token => token.kind === "struct");
  assert.deepEqual(structTypes.map(({ offset, spelling, modifiers }) => [offset, spelling, modifiers]), [
    [structSource.indexOf("Point {"), "Point", ["declaration"]],
    [structSource.indexOf("Point(x:"), "Point", []],
    [structSource.indexOf("Point[]"), "Point", []]
  ]);
  console.log("PASS: struct keywords and names are highlighted in the runnable example");

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
  for (let attempt = 0; attempt < 50
    && vscode.window.activeTextEditor.selection.active.line !== definitions[0].range.start.line; ++attempt) {
    await new Promise(resolve => setTimeout(resolve, 20));
  }
  assert.equal(vscode.window.activeTextEditor.selection.active.line, definitions[0].range.start.line,
    "the real F12 command must navigate to validate's declaration");
  console.log("PASS: exceptions.vela F12 command navigates to the declaration");

  const directory = await fs.realpath(await fs.mkdtemp(path.join(os.tmpdir(), "vela-vscode-")));
  const uri = vscode.Uri.file(path.join(directory, "exceptions.vela"));
  const temporaryDocuments = [];
  const openTemporary = async (uri, text) => {
    await fs.writeFile(uri.fsPath, text, { encoding: "utf8" });
    const opened = await vscode.workspace.openTextDocument(uri);
    temporaryDocuments.push(opened);
    return opened;
  };
  try {
    const asyncUri = vscode.Uri.file(path.join(directory, "async-highlighting.vela"));
    const asyncText = "async func main(): Future<int> {\n" +
      "  // async await Future override\n" +
      "  const label = 'async await Future override';\n" +
      "  const overrideValue = 1;\n" +
      "  const remainder = 43 % (5);\n" +
      "  const futureValue = Future.value(42);\n" +
      "  return await futureValue;\n}\n" +
      "class Base {\n" +
      "  value: int = 1;\n" +
      "  read(): int { return this.value; }\n" +
      "  get amount(): int { return this.value; }\n" +
      "  set amount(value: int): void { this.value = value; }\n}\n" +
      "class Child extends Base {\n" +
      "  override read(): int { return 2; }\n" +
      "  override get amount(): int { return this.value + 1; }\n" +
      "  override set amount(value: int): void { this.value = value + 1; }\n}\n";
    const asyncDocument = await openTemporary(asyncUri, asyncText);
    const syntaxTokens = await vscode.commands.executeCommand("_workbench.captureSyntaxTokens", asyncUri);
    for (const [spelling, scope, count] of [
      ["async", "storage.modifier.vela", 1],
      ["override", "storage.modifier.vela", 3],
      ["await", "keyword.control.vela", 1],
      ["Future", "support.type.vela", 2]
    ]) {
      const highlighted = syntaxTokens.filter(token => token.c === spelling && token.t.split(" ").includes(scope));
      assert.equal(highlighted.length, count, `${spelling} must receive ${scope} outside comments and strings`);
      for (const token of highlighted) {
        for (const theme of ["dark_plus", "light_plus", "hc_black"]) {
          assert.ok(!token.r[theme].startsWith("default:"),
            `${spelling} must have a syntax color in ${theme}`);
        }
      }
    }
    assert.equal(syntaxTokens.filter(token => token.c === "%"
      && token.t.split(" ").includes("keyword.operator.vela")).length, 1,
      "remainder must receive the arithmetic operator scope");
    assert.ok(syntaxTokens.some(token => token.c === "futureValue"
      && token.t.split(" ").includes("variable.other.constant.vela")),
      "const declarations must use the theme's constant scope");
    assert.deepEqual((await readSemanticTokens(asyncDocument))
      .filter(token => token.spelling === "futureValue")
      .map(({ kind, modifiers }) => [kind, modifiers]),
      [["variable", ["declaration", "readonly"]], ["variable", ["readonly"]]],
      "const declarations and references must use the readonly variable token");
    const futureTokens = (await readSemanticTokens(asyncDocument)).filter(token => token.spelling === "Future");
    assert.deepEqual(futureTokens.map(({ kind, modifiers }) => [kind, modifiers]),
      [["type", ["defaultLibrary"]], ["type", ["defaultLibrary"]]],
      "Future annotations and factory receivers must keep built-in type coloring");
    console.log("PASS: async, await, Future and override have syntax colors and preserve semantic type coloring");

    const parameterUri = vscode.Uri.file(path.join(directory, "parameter-highlighting.vela"));
    const parameterDocument = await openTemporary(parameterUri, [
      "class Sample {",
      "  #values: double[][] = [[0.0]];",
      "  update(action: () => void): void { action(); }",
      "  #change(index: int, values: double[]): void {",
      "    this.update(() => { this.#values[index] = values; });",
      "  }",
      "}",
    ].join("\n"));
    assert.deepEqual((await readSemanticTokens(parameterDocument))
      .filter(token => token.spelling === "index" || token.spelling === "values")
      .map(({ spelling, kind, modifiers }) => [spelling, kind, modifiers]),
      [["index", "parameter", ["declaration"]], ["values", "parameter", ["declaration"]],
        ["index", "parameter", []], ["values", "parameter", []]],
      "method parameters and their closure captures must use the same parameter coloring");

    await fs.writeFile(path.join(directory, "highlight-library.vela"), [
      "export class remote {}",
      "export interface measurable { measure(): int; }",
      "export func identity<T>(value: T): T { return value; }",
      ""
    ].join("\n"));
    const typeUri = vscode.Uri.file(path.join(directory, "type-highlighting.vela"));
    const typeText = [
      "import 'highlight-library' as lib;",
      "class payload {}",
      "interface reader<T> { read(): T; }",
      "type callback<T extends payload> = (T) => lib.remote;",
      "class highlighted extends payload implements reader<payload>, lib.measurable {",
      "  field: payload? = null;",
      "  handler: callback<payload>? = null;",
      "  static entries: Map<string, lib.remote?[]> = {};",
      "  constructor(initial: payload) { this.field = initial; }",
      "  get current(): payload? { return this.field; }",
      "  set current(value: payload?): void { this.field = value; }",
      "  read(): payload { return payload(); }",
      "  measure(): int { return 1; }",
      "}",
      "func consume(value: payload): payload { return value; }",
      "func choose(value: dynamic, flag: bool, fallback: payload, count: int, other: int): payload {",
      "  const variable:",
      "    payload = fallback;",
      "  const fn = (input: payload): payload => input;",
      "  const tested = value is",
      "    lib.remote;",
      "  const nullable = value is payload?;",
      "  const cast = value as payload;",
      "  const explicit = lib.identity<lib.remote>(value as lib.remote);",
      "  const chosen = flag ? variable : fallback;",
      "  const mapped = {'fallback': fallback};",
      "  const smaller = count < other && other > count;",
      "  try {} catch (error: lib.remote) {}",
      "  return consume(value: fallback);",
      "}",
      ""
    ].join("\n");
    const typeDocument = await openTemporary(typeUri, typeText);
    await checkTypeHighlighting(typeDocument, typeText);
    console.log("PASS: type annotations, type tests, casts and generic arguments use the editor's type colors");

    const enumUri = vscode.Uri.file(path.join(directory, "enum-highlighting.vela"));
    const enumText = "enum Palette { light, dark }\n" +
      "class Item { light: int = 0; }\n" +
      "func main(): void { const color: Palette = Palette.dark; print(Item().light); }\n";
    const enumDocument = await openTemporary(enumUri, enumText);
    const enumSyntax = await vscode.commands.executeCommand("_workbench.captureSyntaxTokens", enumUri);
    const enumMembers = enumSyntax.filter(token =>
      token.t.split(" ").includes("variable.other.enummember.vela"));
    assert.deepEqual(enumMembers.map(token => token.c), ["light", "dark"]);
    for (const token of enumMembers) {
      for (const theme of ["dark_modern", "light_modern", "dark_plus", "light_plus"]) {
        assert.ok(token.r[theme] && !token.r[theme].startsWith("default:"),
          "enum members must use the theme's enum member color");
      }
    }
    const enumSemantic = await readSemanticTokens(enumDocument);
    assert.deepEqual(enumSemantic.filter(token => token.kind === "enumMember")
      .map(token => [token.spelling, token.modifiers]),
    [["light", ["declaration"]], ["dark", ["declaration"]], ["dark", []]]);
    console.log("PASS: enum declarations and member references have enum colors without coloring instance fields");

    await fs.writeFile(path.join(directory, "warning-library.vela"),
      "export func echo(value: int): int { return value; }\n");
    const warningUri = vscode.Uri.file(path.join(directory, "unused-warnings.vela"));
    const warningText = "import 'warning-library' as unused;\nfunc consume(value: int): void {}\n";
    const warningDocument = await openTemporary(warningUri, warningText);
    await readSemanticTokens(warningDocument);
    const warnings = vscode.languages.getDiagnostics(warningUri);
    assert.deepEqual(warnings.map(diagnostic => diagnostic.code), ["unused_import", "unused_parameter"]);
    for (const diagnostic of warnings) {
      assert.equal(diagnostic.severity, vscode.DiagnosticSeverity.Warning);
      assert.deepEqual(diagnostic.tags, [vscode.DiagnosticTag.Unnecessary]);
      assert.ok(!diagnostic.range.isEmpty, "unused declarations must have a range for editor fading");
    }
    const warningEdit = new vscode.WorkspaceEdit();
    warningEdit.replace(warningUri, new vscode.Range(0, 0, warningDocument.lineCount, 0),
      warningText.replace("void {}", "void { print(unused.echo(value)); }"));
    assert.equal(await vscode.workspace.applyEdit(warningEdit), true);
    await readSemanticTokens(warningDocument);
    assert.deepEqual(vscode.languages.getDiagnostics(warningUri), [],
      "using the parameter and import in an unsaved edit must clear the warnings");
    console.log("PASS: unused parameters and imports show warnings and fading, then clear after edits");

    const referenceUri = vscode.Uri.file(path.join(directory, "references.vela"));
    const referenceText = "export func target(value: int): int { return value; }\n"
      + "func invoke(): int { return target(1); }\n";
    const referenceDocument = await openTemporary(referenceUri, referenceText);
    const referencePosition = referenceDocument.positionAt(referenceText.indexOf("target"));
    const references = await vscode.commands.executeCommand("vscode.executeReferenceProvider",
      referenceUri, referencePosition);
    assert.ok(references.some(reference => reference.uri.toString() === referenceUri.toString()
      && reference.range.start.line === 1), "Find All References must reach the language server");
    assert.ok(references.every(reference => referenceDocument.getText(reference.range) === "target"));
    console.log("PASS: Find All References returns semantic call locations through the real provider");

    const runUri = vscode.Uri.file(path.join(directory, "run $shell; 中文.vela"));
    const runDocument = await openTemporary(runUri, "func main(): int { return 1; }\n");
    await vscode.window.showTextDocument(runDocument);
    const runEdit = new vscode.WorkspaceEdit();
    const runText = "func main(): int { print('Run File works'); return 23; }\n";
    runEdit.replace(runUri, new vscode.Range(0, 0, runDocument.lineCount, 0), runText);
    assert.equal(await vscode.workspace.applyEdit(runEdit), true);
    assert.ok(runDocument.isDirty);
    assert.equal(await runVelaTask(runUri.fsPath), 0,
      "the task must save and successfully execute the current edits");
    assert.equal(await fs.readFile(runUri.fsPath, "utf8"), runText);
    assert.equal(runDocument.isDirty, false);
    console.log("PASS: Run File saves edits and executes paths containing spaces, Unicode and shell characters");

    const invalidUri = vscode.Uri.file(path.join(directory, "run-invalid.vela"));
    const invalidDocument = await openTemporary(invalidUri, "func main(): int { return true; }\n");
    await vscode.window.showTextDocument(invalidDocument);
    assert.equal(await runVelaTask(invalidUri.fsPath), 65, "compile errors must reach the task terminal");
    console.log("PASS: Run File preserves compilation failure status");

    const waitingUri = vscode.Uri.file(path.join(directory, "run-waiting.vela"));
    const waitingDocument = await openTemporary(waitingUri, "async func main(): Future<void> { await sleep(60000); }\n");
    await vscode.window.showTextDocument(waitingDocument);
    await runVelaTask(waitingUri.fsPath, true);
    console.log("PASS: a running Vela task can be terminated");

    const standardUri = vscode.Uri.file(path.join(directory, "standard.vela"));
    const standardText = "func main(): void { const values = [2, 1]; values.sort((a: int, b: int): int => a - b); print(Math.sqrt(9.0)); }";
    const standardDocument = await openTemporary(standardUri, standardText);
    const standardCompletion = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider", standardUri,
      standardDocument.positionAt(standardText.indexOf("Math.sqrt") + "Math.".length));
    assert.ok(standardCompletion.items.some(item => item.label === "sqrt"));
    const arrayHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", standardUri,
      standardDocument.positionAt(standardText.indexOf("sort(")));
    assert.ok(arrayHover[0].contents.some(content => content.value.includes("sort(compareFn?: (int, int) => int)")));
    assert.ok(arrayHover[0].contents.some(content => content.value.includes("Sorts in place")));
    const mathHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", standardUri,
      standardDocument.positionAt(standardText.indexOf("sqrt(")));
    assert.ok(mathHover[0].contents.some(content => content.value.includes("square root")));
    const sqrtCompletion = standardCompletion.items.find(item => item.label === "sqrt");
    assert.ok(sqrtCompletion.documentation.value.includes("square root"));
    console.log("PASS: default standard objects and array algorithm signatures work without imports");
    const typeHoverUri = vscode.Uri.file(path.join(directory, "type-hover.vela"));
    const typeHoverText = "/// Brightness levels.\nenum Brightness { light, dark }\n" +
      "/// A readable value.\ninterface Readable { read(scale: int): int; }\n" +
      "type Callback<T> = (T) => string;\n" +
      "func use(value: Readable): int { return value.read(2); }\n" +
      "func main(): void { const convert: Callback<int> = value => value.toString(); print(Brightness.dark); }\n";
    const typeHoverDocument = await openTemporary(typeHoverUri, typeHoverText);
    for (const [marker, expected] of [
      ["Brightness {", "enum Brightness\n```\n\nBrightness levels."],
      ["dark);", "(enum member) Brightness.dark = 1"],
      ["Readable {", "interface Readable\n```\n\nA readable value."],
      ["read(2)", "(method) Readable.read(scale: int): int"],
      ["Callback<int>", "type Callback<T> = (T) => string"]
    ]) {
      const typeHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", typeHoverUri,
        typeHoverDocument.positionAt(typeHoverText.indexOf(marker)));
      assert.ok(typeHover[0].contents.some(content => content.value.includes(expected)), expected);
    }
    console.log("PASS: enum, interface, member and generic alias hovers follow TypeScript conventions");
    const mutableStringUri = vscode.Uri.file(path.join(directory, "mutable-string.vela"));
    const mutableStringText = "func main(): void { const text: String = String(); text.append('x'); print(text.toString()); }\n";
    const mutableStringDocument = await openTemporary(mutableStringUri, mutableStringText);
    const mutableStringHover = await vscode.commands.executeCommand("vscode.executeHoverProvider", mutableStringUri,
      mutableStringDocument.positionAt(mutableStringText.indexOf("String()")));
    assert.ok(mutableStringHover[0].contents.some(content => content.value.includes("class String {\n  constructor();\n}")));
    const mutableStringCompletion = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider", mutableStringUri,
      mutableStringDocument.positionAt(mutableStringText.indexOf("text.append") + "text.".length));
    assert.ok(mutableStringCompletion.items.some(item => item.label === "append"));
    assert.ok(mutableStringCompletion.items.some(item => item.label === "toString"));
    console.log("PASS: String constructor hover and mutable string member completion use the renamed API");
    const argumentUri = vscode.Uri.file(path.join(directory, "arguments.vela"));
    const argumentText = "class Counter { value: int; step: int = 1; }\nfunc main(): void { Counter(); }\n";
    const argumentDocument = await openTemporary(argumentUri, argumentText);
    const argumentOffset = argumentText.lastIndexOf("Counter(") + "Counter(".length;
    const classHover = await vscode.commands.executeCommand("vscode.executeHoverProvider",
      argumentUri, argumentDocument.positionAt(argumentOffset - "Counter(".length));
    assert.ok(classHover[0].contents.some(content => content.value.includes("constructor(value: int, step: int = 1)")));
    const classUses = (await readSemanticTokens(argumentDocument)).filter(token =>
      token.kind === "class" && token.spelling === "Counter");
    assert.equal(classUses.length, 2, "class declaration and constructor use must both receive class coloring");
    const argumentsList = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider",
      argumentUri, argumentDocument.positionAt(argumentOffset), "(");
    const valueArgument = argumentsList.items.find(item => item.label === "value:");
    assert.ok(valueArgument, "constructors must offer actual named-argument code completion");
    assert.equal(valueArgument.insertText, "value: ");
    assert.equal(valueArgument.detail, "value: int");
    assert.ok(argumentsList.items.some(item => item.label === "step:" && item.detail.includes("= …")));
    const argumentEdit = new vscode.WorkspaceEdit();
    argumentEdit.replace(argumentUri, valueArgument.range, valueArgument.insertText + "40, ");
    assert.equal(await vscode.workspace.applyEdit(argumentEdit), true);
    assert.ok(argumentDocument.getText().includes("Counter(value: 40, )"));
    const secondArguments = await vscode.commands.executeCommand("vscode.executeCompletionItemProvider",
      argumentUri, argumentDocument.positionAt(argumentDocument.getText().indexOf("40, ") + 4), ",");
    assert.ok(secondArguments.items.some(item => item.label === "step:"));
    assert.ok(!secondArguments.items.some(item => item.label === "value:"), "filled parameters must be excluded");
    const signature = await vscode.commands.executeCommand("vscode.executeSignatureHelpProvider",
      argumentUri, argumentDocument.positionAt(argumentOffset));
    assert.ok(!signature || signature.signatures.length === 0, "parameter tooltips must be removed");
    console.log("PASS: constructor code completion inserts named arguments and removes filled parameters");
    const importedUri = vscode.Uri.file(path.join(directory, "imported.vela"));
    const importingUri = vscode.Uri.file(path.join(directory, "imports.vela"));
    const importingSource = "import './imported';\nfunc main(): int { return answer(); }\n";
    await fs.writeFile(importedUri.fsPath, "export func answer(): int { return 42; }\n");
    const importingDocument = await openTemporary(importingUri, importingSource);
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
    const hoverDocument = await openTemporary(hoverUri, hoverText);
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
    const scratch = await openTemporary(uri, text);
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
    const formatting = await openTemporary(formattingUri,
      'func greet(name:string):string{return "Hello, ${name}!";}func main():void{const label:string=greet(name:"Vela");print(label);}');
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
    const quoteDocument = await openTemporary(quoteUri, quoteSource);
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
