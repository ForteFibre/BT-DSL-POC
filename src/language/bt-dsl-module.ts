import { inject, type Module } from "langium";
import {
  createDefaultModule,
  createDefaultSharedModule,
  type DefaultSharedModuleContext,
  type LangiumSharedServices,
  type PartialLangiumServices,
} from "langium/lsp";
import {
  BtDslGeneratedModule,
  BtDslGeneratedSharedModule,
} from "./generated/module.js";
import { registerValidationChecks } from "./bt-dsl-validator.js";
import { BtDslFormatter } from "./lsp/bt-dsl-formatter.js";
import { BtDslSemanticTokenProvider } from "./lsp/bt-dsl-semantic-token-provider.js";
import {
  BtDslModuleCore,
  BtDslSharedModule,
  type BtDslAddedServices,
  type BtDslServices,
} from "./bt-dsl-module-core.js";

export type { BtDslAddedServices, BtDslServices };

/**
 * Dependency injection module for BT DSL services.
 */
export const BtDslModule: Module<
  BtDslServices,
  PartialLangiumServices & BtDslAddedServices
> = {
  ...BtDslModuleCore,
  lsp: {
    ...BtDslModuleCore.lsp,
    Formatter: () => new BtDslFormatter(),
    SemanticTokenProvider: (services) =>
      new BtDslSemanticTokenProvider(services),
  },
};

/**
 * Create the full set of services for BT DSL.
 */
export function createBtDslServices(context: DefaultSharedModuleContext): {
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
    BtDslModule
  );
  shared.ServiceRegistry.register(BtDsl);
  registerValidationChecks(BtDsl);

  // IMPORTANT:
  // ManifestManager registers async DocumentBuilder hooks (Parsed phase) that must be active
  // even when we build documents with { validation: false } (CLI import discovery pass).
  // Since services are lazily instantiated, we eagerly initialize it here.
  // eslint-disable-next-line @typescript-eslint/no-unused-expressions
  BtDsl.manifest.ManifestManager;

  return { shared, BtDsl };
}
