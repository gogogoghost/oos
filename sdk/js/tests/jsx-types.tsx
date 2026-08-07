import { createCanvas } from "../src/canvas";
import type { JSX } from "../src/jsx-runtime";

const canvas = createCanvas("2d", { width: 20, height: 20 });
const valid: JSX.Element = (
  <view class="flex flex-col">
    <text text="Hello" class="text-sm" />
    <canvas canvas={canvas} class="grow w-full" />
  </view>
);

void valid;
