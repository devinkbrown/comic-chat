#pragma once

#include "comicchat/cpp26.hpp"

#include <cstddef>
#include <expected>
#include <span>
#include <vector>

namespace comicchat {

enum class AssetError { invalid_stream, output_limit, allocation };

[[nodiscard]] auto inflate_asset(std::span<const std::byte> compressed, std::size_t output_limit)
    -> std::expected<std::vector<std::byte>, AssetError>;

} // namespace comicchat
