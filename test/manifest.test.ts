import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
import { describe, expect, it } from "vitest";
import {
  convertManifestXmlToBt,
  convertManifestXmlToBtAction,
} from "../src/cli/manifest-xml-to-bt.js";

async function convertXml(xml: string): Promise<string> {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "bt-dsl-xml-to-bt-"));
  const xmlPath = path.join(dir, "nodes.xml");
  fs.writeFileSync(xmlPath, xml, "utf-8");
  const { btText } = await convertManifestXmlToBt(xmlPath);
  return btText;
}

describe("manifest:xml-to-bt converter", () => {
  it("converts Action ports (input/output) to declare", async () => {
    const bt = await convertXml(`
      <TreeNodesModel>
        <Action ID="FindEnemy">
          <input_port name="range" type="Float"/>
          <output_port name="pos" type="Vector3"/>
        </Action>
      </TreeNodesModel>
    `);

    expect(bt).toContain("declare Action FindEnemy");
    expect(bt).toContain("in range: Float");
    expect(bt).toContain("out pos: Vector3");
  });

  it("maps inout_port to ref", async () => {
    const bt = await convertXml(`
      <TreeNodesModel>
        <Action ID="Update">
          <inout_port name="value" type="Int"/>
        </Action>
      </TreeNodesModel>
    `);

    expect(bt).toContain("declare Action Update");
    expect(bt).toContain("ref value: Int");
  });

  it("sanitizes type names to fit ID grammar", async () => {
    const bt = await convertXml(`
      <TreeNodesModel>
        <Action ID="Sleep">
          <input_port name="msec" type="unsigned int">Sleep duration</input_port>
        </Action>
      </TreeNodesModel>
    `);

    // "unsigned int" -> "unsigned_int"
    expect(bt).toContain("in msec: unsigned_int");
    // description should be emitted as /// comment
    expect(bt).toContain("/// Sleep duration");
  });

  it("throws when no nodes are found (likely wrong input)", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "bt-dsl-xml-to-bt-"));
    const xmlPath = path.join(dir, "empty.xml");
    fs.writeFileSync(xmlPath, `<TreeNodesModel></TreeNodesModel>`, "utf-8");

    await expect(convertManifestXmlToBt(xmlPath)).rejects.toThrow(
      /No node definitions found/i
    );
  });

  it("treats a .bt argument as output path and infers sibling .xml", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "bt-dsl-xml-to-bt-"));
    const xmlPath = path.join(dir, "StandardNodes.xml");
    const btPath = path.join(dir, "StandardNodes.bt");

    fs.writeFileSync(
      xmlPath,
      `<TreeNodesModel><Action ID="Ping"><input_port name="x" type="int"/></Action></TreeNodesModel>`,
      "utf-8"
    );

    // No output file exists yet, should succeed without --force.
    await convertManifestXmlToBtAction(btPath, {});
    const out = fs.readFileSync(btPath, "utf-8");
    expect(out).toContain("declare Action Ping");
    expect(out).toContain("in x: int");
  });

  it("refuses to overwrite existing output unless --force is provided", async () => {
    const dir = fs.mkdtempSync(path.join(os.tmpdir(), "bt-dsl-xml-to-bt-"));
    const xmlPath = path.join(dir, "nodes.xml");
    const btPath = path.join(dir, "nodes.bt");

    fs.writeFileSync(
      xmlPath,
      `<TreeNodesModel><Action ID="A"/></TreeNodesModel>`,
      "utf-8"
    );
    fs.writeFileSync(btPath, "declare Action Existing()\n", "utf-8");

    await expect(
      convertManifestXmlToBtAction(xmlPath, { output: btPath })
    ).rejects.toThrow(/Refusing to overwrite/i);

    await convertManifestXmlToBtAction(xmlPath, {
      output: btPath,
      force: true,
    });
    const out = fs.readFileSync(btPath, "utf-8");
    expect(out).toContain("declare Action A");
  });
});
