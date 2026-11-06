#include "analysis/stream_analyzer.hpp"
#include "cli.hpp"
#include "decode/message_decoder.hpp"
#include "emit/json_writer.hpp"
#include "emit/message_spool.hpp"
#include "emit/proto_writer.hpp"
#include "framing/encoder.hpp"
#include "framing/learner.hpp"
#include "greeting/greeting.hpp"
#include "infer/schema.hpp"
#include "infer/type_resolver.hpp"
#include "net/session.hpp"
#include "wire/validator.hpp"

#include <boost/asio/buffer.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

int g_failures = 0;

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #condition "\n"; \
            ++g_failures;                                                                   \
        }                                                                                   \
    } while (false)

using pr::ByteSpan;

void put_varint(std::vector<std::uint8_t>& out, std::uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<std::uint8_t>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<std::uint8_t>(value));
}

void put_tag(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint8_t wire_type) {
    put_varint(out, (static_cast<std::uint64_t>(field) << 3) | wire_type);
}

void add_varint(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t value) {
    put_tag(out, field, 0);
    put_varint(out, value);
}

void add_fixed32(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint32_t value) {
    put_tag(out, field, 5);
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void add_fixed64(std::vector<std::uint8_t>& out, std::uint32_t field, std::uint64_t value) {
    put_tag(out, field, 1);
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

void add_bytes(std::vector<std::uint8_t>& out, std::uint32_t field,
               const std::vector<std::uint8_t>& value) {
    put_tag(out, field, 2);
    put_varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void add_string(std::vector<std::uint8_t>& out, std::uint32_t field, const std::string& value) {
    add_bytes(out, field, std::vector<std::uint8_t>(value.begin(), value.end()));
}

std::vector<std::uint8_t> frame_u32_be(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> frame_u32_le(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 24) & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> frame_varint(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    put_varint(out, payload.size());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> frame_grpc(const std::vector<std::uint8_t>& payload) {
    std::vector<std::uint8_t> out;
    out.push_back(0);
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((payload.size() >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(payload.size() & 0xff));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> frame_length(const std::vector<std::uint8_t>& payload,
                                       std::uint8_t width, bool big_endian,
                                       bool includes_header) {
    const std::uint64_t value =
        payload.size() + (includes_header ? static_cast<std::uint64_t>(width) : 0);
    std::vector<std::uint8_t> out(width);
    for (std::uint8_t i = 0; i < width; ++i) {
        const unsigned shift =
            big_endian ? static_cast<unsigned>((width - 1 - i) * 8)
                       : static_cast<unsigned>(i * 8);
        out[i] = static_cast<std::uint8_t>((value >> shift) & 0xff);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> frame_typed(const std::vector<std::uint8_t>& payload,
                                      std::uint64_t type, std::uint8_t type_width,
                                      bool type_big_endian, std::uint8_t length_width,
                                      bool length_big_endian, bool includes_header) {
    const std::size_t header =
        static_cast<std::size_t>(type_width) + static_cast<std::size_t>(length_width);
    const std::uint64_t value =
        payload.size() + (includes_header ? static_cast<std::uint64_t>(header) : 0);
    std::vector<std::uint8_t> out;

    auto put = [&](std::uint64_t v, std::uint8_t width, bool big_endian) {
        for (std::uint8_t i = 0; i < width; ++i) {
            const unsigned shift =
                big_endian ? static_cast<unsigned>((width - 1 - i) * 8)
                           : static_cast<unsigned>(i * 8);
            out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xff));
        }
    };

    put(type, type_width, type_big_endian);
    put(value, length_width, length_big_endian);
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> delimiter_safe_message(std::uint64_t value) {
    std::vector<std::uint8_t> payload;
    add_varint(payload, 1, 64 + value);
    return payload;
}

struct FramerRun {
    std::vector<std::vector<std::uint8_t>> frames;
    std::size_t consumed = 0;
    bool need_more = false;
    bool error = false;
    std::string message;
};

FramerRun run_framer(pr::Framer& framer, const std::vector<std::uint8_t>& data) {
    FramerRun run;
    std::size_t offset = 0;
    while (offset < data.size()) {
        const ByteSpan remaining(data.data() + offset, data.size() - offset);
        std::size_t consumed = 0;
        ByteSpan frame;
        std::string error;
        const pr::FrameStatus status = framer.next(remaining, consumed, frame, error);
        if (status == pr::FrameStatus::NeedMore) {
            run.need_more = true;
            break;
        }
        if (status == pr::FrameStatus::Error) {
            run.error = true;
            run.message = error;
            break;
        }
        if (consumed == 0) {
            run.error = true;
            run.message = "no progress";
            break;
        }
        run.frames.emplace_back(frame.begin(), frame.end());
        offset += consumed;
    }
    run.consumed = offset;
    return run;
}

std::vector<std::uint8_t> sample_message(std::uint64_t value) {
    std::vector<std::uint8_t> payload;
    add_varint(payload, 1, value);
    add_string(payload, 2, "hello");

    std::vector<std::uint8_t> nested;
    add_varint(nested, 1, value * 2);
    add_fixed32(nested, 2, static_cast<std::uint32_t>(value));
    add_bytes(payload, 3, nested);
    return payload;
}

pr::FramingSpec make_spec(pr::FramingKind kind, std::uint8_t width, bool big_endian) {
    pr::FramingSpec spec;
    spec.kind = kind;
    spec.width = width;
    spec.big_endian = big_endian;
    return spec;
}

void test_varint() {
    const std::uint64_t values[] = {0, 1, 127, 128, 300, 16384, 0xffffffffull,
                                    0xffffffffffffffffull};
    for (std::uint64_t value : values) {
        std::vector<std::uint8_t> data;
        put_varint(data, value);
        pr::WireReader reader(data);
        std::uint64_t decoded = 0;
        CHECK(reader.read_varint(decoded) == pr::ReadResult::Ok);
        CHECK(decoded == value);
        CHECK(reader.remaining() == 0);
    }
}

void test_parse_message() {
    const auto payload = sample_message(42);
    pr::ParsedMessage message;
    std::string error;
    CHECK(pr::parse_message(payload, message, pr::ParseLimits{}, 0, error));
    CHECK(message.fields.size() == 3);
    CHECK(message.fields[0].number == 1);
    CHECK(message.fields[0].wire_type == 0);
    CHECK(message.fields[0].varint == 42);
    CHECK(message.fields[1].number == 2);
    CHECK(message.fields[1].wire_type == 2);
    CHECK(message.fields[2].number == 3);
    CHECK(message.fields[2].wire_type == 2);
}

void test_framing(pr::FramingKind kind, std::uint8_t width, bool big_endian,
                  const std::string& label) {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 24; ++i) {
        const auto payload = sample_message(i);
        std::vector<std::uint8_t> framed;
        if (kind == pr::FramingKind::Varint) {
            framed = frame_varint(payload);
        } else if (kind == pr::FramingKind::Grpc) {
            framed = frame_grpc(payload);
        } else {
            framed = big_endian ? frame_u32_be(payload) : frame_u32_le(payload);
        }
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::FramingLearner learner(make_spec(pr::FramingKind::Auto, 4, true), 4096, 8,
                               std::chrono::milliseconds(1000), 1 << 20, 1 << 20,
                               pr::ParseLimits{});
    learner.contribute(1, stream);
    CHECK(learner.try_lock(false));
    CHECK(learner.locked());
    const bool varint_equivalent =
        kind == pr::FramingKind::Varint && learner.spec().kind == pr::FramingKind::Length &&
        learner.spec().width == 1;
    CHECK(learner.spec().kind == kind || varint_equivalent);
    if (kind == pr::FramingKind::Length) {
        CHECK(learner.spec().width == width);
        CHECK(learner.spec().big_endian == big_endian);
    }
    if (!(learner.spec().kind == kind || varint_equivalent)) {
        std::cerr << "framing test " << label << " selected "
                  << pr::to_string(learner.spec()) << "\n";
    }
}

void test_schema_and_proto() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 24; ++i) {
        const auto framed = frame_u32_be(sample_message(i));
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Length, 4, true);
    config.learning_bytes = 4096;
    config.learning_frames = 8;
    config.no_samples = false;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);
    analyzer.feed(stream);
    analyzer.finish();

    CHECK(learner.locked());
    CHECK(analyzer.frames_processed() == 24);
    CHECK(schema.root().fields.size() == 3);
    CHECK(schema.root().fields.at(1).varint.count == 24);
    CHECK(schema.root().fields.at(2).utf8_ok == 24);
    CHECK(schema.root().fields.at(3).nested_ok == 24);

    pr::ResolveOptions options;
    options.root_name = "ClientMessage";
    const pr::ResolvedModel model = pr::resolve_schema(schema.root(), options);
    pr::ProtoOptions proto_options;
    proto_options.package = "inferred";
    const std::string proto = pr::render_proto(model, proto_options);

    CHECK(proto.find("message ClientMessage") != std::string::npos);
    CHECK(proto.find("int32 field_1 = 1;") != std::string::npos);
    CHECK(proto.find("string field_2 = 2;") != std::string::npos);
    CHECK(proto.find("ClientMessage_Field3 field_3 = 3;") != std::string::npos);
    CHECK(proto.find("message ClientMessage_Field3") != std::string::npos);
}

void test_framing_spec_parsing() {
    std::string error;
    auto spec = pr::parse_framing("auto", error);
    CHECK(spec && spec->kind == pr::FramingKind::Auto);
    spec = pr::parse_framing("raw", error);
    CHECK(spec && spec->kind == pr::FramingKind::Raw);
    spec = pr::parse_framing("varint", error);
    CHECK(spec && spec->kind == pr::FramingKind::Varint);
    spec = pr::parse_framing("grpc", error);
    CHECK(spec && spec->kind == pr::FramingKind::Grpc);
    spec = pr::parse_framing("newline", error);
    CHECK(spec && spec->kind == pr::FramingKind::Newline);
    spec = pr::parse_framing("nul", error);
    CHECK(spec && spec->kind == pr::FramingKind::Nul);
    spec = pr::parse_framing("fixed:256", error);
    CHECK(spec && spec->kind == pr::FramingKind::Fixed && spec->fixed_size == 256);

    spec = pr::parse_framing("len:4:le:incl", error);
    CHECK(spec && spec->kind == pr::FramingKind::Length);
    if (spec) {
        CHECK(spec->width == 4);
        CHECK(!spec->big_endian);
        CHECK(spec->includes_header);
        CHECK(pr::to_string(*spec) == "len:4:le:incl");
    }

    spec = pr::parse_framing("u32be", error);
    CHECK(spec && spec->kind == pr::FramingKind::Length);
    if (spec) {
        CHECK(spec->width == 4);
        CHECK(spec->big_endian);
    }
    spec = pr::parse_framing("u16le", error);
    CHECK(spec && spec->kind == pr::FramingKind::Length);
    if (spec) {
        CHECK(spec->width == 2);
        CHECK(!spec->big_endian);
    }
    spec = pr::parse_framing("u8", error);
    CHECK(spec && spec->kind == pr::FramingKind::Length);
    if (spec) {
        CHECK(spec->width == 1);
    }

    spec = pr::parse_framing("type:4:be+len:4:le", error);
    CHECK(spec && spec->kind == pr::FramingKind::TypedLength);
    if (spec) {
        CHECK(spec->type_width == 4);
        CHECK(spec->type_big_endian);
        CHECK(spec->width == 4);
        CHECK(!spec->big_endian);
        CHECK(!spec->includes_header);
        CHECK(pr::to_string(*spec) == "type:4:be+len:4:le");
    }

    spec = pr::parse_framing("type:2:le+len:8:be:incl", error);
    CHECK(spec && spec->kind == pr::FramingKind::TypedLength);
    if (spec) {
        CHECK(spec->type_width == 2);
        CHECK(!spec->type_big_endian);
        CHECK(spec->width == 8);
        CHECK(spec->big_endian);
        CHECK(spec->includes_header);
        CHECK(pr::to_string(*spec) == "type:2:le+len:8:be:incl");
    }

    spec = pr::parse_framing("type4+len4:le", error);
    CHECK(spec && spec->kind == pr::FramingKind::TypedLength);
    if (spec) {
        CHECK(spec->type_width == 4);
        CHECK(spec->type_big_endian);
        CHECK(spec->width == 4);
        CHECK(!spec->big_endian);
        CHECK(!spec->includes_header);
        CHECK(pr::to_string(*spec) == "type:4:be+len:4:le");
    }

    spec = pr::parse_framing("type8+len2:be:payload", error);
    CHECK(spec && spec->kind == pr::FramingKind::TypedLength);
    if (spec) {
        CHECK(spec->type_width == 8);
        CHECK(spec->width == 2);
        CHECK(spec->big_endian);
        CHECK(!spec->includes_header);
    }

    CHECK(!pr::parse_framing("type:3:be+len:4:le", error));
    CHECK(!pr::parse_framing("type:4:xx+len:4:le", error));
    CHECK(!pr::parse_framing("type4+len4", error));
    CHECK(!pr::parse_framing("type:4:be+len:4:le:bogus", error));
    CHECK(!pr::parse_framing("len:3:be", error));
    CHECK(!error.empty());
    CHECK(!pr::parse_framing("bogus", error));
}

void test_random_data() {
    std::mt19937_64 rng(12345);
    std::vector<std::uint8_t> data(32768);
    for (auto& byte : data) {
        byte = static_cast<std::uint8_t>(rng());
    }

    pr::FramingLearner learner(make_spec(pr::FramingKind::Auto, 4, true), 4096, 8,
                               std::chrono::milliseconds(1000), 1 << 20, 1 << 20,
                               pr::ParseLimits{});
    learner.contribute(1, data);
    learner.force_lock();
    CHECK(learner.locked());
}

void test_length_framers() {
    struct Case {
        std::uint8_t width;
        bool big_endian;
        bool includes_header;
    };
    const Case cases[] = {
        {1, true, false},  {1, true, true},   {2, true, false},  {2, false, false},
        {2, true, true},   {4, true, false},  {4, false, false}, {4, true, true},
        {8, true, false},  {8, false, false}, {8, true, true},
    };

    for (const Case& test : cases) {
        pr::FramingSpec spec;
        spec.kind = pr::FramingKind::Length;
        spec.width = test.width;
        spec.big_endian = test.big_endian;
        spec.includes_header = test.includes_header;

        std::vector<std::vector<std::uint8_t>> expected;
        std::vector<std::uint8_t> stream;
        for (std::uint64_t i = 1; i <= 12; ++i) {
            const auto payload = sample_message(i);
            expected.push_back(payload);
            const auto framed =
                frame_length(payload, test.width, test.big_endian, test.includes_header);
            stream.insert(stream.end(), framed.begin(), framed.end());
        }

        auto framer = pr::make_framer(spec, 1 << 20);
        CHECK(framer != nullptr);
        if (!framer) {
            continue;
        }
        const FramerRun run = run_framer(*framer, stream);
        CHECK(!run.error);
        CHECK(!run.need_more);
        CHECK(run.consumed == stream.size());
        CHECK(run.frames.size() == expected.size());
        for (std::size_t i = 0; i < std::min(run.frames.size(), expected.size()); ++i) {
            CHECK(run.frames[i] == expected[i]);
        }

        // A truncated final frame must be reported as NeedMore, not as an error.
        std::vector<std::uint8_t> partial(stream.begin(),
                                          stream.end() - static_cast<std::ptrdiff_t>(1));
        auto partial_framer = pr::make_framer(spec, 1 << 20);
        CHECK(partial_framer != nullptr);
        if (partial_framer) {
            const FramerRun partial_run = run_framer(*partial_framer, partial);
            CHECK(!partial_run.error);
            CHECK(partial_run.need_more);
            CHECK(partial_run.frames.size() + 1 == expected.size());
        }
    }
}

void test_delimiter_framers() {
    struct Case {
        pr::FramingKind kind;
        std::uint8_t delimiter;
        const char* label;
    };
    const Case cases[] = {
        {pr::FramingKind::Newline, '\n', "newline"},
        {pr::FramingKind::Nul, '\0', "nul"},
    };

    for (const Case& test : cases) {
        pr::FramingSpec spec;
        spec.kind = test.kind;

        std::vector<std::vector<std::uint8_t>> expected;
        std::vector<std::uint8_t> stream;
        for (std::uint64_t i = 1; i <= 12; ++i) {
            const auto payload = delimiter_safe_message(i);
            expected.push_back(payload);
            stream.insert(stream.end(), payload.begin(), payload.end());
            stream.push_back(test.delimiter);
        }

        auto framer = pr::make_framer(spec, 1 << 20);
        CHECK(framer != nullptr);
        if (!framer) {
            continue;
        }
        const FramerRun run = run_framer(*framer, stream);
        CHECK(!run.error);
        CHECK(!run.need_more);
        CHECK(run.frames.size() == expected.size());
        for (std::size_t i = 0; i < std::min(run.frames.size(), expected.size()); ++i) {
            CHECK(run.frames[i] == expected[i]);
        }
        if (run.frames.size() != expected.size()) {
            std::cerr << test.label << " framing produced " << run.frames.size()
                      << " frames, expected " << expected.size() << "\n";
        }
    }
}

void test_fixed_framer() {
    const auto payload = delimiter_safe_message(7);
    pr::FramingSpec spec;
    spec.kind = pr::FramingKind::Fixed;
    spec.fixed_size = payload.size();

    std::vector<std::uint8_t> stream;
    std::vector<std::vector<std::uint8_t>> expected;
    for (std::uint64_t i = 1; i <= 10; ++i) {
        const auto frame = delimiter_safe_message(i);
        CHECK(frame.size() == payload.size());
        expected.push_back(frame);
        stream.insert(stream.end(), frame.begin(), frame.end());
    }

    auto framer = pr::make_framer(spec, 1 << 20);
    CHECK(framer != nullptr);
    if (framer) {
        const FramerRun run = run_framer(*framer, stream);
        CHECK(!run.error);
        CHECK(!run.need_more);
        CHECK(run.frames == expected);
    }
}

void test_typed_framers() {
    struct Case {
        std::uint8_t type_width;
        bool type_big_endian;
        std::uint8_t length_width;
        bool length_big_endian;
        bool includes_header;
    };
    const Case cases[] = {
        {4, true, 4, false, false}, {4, false, 4, true, false},
        {4, true, 4, true, true},   {2, true, 8, false, false},
        {1, true, 4, false, true},  {8, false, 2, true, false},
    };

    for (const Case& test : cases) {
        pr::FramingSpec spec;
        spec.kind = pr::FramingKind::TypedLength;
        spec.type_width = test.type_width;
        spec.type_big_endian = test.type_big_endian;
        spec.width = test.length_width;
        spec.big_endian = test.length_big_endian;
        spec.includes_header = test.includes_header;

        std::vector<std::vector<std::uint8_t>> expected;
        std::vector<std::uint8_t> stream;
        for (std::uint64_t i = 1; i <= 12; ++i) {
            const auto payload = sample_message(i);
            expected.push_back(payload);
            const std::uint64_t type = (i * 37) & 0xff;
            const auto framed = frame_typed(payload, type, test.type_width,
                                            test.type_big_endian, test.length_width,
                                            test.length_big_endian, test.includes_header);
            stream.insert(stream.end(), framed.begin(), framed.end());
        }

        auto framer = pr::make_framer(spec, 1 << 20);
        CHECK(framer != nullptr);
        if (!framer) {
            continue;
        }
        const FramerRun run = run_framer(*framer, stream);
        CHECK(!run.error);
        CHECK(!run.need_more);
        CHECK(run.consumed == stream.size());
        CHECK(run.frames == expected);

        // A truncated final frame must be reported as NeedMore, not as an error.
        std::vector<std::uint8_t> partial(stream.begin(),
                                          stream.end() - static_cast<std::ptrdiff_t>(1));
        auto partial_framer = pr::make_framer(spec, 1 << 20);
        CHECK(partial_framer != nullptr);
        if (partial_framer) {
            const FramerRun partial_run = run_framer(*partial_framer, partial);
            CHECK(!partial_run.error);
            CHECK(partial_run.need_more);
            CHECK(partial_run.frames.size() + 1 == expected.size());
        }
    }

    // The length field must not be smaller than the typed header when it is
    // declared to include that header.
    pr::FramingSpec too_small;
    too_small.kind = pr::FramingKind::TypedLength;
    too_small.type_width = 4;
    too_small.width = 4;
    too_small.includes_header = true;
    std::vector<std::uint8_t> invalid(8, 0);
    auto framer = pr::make_framer(too_small, 1 << 20);
    CHECK(framer != nullptr);
    if (framer) {
        const FramerRun run = run_framer(*framer, invalid);
        CHECK(run.error);
    }
}

void test_typed_auto_detection() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        const auto framed = frame_typed(sample_message(i), 100 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::FramingLearner learner(make_spec(pr::FramingKind::Auto, 4, true), 4096, 8,
                               std::chrono::milliseconds(1000), 1 << 20, 1 << 20,
                               pr::ParseLimits{});
    learner.contribute(1, stream);
    CHECK(learner.try_lock(false));
    CHECK(learner.locked());
    CHECK(learner.spec().kind == pr::FramingKind::TypedLength);
    CHECK(learner.spec().type_width == 4);
    CHECK(learner.spec().width == 4);
    CHECK(!learner.spec().big_endian);
    CHECK(!learner.spec().includes_header);
    CHECK(learner.spec().type_value == 0);
}

void test_typed_stream_analyzer() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        const auto framed = frame_typed(sample_message(i), 200 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing.kind = pr::FramingKind::TypedLength;
    config.framing.type_width = 4;
    config.framing.type_big_endian = true;
    config.framing.width = 4;
    config.framing.big_endian = false;
    config.framing.includes_header = false;
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);

    std::size_t offset = 0;
    const std::size_t chunks[] = {3, 11, 7, 19, 5};
    std::size_t chunk_index = 0;
    while (offset < stream.size()) {
        const std::size_t take =
            std::min(chunks[chunk_index % std::size(chunks)], stream.size() - offset);
        analyzer.feed(ByteSpan(stream.data() + offset, take));
        offset += take;
        ++chunk_index;
    }
    analyzer.finish();

    CHECK(!analyzer.fatal_error());
    CHECK(analyzer.frames_processed() == 12);
    CHECK(schema.root().fields.count(1) == 1);
    CHECK(schema.root().fields.count(2) == 1);
    CHECK(schema.root().fields.count(3) == 1);
    CHECK(schema.root().fields.at(1).varint.count == 12);
}

void test_auto_length_prefixes() {
    struct Case {
        std::uint8_t width;
        bool big_endian;
        bool includes_header;
    };
    const Case cases[] = {
        {2, true, false},
        {2, false, false},
        {4, true, true},
        {8, true, false},
        {8, false, false},
    };

    for (const Case& test : cases) {
        std::vector<std::uint8_t> stream;
        for (std::uint64_t i = 1; i <= 40; ++i) {
            const auto framed = frame_length(sample_message(i), test.width,
                                             test.big_endian, test.includes_header);
            stream.insert(stream.end(), framed.begin(), framed.end());
        }

        pr::FramingLearner learner(make_spec(pr::FramingKind::Auto, 4, true), 8192, 8,
                                   std::chrono::milliseconds(1000), 1 << 20, 1 << 20,
                                   pr::ParseLimits{});
        learner.contribute(1, stream);
        CHECK(learner.try_lock(false));
        CHECK(learner.locked());
        CHECK(learner.spec().kind == pr::FramingKind::Length);
        CHECK(learner.spec().width == test.width);
        CHECK(learner.spec().big_endian == test.big_endian);
        CHECK(learner.spec().includes_header == test.includes_header);
        if (learner.spec().kind != pr::FramingKind::Length ||
            learner.spec().width != test.width ||
            learner.spec().big_endian != test.big_endian) {
            std::cerr << "auto framing selected " << pr::to_string(learner.spec())
                      << " for len:" << static_cast<int>(test.width) << ":"
                      << (test.big_endian ? "be" : "le") << "\n";
        }
    }
}

void test_chunked_stream() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 40; ++i) {
        const auto framed = frame_u32_be(sample_message(i));
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Length, 4, true);
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);

    const std::size_t chunk_sizes[] = {1, 2, 5, 13, 64, 3, 17};
    std::size_t offset = 0;
    std::size_t chunk_index = 0;
    while (offset < stream.size()) {
        const std::size_t take =
            std::min(chunk_sizes[chunk_index % std::size(chunk_sizes)],
                     stream.size() - offset);
        analyzer.feed(ByteSpan(stream.data() + offset, take));
        offset += take;
        ++chunk_index;
    }
    analyzer.finish();

    CHECK(!analyzer.fatal_error());
    CHECK(analyzer.frames_processed() == 40);
    CHECK(schema.root().message_count == 40);
    CHECK(schema.root().fields.size() == 3);
    CHECK(schema.root().fields.at(1).varint.count == 40);
    CHECK(schema.root().fields.at(2).utf8_ok == 40);
    CHECK(schema.root().fields.at(3).nested_ok == 40);
}

void test_multiple_sessions() {
    std::vector<std::uint8_t> stream_a;
    std::vector<std::uint8_t> stream_b;
    for (std::uint64_t i = 1; i <= 15; ++i) {
        const auto framed = frame_u32_be(sample_message(i));
        stream_a.insert(stream_a.end(), framed.begin(), framed.end());
    }
    for (std::uint64_t i = 16; i <= 35; ++i) {
        const auto framed = frame_u32_be(sample_message(i));
        stream_b.insert(stream_b.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Auto, 4, true);
    config.learning_bytes = 2048;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);

    {
        pr::StreamAnalyzer analyzer_a(1, learner, schema, config);
        std::size_t offset = 0;
        while (offset < stream_a.size()) {
            const std::size_t take = std::min<std::size_t>(7, stream_a.size() - offset);
            analyzer_a.feed(ByteSpan(stream_a.data() + offset, take));
            offset += take;
        }
        analyzer_a.finish();
        CHECK(analyzer_a.frames_processed() == 15);
    }

    CHECK(learner.locked());
    CHECK(learner.spec().kind == pr::FramingKind::Length);
    CHECK(learner.spec().width == 4);
    CHECK(learner.spec().big_endian);
    const pr::FramingSpec locked_spec = learner.spec();

    {
        pr::StreamAnalyzer analyzer_b(2, learner, schema, config);
        std::size_t offset = 0;
        while (offset < stream_b.size()) {
            const std::size_t take = std::min<std::size_t>(11, stream_b.size() - offset);
            analyzer_b.feed(ByteSpan(stream_b.data() + offset, take));
            offset += take;
        }
        analyzer_b.finish();
        CHECK(analyzer_b.frames_processed() == 20);
    }

    CHECK(learner.spec() == locked_spec);
    CHECK(schema.root().message_count == 35);
    CHECK(schema.root().fields.size() == 3);
    CHECK(schema.root().fields.at(1).varint.count == 35);
    CHECK(schema.root().fields.at(2).utf8_ok == 35);
    CHECK(schema.root().fields.at(3).nested_ok == 35);
}

const pr::ResolvedField* find_root_field(const pr::ResolvedModel& model,
                                         std::uint32_t number) {
    const pr::ResolvedMessage* root = nullptr;
    for (const pr::ResolvedMessage& message : model.messages) {
        if (message.name == model.root_name) {
            root = &message;
            break;
        }
    }
    if (root == nullptr && !model.messages.empty()) {
        root = &model.messages.back();
    }
    if (root == nullptr) {
        return nullptr;
    }
    for (const pr::ResolvedField& field : root->fields) {
        if (field.number == number) {
            return &field;
        }
    }
    return nullptr;
}

void test_types_and_proto2() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        std::vector<std::uint8_t> payload;
        add_varint(payload, 1, i % 2);                            // bool-like 0/1
        add_bytes(payload, 4, std::vector<std::uint8_t>{1, 2, 3}); // packed varints

        const std::vector<std::uint8_t> packed_fixed32(16, 0xff);
        add_bytes(payload, 5, packed_fixed32);
        if (i == 1) {
            add_fixed32(payload, 5, 0xffffffffu);
        }

        const std::vector<std::uint8_t> packed_fixed64(32, 0xff);
        add_bytes(payload, 6, packed_fixed64);
        if (i == 1) {
            add_fixed64(payload, 6, 0xffffffffffffffffull);
        }

        const auto framed = frame_u32_be(payload);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Length, 4, true);
    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);
    analyzer.feed(stream);
    analyzer.finish();
    CHECK(analyzer.frames_processed() == 20);

    pr::ResolveOptions options;
    options.root_name = "ClientMessage";
    const pr::ResolvedModel model = pr::resolve_schema(schema.root(), options);

    const pr::ResolvedField* boolean = find_root_field(model, 1);
    CHECK(boolean != nullptr);
    if (boolean) {
        CHECK(boolean->type == "bool");
        CHECK(!boolean->repeated);
        CHECK(boolean->comment.find("int32 is also possible") != std::string::npos);
    }

    const pr::ResolvedField* packed_varints = find_root_field(model, 4);
    CHECK(packed_varints != nullptr);
    if (packed_varints) {
        CHECK(packed_varints->type == "int32");
        CHECK(packed_varints->repeated);
        CHECK(packed_varints->packed);
    }

    const pr::ResolvedField* packed_fixed32 = find_root_field(model, 5);
    CHECK(packed_fixed32 != nullptr);
    if (packed_fixed32) {
        CHECK(packed_fixed32->type == "fixed32");
        CHECK(packed_fixed32->repeated);
        CHECK(packed_fixed32->packed);
    }

    const pr::ResolvedField* packed_fixed64 = find_root_field(model, 6);
    CHECK(packed_fixed64 != nullptr);
    if (packed_fixed64) {
        CHECK(packed_fixed64->type == "fixed64");
        CHECK(packed_fixed64->repeated);
        CHECK(packed_fixed64->packed);
    }

    pr::ResolveOptions proto2_options;
    proto2_options.root_name = "ClientMessage";
    proto2_options.proto2 = true;
    const pr::ResolvedModel proto2_model = pr::resolve_schema(schema.root(), proto2_options);

    pr::ProtoOptions proto_options;
    proto_options.proto2 = true;
    proto_options.package = "inferred";
    const std::string proto = pr::render_proto(proto2_model, proto_options);
    CHECK(proto.find("syntax = \"proto2\";") != std::string::npos);
    CHECK(proto.find("optional bool field_1 = 1;") != std::string::npos);
    CHECK(proto.find("repeated int32 field_4 = 4 [packed = true];") != std::string::npos);
    CHECK(proto.find("repeated fixed32 field_5 = 5 [packed = true];") != std::string::npos);
    CHECK(proto.find("repeated fixed64 field_6 = 6 [packed = true];") != std::string::npos);
}

