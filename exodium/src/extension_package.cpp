#include "extension_package.h"

#include "text_util.h"

#include <windows.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <fstream>
#include <system_error>

namespace exodium {

namespace {

constexpr std::size_t kNone = std::string_view::npos;

// What an unpacked extension may come to, well past any real one (Bitwarden is some 200
// files and 95 MB), so that a damaged archive can't fill the disk.
constexpr std::size_t kMostFiles = 20000;
constexpr std::uint64_t kMostBytes = 1ull << 30;

[[nodiscard]] std::uint16_t Read16(ByteSpan bytes, std::size_t at) noexcept {
    return static_cast<std::uint16_t>(bytes[at] | bytes[at + 1] << 8);
}
[[nodiscard]] std::uint32_t Read32(ByteSpan bytes, std::size_t at) noexcept {
    return static_cast<std::uint32_t>(bytes[at]) | static_cast<std::uint32_t>(bytes[at + 1]) << 8 |
           static_cast<std::uint32_t>(bytes[at + 2]) << 16 | static_cast<std::uint32_t>(bytes[at + 3]) << 24;
}

// ---- protocol buffers, as CRX3 headers are written ----

class ProtoReader {
public:
    explicit ProtoReader(ByteSpan data) noexcept : data_(data) {}

    // The next field: its number, and for a length-delimited one its bytes. False at the
    // end, and on damage, which failed() then says.
    bool Next(std::uint64_t& field, ByteSpan& bytes) noexcept {
        if (failed_ || at_ >= data_.size()) return false;
        std::uint64_t tag = 0;
        if (!Varint(tag)) return Fail();
        field = tag >> 3;
        bytes = {};
        switch (tag & 7) {
        case 0: { // a number
            std::uint64_t ignored = 0;
            return Varint(ignored) || Fail();
        }
        case 1: // 64 bits
            return Skip(8);
        case 2: { // length-delimited
            std::uint64_t length = 0;
            if (!Varint(length) || length > data_.size() - at_) return Fail();
            bytes = data_.subspan(at_, static_cast<std::size_t>(length));
            at_ += static_cast<std::size_t>(length);
            return true;
        }
        case 5: // 32 bits
            return Skip(4);
        default:
            return Fail();
        }
    }
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    bool Fail() noexcept {
        failed_ = true;
        return false;
    }
    bool Skip(std::size_t count) noexcept {
        if (data_.size() - at_ < count) return Fail();
        at_ += count;
        return true;
    }
    bool Varint(std::uint64_t& value) noexcept {
        value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (at_ >= data_.size()) return false;
            const std::uint8_t byte = data_[at_++];
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }

    ByteSpan data_;
    std::size_t at_ = 0;
    bool failed_ = false;
};

[[nodiscard]] std::optional<KeyProof> ReadKeyProof(ByteSpan bytes) {
    KeyProof proof;
    ProtoReader reader(bytes);
    std::uint64_t field = 0;
    ByteSpan value;
    while (reader.Next(field, value)) {
        if (field == 1) proof.publicKey.assign(value.begin(), value.end());
        else if (field == 2) proof.signature.assign(value.begin(), value.end());
    }
    if (reader.failed()) return std::nullopt;
    return proof;
}

// ---- inflating (RFC 1951), after Mark Adler's puff ----

constexpr int kMaxBits = 15;
constexpr int kMaxLiteralCodes = 286;
constexpr int kMaxDistanceCodes = 30;
constexpr int kFixedLiteralCodes = 288;

// A canonical Huffman code: how many codes there are of each length, and the symbols in
// the order of their codes.
struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::array<std::uint16_t, kFixedLiteralCodes> symbol{};
};

// Builds a code from the lengths of its symbols' codes. Gives 0 for a complete code, more
// than 0 for one with codes left unused, and less than 0 for one with too many.
int Construct(Huffman& code, const std::uint16_t* lengths, int symbols) noexcept {
    code.count.fill(0);
    for (int symbol = 0; symbol < symbols; ++symbol) ++code.count[lengths[symbol]];
    if (code.count[0] == symbols) return 0; // no codes at all: decoding with it fails
    int left = 1;
    for (int length = 1; length <= kMaxBits; ++length) {
        left <<= 1;
        left -= code.count[length];
        if (left < 0) return left;
    }
    std::array<std::uint16_t, kMaxBits + 1> offsets{};
    for (int length = 1; length < kMaxBits; ++length) {
        offsets[length + 1] = static_cast<std::uint16_t>(offsets[length] + code.count[length]);
    }
    for (int symbol = 0; symbol < symbols; ++symbol) {
        if (lengths[symbol] != 0) code.symbol[offsets[lengths[symbol]]++] = static_cast<std::uint16_t>(symbol);
    }
    return left;
}

class Inflater {
public:
    Inflater(ByteSpan in, std::size_t size) : in_(in), size_(size) { out_.reserve(size); }

