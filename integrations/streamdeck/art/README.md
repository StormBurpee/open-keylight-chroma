# Stream Deck artwork

`source/` preserves six generated PNG originals byte-for-byte. Their original generated filenames are recorded in `source-files.json`; dimensions and SHA-256 values are in `exports.json`. The artwork depicts abstract glass, light and control symbols, not a physical lamp.

Run `npm run art` to regenerate the shipped PNGs with pinned Sharp. Generated images receive only proportional Lanczos downsampling and PNG export: no recolouring, cropping, retouching or text is added. Source files stay outside the installable plugin package.

The plugin badge is 256/512 px; keys are 72/144 px. The smaller action-list glyphs are mechanical raster exports of the project's original outline symbols, with a white foreground and transparent background, as required by Elgato. They are 20/40 px, with the category symbol at 28/56 px. The category uses the abstract power glyph. No SVG files ship in the plugin.

`review/keys-72px.png` places all five key states side by side at their native 72 px size. Titles remain live Stream Deck overlays in the artwork's lower dark margin.

Sizes and formats: [Elgato manifest reference](https://docs.elgato.com/streamdeck/sdk/references/manifest/), [plugin artwork guidelines](https://docs.elgato.com/guidelines/stream-deck/plugins/).
