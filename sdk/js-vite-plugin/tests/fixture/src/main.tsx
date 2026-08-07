import { createSignal } from "solid-js";
import { createCanvas, rgba } from "@oos/platform/canvas";
import { render } from "@oos/platform/solid";

const surface = createCanvas("2d", { width: 32, height: 24 });
const context = surface.getContext("2d");
const [count, setCount] = createSignal(0);
let dispose: (() => void) | undefined;

export function initialize(): boolean {
  context.beginFrame(rgba(20, 30, 40));
  context.flush();
  dispose = render(() => (
    <view class="flex flex-col w-full h-full p-2 bg-black">
      <text class="text-sm text-white">Count {count()}</text>
      <canvas class="grow w-full" canvas={surface} />
    </view>
  ));
  return true;
}

export function onKey(): void {
  setCount((value) => value + 1);
}

export function frame(): number {
  return 1000;
}

export function shutdown(): void {
  dispose?.();
  surface.destroy();
}
