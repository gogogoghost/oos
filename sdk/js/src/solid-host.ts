import type { Canvas } from "./canvas";

export type PlatformElementName = "view" | "text" | "canvas";
export type PlatformProperty = string | number | boolean | Canvas | null | undefined;

export interface NativeUiNode {
  id: number;
  parent?: number;
  kind: "container" | "text" | "canvas";
  class?: string;
  text?: string;
  canvas?: number;
}

export class PlatformNode {
  readonly children: PlatformNode[] = [];
  parent?: PlatformNode;
  className = "";
  text = "";
  canvas?: Canvas;

  constructor(
    readonly id: number,
    readonly type: PlatformElementName | "#text" | "#root",
    private readonly changed: () => void,
  ) {}

  setProperty(name: string, value: PlatformProperty): void {
    if (name === "class" || name === "className") {
      this.className = value == null ? "" : String(value);
    } else if (name === "text" || name === "textContent") {
      this.text = value == null ? "" : String(value);
    } else if (name === "canvas") {
      if (value != null && typeof value !== "object") {
        throw new TypeError("the canvas property expects an OOS Canvas");
      }
      this.canvas = value as Canvas | undefined;
    } else {
      throw new TypeError(`unsupported Solid platform property: ${name}`);
    }
    this.changed();
  }

  replaceText(value: string): void {
    this.text = value;
    this.changed();
  }
}

export interface PlatformHost {
  root: PlatformNode;
  createElement(type: string): PlatformNode;
  createTextNode(value: string): PlatformNode;
  replaceText(node: PlatformNode, value: string): void;
  setProperty(node: PlatformNode, name: string, value: PlatformProperty): void;
  insertNode(parent: PlatformNode, node: PlatformNode, anchor?: PlatformNode): void;
  removeNode(parent: PlatformNode, node: PlatformNode): void;
  serialize(): NativeUiNode[];
  flush(): void;
  clear(): void;
}

export function createPlatformHost(
  submit: (nodes: NativeUiNode[]) => void,
  clearNative: () => void,
): PlatformHost {
  let nextId = 1;
  let queued = false;
  let queueVersion = 0;
  const publish = (): void => {
    if (root.children.length > 0) submit(serialize());
    else clearNative();
  };
  const schedule = (): void => {
    if (queued) return;
    queued = true;
    const version = ++queueVersion;
    Promise.resolve().then(() => {
      if (!queued || version !== queueVersion) return;
      queued = false;
      publish();
    });
  };
  const root = new PlatformNode(0, "#root", schedule);

  const serialize = (): NativeUiNode[] => {
    const output: NativeUiNode[] = [];
    const visit = (node: PlatformNode, parent?: number): void => {
      if (node.type === "#root") {
        for (const child of node.children) visit(child);
        return;
      }
      const kind =
        node.type === "canvas" ? "canvas" : node.type === "#text" || node.type === "text" ? "text" : "container";
      const text = kind === "text"
        ? node.text + node.children
            .filter((child) => child.type === "#text")
            .map((child) => child.text)
            .join("")
        : "";
      output.push({
        id: node.id,
        ...(parent === undefined ? {} : { parent }),
        kind,
        ...(node.className ? { class: node.className } : {}),
        ...(kind === "text" ? { text } : {}),
        ...(kind === "canvas" && node.canvas ? { canvas: node.canvas.handle } : {}),
      });
      if (node.type !== "text") {
        for (const child of node.children) visit(child, node.id);
      }
    };
    visit(root);
    return output;
  };

  return {
    root,
    createElement(type: string): PlatformNode {
      if (type !== "view" && type !== "text" && type !== "canvas") {
        throw new TypeError(`unsupported Solid platform element: ${type}`);
      }
      return new PlatformNode(nextId++, type, schedule);
    },
    createTextNode(value: string): PlatformNode {
      const node = new PlatformNode(nextId++, "#text", schedule);
      node.text = value;
      return node;
    },
    replaceText(node, value): void {
      node.replaceText(value);
    },
    setProperty(node, name, value): void {
      node.setProperty(name, value);
    },
    insertNode(parent, node, anchor): void {
      if (parent.type === "text" && node.type !== "#text") {
        throw new TypeError("text elements may contain only text children");
      }
      if (node.parent) {
        const oldIndex = node.parent.children.indexOf(node);
        if (oldIndex >= 0) node.parent.children.splice(oldIndex, 1);
      }
      const index = anchor ? parent.children.indexOf(anchor) : -1;
      if (index >= 0) parent.children.splice(index, 0, node);
      else parent.children.push(node);
      node.parent = parent;
      schedule();
    },
    removeNode(parent, node): void {
      const index = parent.children.indexOf(node);
      if (index >= 0) parent.children.splice(index, 1);
      node.parent = undefined;
      schedule();
    },
    serialize,
    flush(): void {
      queued = false;
      ++queueVersion;
      publish();
    },
    clear(): void {
      queued = false;
      ++queueVersion;
      root.children.length = 0;
      clearNative();
    },
  };
}
