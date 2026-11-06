#include "emit/json_writer.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace pr {

bool write_messages_json(const std::string& path, MessageSpool& spool,
                         const ResolvedModel& model, const JsonOptions& options,
                         const ParseLimits& limits, std::uint64_t& messages_written,
                         std::string& error) {
    namespace fs = std::filesystem;

    messages_written = 0;

    if (!spool.rewind(error)) {
        return false;
    }

    const fs::path target(path);
    if (target.has_parent_path() && !target.parent_path().empty()) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (ec) {
            error = "cannot create output directory: " + ec.message();
            return false;
        }
    }

    const fs::path temporary =
        target.parent_path() /
        (target.filename().string() + ".tmp." +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "cannot open temporary output file: " + temporary.string();
        return false;
    }

    const DecodeSchema schema = make_decode_schema(model);
    DecodeOptions decode_options;
    decode_options.hexdump_bytes = options.hexdump_bytes;

    while (true) {
        MessageRecord record;
        bool end = false;
        if (!spool.next(record, end, error)) {
            out.close();
            std::error_code ec;
            fs::remove(temporary, ec);
            return false;
        }
        if (end) {
            break;
        }

        MessageMetadata metadata;
        metadata.session = record.session;
        metadata.index = record.index;
        metadata.frame_type = record.frame_type;

        std::string line;
        render_message_json(record.payload, schema, metadata, decode_options, limits, line);
        out.write(line.data(), static_cast<std::streamsize>(line.size()));
        out.put('\n');
        if (!out) {
            out.close();
            std::error_code ec;
            fs::remove(temporary, ec);
            error = "cannot write output file: " + temporary.string();
            return false;
        }
        ++messages_written;
    }

    out.flush();
    if (!out) {
        out.close();
        std::error_code ec;
        fs::remove(temporary, ec);
        error = "cannot write output file: " + temporary.string();
        return false;
    }
    out.close();

    std::error_code ec;
    fs::rename(temporary, target, ec);
    if (ec) {
        error = "cannot replace output file: " + ec.message();
        std::error_code remove_ec;
        fs::remove(temporary, remove_ec);
        return false;
    }

    return true;
}

} // namespace pr
