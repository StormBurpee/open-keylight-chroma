# Home Assistant through MQTT

Connect Open Keylight to your MQTT broker to add a light entity in Home Assistant. The firmware publishes discovery, accepts lighting commands and reports controller state. A complete Home Assistant → broker → physical light test has not yet been recorded; the protocol and failure paths have automated tests.

## Connect the broker

1. Configure Home Assistant's [MQTT integration](https://www.home-assistant.io/integrations/mqtt/) with your broker. Keep its discovery prefix `homeassistant` and birth topic `homeassistant/status`; those values are fixed in this firmware.
2. Pair the light's dashboard during its physical pairing window. In **System**, enable MQTT, enter the broker URI and credentials, then choose **Save MQTT settings**. The light saves the changes and restarts.
3. After reconnecting, inspect the light's MQTT connection status. It publishes one discoverable light using its stable `keylight-xxxxxx` device ID. The entity ID Home Assistant assigns can differ from that device ID.

Broker URIs accept `mqtt://host[:port]` or `mqtts://host[:port]`, without an embedded username, password, path or query. TLS uses the ESP certificate bundle; installing a private broker CA through this API is not implemented. Broker credentials are separate from the dashboard's paired HTTP token. Use broker permissions to restrict who can publish light commands.

The equivalent authenticated HTTP settings patch is:

```json
{
  "mqtt": {
    "enabled": true,
    "uri": "mqtts://broker.example:8883",
    "username": "keylight",
    "password": "replace-locally"
  }
}
```

Send it to `PATCH /api/v1/settings` with `Content-Type: application/json` and a paired bearer token. These are placeholders, not working credentials. The response omits passwords; omitted configuration fields keep their previous values. See the [HTTP contract](api-contract.md) and [OpenAPI document](openapi.json).

## Topics and payloads

Read `id` from `GET /api/v1/device` and substitute it for `<id>` below.

| Topic | Direction | Meaning |
| --- | --- | --- |
| `homeassistant/light/<id>/config` | Light → broker | Retained JSON discovery, QoS 1 |
| `openkeylight/<id>/set` | Client → light | Non-retained JSON lighting command |
| `openkeylight/<id>/state` | Light → broker | Retained getter-supported state, QoS 1 |
| `openkeylight/<id>/availability` | Light → broker | Retained controller availability, with an `offline` last will |
| `homeassistant/status` | Home Assistant → light | `online` prompts discovery and available state to be republished |

Discovery uses Home Assistant's [JSON MQTT light schema](https://www.home-assistant.io/integrations/light.mqtt/#json-schema), a brightness scale of 100, RGB plus color-temperature modes, and Kelvin temperature values. No manually maintained YAML light entity is needed for this discovery path.

Examples to publish to the `set` topic, with **retain disabled**:

```json
{"state":"ON","brightness":30,"color_temp":4500,"effect":"none"}
```

```json
{"state":"ON","brightness":40,"color":{"r":255,"g":0,"b":32},"effect":"none","transition":1.0}
```

```json
{"state":"ON","brightness":25,"effect":"aurora","transition":0.6}
```

```json
{"state":"OFF"}
```

| Field | Accepted values |
| --- | --- |
| `state` | `ON` or `OFF` |
| `brightness` | Integer 0–100 |
| `color` | Exactly integer `r`, `g`, `b`, each 0–255; selects color mode |
| `color_temp` | Integer 3000–7000, in Kelvin; selects white mode |
| `effect` | `none`, `aurora`, `breathe`; a non-`none` effect selects color mode |
| `transition` | Finite number of seconds, 0–10; rounded to milliseconds |

Other fields, duplicate keys, mixed color-and-temperature commands, NUL and bodies over 512 bytes are rejected. MQTT events split across multiple delivery fragments are ignored by this implementation. Do not send `color_mode` as a command; it is an output-state field. A color without `effect:"none"` retains an existing active effect. Omitted power retains the current power setting.

Retained command deliveries are ignored to prevent replay after reconnection. Broker permission to publish to `set` is sufficient to control the light: MQTT does not carry the HTTP bearer token. Recording Lock still applies, and it cannot be changed over MQTT. While locked, send exactly `{"state":"OFF"}` to turn off; combined output fields such as brightness or transition do not get the Off exception.

## What a state message proves

State publication requires a connected controller, a valid report, an idle operation and a report belonging to the current output revision. The message includes only the fields supported by that controller report. It does not publish the requested RGB as though it were a measured result.

During colour transitions and ongoing effects, the controller has no RGB framebuffer getter. New state is withheld until getter-supported state is available again; the retained MQTT message can therefore describe the last confirmed static state.

`online` requires a ready, connected lighting controller, no firmware update in progress and no unresolved controller-update journal. A controller fault or update makes the entity unavailable; losing the broker connection also publishes the `offline` last will. Availability does not prove a particular colour or optical output. Use `GET /api/v1/device`, `/state` and `/history` for controller faults and command rejection details.

Colour transitions are supported; white changes and Off are immediate. Effect controls are accepted, but running RGB and active effect state are not confirmed through MQTT. Recording Lock, pairing, scenes and OTA use the HTTP API; no separate MQTT entities for those features are implemented.

## Check an installation

- Confirm discovery appears on the expected topic and uses the device's actual ID. A different discovery prefix is not supported by this firmware.
- Send a modest static white or RGB command. Check `operation.status`, reported fields and the physical light before relying on an automation.
- If Home Assistant's display stays at an earlier color during an effect, inspect the HTTP state before assuming a disconnected device. Do not enable optimistic reporting to disguise missing controller evidence.
- Verify broker credentials, TLS trust and topic permissions if discovery is absent. A successful HTTP pairing does not authenticate the MQTT connection.
- Retained discovery and state remain at the broker after a device is removed or MQTT is disabled. Removing those retained records is a deliberate broker administration task; disabling MQTT does not erase them automatically.

The source of truth for this mapping is [`firmware/main/mqtt.c`](../firmware/main/mqtt.c). The discovery and Kelvin conventions were checked against Home Assistant's documentation; broker interoperability and optical behavior require a recorded live integration test.
