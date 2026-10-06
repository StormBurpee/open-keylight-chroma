import { readdir, readFile, writeFile, mkdir } from "node:fs/promises";
import { gzipSync } from "node:zlib";
import { createHash } from "node:crypto";
const root = new URL("../dist/", import.meta.url),
  entries = [];
async function walk(relative = "") {
  for (const item of await readdir(new URL(relative, root), {
    withFileTypes: true,
  })) {
    const name = relative + item.name;
    if (item.isDirectory()) {
      await walk(name + "/");
      continue;
    }
    if (name.endsWith(".gz") || name === "asset-manifest.json") continue;
    const data = await readFile(new URL(name, root)),
      gzip = gzipSync(data, { level: 9 });
    await writeFile(new URL(name + ".gz", root), gzip);
    entries.push({
      path: "/" + name,
      gzip_path: name + ".gz",
      content_type: name.endsWith(".html")
        ? "text/html; charset=utf-8"
        : name.endsWith(".css")
          ? "text/css; charset=utf-8"
          : name.endsWith(".js")
            ? "text/javascript; charset=utf-8"
            : "application/octet-stream",
      size: data.length,
      gzip_size: gzip.length,
      sha256: createHash("sha256").update(data).digest("hex"),
    });
  }
}
await mkdir(root, { recursive: true });
await walk();
const bytes = entries.reduce((sum, e) => sum + e.gzip_size, 0);
if (bytes > 230 * 1024)
  throw Error(`Embedded asset budget exceeded: ${bytes} bytes`);
await writeFile(
  new URL("asset-manifest.json", root),
  JSON.stringify({ version: 1, gzip_bytes: bytes, files: entries }, null, 2) +
    "\n",
);
console.log(
  `Embedded dashboard: ${bytes.toLocaleString()} gzip bytes / 235,520 budget (${entries.length} files).`,
);
