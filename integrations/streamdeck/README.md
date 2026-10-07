# Open Keylight Chroma for Stream Deck

Local controls for the independent Open Keylight API v1. This plugin does not control stock Razer firmware or require a cloud account.

## Install

1. Install Stream Deck 7.0 or later on Windows 10+ or macOS 12+. The manifest selects Stream Deck's bundled Node 20 runtime.
2. Open the built `dist/org.openkeylight.chroma.streamDeckPlugin` package to install it. Building this project does **not** install it or modify Stream Deck profiles.
3. Drag an action from **Open Keylight Chroma** onto a key or compatible dial.
4. Enter the light's explicit local origin, such as `http://open-keylight.local` or `http://192.168.1.42`, and paste a token obtained from the dashboard's pairing flow. Initial access uses the physical button; an already trusted client can open another pairing window. Save settings.
5. **Check connection** reads API information without changing output. A reachable API does not prove that the token authorizes mutations.

The token is stored in that action's Stream Deck settings, not an encrypted credential vault. Exported actions/profiles may contain it. Do not share credential-bearing exports. The plugin never intentionally logs tokens or HTTP headers; leave SDK trace logging disabled.

## Actions

| Action | Key | Dial / touch strip |
| --- | --- | --- |
| Power | Toggle power | Use the brightness dial's press/tap |
| Brightness | Change by a configured signed 1, 5, 10 or 25 percentage points | Rotate with that step magnitude; press/tap toggles power |
| Scene | Recall saved scene 1–8 | — |
| Recording lock | Explicitly lock or unlock | — |

Off remains available during recording lock. All other output changes require a separate unlock. Brightness changes do not implicitly turn an off light on. Scenes must already exist on the light and its `scenes` capability must be advertised. There are no fabricated scene presets or successful unsupported operations.

A value followed by `*` is requested or awaiting matching controller readback. It is not described as confirmed until `reported.valid`, the appropriate `confirmed_fields`, the returned value, and idle operation agree. This still is not an optical measurement. **Scene N / Ready** means the recall button is available, not that this scene is currently active.

Visible actions refresh every five seconds; simultaneous reads share a request. Every mutation reads the current revision first. One operation per device is allowed across all actions. Dial ticks coalesce, and a failed/uncertain submission drops queued ticks. HTTP conflicts, recording lock, missing authorization and connection failures remain visible. No mutation is automatically retried. Multi Actions and key-logic sequences are intentionally unsupported in this first version.

## Build and verify

```powershell
npm ci
npm run build
npm test
npm run validate
npm run pack
```

Dependencies are pinned in the lockfile. The official SDK is `@elgato/streamdeck` 3.0.1 and the local CLI is 1.10.1. Build and test tools use Node 22.16 or newer; the shipped plugin targets Node 20 and uses Stream Deck's bundled runtime. The build bundles the SDK into the plugin; no npm install is required on the user's machine. Nothing is installed globally.

To exercise the compiled plugin with the runtime from an installed Stream Deck application, set `OPEN_KEYLIGHT_PLUGIN_NODE` to that executable before running the tests. On Windows:

```powershell
$env:OPEN_KEYLIGHT_PLUGIN_NODE = "$env:APPDATA\Elgato\StreamDeck\NodeJS\20.20.0\node.exe"
npm test
```

Use the runtime path from your installed version. The runtime harness covers registration, power, brightness dial, scene recall, recording lock and settings replies without message identifiers. The plugin explicitly enables the SDK's legacy settings behaviour for compatibility with Stream Deck 7.0.

`npm test` includes pure API/queue tests, a DOM harness for the settings panel, and a real compiled-plugin run connected to fake Stream Deck WebSocket events and a loopback HTTP light. It exercises key power, dial brightness, recording lock/unlock, scene recall and a read-only connection check. It never contacts a physical light. A passing harness and package validation do not replace verification in a real Stream Deck application/device; that installation has not been performed as part of this build.

Generated raster artwork is included locally: the abstract plugin badge uses 256/512 px PNGs and the key states use 72/144 px PNGs. Elgato's action list uses separate white transparent 20/40 px glyphs, with a 28/56 px category glyph. No SVG icons ship in the plugin. The generated originals, provenance and mechanical export script are kept under `art/` and `scripts/export-art.mjs`; `npm run art` recreates the exports. The property inspector has no CDN dependencies. Third-party notices are included in the plugin package.

Official references: [SDK getting started](https://docs.elgato.com/streamdeck/sdk/introduction/getting-started/), [manifest](https://docs.elgato.com/streamdeck/sdk/references/manifest/), [dial feedback](https://docs.elgato.com/streamdeck/sdk/guides/dials/), [property inspector protocol](https://docs.elgato.com/streamdeck/sdk/references/websocket/ui/), [distribution](https://docs.elgato.com/streamdeck/sdk/introduction/distribution/).
