import { render } from "solid-js/web";

import { Launcher } from "./Launcher";
import "./styles.css";

const root = document.getElementById("root");

if (!root) {
  throw new Error("Launcher root element is missing");
}

render(() => <Launcher />, root);