void test_pure_packed_fixed_stats() {
    pr::MessageSchema root;
    root.message_count = 5;

    pr::FieldStats& fixed32 = root.fields[7];
    fixed32.number = 7;
    fixed32.wire_counts[2] = 5;
    fixed32.occurrences = 5;
    fixed32.messages_seen = 5;
    fixed32.len_total = 5;
    fixed32.len_min = 16;
    fixed32.len_max = 16;
    fixed32.packed_fixed32_attempts = 5;
    fixed32.packed_fixed32_ok = 5;
    for (int message = 0; message < 5; ++message) {
        for (int value = 0; value < 4; ++value) {
            fixed32.packed_fixed32.add(0xffffffffu);
        }
    }

    pr::FieldStats& fixed64 = root.fields[8];
    fixed64.number = 8;
    fixed64.wire_counts[2] = 5;
    fixed64.occurrences = 5;
    fixed64.messages_seen = 5;
    fixed64.len_total = 5;
    fixed64.len_min = 32;
    fixed64.len_max = 32;
    fixed64.packed_fixed64_attempts = 5;
    fixed64.packed_fixed64_ok = 5;
    for (int message = 0; message < 5; ++message) {
        for (int value = 0; value < 4; ++value) {
            fixed64.packed_fixed64.add(0xffffffffffffffffull);
        }
    }

    pr::ResolveOptions options;
    const pr::ResolvedModel model = pr::resolve_schema(root, options);
    const pr::ResolvedField* field32 = find_root_field(model, 7);
    const pr::ResolvedField* field64 = find_root_field(model, 8);
    CHECK(field32 != nullptr);
    CHECK(field64 != nullptr);
    if (field32) {
        CHECK(field32->type == "fixed32");
        CHECK(field32->packed);
    }
    if (field64) {
        CHECK(field64->type == "fixed64");
        CHECK(field64->packed);
    }
}

