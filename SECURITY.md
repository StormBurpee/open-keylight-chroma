# Security reports

Please use the repository's private vulnerability reporting feature for a security issue, or contact the maintainer through their GitHub profile before publishing an exploit that affects owners' devices. Include the firmware version, relevant request, expected boundary and a minimal reproducer. Never include Wi-Fi passwords, pairing tokens or a full flash dump in a public issue.

Open Keylight is designed for a trusted local network. HTTP pairing tokens authorize mutation but do not encrypt traffic. New tokens require an open pairing window, enabled by the physical button or an existing trusted client. MQTT uses broker credentials and supports TLS certificate validation; retained commands are rejected.

Application updates require authorization, a matching SHA-256 digest and the platform's image validation. Download releases from this repository's GitHub Releases page. Published checksums detect changed files; firmware publisher-signature verification is not implemented, and a matching checksum does not authenticate the publisher.

The installed ESP bootloader observed during investigation does not automatically revert a crashing application. The new application's timed trial fallback works only after its startup code runs. Neither an A/B layout nor a recovery web page can repair every boot failure. These limits must remain visible in installation documentation.