    std::optional<Bytes> Run() {
        for (bool last = false; !last;) {
            last = Bits(1) != 0;
            bool ok = false;
            switch (Bits(2)) {
            case 0:
                ok = Stored();
                break;
            case 1:
                ok = Fixed();
                break;
            case 2:
                ok = Dynamic();
                break;
            default:
                break;
            }
            if (!ok || failed_) return std::nullopt;
        }
        if (out_.size() != size_) return std::nullopt;
        return std::move(out_);
    }

private:
    int Bits(int need) noexcept {
        std::uint32_t value = bitBuffer_;
        while (bitCount_ < need) {
            if (at_ >= in_.size()) {
                failed_ = true;
                return 0;
            }
            value |= static_cast<std::uint32_t>(in_[at_++]) << bitCount_;
            bitCount_ += 8;
        }
        bitBuffer_ = value >> need;
        bitCount_ -= need;
        return static_cast<int>(value & ((1u << need) - 1));
    }

    bool Stored() {
        bitBuffer_ = 0; // a stored block starts on a byte
        bitCount_ = 0;
        if (in_.size() - at_ < 4) return false;
        const std::size_t length = Read16(in_, at_);
        const std::size_t check = Read16(in_, at_ + 2);
        at_ += 4;
        if (length != (~check & 0xFFFF) || in_.size() - at_ < length || size_ - out_.size() < length) return false;
        out_.insert(out_.end(), in_.begin() + static_cast<std::ptrdiff_t>(at_),
                    in_.begin() + static_cast<std::ptrdiff_t>(at_ + length));
        at_ += length;
        return true;
    }

    int Decode(const Huffman& code) noexcept {
        int bits = 0, first = 0, index = 0;
        for (int length = 1; length <= kMaxBits; ++length) {
            bits |= Bits(1);
            if (failed_) return -1;
            const int count = code.count[length];
            if (bits - count < first) return code.symbol[index + (bits - first)];
            index += count;
            first += count;
            first <<= 1;
            bits <<= 1;
        }
        return -1;
    }

    bool Codes(const Huffman& literals, const Huffman& distances) {
        static constexpr std::uint16_t kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                          31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr std::uint8_t kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                          2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static constexpr std::uint16_t kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                            33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                            1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static constexpr std::uint8_t kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                            6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int symbol = Decode(literals);
            if (symbol < 0) return false;
            if (symbol < 256) {
                if (out_.size() >= size_) return false;
                out_.push_back(static_cast<std::uint8_t>(symbol));
                continue;
            }
            if (symbol == 256) return true;
            symbol -= 257;
            if (symbol >= 29) return false;
            const std::size_t length = kLengthBase[symbol] + static_cast<std::size_t>(Bits(kLengthExtra[symbol]));
            const int code = Decode(distances);
            if (code < 0 || code >= 30) return false;
            const std::size_t distance = kDistanceBase[code] + static_cast<std::size_t>(Bits(kDistanceExtra[code]));
            if (failed_ || distance > out_.size() || size_ - out_.size() < length) return false;
            // Byte by byte, since the copy may run into what it is making.
            const std::size_t from = out_.size() - distance;
            for (std::size_t i = 0; i < length; ++i) out_.push_back(out_[from + i]);
        }
    }

    bool Fixed() {
        if (!fixedMade_) {
            std::array<std::uint16_t, kFixedLiteralCodes> lengths{};
            for (int symbol = 0; symbol < kFixedLiteralCodes; ++symbol) {
                lengths[symbol] = symbol < 144 ? 8 : symbol < 256 ? 9 : symbol < 280 ? 7 : 8;
            }
            Construct(fixedLiterals_, lengths.data(), kFixedLiteralCodes);
            std::array<std::uint16_t, kMaxDistanceCodes> distances{};
            distances.fill(5);
            Construct(fixedDistances_, distances.data(), kMaxDistanceCodes);
            fixedMade_ = true;
        }
        return Codes(fixedLiterals_, fixedDistances_);
    }

