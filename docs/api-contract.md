# API v1 implementation contract

All paths use `/api/v1`. JSON responses are UTF-8. Mutations require `Authorization: Bearer <device-token>`; tokens are installed locally or issued inside a physical pairing window. Same-origin browser requests only. Bodies are bounded; unknown fields and invalid values fail without partial application.

`GET /device`: `{id,name,model,firmware,api_version:1,uptime_ms,network:{connected,rssi,ip},controller:{connected,version},capabilities:{...}}`.

`GET /state`: `{revision,desired:{power,mode,brightness,temperature_k,rgb:{r,g,b},transition_ms,effect,recording_lock},reported:{...same output fields...,valid},operation:{status,error},last_actor}`. `mode` is `white` or `color`; brightness is 0–100; temperature 3000–7000 K; RGB bytes 0–255; transition 0–10000 ms; effect `none`, `aurora`, or `breathe`. Reported output is getter-confirmed where available, not an optical measurement. `operation.status` is `idle`, `pending`, or `error`.

`PATCH /state`: partial desired fields plus optional `expected_revision`. Optional `X-Keylight-Actor: dashboard|streamdeck|automation`; actor is attribution, not an authorization role. A valid authorized patch returns 202 and the state envelope. Stale expected_revision returns 409. No optimistic success indicator before worker confirmation. Recording lock rejects output changes with 423 except Off; an explicit separate unlock is required before changing output. Actor headers cannot bypass it. Never automatically retry a timed-out mutation.

`GET /history`: `{entries:[{sequence,uptime_ms,actor,event,detail}]}` bounded newest-first history.

`GET /scenes`: `{scenes:[{id,name,state}]}`. `PUT /scenes/{id}` stores a named current or supplied state; `POST /scenes/{id}/activate` applies it. Eight bounded scenes, ids 1–8, names max32 UTF-8 bytes. Initial presets can be created in UI only when saved; no fictitious device scenes.

`GET /settings`: `{name,role,mqtt:{enabled,uri,username,connected},button:{single:"toggle",double:"next_scene",hold:"pair"}}`, without credentials. `PATCH /settings`: name/role plus MQTT configuration, password write-only. Role is `key`, `fill`, `background`, or `other`.

`POST /pair`: during the physical pairing window, `{label}` returns `{token}` once. `POST /update`: authenticated application/octet-stream upload, `X-SHA256` header and application-image validation; response accepted before controlled reboot. Unsupported/unimplemented endpoints must return a real error and the UI must not simulate success.

`POST /confirm`: authenticated acceptance of the current trial application. A new image has a 180-second application-level trial; absent confirmation, it selects the previous valid slot and restarts. This does not protect failures before application startup. Another OTA upload is refused while a trial is pending so its previous slot is preserved.

`POST /pairing`: an already paired client can open one 180-second pairing window. The physical three-second button hold opens the same window. `GET /clients` requires authentication and returns `{clients:[{id,label}]}` without token material. `DELETE /clients/{id}` revokes that client; revoking the current client also ends its access. Tokens are stored as hashes in one atomic NVS record. Holding the physical button continuously for ten seconds beginning at boot revokes all clients while preserving Wi-Fi and scenes.

When disconnected from a configured network, a pairing hold also opens a temporary `Keylight-Setup-…` access point for 180 seconds. Pairing does not shorten that network's lifetime. A light with no configured network remains in setup mode. Connect to that network and open `http://192.168.4.1/`; the System page accepts a new SSID/password and explicitly saves and restarts. Credentials are write-only.

`GET /device` also reports `pairing_open`, `trial_pending`, `free_heap_bytes` and `reset_reason`. Capability booleans are `white`, `color`, `transitions`, `effects`, `scenes`, `settings`, `ota`, and `white_transitions`; absent capabilities must not be assumed. `effect_names` is the supported list. `reported.confirmed_fields` identifies which normalized fields are supported by a controller getter. During the compatibility backend's custom RGB rendering, native RGB is not getter-readable and its report is invalidated. Parked native RGB/brightness can differ in representation from the desired state while expressing the same quantized output; the API reports that raw controller representation honestly.

The dashboard polls state at 1 Hz while visible, never during an in-flight mutation. It coalesces continuous controls and keeps at most one mutation in flight. Connection errors retain the last confirmed display and visibly mark it stale. Demo mode is explicit (`?demo=1`) with an always-visible label and entirely separate transport.
