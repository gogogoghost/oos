import { describe, expect, it, vi } from "vitest";
import { createPlatformHost } from "../src/solid-host";

describe("Solid platform host", () => {
  it("serializes a platform tree without DOM objects", () => {
    const submit = vi.fn();
    const clear = vi.fn();
    const host = createPlatformHost(submit, clear);
    const view = host.createElement("view");
    const text = host.createElement("text");
    host.setProperty(view, "class", "flex flex-col p-4");
    host.setProperty(text, "text", "Hello");
    host.insertNode(host.root, view);
    host.insertNode(view, text);
    host.flush();

    expect(host.serialize()).toEqual([
      { id: view.id, kind: "container", class: "flex flex-col p-4" },
      { id: text.id, parent: view.id, kind: "text", text: "Hello" },
    ]);
    expect(submit).toHaveBeenCalledOnce();
    expect("ownerDocument" in view).toBe(false);
    host.clear();
    expect(clear).toHaveBeenCalledOnce();
  });

  it("rejects DOM and CSS-shaped properties", () => {
    const host = createPlatformHost(() => undefined, () => undefined);
    const view = host.createElement("view");
    expect(() => host.setProperty(view, "style", "position: fixed")).toThrow(
      /unsupported Solid platform property/,
    );
    expect(() => host.createElement("div")).toThrow(
      /unsupported Solid platform element/,
    );
    expect(() =>
      host.setProperty(view, "onClick", (() => undefined) as never),
    ).toThrow(
      /unsupported Solid platform property/,
    );
  });

  it("merges text children and clears an empty reactive root", async () => {
    const submit = vi.fn();
    const clear = vi.fn();
    const host = createPlatformHost(submit, clear);
    const label = host.createElement("text");
    const prefix = host.createTextNode("Count ");
    const value = host.createTextNode("1");
    host.insertNode(label, prefix);
    host.insertNode(label, value);
    host.insertNode(host.root, label);
    host.flush();
    await Promise.resolve();

    expect(submit).toHaveBeenCalledOnce();
    expect(host.serialize()).toEqual([
      { id: label.id, kind: "text", text: "Count 1" },
    ]);

    host.removeNode(host.root, label);
    await Promise.resolve();
    expect(clear).toHaveBeenCalledOnce();

    host.insertNode(host.root, label);
    await Promise.resolve();
    expect(submit).toHaveBeenCalledTimes(2);
  });
});
