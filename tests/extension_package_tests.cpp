#include "test_framework.h"

#include "extension_package.h"
#include "extension_package_vectors.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <cstdlib>
#include <fstream>
#include <iterator>
#include <sstream>

using namespace exodium;

namespace {

[[nodiscard]] std::string AsText(const Bytes& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

[[nodiscard]] std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// A folder of the test's own under %TEMP%, gone again at the end.
struct TempFolder {
    std::filesystem::path path;
    TempFolder() {
        path = std::filesystem::temp_directory_path() / std::format("exodium-test-{}-{}", ::GetCurrentProcessId(), next++);
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    ~TempFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
    static inline int next = 0;
};

// ---- a package made and signed here, the way the store makes them ----

void AppendVarint(Bytes& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void AppendField(Bytes& out, std::uint64_t field, ByteSpan bytes) {
    AppendVarint(out, field << 3 | 2);
    AppendVarint(out, bytes.size());
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void Append32(Bytes& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

class TestKey {
public:
    TestKey() {
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        ::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0);
        ::BCryptGenerateKeyPair(algorithm, &key_, 2048, 0);
        ::BCryptFinalizeKeyPair(key_, 0);
        ::BCryptCloseAlgorithmProvider(algorithm, 0);

        DWORD size = 0;
        char rsa[] = szOID_RSA_RSA;
        ::CryptExportPublicKeyInfoFromBCryptKeyHandle(key_, X509_ASN_ENCODING, rsa, 0, nullptr, nullptr, &size);
        std::vector<std::uint8_t> info(size);
        auto* const publicKeyInfo = reinterpret_cast<CERT_PUBLIC_KEY_INFO*>(info.data());
        ::CryptExportPublicKeyInfoFromBCryptKeyHandle(key_, X509_ASN_ENCODING, rsa, 0, nullptr, publicKeyInfo, &size);
        BYTE* der = nullptr;
        DWORD derSize = 0;
        ::CryptEncodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, publicKeyInfo, CRYPT_ENCODE_ALLOC_FLAG, nullptr,
                              &der, &derSize);
        spki_.assign(der, der + derSize);
        ::LocalFree(der);
    }
    ~TestKey() { ::BCryptDestroyKey(key_); }
    TestKey(const TestKey&) = delete;
    TestKey& operator=(const TestKey&) = delete;

    [[nodiscard]] const Bytes& spki() const noexcept { return spki_; }
    [[nodiscard]] std::string id() const { return ExtensionIdFromKey(spki_); }

    [[nodiscard]] Bytes Sign(const Sha256Digest& hash) const {
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ULONG size = 0;
        ::BCryptSignHash(key_, &padding, const_cast<PUCHAR>(hash.data()), 32, nullptr, 0, &size, BCRYPT_PAD_PKCS1);
        Bytes signature(size);
        ::BCryptSignHash(key_, &padding, const_cast<PUCHAR>(hash.data()), 32, signature.data(), size, &size,
                         BCRYPT_PAD_PKCS1);
        return signature;
    }

private:
    BCRYPT_KEY_HANDLE key_ = nullptr;
    Bytes spki_;
};

// A CRX3 package of `archive` for the extension whose id comes from `idKey`, signed by
// `signer`.
[[nodiscard]] Bytes MakeCrx(const TestKey& idKey, const TestKey& signer, ByteSpan archive) {
    Bytes signedData;
    AppendField(signedData, 1, *ExtensionIdBytes(idKey.id()));

    CrxPackage parts;
    parts.signedHeaderData = signedData;
    parts.archive = archive;
    const Bytes signature = signer.Sign(*CrxSignedHash(parts));

    Bytes proof;
    AppendField(proof, 1, signer.spki());
    AppendField(proof, 2, signature);
    Bytes header;
    AppendField(header, 2, proof);
    AppendField(header, 10000, signedData);

    Bytes crx = {'C', 'r', '2', '4'};
    Append32(crx, 3);
    Append32(crx, static_cast<std::uint32_t>(header.size()));
    crx.insert(crx.end(), header.begin(), header.end());
    crx.insert(crx.end(), archive.begin(), archive.end());
    return crx;
}

// Bitwarden's public key, as its manifest in Chrome has it, and the id Chrome gives it.
constexpr std::string_view kBitwardenKey =
    "MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEAmqKbvreshyXRuN2gikeR1idqR6KL0Di89JZcMyD4bjJRZVmQO7aznSGSALIHzSAUGYocUY"
    "BNDOP5QAhImxXyQ1qG8+goXs93v9GzrNJETdVuCEhqBggC4/DFabryJZDiKvZ2Jl0DM7MsWdoybZPwrj70V3aJ/nVNOMkf868scNTMliwitCqqjT5baT"
    "ANsG0DkZWQExD4lSXzSZHH9MEO8q0iZ7RRlNuGRBAkZgNV8FwZRsPKm/rwQ9dy3VpgLcmLp5GiMt+kAEncqKAkuRYnhVXXBsKqIyYTMjHSLkLnpfFySy"
    "OPLBdS617i/PGNiP/MT6Xy6z//v5NozUgaAZ4gJQIDAQAB";
constexpr std::string_view kBitwardenId = "nngceckbapebfimnlniiiahkandclblb";

} // namespace

TEST_CASE(HashesAndEncodesAsTheStandardsSay) {
    EXPECT_EQ(Crc32(AsBytes("123456789")), 0xCBF43926u);
    EXPECT_EQ(Crc32({}), 0u);
    const auto empty = Sha256({});
    EXPECT(empty && (*empty)[0] == 0xE3 && (*empty)[31] == 0x55); // e3b0c442...7852b855
    // In pieces or whole, the same.
    EXPECT(Sha256({AsBytes("ab"), AsBytes("c")}) == Sha256({AsBytes("abc")}));

    EXPECT_EQ(Base64Encode(AsBytes("")), std::string(""));
    EXPECT_EQ(Base64Encode(AsBytes("f")), std::string("Zg=="));
    EXPECT_EQ(Base64Encode(AsBytes("fo")), std::string("Zm8="));
    EXPECT_EQ(Base64Encode(AsBytes("foobar")), std::string("Zm9vYmFy"));
    EXPECT_EQ(AsText(*Base64Decode("Zm9vYg==")), std::string("foob"));
    EXPECT_EQ(AsText(*Base64Decode("Zm9vYmE=")), std::string("fooba"));
    EXPECT(!Base64Decode("Zm9"));
    EXPECT(!Base64Decode("Z=9v"));
    EXPECT(!Base64Decode("Zm9v!A=="));
    EXPECT(!Base64Decode("Zg==Zg=="));

    EXPECT(HexDecode("00ff10Ab") == (Bytes{0x00, 0xFF, 0x10, 0xAB}));
    EXPECT(!HexDecode("abc"));
    EXPECT(!HexDecode("zz"));
}

TEST_CASE(GivesAnExtensionTheIdChromeGivesIt) {
    const auto key = Base64Decode(kBitwardenKey);
    EXPECT(key.has_value());
    EXPECT_EQ(ExtensionIdFromKey(*key), std::string(kBitwardenId));
    const auto bytes = ExtensionIdBytes(kBitwardenId);
    EXPECT(bytes && bytes->size() == 16 && (*bytes)[0] == 0xDD); // "nn": 13, 13
    EXPECT(!ExtensionIdBytes("nngceckbapebfimnlniiiahkandclbl"));   // one short
    EXPECT(!ExtensionIdBytes("nngceckbapebfimnlniiiahkandclblz"));  // z is no id's
    EXPECT(!ExtensionIdBytes("NNGCECKBAPEBFIMNLNIIIAHKANDCLBLB"));
}

TEST_CASE(AsksTheStoreForTheLatestVersion) {
    EXPECT_EQ(StoreUpdateCheckUrl(kBitwardenId, L"157.0.4341.42"),
              std::wstring(L"https://clients2.google.com/service/update2/crx?response=updatecheck&acceptformat=crx3"
                           L"&prodversion=157.0.4341.42&x=id%3Dnngceckbapebfimnlniiiahkandclblb%26v%3D0.0.0%26uc"));
    // Nothing but a version goes into the address.
    EXPECT(StoreUpdateCheckUrl(kBitwardenId, L"157.0&x=y").contains(L"prodversion=157.0&x=id"));
}

TEST_CASE(ReadsTheStoresAnswer) {
    // As the store answered on 2026-11-07, its address shortened.
    constexpr std::string_view answer =
        R"(<?xml version="1.0" encoding="UTF-8"?><gupdate xmlns="http://www.google.com/update2/response" protocol="2.0" )"
        R"(server="prod"><daystart elapsed_days="7250" elapsed_seconds="77645"/><app appid="nngceckbapebfimnlniiiahkandclblb" )"
        R"(cohort="1::" cohortname="" status="ok"><updatecheck _esbAllowlist="true" )"
        R"(codebase="https://clients2.googleusercontent.com/crx/blobs/ARGa/NNGC_2026_10_2_0.crx?a=1&amp;b=2" )"
        R"(fp="1.1c99" hash_sha256="1c995179f75863104c8a6e389e1050568a82c0d2c07d6acf6b62703d249430c9" protected="0" )"
        R"(size="26044856" status="ok" version="2026.10.2"/></app></gupdate>)";
    const auto release = ParseStoreUpdateCheck(answer, kBitwardenId);
    EXPECT(release.has_value());
    if (!release) return;
    EXPECT_EQ(release->version, std::string("2026.10.2"));
    EXPECT_EQ(release->codebase, std::string("https://clients2.googleusercontent.com/crx/blobs/ARGa/NNGC_2026_10_2_0.crx?a=1&b=2"));
    EXPECT_EQ(release->sha256Hex, std::string("1c995179f75863104c8a6e389e1050568a82c0d2c07d6acf6b62703d249430c9"));

    // Another extension's answer, no update, and a package anywhere but https.
    EXPECT(!ParseStoreUpdateCheck(answer, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    EXPECT(!ParseStoreUpdateCheck(R"(<gupdate><app appid="nngceckbapebfimnlniiiahkandclblb" status="ok">)"
                                  R"(<updatecheck status="noupdate"/></app></gupdate>)",
                                  kBitwardenId));
    EXPECT(!ParseStoreUpdateCheck(R"(<gupdate><app appid="nngceckbapebfimnlniiiahkandclblb" status="ok"><updatecheck )"
                                  R"(codebase="http://example.com/x.crx" status="ok" version="1.0"/></app></gupdate>)",
                                  kBitwardenId));
    EXPECT(!ParseStoreUpdateCheck("not xml at all", kBitwardenId));
}

TEST_CASE(ComparesVersionsNumberByNumber) {
    EXPECT_EQ(CompareVersions("2026.10.2", "2026.10.2"), 0);
    EXPECT_EQ(CompareVersions("2026.10.2", "2026.9.30"), 1);
    EXPECT_EQ(CompareVersions("2026.10", "2026.10.0.0"), 0);
    EXPECT_EQ(CompareVersions("1.2", "1.10"), -1);
    EXPECT_EQ(CompareVersions("", "0.1"), -1);
}

TEST_CASE(InflatesEveryKindOfBlock) {
    const auto fixed = Inflate(vectors::kFixedDeflated, vectors::kFixedText.size());
    EXPECT(fixed && AsText(*fixed) == vectors::kFixedText);
    const auto stored = Inflate(vectors::kStoredDeflated, vectors::kStoredText.size());
    EXPECT(stored && AsText(*stored) == vectors::kStoredText);
    const auto dynamic = Inflate(vectors::kDynamicDeflated, 1756);
    EXPECT(dynamic.has_value());
    if (dynamic) {
        EXPECT_EQ(Crc32(*dynamic), 0x4E5A2DB1u);
        EXPECT_EQ(static_cast<int>((*dynamic)[1500 + 200]), 200);
    }
}

TEST_CASE(RefusesToInflateWhatDoesntAddUp) {
    // The wrong size, either way.
    EXPECT(!Inflate(vectors::kFixedDeflated, vectors::kFixedText.size() - 1));
    EXPECT(!Inflate(vectors::kFixedDeflated, vectors::kFixedText.size() + 1));
    // Cut short.
    EXPECT(!Inflate(ByteSpan(vectors::kDynamicDeflated).first(std::size(vectors::kDynamicDeflated) / 2), 1756));
    // A block of the reserved type, 3.
    const std::uint8_t reserved[] = {0x07, 0x00};
    EXPECT(!Inflate(reserved, 0));
    // A stored block whose length check is wrong.
    const std::uint8_t stored[] = {0x01, 0x02, 0x00, 0x00, 0x00, 'a', 'b'};
    EXPECT(!Inflate(stored, 2));
    // A copy from before the start.
    Bytes damaged(std::begin(vectors::kDynamicDeflated), std::end(vectors::kDynamicDeflated));
    for (std::size_t i = 20; i < damaged.size(); i += 7) damaged[i] ^= 0x5A;
    const auto garbled = Inflate(damaged, 1756);
    EXPECT(!garbled || Crc32(*garbled) != 0x4E5A2DB1u);
}

TEST_CASE(ReadsAZipArchive) {
    const auto entries = ReadZipDirectory(vectors::kTestZip);
    EXPECT(entries && entries->size() == 4);
    if (!entries || entries->size() != 4) return;
    EXPECT_EQ((*entries)[0].name, std::string("manifest.json"));
    EXPECT_EQ((*entries)[0].method, std::uint16_t{8});
    EXPECT_EQ((*entries)[1].name, std::string("images/"));
    const auto manifest = ReadZipEntry(vectors::kTestZip, (*entries)[0]);
    EXPECT(manifest && AsText(*manifest).starts_with(R"({ "name": "Test")"));
    const auto icon = ReadZipEntry(vectors::kTestZip, (*entries)[2]);
    EXPECT(icon && AsText(*icon) == "an icon");

    // A CRC that doesn't match the contents.
    ZipEntry wrong = (*entries)[2];
    wrong.crc ^= 1;
    EXPECT(!ReadZipEntry(vectors::kTestZip, wrong));
    // Not an archive, or one cut short.
    EXPECT(!ReadZipDirectory(AsBytes("PK but nothing else")));
    EXPECT(!ReadZipDirectory(ByteSpan(vectors::kTestZip).first(std::size(vectors::kTestZip) - 10)));
}

TEST_CASE(KeepsUnpackedFilesInsideTheirFolder) {
    EXPECT(SafeArchivePath("manifest.json") == std::filesystem::path(L"manifest.json"));
    EXPECT(SafeArchivePath("popup/fonts/inter.woff2") == std::filesystem::path(L"popup") / L"fonts" / L"inter.woff2");
    EXPECT(SafeArchivePath("_locales/en_GB/messages.json").has_value());
    EXPECT(SafeArchivePath("a/.hidden").has_value());
    for (const std::string_view bad :
         {"", "/etc/passwd", "../escaped", "a/../../b", "a/./b", "a//b", "a/", "C:/Windows/x", "C:x", "a\\..\\b",
          "con", "CON.txt", "a/nul.json", "lpt1", "com9.js", "name.", "name ", "...", "a/ /b", "what?", "tab\there"}) {
        if (SafeArchivePath(bad)) test::ReportFailure(std::format("took \"{}\"", bad), std::source_location::current());
    }
    EXPECT(SafeArchivePath("console.js").has_value()); // starts like a device, isn't one
    EXPECT(SafeArchivePath("com10.js").has_value());
}

TEST_CASE(ReadsAManifestsVersion) {
    EXPECT(ManifestVersion(R"({"name": "x", "version": "2026.10.2"})") == "2026.10.2");
    EXPECT(ManifestVersion("\xEF\xBB\xBF{ \"version\" : \"1.0\" }") == "1.0");
    EXPECT(!ManifestVersion(R"({"name": "x"})"));
    EXPECT(!ManifestVersion(R"({"version": 3})"));
    EXPECT(!ManifestVersion(R"({"version": "1.0")"));
    EXPECT(!ManifestVersion(R"({"version": "1.0"} trailing)"));
    // Only the manifest's own, not one deeper in.
    EXPECT(!ManifestVersion(R"({"browser_specific_settings": {"version": "9"}})"));
}

TEST_CASE(PreparesAManifestForWebView2) {
    constexpr std::string_view manifest = R"({
   "action": { "default_popup": "popup/index.html" },
   "key": "old",
   "name": "Bitwarden \"Password\" Manager",
   "permissions": [ "activeTab", "sidePanel", "storage" ],
   "side_panel": { "default_path": "sidepanel-disabled.html" },
   "optional_permissions": [ "sidePanel" ],
   "version": "2026.10.2"
})";
    const auto prepared = PrepareManifest(manifest, "QUJD+/0=");
    EXPECT(prepared.has_value());
    if (!prepared) return;
    // The side panel's permission and page are gone, and the key is the one given.
    EXPECT(!prepared->contains(R"("side_panel")"));
    EXPECT(prepared->contains(R"("permissions": ["activeTab", "storage"])"));
    EXPECT(prepared->contains(R"("key": "QUJD+/0=")"));
    EXPECT(!prepared->contains(R"("old")"));
    // The rest as it was, and still a manifest.
    EXPECT(prepared->contains(R"("name": "Bitwarden \"Password\" Manager")"));
    EXPECT(prepared->contains(R"("optional_permissions": [ "sidePanel" ])"));
    EXPECT(prepared->contains(R"("action": { "default_popup": "popup/index.html" })"));
    EXPECT(ManifestVersion(*prepared) == "2026.10.2");

    EXPECT(!PrepareManifest("[1, 2]", "QUJD"));
    EXPECT(!PrepareManifest(R"({"a": tru})", "QUJD"));
    EXPECT(!PrepareManifest(R"({"a": 1})", R"(QU"JD)")); // nothing but Base64 goes into it
    EXPECT(PrepareManifest("{}", "QUJD") == "{\n   \"key\": \"QUJD\"\n}\n");
}

TEST_CASE(ChecksAPackagesSignature) {
    const TestKey key;
    const TestKey other;
    const Bytes crx = MakeCrx(key, key, vectors::kTestZip);

    const auto read = ReadCrx(crx);
    EXPECT(read && read->rsaProofs.size() == 1 && read->archive.size() == std::size(vectors::kTestZip));
    const auto verified = VerifyCrx(crx, key.id());
    EXPECT(verified && verified->publicKey == key.spki() && verified->archive.size() == std::size(vectors::kTestZip));

    // Another extension's id, an archive changed after signing, a package signed by a key
    // other than the one its id comes from, and things that aren't packages at all.
    EXPECT(!VerifyCrx(crx, other.id()));
    Bytes changed = crx;
    changed[changed.size() - 30] ^= 1;
    EXPECT(!VerifyCrx(changed, key.id()));
    EXPECT(!VerifyCrx(MakeCrx(key, other, vectors::kTestZip), key.id()));
    EXPECT(!VerifyCrx(vectors::kTestZip, key.id()));
    Bytes cut(crx.begin(), crx.begin() + 40);
    EXPECT(!ReadCrx(cut));
}

TEST_CASE(UnpacksAPackageReadyForWebView2) {
    const TestKey key;
    const TempFolder temp;
    const auto version = UnpackStoreExtension(MakeCrx(key, key, vectors::kTestZip), key.id(), temp.path);
    EXPECT(version == "1.2.3");
    EXPECT_EQ(ReadText(temp.path / L"images" / L"icon.txt"), std::string("an icon"));
    EXPECT(!std::filesystem::exists(temp.path / L"_metadata"));
    const std::string manifest = ReadText(temp.path / L"manifest.json");
    EXPECT(manifest.contains("\"key\": \"" + Base64Encode(key.spki()) + "\""));
    EXPECT(manifest.contains(R"("permissions": ["storage"])"));

    // Not into a folder that is there already.
    EXPECT(!UnpackStoreExtension(MakeCrx(key, key, vectors::kTestZip), key.id(), temp.path));
}

TEST_CASE(LeavesNothingBehindWhenUnpackingFails) {
    const TestKey key;
    const TempFolder temp;
    // A file that would land outside the folder, after one that went in.
    EXPECT(!UnpackStoreExtension(MakeCrx(key, key, vectors::kEscapingZip), key.id(), temp.path));
    EXPECT(!std::filesystem::exists(temp.path));
    EXPECT(!std::filesystem::exists(temp.path.parent_path() / L"escaped.txt"));
    // Signed by the wrong key.
    const TestKey other;
    EXPECT(!UnpackStoreExtension(MakeCrx(key, other, vectors::kTestZip), key.id(), temp.path));
    EXPECT(!std::filesystem::exists(temp.path));
}

// Bitwarden's real package, when EXODIUM_TEST_CRX names a copy of it: too big to keep here.
TEST_CASE(UnpacksBitwardensRealPackage) {
    char* named = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&named, &length, "EXODIUM_TEST_CRX") != 0 || !named) return;
    const std::filesystem::path crxPath(named);
    std::free(named);
    std::ifstream in(crxPath, std::ios::binary);
    const Bytes crx((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const TempFolder temp;
    const auto version = UnpackStoreExtension(crx, kBitwardenId, temp.path);
    EXPECT(version.has_value());
    const std::string manifest = ReadText(temp.path / L"manifest.json");
    EXPECT(manifest.contains(std::string("\"key\": \"") + std::string(kBitwardenKey) + "\""));
    EXPECT(!manifest.contains(R"("sidePanel")"));
    EXPECT(std::filesystem::exists(temp.path / L"popup" / L"index.html"));
    EXPECT(!std::filesystem::exists(temp.path / L"_metadata"));
    std::puts(std::format("      unpacked Bitwarden {}", version.value_or("(nothing)")).c_str());
}