void test_raw_mode() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 20; ++i) {
        const auto payload = sample_message(i);
        stream.insert(stream.end(), payload.begin(), payload.end());
    }

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Raw, 4, true);
    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);

    std::size_t offset = 0;
    const std::size_t chunk_sizes[] = {3, 11, 29, 7};
    std::size_t chunk_index = 0;
    while (offset < stream.size()) {
        const std::size_t take =
            std::min(chunk_sizes[chunk_index % std::size(chunk_sizes)],
                     stream.size() - offset);
        analyzer.feed(ByteSpan(stream.data() + offset, take));
        offset += take;
        ++chunk_index;
    }
    analyzer.finish();

    CHECK(!analyzer.fatal_error());
    CHECK(analyzer.frames_processed() == 60);
    CHECK(schema.root().fields.size() == 3);
    CHECK(schema.root().fields.at(1).occurrences == 20);
    CHECK(schema.root().fields.at(1).messages_seen == 1);

    pr::ResolveOptions options;
    options.root_name = "ClientMessage";
    options.raw_mode = true;
    const pr::ResolvedModel model = pr::resolve_schema(schema.root(), options);
    CHECK(model.raw_mode);
    const pr::ResolvedMessage* root_message = nullptr;
    for (const pr::ResolvedMessage& message : model.messages) {
        if (message.name == model.root_name) {
            root_message = &message;
            break;
        }
    }
    CHECK(root_message != nullptr);
    if (root_message != nullptr) {
        CHECK(root_message->comment.find("RAW mode") != std::string::npos);
    }

    const pr::ResolvedField* field = find_root_field(model, 1);
    CHECK(field != nullptr);
    if (field) {
        CHECK(field->repeated);
        CHECK(field->comment.find("RAW mode") != std::string::npos);
        CHECK(field->comment.find("cardinality is unreliable") != std::string::npos);
    }

    pr::ProtoOptions proto_options;
    proto_options.package = "inferred";
    const std::string proto = pr::render_proto(model, proto_options);
    CHECK(proto.find("WARNING: RAW mode") != std::string::npos);
}

