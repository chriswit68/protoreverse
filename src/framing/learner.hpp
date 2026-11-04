#pragma once

#include "framing/framer.hpp"
#include "framing/spec.hpp"
#include "wire/validator.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pr {

struct CandidateScore {
    FramingSpec spec;
    std::string label;
    double score = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t bytes = 0;
    std::uint64_t samples = 0;
    bool valid = false;
    std::string reason;
};

class FramingLearner {
public:
    FramingLearner(FramingSpec requested, std::size_t learning_bytes,
                   std::size_t learning_frames,
                   std::chrono::milliseconds learning_timeout,
                   std::size_t max_frame, std::size_t max_memory,
                   ParseLimits parse_limits);

    void contribute(std::uint64_t session_id, ByteSpan data);
    bool try_lock(bool force);
    void force_lock() { try_lock(true); }

    bool locked() const { return locked_; }
    FramingSpec spec() const { return spec_; }
    double confidence() const { return confidence_; }
    const std::vector<CandidateScore>& candidates() const { return candidates_; }
    std::uint64_t total_bytes() const { return total_bytes_; }
    std::uint64_t sample_bytes() const { return sample_bytes_; }

private:
    struct Aggregate {
        std::array<std::uint64_t, 8> wire_counts{};
        std::uint64_t occurrences = 0;
    };

    struct Analysis {
        CandidateScore score;
        std::map<std::uint32_t, Aggregate> fields;
        std::uint64_t zero_frames = 0;
    };

    void evaluate();
    Analysis analyze_spec(const FramingSpec& spec);
    bool analyze_framed_sample(const FramingSpec& spec, ByteSpan data, Analysis& analysis,
                               std::string& error);
    bool analyze_raw_sample(ByteSpan data, Analysis& analysis, std::string& error);
    void lock(const CandidateScore& best, double confidence, bool fallback);

    static std::vector<FramingSpec> candidate_specs();

    FramingSpec requested_;
    std::size_t learning_bytes_;
    std::size_t learning_frames_;
    std::chrono::milliseconds learning_timeout_;
    std::size_t max_frame_;
    std::size_t max_memory_;
    ParseLimits parse_limits_;

    std::map<std::uint64_t, std::vector<std::uint8_t>> samples_;
    std::size_t sample_bytes_ = 0;
    std::uint64_t total_bytes_ = 0;

    bool locked_ = false;
    FramingSpec spec_;
    double confidence_ = 0.0;
    std::vector<CandidateScore> candidates_;

    bool have_first_byte_ = false;
    std::chrono::steady_clock::time_point first_byte_time_{};
    std::chrono::steady_clock::time_point last_eval_{};
    bool evaluated_once_ = false;
};

} // namespace pr
