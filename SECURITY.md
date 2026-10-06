# Security reports

Please use the repository's private vulnerability reporting feature for a security issue, or contact the maintainer through their GitHub profile before publishing an exploit that affects owners' devices. Include the firmware version, relevant request, expected boundary and a minimal reproducer. Never include Wi-Fi passwords, pairing tokens or a full flash dump in a public issue.

The first development release is designed for a trusted local network. HTTP pairing tokens authorize mutation but do not encrypt traffic. A physical pairing window is required to issue another token after initial setup. MQTT uses broker credentials and supports TLS certificate validation; retained commands are rejected.

Application updates require authorization, a matching SHA-256 digest and the platform's image validation. A client-supplied digest proves transfer integrity, not publisher authenticity. Signed release verification is a separate release requirement and must not be implied by that checksum.

The installed ESP bootloader observed during investigation does not automatically revert a crashing application. The new application's timed trial fallback works only after its startup code runs. Neither an A/B layout nor a recovery web page can repair every boot failure. These limits must remain visible in installation documentation.