    bool Dynamic() {
        static constexpr std::uint8_t kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        const int literalCount = Bits(5) + 257;
        const int distanceCount = Bits(5) + 1;
        const int lengthCodeCount = Bits(4) + 4;
        if (failed_ || literalCount > kMaxLiteralCodes || distanceCount > kMaxDistanceCodes) return false;

        std::array<std::uint16_t, kMaxLiteralCodes + kMaxDistanceCodes> lengths{};
        for (int i = 0; i < lengthCodeCount; ++i) lengths[kOrder[i]] = static_cast<std::uint16_t>(Bits(3));
        Huffman literals, distances;
        if (failed_ || Construct(literals, lengths.data(), 19) != 0) return false;

        for (int i = 0; i < literalCount + distanceCount;) {
            const int symbol = Decode(literals);
            if (symbol < 0) return false;
            if (symbol < 16) {
                lengths[i++] = static_cast<std::uint16_t>(symbol);
                continue;
            }
            std::uint16_t length = 0;
            int repeat = 0;
            if (symbol == 16) {
                if (i == 0) return false;
                length = lengths[i - 1];
                repeat = 3 + Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + Bits(3);
            } else {
                repeat = 11 + Bits(7);
            }
            if (failed_ || i + repeat > literalCount + distanceCount) return false;
            while (repeat-- > 0) lengths[i++] = length;
        }
        if (lengths[256] == 0) return false; // no end of block

        // An incomplete code is allowed only when it is a single code.
        int left = Construct(literals, lengths.data(), literalCount);
        if (left < 0 || (left > 0 && literalCount - literals.count[0] != 1)) return false;
        left = Construct(distances, lengths.data() + literalCount, distanceCount);
        if (left < 0 || (left > 0 && distanceCount - distances.count[0] != 1)) return false;
        return Codes(literals, distances);
    }

    ByteSpan in_;
    std::size_t at_ = 0;
    std::uint32_t bitBuffer_ = 0;
    int bitCount_ = 0;
    bool failed_ = false;
    std::size_t size_;
    Bytes out_;
    bool fixedMade_ = false;
    Huffman fixedLiterals_, fixedDistances_;
};

// ---- JSON, as far as a manifest needs ----

namespace json {

[[nodiscard]] std::size_t SkipSpace(std::string_view text, std::size_t at) noexcept {
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\n' || text[at] == '\r')) ++at;
    return at;
}

// Past the string whose opening quote is at `at`; kNone if it never ends.
[[nodiscard]] std::size_t SkipString(std::string_view text, std::size_t at) noexcept {
    for (++at; at < text.size(); ++at) {
        if (text[at] == '\\') {
            ++at;
        } else if (text[at] == '"') {
            return at + 1;
        } else if (static_cast<unsigned char>(text[at]) < 0x20) {
            return kNone;
        }
    }
    return kNone;
}

// Past the value at `at`, whatever it holds; kNone if it is malformed.
[[nodiscard]] std::size_t SkipValue(std::string_view text, std::size_t at, int depth = 0) noexcept {
    at = SkipSpace(text, at);
    if (at >= text.size() || depth > 64) return kNone;
    if (text[at] == '"') return SkipString(text, at);
    if (text[at] == '{' || text[at] == '[') {
        const bool object = text[at] == '{';
        const char close = object ? '}' : ']';
        at = SkipSpace(text, at + 1);
        if (at < text.size() && text[at] == close) return at + 1;
        for (;;) {
            if (object) {
                at = SkipSpace(text, at);
                if (at >= text.size() || text[at] != '"') return kNone;
                at = SkipString(text, at);
                if (at == kNone) return kNone;
                at = SkipSpace(text, at);
                if (at >= text.size() || text[at] != ':') return kNone;
                ++at;
            }
            at = SkipValue(text, at, depth + 1);
            if (at == kNone) return kNone;
            at = SkipSpace(text, at);
            if (at >= text.size()) return kNone;
            if (text[at] == close) return at + 1;
            if (text[at] != ',') return kNone;
            ++at;
        }
    }
    // A number, true, false or null.
    std::size_t end = text.find_first_of(",}] \t\r\n", at);
    if (end == kNone) end = text.size();
    const std::string_view word = text.substr(at, end - at);
    if (word.empty()) return kNone;
    if (word != "true" && word != "false" && word != "null" && word.find_first_not_of("0123456789+-.eE") != kNone) {
        return kNone;
    }
    return end;
}

struct Member {
    std::string_view name; // between its quotes, as written
    std::size_t begin = 0; // where its name starts
    std::size_t valueBegin = 0;
    std::size_t end = 0; // past its value
};

// The members of the object `text` holds, and nothing but; nothing if it holds anything else.
[[nodiscard]] std::optional<std::vector<Member>> Members(std::string_view text) {
    std::size_t at = SkipSpace(text, 0);
    if (at >= text.size() || text[at] != '{') return std::nullopt;
    const std::size_t end = SkipValue(text, at);
    if (end == kNone || SkipSpace(text, end) != text.size()) return std::nullopt;
    // Well formed, so from here on nothing needs checking.
    std::vector<Member> members;
    at = SkipSpace(text, at + 1);
    if (text[at] == '}') return members;
    for (;;) {
        Member member;
        member.begin = SkipSpace(text, at);
        const std::size_t nameEnd = SkipString(text, member.begin);
        member.name = text.substr(member.begin + 1, nameEnd - member.begin - 2);
        member.valueBegin = SkipSpace(text, SkipSpace(text, nameEnd) + 1);
        member.end = SkipValue(text, member.valueBegin);
        members.push_back(member);
        at = SkipSpace(text, member.end);
        if (text[at] != ',') return members;
        ++at;
    }
}

// The elements of the array `text` is, as their places in it; nothing if it isn't one.
[[nodiscard]] std::optional<std::vector<std::string_view>> Elements(std::string_view text) {
    std::size_t at = SkipSpace(text, 0);
    if (at >= text.size() || text[at] != '[') return std::nullopt;
    const std::size_t end = SkipValue(text, at);
    if (end == kNone || SkipSpace(text, end) != text.size()) return std::nullopt;
    std::vector<std::string_view> elements;
    at = SkipSpace(text, at + 1);
    if (text[at] == ']') return elements;
    for (;;) {
        const std::size_t begin = SkipSpace(text, at);
        const std::size_t elementEnd = SkipValue(text, begin);
        elements.push_back(text.substr(begin, elementEnd - begin));
        at = SkipSpace(text, elementEnd);
        if (text[at] != ',') return elements;
        ++at;
    }
}

} // namespace json

