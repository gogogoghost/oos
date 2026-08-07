# `@oos/vite-plugin`

This package builds Solid TSX applications for the OOS QuickJS runtime. It
selects Solid's universal renderer, preserves native `oos:*` imports, rejects
CSS and DOM renderer imports, and emits one self-contained `main.mjs` bundle.

Install it alongside the runtime SDK:

```sh
npm install solid-js @oos/platform
npm install --save-dev vite @oos/vite-plugin
```

```ts
// vite.config.ts
import { defineConfig } from "vite";
import oos from "@oos/vite-plugin";

export default defineConfig({
  plugins: [oos({ entry: "src/main.tsx" })],
});
```

Use the OOS JSX definitions in `tsconfig.json`:

```json
{
  "compilerOptions": {
    "jsx": "preserve",
    "jsxImportSource": "@oos/platform",
    "moduleResolution": "Bundler"
  }
}
```

The entry exports the OOS lifecycle functions. Only `frame` is mandatory;
`initialize`, `onKey`, and `shutdown` are optional. UI source uses the OOS
intrinsics instead of HTML elements:

```tsx
import { render } from "@oos/platform/solid";

export function initialize() {
  render(() => (
    <view class="flex flex-col w-full h-full p-4 bg-black">
      <text class="text-lg text-white" text="Hello" />
    </view>
  ));
}

export function frame() {
  return 1000;
}
```

Tailwind utility strings are interpreted by OOS at runtime; the plugin does
not emit CSS. Images and other package data must be placed under `assets/` and
loaded through `@oos/platform/services`, rather than imported as browser
assets.

`vite build` writes a single `dist/main.mjs`. Dynamic imports, CSS outputs,
browser assets, `solid-js/web`, and entries without a `frame` export fail the
build. The output can therefore be copied directly to `app/main.mjs` before
running `scripts/package-oos-app.sh`.
