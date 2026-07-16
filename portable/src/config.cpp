#include "comicchat/config.hpp"

#include <charconv>
#include <vector>

namespace comicchat {
namespace {

auto parse_port(const std::string_view value) -> std::optional<std::uint16_t> {
    unsigned int parsed{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed > 65'535U) {
        return std::nullopt;
    }
    return static_cast<std::uint16_t>(parsed);
}

} // namespace

auto parse_connection_args(const std::span<const std::string_view> args)
    -> std::optional<ConnectionConfig> {
    std::vector<std::string_view> positional;
    ConnectionConfig result;
    for (std::size_t index = 0; index < args.size(); ++index) {
        const auto argument = args[index];
        if (argument == "--plaintext") {
            result.security = Security::plaintext;
        } else if (argument == "--tls") {
            result.security = Security::tls;
        } else if (argument == "--ca-file") {
            if (++index == args.size()) {
                return std::nullopt;
            }
            result.ca_file = std::string{args[index]};
        } else if (argument.starts_with("--ca-file=")) {
            const auto path = argument.substr(std::string_view{"--ca-file="}.size());
            if (path.empty()) {
                return std::nullopt;
            }
            result.ca_file = std::string{path};
        } else if (argument.starts_with("--")) {
            return std::nullopt;
        } else {
            positional.push_back(argument);
        }
    }
    if (positional.size() < 3 || positional.size() > 4) {
        return std::nullopt;
    }
    result.host = positional[0];
    if (positional.size() == 4) {
        const auto port = parse_port(positional[1]);
        if (!port) {
            return std::nullopt;
        }
        result.port = *port;
        result.nickname = positional[2];
        result.channel = positional[3];
    } else {
        result.nickname = positional[1];
        result.channel = positional[2];
    }
    return result;
}

} // namespace comicchat
