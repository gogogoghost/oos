import { mkdtemp, readFile, readdir, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { build } from "vite";
import oos from "../src/index.js";

const outputs: string[] = [];

afterEach(async () => {
  await Promise.all(
    outputs.splice(0).map((path) => rm(path, { recursive: true })),
  );
});

async function temporaryProject(files: Record<string, string>): Promise<string> {
  const root = await mkdtemp(join(tmpdir(), "oos-vite-project-"));
  outputs.push(root);
  await Promise.all(
    Object.entries(files).map(([name, source]) =>
      writeFile(join(root, name), source, "utf8"),
    ),
  );
  return root;
}

describe("OOS Vite plugin", () => {
  it("builds Solid TSX into one browser-free QuickJS module", async () => {
    const output = await mkdtemp(join(tmpdir(), "oos-vite-plugin-"));
    outputs.push(output);
    const root = resolve("tests/fixture");
    await build({
      root,
      logLevel: "silent",
      plugins: oos({ outDir: output, minify: false }),
    });

    expect(await readdir(output)).toEqual(["main.mjs"]);
    const source = await readFile(join(output, "main.mjs"), "utf8");
    expect(source).toContain('from "oos:solid-internal"');
    expect(source).toContain("function frame");
    expect(source).not.toContain("document.");
    expect(source).not.toContain("window.");
  });

  it("rejects CSS and the DOM renderer", async () => {
    const cssRoot = await temporaryProject({
      "main.ts": 'import "./style.css"; export function frame() { return 0; }',
      "style.css": "view { display: flex; }",
    });
    await expect(
      build({
        root: cssRoot,
        logLevel: "silent",
        plugins: oos({ entry: "main.ts" }),
      }),
    ).rejects.toThrow(/CSS files are not supported/);

    const domRoot = await temporaryProject({
      "main.ts":
        'import { render } from "solid-js/web"; export function frame() { return typeof render; }',
    });
    await expect(
      build({
        root: domRoot,
        logLevel: "silent",
        plugins: oos({ entry: "main.ts" }),
      }),
    ).rejects.toThrow(/requires a DOM/);
  });

  it("rejects missing frame exports and code splitting", async () => {
    const lifecycleRoot = await temporaryProject({
      "main.ts": "export function initialize() { return true; }",
    });
    await expect(
      build({
        root: lifecycleRoot,
        logLevel: "silent",
        plugins: oos({ entry: "main.ts" }),
      }),
    ).rejects.toThrow(/must export a frame function/);

    const splitRoot = await temporaryProject({
      "main.ts":
        'export function frame() { void import("./lazy.js"); return 0; }',
      "lazy.js": "export const value = 1;",
    });
    await expect(
      build({
        root: splitRoot,
        logLevel: "silent",
        plugins: oos({ entry: "main.ts" }),
      }),
    ).rejects.toThrow(/one self-contained ESM chunk/);
  });

  it("validates the output filename", () => {
    expect(() => oos({ fileName: "nested/main.mjs" })).toThrow(
      /plain .mjs filename/,
    );
  });
});
