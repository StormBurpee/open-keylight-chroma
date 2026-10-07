# Open Keylight Studio

The embedded dashboard is an original React/TypeScript interface built with locally generated shadcn/ui Radix components. It uses system fonts and local assets only. Light, Scenes and System share the same API v1 transport and mutation queue.

## Build and preview

Tested with Node 22.16 and npm 11.8:

```powershell
cd dashboard
npm ci
npm test
npm run build
npm run dev
```

Open `http://127.0.0.1:5178/?demo=1` for the explicitly labelled, isolated preview. The demo transport has no network access and never reads or changes saved device credentials. Its four sample scenes exist only in page memory. It is included only in development; production builds cannot activate it. Visiting the development page without `?demo=1` uses the real same-origin API, without a device proxy.

Production files are in `dashboard/dist`. `asset-manifest.json` lists each URL, MIME type, uncompressed SHA-256, original size and gzip size. The packaging step fails if total compressed assets exceed 230 KiB. Serve `/` using `index.html.gz`, and each listed asset path using its matching `.gz` file, the listed Content-Type and `Content-Encoding: gzip`. HTML should not be cached across firmware versions; hashed assets can use immutable caching. There are no external font, script or asset requests.

## Behaviour that matters

- Controls show requested values alongside a separate controller report. A field is described as confirmed only when the report is valid and names it in `confirmed_fields`. This is electrical/controller feedback, not an optical measurement.
- Mutations are serialized. Pending slider changes coalesce; a stale revision or uncertain timeout is not replayed. Poll replies begun before a mutation cannot overwrite its newer result. State polls at 1 Hz while visible and pauses during writes.
- Recording lock blocks every output change except Off. Unlock is an explicit, separate operation. Actor attribution cannot bypass the lock.
- Absent or false capability flags disable their controls. Effects come from `effect_names`. A false `white_transitions` disables white-mode transition selection while leaving colour transitions available.
- Scene save is explicit, with eight device slots. There are no simulated saved scenes on a real device. A scene does not store recording lock.
- A pasted or paired token is retained only in this tab's session storage, never in the URL. Initial access requires the firmware's physical pairing window. An already trusted client can explicitly open another bounded window. The dashboard uses same-origin bearer-authenticated requests; it does not add TLS to an HTTP device.
- System includes Wi-Fi setup with an explicit save-and-restart action. SSID length is checked in UTF-8 bytes; choosing an open network is deliberate. Passwords are submitted only in the authenticated settings write and then cleared from the form. Existing Wi-Fi passwords are never requested or displayed.
- Paired clients load through the authenticated `/clients` endpoint. Revocation requires a named confirmation, does not expose tokens, and is never retried after an uncertain response. The dashboard identifies this tab using the same SHA-256 prefix as the device; confirmed self-revocation clears its session token.
- System's **Open pairing for another client** explicitly posts the authenticated `/pairing` endpoint without a body. The confirmed window lasts at most 180 seconds and closes after one successful pairing. No window opens on page load; a new client still requests its own token through `/pair`. This avoids exporting another client's token.
- OTA computes SHA-256 locally, including on plain HTTP where Web Crypto may be unavailable. Upload remains disabled until the user supplies a matching expected release digest. The header/size check is preliminary; firmware performs the actual image validation. A 202 response means accepted, not proof of a successful reboot.
- Trial firmware displays a persistent confirmation banner. Only an explicit click posts `/api/v1/confirm`; confirmation is never automatic. The previous-application fallback is application-level recovery and is not a promise that an unbootable image can recover without serial access.

## Verification

Tests cover API headers and binary uploads, timeout/conflict handling, queued writes, delayed-poll races, invalid state, exact RGB conversion, portable hash vectors, lock semantics, capability gating, trial confirmation, isolated preview credentials, empty scenes and OTA digest review. Run `npm test` for the current count and `npm run build` for the current compressed budget.

The current graphite, ivory and copper design follows the refined concept in `assets/design/studio-concept-v2.png`. That file is generated design direction, not a browser screenshot. Scene photography and prompts are retained alongside it; the interface is implemented as native controls rather than a flattened image. Browser review covered desktop Light, Scenes and System pages and layouts at 300 and 868 CSS pixels without horizontal overflow. The captures in `dashboard/review` show the preceding design and are retained as historical evidence.

Current screenshots in [`assets/dashboard`](../assets/dashboard) are captured from the reference light serving ESP 0.1.7, with original NXP 0.1.1 ready. They show the installed interface rather than the demo; the Light capture includes getter-confirmed colour and brightness.

The colour picker provides pointer gestures and keyboard-accessible hue, saturation and value controls. Cancelling a gesture, changing sections or recalling a scene discards an unapplied colour draft and resumes fresh device state. Tests exercise these boundaries, scene failures and remembered fade duration. UI tests and local previews do not establish physical lamp behavior.

Component provenance is recorded in `dashboard/components.json`. The shadcn CLI generated Radix Nova components; small local adjustments provide slider thumb labelling. Dependency versions are locked by `package-lock.json`. Distributed asset licensing is recorded in `dashboard/THIRD_PARTY_NOTICES.md`.
