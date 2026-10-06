/** Mechanical raster exports. Full-resolution generated originals are never overwritten. */
import sharp from "sharp";
import { createHash } from "node:crypto";
import { readFile, writeFile, mkdir } from "node:fs/promises";

const output = "org.openkeylight.chroma.sdPlugin/imgs";
await mkdir(output, { recursive: true });
await mkdir("art/review", { recursive: true });
const sources = JSON.parse((await readFile("art/source-files.json", "utf8")).replace(/^\uFEFF/, ""));
const manifest = { tool: "sharp", version: sharp.versions.sharp, sources: {}, exports: [] };
const png = { compressionLevel: 9, palette: false, effort: 10 };
async function writePng(name, size, pipeline) {
  const path = `${output}/${name}.png`;
  const data = await pipeline.png(png).toBuffer();
  await writeFile(path, data);
  manifest.exports.push({ path, width: size, height: size, sha256: createHash("sha256").update(data).digest("hex") });
}
for (const name of Object.keys(sources)) {
  const path = `art/source/${name}.png`, data = await readFile(path);
  const info = await sharp(data).metadata();
  if (info.format !== "png" || info.width !== info.height) throw Error(`Expected a square source PNG: ${path}`);
  manifest.sources[name] = { path, generated_filename: sources[name], width: info.width, height: info.height, sha256: createHash("sha256").update(data).digest("hex") };
  const size = name === "plugin" ? 256 : 72;
  for (const scale of [1, 2]) {
    await writePng(name + (scale === 2 ? "@2x" : ""), size * scale,
      sharp(data).resize(size * scale, size * scale, { fit: "contain", kernel: "lanczos3" }));
  }
}

// Existing original outline glyphs, exported as white alpha masks for Elgato's
// action list. No physical lamp silhouette. Rich artwork is reserved for keys.
const glyphs = {
  power: '<path d="M72 22v34M49 32a31 31 0 1 0 46 0"/>',
  brightness: '<circle cx="72" cy="51" r="19"/><path d="M72 19v-9M72 84v9M39 51h-9M105 51h9M49 28l-7-7M95 74l7 7M49 74l-7 7M95 28l7-7"/>',
  scene: '<rect x="37" y="20" width="70" height="59" rx="7"/><path d="M53 36h38M53 48h28M53 60h18"/>',
  lock: '<rect x="44" y="43" width="56" height="40" rx="6"/><path d="M58 43V30a14 14 0 0 1 28 0"/><path d="M72 59v11"/>',
};
for (const [name, glyph] of Object.entries({ ...glyphs, category: glyphs.power })) {
  const size = name === "category" ? 28 : 20;
  const svg = Buffer.from(`<svg xmlns="http://www.w3.org/2000/svg" width="144" height="144"><g fill="none" stroke="#ffffff" stroke-width="5" stroke-linecap="round" stroke-linejoin="round">${glyph}</g></svg>`);
  const trimmed = await sharp(svg, { density: 576 }).trim().toBuffer();
  for (const scale of [1, 2]) {
    const pixels = size * scale, border = 2 * scale;
    const alpha = await sharp(trimmed).resize(pixels - border * 2, pixels - border * 2, { fit: "contain", background: "#00000000" })
      .extend({ top: border, bottom: border, left: border, right: border, background: "#00000000" }).ensureAlpha().extractChannel("alpha").toBuffer();
    await writePng(`${name === "category" ? name : name + "-list"}${scale === 2 ? "@2x" : ""}`, pixels,
      sharp({ create: { width: pixels, height: pixels, channels: 3, background: "#ffffff" } }).joinChannel(alpha));
  }
}
await writeFile("art/exports.json", JSON.stringify(manifest, null, 2) + "\n");
// A review strip at exactly 72 px per key; not shipped in the plugin package.
await sharp({ create: { width: 72 * 5, height: 72, channels: 3, background: "#111111" } })
  .composite(["power", "brightness", "scene", "lock", "locked"].map((name, index) => ({ input: `${output}/${name}.png`, left: 72 * index, top: 0 })))
  .png(png).toFile("art/review/keys-72px.png");
console.log(`Exported ${manifest.exports.length} PNGs without cropping or changing the generated artwork.`);
