#pragma once

#include "comicchat/cpp26.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace comicchat {

enum class Security { tls, plaintext };

struct ConnectionConfig final {
    std::string host;
    std::uint16_t port{6697};
    std::string nickname;
    std::string channel;
    Security security{Security::tls};
    std::optional<std::string> ca_file;
};

[[nodiscard]] auto parse_connection_args(std::span<const std::string_view> args)
    -> std::optional<ConnectionConfig>;

} // namespace comicchat