void test_frame_encoder() {
    const std::vector<std::uint8_t> payload = sample_message(5);

    auto check = [&](const pr::FramingSpec& spec, const std::vector<std::uint8_t>& expected) {
        std::vector<std::uint8_t> out;
        std::string error;
        const bool ok = pr::encode_frame(spec, ByteSpan(payload.data(), payload.size()), out,
                                         error);
        CHECK(ok);
        CHECK(out == expected);
        if (!ok) {
            std::cerr << "frame encoder failed for " << pr::to_string(spec) << ": " << error
                      << "\n";
        }
    };

    pr::FramingSpec raw;
    raw.kind = pr::FramingKind::Raw;
    check(raw, payload);

    const std::uint8_t widths[] = {1, 2, 4, 8};
    for (std::uint8_t width : widths) {
        for (bool big_endian : {true, false}) {
            for (bool includes_header : {true, false}) {
                pr::FramingSpec spec;
                spec.kind = pr::FramingKind::Length;
                spec.width = width;
                spec.big_endian = big_endian;
                spec.includes_header = includes_header;
                check(spec, frame_length(payload, width, big_endian, includes_header));
            }
        }
    }

    for (std::uint8_t type_width : widths) {
        for (bool type_big_endian : {true, false}) {
            const std::uint64_t type =
                type_width == 1   ? 0x5a
                : type_width == 2 ? 0x1234
                : type_width == 4 ? 0x01020304ull
                                  : 0x0102030405060708ull;
            for (std::uint8_t length_width : widths) {
                for (bool length_big_endian : {true, false}) {
                    for (bool includes_header : {true, false}) {
                        pr::FramingSpec spec;
                        spec.kind = pr::FramingKind::TypedLength;
                        spec.type_width = type_width;
                        spec.type_big_endian = type_big_endian;
                        spec.type_value = type;
                        spec.width = length_width;
                        spec.big_endian = length_big_endian;
                        spec.includes_header = includes_header;
                        check(spec, frame_typed(payload, type, type_width, type_big_endian,
                                                length_width, length_big_endian,
                                                includes_header));
                    }
                }
            }
        }
    }

    pr::FramingSpec varint;
    varint.kind = pr::FramingKind::Varint;
    check(varint, frame_varint(payload));

    pr::FramingSpec grpc;
    grpc.kind = pr::FramingKind::Grpc;
    check(grpc, frame_grpc(payload));

    pr::FramingSpec newline;
    newline.kind = pr::FramingKind::Newline;
    std::vector<std::uint8_t> newline_expected = payload;
    newline_expected.push_back('\n');
    check(newline, newline_expected);

    pr::FramingSpec nul;
    nul.kind = pr::FramingKind::Nul;
    std::vector<std::uint8_t> nul_expected = payload;
    nul_expected.push_back('\0');
    check(nul, nul_expected);

    pr::FramingSpec fixed;
    fixed.kind = pr::FramingKind::Fixed;
    fixed.fixed_size = payload.size();
    check(fixed, payload);

    std::vector<std::uint8_t> out;
    std::string error;
    pr::FramingSpec auto_spec;
    auto_spec.kind = pr::FramingKind::Auto;
    CHECK(!pr::encode_frame(auto_spec, ByteSpan(payload.data(), payload.size()), out, error));
    CHECK(!error.empty());

    pr::FramingSpec bad_fixed = fixed;
    bad_fixed.fixed_size = payload.size() + 1;
    CHECK(!pr::encode_frame(bad_fixed, ByteSpan(payload.data(), payload.size()), out, error));
    CHECK(!error.empty());

    std::vector<std::uint8_t> large(300, 0);
    pr::FramingSpec narrow;
    narrow.kind = pr::FramingKind::Length;
    narrow.width = 1;
    CHECK(!pr::encode_frame(narrow, ByteSpan(large.data(), large.size()), out, error));
    CHECK(!error.empty());
}

