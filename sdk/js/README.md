# `@oos/platform`

`@oos/platform` is the typed JavaScript SDK for OOS applications. It provides
the Solid universal renderer, host-owned Canvas2D/mesh/WebGL canvases, runtime
services, and package-module messaging without exposing a DOM or browser
globals.

Install the SDK and the OOS Vite integration:

```sh
npm install solid-js @oos/platform
npm install --save-dev vite @oos/vite-plugin typescript
```

Configure JSX types in `tsconfig.json`:

```json
{
  "compilerOptions": {
    "jsx": "preserve",
    "jsxImportSource": "@oos/platform",
    "moduleResolution": "Bundler"
  }
}
```

Applications import only the APIs they use. Solid elements are `view`, `text`,
and `canvas`; a canvas may independently provide a `2d`, `mesh2d`, or `webgl`
context and can be attached anywhere in the Solid tree.

```tsx
import { createCanvas } from "@oos/platform/canvas";
import { render } from "@oos/platform/solid";

const canvas = createCanvas("2d", { width: 160, height: 90 });

export function initialize() {
  render(() => (
    <view class="flex flex-col w-full h-full p-4 bg-black">
      <text class="text-lg text-white" text="Hello" />
      <canvas class="grow w-full" canvas={canvas} />
    </view>
  ));
}

export function frame() {
  return 1000;
}
```

The package exports `runtime`, `services`, `modules`, `canvas`, `graphics`,
`mesh`, `webgl`, and `solid` entry points. Native `oos:*` modules are resolved
only by the OOS QuickJS host. Use `@oos/vite-plugin` to bundle dependencies and
preserve those native imports in the application entry.
