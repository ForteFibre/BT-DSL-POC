declare module '../formatter_wasm.js' {
  // Emscripten MODULARIZE + EXPORT_ES6 output exports a default async factory.
  // We intentionally keep this as `any` because it is generated at build time.
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  const factory: any;
  export default factory;
}