void test_greeting_fields() {
    pr::GreetingConfig config;
    config.fields = {"4:int32=2", "5:int32=0", "6:int32=2", "2:string=abc",
                     "3:string=vendor"};
    config.framing = make_spec(pr::FramingKind::Length, 4, true);

    std::vector<std::uint8_t> wire;
    std::string error;
    CHECK(pr::build_greeting_wire(config, 1024, wire, error));

    std::vector<std::uint8_t> expected_payload;
    add_varint(expected_payload, 4, 2);
    add_varint(expected_payload, 5, 0);
    add_varint(expected_payload, 6, 2);
    add_string(expected_payload, 2, "abc");
    add_string(expected_payload, 3, "vendor");
    CHECK(wire == frame_u32_be(expected_payload));

    pr::GreetingConfig typed_config;
    typed_config.hex = "08 01";
    pr::FramingSpec typed_framing;
    typed_framing.kind = pr::FramingKind::TypedLength;
    typed_framing.type_width = 4;
    typed_framing.type_big_endian = true;
    typed_framing.width = 4;
    typed_framing.big_endian = false;
    typed_framing.includes_header = false;
    typed_config.framing = typed_framing;
    typed_config.message_type = 0x01020304;
    CHECK(pr::build_greeting_wire(typed_config, 1024, wire, error));
    const std::vector<std::uint8_t> typed_payload{0x08, 0x01};
    CHECK(wire == frame_typed(typed_payload, 0x01020304, 4, true, 4, false, false));

    pr::GreetingConfig hex_config;
    hex_config.hex = "0x20 02";
    hex_config.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(pr::build_greeting_wire(hex_config, 1024, wire, error));
    CHECK((wire == std::vector<std::uint8_t>{0x20, 0x02}));

    const std::string path = "/tmp/protoreverse_greeting_test.bin";
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.put('\x08');
        file.put('\x01');
    }
    pr::GreetingConfig file_config;
    file_config.file = path;
    file_config.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(pr::build_greeting_wire(file_config, 1024, wire, error));
    CHECK((wire == std::vector<std::uint8_t>{0x08, 0x01}));
    (void)std::remove(path.c_str());

    pr::GreetingConfig negative;
    negative.fields = {"1:int32=-1"};
    negative.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(pr::build_greeting_wire(negative, 1024, wire, error));
    std::vector<std::uint8_t> expected_negative;
    add_varint(expected_negative, 1, static_cast<std::uint64_t>(static_cast<std::int64_t>(-1)));
    CHECK(wire == expected_negative);

    pr::GreetingConfig conflicting;
    conflicting.hex = "00";
    conflicting.fields = {"1:int32=1"};
    conflicting.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(!pr::build_greeting_wire(conflicting, 1024, wire, error));

    pr::GreetingConfig missing_framing;
    missing_framing.fields = {"1:int32=1"};
    CHECK(!pr::build_greeting_wire(missing_framing, 1024, wire, error));

    pr::GreetingConfig auto_framing;
    auto_framing.fields = {"1:int32=1"};
    auto_framing.framing = make_spec(pr::FramingKind::Auto, 4, true);
    auto_framing.message_type = 7;
    auto_framing.message_type_set = true;
    CHECK(!pr::build_greeting_wire(auto_framing, 1024, wire, error));

    pr::FramingSpec learned;
    learned.kind = pr::FramingKind::TypedLength;
    learned.type_width = 4;
    learned.type_big_endian = true;
    learned.width = 4;
    learned.big_endian = false;
    pr::FramingSpec resolved;
    CHECK(pr::resolve_greeting_framing(auto_framing, learned, resolved, error));
    CHECK(resolved.kind == pr::FramingKind::TypedLength);
    CHECK(resolved.type_value == 7);

    const std::vector<std::uint8_t> auto_payload{0x08, 0x01};
    CHECK(pr::encode_greeting_wire(auto_framing, resolved,
                                   ByteSpan(auto_payload.data(), auto_payload.size()),
                                   wire, error));
    CHECK(wire == frame_typed(auto_payload, 7, 4, true, 4, false, false));

    pr::FramingSpec raw_learned = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(!pr::resolve_greeting_framing(auto_framing, raw_learned, resolved, error));
    pr::FramingSpec auto_learned = make_spec(pr::FramingKind::Auto, 4, true);
    CHECK(!pr::resolve_greeting_framing(auto_framing, auto_learned, resolved, error));
    CHECK(pr::resolve_greeting_framing(config, raw_learned, resolved, error));
    CHECK(resolved.kind == pr::FramingKind::Length);

    pr::FramingSpec unresolved = make_spec(pr::FramingKind::Auto, 4, true);
    CHECK(!pr::encode_greeting_wire(config, unresolved,
                                    ByteSpan(auto_payload.data(), auto_payload.size()),
                                    wire, error));

    pr::GreetingConfig bad_type;
    bad_type.fields = {"1:not_a_type=1"};
    bad_type.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(!pr::build_greeting_wire(bad_type, 1024, wire, error));

    pr::GreetingConfig empty_hex;
    empty_hex.hex = "0x";
    empty_hex.framing = make_spec(pr::FramingKind::Raw, 4, true);
    CHECK(!pr::build_greeting_wire(empty_hex, 1024, wire, error));
}

bool parse_cli(std::initializer_list<const char*> args, pr::Config& config) {
    std::vector<std::string> storage;
    storage.reserve(args.size());
    for (const char* arg : args) {
        storage.emplace_back(arg);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& arg : storage) {
        argv.push_back(arg.data());
    }
    try {
        config = pr::parse_args(static_cast<int>(argv.size()), argv.data());
        return true;
    } catch (const pr::CliError&) {
        return false;
    }
}

void test_frame_probe() {
    const auto payload = sample_message(7);
    const auto framed = frame_u32_be(payload);
    const ByteSpan data(framed.data(), framed.size());

    const auto probe = pr::probe_frame(make_spec(pr::FramingKind::Length, 4, true), data, 1024);
    CHECK(probe.header_ready);
    CHECK(probe.valid);
    CHECK(probe.header_size == 4);
    CHECK(probe.declared_length == payload.size());
    CHECK(probe.total_size == framed.size());

    pr::FramingSpec includes_header = make_spec(pr::FramingKind::Length, 4, true);
    includes_header.includes_header = true;
    const auto incl_framed = frame_length(payload, 4, true, true);
    const auto incl_probe =
        pr::probe_frame(includes_header, ByteSpan(incl_framed.data(), incl_framed.size()), 1024);
    CHECK(incl_probe.header_ready);
    CHECK(incl_probe.valid);
    CHECK(incl_probe.declared_length == payload.size());
    CHECK(incl_probe.total_size == incl_framed.size());

    pr::FramingSpec typed;
    typed.kind = pr::FramingKind::TypedLength;
    typed.type_width = 4;
    typed.type_big_endian = true;
    typed.width = 4;
    typed.big_endian = false;
    const auto typed_framed = frame_typed(payload, 0x01020304, 4, true, 4, false, false);
    const auto typed_probe =
        pr::probe_frame(typed, ByteSpan(typed_framed.data(), typed_framed.size()), 1024);
    CHECK(typed_probe.header_ready);
    CHECK(typed_probe.valid);
    CHECK(typed_probe.header_size == 8);
    CHECK(typed_probe.declared_length == payload.size());
    CHECK(typed_probe.total_size == typed_framed.size());

    typed.includes_header = true;
    const auto typed_incl_framed = frame_typed(payload, 0x01020304, 4, true, 4, false, true);
    const auto typed_incl_probe = pr::probe_frame(
        typed, ByteSpan(typed_incl_framed.data(), typed_incl_framed.size()), 1024);
    CHECK(typed_incl_probe.header_ready);
    CHECK(typed_incl_probe.valid);
    CHECK(typed_incl_probe.declared_length == payload.size());
    CHECK(typed_incl_probe.total_size == typed_incl_framed.size());

    const auto grpc = frame_grpc(payload);
    const auto grpc_probe =
        pr::probe_frame(make_spec(pr::FramingKind::Grpc, 4, true),
                        ByteSpan(grpc.data(), grpc.size()), 1024);
    CHECK(grpc_probe.header_ready);
    CHECK(grpc_probe.valid);
    CHECK(grpc_probe.header_size == 5);
    CHECK(grpc_probe.declared_length == payload.size());
}

