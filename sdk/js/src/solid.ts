/// <reference path="./oos-native.d.ts" />

import { createRenderer } from "solid-js/universal";
import { clear, submit } from "oos:solid-internal";
import {
  createPlatformHost,
  type PlatformNode,
  type PlatformProperty,
} from "./solid-host";

const host = createPlatformHost(submit, clear);

const renderer = createRenderer<PlatformNode>({
  createElement: (type) => host.createElement(type),
  createTextNode: (value) => host.createTextNode(value),
  replaceText: (node, value) => host.replaceText(node, value),
  setProperty: (node, name, value) =>
    host.setProperty(node, name, value as PlatformProperty),
  insertNode: (parent, node, anchor) => host.insertNode(parent, node, anchor),
  isTextNode: (node) => node.type === "#text",
  removeNode: (parent, node) => host.removeNode(parent, node),
  getParentNode: (node) => node.parent,
  getFirstChild: (node) => node.children[0],
  getNextSibling: (node) => {
    if (!node.parent) return undefined;
    const index = node.parent.children.indexOf(node);
    return index < 0 ? undefined : node.parent.children[index + 1];
  },
});

export const {
  effect,
  memo,
  createComponent,
  createElement,
  createTextNode,
  insertNode,
  insert,
  spread,
  setProp,
  mergeProps,
  use,
} = renderer;

export function render(code: () => PlatformNode): () => void {
  const dispose = renderer.render(code, host.root);
  host.flush();
  return () => {
    dispose();
    host.clear();
  };
}

export type { PlatformNode } from "./solid-host";
