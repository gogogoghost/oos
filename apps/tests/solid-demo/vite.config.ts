import { defineConfig } from "vite";
import oos from "@oos/vite-plugin";

export default defineConfig({
  plugins: [
    oos({ outDir: "../../../build/native-apps/solid-demo" }),
  ],
});