[[nodiscard]] std::string_view WithoutBom(std::string_view text) noexcept {
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    return text;
}

// The permissions without the side panel's.
[[nodiscard]] std::string WithoutSidePanel(std::string_view array) {
    const auto elements = json::Elements(array);
    if (!elements) return std::string(array);
    std::string out = "[";
    for (const std::string_view element : *elements) {
        if (element == R"("sidePanel")") continue;
        if (out.size() > 1) out += ", ";
        out += element;
    }
    return out + "]";
}

// ---- the update service's XML ----

[[nodiscard]] std::string XmlUnescape(std::string_view text) {
    static constexpr std::pair<std::string_view, char> kEntities[] = {
        {"&amp;", '&'}, {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}};
    std::string out;
    out.reserve(text.size());
    for (std::size_t at = 0; at < text.size();) {
        bool replaced = false;
        if (text[at] == '&') {
            for (const auto& [entity, character] : kEntities) {
                if (text.substr(at).starts_with(entity)) {
                    out += character;
                    at += entity.size();
                    replaced = true;
                    break;
                }
            }
        }
        if (!replaced) out += text[at++];
    }
    return out;
}

[[nodiscard]] bool IsXmlSpace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// The value of an attribute of an element, given its start tag from '<' to '>'.
[[nodiscard]] std::optional<std::string> Attribute(std::string_view tag, std::string_view name) {
    for (std::size_t at = tag.find(name); at != kNone; at = tag.find(name, at + 1)) {
        if (at == 0 || !IsXmlSpace(tag[at - 1])) continue;
        std::size_t next = at + name.size();
        while (next < tag.size() && IsXmlSpace(tag[next])) ++next;
        if (next >= tag.size() || tag[next] != '=') continue;
        ++next;
        while (next < tag.size() && IsXmlSpace(tag[next])) ++next;
        if (next >= tag.size() || (tag[next] != '"' && tag[next] != '\'')) continue;
        const std::size_t close = tag.find(tag[next], next + 1);
        if (close == kNone) return std::nullopt;
        return XmlUnescape(tag.substr(next + 1, close - next - 1));
    }
    return std::nullopt;
}

// The start tags of the elements named `name`, in order.
[[nodiscard]] std::vector<std::string_view> StartTags(std::string_view xml, std::string_view name) {
    std::vector<std::string_view> tags;
    const std::string open = "<" + std::string(name);
    for (std::size_t at = xml.find(open); at != kNone; at = xml.find(open, at + 1)) {
        const std::size_t after = at + open.size();
        if (after >= xml.size() || !(IsXmlSpace(xml[after]) || xml[after] == '>' || xml[after] == '/')) continue;
        const std::size_t close = xml.find('>', after);
        if (close == kNone) break;
        tags.push_back(xml.substr(at, close - at + 1));
    }
    return tags;
}

[[nodiscard]] bool IsExtensionId(std::string_view id) noexcept {
    return id.size() == 32 && std::ranges::all_of(id, [](char c) { return c >= 'a' && c <= 'p'; });
}

// ---- unpacking ----

[[nodiscard]] bool SafeNamePart(std::string_view part) {
    // ".", ".." and the like, and names Windows would shorten to another: it drops dots and
    // spaces at the end of a name.
    if (part.find_first_not_of(". ") == kNone || part.back() == '.' || part.back() == ' ') return false;
    for (const char c : part) {
        if (static_cast<unsigned char>(c) < 0x20 || std::string_view(R"(<>:"\|?*)").contains(c)) return false;
    }
    // Devices, which are found in every folder, with or without an extension.
    std::string stem(part.substr(0, part.find('.')));
    while (!stem.empty() && stem.back() == ' ') stem.pop_back();
    for (char& c : stem) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    static constexpr std::string_view kDevices[] = {"con", "prn", "aux", "nul", "conin$", "conout$", "clock$"};
    if (std::ranges::find(kDevices, stem) != std::end(kDevices)) return false;
    if (stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '0' && stem[3] <= '9') {
        return false;
    }
    return true;
}

[[nodiscard]] bool WriteFileBytes(const std::filesystem::path& path, ByteSpan bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.close();
    return !out.fail();
}

} // namespace

