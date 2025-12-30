/** @type {import('prettier').Config} */
module.exports = {
  printWidth: 100,
  tabWidth: 2,
  useTabs: false,
  semi: true,
  singleQuote: true,
  trailingComma: 'all',
  bracketSpacing: true,
  arrowParens: 'always',
  endOfLine: 'lf',
  overrides: [
    {
      files: ['*.json', '*.jsonc', '*.code-workspace'],
      options: {
        singleQuote: false,
      },
    },
    {
      files: ['*.md'],
      options: {
        printWidth: 100,
        // Avoid reflowing Markdown prose because it can rewrite VitePress GitHub alerts
        // like:
        //   > [!NOTE]
        //   > body...
        // into a same-line "title":
        //   > [!NOTE] body...
        // which VitePress may render literally (e.g. **bold**, [link](...)).
        proseWrap: 'preserve',
      },
    },
  ],
};
