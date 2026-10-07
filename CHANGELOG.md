# Changelog

There is no qualified public installation release yet. These entries describe development builds; [the development record](PROGRESS.md) separates automated checks from device observations.

## 0.2.0-alpha.1 — candidate

- Add a guided React / Ink installer with local discovery, named device selection, verified release bundles, stage progress and explicit visual checks.
- Acquire the reviewed stock recovery image from Razer's official download automatically. No vendor firmware is included in the repository or release ZIP.
- Prepare a Windows x64 launcher with checksum-pinned portable Node and Python runtimes; no global runtime or compiler installation is required.
- Check the running ESP image, embedded dashboard, controller and low-output controls before first-boot confirmation; keep the paired credential in a private local file.
- Fix the migration HTTP reader's handling of completed and truncated responses. Real HTTP regressions cover compressed, fragmented, chunked and close-delimited responses.
- Add reproducible, allowlisted release packaging and a user-focused setup guide with actual dashboard and Ink screenshots.

Hardware qualification and the final public release record remain pending. NXP lighting remains **0.1.1.0** and Stream Deck remains **0.1.3**.

## 0.1.9-dev

- Clear a temporary dashboard connection error after fresh device and state reads succeed. Keep an uncertain command visible across a later outage; reconnecting never retries it.
- Add an experimental guided stock-migration installer and offline plan preparation, with exact artifact checks, staged controller diagnostics and ESP installation last. Fresh-stock hardware qualification remains pending.

## 0.1.8-dev

- Update progress changes from blue through cyan to green, with gentle breathing and a green completion point after verification. Failed uploads retain red pulses.
- The update's combined blue/green level stays bounded; controller ownership, Off priority and restoration behavior are unchanged.

## 0.1.7-dev

- Coordinate ESP flash writes with complete NXP transactions. Sequential OTA erase avoids the previous bulk erase before upload reception.
- Bound admission waits and preserve the fixed reboot deadline even when an indicator worker has an older progress snapshot.
- Preserve the first repeated controller fault in history. Brownout startup requests and verifies Off once before reopening controls.
- Make MQTT availability follow controller health and update state; preserve physical scene selection when a command is refused.
- Add verified development packaging, Getting Started, installed-dashboard screenshots and compiled NXP regression CI.

This ESP build has been exercised with original NXP **0.1.1.0**, which makes duty reductions take effect before increases during channel handoff. Progressive colour tests through 100%, a two-minute high-output hold and subsequent control checks passed on the reference light. These observations do not establish general board compatibility or long-term thermal behavior.

Stream Deck **0.1.3** remains the companion plugin. Firmware and plugin versions are independent; see [version and release rules](docs/releasing.md).