// ---- hashes and encodings ----

std::optional<Sha256Digest> Sha256(std::initializer_list<ByteSpan> parts) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (!BCRYPT_SUCCESS(::BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return std::nullopt;
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCRYPT_SUCCESS(::BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0));
    for (const ByteSpan part : parts) {
        // It takes a ULONG's worth at a time.
        for (std::size_t done = 0; ok && done < part.size();) {
            const auto count = static_cast<ULONG>(std::min<std::size_t>(part.size() - done, 1u << 30));
            ok = BCRYPT_SUCCESS(::BCryptHashData(hash, const_cast<PUCHAR>(part.data() + done), count, 0));
            done += count;
        }
    }
    Sha256Digest digest{};
    ok = ok && BCRYPT_SUCCESS(::BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0));
    if (hash) ::BCryptDestroyHash(hash);
    ::BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!ok) return std::nullopt;
    return digest;
}

std::uint32_t Crc32(ByteSpan data) noexcept {
    static constexpr auto kTable = [] {
        std::array<std::uint32_t, 256> table{};
        for (std::uint32_t n = 0; n < 256; ++n) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[n] = c;
        }
        return table;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t byte : data) crc = kTable[(crc ^ byte) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

namespace {
constexpr std::string_view kBase64Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
}

std::string Base64Encode(ByteSpan data) {
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (std::size_t at = 0; at < data.size(); at += 3) {
        const std::size_t left = std::min<std::size_t>(3, data.size() - at);
        const std::uint32_t group = static_cast<std::uint32_t>(data[at]) << 16 |
                                    (left > 1 ? static_cast<std::uint32_t>(data[at + 1]) << 8 : 0) |
                                    (left > 2 ? static_cast<std::uint32_t>(data[at + 2]) : 0);
        out += kBase64Alphabet[(group >> 18) & 63];
        out += kBase64Alphabet[(group >> 12) & 63];
        out += left > 1 ? kBase64Alphabet[(group >> 6) & 63] : '=';
        out += left > 2 ? kBase64Alphabet[group & 63] : '=';
    }
    return out;
}

std::optional<Bytes> Base64Decode(std::string_view text) {
    if (text.size() % 4 != 0) return std::nullopt;
    Bytes out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t at = 0; at < text.size(); at += 4) {
        std::uint32_t group = 0;
        int padding = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            const char c = text[at + i];
            if (c == '=') {
                // Only at the very end, and no more than two.
                if (at + 4 != text.size() || i < 2) return std::nullopt;
                ++padding;
                group <<= 6;
                continue;
            }
            if (padding) return std::nullopt;
            const std::size_t value = kBase64Alphabet.find(c);
            if (value == kNone) return std::nullopt;
            group = group << 6 | static_cast<std::uint32_t>(value);
        }
        out.push_back(static_cast<std::uint8_t>(group >> 16));
        if (padding < 2) out.push_back(static_cast<std::uint8_t>(group >> 8));
        if (padding < 1) out.push_back(static_cast<std::uint8_t>(group));
    }
    return out;
}

std::optional<Bytes> HexDecode(std::string_view text) {
    if (text.size() % 2 != 0) return std::nullopt;
    const auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    Bytes out;
    for (std::size_t at = 0; at < text.size(); at += 2) {
        const int high = digit(text[at]), low = digit(text[at + 1]);
        if (high < 0 || low < 0) return std::nullopt;
        out.push_back(static_cast<std::uint8_t>(high << 4 | low));
    }
    return out;
}

// ---- extension ids ----

std::string ExtensionIdFromKey(ByteSpan publicKey) {
    const auto hash = Sha256({publicKey});
    if (!hash) return {};
    std::string id;
    for (std::size_t i = 0; i < 16; ++i) {
        id += static_cast<char>('a' + ((*hash)[i] >> 4));
        id += static_cast<char>('a' + ((*hash)[i] & 15));
    }
    return id;
}

std::optional<Bytes> ExtensionIdBytes(std::string_view id) {
    if (!IsExtensionId(id)) return std::nullopt;
    Bytes bytes;
    for (std::size_t at = 0; at < id.size(); at += 2) {
        bytes.push_back(static_cast<std::uint8_t>((id[at] - 'a') << 4 | (id[at + 1] - 'a')));
    }
    return bytes;
}

// ---- the store's update service ----