void test_early_framing_lock() {
    const auto payload = sample_message(7);
    const auto framed = frame_u32_be(payload);

    pr::ParseLimits limits{1'000'000, 64, 1 << 20};
    auto make_auto_learner = [&](bool early_lock) {
        return pr::FramingLearner(make_spec(pr::FramingKind::Auto, 4, true), 64 * 1024, 32,
                                  std::chrono::milliseconds(2000), 1 << 20, 1 << 20, limits,
                                  early_lock);
    };

    // A complete prefix plus the first payload tag is enough for an early lock.
    {
        pr::FramingLearner learner = make_auto_learner(true);
        const ByteSpan prefix_and_tag(framed.data(), 5);
        learner.contribute(1, prefix_and_tag);
        CHECK(learner.try_lock(false));
        CHECK(learner.locked());
        CHECK(learner.spec().kind == pr::FramingKind::Length);
        CHECK(learner.spec().width == 4);
        CHECK(learner.spec().big_endian);
        CHECK(learner.candidates().front().partial);
        CHECK(learner.candidates().front().first_tag_valid);
    }

    // A prefix without any payload tag must not lead to a premature lock.
    {
        pr::FramingLearner learner = make_auto_learner(true);
        const ByteSpan prefix_only(framed.data(), 4);
        learner.contribute(1, prefix_only);
        CHECK(!learner.try_lock(false));
        CHECK(!learner.locked());
    }

    // One complete frame is also sufficient; no 2s timeout is involved.
    {
        pr::FramingLearner learner = make_auto_learner(true);
        learner.contribute(1, ByteSpan(framed.data(), framed.size()));
        CHECK(learner.try_lock(false));
        CHECK(learner.locked());
        CHECK(learner.spec().kind == pr::FramingKind::Length);
        CHECK(learner.spec().width == 4);
        CHECK(learner.spec().big_endian);
    }

    // gRPC has its own five-byte header and must not be mistaken for len:4:be.
    {
        const auto grpc = frame_grpc(payload);
        pr::FramingLearner learner = make_auto_learner(true);
        const ByteSpan grpc_prefix_and_tag(grpc.data(), 6);
        learner.contribute(1, grpc_prefix_and_tag);
        CHECK(learner.try_lock(false));
        CHECK(learner.locked());
        CHECK(learner.spec().kind == pr::FramingKind::Grpc);
    }

    // --early-lock off keeps the old threshold-based behaviour.
    {
        pr::FramingLearner learner = make_auto_learner(false);
        const ByteSpan prefix_and_tag(framed.data(), 5);
        learner.contribute(1, prefix_and_tag);
        CHECK(!learner.try_lock(false));
        CHECK(!learner.locked());
    }
}

void test_stream_analyzer_early_lock() {
    const auto payload = sample_message(9);
    const auto framed = frame_u32_be(payload);

    pr::Config config;
    config.framing = make_spec(pr::FramingKind::Auto, 4, true);
    config.early_lock = true;
    config.learning_bytes = 64 * 1024;
    config.learning_frames = 32;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits,
                               config.early_lock);
    pr::SchemaStore schema(false, true, limits);
    pr::StreamAnalyzer analyzer(1, learner, schema, config);

    analyzer.feed(ByteSpan(framed.data(), 5));
    CHECK(learner.locked());
    CHECK(learner.spec().kind == pr::FramingKind::Length);
    CHECK(learner.spec().width == 4);
    CHECK(learner.spec().big_endian);

    analyzer.feed(ByteSpan(framed.data() + 5, framed.size() - 5));
    analyzer.finish();
    CHECK(analyzer.frames_processed() == 1);
    CHECK(schema.root().fields.count(1) == 1);
    CHECK(schema.root().fields.count(2) == 1);
}

void test_early_lock_cli() {
    pr::Config config;
    CHECK(parse_cli({"protoreverse", "serve", "--early-lock", "off"}, config));
    CHECK(!config.early_lock);
    CHECK(parse_cli({"protoreverse", "serve", "--early-lock", "balanced"}, config));
    CHECK(config.early_lock);
    CHECK(!parse_cli({"protoreverse", "serve", "--early-lock", "sometimes"}, config));
}

void test_greeting_cli() {
    pr::Config config;
    CHECK(parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "len:4:be"},
                    config));
    CHECK(config.greeting.enabled());
    CHECK(config.greeting.framing.has_value());
    CHECK(config.greeting.framing && config.greeting.framing->kind == pr::FramingKind::Length);

    CHECK(parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "type4+len4:le", "--greeting-type", "7"},
                    config));
    CHECK(config.greeting.message_type == 7);
    CHECK(config.greeting.framing &&
          config.greeting.framing->kind == pr::FramingKind::TypedLength);

    CHECK(parse_cli({"protoreverse", "serve", "--greeting-field", "4:int32=2",
                     "--greeting-framing", "raw"},
                    config));
    CHECK(config.greeting.fields.size() == 1);
    CHECK(config.greeting.message_type == 0);

    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-hex", "00"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-file", "a", "--greeting-hex",
                      "00", "--greeting-framing", "raw"},
                     config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-file", "a", "--greeting-file",
                      "b", "--greeting-framing", "raw"},
                     config));
    CHECK(parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "auto"},
                    config));
    CHECK(config.greeting.framing &&
          config.greeting.framing->kind == pr::FramingKind::Auto);
    CHECK(parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "auto", "--greeting-type", "7"},
                    config));
    CHECK(config.greeting.message_type == 7);
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                      "len:4:be", "--greeting-type", "7"},
                     config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-type", "7"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-framing", "raw"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-field", "not-a-field",
                      "--greeting-framing", "raw"},
                     config));
    CHECK(parse_cli({"protoreverse", "serve", "--reply-with-first-message", "--greeting-framing",
                     "auto"},
                    config));
    CHECK(config.reply_with_first_message);
    CHECK(config.greeting.framing &&
          config.greeting.framing->kind == pr::FramingKind::Auto);
    CHECK(!parse_cli({"protoreverse", "serve", "--reply-with-first-message", "--greeting-framing",
                      "len:4:be"},
                     config));
    CHECK(!parse_cli({"protoreverse", "serve", "--reply-with-first-message"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--reply-with-first-message", "--greeting-framing",
                      "len:4:be"},
                     config));
    CHECK(parse_cli({"protoreverse", "serve", "--reply-with-first-message", "--greeting-framing",
                     "auto", "--alive-message"},
                    config));
    CHECK(config.reply_with_first_message);
    CHECK(config.alive_message);
    CHECK(parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "auto", "--alive-message"},
                    config));
    CHECK(config.alive_message);
    CHECK(parse_cli({"protoreverse", "serve", "--reply-with-first-message", "--greeting-framing",
                     "auto", "--alive-message", "--alive-message-type", "5"},
                    config));
    CHECK(config.alive_message_type == 5);
    CHECK(config.alive_message_type_set);
    CHECK(!parse_cli({"protoreverse", "serve", "--alive-message-type", "5"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--alive-message"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--alive-message", "--alive-interval", "0ms"}, config));
    CHECK(!parse_cli({"protoreverse", "analyze", "--stdin", "--alive-message"}, config));
    CHECK(!parse_cli({"protoreverse", "serve", "--greeting-hex", "00", "--greeting-framing",
                     "len:4:be", "--alive-message"},
                    config));
    CHECK(!parse_cli({"protoreverse", "serve", "--stdin", "--alive-message"}, config));
    CHECK(!parse_cli({"protoreverse", "analyze", "--stdin", "--greeting-hex", "00",
                      "--greeting-framing", "raw"},
                     config));
}

