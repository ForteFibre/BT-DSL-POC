import { describe, it, expect, beforeEach } from "vitest";
import { NodeFileSystem } from "langium/node";
import { createBtDslServices } from "../src/language/bt-dsl-module.js";
import { XmlGenerator } from "../src/generator/xml-generator.js";
import { URI } from "langium";
import type { Program } from "../src/language/generated/ast.js";

describe("XML Generator", () => {
  let services: ReturnType<typeof createBtDslServices>;
  let generator: XmlGenerator;
  let testId = 0;

  beforeEach(() => {
    services = createBtDslServices(NodeFileSystem);
    generator = new XmlGenerator();
    testId++;
  });

  async function generateXml(content: string): Promise<string> {
    const uri = URI.file(
      `/test/generator-test-${testId}-${Date.now()}-${Math.random()}.bt`
    );
    const document =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        content,
        uri
      );
    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(document);
    await services.BtDsl.shared.workspace.DocumentBuilder.build([document], {
      validation: true,
    });
    const program = document.parseResult.value as Program;
    return generator.generate(program);
  }

  it("should generate basic tree structure", async () => {
    const xml = await generateXml(`
            Tree Main() {
                Sequence() {}
            }
        `);
    expect(xml).toContain('<?xml version="1.0"');
    expect(xml).toContain('<root BTCPP_format="4"');
    expect(xml).toContain('main_tree_to_execute="Main"');
    expect(xml).toContain('<BehaviorTree ID="Main">');
    expect(xml).toContain("<Sequence />");
  });

  it("should generate TreeNodesModel for subtrees with params", async () => {
    const xml = await generateXml(`
            Tree Main() { Sequence() {} }
            Tree SubTree(ref target: Vector3, amount: Int) { Sequence() {} }
        `);
    expect(xml).toContain("<TreeNodesModel>");
    expect(xml).toContain('<SubTree ID="SubTree">');
    expect(xml).toContain('<inout_port name="target" type="Vector3" />');
    expect(xml).toContain('<input_port name="amount" type="Int" />');
  });

  it("should generate global var references with braces", async () => {
    const xml = await generateXml(`
            var Target: Vector3
            Tree Main() {
                Action(pos: Target)
            }
        `);
    expect(xml).toContain('pos="{Target}"');
  });

  it("should generate string literals without braces", async () => {
    const xml = await generateXml(`
            Tree Main() {
                Action(result: "SUCCESS")
            }
        `);
    expect(xml).toContain('result="SUCCESS"');
  });

  it("should generate decorators as wrapper elements", async () => {
    const xml = await generateXml(`
            Tree Main() {
                @Repeat(times: 3)
                @Inverter
                Action()
            }
        `);
    expect(xml).toContain("<Inverter>");
    expect(xml).toContain('<Repeat times="3">');
    expect(xml).toContain("<Action />");
    expect(xml).toContain("</Repeat>");
    expect(xml).toContain("</Inverter>");
  });

  it("should generate metadata from doc comments", async () => {
    const xml = await generateXml(`
            /// Main tree description
            Tree Main() {
                Sequence() {}
            }
        `);
    expect(xml).toContain("<Metadata>");
    expect(xml).toContain('key="description"');
    expect(xml).toContain('value="Main tree description"');
  });

  it("should generate _description from node doc comments", async () => {
    const xml = await generateXml(`
            Tree Main() {
                /// This is an action
                MyAction()
            }
        `);
    expect(xml).toContain('_description="This is an action"');
  });

  it("should escape XML special characters", async () => {
    const xml = await generateXml(`
            Tree Main() {
                Action(text: "<tag>&value</tag>")
            }
        `);
    expect(xml).toContain("&lt;tag&gt;&amp;value&lt;/tag&gt;");
  });

  it("should generate nested children correctly", async () => {
    const xml = await generateXml(`
            Tree Main() {
                Sequence {
                    Fallback {
                        Action1()
                        Action2()
                    }
                }
            }
        `);
    expect(xml).toContain("<Sequence>");
    expect(xml).toContain("<Fallback>");
    expect(xml).toContain("<Action1 />");
    expect(xml).toContain("<Action2 />");
    expect(xml).toContain("</Fallback>");
    expect(xml).toContain("</Sequence>");
  });

  it("should generate Script initialization for local vars with initial values", async () => {
    const xml = await generateXml(`
            Tree Main() {
                var msg = "hello"
                var count = 42
                Sequence() {}
            }
        `);
    expect(xml).toContain("<Script code=");
    expect(xml).toContain("msg:='hello'");
    expect(xml).toContain("count:=42");
  });

  it("should wrap tree body with Sequence when local vars have initial values", async () => {
    const xml = await generateXml(`
            Tree Main() {
                var active = true
                Action()
            }
        `);
    // Should have outer Sequence wrapper
    expect(xml).toMatch(/<Sequence>\s*<Script/);
    expect(xml).toContain("active:=true");
  });

  it("should not wrap tree when local vars have no initial values", async () => {
    const xml = await generateXml(`
            Tree Main() {
                var active: bool
                Action()
            }
        `);
    // Should NOT have Script node
    expect(xml).not.toContain("<Script");
  });

  describe("Expression Statement Generation", () => {
    it("should generate Script node for simple assignment", async () => {
      const xml = await generateXml(`
        var counter: int
        Tree Main() {
          Sequence {
            counter = 0
          }
        }
      `);
      expect(xml).toContain("<Script code=");
      expect(xml).toContain("counter = 0");
    });

    it("should generate Script node for compound assignment", async () => {
      const xml = await generateXml(`
        var count: int
        Tree Main() {
          Sequence {
            count += 1
          }
        }
      `);
      expect(xml).toContain("<Script code=");
      expect(xml).toContain("count += 1");
    });

    it("should wrap binary expressions in parentheses", async () => {
      const xml = await generateXml(`
        var a: int
        var b: int
        var result: int
        Tree Main() {
          Sequence {
            result = a + b
          }
        }
      `);
      expect(xml).toContain("(a + b)");
    });

    it("should wrap nested binary expressions in parentheses", async () => {
      const xml = await generateXml(`
        var a: int
        var b: int
        var result: int
        Tree Main() {
          Sequence {
            result = a * b + 1
          }
        }
      `);
      // Should have nested parentheses for BTCPP compatibility
      expect(xml).toContain("((a * b) + 1)");
    });
  });
});

