#include "ui/rml/RmlFileInterface.hpp"

#include <vector>
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace {
    std::string NormalizeResourcePath(std::string path) {
        std::replace(path.begin(), path.end(), '\\', '/');
        constexpr const char* prefix = "res://";
        if (path.rfind(prefix, 0) == 0) path.erase(0, std::strlen(prefix));
        while (!path.empty() && path.front() == '/')
            path.erase(path.begin());
        return path;
    }
}

bool RmlFileInterface::IsResourcePath(const std::string& path) {
    return path.rfind("res://", 0) == 0;
}

std::string RmlFileInterface::ResourceNameForPath(const std::string& rawPath) {
    const std::string path = NormalizeResourcePath(rawPath);
    if (path == "main.rml") return "RML_MAIN";
    if (path == "omnistats.rcss") return "RML_STYLE";
    if (path == "onboarding.rml") return "RML_ONBOARDING";
    if (path == "insights.rml") return "RML_INSIGHTS";
    return {};
}

RmlFileInterface::Handle* RmlFileInterface::OpenDisk(const std::string& path) const {
    FILE* file = nullptr;
#ifdef _MSC_VER
    fopen_s(&file, path.c_str(), "rb");
#else
    file = std::fopen(path.c_str(), "rb");
#endif
    if (!file) return nullptr;
    auto* handle = new Handle();
    handle->kind = Handle::Kind::Disk;
    handle->disk = file;
    return handle;
}

Rml::FileHandle RmlFileInterface::Open(const Rml::String& rmlPath) {
    const std::string path = rmlPath;

    if (IsResourcePath(path)) {
        const std::string resourceName = ResourceNameForPath(path);
        if (resourceName.empty()) return 0;

        if (!m_overrideDirectory.empty()) {
            if (Handle* handle = OpenDisk(m_overrideDirectory + "/" + NormalizeResourcePath(path)))
                return reinterpret_cast<Rml::FileHandle>(handle);
        }

        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceA(module, resourceName.c_str(), RT_RCDATA);
        if (resource) {
            HGLOBAL loaded = LoadResource(module, resource);
            if (loaded) {
                const DWORD resourceSize = SizeofResource(module, resource);
                const void* bytes = LockResource(loaded);
                if (bytes && resourceSize > 0) {
                    auto* handle = new Handle();
                    handle->kind = Handle::Kind::Memory;
                    handle->memory = static_cast<const unsigned char*>(bytes);
                    handle->size = static_cast<size_t>(resourceSize);
                    handle->position = 0;
                    return reinterpret_cast<Rml::FileHandle>(handle);
                }
            }
        }

        // Fallback to disk for tests or unpacked runs
        const std::string relPath = "resources/rml/" + NormalizeResourcePath(path);
        std::vector<std::string> candidates = {relPath};
#ifdef OMNISTATS_SOURCE_DIR
        candidates.push_back(std::string(OMNISTATS_SOURCE_DIR) + "/" + relPath);
#endif
        for (const auto& candidate : candidates) {
            if (Handle* handle = OpenDisk(candidate)) return reinterpret_cast<Rml::FileHandle>(handle);
        }
        return 0;
    }

    Handle* handle = OpenDisk(path);
    return reinterpret_cast<Rml::FileHandle>(handle);
}

void RmlFileInterface::Close(Rml::FileHandle file) {
    auto* handle = reinterpret_cast<Handle*>(file);
    if (!handle) return;
    if (handle->kind == Handle::Kind::Disk && handle->disk) std::fclose(handle->disk);
    delete handle;
}

size_t RmlFileInterface::Read(void* buffer, size_t size, Rml::FileHandle file) {
    auto* handle = reinterpret_cast<Handle*>(file);
    if (!handle || !buffer || size == 0) return 0;
    if (handle->kind == Handle::Kind::Disk) return std::fread(buffer, 1, size, handle->disk);

    const size_t remaining = handle->position < handle->size ? handle->size - handle->position : 0;
    const size_t count = std::min(size, remaining);
    if (count) {
        std::memcpy(buffer, handle->memory + handle->position, count);
        handle->position += count;
    }
    return count;
}

bool RmlFileInterface::Seek(Rml::FileHandle file, long offset, int origin) {
    auto* handle = reinterpret_cast<Handle*>(file);
    if (!handle) return false;
    if (handle->kind == Handle::Kind::Disk) return std::fseek(handle->disk, offset, origin) == 0;

    long long base = 0;
    if (origin == SEEK_CUR)
        base = static_cast<long long>(handle->position);
    else if (origin == SEEK_END)
        base = static_cast<long long>(handle->size);
    const long long target = base + static_cast<long long>(offset);
    if (target < 0 || static_cast<size_t>(target) > handle->size) return false;
    handle->position = static_cast<size_t>(target);
    return true;
}

size_t RmlFileInterface::Tell(Rml::FileHandle file) {
    auto* handle = reinterpret_cast<Handle*>(file);
    if (!handle) return 0;
    if (handle->kind == Handle::Kind::Disk) {
        const long value = std::ftell(handle->disk);
        return value < 0 ? 0 : static_cast<size_t>(value);
    }
    return handle->position;
}

size_t RmlFileInterface::Length(Rml::FileHandle file) {
    auto* handle = reinterpret_cast<Handle*>(file);
    if (!handle) return 0;
    if (handle->kind == Handle::Kind::Memory) return handle->size;

    const long current = std::ftell(handle->disk);
    if (current < 0) return 0;
    if (std::fseek(handle->disk, 0, SEEK_END) != 0) return 0;
    const long length = std::ftell(handle->disk);
    std::fseek(handle->disk, current, SEEK_SET);
    return length < 0 ? 0 : static_cast<size_t>(length);
}
