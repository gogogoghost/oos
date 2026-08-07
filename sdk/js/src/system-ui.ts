/// <reference path="./oos-native.d.ts" />

import {
  setLocked,
  snapshot as nativeSnapshot,
  type SystemUiSnapshot,
} from "oos:system-ui";

export function snapshot(): SystemUiSnapshot {
  return JSON.parse(nativeSnapshot()) as SystemUiSnapshot;
}

export { setLocked, type SystemUiSnapshot };
