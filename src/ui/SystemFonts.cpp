#include "ui/SystemFonts.hpp"

#include <windows.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "dwrite.lib")

using Microsoft::WRL::ComPtr;

namespace {
    ComPtr<IDWriteFontCollection> SystemCollection() {
        ComPtr<IDWriteFactory> factory;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                       reinterpret_cast<IUnknown**>(factory.GetAddressOf()))))
            return {};
        ComPtr<IDWriteFontCollection> collection;
        if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) return {};
        return collection;
    }

    std::string ToUtf8(std::wstring_view text) {
        if (text.empty()) return {};
        const int length = static_cast<int>(text.size());
        const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
        std::string result(static_cast<size_t>(std::max(count, 0)), '\0');
        if (count > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), length, result.data(), count, nullptr, nullptr);
        return result;
    }

    std::wstring ToWide(std::string_view text) {
        if (text.empty()) return {};
        const int length = static_cast<int>(text.size());
        const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), length, nullptr, 0);
        std::wstring result(static_cast<size_t>(std::max(count, 0)), L'\0');
        if (count > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), length, result.data(), count);
        return result;
    }

    std::wstring FamilyName(IDWriteFontFamily* family) {
        ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(family->GetFamilyNames(&names)) || names->GetCount() == 0) return {};
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &index, &exists)) || !exists) index = 0;
        UINT32 length = 0;
        if (FAILED(names->GetStringLength(index, &length))) return {};
        std::wstring name(static_cast<size_t>(length) + 1, L'\0');
        if (FAILED(names->GetString(index, name.data(), length + 1))) return {};
        name.resize(length);
        return name;
    }

    bool IsTextFamily(IDWriteFontFamily* family) {
        ComPtr<IDWriteFont> font;
        if (FAILED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font)))
            return false;
        if (font->IsSymbolFont() || font->GetStyle() != DWRITE_FONT_STYLE_NORMAL) return false;
        BOOL hasLatin = FALSE;
        return SUCCEEDED(font->HasCharacter(L'A', &hasLatin)) && hasLatin;
    }

    bool LocalFontFile(IDWriteFont* font, SystemFonts::Face& out) {
        ComPtr<IDWriteFontFace> face;
        if (FAILED(font->CreateFontFace(&face))) return false;
        UINT32 fileCount = 0;
        if (FAILED(face->GetFiles(&fileCount, nullptr)) || fileCount != 1) return false;
        ComPtr<IDWriteFontFile> file;
        if (FAILED(face->GetFiles(&fileCount, file.GetAddressOf()))) return false;
        const void* key = nullptr;
        UINT32 keySize = 0;
        ComPtr<IDWriteFontFileLoader> loader;
        ComPtr<IDWriteLocalFontFileLoader> localLoader;
        if (FAILED(file->GetReferenceKey(&key, &keySize)) || FAILED(file->GetLoader(&loader)) || FAILED(loader.As(&localLoader)))
            return false;
        UINT32 length = 0;
        if (FAILED(localLoader->GetFilePathLengthFromKey(key, keySize, &length))) return false;
        std::wstring path(static_cast<size_t>(length) + 1, L'\0');
        if (FAILED(localLoader->GetFilePathFromKey(key, keySize, path.data(), length + 1))) return false;
        path.resize(length);
        out.file = std::filesystem::path(path);
        out.faceIndex = face->GetIndex();
        out.weight = static_cast<int>(font->GetWeight());
        return true;
    }

    std::wstring Folded(std::wstring_view text) {
        std::wstring folded(text);
        std::transform(folded.begin(), folded.end(), folded.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return folded;
    }
}

namespace SystemFonts {
    std::vector<std::string> ListFamilies() {
        auto collection = SystemCollection();
        if (!collection) return {};
        std::vector<std::wstring> names;
        const UINT32 count = collection->GetFontFamilyCount();
        names.reserve(count);
        for (UINT32 i = 0; i < count; ++i) {
            ComPtr<IDWriteFontFamily> family;
            if (FAILED(collection->GetFontFamily(i, &family)) || !IsTextFamily(family.Get())) continue;
            std::wstring name = FamilyName(family.Get());
            if (!name.empty()) names.push_back(std::move(name));
        }
        std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return Folded(a) < Folded(b); });
        std::vector<std::string> result;
        result.reserve(names.size());
        for (const auto& name : names)
            result.push_back(ToUtf8(name));
        return result;
    }

    std::vector<Face> FindFaces(std::string_view familyName, std::initializer_list<int> weights) {
        std::vector<Face> faces;
        if (familyName.empty()) return faces;
        auto collection = SystemCollection();
        if (!collection) return faces;
        UINT32 index = 0;
        BOOL exists = FALSE;
        if (FAILED(collection->FindFamilyName(ToWide(familyName).c_str(), &index, &exists)) || !exists) return faces;
        ComPtr<IDWriteFontFamily> family;
        if (FAILED(collection->GetFontFamily(index, &family))) return faces;

        for (int weight : weights) {
            ComPtr<IDWriteFont> font;
            if (FAILED(family->GetFirstMatchingFont(static_cast<DWRITE_FONT_WEIGHT>(weight), DWRITE_FONT_STRETCH_NORMAL,
                                                    DWRITE_FONT_STYLE_NORMAL, &font)))
                continue;
            // Synthesized bold/oblique has no file of its own; RmlUi picks the
            // nearest registered weight instead.
            if (font->GetSimulations() != DWRITE_FONT_SIMULATIONS_NONE || font->GetStyle() != DWRITE_FONT_STYLE_NORMAL) continue;
            Face face;
            if (!LocalFontFile(font.Get(), face)) continue;
            const bool duplicate = std::any_of(faces.begin(), faces.end(), [&](const Face& existing) {
                return existing.file == face.file && existing.faceIndex == face.faceIndex && existing.weight == face.weight;
            });
            if (!duplicate) faces.push_back(std::move(face));
        }
        return faces;
    }
}
