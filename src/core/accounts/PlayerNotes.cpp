#include "core/accounts/PlayerNotes.hpp"

#include "core/accounts/TrackedAccounts.hpp"

#include <vector>

namespace PlayerNotes {
    namespace {
        constexpr char32_t kReplacement = 0xFFFD;

        std::vector<char32_t> Decode(std::string_view text) {
            std::vector<char32_t> out;
            out.reserve(text.size());
            const auto continuation = [&](size_t index) {
                return index < text.size() && (static_cast<unsigned char>(text[index]) & 0xC0u) == 0x80u;
            };
            for (size_t i = 0; i < text.size();) {
                const auto lead = static_cast<unsigned char>(text[i]);
                char32_t cp = kReplacement;
                size_t length = 1;
                if (lead < 0x80u) {
                    cp = lead;
                } else if (lead >= 0xC2u && lead <= 0xDFu && continuation(i + 1)) {
                    cp = ((lead & 0x1Fu) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3Fu);
                    length = 2;
                } else if (lead >= 0xE0u && lead <= 0xEFu && continuation(i + 1) && continuation(i + 2)) {
                    cp = ((lead & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 6) |
                         (static_cast<unsigned char>(text[i + 2]) & 0x3Fu);
                    length = 3;
                    if (cp < 0x800u || (cp >= 0xD800u && cp <= 0xDFFFu)) {
                        cp = kReplacement;
                        length = 1;
                    }
                } else if (lead >= 0xF0u && lead <= 0xF4u && continuation(i + 1) && continuation(i + 2) && continuation(i + 3)) {
                    cp = ((lead & 0x07u) << 18) | ((static_cast<unsigned char>(text[i + 1]) & 0x3Fu) << 12) |
                         ((static_cast<unsigned char>(text[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(text[i + 3]) & 0x3Fu);
                    length = 4;
                    if (cp < 0x10000u || cp > 0x10FFFFu) {
                        cp = kReplacement;
                        length = 1;
                    }
                }
                out.push_back(cp);
                i += length;
            }
            return out;
        }

        void Encode(char32_t cp, std::string& out) {
            if (cp < 0x80u) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800u) {
                out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else if (cp < 0x10000u) {
                out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
            }
        }

        bool IsSpace(char32_t cp) {
            return cp == U' ' || cp == U'\t' || cp == U'\n';
        }

        void TrimTrailing(std::vector<char32_t>& cps) {
            while (!cps.empty() && IsSpace(cps.back()))
                cps.pop_back();
        }
    }

    std::string Identity::Key() const {
        std::string key = platform + "|" + accountId;
        if (splitscreen != 0) key += "|" + std::to_string(splitscreen);
        return key;
    }

    std::optional<Identity> IdentityFromPrimaryId(std::string_view primaryId) {
        const auto account = TrackedAccounts::FromPrimaryId(primaryId);
        if (!account) return std::nullopt;

        Identity identity;
        identity.platform = TrackedAccounts::PlatformToken(account->platform);
        identity.accountId = account->accountId;

        const size_t first = primaryId.find('|');
        const size_t second = primaryId.find('|', first + 1);
        if (second != std::string_view::npos) {
            std::string_view slot = primaryId.substr(second + 1);
            slot = slot.substr(0, slot.find('|'));
            int value = 0;
            bool numeric = !slot.empty() && slot.size() <= 2;
            for (char c : slot) {
                if (c < '0' || c > '9') {
                    numeric = false;
                    break;
                }
                value = value * 10 + (c - '0');
            }
            if (numeric) identity.splitscreen = value;
        }
        return identity;
    }

    size_t CodePointCount(std::string_view text) {
        return Decode(text).size();
    }

    std::string NormalizeNote(std::string_view text) {
        const std::vector<char32_t> decoded = Decode(text);
        std::vector<char32_t> cps;
        cps.reserve(decoded.size());
        for (size_t i = 0; i < decoded.size(); ++i) {
            char32_t cp = decoded[i];
            if (cp == U'\r') {
                if (i + 1 < decoded.size() && decoded[i + 1] == U'\n') continue;
                cp = U'\n';
            }
            const bool control = cp < 0x20u || (cp >= 0x7Fu && cp <= 0x9Fu);
            if (control && cp != U'\n' && cp != U'\t') continue;
            cps.push_back(cp);
        }

        size_t start = 0;
        while (start < cps.size() && IsSpace(cps[start]))
            ++start;
        cps.erase(cps.begin(), cps.begin() + static_cast<std::ptrdiff_t>(start));
        if (cps.size() > kMaxNoteCodePoints) cps.resize(kMaxNoteCodePoints);
        TrimTrailing(cps);

        std::string out;
        out.reserve(cps.size());
        for (char32_t cp : cps)
            Encode(cp, out);
        return out;
    }

    std::string NormalizeTag(std::string_view text) {
        const auto normalized = NormalizeNote(text);
        std::vector<char32_t> clean;
        bool lastSpace = true;
        for (char32_t cp : Decode(normalized)) {
            if (cp == U'\n' || cp == U'\t' || cp == U' ') {
                if (!lastSpace && clean.size() < kMaxTagCodePoints) clean.push_back(U' ');
                lastSpace = true;
                continue;
            }
            if (clean.size() >= kMaxTagCodePoints) break;
            clean.push_back(cp);
            lastSpace = false;
        }
        TrimTrailing(clean);
        std::string out;
        for (char32_t cp : clean)
            Encode(cp, out);
        return out;
    }

    std::string Preview(std::string_view note, size_t maxCodePoints) {
        std::vector<char32_t> cps;
        for (char32_t cp : Decode(note)) {
            if (cp == U'\n' || cp == U'\t' || cp == U'\r') cp = U' ';
            if (cp == U' ' && (cps.empty() || cps.back() == U' ')) continue;
            cps.push_back(cp);
        }
        TrimTrailing(cps);
        const bool truncated = cps.size() > maxCodePoints;
        if (truncated) {
            cps.resize(maxCodePoints);
            TrimTrailing(cps);
        }
        std::string out;
        for (char32_t cp : cps)
            Encode(cp, out);
        if (truncated) out += "\xE2\x80\xA6";
        return out;
    }

} // namespace PlayerNotes
