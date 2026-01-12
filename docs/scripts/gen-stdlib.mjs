import { readFile } from 'node:fs/promises';

function stripTripleSlash(line) {
  return line.replace(/^\s*\/\/\//, '').trim();
}

function parseDeclareHeader(line) {
  // declare <Category> <Name>(
  const m = /^\s*declare\s+(\w+)\s+(\w+)\s*\(/.exec(line);
  if (!m) return null;
  return { category: m[1], name: m[2] };
}

function parsePortLine(rawLine) {
  // examples:
  // in msec: int
  // out pos: Vector3,
  // inout entry: any
  let line = rawLine.trim();
  if (!line) return null;
  if (line.startsWith('///')) return null;
  // remove trailing comma
  line = line.replace(/,$/, '').trim();
  if (!line) return null;

  // direction optional
  const m =
    /^(?:(in|out|inout)\s+)?([A-Za-z_][A-Za-z0-9_]*)\s*:\s*([A-Za-z_][A-Za-z0-9_]*)\s*$/.exec(line);
  if (!m) return null;
  return {
    direction: m[1] ?? 'in',
    name: m[2],
    type: m[3],
  };
}

export async function generateStdlibMarkdown(stdlibFilePath) {
  const text = await readFile(stdlibFilePath, 'utf8');
  const lines = text.split(/\r?\n/);

  const nodes = [];

  let pendingNodeDocs = [];
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    const trimmed = line.trim();

    if (trimmed.startsWith('///')) {
      pendingNodeDocs.push(stripTripleSlash(line));
      i++;
      continue;
    }

    if (trimmed.startsWith('declare ')) {
      const header = parseDeclareHeader(line);
      if (!header) {
        i++;
        continue;
      }

      // Capture full declare statement until matching ')'
      let stmt = line;
      while (!stmt.includes(')') && i + 1 < lines.length) {
        i++;
        stmt += `\n${lines[i]}`;
      }

      const stmtMatch = /^\s*declare\s+(\w+)\s+(\w+)\s*\(([^)]*)\)\s*$/.exec(
        stmt.replace(/\r/g, ''),
      );

      const category = header.category;
      const name = header.name;

      let portBlock = '';
      if (stmtMatch) {
        portBlock = stmtMatch[3] ?? '';
      }

      // Parse port block line-by-line to preserve /// doc comments per-port.
      const portLines = portBlock.split(/\r?\n/);
      const ports = [];
      let pendingPortDocs = [];
      for (const pl of portLines) {
        const t = pl.trim();
        if (!t) continue;
        if (t.startsWith('///')) {
          pendingPortDocs.push(stripTripleSlash(pl));
          continue;
        }
        const port = parsePortLine(pl);
        if (!port) continue;
        ports.push({
          ...port,
          docs: pendingPortDocs.join(' ').trim() || null,
        });
        pendingPortDocs = [];
      }

      nodes.push({
        category,
        name,
        docs: pendingNodeDocs.join(' ').trim() || null,
        ports,
        raw: stmt.trim().replace(/\s+/g, ' '),
      });

      pendingNodeDocs = [];
      i++;
      continue;
    }

    // reset node docs on blank line or other content
    if (pendingNodeDocs.length && trimmed === '') {
      pendingNodeDocs = [];
    }

    i++;
  }

  // Group by category in a stable order
  const order = ['Action', 'Condition', 'Control', 'Decorator', 'SubTree'];
  const byCat = new Map(order.map((c) => [c, []]));
  for (const n of nodes) {
    if (!byCat.has(n.category)) byCat.set(n.category, []);
    byCat.get(n.category).push(n);
  }

  const out = [];
  out.push('<!-- THIS FILE IS AUTO-GENERATED. DO NOT EDIT DIRECTLY. -->');
  out.push('');
  out.push('## 標準ライブラリ宣言一覧（自動生成）');
  out.push('');

  for (const cat of order) {
    const list = byCat.get(cat) ?? [];
    if (!list.length) continue;

    const heading =
      cat === 'Control' ? 'Control nodes' : cat === 'Decorator' ? 'Decorators' : `${cat}s`;

    out.push(`---`);
    out.push('');
    out.push(`## ${heading}`);
    out.push('');

    for (const n of list) {
      out.push(`### ${n.name}`);
      out.push('');
      out.push(`- 宣言: \`${n.raw}\``);
      if (n.docs) {
        out.push(`- 説明: ${n.docs}`);
      }
      if (!n.ports.length) {
        out.push(`- ポート: なし`);
      } else {
        out.push(`- ポート:`);
        for (const p of n.ports) {
          const doc = p.docs ? `: ${p.docs}` : '';
          out.push(`  - \`${p.name}\` (${p.direction}, \`${p.type}\`)${doc}`);
        }
      }
      out.push('');
    }
  }

  return out.join('\n');
}
