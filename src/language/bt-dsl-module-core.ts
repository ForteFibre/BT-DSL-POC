import { inject, type Module } from "langium";
import {
  createDefaultModule,
  createDefaultSharedModule,
  type DefaultSharedModuleContext,
  type LangiumServices,
  type LangiumSharedServices,
  type PartialLangiumSharedServices,
  type PartialLangiumServices,
} from "langium/lsp";
import {
  BtDslGeneratedModule,
  BtDslGeneratedSharedModule,
} from "./generated/module.js";
import {
  BtDslValidator,
  registerValidationChecks,
} from "./bt-dsl-validator.js";
import { BtDslScopeProvider } from "./bt-dsl-scope-provider.js";
import { BtDslDocumentBuilder } from "./bt-dsl-document-builder.js";
import { BtDslWorkspaceManager } from "./bt-dsl-workspace-manager.js";
import { ManifestManager } from "../manifest/manifest-manager.js";
import { BtDslCompletionProvider } from "./lsp/bt-dsl-completion-provider.js";
import { BtDslDefinitionProvider } from "./lsp/bt-dsl-definition-provider.js";
import { BtDslDocumentHighlightProvider } from "./lsp/bt-dsl-document-highlight-provider.js";
import { BtDslHoverProvider } from "./lsp/bt-dsl-hover-provider.js";

/**
 * Shared services override.
 *
 * We replace Langium's default WorkspaceManager so that `import "..."` can pull in transitive
 * `.bt` dependencies that live outside the workspace folders.
 */
export const BtDslSharedModule: Module<
  LangiumSharedServices,
  PartialLangiumSharedServices
> = {
  workspace: {
    DocumentBuilder: (services) => new BtDslDocumentBuilder(services),
    WorkspaceManager: (services) => new BtDslWorkspaceManager(services),
  },
};

/**
 * Declaration of custom services - add your own service definitions here.
 */
export type BtDslAddedServices = {
  manifest: {
    ManifestManager: ManifestManager;
  };
  validation: {
    BtDslValidator: BtDslValidator;
  };
};

/**
 * Union of Langium default services and BT DSL custom services.
 */
export type BtDslServices = LangiumServices & BtDslAddedServices;

/**
 * Dependency injection module for BT DSL services.
 *
 * NOTE: This core module intentionally does NOT register a Formatter.
 * The Prettier plugin needs to construct services without importing the formatter,
 * otherwise we'd create a circular dependency (formatter -> prettier plugin -> services -> formatter).
 */
export const BtDslModuleCore: Module<
  BtDslServices,
  PartialLangiumServices & BtDslAddedServices
> = {
  manifest: {
    ManifestManager: (services: BtDslServices) => new ManifestManager(services),
  },
  references: {
    ScopeProvider: (services: BtDslServices) =>
      new BtDslScopeProvider(services),
  },
  lsp: {
    CompletionProvider: (services: BtDslServices) =>
      new BtDslCompletionProvider(services),
    DefinitionProvider: (services: BtDslServices) =>
      new BtDslDefinitionProvider(services),
    DocumentHighlightProvider: (services: BtDslServices) =>
      new BtDslDocumentHighlightProvider(services),
    HoverProvider: (services: BtDslServices) =>
      new BtDslHoverProvider(services),
  },
  validation: {
    BtDslValidator: (services: BtDslServices) => new BtDslValidator(services),
  },
};

/**
 * Create the full set of services for BT DSL, excluding the Formatter.
 */
export function createBtDslServicesCore(context: DefaultSharedModuleContext): {
  shared: LangiumSharedServices;
  BtDsl: BtDslServices;
} {
  const shared = inject(
    createDefaultSharedModule(context),
    BtDslGeneratedSharedModule,
    BtDslSharedModule
  );
  const BtDsl = inject(
    createDefaultModule({ shared }),
    BtDslGeneratedModule,
    BtDslModuleCore
  );
  shared.ServiceRegistry.register(BtDsl);
  registerValidationChecks(BtDsl);
  return { shared, BtDsl };
}
