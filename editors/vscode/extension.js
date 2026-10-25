"use strict";

const vscode = require("vscode");
const childProcess = require("node:child_process");
const path = require("node:path");

// 块队列只在完整消息跨块时复制一次 body，避免大文档的反复 Buffer.concat。
class ByteQueue {
  constructor() { this.chunks = []; this.head = 0; this.offset = 0; this.size = 0; }
  push(bytes) { this.chunks.push(bytes); this.size += bytes.length; }
  peek(count) {
    const first = this.chunks[this.head];
    if (first.length - this.offset >= count) return first.subarray(this.offset, this.offset + count);
    const result = Buffer.allocUnsafe(count);
    let copied = 0;
    for (let index = this.head; copied < count; ++index) {
      const offset = index === this.head ? this.offset : 0;
      copied += this.chunks[index].copy(result, copied, offset,
        offset + Math.min(count - copied, this.chunks[index].length - offset));
    }
    return result;
  }
  read(count) {
    const result = this.peek(count);
    this.size -= count;
    while (count > 0) {
      const available = this.chunks[this.head].length - this.offset;
      const consumed = Math.min(available, count);
      this.offset += consumed;
      count -= consumed;
      if (this.offset === this.chunks[this.head].length) { ++this.head; this.offset = 0; }
    }
    if (this.head === this.chunks.length) { this.chunks = []; this.head = 0; }
    else if (this.head > 64) { this.chunks = this.chunks.slice(this.head); this.head = 0; }
    return result;
  }
}

class Connection {
  constructor(executable, root, output, diagnostics) {
    this.output = output;
    this.diagnostics = diagnostics;
    this.pending = new Map();
    this.sequence = 0;
    this.queue = new ByteQueue();
    this.length = null;
    this.closed = false;
    this.child = childProcess.spawn(executable, ["lsp"], { cwd: root, windowsHide: true });
    this.child.stdout.on("data", bytes => {
      try { this.receive(bytes); }
      catch (error) { this.fail(error); this.child.kill(); }
    });
    this.child.stderr.on("data", bytes => output.append(bytes.toString("utf8")));
    this.child.stdin.on("error", error => this.fail(error));
    this.child.on("error", error => this.fail(error));
    this.child.on("close", code => this.fail(new Error(`Vela language server exited (${code}).`)));
    this.ready = this.request("initialize", { processId: process.pid, capabilities: {
      general: { positionEncodings: ["utf-16"] }, textDocument: {
        publishDiagnostics: { versionSupport: true }, synchronization: { didSave: true }
      }
    }}).then(reply => {
      if (reply.capabilities.positionEncoding !== "utf-16") throw new Error("Server did not select UTF-16 positions.");
      this.notify("initialized", {});
    });
  }
  fail(error) {
    if (this.closed) return;
    this.closed = true;
    if (!this.stopping) this.output.appendLine(error.message);
    for (const pending of this.pending.values()) { pending.cleanup(); pending.reject(error); }
    this.pending.clear();
  }
  write(message) {
    if (this.closed) throw new Error("Vela language server is closed.");
    const body = Buffer.from(JSON.stringify({ jsonrpc: "2.0", ...message }), "utf8");
    this.child.stdin.write(Buffer.from(`Content-Length: ${body.length}\r\n\r\n`, "ascii"));
    this.child.stdin.write(body);
  }
  notify(method, params) { this.write({ method, params }); }
  request(method, params, token) {
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      const cancel = token?.onCancellationRequested(() => {
        if (!this.closed) this.notify("$/cancelRequest", { id });
      });
      this.pending.set(id, { resolve, reject, cleanup: () => cancel?.dispose() });
      try { this.write({ id, method, params }); }
      catch (error) { this.pending.delete(id); cancel?.dispose(); reject(error); }
    });
  }
  receive(bytes) {
    this.queue.push(bytes);
    while (this.queue.size > 0) {
      if (this.length === null) {
        const header = this.queue.peek(Math.min(this.queue.size, 8192));
        const end = header.indexOf("\r\n\r\n");
        if (end < 0) {
          if (this.queue.size > 8192) throw new Error("Oversized language server header.");
          return;
        }
        const text = this.queue.read(end + 4).toString("ascii");
        const match = /^Content-Length: ([0-9]+)\r\n/i.exec(text);
        if (!match) throw new Error("Missing language server Content-Length.");
        this.length = Number(match[1]);
        if (!Number.isSafeInteger(this.length) || this.length > 64 * 1024 * 1024)
          throw new Error("Oversized language server message.");
      }
      if (this.queue.size < this.length) return;
      const message = JSON.parse(this.queue.read(this.length).toString("utf8"));
      this.length = null;
      this.dispatch(message);
    }
  }
  dispatch(message) {
    if (Object.hasOwn(message, "id")) {
      const pending = this.pending.get(message.id);
      if (!pending) return;
      this.pending.delete(message.id);
      pending.cleanup();
      if (message.error) pending.reject(new Error(message.error.message));
      else pending.resolve(message.result);
    } else if (message.method === "textDocument/publishDiagnostics") {
      const params = message.params;
      const open = vscode.workspace.textDocuments.find(document => document.uri.toString() === params.uri);
      if (open && params.version !== undefined && params.version !== open.version) return;
      this.diagnostics.set(vscode.Uri.parse(params.uri), params.diagnostics.map(item => {
        const start = item.range.start, end = item.range.end;
        const diagnostic = new vscode.Diagnostic(new vscode.Range(start.line, start.character, end.line, end.character),
          item.message, vscode.DiagnosticSeverity.Error);
        diagnostic.source = item.source;
        return diagnostic;
      }));
    } else if (message.method === "window/logMessage") this.output.appendLine(message.params.message);
  }
  async stop() {
    if (this.closed) return;
    this.stopping = true;
    try {
      await Promise.race([this.request("shutdown"), new Promise(resolve => setTimeout(resolve, 1000))]);
      this.notify("exit");
      this.child.stdin.end();
    } catch (error) { this.output.appendLine(error.message); }
    const kill = setTimeout(() => { if (!this.closed) this.child.kill(); }, 1000);
    kill.unref();
  }
}