std::wstring StoreUpdateCheckUrl(std::string_view id, std::wstring_view browserVersion) {
    std::wstring version;
    for (const wchar_t c : browserVersion) {
        if ((c >= L'0' && c <= L'9') || c == L'.') version += c;
    }
    if (version.empty()) version = L"0";
    // Version 0.0.0 asks for the latest whatever is installed: the answer always offers it.
    return L"https://clients2.google.com/service/update2/crx?response=updatecheck&acceptformat=crx3&prodversion=" +
           version + L"&x=id%3D" + FromUtf8(IsExtensionId(id) ? id : std::string_view()) + L"%26v%3D0.0.0%26uc";
}

std::optional<StoreRelease> ParseStoreUpdateCheck(std::string_view xml, std::string_view id) {
    // The <app> element of the extension, and the <updatecheck> inside it.
    for (const std::string_view app : StartTags(xml, "app")) {
        if (Attribute(app, "appid") != id) continue;
        const std::size_t from = static_cast<std::size_t>(app.data() - xml.data()) + app.size();
        std::size_t to = xml.find("</app>", from);
        if (to == kNone) to = xml.size();
        for (const std::string_view check : StartTags(xml.substr(from, to - from), "updatecheck")) {
            if (Attribute(check, "status") != "ok") return std::nullopt;
            StoreRelease release;
            release.version = Attribute(check, "version").value_or(std::string());
            release.codebase = Attribute(check, "codebase").value_or(std::string());
            release.sha256Hex = Attribute(check, "hash_sha256").value_or(std::string());
            if (release.version.empty() || release.version.find_first_not_of("0123456789.") != kNone ||
                !release.codebase.starts_with("https://")) {
                return std::nullopt;
            }
            return release;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

int CompareVersions(std::string_view a, std::string_view b) noexcept {
    while (!a.empty() || !b.empty()) {
        const auto take = [](std::string_view& text) {
            const std::size_t dot = text.find('.');
            const std::string_view part = text.substr(0, dot);
            text = dot == kNone ? std::string_view() : text.substr(dot + 1);
            std::uint64_t value = 0;
            std::from_chars(part.data(), part.data() + part.size(), value);
            return value;
        };
        const std::uint64_t left = take(a), right = take(b);
        if (left != right) return left < right ? -1 : 1;
    }
    return 0;
}

// ---- CRX3 packages ----

std::optional<CrxPackage> ReadCrx(ByteSpan bytes) {
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "Cr24", 4) != 0 || Read32(bytes, 4) != 3) return std::nullopt;
    const std::uint32_t headerSize = Read32(bytes, 8);
    if (headerSize > bytes.size() - 12) return std::nullopt;

    CrxPackage crx;
    crx.archive = bytes.subspan(12 + static_cast<std::size_t>(headerSize));
    ProtoReader header(bytes.subspan(12, headerSize));
    std::uint64_t field = 0;
    ByteSpan value;
    while (header.Next(field, value)) {
        if (field == 2 || field == 3) {
            auto proof = ReadKeyProof(value);
            if (!proof) return std::nullopt;
            (field == 2 ? crx.rsaProofs : crx.ecdsaProofs).push_back(std::move(*proof));
        } else if (field == 10000) {
            crx.signedHeaderData.assign(value.begin(), value.end());
        }
    }
    if (header.failed()) return std::nullopt;

    ProtoReader signedData(crx.signedHeaderData);
    while (signedData.Next(field, value)) {
        if (field == 1) crx.crxId.assign(value.begin(), value.end());
    }
    if (signedData.failed() || crx.crxId.size() != 16) return std::nullopt;
    return crx;
}

std::optional<Sha256Digest> CrxSignedHash(const CrxPackage& crx) {
    static constexpr char kPreamble[] = "CRX3 SignedData"; // with its terminating zero
    const auto size = static_cast<std::uint32_t>(crx.signedHeaderData.size());
    const std::uint8_t sizeBytes[4] = {static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
                                       static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24)};
    return Sha256({AsBytes(std::string_view(kPreamble, sizeof(kPreamble))), ByteSpan(sizeBytes),
                   ByteSpan(crx.signedHeaderData), crx.archive});
}

bool VerifyRsaSha256(ByteSpan publicKey, const Sha256Digest& hash, ByteSpan signature) {
    CERT_PUBLIC_KEY_INFO* info = nullptr;
    DWORD infoSize = 0;
    if (publicKey.empty() || signature.empty() ||
        !::CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, publicKey.data(),
                               static_cast<DWORD>(publicKey.size()), CRYPT_DECODE_ALLOC_FLAG, nullptr, &info,
                               &infoSize)) {
        return false;
    }
    BCRYPT_KEY_HANDLE key = nullptr;
    const bool rsa = info->Algorithm.pszObjId && std::string_view(info->Algorithm.pszObjId) == szOID_RSA_RSA;
    const bool imported = rsa && ::CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, nullptr, &key);
    ::LocalFree(info);
    if (!imported) return false;
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    const NTSTATUS status = ::BCryptVerifySignature(key, &padding, const_cast<PUCHAR>(hash.data()),
                                                    static_cast<ULONG>(hash.size()), const_cast<PUCHAR>(signature.data()),
                                                    static_cast<ULONG>(signature.size()), BCRYPT_PAD_PKCS1);
    ::BCryptDestroyKey(key);
    return BCRYPT_SUCCESS(status);
}

