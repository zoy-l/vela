"use strict";

const assert = require("node:assert/strict");
const fs = require("node:fs");
const os = require("node:os");
const path = require("node:path");
const vm = require("node:vm");
const { pathToFileURL } = require("node:url");
const { test } = require("node:test");

const mock = {
  workspace: { textDocuments: [] },
  Uri: { parse: value => ({ toString: () => value }) },
  Range: class { constructor(...values) { this.values = values; } },
  Diagnostic: class { constructor(range, message, severity) { Object.assign(this, { range, message, severity }); } },
  CompletionItem: class { constructor(label, kind) { Object.assign(this, { label, kind }); } },
  Location: class { constructor(uri, range) { Object.assign(this, { uri, range }); } },
  MarkdownString: class { constructor(value) { this.value = value; this.isTrusted = false; } },
  Hover: class { constructor(contents, range) { Object.assign(this, { contents, range }); } },
  DocumentLink: class { constructor(range, target) { Object.assign(this, { range, target }); } },
  EventEmitter: class {
    constructor() { this.listeners = []; this.event = listener => {
      this.listeners.push(listener); return { dispose: () => { this.listeners = this.listeners.filter(item => item !== listener); } };
    }; }
    fire() { for (const listener of this.listeners) listener(); }
    dispose() { this.listeners = []; }
  },
  DiagnosticSeverity: { Error: 0 }
};
const source = fs.readFileSync(path.join(__dirname, "extension.js"), "utf8");
const { ByteQueue, Connection, registerSymbolProviders } = vm.runInNewContext(`${source}\n({ ByteQueue, Connection, registerSymbolProviders });`, {
  require: name => name === "vscode" ? mock : require(name), module: { exports: {} },
  Buffer, process, setTimeout, clearTimeout
});

test("byte queue preserves fragmented UTF-8 and releases consumed chunks", () => {
  const queue = new ByteQueue();
  const expected = Buffer.from("中文😀 and bytes", "utf8");
  for (const byte of expected) queue.push(Buffer.from([byte]));
  assert.deepEqual(queue.peek(expected.length), expected);
  assert.equal(queue.size, expected.length);
  assert.deepEqual(queue.read(expected.length), expected);
  assert.equal(queue.size, 0);
  assert.equal(queue.chunks.length, 0);
  queue.push(Buffer.from("next"));
  assert.equal(queue.read(2).toString(), "ne");
  queue.push(Buffer.from(" time"));
  assert.equal(queue.read(queue.size).toString(), "xt time");
});

test("client decodes byte-at-a-time headers and multiple Unicode frames", () => {
  const client = Object.create(Connection.prototype);
  client.queue = new ByteQueue();
  client.length = null;
  const messages = [];
  client.dispatch = message => messages.push(message);
  const expected = [{ text: "你好😀" }, { id: 2, result: [] }];
  const frames = expected.map(message => {
    const body = Buffer.from(JSON.stringify(message));
    return Buffer.concat([Buffer.from(`Content-Length: ${body.length}\r\n\r\n`), body]);
  });
  for (const byte of Buffer.concat(frames)) client.receive(Buffer.from([byte]));
  assert.equal(JSON.stringify(messages), JSON.stringify(expected));
  assert.equal(client.queue.size, 0);
  assert.throws(() => client.receive(Buffer.from("Content-Length: 67108865\r\n\r\n")), /Oversized/);
});

test("extension connection initializes, formats, publishes diagnostics and shuts down", { timeout: 10000 }, async context => {
  const root = path.resolve(__dirname, "../..");
  const executable = process.env.VELA_TEST_EXECUTABLE || path.join(root, "out", "debug", process.platform === "win32" ? "vela.exe" : "vela");
  const directory = fs.mkdtempSync(path.join(os.tmpdir(), "vela-editor-"));
  const uri = pathToFileURL(path.join(directory, "你好.vela")).toString();
  const logs = [];
  const diagnostics = new Map();
  const connection = new Connection(executable, root, {
    append: text => logs.push(text), appendLine: text => logs.push(text)
  }, { set: (key, value) => diagnostics.set(key.toString(), value) });
  context.after(async () => { await connection.stop(); fs.rmSync(directory, { recursive: true, force: true }); });
  await connection.ready;
  connection.notify("textDocument/didOpen", { textDocument: {
    uri, version: 1, languageId: "vela", text: "func main():int{return 42;}"
  }});
  const first = await connection.request("textDocument/formatting", { textDocument: { uri }, options: {} });
  assert.equal(first[0].newText, "func main(): int {\n  return 42;\n}\n");
  assert.equal(diagnostics.get(uri).length, 0);
  connection.notify("textDocument/didChange", { textDocument: { uri, version: 2 }, contentChanges: [
    { text: 'func main(): int { const label = "中文😀"; return true; }' }
  ]});
  await connection.request("textDocument/formatting", { textDocument: { uri } });
  assert.equal(diagnostics.get(uri).length, 1);
  assert.match(diagnostics.get(uri)[0].message, /return/);
  const text = "func answer(): int { return 42; }\nfunc main(): int { return answer(); }";
  connection.notify("textDocument/didChange", { textDocument: { uri, version: 3 }, contentChanges: [{ text }] });
  const completions = await connection.request("textDocument/completion", {
    textDocument: { uri }, position: { line: 1, character: 26 }
  });
  assert.ok(completions.some(item => item.label === "answer"));
  const target = await connection.request("textDocument/definition", {
    textDocument: { uri }, position: { line: 1, character: 26 }
  });
  assert.equal(target.uri, uri);
  assert.equal(target.range.start.line, 0);
  assert.equal(target.range.start.character, 5);
  assert.equal(logs.length, 0);
});

