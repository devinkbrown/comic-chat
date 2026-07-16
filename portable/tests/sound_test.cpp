#include "comicchat/sound.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

namespace {

class TemporaryDirectory final {
public:
    TemporaryDirectory() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        path = std::filesystem::temp_directory_path() /
            ("comic-chat-sound-" + std::to_string(suffix));
        std::filesystem::create_directories(path);
    }
    ~TemporaryDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }
    std::filesystem::path path;
};

} // namespace

TEST_CASE("remote CTCP SOUND names are lexical basenames") {
    using comicchat::sound::ResolveError;
    using comicchat::sound::validate_name;

    const std::array rejected{
        "/nul/nul.wav",
        R"(\??\C:\sounds\alert.wav)",
        R"(\\server\share\alert.wav)",
        R"(C:\sounds\alert.wav)",
        "../alert.wav",
        R"(folder\alert.wav)",
        "folder/alert.wav",
        "NUL.wav",
        "NUL .wav",
        "nul.WAV",
        "CONIN$.wav",
        "CONOUT$.wav",
        "COM1.mid",
        "COM1 .wav",
        "LPT9.rmi",
        "COM¹.wav",
        "COM².wav",
        "COM³.wav",
        "LPT¹.wav",
        "LPT².wav",
        "LPT³.wav",
        "CON.txt.wav",
        "alert.wav.",
        "alert.wav ",
        "alert.exe",
    };
    for (const auto* name : rejected) {
        CAPTURE(name);
        REQUIRE_FALSE(validate_name(name));
    }
    const std::string embedded_nul{"alert\0.wav", 10};
    REQUIRE(validate_name(embedded_nul) ==
        std::unexpected{ResolveError::control_character});
    REQUIRE_FALSE(validate_name(std::string(129, 'a') + ".wav"));
    REQUIRE(validate_name("alert.wav"));
    REQUIRE(validate_name("welcome.MID"));
    REQUIRE(validate_name("theme.rmi"));
}

TEST_CASE("sound resolution requires a contained regular file") {
    using comicchat::sound::Format;
    using comicchat::sound::ResolveError;
    using comicchat::sound::resolve;

    TemporaryDirectory root;
    {
        std::ofstream file{root.path / "alert.wav", std::ios::binary};
        file << "RIFF";
    }
    auto resolved = resolve(root.path, "alert.wav");
    REQUIRE(resolved);
    CHECK(resolved->format == Format::wave);
    CHECK(resolved->display_name == "alert.wav");
    CHECK(std::filesystem::is_regular_file(resolved->path));
    CHECK(resolve(root.path, "missing.wav") == std::unexpected{ResolveError::not_found});
    std::filesystem::create_directory(root.path / "directory.wav");
    CHECK(resolve(root.path, "directory.wav") ==
        std::unexpected{ResolveError::not_regular_file});

    TemporaryDirectory outside;
    {
        std::ofstream file{outside.path / "escape.wav", std::ios::binary};
        file << "RIFF";
    }
    std::error_code error;
    std::filesystem::create_symlink(outside.path / "escape.wav", root.path / "escape.wav", error);
    if (!error) {
        CHECK(resolve(root.path, "escape.wav") ==
            std::unexpected{ResolveError::outside_root});
    }
}