std::optional<VerifiedCrx> VerifyCrx(ByteSpan bytes, std::string_view id) {
    const auto crx = ReadCrx(bytes);
    const auto wanted = ExtensionIdBytes(id);
    if (!crx || !wanted || crx->crxId != *wanted) return std::nullopt;
    const auto hash = CrxSignedHash(*crx);
    if (!hash) return std::nullopt;
    // The extension's own key is the one its id comes from. The store signs the package
    // too, with a key of its own, which this doesn't need.
    for (const KeyProof& proof : crx->rsaProofs) {
        const auto keyHash = Sha256({ByteSpan(proof.publicKey)});
        if (!keyHash || !std::equal(wanted->begin(), wanted->end(), keyHash->begin())) continue;
        if (VerifyRsaSha256(proof.publicKey, *hash, proof.signature)) return VerifiedCrx{proof.publicKey, crx->archive};
    }
    return std::nullopt;
}

// ---- zip archives ----

namespace {
constexpr std::uint32_t kEndOfDirectory = 0x06054B50;
constexpr std::uint32_t kDirectoryEntry = 0x02014B50;
constexpr std::uint32_t kLocalHeader = 0x04034B50;
constexpr std::size_t kEndSize = 22;
constexpr std::size_t kDirectoryEntrySize = 46;
constexpr std::size_t kLocalHeaderSize = 30;
} // namespace

std::optional<std::vector<ZipEntry>> ReadZipDirectory(ByteSpan zip) {
    if (zip.size() < kEndSize) return std::nullopt;
    // The end record is the archive's last, followed only by a comment of up to 64 KB.
    std::size_t end = kNone;
    const std::size_t lowest = zip.size() > kEndSize + 0xFFFF ? zip.size() - kEndSize - 0xFFFF : 0;
    for (std::size_t at = zip.size() - kEndSize;; --at) {
        if (Read32(zip, at) == kEndOfDirectory && at + kEndSize + Read16(zip, at + 20) == zip.size()) {
            end = at;
            break;
        }
        if (at == lowest) break;
    }
    if (end == kNone) return std::nullopt;

    const std::uint16_t disk = Read16(zip, end + 4), directoryDisk = Read16(zip, end + 6);
    const std::uint16_t countHere = Read16(zip, end + 8), count = Read16(zip, end + 10);
    const std::uint32_t directorySize = Read32(zip, end + 12), directoryAt = Read32(zip, end + 16);
    // Split archives, and ZIP64's, which these numbers would only point to.
    if (disk != 0 || directoryDisk != 0 || countHere != count || count == 0xFFFF || directoryAt == 0xFFFFFFFF ||
        directorySize == 0xFFFFFFFF || directoryAt > end || directorySize > end - directoryAt) {
        return std::nullopt;
    }

    std::vector<ZipEntry> entries;
    entries.reserve(count);
    std::size_t at = directoryAt;
    for (std::uint16_t i = 0; i < count; ++i) {
        if (end - at < kDirectoryEntrySize || Read32(zip, at) != kDirectoryEntry) return std::nullopt;
        const std::uint16_t flags = Read16(zip, at + 8);
        ZipEntry entry;
        entry.method = Read16(zip, at + 10);
        entry.crc = Read32(zip, at + 16);
        entry.compressedSize = Read32(zip, at + 20);
        entry.size = Read32(zip, at + 24);
        const std::size_t nameLength = Read16(zip, at + 28);
        const std::size_t extraLength = Read16(zip, at + 30);
        const std::size_t commentLength = Read16(zip, at + 32);
        entry.localHeaderOffset = Read32(zip, at + 42);
        if ((flags & 1) != 0) return std::nullopt; // encrypted
        if (entry.compressedSize == 0xFFFFFFFF || entry.size == 0xFFFFFFFF || entry.localHeaderOffset == 0xFFFFFFFF) {
            return std::nullopt; // ZIP64
        }
        const std::size_t total = kDirectoryEntrySize + nameLength + extraLength + commentLength;
        if (end - at < total) return std::nullopt;
        entry.name.assign(reinterpret_cast<const char*>(zip.data() + at + kDirectoryEntrySize), nameLength);
        entries.push_back(std::move(entry));
        at += total;
    }
    return entries;
}

