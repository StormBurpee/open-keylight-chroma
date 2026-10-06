import { build } from "esbuild";
import { readFile, writeFile, mkdir } from "node:fs/promises";
const plugin = "org.openkeylight.chroma.sdPlugin";
await mkdir(plugin + "/bin", { recursive: true });
await build({
  entryPoints: ["src/plugin.ts"],
  outfile: plugin + "/bin/plugin.js",
  bundle: true,
  platform: "node",
  format: "esm",
  target: "node24",
  minify: true,
  banner: {
    js: "import {createRequire as __createRequire} from 'node:module'; const require=__createRequire(import.meta.url);",
  },
  external: ["bufferutil", "utf-8-validate"],
  legalComments: "eof",
});
await writeFile(
  plugin + "/bin/package.json",
  JSON.stringify({ type: "module" }),
);
let notices = "Open Keylight Chroma Stream Deck third-party notices\n";
for (const name of [
  "@elgato/streamdeck",
  "@elgato/utils",
  "@elgato/schemas",
  "ws",
]) {
  notices +=
    "\n\n" +
    name +
    "\n" +
    (await readFile("node_modules/" + name + "/LICENSE", "utf8"));
}
await writeFile(plugin + "/THIRD_PARTY_NOTICES.txt", notices);
