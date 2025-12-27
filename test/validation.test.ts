import { describe, it, expect, beforeEach } from "vitest";
import { NodeFileSystem } from "langium/node";
import { createBtDslServices } from "../src/language/bt-dsl-module.js";
import { URI } from "langium";
import type { Diagnostic } from "vscode-languageserver-types";
import type { Program } from "../src/language/generated/ast.js";

function getErrors(diagnostics: Diagnostic[]): Diagnostic[] {
  return diagnostics.filter((d) => d.severity === 1);
}

function getWarnings(diagnostics: Diagnostic[]): Diagnostic[] {
  return diagnostics.filter((d) => d.severity === 2);
}

describe("Semantic Validation", () => {
  let services: ReturnType<typeof createBtDslServices>;
  let testId = 0;

  beforeEach(() => {
    services = createBtDslServices(NodeFileSystem);
    testId++;
  });

  async function parseAndValidate(
    content: string
  ): Promise<{ program: Program; diagnostics: Diagnostic[] }> {
    // Ensure builtin nodes are loaded before validation
    await services.BtDsl.manifest.ManifestManager.initialize();

    // Many tests assume there is a generic Action node with a single input port.
    // In the new world, manifests are defined by `.bt` `declare` statements.
    const prelude = `declare Action Action(in pos: Vector3)\n`;
    const fullContent = `${prelude}\n${content}`;

    const uri = URI.file(
      `/test/validation-test-${testId}-${Date.now()}-${Math.random()}.bt`
    );
    const document =
      services.BtDsl.shared.workspace.LangiumDocumentFactory.fromString(
        fullContent,
        uri
      );
    services.BtDsl.shared.workspace.LangiumDocuments.addDocument(document);
    await services.BtDsl.shared.workspace.DocumentBuilder.build([document], {
      validation: true,
    });
    return {
      program: document.parseResult.value as Program,
      diagnostics: document.diagnostics ?? [],
    };
  }

  describe("Duplicate Checks", () => {
    it("should error on duplicate tree names", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() { Sequence() {} }
                Tree Main() { Sequence() {} }
            `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(
        errors.some((e) => e.message.includes("Duplicate tree name"))
      ).toBe(true);
    });

    it("should error on duplicate global variables", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Pos: Vector3
                var Pos: Vector3
                Tree Main() { Sequence() {} }
            `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some((e) => e.message.includes("Duplicate global variable"))
      ).toBe(true);
    });

    it("should error on duplicate parameter names", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main(x: Int, x: Float) { Sequence() {} }
            `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some((e) => e.message.includes("Duplicate parameter name"))
      ).toBe(true);
    });
  });

  describe("Symbol Resolution", () => {
    it("should resolve global variable references", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Target: Vector3
                Tree Main() {
                    Action(pos: Target)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should resolve tree parameter references", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main(target: Vector3) {
                    Action(pos: target)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should error on undefined variable reference", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    Action(pos: UndefinedVar)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
    });
  });

  describe("Manifest Category Scoping", () => {
    it("should allow only manifest Decorator nodes in @decorator position", async () => {
      // @Action is invalid because Action is not a Decorator category.
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    @Action Action()
                }
            `);

      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(
        errors.some(
          (e) => e.message.includes("Action") && e.message.includes("Decorator")
        )
      ).toBe(true);
    });

    it("should not allow manifest Decorator nodes as NodeStmt.nodeName", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    Delay()
                }
            `);

      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("Delay"))).toBe(true);
    });

    it("should resolve a manifest Decorator node in @decorator position", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    @Delay Action()
                }
            `);

      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should error when a non-Control manifest node has a children block", async () => {
      const { diagnostics } = await parseAndValidate(`
                declare Action TestAction()
                Tree Main() {
                    TestAction() {
                        Action(pos: Target)
                    }
                }
            `);

      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(
        errors.some(
          (e) =>
            e.message.includes("children block") &&
            e.message.includes("Only 'Control'")
        )
      ).toBe(true);
    });

    it("should error when a Control manifest node is used without children", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    Fallback()
                }
            `);

      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(
        errors.some(
          (e) =>
            e.message.includes("Control node 'Fallback'") &&
            e.message.includes("children block")
        )
      ).toBe(true);
    });
  });

  describe("Direction Permission Checks", () => {
    it("should error when using ref on non-ref parameter", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Target: Vector3
                Tree Main(target: Vector3) {
                    Action(pos: ref target)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors.some((e) => e.message.includes("input-only"))).toBe(true);
    });

    it("should warn when ref parameter is never used for write access", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Target: Vector3
                Tree Main(ref target: Vector3) {
                    Action(pos: target)
                }
            `);
      const warnings = getWarnings(diagnostics);
      expect(
        warnings.some((w) => w.message.includes("never used for write access"))
      ).toBe(true);
    });

    it("should allow ref parameter used with ref", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Target: Vector3
                Tree Main(ref target: Vector3) {
                    Action(pos: ref target)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should allow out parameter for output ports", async () => {
      const { diagnostics } = await parseAndValidate(`
                declare Action OutputAction(out result: Vector3)
                var Target: Vector3
                Tree Main(out result: Vector3) {
                    OutputAction(result: out result)
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should error when SubTree ref param is passed with in", async () => {
      const { diagnostics } = await parseAndValidate(`
                var Target: Vector3
                Tree Main() {
                    SubTree(x: Target)
                }
                Tree SubTree(ref x: Vector3) {
                    Action()
                }
            `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some(
          (e) => e.message.includes("requires") || e.message.includes("ref")
        )
      ).toBe(true);
    });
  });

  describe("Local Variable Initial Value Checks", () => {
    it("should allow local var with initial value only", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    var msg = "hello"
                    Sequence() {}
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should allow local var with matching type and initial value", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    var count: int = 42
                    Sequence() {}
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should error on type mismatch between explicit type and initial value", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    var count: int = "hello"
                    Sequence() {}
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("Type mismatch"))).toBe(
        true
      );
    });

    it("should error on local var without type and initial value", async () => {
      const { diagnostics } = await parseAndValidate(`
                Tree Main() {
                    var unknown
                    Sequence() {}
                }
            `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("must have either"))).toBe(
        true
      );
    });
  });

  describe("Expression Type Checks", () => {
    it("should error on adding int and bool", async () => {
      const { diagnostics } = await parseAndValidate(`
        var result: int
        var flag: bool
        Tree Main() {
          Sequence {
            result = 30 + flag
          }
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("cannot be applied"))).toBe(
        true
      );
    });

    it("should allow adding int and int", async () => {
      const { diagnostics } = await parseAndValidate(`
        var a: int
        var b: int
        Tree Main() {
          Sequence {
            a = b + 1
          }
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBe(0);
    });

    it("should error on logical operator with non-bool", async () => {
      const { diagnostics } = await parseAndValidate(`
        var a: int
        var result: bool
        Tree Main() {
          Sequence {
            result = a && true
          }
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("bool operands"))).toBe(
        true
      );
    });

    it("should error on assigning string to int", async () => {
      const { diagnostics } = await parseAndValidate(`
        var count: int
        Tree Main() {
          Sequence {
            count = "hello"
          }
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("Cannot assign"))).toBe(
        true
      );
    });
  });

  describe("Positional Argument Checks", () => {
    it("should allow positional argument for single-port node", async () => {
      const { diagnostics } = await parseAndValidate(`
        Tree Main() {
            @Repeat(3) Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should error on positional argument for multi-port node", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Action MultiPort(in a: any, in b: any)
        Tree Main() {
            MultiPort("value")
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(errors.some((e) => e.message.includes("2 ports"))).toBe(true);
    });

    it("should error on multiple positional arguments", async () => {
      const { diagnostics } = await parseAndValidate(`
        Tree Main() {
            Action("a", "b")
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.length).toBeGreaterThan(0);
      expect(
        errors.some((e) => e.message.includes("Only one positional argument"))
      ).toBe(true);
    });
  });

  describe("Declare Statement Validation", () => {
    it("should error on duplicate port names in declaration", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Action MyAction(in target: Vector3, in target: bool)
        Tree Main() {
          Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some((e) => e.message.includes("Duplicate port name"))
      ).toBe(true);
    });

    it("should error on invalid category", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare InvalidCategory MyNode()
        Tree Main() {
          Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors.some((e) => e.message.includes("Invalid category"))).toBe(
        true
      );
    });

    it("should error on duplicate declaration names", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Action MyAction()
        declare Condition MyAction()
        Tree Main() {
          Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some((e) => e.message.includes("Duplicate declaration"))
      ).toBe(true);
    });

    it("should error on declaration conflicting with tree name", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Action Main()
        Tree Main() {
          Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(
        errors.some((e) =>
          e.message.includes("conflicts with a Tree definition")
        )
      ).toBe(true);
    });

    it("should allow using declared node in tree", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Action MyAction(in target: string)
        Tree Main() {
          MyAction(target: "hello")
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });

    it("should allow using declared decorator", async () => {
      const { diagnostics } = await parseAndValidate(`
        declare Decorator MyDecorator(in timeout: double)
        Tree Main() {
          @MyDecorator(timeout: 5.0)
          Sequence {}
        }
      `);
      const errors = getErrors(diagnostics);
      expect(errors).toHaveLength(0);
    });
  });
});
