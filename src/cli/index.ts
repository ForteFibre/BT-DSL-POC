import { Command } from "commander";
import { NodeFileSystem } from "langium/node";
import { createBtDslServices } from "../language/bt-dsl-module.js";
import { XmlGenerator } from "../generator/xml-generator.js";
import { BtNodeResolver } from "../language/node-resolver.js";
import { extractDocumentWithBtImports } from "./cli-util.js";
import type { Program } from "../language/generated/ast.js";
import { isTreeDef } from "../language/generated/ast.js";
import { formatBtDslText } from "../formatter/format-bt-dsl.js";
import { convertManifestXmlToBtAction } from "./manifest-xml-to-bt.js";
import * as fs from "node:fs";
import * as path from "node:path";
import chalk from "chalk";

const program = new Command();

program
  .name("bt-dsl")
  .version("0.1.0")
  .description("BehaviorTree DSL - Compile .bt files to BehaviorTree.CPP XML");

program
  .command("generate")
  .argument("<file>", "Source .bt file to compile")
  .option("-o, --output <file>", "Output XML file (default: <input>.xml)")
  .option("-v, --validate-only", "Only validate, do not generate output")
  .description("Compile a .bt file to BehaviorTree.CPP XML")
  .action(
    async (
      file: string,
      options: { output?: string; validateOnly?: boolean }
    ) => {
      await generateAction(file, options);
    }
  );

program
  .command("validate")
  .argument("<file>", "Source .bt file to validate")
  .description("Validate a .bt file without generating output")
  .action(async (file: string) => {
    await generateAction(file, { validateOnly: true });
  });

program
  .command("format")
  .argument("<file>", "Source .bt file to format")
  .option("-c, --check", "Check if file is formatted without modifying it")
  .description("Format a .bt file")
  .action(async (file: string, options: { check?: boolean }) => {
    await formatAction(file, options);
  });

program
  .command("manifest:xml-to-bt")
  .argument("<file>", "TreeNodesModel XML manifest file to convert")
  .option("-o, --output <file>", "Output .bt file (default: <input>.bt)")
  .option("--stdout", "Write converted .bt to stdout")
  .option("--force", "Overwrite output file if it already exists")
  .description(
    "Convert a BehaviorTree.CPP TreeNodesModel XML manifest to bt-dsl `declare` statements"
  )
  .action(
    async (
      file: string,
      options: { output?: string; stdout?: boolean; force?: boolean }
    ) => {
      try {
        await convertManifestXmlToBtAction(file, options);
      } catch (err) {
        console.error(
          chalk.red(
            `Error: ${err instanceof Error ? err.message : String(err)}`
          )
        );
        process.exit(1);
      }
    }
  );

async function generateAction(
  file: string,
  options: { output?: string; validateOnly?: boolean }
): Promise<void> {
  const services = createBtDslServices(NodeFileSystem);
  await services.BtDsl.manifest.ManifestManager.initialize();

  const filePath = path.resolve(file);
  if (!fs.existsSync(filePath)) {
    console.error(chalk.red(`Error: File not found: ${filePath}`));
    process.exit(1);
  }

  console.log(chalk.blue(`Processing: ${filePath}`));

  try {
    const document = await extractDocumentWithBtImports(
      filePath,
      services.BtDsl
    );

    // Document + imports have already been validated by extractDocumentWithBtImports
    const errors = document.diagnostics?.filter((d) => d.severity === 1) ?? [];
    const warnings =
      document.diagnostics?.filter((d) => d.severity === 2) ?? [];

    // Report diagnostics
    for (const diag of errors) {
      const line = diag.range.start.line + 1;
      const col = diag.range.start.character + 1;
      console.error(chalk.red(`  Error (${line}:${col}): ${diag.message}`));
    }

    for (const diag of warnings) {
      const line = diag.range.start.line + 1;
      const col = diag.range.start.character + 1;
      console.warn(chalk.yellow(`  Warning (${line}:${col}): ${diag.message}`));
    }

    if (errors.length > 0) {
      console.error(
        chalk.red(`\nValidation failed with ${errors.length} error(s).`)
      );
      process.exit(1);
    }

    if (options.validateOnly) {
      console.log(chalk.green("Validation successful."));
      return;
    }

    // Generate XML
    const program = document.parseResult.value as Program;
    const generator = new XmlGenerator();
    generator.setNodeResolver(new BtNodeResolver(services.BtDsl));

    // Include imported tree definitions in the output so BehaviorTree.CPP can resolve SubTrees.
    const entryNames = new Set(program.trees.map((t) => t.name));
    const allTrees = services.BtDsl.shared.workspace.IndexManager.allElements(
      "TreeDef"
    )
      .map((d) => d.node)
      .filter(Boolean)
      .filter(isTreeDef)
      .toArray();
    const mergedTrees = [
      ...program.trees,
      ...allTrees.filter((t) => !entryNames.has(t.name)),
    ];

    const xml = generator.generate(program, mergedTrees);

    // Determine output path
    const outputPath = options.output ?? filePath.replace(/\.bt$/, ".xml");
    fs.writeFileSync(outputPath, xml, "utf-8");

    console.log(chalk.green(`Generated: ${outputPath}`));
  } catch (err) {
    console.error(
      chalk.red(`Error: ${err instanceof Error ? err.message : String(err)}`)
    );
    process.exit(1);
  }
}

async function formatAction(
  file: string,
  options: { check?: boolean }
): Promise<void> {
  const filePath = path.resolve(file);
  if (!fs.existsSync(filePath)) {
    console.error(chalk.red(`Error: File not found: ${filePath}`));
    process.exit(1);
  }

  try {
    const original = fs.readFileSync(filePath, "utf-8");
    const formatted = await formatBtDslText(original, {
      filepath: filePath,
      tabWidth: 2,
      useTabs: false,
      endOfLine: "lf",
    });

    if (formatted === original) {
      console.log(chalk.green(`${filePath}: Already formatted`));
      return;
    }

    if (options.check) {
      console.log(chalk.yellow(`${filePath}: Would be reformatted`));
      process.exit(1);
    }

    fs.writeFileSync(filePath, formatted, "utf-8");
    console.log(chalk.green(`${filePath}: Formatted`));
  } catch (err) {
    console.error(
      chalk.red(`Error: ${err instanceof Error ? err.message : String(err)}`)
    );
    process.exit(1);
  }
}

program.parse();
