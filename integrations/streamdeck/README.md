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

| Action         | Key                                                            | Dial / touch strip                                              |
| -------------- | -------------------------------------------------------------- | --------------------------------------------------------------- |
| Power          | Toggle power                                                   | Use the brightness dial's press/tap                             |
| Brightness     | Change by a configured signed 1, 5, 10 or 25 percentage points | Rotate with that step magnitude; press/tap toggles power        |
| Colour         | Select a colour using the settings colour picker               | Rotate hue in 1°, 5°, 10° or 15° steps; press/tap toggles power |
| Scene          | Recall saved scene 1–8                                         | —                                                               |
| Recording lock | Explicitly lock or unlock                                      | —                                                               |

Off remains available during recording lock. All other output changes require a separate unlock. Brightness and colour controls preserve power: changing an off light does not turn it on. Hue rotation preserves saturation and RGB intensity; neutral colours become saturated so the hue dial has a visible colour to adjust. Colour selection replaces any running effect with a static colour.

Choose an instant change or a short 100–400 ms colour fade in the action settings (150 ms by default). Fades apply only when the light advertises the appropriate capability. Current firmware advertises colour transitions but not white transitions, so white brightness changes remain direct steps. Coalescing preserves dial intent in both modes; it does not claim a white fade.

Scenes must already exist on the light and its `scenes` capability must be advertised. Recall reads the saved values and submits them with a fresh state revision, so another client's intervening change is rejected instead of overwritten. There are no fabricated scene presets or successful unsupported operations.

A value followed by `*` is requested or awaiting matching controller readback. It is not described as confirmed until `reported.valid`, the appropriate `confirmed_fields`, the returned value, and idle operation agree. This still is not an optical measurement. **Scene N / Ready** means the recall button is available, not that this scene is currently active.

Visible actions refresh every two seconds, with faster readback while a change is pending; simultaneous reads share a request. Dial feedback immediately shows the accumulated requested value with `*`. Turns coalesce over 60 ms, with at least 90 ms between write starts and only one mutation in flight per light. Turns received during a request remain in one bounded continuation. Reversing direction at 0% or 100% preserves the actual sequence of turns instead of losing steps to a saturated sum.

Every mutation reads the current revision first. A queued dial continuation stops if another client changes that revision. Power, scene and lock commands supersede unsent dial input and follow any mutation already in flight. Settings changes and disappearing actions cancel unsent work. A failed or uncertain response drops pending work, including a queued power press: check the light and press again deliberately. Nothing automatically retries a mutation. HTTP conflicts, recording lock, missing authorization and connection failures remain visible. Multi Actions and key-logic sequences are unsupported.

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

Use the runtime path from your installed version. The runtime harness covers registration, power, brightness and hue dials, colour selection, revision-guarded scene recall, recording lock and settings replies without message identifiers. The plugin explicitly enables the SDK's legacy settings behaviour for compatibility with Stream Deck 7.0. Existing action UUIDs, tokens, steps and saved-scene settings remain compatible with 0.1.x profiles.

`npm test` includes API and queue fault tests, a DOM harness for the settings panel, and a real compiled-plugin run connected to fake Stream Deck WebSocket events and a loopback HTTP light. The harness delays actual HTTP replies to prove that rapid dial input remains visible and accumulated while power presses supersede unsent turns. The pure queue suite compares 101,000 sequential clamp results and injects cancellation, external revisions and lost replies. It never contacts a physical light. This 0.2.0 build has not been installed or physically qualified as part of these offline checks.

Generated raster artwork is included locally: the abstract plugin badge uses 256/512 px PNGs and the key states use 72/144 px PNGs. Elgato's action list uses separate white transparent 20/40 px glyphs, with a 28/56 px category glyph. No SVG icons ship in the plugin. The generated originals, provenance and mechanical export script are kept under `art/` and `scripts/export-art.mjs`; `npm run art` recreates the exports. The property inspector has no CDN dependencies. Third-party notices are included in the plugin package.

Official references: [SDK getting started](https://docs.elgato.com/streamdeck/sdk/introduction/getting-started/), [manifest](https://docs.elgato.com/streamdeck/sdk/references/manifest/), [dial feedback](https://docs.elgato.com/streamdeck/sdk/guides/dials/), [property inspector protocol](https://docs.elgato.com/streamdeck/sdk/references/websocket/ui/), [distribution](https://docs.elgato.com/streamdeck/sdk/introduction/distribution/).
