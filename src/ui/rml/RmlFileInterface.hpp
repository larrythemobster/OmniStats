#pragma once

#include <RmlUi/Core/FileInterface.h>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

class RmlFileInterface final : public Rml::FileInterface {
  public:
    Rml::FileHandle Open(const Rml::String& path) override;
    void Close(Rml::FileHandle file) override;
    size_t Read(void* buffer, size_t size, Rml::FileHandle file) override;
    bool Seek(Rml::FileHandle file, long offset, int origin) override;
    size_t Tell(Rml::FileHandle file) override;
    size_t Length(Rml::FileHandle file) override;

  private:
    struct Handle {
        enum class Kind { Disk,
                          Memory } kind = Kind::Disk;
        FILE* disk = nullptr;
        const unsigned char* memory = nullptr;
        size_t size = 0;
        size_t position = 0;
    };

    static bool IsResourcePath(const std::string& path);
    static std::string ResourceNameForPath(const std::string& path);
};
