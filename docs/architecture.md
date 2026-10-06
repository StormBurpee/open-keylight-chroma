# Architecture

Open Keylight Chroma replaces both applications in the Razer Key Light Chroma: ESP32 networking/control and NXP LED control. Their installed bootloaders remain the recovery and installation boundary. No vendor application executable is linked into this project or redistributed here.

During bring-up, the independently written ESP application can also communicate with the existing NXP application. This compatibility stage is not the final replacement claim. The independent NXP application must establish the exact MCU, output-channel mapping, timing and electrical constraints before it can drive a panel.

## Boundaries

`firmware/components/keylight_core` is portable C: state validation, arbitration, transition sampling and button gestures. It knows nothing about Wi-Fi, JSON or FreeRTOS.

`firmware/components/keylight_nxp` is an independently written protocol codec and transport driver. A single worker owns every complete SPI exchange. A finite command queue handles intent; animation uses the most recent frame. A request timeout is an uncertain outcome, never an instruction to replay a mutation blindly.

`firmware/main` adapts those modules to ESP-IDF. HTTP and MQTT commands share validation and arbitration. They never call SPI directly. NVS writes are explicit or delayed configuration commits; animation frames never touch flash.

`dashboard` is a React, TypeScript and shadcn/ui application. Its compressed assets are embedded in the application image, without a CDN. The firmware API is also usable without the dashboard.

## State and authority

An accepted command advances the desired-state revision. The worker separately reports what was acknowledged and which values were confirmed through controller getters. Neither is a measurement of emitted light. The API must preserve this distinction on faults and during transitions.

Manual Off cancels all animation immediately. New commands replace older transitions. A recording lock blocks output changes except Off until explicitly unlocked. Actor attribution cannot bypass it. No client obtains a permanent controller lock.

The first application starts by reading NXP state, leaving output unchanged. Wi-Fi failures preserve the light state and saved credentials. Recovery networking is independent of controller health.

## Persistent data and updates

Keep the installed bootloader and partition table. Use application-only OTA within the existing 1,572,864-byte slots. Do not burn security eFuses, repartition, or erase shared NVS. Import known vendor Wi-Fi strings locally into our own versioned namespace without returning them over the API.

Authenticated updates validate the image, target, length and digest before selecting the next boot partition. A/B slots alone do not establish automatic rollback: that depends on the installed bootloader. The public installer must say which recovery paths were actually exercised.

## Initial scope

Power, white temperature, RGB, brightness, smooth transitions, bounded built-in effects, local scenes, status and change history; authenticated HTTP API; MQTT discovery for Home Assistant; a small Stream Deck plugin; independently serviced physical-button actions; and application OTA.

Audio, optical calibration, arbitrary scripts and synchronized multi-light rendering are later work. Hardware current and thermal limits remain intact. NXP application replacement is in scope; changing power electronics or bootloaders is not.
