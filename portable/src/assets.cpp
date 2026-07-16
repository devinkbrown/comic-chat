#include "comicchat/assets.hpp"

#include <algorithm>
#include <limits>
#include <new>

#include <zlib.h>

namespace comicchat {

auto inflate_asset(const std::span<const std::byte> compressed, const std::size_t output_limit)
    -> std::expected<std::vector<std::byte>, AssetError> {
    if (compressed.empty() || output_limit == 0) return std::unexpected{AssetError::invalid_stream};
    try {
        std::vector<std::byte> output(output_limit);
        z_stream stream{};
        if (inflateInit(&stream) != Z_OK) return std::unexpected{AssetError::allocation};
        std::size_t input_offset{};
        std::size_t output_offset{};
        int status = Z_OK;
        while (status == Z_OK) {
            if (stream.avail_in == 0 && input_offset < compressed.size()) {
                const auto amount = std::min(compressed.size() - input_offset,
                    static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
                stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(compressed.data() + input_offset));
                stream.avail_in = static_cast<uInt>(amount);
                input_offset += amount;
            }
            if (stream.avail_out == 0 && output_offset < output.size()) {
                const auto amount = std::min(output.size() - output_offset,
                    static_cast<std::size_t>(std::numeric_limits<uInt>::max()));
                stream.next_out = reinterpret_cast<Bytef*>(output.data() + output_offset);
                stream.avail_out = static_cast<uInt>(amount);
            }
            const auto before = stream.avail_out;
            status = inflate(&stream, Z_NO_FLUSH);
            output_offset += static_cast<std::size_t>(before - stream.avail_out);
            if (status == Z_STREAM_END) break;
            if (stream.avail_out == 0 && output_offset == output.size()) {
                inflateEnd(&stream);
                return std::unexpected{AssetError::output_limit};
            }
            if (status != Z_OK || (stream.avail_in == 0 && input_offset == compressed.size() && before == stream.avail_out)) {
                inflateEnd(&stream);
                return std::unexpected{AssetError::invalid_stream};
            }
        }
        inflateEnd(&stream);
        output.resize(output_offset);
        return output;
    } catch (const std::bad_alloc&) {
        return std::unexpected{AssetError::allocation};
    }
}

} // namespace comicchat