std::optional<Bytes> ReadZipEntry(ByteSpan zip, const ZipEntry& entry) {
    const std::size_t at = entry.localHeaderOffset;
    if (at > zip.size() || zip.size() - at < kLocalHeaderSize || Read32(zip, at) != kLocalHeader) return std::nullopt;
    const std::size_t dataAt = at + kLocalHeaderSize + Read16(zip, at + 26) + Read16(zip, at + 28);
    if (dataAt > zip.size() || zip.size() - dataAt < entry.compressedSize) return std::nullopt;
    const ByteSpan data = zip.subspan(dataAt, entry.compressedSize);

    std::optional<Bytes> contents;
    if (entry.method == 0) {
        if (entry.compressedSize != entry.size) return std::nullopt;
        contents = Bytes(data.begin(), data.end());
    } else if (entry.method == 8) {
        contents = Inflate(data, entry.size);
    }
    if (!contents || Crc32(*contents) != entry.crc) return std::nullopt;
    return contents;
}

std::optional<Bytes> Inflate(ByteSpan deflated, std::size_t size) {
    return Inflater(deflated, size).Run();
}

std::optional<std::filesystem::path> SafeArchivePath(std::string_view name) {
    if (name.empty() || name.size() > 400) return std::nullopt;
    std::filesystem::path path;
    for (std::size_t at = 0;;) {
        const std::size_t slash = name.find('/', at);
        const std::string_view part = name.substr(at, slash == kNone ? kNone : slash - at);
        // An empty part is a name that starts or ends with a slash, or has two together.
        if (part.empty() || !SafeNamePart(part)) return std::nullopt;
        path /= FromUtf8(part);
        if (slash == kNone) return path;
        at = slash + 1;
    }
}

// ---- manifests ----

std::optional<std::string> ManifestVersion(std::string_view manifest) {
    manifest = WithoutBom(manifest);
    const auto members = json::Members(manifest);
    if (!members) return std::nullopt;
    for (const json::Member& member : *members) {
        if (member.name != "version" || manifest[member.valueBegin] != '"') continue;
        const std::string_view value = manifest.substr(member.valueBegin + 1, member.end - member.valueBegin - 2);
        if (value.empty() || value.find_first_not_of("0123456789.") != kNone) return std::nullopt;
        return std::string(value);
    }
    return std::nullopt;
}

std::optional<std::string> PrepareManifest(std::string_view manifest, std::string_view keyBase64) {
    manifest = WithoutBom(manifest);
    const auto members = json::Members(manifest);
    // Written into the JSON as it is, so nothing but Base64 may be in it.
    if (!members || keyBase64.empty() ||
        keyBase64.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") != kNone) {
        return std::nullopt;
    }
    std::string out = "{";
    const auto next = [&out] { out += out.size() > 1 ? ",\n   " : "\n   "; };
    for (const json::Member& member : *members) {
        if (member.name == "key" || member.name == "side_panel") continue;
        next();
        if (member.name == "permissions") {
            out += R"("permissions": )";
            out += WithoutSidePanel(manifest.substr(member.valueBegin, member.end - member.valueBegin));
        } else {
            out += manifest.substr(member.begin, member.end - member.begin);
        }
    }
    // Last, as the JSON reader keeps the last of two members of the same name.
    next();
    out += R"("key": ")";
    out += keyBase64;
    out += "\"\n}\n";
    return out;
}

// ---- all of it ----

std::optional<std::string> UnpackStoreExtension(ByteSpan crx, std::string_view id, const std::filesystem::path& folder) {
    std::error_code error;
    if (std::filesystem::exists(folder, error) || error) return std::nullopt;
    const auto verified = VerifyCrx(crx, id);
    if (!verified) return std::nullopt;
    const auto entries = ReadZipDirectory(verified->archive);
    if (!entries || entries->size() > kMostFiles) return std::nullopt;

    const auto failed = [&folder] {
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
        return std::optional<std::string>();
    };
    std::optional<std::string> version;
    std::uint64_t total = 0;
    for (const ZipEntry& entry : *entries) {
        const std::string_view name = entry.name;
        if (name.ends_with('/')) continue; // a folder comes with the files in it
        if (name == "_metadata" || name.starts_with("_metadata/")) continue;
        const auto relative = SafeArchivePath(name);
        total += entry.size;
        if (!relative || total > kMostBytes) return failed();
        auto contents = ReadZipEntry(verified->archive, entry);
        if (!contents) return failed();
        if (name == "manifest.json") {
            const std::string_view text(reinterpret_cast<const char*>(contents->data()), contents->size());
            version = ManifestVersion(text);
            const auto prepared = PrepareManifest(text, Base64Encode(verified->publicKey));
            if (!version || !prepared) return failed();
            contents->assign(prepared->begin(), prepared->end());
        }
        const std::filesystem::path path = folder / *relative;
        std::filesystem::create_directories(path.parent_path(), error);
        if (error || !WriteFileBytes(path, *contents)) return failed();
    }
    if (!version) return failed(); // there was no manifest
    return version;
}

} // namespace exodium
