import { test } from "node:test";
import assert from "node:assert/strict";
import { readFile, readdir } from "node:fs/promises";
import { createHash } from "node:crypto";
import sharp from "sharp";

const root = "org.openkeylight.chroma.sdPlugin";
const manifest = JSON.parse(await readFile(`${root}/manifest.json`, "utf8"));
const exports = JSON.parse(await readFile("art/exports.json", "utf8"));

test("every manifest image resolves to exact standard/high-DPI PNG dimensions", async () => {
  const images: Array<[string, number]> = [[manifest.Icon, 256], [manifest.CategoryIcon, 28]];
  for (const action of manifest.Actions) {
    images.push([action.Icon, 20]);
    for (const state of action.States) images.push([state.Image, 72]);
    if (action.Encoder?.Icon) images.push([action.Encoder.Icon, 72]);
  }
  for (const [image, size] of images) {
    assert.match(image, /^imgs\/[a-z-]+$/);
    for (const scale of [1, 2]) {
      const info = await sharp(`${root}/${image}${scale === 2 ? "@2x" : ""}.png`).metadata();
      assert.equal(info.format, "png");
      assert.deepEqual([info.width, info.height], [size * scale, size * scale], image);
    }
  }
  assert.equal((await readdir(`${root}/imgs`)).some((name) => name.endsWith(".svg")), false);
});

test("action-list and category glyphs remain white with real transparent backgrounds", async () => {
  for (const name of [manifest.CategoryIcon, ...manifest.Actions.map((action: { Icon: string }) => action.Icon)]) {
    for (const suffix of ["", "@2x"]) {
      const { data, info } = await sharp(`${root}/${name}${suffix}.png`).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
      assert.equal(info.channels, 4);
      let transparent = 0, visible = 0;
      for (let i = 0; i < data.length; i += 4) {
        if (data[i + 3] === 0) transparent++;
        else {
          visible++;
          assert.deepEqual([...data.subarray(i, i + 3)], [255, 255, 255]);
        }
      }
      assert.ok(transparent > visible, `${name} needs negative space`);
      assert.ok(visible > 20, `${name} must not be blank`);
    }
  }
});

test("generated originals and all raster exports match their recorded hashes", async () => {
  for (const file of [...Object.values(exports.sources), ...exports.exports] as Array<{ path: string; sha256: string }>) {
    const bytes = await readFile(file.path);
    assert.equal(createHash("sha256").update(bytes).digest("hex"), file.sha256, file.path);
  }
  const unlocked = await readFile(`${root}/imgs/lock.png`);
  const locked = await readFile(`${root}/imgs/locked.png`);
  assert.notDeepEqual(unlocked, locked, "recording-lock states need distinct artwork");
});
