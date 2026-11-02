#include "ota_manifest.h"

#include <stdio.h>
#include <string.h>

namespace sp {

namespace {

bool parseVersion(const char* s, unsigned v[3]) {
    if (!s) return false;
    for (int i = 0; i < 3; ++i) {
        if (*s < '0' || *s > '9') return false;
        unsigned n = 0;
        int digits = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + static_cast<unsigned>(*s++ - '0');
            if (++digits > 3) return false;
        }
        v[i] = n;
        if (i < 2 && *s++ != '.') return false;
    }
    return *s == 0;
}

int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;  // the manifest uses lowercase only
}

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

}  // namespace

bool compareVersions(const char* a, const char* b, int& result) {
    unsigned va[3], vb[3];
    if (!parseVersion(a, va) || !parseVersion(b, vb)) return false;
    result = 0;
    for (int i = 0; i < 3 && result == 0; ++i) result = va[i] < vb[i] ? -1 : va[i] > vb[i] ? 1 : 0;
    return true;
}

size_t base64Decode(const char* in, uint8_t* out, size_t cap) {
    size_t len = strlen(in);
    if (len == 0 || len % 4) return 0;
    size_t n = 0;
    for (size_t i = 0; i < len; i += 4) {
        int v[4];
        for (int k = 0; k < 4; ++k) {
            char c = in[i + k];
            bool pad = c == '=' && i + 4 == len && (k == 3 || (k == 2 && in[i + 3] == '='));
            v[k] = pad ? -2 : b64Value(c);
            if (v[k] == -1) return 0;
        }
        if (v[0] < 0 || v[1] < 0) return 0;
        uint32_t triple = (static_cast<uint32_t>(v[0]) << 18) | (static_cast<uint32_t>(v[1]) << 12) |
                          (static_cast<uint32_t>(v[2] < 0 ? 0 : v[2]) << 6) | static_cast<uint32_t>(v[3] < 0 ? 0 : v[3]);
        int bytes = v[2] < 0 ? 1 : v[3] < 0 ? 2 : 3;
        if (n + bytes > cap) return 0;
        for (int k = 0; k < bytes; ++k) out[n++] = static_cast<uint8_t>(triple >> (16 - 8 * k));
    }
    return n;
}

bool parseOtaManifest(JsonDocument& doc, OtaManifest& m) {
    m = OtaManifest{};
    const char* version = doc["version"];
    const char* sha = doc["sha256"];
    const char* sig = doc["signature"];
    unsigned v[3];
    if (!version || strlen(version) >= sizeof m.version || !parseVersion(version, v)) return false;
    if (!doc["size"].is<uint32_t>() || !sha || strlen(sha) != 64 || !sig || strlen(sig) > 200) return false;
    m.size = doc["size"];
    if (m.size == 0 || m.size > kOtaMaxImage) return false;
    strcpy(m.version, version);
    for (int i = 0; i < 32; ++i) {
        int hi = hexNibble(sha[2 * i]), lo = hexNibble(sha[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        m.sha256[i] = static_cast<uint8_t>(hi << 4 | lo);
    }
    m.signatureLen = base64Decode(sig, m.signature, sizeof m.signature);
    return m.signatureLen >= 8;
}

size_t otaSignedMessage(const OtaManifest& m, char* out, size_t cap) {
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + 2 * i, 3, "%02x", m.sha256[i]);
    int n = snprintf(out, cap, "smartpager-ota|%s|%u|%s", m.version, static_cast<unsigned>(m.size), hex);
    return n > 0 && static_cast<size_t>(n) < cap ? static_cast<size_t>(n) : 0;
}

}  // namespace sp
