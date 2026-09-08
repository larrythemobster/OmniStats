#pragma once

// Single source of truth for OmniStats release certificate pins.
// The updater verifies downloaded MSIs against the SHA-256 fingerprint of the DER-encoded certificate.
namespace SigningConfig {

    // Current pinned certificate SHA-256 (DER-encoded X.509 certificate fingerprint)
    inline constexpr const char* CURRENT_CERT_SHA256 =
        "bad9e3ec198128629f1a767cc3fa17beb29d471478eab13312869c7dc5a219d4";

    // Optional next pinned certificate SHA-256 for rotation and CA migration.
    // Existing updaters will accept either the current or next certificate.
    inline constexpr const char* NEXT_CERT_SHA256 = "";

} // namespace SigningConfig