let connection;

function editorRange(range) {
  return new vscode.Range(range.start.line, range.start.character, range.end.line, range.end.character);
}

function registerSymbolProviders(context, client, flush) {
  const selector = { language: "vela", scheme: "file" };
  const linksChanged = new vscode.EventEmitter();
  const tokensChanged = new vscode.EventEmitter();
  const tokenLegend = new vscode.SemanticTokensLegend(
    ["class", "interface", "enum", "type", "typeParameter"], ["declaration", "defaultLibrary"]);
  context.subscriptions.push(
    linksChanged,
    tokensChanged,
    vscode.languages.registerDocumentSemanticTokensProvider(selector, {
      onDidChangeSemanticTokens: tokensChanged.event,
      async provideDocumentSemanticTokens(document, token) {
        const version = document.version;
        if (token.isCancellationRequested) return null;
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return null;
        const result = await client.request("textDocument/semanticTokens/full", {
          textDocument: { uri: document.uri.toString() }
        }, token);
        if (token.isCancellationRequested || document.version !== version) return null;
        return new vscode.SemanticTokens(Uint32Array.from(result.data));
      }
    }, tokenLegend),
    vscode.languages.registerDocumentLinkProvider(selector, {
      onDidChangeDocumentLinks: linksChanged.event,
      async provideDocumentLinks(document, token) {
        const version = document.version;
        if (token.isCancellationRequested) return [];
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return [];
        const links = await client.request("textDocument/documentLink", {
          textDocument: { uri: document.uri.toString() }
        }, token);
        if (token.isCancellationRequested || document.version !== version) return [];
        return links.map(item => {
          const link = new vscode.DocumentLink(editorRange(item.range), vscode.Uri.parse(item.target));
          link.tooltip = item.tooltip;
          return link;
        });
      }
    }),
    vscode.languages.registerCompletionItemProvider(selector, {
      async provideCompletionItems(document, position, token) {
        const version = document.version;
        if (token.isCancellationRequested) return [];
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return [];
        const items = await client.request("textDocument/completion", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character }
        }, token);
        if (token.isCancellationRequested || document.version !== version) return [];
        return items.map(item => {
          // LSP 的 kind 从 1 起，VS Code 的对应枚举从 0 起。
          const completion = new vscode.CompletionItem(item.label, item.kind - 1);
          completion.detail = item.detail;
          completion.range = editorRange(item.textEdit.range);
          completion.insertText = item.textEdit.newText;
          completion.sortText = item.sortText;
          if (item.documentation) completion.documentation = new vscode.MarkdownString(item.documentation.value);
          return completion;
        });
      }
    }, ".", "(", ","),
    vscode.languages.registerHoverProvider(selector, {
      async provideHover(document, position, token) {
        const version = document.version;
        if (token.isCancellationRequested) return null;
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return null;
        const result = await client.request("textDocument/hover", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character }
        }, token);
        if (token.isCancellationRequested || document.version !== version || !result) return null;
        const contents = new vscode.MarkdownString(result.contents.value);
        return new vscode.Hover(contents, editorRange(result.range));
      }
    }),
    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(document, position, token) {
        const version = document.version;
        if (token.isCancellationRequested) return null;
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return null;
        const target = await client.request("textDocument/definition", {
          textDocument: { uri: document.uri.toString() },
          position: { line: position.line, character: position.character }
        }, token);
        if (token.isCancellationRequested || document.version !== version || !target) return null;
        return new vscode.Location(vscode.Uri.parse(target.uri), editorRange(target.range));
      }
    })
  );
  return () => { linksChanged.fire(); tokensChanged.fire(); };
}

