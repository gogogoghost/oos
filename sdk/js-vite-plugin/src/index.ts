import solidPlugin from "vite-plugin-solid";
import type { Plugin, PluginOption } from "vite";

export interface OosPluginOptions {
  /** Application entry module. Defaults to `src/main.tsx`. */
  entry?: string;
  /** Build output directory. Defaults to `dist`. */
  outDir?: string;
  /** Emitted QuickJS entry filename. Defaults to `main.mjs`. */
  fileName?: string;
  /** Vite minifier setting. Defaults to Vite's version-appropriate minifier. */
  minify?: boolean | "esbuild" | "oxc" | "terser";
}

function isNativeModule(id: string): boolean {
  return id.startsWith("oos:");
}

function oosRuntimePlugin(options: OosPluginOptions): Plugin {
  const fileName = options.fileName ?? "main.mjs";
  if (!fileName.endsWith(".mjs") || fileName.includes("/") ||
      fileName.includes("\\")) {
    throw new TypeError("OOS Vite output fileName must be a plain .mjs filename");
  }

  return {
    name: "oos-quickjs-runtime",
    enforce: "pre",
    config(_config, environment) {
      if (environment.command !== "build")
        return;
      return {
        appType: "custom",
        build: {
          target: "es2023",
          outDir: options.outDir ?? "dist",
          emptyOutDir: true,
          cssCodeSplit: false,
          minify: options.minify ?? true,
          lib: {
            entry: options.entry ?? "src/main.tsx",
            formats: ["es"],
            fileName: () => fileName,
          },
          rollupOptions: {
            external: isNativeModule,
          },
        },
      };
    },
    resolveId(source) {
      if (isNativeModule(source))
        return { id: source, external: true };
      if (/\.css(?:$|\?)/i.test(source)) {
        this.error(
          "OOS applications use Tailwind utility strings; CSS files are not supported",
        );
      }
      if (source === "solid-js/web") {
        this.error(
          "solid-js/web requires a DOM; import @oos/platform/solid instead",
        );
      }
      return null;
    },
    generateBundle(_output, bundle) {
      const chunks = Object.values(bundle).filter(
        (entry) => entry.type === "chunk",
      );
      const assets = Object.values(bundle).filter(
        (entry) => entry.type === "asset",
      );
      if (chunks.length !== 1 || assets.length !== 0) {
        this.error(
          "OOS application builds must emit one self-contained ESM chunk and no browser assets",
        );
      }
      const entry = chunks[0];
      if (entry.type !== "chunk" || !entry.isEntry ||
          !entry.exports.includes("frame")) {
        this.error("OOS JavaScript entries must export a frame function");
      }
    },
  };
}

/**
 * Configures Solid's universal compiler and a browser-free, single-module
 * QuickJS production build. Native `oos:*` imports remain external for the
 * runtime module loader.
 */
export default function oos(options: OosPluginOptions = {}): PluginOption[] {
  return [
    solidPlugin({
      dev: false,
      hot: false,
      solid: {
        generate: "universal",
        hydratable: false,
        moduleName: "@oos/platform/solid",
      },
    }),
    oosRuntimePlugin(options),
  ];
}