void test_deferred_greeting_session() {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context io;
    boost::system::error_code ec;

    tcp::acceptor acceptor(io);
    acceptor.open(tcp::v4(), ec);
    CHECK(!ec);
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    CHECK(!ec);
    acceptor.bind(tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), ec);
    CHECK(!ec);
    acceptor.listen(1, ec);
    CHECK(!ec);

    tcp::socket client(io);
    client.connect(acceptor.local_endpoint(), ec);
    CHECK(!ec);
    tcp::socket server_socket(io);
    acceptor.accept(server_socket, ec);
    CHECK(!ec);
    acceptor.close();

    pr::Config config;
    config.greeting.hex = "08 01";
    config.greeting.framing = make_spec(pr::FramingKind::Auto, 4, true);
    config.greeting.message_type = 7;
    config.greeting.message_type_set = true;
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    std::vector<std::uint8_t> payload;
    std::string error;
    CHECK(pr::build_greeting_payload(config.greeting, config.max_frame, payload, error));

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits,
                               config.early_lock);
    pr::SchemaStore schema(false, true, limits);
    pr::SessionCallbacks callbacks;

    auto session = std::make_shared<pr::Session>(
        std::move(server_socket), 1, learner, schema, config, std::move(callbacks), nullptr,
        std::make_shared<const std::vector<std::uint8_t>>(std::move(payload)));
    session->start();
    CHECK(!learner.locked());

    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        const auto framed = frame_typed(sample_message(i), 100 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }
    asio::write(client, asio::buffer(stream), ec);
    CHECK(!ec);

    const std::vector<std::uint8_t> greeting_payload{0x08, 0x01};
    const std::vector<std::uint8_t> expected =
        frame_typed(greeting_payload, 7, 4, true, 4, false, false);
    std::vector<std::uint8_t> received(expected.size());

    bool got_greeting = false;
    asio::async_read(client, asio::buffer(received),
                     [&](const boost::system::error_code& read_error, std::size_t) {
                         got_greeting = !read_error;
                         io.stop();
                     });

    asio::steady_timer timeout(io);
    timeout.expires_after(std::chrono::milliseconds(2000));
    timeout.async_wait([&](const boost::system::error_code& timer_error) {
        if (!timer_error) {
            io.stop();
        }
    });

    io.run();
    CHECK(got_greeting);
    CHECK(received == expected);
    CHECK(learner.locked());
    CHECK(learner.spec().kind == pr::FramingKind::TypedLength);
    CHECK(session->analyzer().frames_processed() == 12);

    session->close();
}

void test_reply_with_first_message_greeting() {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context io;
    boost::system::error_code ec;

    tcp::acceptor acceptor(io);
    acceptor.open(tcp::v4(), ec);
    CHECK(!ec);
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    CHECK(!ec);
    acceptor.bind(tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), ec);
    CHECK(!ec);
    acceptor.listen(1, ec);
    CHECK(!ec);

    tcp::socket client(io);
    client.connect(acceptor.local_endpoint(), ec);
    CHECK(!ec);
    tcp::socket server_socket(io);
    acceptor.accept(server_socket, ec);
    CHECK(!ec);
    acceptor.close();

    pr::Config config;
    config.greeting.framing = make_spec(pr::FramingKind::Auto, 4, true);
    config.reply_with_first_message = true;
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits,
                               config.early_lock);
    pr::SchemaStore schema(false, true, limits);
    pr::SessionCallbacks callbacks;

    auto session = std::make_shared<pr::Session>(
        std::move(server_socket), 1, learner, schema, config, std::move(callbacks), nullptr,
        nullptr);
    session->start();
    CHECK(!learner.locked());

    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        const auto framed = frame_typed(sample_message(i), 100 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }
    asio::write(client, asio::buffer(stream), ec);
    CHECK(!ec);

    const std::vector<std::uint8_t> expected =
        frame_typed(sample_message(1), 101, 4, true, 4, false, false);
    std::vector<std::uint8_t> received(expected.size());

    bool got_greeting = false;
    asio::async_read(client, asio::buffer(received),
                     [&](const boost::system::error_code& read_error, std::size_t) {
                         got_greeting = !read_error;
                         io.stop();
                     });

    asio::steady_timer timeout(io);
    timeout.expires_after(std::chrono::milliseconds(2000));
    timeout.async_wait([&](const boost::system::error_code& timer_error) {
        if (!timer_error) {
            io.stop();
        }
    });

    io.run();
    CHECK(got_greeting);
    CHECK(received == expected);
    CHECK(learner.locked());
    CHECK(learner.spec().kind == pr::FramingKind::TypedLength);
    CHECK(session->analyzer().frames_processed() == 12);

    session->close();
}

void test_alive_message() {
    namespace asio = boost::asio;
    using tcp = asio::ip::tcp;

    asio::io_context io;
    boost::system::error_code ec;

    tcp::acceptor acceptor(io);
    acceptor.open(tcp::v4(), ec);
    CHECK(!ec);
    acceptor.set_option(tcp::acceptor::reuse_address(true), ec);
    CHECK(!ec);
    acceptor.bind(tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0), ec);
    CHECK(!ec);
    acceptor.listen(1, ec);
    CHECK(!ec);

    tcp::socket client(io);
    client.connect(acceptor.local_endpoint(), ec);
    CHECK(!ec);
    tcp::socket server_socket(io);
    acceptor.accept(server_socket, ec);
    CHECK(!ec);
    acceptor.close();

    pr::Config config;
    config.greeting.framing = make_spec(pr::FramingKind::Auto, 4, true);
    config.reply_with_first_message = true;
    config.alive_message = true;
    config.alive_interval = std::chrono::milliseconds(100);
    config.alive_message_type = 5;
    config.alive_message_type_set = true;
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits,
                               config.early_lock);
    pr::SchemaStore schema(false, true, limits);
    pr::SessionCallbacks callbacks;

    auto session = std::make_shared<pr::Session>(
        std::move(server_socket), 1, learner, schema, config, std::move(callbacks), nullptr,
        nullptr);
    session->start();
    CHECK(!learner.locked());

    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 12; ++i) {
        const auto framed = frame_typed(sample_message(i), 100 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }
    asio::write(client, asio::buffer(stream), ec);
    CHECK(!ec);

    const std::vector<std::uint8_t> expected =
        frame_typed(sample_message(1), 101, 4, true, 4, false, false);
    std::vector<std::uint8_t> received(expected.size());
    std::vector<std::uint8_t> alive_received;

    const std::uint64_t alive_type =
        config.alive_message_type_set ? config.alive_message_type : learner.spec().type_value;

    bool got_greeting = false;
    bool got_alive = false;
    asio::steady_timer timeout(io);
    timeout.expires_after(std::chrono::milliseconds(2000));
    timeout.async_wait([&](const boost::system::error_code& timer_error) {
        if (!timer_error) {
            io.stop();
        }
    });
    asio::async_read(client, asio::buffer(received),
                     [&](const boost::system::error_code& read_error, std::size_t) {
                         got_greeting = !read_error;
                         if (read_error || !got_greeting) {
                             return;
                         }
                         // The server replies with the first client frame and then
                         // sends a header-only keep-alive frame (type + zero length).
                         const std::vector<std::uint8_t> empty_payload;
                         std::vector<std::uint8_t> alive_expected;
                         alive_expected = frame_typed(empty_payload, alive_type, 4, true, 4, false, false);
                         alive_received.assign(alive_expected.size(), 0);
                         got_alive = false;
                         asio::async_read(client, asio::buffer(alive_received),
                                          [&](const boost::system::error_code& read_error,
                                              std::size_t) {
                                              got_alive = !read_error;
                                              io.stop();
                                          });
                     });

    io.run();
    CHECK(got_greeting);
    CHECK(received == expected);

    const std::vector<std::uint8_t> empty_payload;
    std::vector<std::uint8_t> alive_expected =
        frame_typed(empty_payload, alive_type, 4, true, 4, false, false);
    CHECK(alive_expected.size() == 8);
    CHECK(got_alive);
    CHECK(alive_received == alive_expected);

    session->close();
}

