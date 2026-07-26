import tailwindcss from "@tailwindcss/vite";
import { defineConfig } from "vite";
import { viteSingleFile } from "vite-plugin-singlefile";
import solidPlugin from "vite-plugin-solid";

export default defineConfig({
  base: "./",
  plugins: [solidPlugin(), tailwindcss(), viteSingleFile()],
  build: {
    target: "es2020",
    cssCodeSplit: false,
    sourcemap: false,
  },
});
