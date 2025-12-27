import { describe, it, expect, beforeEach } from "vitest";
import { NodeFileSystem } from "langium/node";
import { createBtDslServices } from "../src/language/bt-dsl-module.js";
import { URI } from "langium";
import type { Program } from "../src/language/generated/ast.js";

describe("Grammar Parsing", () => {
  let services: ReturnType<typeof createBtDslServices>;
  let testId = 0;

  beforeEach(() => {
    services = createBtDslServices(NodeFileSystem);
    testId++;
  });

  async function parseDocument(content: string): Promise<Program> {
    await services.BtDsl.manifest.ManifestManager.initialize();
    const uri = URI.file(`/test/grammar-test-${testId}-${Date.now()}.bt`);
    const document =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        content,
        uri
      );
    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(document);
    await services.BtDsl.shared.workspace.DocumentBuilder.build([document], {
      validation: true,
    });
    return document.parseResult.value as Program;
  }

  it("should parse empty tree", async () => {
    const program = await parseDocument(`
            Tree Main() {
                Sequence {}
            }
        `);
    expect(program.trees).toHaveLength(1);
    expect(program.trees[0].name).toBe("Main");
  });

  it("should parse non-control node with required parentheses", async () => {
    const program = await parseDocument(`
            Tree Main() {
                Action()
            }
        `);
    const node = program.trees[0].body;
    expect(node.nodeName.$refText).toBe("Action");
    expect(node.propertyBlock).toBeDefined();
    expect(node.childrenBlock).toBeUndefined();
  });

  it("should parse global var declaration", async () => {
    const program = await parseDocument(`
            var TargetPos: Vector3
            var Ammo: Int
            Tree Main() {
                Sequence {}
            }
        `);
    expect(program.globalVars).toHaveLength(2);
    expect(program.globalVars[0].name).toBe("TargetPos");
    expect(program.globalVars[0].typeName).toBe("Vector3");
  });

  it("should parse tree with parameters", async () => {
    const program = await parseDocument(`
            Tree MyTree(ref target, amount: Int) {
                Sequence {}
            }
        `);
    const tree = program.trees[0];
    expect(tree.params?.params).toHaveLength(2);
    expect(tree.params?.params[0].direction).toBe("ref");
    expect(tree.params?.params[0].name).toBe("target");
    expect(tree.params?.params[1].typeName).toBe("Int");
  });

  it("should parse decorators", async () => {
    const program = await parseDocument(`
            Tree Main() {
                @Repeat(times: 3)
                @Inverter
                Sequence {}
            }
        `);
    const node = program.trees[0].body;
    expect(node.decorators).toHaveLength(2);
    expect(node.decorators[0].name.$refText).toBe("Repeat");
    expect(node.decorators[1].name.$refText).toBe("Inverter");
  });

  it("should parse node with arguments", async () => {
    const program = await parseDocument(`
            Tree Main() {
                MyAction(
                    text: "hello",
                    count: 42,
                    rate: 3.14,
                    active: true
                )
            }
        `);
    const node = program.trees[0].body;
    expect(node.propertyBlock?.args).toHaveLength(4);
  });

  it("should parse documentation comments", async () => {
    const program = await parseDocument(`
            /// Main tree description
            Tree Main() {
                /// Node description
                Sequence {}
            }
        `);
    expect(program.trees[0].outerDocs).toHaveLength(1);
    expect(program.trees[0].body.outerDocs).toHaveLength(1);
  });

  it("should parse import statements", async () => {
    const program = await parseDocument(`
            import "nodes.bt"
            import "actions.bt"
            Tree Main() {
                Sequence {}
            }
        `);
    expect(program.imports).toHaveLength(2);
    expect(program.imports[0].path).toBe("nodes.bt");
  });

  describe("Expression Parsing", () => {
    it("should parse simple assignment expression in children", async () => {
      const program = await parseDocument(`
        Tree Main() {
          Sequence {
            counter = 0
          }
        }
      `);
      const children = program.trees[0].body.childrenBlock?.children ?? [];
      expect(children).toHaveLength(1);
    });

    it("should parse compound assignment expression", async () => {
      const program = await parseDocument(`
        var count: int
        Tree Main() {
          Sequence {
            count += 1
          }
        }
      `);
      const children = program.trees[0].body.childrenBlock?.children ?? [];
      expect(children).toHaveLength(1);
    });

    it("should parse binary expression with parentheses", async () => {
      const program = await parseDocument(`
        var a: int
        var b: int
        Tree Main() {
          Sequence {
            result = (a + b)
          }
        }
      `);
      const children = program.trees[0].body.childrenBlock?.children ?? [];
      expect(children).toHaveLength(1);
    });

    it("should parse complex expression", async () => {
      const program = await parseDocument(`
        var a: int
        var b: int
        Tree Main() {
          Sequence {
            result = a * b + 1
          }
        }
      `);
      const children = program.trees[0].body.childrenBlock?.children ?? [];
      expect(children).toHaveLength(1);
    });

    it("should parse expression in local var initialization", async () => {
      const program = await parseDocument(`
        var a: int
        Tree Main() {
          var x = a + 1
          Sequence {}
        }
      `);
      expect(program.trees[0].localVars).toHaveLength(1);
      expect(program.trees[0].localVars[0].initialValue).toBeDefined();
    });
  });

  describe("Declare Statement Parsing", () => {
    it("should parse simple declare statement", async () => {
      const program = await parseDocument(`
        declare Action MyAction()
        Tree Main() {
          Sequence {}
        }
      `);
      expect(program.declarations).toHaveLength(1);
      expect(program.declarations[0].category).toBe("Action");
      expect(program.declarations[0].name).toBe("MyAction");
      expect(program.declarations[0].ports).toHaveLength(0);
    });

    it("should parse declare statement with ports", async () => {
      const program = await parseDocument(`
        declare Action MoveAction(in target: Vector3, out result: bool)
        Tree Main() {
          Sequence {}
        }
      `);
      expect(program.declarations).toHaveLength(1);
      expect(program.declarations[0].ports).toHaveLength(2);
      expect(program.declarations[0].ports[0].name).toBe("target");
      expect(program.declarations[0].ports[0].direction).toBe("in");
      expect(program.declarations[0].ports[0].typeName).toBe("Vector3");
      expect(program.declarations[0].ports[1].name).toBe("result");
      expect(program.declarations[0].ports[1].direction).toBe("out");
    });

    it("should parse declare statement with ref port", async () => {
      const program = await parseDocument(`
        declare Decorator RetryDecorator(ref counter: int)
        Tree Main() {
          Sequence {}
        }
      `);
      expect(program.declarations[0].category).toBe("Decorator");
      expect(program.declarations[0].ports[0].direction).toBe("ref");
    });

    it("should parse multiple declare statements", async () => {
      const program = await parseDocument(`
        declare Action ActionA()
        declare Condition ConditionB(in flag: bool)
        declare Control MySequence()
        Tree Main() {
          Sequence {}
        }
      `);
      expect(program.declarations).toHaveLength(3);
      expect(program.declarations[0].name).toBe("ActionA");
      expect(program.declarations[1].name).toBe("ConditionB");
      expect(program.declarations[2].name).toBe("MySequence");
    });

    it("should use declared node in tree", async () => {
      const program = await parseDocument(`
        declare Action MyAction(in target: string)
        Tree Main() {
          MyAction(target: "hello")
        }
      `);
      expect(program.declarations).toHaveLength(1);
      expect(program.trees[0].body.nodeName.$refText).toBe("MyAction");
    });
  });
});
