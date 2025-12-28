import * as vscode from 'vscode';
import * as path from 'node:path';
import type { BtDslCore } from '../wasm/btDslCore';
import { byteOffsetAtPosition, byteRangeToRange } from './utf8';
import { formatBtDslText, setTreeSitterWasmPath } from '@bt-dsl/formatter';

interface DiagItem {
  source?: string;
  message: string;
  severity: 'Error' | 'Warning' | 'Info' | 'Hint';
  code?: string;
  range: { startByte: number; endByte: number };
}

interface CompletionItemJson {
  label: string;
  kind?: string;
  detail?: string;
  insertText?: string;
  replaceRange?: { startByte: number; endByte: number };
}

interface CompletionListJson {
  items?: CompletionItemJson[];
  isIncomplete?: boolean;
}

interface HoverJson {
  contents: string | null;
  range: { startByte: number; endByte: number } | null;
}

interface DefinitionJson {
  locations?: {
    uri: string;
    range: { startByte: number; endByte: number };
  }[];
}

interface DocumentSymbolsJson {
  symbols?: {
    name: string;
    kind: string;
    range: { startByte: number; endByte: number };
    selectionRange?: { startByte: number; endByte: number };
  }[];
}

interface DocumentHighlightsJson {
  items?: {
    range: { startByte: number; endByte: number };
    kind?: string;
  }[];
}

interface SemanticTokensJson {
  tokens?: {
    range: { startByte: number; endByte: number };
    type: string;
    modifiers?: string[];
  }[];
}

// -----------------------------------------------------------------------------
// Workspace syncing (serverless workspace needs all documents to be present)
// -----------------------------------------------------------------------------

async function readTextFile(uri: vscode.Uri): Promise<string | null> {
  try {
    const data = await vscode.workspace.fs.readFile(uri);
    return Buffer.from(data).toString('utf8');
  } catch {
    return null;
  }
}

function mapSeverity(s: DiagItem['severity']): vscode.DiagnosticSeverity {
  switch (s) {
    case 'Error':
      return vscode.DiagnosticSeverity.Error;
    case 'Warning':
      return vscode.DiagnosticSeverity.Warning;
    case 'Info':
      return vscode.DiagnosticSeverity.Information;
    case 'Hint':
      return vscode.DiagnosticSeverity.Hint;
  }
}

function mapCompletionKind(k?: string): vscode.CompletionItemKind {
  switch (k) {
    case 'Port':
      return vscode.CompletionItemKind.Field;
    case 'Node':
      return vscode.CompletionItemKind.Function;
    case 'Variable':
      return vscode.CompletionItemKind.Variable;
    case 'Keyword':
      return vscode.CompletionItemKind.Keyword;
    default:
      return vscode.CompletionItemKind.Text;
  }
}

function mapSymbolKind(k: string): vscode.SymbolKind {
  switch (k) {
    case 'Tree':
      return vscode.SymbolKind.Function;
    case 'Declare':
      return vscode.SymbolKind.Class;
    case 'GlobalVar':
      return vscode.SymbolKind.Variable;
    default:
      return vscode.SymbolKind.Object;
  }
}

