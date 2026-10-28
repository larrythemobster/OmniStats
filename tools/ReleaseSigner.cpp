#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <cstring>
#include <algorithm>
#include <sodium.h>

namespace {

    std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return "";
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    std::vector<unsigned char> ReadFileBytes(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) return {};
        file.seekg(0, std::ios::end);
        const auto size = file.tellg();
        file.seekg(0, std::ios::beg);
        if (size <= 0) return {};
        std::vector<unsigned char> data(static_cast<size_t>(size));
        file.read(reinterpret_cast<char*>(data.data()), size);
        return data;
    }

    bool WriteFileBytes(const std::string& path, const unsigned char* data, size_t size) {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) return false;
        file.write(reinterpret_cast<const char*>(data), size);
        return file.good();
    }

    bool LoadSecretKey(const std::string& keyPath, std::vector<unsigned char>& outSk) {
        auto raw = ReadFileBytes(keyPath);
        if (raw.empty()) return false;

        // If 64 bytes binary (libsodium sk: seed || pub)
        if (raw.size() == crypto_sign_SECRETKEYBYTES) {
            outSk = std::move(raw);
            return true;
        }

        // If 32 bytes binary (seed)
        if (raw.size() == crypto_sign_SEEDBYTES) {
            outSk.resize(crypto_sign_SECRETKEYBYTES);
            std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
            crypto_sign_seed_keypair(pk.data(), outSk.data(), raw.data());
            sodium_memzero(raw.data(), raw.size());
            return true;
        }

        // Check if hex encoded string
        std::string str(reinterpret_cast<char*>(raw.data()), raw.size());
        str = Trim(str);
        if (str.length() == crypto_sign_SECRETKEYBYTES * 2) {
            outSk.resize(crypto_sign_SECRETKEYBYTES);
            size_t binLen = 0;
            if (sodium_hex2bin(outSk.data(), outSk.size(), str.data(), str.length(),
                               nullptr, &binLen, nullptr) == 0 &&
                binLen == crypto_sign_SECRETKEYBYTES) {
                return true;
            }
        } else if (str.length() == crypto_sign_SEEDBYTES * 2) {
            std::vector<unsigned char> seed(crypto_sign_SEEDBYTES);
            size_t binLen = 0;
            if (sodium_hex2bin(seed.data(), seed.size(), str.data(), str.length(),
                               nullptr, &binLen, nullptr) == 0 &&
                binLen == crypto_sign_SEEDBYTES) {
                outSk.resize(crypto_sign_SECRETKEYBYTES);
                std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
                crypto_sign_seed_keypair(pk.data(), outSk.data(), seed.data());
                sodium_memzero(seed.data(), seed.size());
                return true;
            }
        }

        return false;
    }

    void PrintUsage() {
        std::cout << "OmniStatsReleaseSigner - Ed25519 Release Manifest Signing Tool\n\n"
                  << "Usage:\n"
                  << "  OmniStatsReleaseSigner sign <manifest_path> <secret_key_path> <sig_output_path>\n"
                  << "    Requires whitespace-free manifest edges; writes a hexadecimal signature.\n"
                  << "  OmniStatsReleaseSigner verify <manifest_path> <sig_path> <public_key_hex>\n"
                  << "  OmniStatsReleaseSigner generate-key <secret_key_path>\n";
    }

} // namespace

