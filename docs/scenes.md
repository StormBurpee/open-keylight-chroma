# Stored scenes

The first boot with this scene-store version saves four real scenes when free
slots are available. It does not activate one or change the desired output.

| Scene | Mode | Brightness | Colour / temperature | Transition |
| --- | --- | --- | --- | --- |
| Focus | White | 80% | 4200 K | 1200 ms |
| Blue hour | Colour | 72% | `#245CFF` | 1200 ms |
| Ember | Colour | 60% | `#FF7026` | 1200 ms |
| Afterglow | Colour | 55% | `#B28AFF` | 1200 ms |

All four are powered on, use no effect and have Recording Lock cleared. White
transition support depends on the controller backend. These are the dashboard
showcase values, now persisted on the device and returned by `/api/v1/scenes`.

## Migration and durability

`scene_store.c` migrates the existing `scene0_v1`–`scene7_v1` slots into one
400-byte `scenes_v2` NVS blob. The format has a version, a durable seeding marker
and eight fixed-size scene entries; fields are encoded explicitly rather than
writing C structure padding or enum representations. Existing IDs and settings
stay in their original slots. Existing unused legacy records also reserve their
IDs during migration. A matching default name, compared without ASCII case,
preserves that existing scene instead of adding another copy.

Defaults fill the lowest available IDs in table order. If all slots are full,
the collection is still marked seeded. Later edits, replacements or storage-level
deletions retain this marker, so removing a default or freeing a slot does not
make a default reappear. The current HTTP API supports replacement, not deletion.

The collection and marker are written as **one NVS blob**, followed by commit;
this does not rely on a transaction spanning several keys. ESP-IDF's NVS blob
writer writes the new chunks and index before replacing the old version. The
source reviewed is `components/nvs_flash/src/nvs_storage.cpp` in ESP-IDF 5.5.5,
particularly `writeMultiPageBlob` and `Storage::writeItem`. New defaults and edits
are published to RAM only after a successful commit. Valid legacy scenes remain
readable if their migration cannot be saved. A failed save returns an error and prevents another
scene edit until reboot reloads storage, because a failure response alone cannot
prove whether the write became durable.

A present but malformed, unsupported or unreadable scene record is never
treated as an empty collection. Migration stops without overwriting it. An
invalid legacy slot similarly blocks migration. Shared NVS is never erased;
configuration, Wi-Fi credentials and paired clients keep their existing keys.
Old legacy scene keys remain intact for rollback. Once `scenes_v2` exists it is
authoritative: edits made by older firmware to legacy keys are not imported again.

## Verification

`python3 tests/services/run_tests.py` compiles the real storage and scene-store
code with deterministic NVS boundaries and AddressSanitizer. Cases cover fresh
startup, exact defaults, no activation, reboot, modified/default-name duplicates,
partial and full legacy collections, retained IDs, deletion without reseeding,
corrupt records, absent namespaces, read/write/commit failures, and both old and
new durable outcomes following a failed commit. Configuration and client bytes
are checked unchanged. Physical power-cut testing is not claimed by these host
tests; blob integrity and recovery remain the NVS driver's responsibility.