void test_json_decoder() {
    std::vector<std::uint8_t> payload;
    add_varint(payload, 1, 42);
    add_varint(payload, 2, static_cast<std::uint64_t>(-1));
    add_varint(payload, 3, std::numeric_limits<std::uint64_t>::max());
    add_varint(payload, 4, 1);
    add_string(payload, 5, std::string("a\"b\\c\n"));

    const std::vector<std::uint8_t> binary = {0x00, 0xff, 0x10};
    add_bytes(payload, 6, binary);
    add_fixed32(payload, 7, 7);
    add_fixed64(payload, 8, 0x8000000000000000ull);
    add_fixed32(payload, 9, std::bit_cast<std::uint32_t>(1.5f));
    add_fixed64(payload, 10, std::bit_cast<std::uint64_t>(2.5));

    std::vector<std::uint8_t> nested;
    add_varint(nested, 1, 5);
    add_bytes(payload, 11, nested);

    add_bytes(payload, 12, std::vector<std::uint8_t>{1, 2, 3});
    add_fixed32(payload, 13, 10);
    add_fixed32(payload, 13, 20);
    add_string(payload, 14, "a");
    add_string(payload, 14, "b");
    add_varint(payload, 15, 99);
    add_bytes(payload, 16, std::vector<std::uint8_t>{0xff});

    pr::ResolvedMessage child;
    child.name = "Root_Field11";
    pr::ResolvedField child_field;
    child_field.number = 1;
    child_field.name = "field_1";
    child_field.type = "int32";
    child.fields.push_back(child_field);

    pr::ResolvedMessage root;
    root.name = "Root";
    auto add_field = [&](std::uint32_t number, const std::string& type) {
        pr::ResolvedField field;
        field.number = number;
        field.name = "field_" + std::to_string(number);
        field.type = type;
        root.fields.push_back(field);
        return &root.fields.back();
    };
    add_field(1, "int32");
    add_field(2, "int64");
    add_field(3, "uint64");
    add_field(4, "bool");
    add_field(5, "string");
    add_field(6, "bytes");
    add_field(7, "fixed32");
    add_field(8, "fixed64");
    add_field(9, "float");
    add_field(10, "double");
    add_field(11, child.name);
    pr::ResolvedField* packed = add_field(12, "int32");
    packed->repeated = true;
    packed->packed = true;
    pr::ResolvedField* repeated_fixed = add_field(13, "fixed32");
    repeated_fixed->repeated = true;
    pr::ResolvedField* repeated_string = add_field(14, "string");
    repeated_string->repeated = true;
    pr::ResolvedField* invalid = add_field(15, "bytes");
    invalid->valid = false;
    add_field(16, "string");

    pr::ResolvedModel model;
    model.root_name = "Root";
    model.messages.push_back(root);
    model.messages.push_back(child);

    const pr::DecodeSchema schema = pr::make_decode_schema(model);
    CHECK(schema.root != nullptr);

    pr::MessageMetadata metadata;
    metadata.session = 2;
    metadata.index = 7;
    metadata.frame_type = 200;

    pr::DecodeOptions options;
    pr::ParseLimits limits{1'000'000, 64, 16 * 1024 * 1024};

    std::string out;
    pr::render_message_json(ByteSpan(payload.data(), payload.size()), schema, metadata,
                            options, limits, out);

    CHECK(out.starts_with("{\"session\":2,\"index\":7,\"type\":200"));
    CHECK(out.find("\"field_1\":42") != std::string::npos);
    CHECK(out.find("\"field_2\":\"-1\"") != std::string::npos);
    CHECK(out.find("\"field_3\":\"18446744073709551615\"") != std::string::npos);
    CHECK(out.find("\"field_4\":true") != std::string::npos);
    CHECK(out.find("\"field_5\":\"a\\\"b\\\\c\\n\"") != std::string::npos);
    CHECK(out.find("\"field_6\":\"00 ff 10\"") != std::string::npos);
    CHECK(out.find("\"field_7\":7") != std::string::npos);
    CHECK(out.find("\"field_8\":\"9223372036854775808\"") != std::string::npos);
    CHECK(out.find("\"field_9\":1.5") != std::string::npos);
    CHECK(out.find("\"field_10\":2.5") != std::string::npos);
    CHECK(out.find("\"field_11\":{\"field_1\":5}") != std::string::npos);
    CHECK(out.find("\"field_12\":[1,2,3]") != std::string::npos);
    CHECK(out.find("\"field_13\":[10,20]") != std::string::npos);
    CHECK(out.find("\"field_14\":[\"a\",\"b\"]") != std::string::npos);
    CHECK(out.find("\"field_15\":{\"wire_type\":0,\"value\":\"99\"}") !=
          std::string::npos);
    CHECK(out.find("\"field_16\":{\"wire_type\":2,\"raw\":\"ff\"}") !=
          std::string::npos);

    options.hexdump_bytes = 2;
    pr::render_message_json(ByteSpan(payload.data(), payload.size()), schema, metadata,
                            options, limits, out);
    CHECK(out.find("\"field_6\":\"00 ff ...\"") != std::string::npos);
}

std::string temp_output_path(const std::string& suffix) {
    namespace fs = std::filesystem;
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return (fs::temp_directory_path() /
            ("protoreverse-test-" + std::to_string(stamp) + "-" + suffix))
        .string();
}

void test_json_spool_and_analyzer() {
    std::vector<std::uint8_t> stream;
    for (std::uint64_t i = 1; i <= 8; ++i) {
        const auto framed = frame_typed(sample_message(i), 100 + i, 4, true, 4, false, false);
        stream.insert(stream.end(), framed.begin(), framed.end());
    }

    pr::Config config;
    config.framing.kind = pr::FramingKind::TypedLength;
    config.framing.type_width = 4;
    config.framing.type_big_endian = true;
    config.framing.width = 4;
    config.framing.big_endian = false;
    config.framing.includes_header = false;
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);

    pr::MessageSpool spool;
    std::string error;
    CHECK(spool.open(error));
    pr::StreamAnalyzer analyzer(7, learner, schema, config, &spool);
    analyzer.feed(stream);
    analyzer.finish();

    CHECK(!analyzer.fatal_error());
    CHECK(analyzer.frames_processed() == 8);

    pr::ResolveOptions resolve_options;
    resolve_options.root_name = "ClientMessage";
    const pr::ResolvedModel model = pr::resolve_schema(schema.root(), resolve_options);

    const std::string path = temp_output_path("messages.jsonl");
    pr::JsonOptions json_options;
    std::uint64_t written = 0;
    CHECK(pr::write_messages_json(path, spool, model, json_options, limits, written, error));
    CHECK(written == 8);

    std::ifstream input(path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    std::string line;
    std::size_t lines = 0;
    bool saw_type = false;
    bool saw_fields = false;
    while (std::getline(input, line)) {
        ++lines;
        if (line.find("\"session\":7") != std::string::npos &&
            line.find("\"type\":") != std::string::npos) {
            saw_type = true;
        }
        if (line.find("\"fields\":{") != std::string::npos) {
            saw_fields = true;
        }
    }
    input.close();
    CHECK(lines == 8);
    CHECK(saw_type);
    CHECK(saw_fields);

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

void test_save_first_message() {
    const auto payload = sample_message(7);
    const auto first = frame_typed(payload, 100, 4, true, 4, false, false);
    const auto second = frame_typed(sample_message(8), 101, 4, true, 4, false, false);

    pr::Config config;
    config.framing.kind = pr::FramingKind::TypedLength;
    config.framing.type_width = 4;
    config.framing.type_big_endian = true;
    config.framing.width = 4;
    config.framing.big_endian = false;
    config.framing.includes_header = false;
    config.save_first_message_path = temp_output_path("first-message.bin");
    config.learning_bytes = 4096;
    config.learning_frames = 8;

    pr::ParseLimits limits{1'000'000, 64, config.max_frame};
    pr::FramingLearner learner(config.framing, config.learning_bytes,
                               config.learning_frames, config.learning_timeout,
                               config.max_frame, config.max_learning_memory, limits);
    pr::SchemaStore schema(false, true, limits);

    pr::StreamAnalyzer analyzer(7, learner, schema, config, nullptr);
    analyzer.feed(first);
    analyzer.feed(second);
    analyzer.finish();

    CHECK(!analyzer.fatal_error());
    CHECK(analyzer.frames_processed() == 2);

    std::ifstream input(config.save_first_message_path, std::ios::binary);
    CHECK(static_cast<bool>(input));
    std::vector<std::uint8_t> saved(first.size());
    input.read(reinterpret_cast<char*>(saved.data()),
               static_cast<std::streamsize>(saved.size()));
    CHECK(input.gcount() == static_cast<std::streamsize>(saved.size()));
    CHECK(saved == first);

    std::error_code ec;
    std::filesystem::remove(config.save_first_message_path, ec);
}

void test_json_cli() {
    pr::Config config;
    CHECK(parse_cli({"protoreverse", "analyze", "--stdin", "--json-output",
                     "messages.jsonl"},
                    config));
    CHECK(config.json_output_path == "messages.jsonl");
    CHECK(config.json_hexdump_bytes == 0);

    CHECK(parse_cli({"protoreverse", "analyze", "--stdin", "--json-output",
                     "messages.jsonl", "--json-hexdump-bytes", "16"},
                    config));
    CHECK(config.json_hexdump_bytes == 16);

    CHECK(!parse_cli({"protoreverse", "analyze", "--stdin", "--json-hexdump-bytes",
                      "16"},
                     config));
    CHECK(!parse_cli({"protoreverse", "analyze", "--stdin", "--json-output",
                      "schema.proto"},
                     config));
    CHECK(!parse_cli({"protoreverse", "analyze", "--stdin", "--learning-only",
                      "--json-output", "messages.jsonl"},
                     config));
    CHECK(parse_cli({"protoreverse", "serve", "--json-output", "messages.jsonl"},
                    config));
}

} // namespace

int main() {
    test_varint();
    test_parse_message();
    test_framing(pr::FramingKind::Length, 4, true, "u32be");
    test_framing(pr::FramingKind::Length, 4, false, "u32le");
    test_framing(pr::FramingKind::Varint, 4, true, "varint");
    test_framing(pr::FramingKind::Grpc, 4, true, "grpc");
    test_length_framers();
    test_delimiter_framers();
    test_fixed_framer();
    test_typed_framers();
    test_typed_auto_detection();
    test_typed_stream_analyzer();
    test_auto_length_prefixes();
    test_chunked_stream();
    test_multiple_sessions();
    test_schema_and_proto();
    test_types_and_proto2();
    test_pure_packed_fixed_stats();
    test_raw_mode();
    test_framing_spec_parsing();
    test_frame_encoder();
    test_frame_probe();
    test_early_framing_lock();
    test_stream_analyzer_early_lock();
    test_greeting_fields();
    test_greeting_cli();
    test_deferred_greeting_session();
    test_reply_with_first_message_greeting();
    test_alive_message();
    test_json_decoder();
    test_json_spool_and_analyzer();
    test_save_first_message();
    test_json_cli();
    test_early_lock_cli();
    test_random_data();

    if (g_failures != 0) {
        std::cerr << g_failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all tests passed\n";
    return 0;
}
