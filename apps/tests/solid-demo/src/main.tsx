import { createSignal } from "solid-js";
import { render } from "@oos/platform/solid";

const [count, setCount] = createSignal(0);
let dispose: (() => void) | undefined;

export function initialize(): boolean {
  dispose = render(() => (
    <view class="flex flex-col w-full h-full p-4 gap-3 bg-black">
      <text class="text-xl text-white">Solid on OOS</text>
      <view class="grow w-full flex flex-col items-center justify-center bg-green-600 rounded-lg">
        <text class="text-lg text-white">Count {count()}</text>
      </view>
      <text class="text-sm text-gray-300">Press any key to update</text>
    </view>
  ));
  return true;
}

export function onKey(): boolean {
  setCount((value) => value + 1);
  return true;
}

export function frame(): number {
  return 1000;
}

export function shutdown(): void {
  dispose?.();
  dispose = undefined;
}
