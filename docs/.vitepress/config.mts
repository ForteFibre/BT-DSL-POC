import { defineConfig } from 'vitepress';
import type { LanguageRegistration } from 'shiki';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '../..');

// Use the same TextMate grammar as the VS Code extension for Shiki highlighting.
//
// NOTE:
// - VitePress v1.6.x uses Shiki v2.
// - `markdown.languages` expects Shiki's `LanguageRegistration` (IRawGrammar + `name`).
//   (旧来の `{ id, grammar }` 形式は通りません)
const btDslTmLanguagePath = path.join(repoRoot, 'vscode/syntaxes/bt-dsl.tmLanguage.json');
const btDslTmLanguage = JSON.parse(fs.readFileSync(btDslTmLanguagePath, 'utf8'));

const btDslLanguage: LanguageRegistration = {
  ...btDslTmLanguage,
  name: 'bt-dsl',
  scopeName: btDslTmLanguage.scopeName ?? 'source.bt-dsl',
  // Do NOT include `name` itself here (would create a circular alias).
  aliases: ['bt', 'bt_dsl'],
};

// EBNF言語のTextMate文法を読み込む
const ebnfTmLanguagePath = path.join(__dirname, 'ebnf.tmLanguage.json');
const ebnfTmLanguage = JSON.parse(fs.readFileSync(ebnfTmLanguagePath, 'utf8'));

const ebnfLanguage: LanguageRegistration = {
  ...ebnfTmLanguage,
  name: 'ebnf',
  scopeName: ebnfTmLanguage.scopeName ?? 'source.ebnf',
};

export default defineConfig({
  lang: 'ja-JP',
  title: 'BT-DSL',
  description: 'BehaviorTree.CPP v4向けDSLの言語仕様ドキュメント',

  // GitHub Pages用の設定
  // リポジトリ名がbt-dslの場合のパス設定
  base: '/BT-DSL-POC/',

  // Railroad 図は構文ガイド内に埋め込む方針のため、
  // standalone ページはビルド対象から除外する。
  srcExclude: ['railroad.md', 'README.md'],

  markdown: {
    languages: [btDslLanguage, ebnfLanguage],
  },

  themeConfig: {
    nav: [
      { text: 'ホーム', link: '/' },
      { text: 'ガイド', link: '/guide/quick-start' },
      { text: 'リファレンス', link: '/reference/grammar' },
      { text: '標準ノード', link: '/standard-library' },
    ],

    sidebar: [
      {
        text: 'ガイド',
        items: [
          { text: 'クイックスタート', link: '/guide/quick-start' },
          { text: '構文ガイド', link: '/guide/syntax' },
          { text: '実践例', link: '/examples' },
        ],
      },
      {
        text: 'リファレンス',
        items: [
          { text: '文法', link: '/reference/grammar' },
          { text: '型システム', link: '/reference/type-system' },
          { text: '意味制約', link: '/reference/semantics' },
          { text: '初期化安全性', link: '/reference/initialization-safety' },
        ],
      },
      {
        text: 'ライブラリ',
        items: [{ text: '標準ノード', link: '/standard-library' }],
      },
    ],

    search: {
      provider: 'local',
    },
  },
});
