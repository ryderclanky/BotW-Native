#include "utf8_path.h"

#include <array>
#include <cstring>
#include <iostream>

namespace fs = std::filesystem;

namespace {
int failures = 0;

std::string bytes(std::u8string_view value) {
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

void expect(bool value, const char* message) {
    if (value) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}  // namespace

int main() {
    const std::string path_bytes = bytes(u8"BotW Native path test λ &");
    const fs::path root = fs::temp_directory_path() / botw::path_from_utf8(path_bytes);
    const fs::path file = root / botw::path_from_utf8(bytes(u8"folder with spaces")) /
                          botw::path_from_utf8(bytes(u8"save & marker λ.bin"));
    std::error_code ec;
    fs::remove_all(root, ec);
    ec.clear();
    fs::create_directories(file.parent_path(), ec);
    expect(!ec, "create directory with UTF-8 path");

    const char payload[] = "UTF-8 path round trip";
    if (FILE* out = botw::fopen_utf8(botw::path_to_utf8(file), "wb")) {
        expect(std::fwrite(payload, 1, sizeof(payload), out) == sizeof(payload), "write through UTF-8 path");
        std::fclose(out);
    } else {
        expect(false, "open file for write through UTF-8 path");
    }
    expect(fs::is_regular_file(file), "filesystem sees the Unicode-named file");
    expect(botw::path_to_utf8(file.filename()) == bytes(u8"save & marker λ.bin"), "path_to_utf8 preserves Unicode");

    std::array<char, sizeof(payload)> readback{};
    if (FILE* in = botw::fopen_utf8(botw::path_to_utf8(file), "rb")) {
        expect(std::fread(readback.data(), 1, readback.size(), in) == readback.size(), "read through UTF-8 path");
        std::fclose(in);
    } else {
        expect(false, "open file for read through UTF-8 path");
    }
    expect(std::memcmp(readback.data(), payload, sizeof(payload)) == 0, "UTF-8 file round trip contents");

#if defined(_WIN32)
    constexpr wchar_t kEnvName[] = L"BOTW_NATIVE_UTF8_PATH_TEST";
    constexpr wchar_t kEnvValue[] = L"BotW folder λ & data";
    expect(SetEnvironmentVariableW(kEnvName, kEnvValue) != 0, "set Unicode environment variable");
    expect(botw::getenv_utf8(kEnvName) == bytes(u8"BotW folder λ & data"), "read Unicode environment variable as UTF-8");
    SetEnvironmentVariableW(kEnvName, nullptr);
#else
    constexpr char kEnvName[] = "BOTW_NATIVE_UTF8_PATH_TEST";
    const std::string env_value = bytes(u8"BotW folder λ & data");
    expect(setenv(kEnvName, env_value.c_str(), 1) == 0, "set UTF-8 environment variable");
    expect(botw::getenv_utf8(kEnvName) == env_value, "read UTF-8 environment variable");
    unsetenv(kEnvName);
#endif

    fs::remove_all(root, ec);
    expect(!ec, "remove temporary UTF-8 path test directory");
    if (failures) return 1;
    std::cout << "UTF-8 path tests passed\n";
    return 0;
}
