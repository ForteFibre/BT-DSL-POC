import * as fs from "node:fs";
import * as path from "node:path";
import { XMLParser } from "fast-xml-parser";
import { formatBtDslText } from "../formatter/format-bt-dsl.js";

type PortDirection = "in" | "out" | "ref";

type Port = {
  name: string;
  direction: PortDirection;
  typeName?: string;
  description?: string;
};

type DeclaredNode = {
  category: "Action" | "Condition" | "Control" | "Decorator" | "SubTree";
  name: string;
  ports: Port[];
};

function asArray<T>(value: T | T[] | undefined | null): T[] {
  if (value === undefined || value === null) return [];
  return Array.isArray(value) ? value : [value];
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null;
}

function getStringProp(
  obj: Record<string, unknown>,
  key: string
): string | undefined {
  const v = obj[key];
  return typeof v === "string" ? v : undefined;
}

function sanitizeTypeName(typeName: string): string {
  // The grammar uses ID for typeName, so we keep it conservative.
  // Replace unsupported characters and ensure it starts with [A-Za-z_].
  const cleaned = typeName.replaceAll(/[^a-zA-Z0-9_]/g, "_");
  if (/^[a-zA-Z_]/.test(cleaned)) return cleaned;
  return `_${cleaned}`;
}

function parseTreeNodesModelXml(xmlContent: string): DeclaredNode[] {
  const parser = new XMLParser({
    ignoreAttributes: false,
    attributeNamePrefix: "",
    textNodeName: "#text",
    trimValues: true,
    parseTagValue: false,
    parseAttributeValue: false,
    allowBooleanAttributes: true,
  });

  const parsed = parser.parse(xmlContent);

  const models: unknown[] = [];
  const visit = (value: unknown): void => {
    if (!value) return;
    if (Array.isArray(value)) {
      for (const item of value) visit(item);
      return;
    }
    if (!isRecord(value)) return;

    const model = value["TreeNodesModel"];
    if (model !== undefined) {
      models.push(...asArray(model as any));
    }

    for (const k of Object.keys(value)) {
      if (k === "TreeNodesModel") continue;
      visit(value[k]);
    }
  };

  visit(parsed);
  if (models.length === 0) {
    // Some files omit the wrapper.
    models.push(parsed);
  }

  const categories = [
    "Action",
    "Condition",
    "Control",
    "Decorator",
    "SubTree",
  ] as const;
  const out: DeclaredNode[] = [];

  for (const m of models) {
    if (!isRecord(m)) continue;

    for (const cat of categories) {
      const rawNodes = m[cat];
      for (const nodeValue of asArray(rawNodes as any)) {
        if (!isRecord(nodeValue)) continue;
        const id = getStringProp(nodeValue, "ID");
        if (!id) continue;

        const ports: Port[] = [];
        const addPorts = (
          tag: "input_port" | "output_port" | "inout_port",
          dir: PortDirection
        ) => {
          for (const p of asArray(nodeValue[tag] as any)) {
            if (!isRecord(p)) continue;
            const name = getStringProp(p, "name");
            if (!name) continue;

            const type = getStringProp(p, "type");
            const attrDesc = getStringProp(p, "description");
            const textDescRaw = p["#text"];
            const textDesc =
              typeof textDescRaw === "string" ? textDescRaw.trim() : undefined;
            const description =
              textDesc && textDesc.length > 0 ? textDesc : attrDesc;

            ports.push({
              name,
              direction: dir,
              typeName: type ? sanitizeTypeName(type) : undefined,
              description,
            });
          }
        };

        addPorts("input_port", "in");
        addPorts("output_port", "out");
        addPorts("inout_port", "ref");

        out.push({
          category: cat,
          name: id,
          ports,
        });
      }
    }
  }

  return out;
}

