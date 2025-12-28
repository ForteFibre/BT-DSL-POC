import js from '@eslint/js';
import globals from 'globals';
import eslintConfigPrettier from 'eslint-config-prettier';
import tseslint from 'typescript-eslint';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const tsconfigRootDir = path.dirname(fileURLToPath(import.meta.url));

export default tseslint.config(
  {
    ignores: [
      '**/.git/**',
      '**/.pnpm/**',
      '**/node_modules/**',

      // build artifacts
      '**/dist/**',
      '**/out/**',
      '**/out-tsc/**',
      '**/out-test/**',
      '**/build/**',
      '**/core/build/**',
      '**/core/build-*/**',
      '**/tree-sitter-bt-dsl/build/**',

      '**/vscode/out-test/**',
      '**/vscode/out-tsc/**',

      // test files not in tsconfig (use separate test tsconfig)
      '**/vscode/test/**',
    ],
  },

  // Base JS rules
  js.configs.recommended,

  // TypeScript rules (with type information)
  ...tseslint.configs.recommendedTypeChecked,
  ...tseslint.configs.strictTypeChecked,
  ...tseslint.configs.stylisticTypeChecked,

  {
    languageOptions: {
      globals: {
        ...globals.es2022,
        ...globals.node,
      },
      parserOptions: {
        projectService: true,
        tsconfigRootDir,
      },
    },
    rules: {
      // Keep import hygiene tight.
      '@typescript-eslint/consistent-type-imports': [
        'error',
        {
          prefer: 'type-imports',
          fixStyle: 'separate-type-imports',
        },
      ],

      // Allow intentional unused values by prefixing with `_`.
      // (This pairs nicely with TypeScript's noUnusedLocals/noUnusedParameters.)
      '@typescript-eslint/no-unused-vars': [
        'error',
        {
          argsIgnorePattern: '^_',
          varsIgnorePattern: '^_',
          caughtErrorsIgnorePattern: '^_',
          ignoreRestSiblings: true,
        },
      ],

      // A few type-aware footguns we almost always want to catch.
      '@typescript-eslint/no-floating-promises': 'error',
      '@typescript-eslint/no-misused-promises': [
        'error',
        {
          checksVoidReturn: {
            // VS Code APIs often expect `() => void` callbacks that can be `async` safely.
            attributes: false,
          },
        },
      ],

      // Prefer explicit intent.
      '@typescript-eslint/no-non-null-assertion': 'error',

      // TS already covers this well; avoid noisy lint churn.
      '@typescript-eslint/no-inferrable-types': 'off',
    },
  },

  // Let Prettier own formatting concerns.
  eslintConfigPrettier,

  // Per-folder tweaks
  {
    files: ['vscode/**/*.{ts,tsx,mts,cts}'],
    languageOptions: {
      globals: {
        ...globals.es2022,
        ...globals.node,
      },
    },
    rules: {
      // VS Code extension entrypoints are commonly default-exported.
      'no-console': 'off',
    },
  },
);