export function registerBtDslProviders(context: vscode.ExtensionContext, core: BtDslCore): void {
  const debugE2E = process.env.BT_DSL_E2E === '1';

  // Configure tree-sitter WASM path for the formatter (CJS environment)
  const treeSitterWasmPath = path.join(context.extensionPath, 'out', 'tree-sitter-bt_dsl.wasm');
  setTreeSitterWasmPath(treeSitterWasmPath);

  if (debugE2E) {
    console.log('bt-dsl: registerBtDslProviders (e2e)');
  }

  const selector: vscode.DocumentSelector = [
    { scheme: 'file', language: 'bt-dsl' },
    { scheme: 'untitled', language: 'bt-dsl' },
  ];

  const diagCollection = vscode.languages.createDiagnosticCollection('bt-dsl');
  context.subscriptions.push(diagCollection);

  // Implicit stdlib (bundled with the extension)
  const stdlibFileUri = vscode.Uri.file(context.asAbsolutePath('stdlib/StandardNodes.bt'));
  const stdlibUri = stdlibFileUri.toString();
  let stdlibTextCache: string | null | undefined;

  const ensureStdlibLoaded = async (): Promise<void> => {
    if (stdlibTextCache === undefined) {
      stdlibTextCache = await readTextFile(stdlibFileUri);
    }
    if (stdlibTextCache != null) {
      core.setDocument(stdlibUri, stdlibTextCache);
    }
  };

  const parseResolvedUris = (raw: string): string[] => {
    try {
      const parsed = JSON.parse(raw) as { uris?: unknown };
      return Array.isArray(parsed.uris)
        ? parsed.uris.filter((x): x is string => typeof x === 'string')
        : [];
    } catch {
      return [];
    }
  };

  const tryLoadUriIntoCore = async (u: string): Promise<boolean> => {
    let target: vscode.Uri;
    try {
      target = vscode.Uri.parse(u);
    } catch {
      return false;
    }
    if (target.scheme !== 'file') {
      return false;
    }

    const open = vscode.workspace.textDocuments.find((d) => d.uri.toString() === u);
    if (open) {
      core.setDocument(u, open.getText());
      return true;
    }

    const text = await readTextFile(target);
    if (text == null) {
      return false;
    }
    core.setDocument(u, text);
    return true;
  };

  const loadMissingImports = async (uris: string[], loaded: Set<string>): Promise<boolean> => {
    let loadedAny = false;
    for (const u of uris) {
      if (loaded.has(u)) continue;
      loaded.add(u);
      const didLoad = await tryLoadUriIntoCore(u);
      if (didLoad) {
        loadedAny = true;
      }
    }
    return loadedAny;
  };

  const syncWorkspaceForDoc = async (doc: vscode.TextDocument) => {
    const uri = doc.uri.toString();
    core.setDocument(uri, doc.getText());

    await ensureStdlibLoaded();

    // Iteratively load the import graph until it stops expanding.
    // (Core can only traverse into documents already present in the workspace.)
    const loaded = new Set<string>([uri, stdlibUri]);
    let importedUris: string[] = [stdlibUri];

    for (let iter = 0; iter < 16; iter++) {
      const next = parseResolvedUris(core.resolveImportsJson(uri, stdlibUri));
      importedUris = next;

      const loadedAny = await loadMissingImports(next, loaded);
      if (!loadedAny) {
        break;
      }
    }

    return { uri, importedUris };
  };

  // ---------------------------------------------------------------------------
  // Semantic Tokens (core-driven classifications + VSCode-side delta edits)
  // ---------------------------------------------------------------------------

  const semanticTokenTypes = [
    'keyword',
    'class',
    'function',
    'variable',
    'parameter',
    'property',
    'type',
    'decorator',
  ];

  const semanticTokenModifiers = ['declaration', 'modification', 'defaultLibrary'];

  const semanticLegend = new vscode.SemanticTokensLegend(
    semanticTokenTypes,
    semanticTokenModifiers,
  );

  interface SemanticCacheEntry {
    version: number;
    resultId: string;
    data: Uint32Array;
  }
  const semanticCache = new Map<string, SemanticCacheEntry>();

  const buildSemanticTokens = async (doc: vscode.TextDocument): Promise<vscode.SemanticTokens> => {
    const { uri, importedUris } = await syncWorkspaceForDoc(doc);

    const raw = core.semanticTokensJsonWithImports(uri, importedUris);
    let parsed: SemanticTokensJson;
    try {
      parsed = JSON.parse(raw) as SemanticTokensJson;
    } catch {
      return new vscode.SemanticTokens(new Uint32Array());
    }

    const builder = new vscode.SemanticTokensBuilder(semanticLegend);
    for (const tok of parsed.tokens ?? []) {
      const typeIdx = semanticTokenTypes.indexOf(tok.type);
      if (typeIdx < 0) continue;

      const r = byteRangeToRange(doc, tok.range);
      if (r.start.line !== r.end.line) {
        // Our core emits identifier-ish tokens; multi-line tokens are skipped.
        continue;
      }
      const len = r.end.character - r.start.character;
      if (len <= 0) continue;

      let modBits = 0;
      for (const m of tok.modifiers ?? []) {
        const mi = semanticTokenModifiers.indexOf(m);
        if (mi >= 0) {
          modBits |= 1 << mi;
        }
      }

      builder.push(r.start.line, r.start.character, len, typeIdx, modBits);
    }

    return builder.build();
  };

  const updateDiagnostics = async (doc: vscode.TextDocument) => {
    if (doc.languageId !== 'bt-dsl') return;

    const { uri, importedUris } = await syncWorkspaceForDoc(doc);

    let parsed: { items?: DiagItem[] };
    try {
      parsed = JSON.parse(core.diagnosticsJsonWithImports(uri, importedUris)) as {
        items?: DiagItem[];
      };
    } catch {
      diagCollection.set(doc.uri, []);
      return;
    }

    const diags: vscode.Diagnostic[] = [];
    for (const item of parsed.items ?? []) {
      const range = byteRangeToRange(doc, item.range);
      const d = new vscode.Diagnostic(range, item.message, mapSeverity(item.severity));
      d.source = item.source ?? 'bt-dsl';
      if (item.code) d.code = item.code;
      diags.push(d);
    }

    diagCollection.set(doc.uri, diags);
  };

  // Keep WASM workspace in sync
  context.subscriptions.push(
    vscode.workspace.onDidOpenTextDocument((doc) => void updateDiagnostics(doc)),
    vscode.workspace.onDidChangeTextDocument((e) => void updateDiagnostics(e.document)),
    vscode.workspace.onDidCloseTextDocument((doc) => {
      if (doc.languageId !== 'bt-dsl') return;
      diagCollection.delete(doc.uri);
      core.removeDocument(doc.uri.toString());
    }),
  );

  // Prime currently open documents
  for (const doc of vscode.workspace.textDocuments) {
    void updateDiagnostics(doc);
  }

  context.subscriptions.push(
    vscode.languages.registerCompletionItemProvider(
      selector,
      {
        async provideCompletionItems(doc, pos) {
          try {
            const { uri, importedUris } = await syncWorkspaceForDoc(doc);

            const byteOff = byteOffsetAtPosition(doc, pos);

            if (debugE2E) {
              console.log('bt-dsl: completion', {
                uri,
                byteOff,
                importedCount: importedUris.length,
              });
            }

            const raw = core.completionJsonWithImports(uri, byteOff, importedUris);

            let parsed: CompletionListJson;
            try {
              parsed = JSON.parse(raw) as CompletionListJson;
            } catch {
              return [];
            }

            const items: vscode.CompletionItem[] = [];
            for (const it of parsed.items ?? []) {
              const ci = new vscode.CompletionItem(it.label, mapCompletionKind(it.kind));
              if (it.detail) ci.detail = it.detail;
              if (it.insertText) ci.insertText = it.insertText;
              if (it.replaceRange) {
                ci.range = byteRangeToRange(doc, it.replaceRange);
              }
              items.push(ci);
            }

            const list = new vscode.CompletionList(items, parsed.isIncomplete ?? false);
            return list;
          } catch (e) {
            console.error('bt-dsl completion provider failed', e);
            return [];
          }
        },
      },
      '(',
      ':',
      ',',
      ' ',
      '@',
    ),

    vscode.languages.registerHoverProvider(selector, {
      async provideHover(doc, pos) {
        try {
          const { uri, importedUris } = await syncWorkspaceForDoc(doc);

          const byteOff = byteOffsetAtPosition(doc, pos);

          if (debugE2E) {
            console.log('bt-dsl: hover', {
              uri,
              byteOff,
              importedCount: importedUris.length,
            });
          }

          const raw = core.hoverJsonWithImports(uri, byteOff, importedUris);

          let parsed: HoverJson;
          try {
            parsed = JSON.parse(raw) as HoverJson;
          } catch {
            return null;
          }

          if (!parsed.contents) return null;

          const md = new vscode.MarkdownString(parsed.contents);
          md.supportHtml = false;
          md.isTrusted = false;

          const range = parsed.range ? byteRangeToRange(doc, parsed.range) : undefined;
          return new vscode.Hover(md, range);
        } catch (e) {
          console.error('bt-dsl hover provider failed', e);
          return null;
        }
      },
    }),

    vscode.languages.registerDefinitionProvider(selector, {
      async provideDefinition(doc, pos) {
        try {
          const { uri, importedUris } = await syncWorkspaceForDoc(doc);

          const byteOff = byteOffsetAtPosition(doc, pos);

          if (debugE2E) {
            console.log('bt-dsl: definition', {
              uri,
              byteOff,
              importedCount: importedUris.length,
            });
          }

          const raw = core.definitionJsonWithImports(uri, byteOff, importedUris);

          let parsed: DefinitionJson;
          try {
            parsed = JSON.parse(raw) as DefinitionJson;
          } catch {
            return null;
          }

          const locs: vscode.Location[] = [];
          for (const loc of parsed.locations ?? []) {
            const targetUri = vscode.Uri.parse(loc.uri);
            try {
              if (targetUri.toString() === doc.uri.toString()) {
                locs.push(new vscode.Location(targetUri, byteRangeToRange(doc, loc.range)));
              } else {
                const targetDoc = await vscode.workspace.openTextDocument(targetUri);
                locs.push(new vscode.Location(targetUri, byteRangeToRange(targetDoc, loc.range)));
              }
            } catch {
              // File not accessible / not a text document: best-effort fallback.
              locs.push(new vscode.Location(targetUri, new vscode.Range(0, 0, 0, 0)));
            }
          }

          return locs.length === 1 ? locs[0] : locs;
        } catch (e) {
          console.error('bt-dsl definition provider failed', e);
          return null;
        }
      },
    }),

    vscode.languages.registerDocumentSymbolProvider(selector, {
      async provideDocumentSymbols(doc) {
        const uri = doc.uri.toString();
        // Keep the serverless workspace up-to-date for future queries.
        await syncWorkspaceForDoc(doc);

        const raw = core.documentSymbolsJson(uri);
        let parsed: DocumentSymbolsJson;
        try {
          parsed = JSON.parse(raw) as DocumentSymbolsJson;
        } catch {
          return [];
        }

        const symbols: vscode.DocumentSymbol[] = [];
        for (const s of parsed.symbols ?? []) {
          const range = byteRangeToRange(doc, s.range);
          const sel = s.selectionRange ? byteRangeToRange(doc, s.selectionRange) : range;
          const ds = new vscode.DocumentSymbol(s.name, '', mapSymbolKind(s.kind), range, sel);
          symbols.push(ds);
        }
        return symbols;
      },
    }),

    // Document Formatting Provider
    vscode.languages.registerDocumentFormattingEditProvider(selector, {
      async provideDocumentFormattingEdits(
        document: vscode.TextDocument,
        options: vscode.FormattingOptions,
      ): Promise<vscode.TextEdit[]> {
        const text = document.getText();
        try {
          const formatted = await formatBtDslText(text, {
            filepath: document.uri.fsPath,
            tabWidth: options.tabSize,
            useTabs: !options.insertSpaces,
          });

          // Replace the entire document
          const fullRange = new vscode.Range(
            document.positionAt(0),
            document.positionAt(text.length),
          );
          return [vscode.TextEdit.replace(fullRange, formatted)];
        } catch (e) {
          const msg = e instanceof Error ? e.message : String(e);
          void vscode.window.showErrorMessage(`BT DSL: Format failed: ${msg}`);
          return [];
        }
      },
    }),
    // Document Highlight Provider
    vscode.languages.registerDocumentHighlightProvider(selector, {
      async provideDocumentHighlights(doc, pos) {
        const { uri, importedUris } = await syncWorkspaceForDoc(doc);

        const byteOff = byteOffsetAtPosition(doc, pos);
        const raw = core.documentHighlightsJsonWithImports(uri, byteOff, importedUris);

        let parsed: DocumentHighlightsJson;
        try {
          parsed = JSON.parse(raw) as DocumentHighlightsJson;
        } catch {
          return [];
        }

        const mapKind = (k?: string): vscode.DocumentHighlightKind => {
          switch (k) {
            case 'Write':
              return vscode.DocumentHighlightKind.Write;
            case 'Text':
              return vscode.DocumentHighlightKind.Text;
            case 'Read':
            default:
              return vscode.DocumentHighlightKind.Read;
          }
        };

        return (parsed.items ?? []).map(
          (it) => new vscode.DocumentHighlight(byteRangeToRange(doc, it.range), mapKind(it.kind)),
        );
      },
    }),

    // Semantic Tokens Provider (+ delta)
    vscode.languages.registerDocumentSemanticTokensProvider(
      selector,
      {
        async provideDocumentSemanticTokens(doc) {
          const tokens = await buildSemanticTokens(doc);
          semanticCache.set(doc.uri.toString(), {
            version: doc.version,
            resultId: String(doc.version),
            data: tokens.data,
          });
          return tokens;
        },

        async provideDocumentSemanticTokensEdits(doc) {
          const uri = doc.uri.toString();
          const prev = semanticCache.get(uri);

          const tokens = await buildSemanticTokens(doc);
          const nextData = tokens.data;
          const nextResultId = String(doc.version);

          if (!prev) {
            semanticCache.set(uri, {
              version: doc.version,
              resultId: nextResultId,
              data: nextData,
            });
            return tokens;
          }

          const oldData = prev.data;
          const minLen = Math.min(oldData.length, nextData.length);

          let prefix = 0;
          while (prefix < minLen && oldData[prefix] === nextData[prefix]) {
            prefix++;
          }

          let suffix = 0;
          while (
            suffix < minLen - prefix &&
            oldData[oldData.length - 1 - suffix] === nextData[nextData.length - 1 - suffix]
          ) {
            suffix++;
          }

          const deleteCount = oldData.length - prefix - suffix;
          const insertData = nextData.slice(prefix, nextData.length - suffix);

          semanticCache.set(uri, {
            version: doc.version,
            resultId: nextResultId,
            data: nextData,
          });

          if (deleteCount === 0 && insertData.length === 0) {
            return new vscode.SemanticTokensEdits([], nextResultId);
          }

          const edits: vscode.SemanticTokensEdit[] = [
            {
              start: prefix,
              deleteCount,
              data: insertData,
            },
          ];

          return new vscode.SemanticTokensEdits(edits, nextResultId);
        },
      },
      semanticLegend,
    ),
  );
}
