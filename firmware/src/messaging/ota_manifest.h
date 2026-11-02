// Signed OTA manifest handling (pure C++, unit-tested on the host).
//
// The server only distributes firmware. Authenticity comes from an ECDSA P-256 signature made
// offline with the owner's key (tools/ota.py) over the string
//     "smartpager-ota|<version>|<size>|<sha256 hex>"
// and verified on the device with the public key compiled into the firmware. Binding the
// version into the signed string prevents replaying an old signed image as "newer".
#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

namespace sp {

struct OtaManifest {
    char version[16] = {};
    uint32_t size = 0;
    uint8_t sha256[32] = {};
    uint8_t signature[80] = {};  // DER-encoded ECDSA P-256 signature (<= 72 bytes)
    size_t signatureLen = 0;
};

constexpr uint32_t kOtaMaxImage = 6553600;  // app partition size in default_16MB.csv

bool parseOtaManifest(JsonDocument& doc, OtaManifest& out);

// <0, 0, >0 like strcmp for "major.minor.patch". Returns false if either is malformed.
bool compareVersions(const char* a, const char* b, int& result);

// Writes the exact string covered by the signature; returns its length (0 if it doesn't fit).
size_t otaSignedMessage(const OtaManifest& m, char* out, size_t cap);

// Standard base64 (RFC 4648) decoder; returns decoded length, or 0 on invalid input/overflow.
size_t base64Decode(const char* in, uint8_t* out, size_t cap);

}  // namespace sp