function renderBt(nodes: DeclaredNode[]): string {
  const lines: string[] = [];
  lines.push("//! Converted from TreeNodesModel XML");
  lines.push("//! This file contains only `declare ...` statements.");
  lines.push("");

  for (const n of nodes) {
    if (n.ports.length === 0) {
      lines.push(`declare ${n.category} ${n.name}()`);
      continue;
    }

    // Multiline style when any port has docs or when there are many ports.
    const multiline =
      n.ports.some((p) => p.description && p.description.length > 0) ||
      n.ports.length > 2;

    if (!multiline) {
      const args = n.ports
        .map((p) => {
          const dir = p.direction;
          const type = p.typeName ?? "any";
          return `${dir} ${p.name}: ${type}`;
        })
        .join(", ");
      lines.push(`declare ${n.category} ${n.name}(${args})`);
      continue;
    }

    lines.push(`declare ${n.category} ${n.name}(`);
    for (let i = 0; i < n.ports.length; i++) {
      const p = n.ports[i];
      if (p.description) {
        lines.push(`    /// ${p.description}`);
      }
      const type = p.typeName ?? "any";
      const comma = i < n.ports.length - 1 ? "," : "";
      lines.push(`    ${p.direction} ${p.name}: ${type}${comma}`);
    }
    lines.push(")");
  }

  lines.push("");
  return lines.join("\n");
}

export async function convertManifestXmlToBt(
  inputXmlPath: string
): Promise<{ btText: string; nodes: number }> {
  const xml = fs.readFileSync(inputXmlPath, "utf-8");
  const nodes = parseTreeNodesModelXml(xml);
  if (nodes.length === 0) {
    throw new Error(
      `No node definitions found in manifest: ${inputXmlPath}. ` +
        `Is this a BehaviorTree.CPP TreeNodesModel XML?`
    );
  }
  const raw = renderBt(nodes);
  const formatted = await formatBtDslText(raw, {
    filepath: inputXmlPath.replace(/\.xml$/i, ".bt"),
    tabWidth: 2,
    useTabs: false,
    endOfLine: "lf",
  });

  return { btText: formatted, nodes: nodes.length };
}

export async function convertManifestXmlToBtAction(
  inputXml: string,
  options: { output?: string; stdout?: boolean; force?: boolean }
): Promise<void> {
  const givenPath = path.resolve(inputXml);
  const ext = path.extname(givenPath).toLowerCase();

  let inputXmlPath: string;
  let outputBtPath: string | undefined;

  if (ext === ".xml") {
    inputXmlPath = givenPath;
    outputBtPath = options.stdout
      ? undefined
      : options.output
        ? path.resolve(options.output)
        : givenPath.replace(/\.xml$/i, ".bt");
  } else if (ext === ".bt") {
    if (options.output) {
      throw new Error(
        `When <file> is a .bt output path, do not also pass --output. ` +
          `Either use:\n` +
          `  bt-dsl manifest:xml-to-bt path/to/manifest.xml -o path/to/manifest.bt\n` +
          `or:\n` +
          `  bt-dsl manifest:xml-to-bt path/to/manifest.bt`
      );
    }
    // Treat <file> as the desired output .bt path and infer the input .xml next to it.
    inputXmlPath = givenPath.replace(/\.bt$/i, ".xml");
    outputBtPath = options.stdout ? undefined : givenPath;
  } else {
    throw new Error(
      `Expected a .xml manifest path (input) or a .bt path (output). ` +
        `Got: ${givenPath}`
    );
  }

  if (!fs.existsSync(inputXmlPath)) {
    throw new Error(
      `XML manifest not found: ${inputXmlPath}. ` +
        `If you intended to convert an XML file, pass the .xml path. ` +
        `If you intended to specify the output, pass a .bt path that has a sibling .xml file.`
    );
  }

  if (!options.stdout && outputBtPath) {
    if (fs.existsSync(outputBtPath) && !options.force) {
      throw new Error(
        `Refusing to overwrite existing file: ${outputBtPath}. ` +
          `Re-run with --force to overwrite.`
      );
    }
  }

  const { btText } = await convertManifestXmlToBt(inputXmlPath);

  if (options.stdout) {
    process.stdout.write(btText);
    return;
  }

  if (!outputBtPath) {
    // Should be unreachable, but keep it safe.
    throw new Error("Output path could not be determined.");
  }

  fs.writeFileSync(outputBtPath, btText, "utf-8");
}
