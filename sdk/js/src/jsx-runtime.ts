import type { Canvas } from "./canvas";
import type { PlatformNode } from "./solid-host";

interface CommonProps {
  class?: string;
  className?: string;
  children?: JSX.Element | JSX.Element[] | string | number | null | undefined;
}

export namespace JSX {
  export type Element = PlatformNode;

  export interface IntrinsicElements {
    view: CommonProps;
    text: CommonProps & { text?: string };
    canvas: CommonProps & { canvas: Canvas };
  }
}
