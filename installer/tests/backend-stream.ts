// Synthetic output from the actual Python Migration + JSONL adapter tests.
// Addresses and display identity normalized; no hardware or firmware evidence.
export const backendStream = [
  {
    "v": 1,
    "seq": 0,
    "event": "status",
    "code": "plan_validated",
    "message": "Offline artifact and target plan checks passed",
    "summary": {
      "profile": "keylight-chroma-1.0.13",
      "target_ip": "192.168.1.25",
      "target_name": "Studio light",
      "device_id": "keylight-aabbcc",
      "esp": {
        "bytes": 400,
        "sha256": "9aa67bc7f99e5bd8cacf6eb76680f95b1d3df06ca06267868befee691d738f66",
        "project": "open_keylight",
        "version": "0.2.0-alpha.1",
        "idf_version": "v5.5",
        "elf_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
        "segments": [
          {
            "address": 1061158944,
            "bytes": 308
          },
          {
            "address": 1074266112,
            "bytes": 4
          }
        ],
        "min_revision": 0,
        "max_revision": 0,
        "integrity_verified": true,
        "authenticity_verified": false,
        "target_compatibility_verified": false
      },
      "packages": {
        "identity": "754cee8f243ab2d3037abf52a9be252401ace785c25620538f2ec21a7970904b",
        "OFF1": "2d179de81c1e7a8be0cc556739fb8c381d570127c8497d63f833ecec11cecb10",
        "LOW1": "0738ef1291d8b729c8f5f755fa8076a889d15b1ecc66e56476a0e23bde5b6a6f",
        "lighting": "067170e62058d4d7da16330895b20f3affd93c4dcceaaee50917f0f4fc053c34"
      },
      "controller_version": "0.1.0.0",
      "restore_sha256": "56b513625afb6753c0781067c32a0154111c216bd8ff9ff84a59aaf6dd0b66f3",
      "restore_provenance": "Synthetic reviewed test fixture only",
      "manifest_sha256": "cd35d136f9376a348961c5d0149376e7cfe7b9817e44d5d0f0fc7308a280888b",
      "device_operations": 0
    }
  },
  {
    "v": 1,
    "seq": 1,
    "event": "stage",
    "id": "stock",
    "index": 1,
    "total": 7,
    "phase": "started",
    "message": "Checking the selected stock light, verifying Off, and entering its controller loader."
  },
  {
    "v": 1,
    "seq": 2,
    "event": "stage",
    "id": "stock",
    "index": 1,
    "total": 7,
    "phase": "completed",
    "message": "Fresh resident controller verified."
  },
  {
    "v": 1,
    "seq": 3,
    "event": "stage",
    "id": "identity",
    "index": 2,
    "total": 7,
    "phase": "started",
    "message": "Installing the no-PWM identity trial, then waiting 33s for resident recovery."
  },
  {
    "v": 1,
    "seq": 4,
    "event": "stage",
    "id": "identity",
    "index": 2,
    "total": 7,
    "phase": "completed",
    "message": "ROM part identity and resident recovery verified."
  },
  {
    "v": 1,
    "seq": 5,
    "event": "stage",
    "id": "off1",
    "index": 3,
    "total": 7,
    "phase": "started",
    "message": "Installing OFF1 and checking its all-low register record, then waiting for recovery."
  },
  {
    "v": 1,
    "seq": 6,
    "event": "prompt",
    "id": "3c8ede1321430c73-1",
    "kind": "off1_observation",
    "message": "Did the light remain completely dark during OFF1? Type yes to proceed to five low pulses: ",
    "choices": [
      "yes",
      "no"
    ]
  },
  {
    "v": 1,
    "seq": 7,
    "event": "stage",
    "id": "off1",
    "index": 3,
    "total": 7,
    "phase": "completed",
    "message": "All-low register evidence and physical observation accepted."
  },
  {
    "v": 1,
    "seq": 8,
    "event": "stage",
    "id": "low1",
    "index": 4,
    "total": 7,
    "phase": "started",
    "message": "Watch the light: one short low pulse each of red, green, blue, warm white and cool white, with dark gaps."
  },
  {
    "v": 1,
    "seq": 9,
    "event": "prompt",
    "id": "3c8ede1321430c73-2",
    "kind": "low1_observation",
    "message": "Were those five low pulses correct, with dark gaps and no unexpected output? Type yes: ",
    "choices": [
      "yes",
      "no"
    ]
  },
  {
    "v": 1,
    "seq": 10,
    "event": "stage",
    "id": "low1",
    "index": 4,
    "total": 7,
    "phase": "completed",
    "message": "Five-channel record and physical observation accepted."
  },
  {
    "v": 1,
    "seq": 11,
    "event": "stage",
    "id": "lighting",
    "index": 5,
    "total": 7,
    "phase": "started",
    "message": "Installing and verifying the original lighting controller, then confirming it once."
  },
  {
    "v": 1,
    "seq": 12,
    "event": "stage",
    "id": "lighting",
    "index": 5,
    "total": 7,
    "phase": "completed",
    "message": "Original lighting controller confirmation read back."
  },
  {
    "v": 1,
    "seq": 13,
    "event": "stage",
    "id": "esp",
    "index": 6,
    "total": 7,
    "phase": "started",
    "message": "Uploading the ESP application once. Keep power connected; acceptance and first boot are separate checks."
  },
  {
    "v": 1,
    "seq": 14,
    "event": "stage",
    "id": "esp",
    "index": 6,
    "total": 7,
    "phase": "completed",
    "message": "Stock ESP accepted the image; independent first-boot acceptance remains."
  },
  {
    "v": 1,
    "seq": 15,
    "event": "stage",
    "id": "native",
    "index": 7,
    "total": 7,
    "phase": "started",
    "message": "Waiting for native HTTP, exact image/assets, and your explicit dashboard confirmation."
  },
  {
    "v": 1,
    "seq": 16,
    "event": "progress",
    "stage_id": "native",
    "scope": "startup_wait",
    "completed": 0,
    "total": 175,
    "unit": "seconds"
  },
  {
    "v": 1,
    "seq": 17,
    "event": "status",
    "code": "message",
    "message": "Waiting for the original ESP dashboard (0s elapsed; first boot can take over 90s). No update will be retried."
  },
  {
    "v": 1,
    "seq": 18,
    "event": "status",
    "code": "message",
    "message": "Exact original image and controller are ready. About 65s remain to verify assets, check controls and explicitly confirm the trial."
  },
  {
    "v": 1,
    "seq": 19,
    "event": "status",
    "code": "message",
    "message": "Verifying 3 embedded dashboard assets\u00e2\u20ac\u00a6"
  },
  {
    "v": 1,
    "seq": 20,
    "event": "action",
    "kind": "native_acceptance",
    "url": "http://192.168.1.25/",
    "device_id": "keylight-aabbcc",
    "firmware": "0.2.0-alpha.1",
    "elf_sha256": "0000000000000000000000000000000000000000000000000000000000000000",
    "manifest_sha256": "cd35d136f9376a348961c5d0149376e7cfe7b9817e44d5d0f0fc7308a280888b",
    "controller_version": "0.1.0.0",
    "pairing_open": false,
    "remaining_ms": 64649
  },
  {
    "v": 1,
    "seq": 21,
    "event": "status",
    "code": "message",
    "message": "Native acceptance pending: about 65s remain. This backend only observes confirmation; the acceptance client must verify controls and preserve its credential privately."
  },
  {
    "v": 1,
    "seq": 22,
    "event": "progress",
    "stage_id": "native",
    "scope": "trial",
    "completed": 110.351,
    "total": 175,
    "unit": "seconds",
    "remaining_ms": 64649
  },
  {
    "v": 1,
    "seq": 23,
    "event": "stage",
    "id": "native",
    "index": 7,
    "total": 7,
    "phase": "completed",
    "message": "Exact native image and explicit confirmation observed."
  },
  {
    "v": 1,
    "seq": 24,
    "event": "status",
    "code": "message",
    "message": "Original controller and ESP confirmations observed; no credential was emitted by this backend."
  },
  {
    "v": 1,
    "seq": 25,
    "event": "completed",
    "outcome": "installed"
  }
] as const;