int main(int argc, char* argv[]) {
    if (sodium_init() < 0) {
        std::cerr << "Error: Failed to initialize libsodium.\n";
        return 1;
    }

    if (argc < 2) {
        PrintUsage();
        return 1;
    }

    const std::string command = argv[1];

    if (command == "sign" || command == "--sign") {
        if (argc < 5) {
            std::cerr << "Error: sign requires <manifest_path> <secret_key_path> <sig_output_path>\n";
            return 1;
        }
        const std::string manifestPath = argv[2];
        const std::string keyPath = argv[3];
        const std::string sigPath = argv[4];

        const auto manifestBytes = ReadFileBytes(manifestPath);
        if (manifestBytes.empty()) {
            std::cerr << "Error: Manifest file is empty or cannot be opened: " << manifestPath << "\n";
            return 1;
        }
        const auto isWhitespace = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        const bool hasBom = manifestBytes.size() >= 3 &&
                            manifestBytes[0] == 0xef && manifestBytes[1] == 0xbb && manifestBytes[2] == 0xbf;
        if (hasBom || isWhitespace(manifestBytes.front()) || isWhitespace(manifestBytes.back())) {
            std::cerr << "Error: Manifest must omit BOM and leading or trailing whitespace for installed updater compatibility.\n";
            return 1;
        }

        std::vector<unsigned char> sk;
        if (!LoadSecretKey(keyPath, sk)) {
            std::cerr << "Error: Failed to load secret key from: " << keyPath << "\n";
            return 1;
        }

        std::vector<unsigned char> sig(crypto_sign_BYTES);
        unsigned long long sigLen = 0;
        if (crypto_sign_detached(sig.data(), &sigLen, manifestBytes.data(), manifestBytes.size(), sk.data()) != 0) {
            sodium_memzero(sk.data(), sk.size());
            std::cerr << "Error: crypto_sign_detached failed.\n";
            return 1;
        }
        sodium_memzero(sk.data(), sk.size());

        char sigHex[crypto_sign_BYTES * 2 + 1];
        sodium_bin2hex(sigHex, sizeof(sigHex), sig.data(), sig.size());
        if (!WriteFileBytes(sigPath, reinterpret_cast<const unsigned char*>(sigHex), sizeof(sigHex) - 1)) {
            std::cerr << "Error: Failed to write signature to: " << sigPath << "\n";
            return 1;
        }

        std::cout << "Successfully signed manifest. Signature written to: " << sigPath << "\n";
        return 0;
    }

    if (command == "verify" || command == "--verify") {
        if (argc < 5) {
            std::cerr << "Error: verify requires <manifest_path> <sig_path> <public_key_hex>\n";
            return 1;
        }
        const std::string manifestPath = argv[2];
        const std::string sigPath = argv[3];
        const std::string pubKeyHex = Trim(argv[4]);

        const auto manifestBytes = ReadFileBytes(manifestPath);
        if (manifestBytes.empty()) {
            std::cerr << "Error: Manifest file is empty or cannot be opened: " << manifestPath << "\n";
            return 1;
        }

        const auto sigBytes = ReadFileBytes(sigPath);
        if (sigBytes.empty()) {
            std::cerr << "Error: Signature file is empty or cannot be opened: " << sigPath << "\n";
            return 1;
        }

        std::vector<unsigned char> sig(crypto_sign_BYTES);
        if (sigBytes.size() == crypto_sign_BYTES) {
            std::memcpy(sig.data(), sigBytes.data(), crypto_sign_BYTES);
        } else {
            std::string sigHex(reinterpret_cast<const char*>(sigBytes.data()), sigBytes.size());
            sigHex = Trim(sigHex);
            size_t binLen = 0;
            if (sigHex.length() != crypto_sign_BYTES * 2 ||
                sodium_hex2bin(sig.data(), sig.size(), sigHex.data(), sigHex.length(),
                               nullptr, &binLen, nullptr) != 0 ||
                binLen != crypto_sign_BYTES) {
                std::cerr << "Error: Signature is neither 64 binary bytes nor 128 hex characters.\n";
                return 1;
            }
        }

        if (pubKeyHex.length() != crypto_sign_PUBLICKEYBYTES * 2) {
            std::cerr << "Error: Public key must be 64 hex characters (32 bytes).\n";
            return 1;
        }

        std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
        size_t binLen = 0;
        if (sodium_hex2bin(pk.data(), pk.size(), pubKeyHex.data(), pubKeyHex.length(),
                           nullptr, &binLen, nullptr) != 0 ||
            binLen != crypto_sign_PUBLICKEYBYTES) {
            std::cerr << "Error: Failed to decode public key hex.\n";
            return 1;
        }

        if (crypto_sign_verify_detached(sig.data(), manifestBytes.data(), manifestBytes.size(), pk.data()) != 0) {
            std::cerr << "Verification FAILED: Signature is invalid or does not match public key.\n";
            return 1;
        }

        std::cout << "Verification SUCCESS: Manifest signature is valid.\n";
        return 0;
    }

    if (command == "generate-key" || command == "--generate-key") {
        if (argc < 3) {
            std::cerr << "Error: generate-key requires <secret_key_output_path>\n";
            return 1;
        }
        const std::string keyPath = argv[2];

        // Refuse to overwrite existing key
        std::ifstream check(keyPath, std::ios::binary);
        if (check.is_open()) {
            std::cerr << "Error: Key file already exists at: " << keyPath << ". Refusing to overwrite.\n";
            return 1;
        }

        std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
        std::vector<unsigned char> sk(crypto_sign_SECRETKEYBYTES);
        crypto_sign_keypair(pk.data(), sk.data());

        if (!WriteFileBytes(keyPath, sk.data(), sk.size())) {
            sodium_memzero(sk.data(), sk.size());
            std::cerr << "Error: Failed to write secret key to: " << keyPath << "\n";
            return 1;
        }
        sodium_memzero(sk.data(), sk.size());

        std::vector<char> pkHex(crypto_sign_PUBLICKEYBYTES * 2 + 1);
        sodium_bin2hex(pkHex.data(), pkHex.size(), pk.data(), pk.size());

        std::cout << "Successfully generated new Ed25519 keypair.\n"
                  << "Secret key saved to: " << keyPath << "\n"
                  << "Public key hex:\n"
                  << pkHex.data() << "\n";
        return 0;
    }

    PrintUsage();
    return 1;
}
