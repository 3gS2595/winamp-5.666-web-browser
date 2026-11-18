// Browser extensions as the Chrome Web Store hands them out, made ready for WebView2: the
// store's answer to "what is the latest version?", a CRX3 package's header read and its
// signature checked, its zip archive unpacked, and its manifest changed for Exodium. No
// window and no network here; extensions.cpp does the fetching and the installing.
//
// WebView2 only loads an extension from a folder, as Chrome loads one being developed,
// and such an extension's id comes from its manifest's "key" when it has one. The store's
// packages carry no key in their manifests, but every one is signed with it, so the key is
// taken from the signature that checks out and written into the manifest. The extension
// then has the id it has in Chrome, which the programs it talks to (such as Bitwarden's
// desktop app) know it by.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace exodium {

using Bytes = std::vector<std::uint8_t>;
using ByteSpan = std::span<const std::uint8_t>;

[[nodiscard]] inline ByteSpan AsBytes(std::string_view text) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()};
}

// ---- hashes and encodings ----

using Sha256Digest = std::array<std::uint8_t, 32>;
// The SHA-256 of the parts one after another. Nothing only if Windows' hashing failed.
[[nodiscard]] std::optional<Sha256Digest> Sha256(std::initializer_list<ByteSpan> parts);
[[nodiscard]] std::uint32_t Crc32(ByteSpan data) noexcept;
[[nodiscard]] std::string Base64Encode(ByteSpan data);
[[nodiscard]] std::optional<Bytes> Base64Decode(std::string_view text);
// Lower-case hexadecimal, two digits a byte, as the store writes its hashes.
[[nodiscard]] std::optional<Bytes> HexDecode(std::string_view text);

// ---- extension ids ----

// An extension's id: the first half of the SHA-256 of its public key, each four bits a
// letter from a to p. 32 letters.
[[nodiscard]] std::string ExtensionIdFromKey(ByteSpan publicKey);
// The 16 bytes an id stands for; nothing for anything that isn't an id.
[[nodiscard]] std::optional<Bytes> ExtensionIdBytes(std::string_view id);

// ---- the store's update service ----

// Asks for the latest version of an extension, for a browser of this version (such as
// "157.0.4341.42"; the store won't offer a version the browser is too old for).
[[nodiscard]] std::wstring StoreUpdateCheckUrl(std::string_view id, std::wstring_view browserVersion);
struct StoreRelease {
    std::string version;   // such as "2026.10.2"
    std::string codebase;  // where its package is
    std::string sha256Hex; // and the SHA-256 of that, if the answer gave it
};
// What the update service answered for the extension, if it offered a package.
[[nodiscard]] std::optional<StoreRelease> ParseStoreUpdateCheck(std::string_view xml, std::string_view id);
// Dotted versions compared number by number, a missing number counting as 0: -1, 0 or 1.
[[nodiscard]] int CompareVersions(std::string_view a, std::string_view b) noexcept;

// ---- CRX3 packages ----

struct KeyProof {
    Bytes publicKey; // DER SubjectPublicKeyInfo
    Bytes signature;
};
struct CrxPackage {
    std::vector<KeyProof> rsaProofs;
    std::vector<KeyProof> ecdsaProofs;
    Bytes signedHeaderData; // what the proofs sign besides the archive
    Bytes crxId;            // from signedHeaderData: the extension's id, as 16 bytes
    ByteSpan archive;       // the zip, a part of the package's own bytes
};
// Reads a package's header; nothing if it isn't a well-formed CRX3 package.
[[nodiscard]] std::optional<CrxPackage> ReadCrx(ByteSpan bytes);
// What every proof's signature is over: a fixed preamble, the signed header data, then
// the archive, hashed with SHA-256.
[[nodiscard]] std::optional<Sha256Digest> CrxSignedHash(const CrxPackage& crx);
// Whether `signature` is an RSA PKCS #1 v1.5 signature of the SHA-256 `hash` with the key.
[[nodiscard]] bool VerifyRsaSha256(ByteSpan publicKey, const Sha256Digest& hash, ByteSpan signature);
struct VerifiedCrx {
    Bytes publicKey;  // the extension's own, which its id comes from
    ByteSpan archive; // a part of the package's bytes
};
// The package, if it is the extension `id`'s and signed with that extension's key.
[[nodiscard]] std::optional<VerifiedCrx> VerifyCrx(ByteSpan bytes, std::string_view id);

// ---- zip archives ----

struct ZipEntry {
    std::string name; // as the archive has it, '/' between folders; a folder's ends in '/'
    std::uint16_t method = 0;
    std::uint32_t crc = 0;
    std::uint32_t compressedSize = 0;
    std::uint32_t size = 0;
    std::uint32_t localHeaderOffset = 0;
};
// The archive's list of files. Nothing for an archive this can't read: one that is
// damaged, encrypted, or needs ZIP64.
[[nodiscard]] std::optional<std::vector<ZipEntry>> ReadZipDirectory(ByteSpan zip);
// An entry's contents, stored or deflated, checked against its size and CRC.
[[nodiscard]] std::optional<Bytes> ReadZipEntry(ByteSpan zip, const ZipEntry& entry);
// A raw deflate stream (RFC 1951) inflated, which must come to exactly `size` bytes.
[[nodiscard]] std::optional<Bytes> Inflate(ByteSpan deflated, std::size_t size);
// Where an entry goes under the folder it is unpacked into, if it can go there safely:
// nothing for a name that leaves the folder, names a drive or a device, or holds what
// Windows doesn't allow in a file name.
[[nodiscard]] std::optional<std::filesystem::path> SafeArchivePath(std::string_view name);

// ---- manifests ----

// The manifest's "version", if it has one.
[[nodiscard]] std::optional<std::string> ManifestVersion(std::string_view manifest);
// The manifest as WebView2 is to load it: with `keyBase64` as its "key", and without the
// side panel. chrome.sidePanel.open takes WebView2's whole browser process down, every
// tab with it; without the permission the API isn't there, and an extension that checks
// for it (Bitwarden does) opens a window instead. Nothing for a manifest that isn't a
// JSON object.
[[nodiscard]] std::optional<std::string> PrepareManifest(std::string_view manifest, std::string_view keyBase64);

// ---- all of it ----

// Checks a package from the store and unpacks it into `folder`, which mustn't exist yet,
// ready for WebView2: without the store's _metadata folder (WebView2 refuses a folder
// that has one), and with its manifest prepared. Gives the version unpacked, or nothing,
// with nothing left behind, if any of it failed.
[[nodiscard]] std::optional<std::string> UnpackStoreExtension(ByteSpan crx, std::string_view id,
                                                              const std::filesystem::path& folder);

} // namespace exodium
