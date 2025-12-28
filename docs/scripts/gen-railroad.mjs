import { readFile } from 'node:fs/promises';
import rr from 'railroad-diagrams';

const { Diagram, Choice, Sequence, Terminal, NonTerminal, OneOrMore, ZeroOrMore, Optional, Skip } =
  rr;

// The upstream `railroad-diagrams` library expects its CSS to be loaded by the host page.
// However, our diagrams are referenced via `<img src="...">`, so the page CSS does NOT apply.
// Without styling, SVG `<path>` elements default to `stroke: none` (invisible), resulting in
// "loaded but not visible" diagrams. Embed a minimal style block into each SVG we generate.
//
// We also add a dark-scheme tweak so the diagrams remain readable in dark mode.
const RAILROAD_SVG_STYLE = `
svg.railroad-diagram{background:transparent}
svg.railroad-diagram path{stroke-width:2;stroke:#111;fill:rgba(0,0,0,0)}
svg.railroad-diagram text{font:600 13px ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono","Courier New",monospace;text-anchor:middle;fill:#111}
svg.railroad-diagram text.label{text-anchor:start}
svg.railroad-diagram text.comment{font:italic 12px ui-monospace,SFMono-Regular,Menlo,Monaco,Consolas,"Liberation Mono","Courier New",monospace}
svg.railroad-diagram rect{stroke-width:2;stroke:#111;fill:#eaffea}
@media (prefers-color-scheme: dark){
  svg.railroad-diagram path{stroke:#ddd}
  svg.railroad-diagram text{fill:#ddd}
  svg.railroad-diagram rect{stroke:#ddd;fill:#0f2a14}
}
`.trim();

function unwrap(node) {
  // Remove wrappers that don't change the structural shape for diagrams.
  if (!node || typeof node !== 'object') return node;
  const t = node.type;
  if (t === 'FIELD' || t === 'TOKEN') return unwrap(node.content);
  if (t === 'PREC' || t === 'PREC_LEFT' || t === 'PREC_RIGHT') return unwrap(node.content);
  return node;
}

function toDiagramExpr(node) {
  node = unwrap(node);

  if (!node) return Skip();

  switch (node.type) {
    case 'SEQ': {
      const parts = node.members.map(toDiagramExpr).filter(Boolean);
      if (!parts.length) return Skip();
      if (parts.length === 1) return parts[0];
      return Sequence(...parts);
    }

    case 'CHOICE': {
      const options = node.members.map(toDiagramExpr);
      if (!options.length) return Skip();
      if (options.length === 1) return options[0];
      return Choice(0, ...options);
    }

    case 'REPEAT': {
      const inner = toDiagramExpr(node.content);
      return ZeroOrMore(inner);
    }

    case 'REPEAT1': {
      const inner = toDiagramExpr(node.content);
      return OneOrMore(inner);
    }

    case 'SYMBOL':
      return NonTerminal(node.name);

    case 'STRING':
      return Terminal(node.value);

    case 'PATTERN':
      return Terminal(`/${node.value}/`);

    case 'BLANK':
      return Skip();

    default:
      // Best-effort fallback for unhandled node kinds
      return Terminal(`<${node.type}>`);
  }
}

function renderSvg(ruleName, ruleNode) {
  // For wide rules (like expression precedence ladders), Stack improves readability.
  const expr = toDiagramExpr(ruleNode);
  const diagram = Diagram(expr);

  // Add a title for accessibility
  const svg = diagram.toString();
  // Minimal post-processing to add embedded styles + <title>
  return svg.replace(/<svg[^>]*>/, (m) => {
    // Ensure SVG namespace exists for maximum compatibility when used as an external image.
    // (Some browsers are picky when loading SVG via <img>.)
    let open = m;
    if (!/\sxmlns=/.test(open)) {
      open = open.replace(/^<svg\b/, '<svg xmlns="http://www.w3.org/2000/svg"');
    }
    return `${open}<style>${RAILROAD_SVG_STYLE}</style><title>${ruleName}</title>`;
  });
}

const DEFAULT_RULES = [
  'program',
  'import_stmt',
  'declare_stmt',
  'declare_port',
  'global_var_decl',
  'tree_def',
  'param_decl',
  'local_var_decl',
  'node_stmt',
  'decorator',
  'argument',
  'assignment_expr',
  'expression',
  'literal',
  'identifier',
];

export async function generateRailroadSvgs(grammarJsonPath, ruleNames = DEFAULT_RULES) {
  const raw = await readFile(grammarJsonPath, 'utf8');
  const grammar = JSON.parse(raw);
  const rules = grammar.rules ?? {};

  const svgs = {};
  const missing = [];

  for (const name of ruleNames) {
    const ruleNode = rules[name];
    if (!ruleNode) {
      missing.push(name);
      continue;
    }
    try {
      svgs[name] = renderSvg(name, ruleNode);
    } catch (e) {
      svgs[name] =
        `<svg xmlns="http://www.w3.org/2000/svg"><text x="10" y="20">Failed to render ${name}: ${String(
          e,
        )}</text></svg>`;
    }
  }

  const md = [];
  md.push('<!-- THIS FILE IS AUTO-GENERATED. DO NOT EDIT DIRECTLY. -->');
  md.push('');
  md.push('このセクションは **自動生成された参考資料**です。');
  md.push('（言語仕様の本文は、各節の規則を正とします。）');
  md.push('');
  md.push('この図は、構文を視覚的に把握するための補助です。');
  md.push('');

  if (missing.length) {
    md.push('> 注意: 次のルールは生成対象から除外されました:');
    md.push(`> ${missing.join(', ')}`);
    md.push('');
  }

  for (const name of ruleNames) {
    if (!svgs[name]) continue;
    // Use a deeper heading so it can be embedded under an existing page heading.
    md.push(`#### ${name}`);
    md.push('');
    // Use a Vue component that calls `withBase` so it works with VitePress `base`
    // (e.g. GitHub Pages) and doesn't trigger Vite's asset-import resolution.
    md.push(`<WithBaseImage src="/railroad/${name}.svg" alt="Railroad diagram for ${name}" />`);
    md.push('');
  }

  return {
    indexMarkdown: md.join('\n'),
    svgs,
  };
}
