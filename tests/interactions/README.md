# MQTT and physical-button boundaries

Run `python3 tests/interactions/run_tests.py` with an ASan/UBSan-capable compiler
and the cJSON development library. These tests compile the actual `mqtt.c` and
`button.c` sources, the real button gesture state machine and the real state/JSON
codecs. GPIO, RTOS scheduling and broker transport are deterministic mocks; no
device or broker connection is made.

The MQTT cases cover readiness and controller connectivity, upload/recovery
blocking, reconnect and Home Assistant birth messages, failed enqueues, coalesced
notifications, a full event queue and a controller failure while JSON is being
built. Every publish is required to execute in the MQTT callback, and retained
availability/state enqueue is required to hold the application mutex. A stale
confirmed state cannot be enqueued after a new output revision or readiness loss.

This execution model follows the installed ESP-IDF 5.5.5 MQTT source:
`esp_mqtt_task` holds the client API lock while dispatching callbacks, whereas
`esp_mqtt_dispatch_custom_event` posts to its event queue with zero wait and no
client API lock. Lifecycle callers use that event API after releasing the
application mutex. The callback can then snapshot/cache/enqueue in order without
an application-lock/client-lock inversion. Failed queue admission leaves the
pending flags for the next MQTT event; failed broker enqueue leaves availability
uncached for a later event or lifecycle notification. This is enqueue evidence,
not proof of broker delivery.

The button cases drive actual sampled press/release timelines. Missing scenes
are skipped; Recording Lock and busy refusals preserve the scene cursor; accepted
activations advance it. GPIO and task-allocation failures are checked separately.
The uninterrupted boot hold retains its one-shot client-reset behavior.

Actual-source worker, controller-job and ESP-update suites separately assert
that readiness, fault, upload admission/cancellation, recovery and storage-failure
transitions issue notifications outside the application mutex. The application
startup suite checks that a failed button does not prevent HTTP or MQTT startup.
