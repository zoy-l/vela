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
  DiagnosticSeverity: { Error: 0 }
};
const source = fs.readFileSync(path.join(__dirname, "extension.js"), "utf8");
const { ByteQueue, Connection } = vm.runInNewContext(`${source}\n({ ByteQueue, Connection });`, {
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
  assert.equal(logs.length, 0);
});