function activate(context) {
  const output = vscode.window.createOutputChannel("Vela");
  const diagnostics = vscode.languages.createDiagnosticCollection("vela");
  context.subscriptions.push(output, diagnostics,
    vscode.commands.registerCommand("vela.showOutput", () => output.show()));
  const folder = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
  const configured = vscode.workspace.getConfiguration("vela").get("serverPath", "");
  if (!folder && !path.isAbsolute(configured)) {
    output.appendLine("Set an absolute vela.serverPath when no workspace folder is open.");
    return;
  }
  const root = folder || path.dirname(configured);
  const executable = configured ? path.resolve(root, configured) : path.join(root, "out", "debug", process.platform === "win32" ? "vela.exe" : "vela");
  connection = new Connection(executable, root, output, diagnostics);
  const opened = new Map();
  const timers = new Map();
  const isVela = document => document.languageId === "vela" && document.uri.scheme === "file";
  const report = error => output.appendLine(error.message);
  const open = async document => {
    if (!isVela(document)) return;
    await connection.ready;
    if (document.isClosed) return;
    const uri = document.uri.toString();
    if (opened.has(uri)) return;
    opened.set(uri, document.version);
    connection.notify("textDocument/didOpen", { textDocument: {
      uri, languageId: "vela", version: document.version, text: document.getText()
    }});
  };
  const flush = async document => {
    const uri = document.uri.toString();
    clearTimeout(timers.get(uri));
    timers.delete(uri);
    await open(document);
    if (document.isClosed || connection.closed) return;
    if (opened.get(uri) === document.version) return;
    opened.set(uri, document.version);
    connection.notify("textDocument/didChange", { textDocument: { uri, version: document.version },
      contentChanges: [{ text: document.getText() }] });
  };
  const refreshLinks = registerSymbolProviders(context, connection, flush);
  for (const document of vscode.workspace.textDocuments) open(document).catch(report);
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument(document => {
      if (isVela(document)) open(document).then(refreshLinks).catch(report);
    }),
    vscode.workspace.onDidChangeTextDocument(event => {
      const document = event.document;
      if (!isVela(document)) return;
      const uri = document.uri.toString();
      clearTimeout(timers.get(uri));
      const delay = vscode.workspace.getConfiguration("vela", document.uri).get("diagnosticsDelay", 150);
      timers.set(uri, setTimeout(() => flush(document).then(refreshLinks).catch(report), delay));
    }),
    vscode.workspace.onDidSaveTextDocument(document => {
      if (isVela(document)) flush(document).then(() => connection.notify("textDocument/didSave", {
        textDocument: { uri: document.uri.toString() }
      })).catch(report);
    }),
    vscode.workspace.onDidCloseTextDocument(document => {
      const uri = document.uri.toString();
      clearTimeout(timers.get(uri));
      timers.delete(uri);
      if (opened.delete(uri) && !connection.closed) connection.notify("textDocument/didClose", { textDocument: { uri } });
      diagnostics.delete(document.uri);
      if (isVela(document)) refreshLinks();
    }),
    vscode.languages.registerDocumentFormattingEditProvider({ language: "vela", scheme: "file" }, {
      async provideDocumentFormattingEdits(document, options, token) {
        const version = document.version;
        await flush(document);
        if (document.isClosed || token.isCancellationRequested) return [];
        const edits = await connection.request("textDocument/formatting", {
          textDocument: { uri: document.uri.toString() }, options
        }, token);
        if (token.isCancellationRequested || document.version !== version) return [];
        const result = edits.map(edit => new vscode.TextEdit(new vscode.Range(edit.range.start.line,
          edit.range.start.character, edit.range.end.line, edit.range.end.character), edit.newText));
        if (document.eol !== vscode.EndOfLine.LF) result.push(vscode.TextEdit.setEndOfLine(vscode.EndOfLine.LF));
        return result;
      }
    }),
    { dispose() { for (const timer of timers.values()) clearTimeout(timer); } }
  );
  const changed = uri => {
    if (opened.has(uri.toString())) return;
    connection.ready.then(() => {
      connection.notify("workspace/didChangeWatchedFiles", {
        changes: [{ uri: uri.toString(), type: 2 }]
      });
      refreshLinks();
    }).catch(report);
  };
  const watcher = vscode.workspace.createFileSystemWatcher("**/*.vela");
  context.subscriptions.push(watcher, watcher.onDidChange(changed), watcher.onDidCreate(changed), watcher.onDidDelete(changed));
  connection.ready.catch(report);
}

async function deactivate() { await connection?.stop(); }
module.exports = { activate, deactivate };