test("VS Code symbol providers synchronize text, map protocol results and discard stale replies", async () => {
  const providers = {};
  mock.languages = {
    registerDocumentLinkProvider(selector, provider) {
      assert.equal(selector.language, "vela");
      providers.links = provider;
      return { dispose() {} };
    },
    registerCompletionItemProvider(selector, provider, trigger) {
      assert.equal(selector.language, "vela");
      assert.equal(trigger, ".");
      providers.completion = provider;
      return { dispose() {} };
    },
    registerDefinitionProvider(selector, provider) {
      assert.equal(selector.scheme, "file");
      providers.definition = provider;
      return { dispose() {} };
    },
    registerHoverProvider(selector, provider) {
      assert.equal(selector.language, "vela");
      providers.hover = provider;
      return { dispose() {} };
    }
  };
  const document = { uri: { toString: () => "file:///test.vela" }, version: 1, isClosed: false };
  const token = { isCancellationRequested: false };
  const position = { line: 2, character: 5 };
  const events = [];
  const range = { start: { line: 2, character: 3 }, end: { line: 2, character: 7 } };
  let stale = false;
  const client = { async request(method, params, cancellation) {
    events.push(method);
    assert.equal(cancellation, token);
    if (stale) document.version++;
    if (method.endsWith("documentLink")) {
      assert.equal(params.textDocument.uri, "file:///test.vela");
      assert.equal(params.position, undefined);
      return [{ target: "file:///library.vela", range, tooltip: "Open imported Vela module" }];
    }
    assert.equal(params.position.line, 2);
    assert.equal(params.position.character, 5);
    if (method.endsWith("hover")) return { contents: { kind: "markdown", value: "```vela\nvalue: int\n```" }, range };
    return method.endsWith("completion")
      ? [{ label: "value", kind: 5, detail: "int", textEdit: { range, newText: "value" } }]
      : { uri: "file:///library.vela", range };
  }};
  const context = { subscriptions: [] };
  const refreshLinks = registerSymbolProviders(context, client, async () => { events.push("flush"); });
  assert.equal(context.subscriptions.length, 5);
  const completions = await providers.completion.provideCompletionItems(document, position, token);
  assert.equal(completions[0].label, "value");
  assert.equal(completions[0].kind, 4);
  assert.equal(completions[0].insertText, "value");
  assert.deepEqual(completions[0].range.values, [2, 3, 2, 7]);
  const target = await providers.definition.provideDefinition(document, position, token);
  assert.equal(target.uri.toString(), "file:///library.vela");
  assert.deepEqual(events, ["flush", "textDocument/completion", "flush", "textDocument/definition"]);
  const hover = await providers.hover.provideHover(document, position, token);
  assert.equal(hover.contents.value, "```vela\nvalue: int\n```");
  assert.equal(hover.contents.isTrusted, false);
  assert.deepEqual(hover.range.values, [2, 3, 2, 7]);
  const links = await providers.links.provideDocumentLinks(document, token);
  assert.equal(links[0].target.toString(), "file:///library.vela");
  assert.equal(links[0].tooltip, "Open imported Vela module");
  assert.deepEqual(links[0].range.values, [2, 3, 2, 7]);
  let invalidations = 0;
  providers.links.onDidChangeDocumentLinks(() => invalidations++);
  refreshLinks();
  assert.equal(invalidations, 1);
  stale = true;
  assert.equal((await providers.completion.provideCompletionItems(document, position, token)).length, 0);
  assert.equal(await providers.definition.provideDefinition(document, position, token), null);
  assert.equal(await providers.hover.provideHover(document, position, token), null);
  assert.equal((await providers.links.provideDocumentLinks(document, token)).length, 0);
  token.isCancellationRequested = true;
  const count = events.length;
  assert.equal((await providers.links.provideDocumentLinks(document, token)).length, 0);
  assert.equal((await providers.completion.provideCompletionItems(document, position, token)).length, 0);
  assert.equal(await providers.hover.provideHover(document, position, token), null);
  assert.equal(events.length, count);
});
