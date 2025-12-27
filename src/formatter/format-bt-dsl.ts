import prettier from "prettier";
import btDslPrettierPlugin from "./prettier-plugin-bt-dsl.js";

export interface FormatBtDslOptions {
  /** Used for parser decisions and for prettier's internal caches.
   * If omitted, a synthetic in-memory filepath is used.
   */
  filepath?: string;
  tabWidth?: number;
  useTabs?: boolean;
  endOfLine?: "lf" | "crlf";
}

export async function formatBtDslText(
  text: string,
  options: FormatBtDslOptions = {}
): Promise<string> {
  const formatted = await prettier.format(text, {
    parser: "bt-dsl",
    plugins: [btDslPrettierPlugin as any],
    filepath: options.filepath ?? "/__bt-dsl__/in-memory.bt",
    tabWidth: options.tabWidth ?? 2,
    useTabs: options.useTabs ?? false,
    endOfLine: options.endOfLine ?? "lf",
  });

  // prettier.format returns a string. Ensure trailing newline (Prettier usually does).
  return formatted.endsWith("\n") ? formatted : formatted + "\n";
}
