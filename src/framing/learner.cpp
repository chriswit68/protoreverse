#include "framing/learner.hpp"

#include "log.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

namespace pr {
namespace {

std::string format_score(double value) {
    std::ostringstream out;
    out.setf(std::ios::fixed);
    out.precision(3);
    out << value;
    return out.str();
}

bool is_valid_tag_byte(std::uint8_t byte) {
    const std::uint32_t field_number = byte >> 3;
    const std::uint8_t wire_type = byte & 0x07;
    if (field_number == 0) {
        return false;
    }
    return wire_type == 0 || wire_type == 1 || wire_type == 2 || wire_type == 5;
}

} // namespace

FramingLearner::FramingLearner(FramingSpec requested, std::size_t learning_bytes,
                               std::size_t learning_frames,
                               std::chrono::milliseconds learning_timeout,
                               std::size_t max_frame, std::size_t max_memory,
                               ParseLimits parse_limits, bool early_lock)
    : requested_(requested),
      learning_bytes_(learning_bytes),
      learning_frames_(learning_frames),
      learning_timeout_(learning_timeout),
      max_frame_(max_frame),
      max_memory_(max_memory),
      parse_limits_(parse_limits),
      early_lock_(early_lock) {
    if (requested_.kind != FramingKind::Auto) {
        locked_ = true;
        spec_ = requested_;
        confidence_ = 1.0;
        log_info("learn", "using explicit framing: ", to_string(spec_));
    }
}

std::vector<FramingSpec> FramingLearner::candidate_specs() {
    std::vector<FramingSpec> specs;

    for (std::size_t width : {std::size_t{4}, std::size_t{2}, std::size_t{8},
                              std::size_t{1}}) {
        for (bool includes_header : {false, true}) {
            if (width == 1) {
                FramingSpec spec;
                spec.kind = FramingKind::Length;
                spec.width = 1;
                spec.big_endian = true;
                spec.includes_header = includes_header;
                specs.push_back(spec);
            } else {
                for (bool big_endian : {true, false}) {
                    FramingSpec spec;
                    spec.kind = FramingKind::Length;
                    spec.width = static_cast<std::uint8_t>(width);
                    spec.big_endian = big_endian;
                    spec.includes_header = includes_header;
                    specs.push_back(spec);
                }
            }
        }
    }

    FramingSpec varint;
    varint.kind = FramingKind::Varint;
    specs.push_back(varint);

    FramingSpec grpc;
    grpc.kind = FramingKind::Grpc;
    specs.push_back(grpc);

    FramingSpec newline;
    newline.kind = FramingKind::Newline;
    specs.push_back(newline);

    FramingSpec nul;
    nul.kind = FramingKind::Nul;
    specs.push_back(nul);

    FramingSpec raw;
    raw.kind = FramingKind::Raw;
    specs.push_back(raw);

    return specs;
}

double FramingLearner::spec_prior(const FramingSpec& spec) {
    switch (spec.kind) {
    case FramingKind::Length:
        switch (spec.width) {
        case 4:
            return spec.includes_header ? 0.94 : (spec.big_endian ? 1.00 : 0.95);
        case 2:
            return spec.includes_header ? 0.77 : (spec.big_endian ? 0.82 : 0.78);
        case 8:
            return spec.includes_header ? 0.67 : (spec.big_endian ? 0.72 : 0.68);
        case 1:
            return spec.includes_header ? 0.50 : 0.55;
        default:
            return 0.50;
        }
    case FramingKind::Varint:
        return 0.85;
    case FramingKind::Grpc:
        return 0.90;
    case FramingKind::Newline:
    case FramingKind::Nul:
        return 0.55;
    case FramingKind::Fixed:
        return 0.45;
    case FramingKind::Raw:
        return 0.10;
    case FramingKind::Auto:
        return 0.0;
    }
    return 0.0;
}

void FramingLearner::contribute(std::uint64_t session_id, ByteSpan data) {
    total_bytes_ += data.size();
    if (locked_ || data.empty()) {
        return;
    }

    if (!have_first_byte_) {
        have_first_byte_ = true;
        first_byte_time_ = std::chrono::steady_clock::now();
    }

    auto& sample = samples_[session_id];
    const std::size_t per_session =
        sample.size() < learning_bytes_ ? learning_bytes_ - sample.size() : 0;
    const std::size_t memory =
        sample_bytes_ < max_memory_ ? max_memory_ - sample_bytes_ : 0;
    const std::size_t take = std::min({data.size(), per_session, memory});
    if (take > 0) {
        sample.insert(sample.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(take));
        sample_bytes_ += take;
    }

    try_lock(false);
}

void FramingLearner::evaluate() {
    candidates_.clear();
    candidates_.reserve(24);

    if (sample_bytes_ > 0) {
        for (const FramingSpec& spec : candidate_specs()) {
            Analysis analysis = analyze_spec(spec);
            candidates_.push_back(std::move(analysis.score));
        }

        std::stable_sort(candidates_.begin(), candidates_.end(),
                         [](const CandidateScore& a, const CandidateScore& b) {
                             if (a.valid != b.valid) {
                                 return a.valid;
                             }
                             return a.score > b.score;
                         });
    }

    evaluated_once_ = true;
    last_eval_ = std::chrono::steady_clock::now();
}

FramingLearner::Analysis FramingLearner::analyze_spec(const FramingSpec& spec) {
    Analysis analysis;
    analysis.score.spec = spec;
    analysis.score.label = to_string(spec);

    std::size_t total_bytes = 0;

    for (const auto& [session_id, sample] : samples_) {
        (void)session_id;
        if (sample.empty()) {
            continue;
        }
        total_bytes += sample.size();
        ++analysis.score.samples;

        std::string error;
        const bool ok = spec.kind == FramingKind::Raw
                            ? analyze_raw_sample(sample, analysis, error)
                            : analyze_framed_sample(spec, sample, analysis, error);
        if (!ok) {
            analysis.score.valid = false;
            analysis.score.reason = error.empty() ? "candidate failed" : error;
            return analysis;
        }
    }

    const bool raw = spec.kind == FramingKind::Raw;
    if (raw && analysis.score.frames == 0) {
        analysis.score.valid = false;
        analysis.score.reason = "no complete fields";
        return analysis;
    }
    if (!raw && analysis.score.frames == 0 && !analysis.score.header_complete) {
        analysis.score.valid = false;
        analysis.score.reason = "insufficient evidence";
        return analysis;
    }
    if (!raw && analysis.score.frames == 0) {
        analysis.score.partial = true;
    }

    double weighted_consistency = 0.0;
    std::uint64_t total_occurrences = 0;
    for (const auto& [number, aggregate] : analysis.fields) {
        (void)number;
        if (aggregate.occurrences == 0) {
            continue;
        }
        const std::uint64_t best_wire_type =
            *std::max_element(aggregate.wire_counts.begin(), aggregate.wire_counts.end());
        weighted_consistency +=
            static_cast<double>(best_wire_type) * static_cast<double>(aggregate.occurrences);
        total_occurrences += aggregate.occurrences;
    }

    const double consistency =
        total_occurrences > 0 ? weighted_consistency / static_cast<double>(total_occurrences)
                              : 0.5;
    const double coverage =
        total_bytes > 0 ? static_cast<double>(analysis.score.bytes) /
                              static_cast<double>(total_bytes)
                        : 0.0;
    // A small sample should saturate faster than the full learning target,
    // otherwise a single complete frame contributes almost nothing.
    const std::size_t frame_target =
        std::max<std::size_t>(1, std::min<std::size_t>(learning_frames_, 4));
    const double frame_score =
        std::min(1.0, static_cast<double>(analysis.score.frames) /
                          static_cast<double>(frame_target));

    double structural = 0.0;
    if (raw) {
        structural = analysis.score.frames > 0 ? 0.20 : 0.0;
    } else {
        if (analysis.score.header_complete) {
            structural += 0.40;
        }
        if (analysis.score.first_tag_valid) {
            structural += 0.30;
        }
        if (analysis.score.frames > 0) {
            structural += 0.20;
        }
        if (analysis.score.second_boundary_valid) {
            structural += 0.10;
        }
    }
    structural = std::clamp(structural, 0.0, 1.0);

    double plausibility = 0.0;
    if (analysis.score.frames >= 2) {
        plausibility = 1.0;
    } else if (analysis.score.frames == 1) {
        plausibility = 0.80;
    } else if (structural > 0.0) {
        plausibility = 0.60;
    }

    double zero_penalty = 1.0;
    if (analysis.score.frames > 0 && analysis.zero_frames > 0) {
        const double zero_ratio =
            static_cast<double>(analysis.zero_frames) /
            static_cast<double>(analysis.score.frames);
        zero_penalty = std::clamp(1.0 - 4.0 * zero_ratio, 0.02, 1.0);
    }

    const double prior = spec_prior(spec);
    double score = 0.0;
    if (raw) {
        score = 0.20 * coverage + 0.20 * consistency + 0.10 * plausibility +
                0.20 * structural + 0.10 * prior;
    } else {
        score = 0.20 * coverage + 0.15 * frame_score + 0.20 * consistency +
                0.10 * plausibility + 0.25 * structural + 0.10 * prior;
    }
    score *= zero_penalty;
    if (analysis.score.frames < 2) {
        score *= 0.85;
    }

    analysis.score.structural = structural;
    analysis.score.score = std::clamp(score, 0.0, 1.0);
    analysis.score.valid = true;
    return analysis;
}

bool FramingLearner::analyze_framed_sample(const FramingSpec& spec, ByteSpan data,
                                           Analysis& analysis, std::string& error) {
    ByteSpan remaining = data;
    while (!remaining.empty()) {
        const FrameProbe probe = probe_frame(spec, remaining, max_frame_);
        if (!probe.header_ready) {
            if (!probe.error.empty()) {
                error = probe.error;
                return false;
            }
            if (analysis.score.frames > 0) {
                analysis.score.partial = true;
            }
            break;
        }
        if (!probe.valid) {
            error = probe.error.empty() ? "invalid frame header" : probe.error;
            return false;
        }

        analysis.score.header_complete = true;
        if (analysis.score.frames == 0) {
            analysis.score.declared_length = probe.declared_length;
        }

        const bool has_payload = probe.total_size > probe.header_size;
        if (has_payload) {
            if (remaining.size() <= probe.header_size) {
                analysis.score.partial = true;
                analysis.score.bytes_needed = probe.total_size - remaining.size();
                break;
            }
            if (!is_valid_tag_byte(remaining[probe.header_size])) {
                error = "payload does not start with a valid protobuf tag";
                return false;
            }
            analysis.score.first_tag_valid = true;
        }

        if (remaining.size() < probe.total_size) {
            analysis.score.partial = true;
            analysis.score.bytes_needed = probe.total_size - remaining.size();
            break;
        }

        const ByteSpan frame =
            remaining.subspan(probe.header_size, probe.total_size - probe.header_size);
        ParsedMessage message;
        std::string parse_error;
        if (!parse_message(frame, message, parse_limits_, 0, parse_error)) {
            error = "frame parse failed: " + parse_error;
            return false;
        }

        ++analysis.score.frames;
        if (frame.empty()) {
            ++analysis.zero_frames;
        }
        analysis.score.bytes += probe.total_size;

        for (const ParsedField& field : message.fields) {
            Aggregate& aggregate = analysis.fields[field.number];
            aggregate.wire_counts[field.wire_type]++;
            aggregate.occurrences++;
        }

        remaining = remaining.subspan(probe.total_size);

        if (!remaining.empty()) {
            const FrameProbe next = probe_frame(spec, remaining, max_frame_);
            if (next.header_ready && next.valid) {
                if (next.total_size == next.header_size) {
                    analysis.score.second_boundary_valid = true;
                } else if (remaining.size() > next.header_size &&
                           is_valid_tag_byte(remaining[next.header_size])) {
                    analysis.score.second_boundary_valid = true;
                }
            }
        }

        if (analysis.score.frames > 1'000'000) {
            error = "too many frames in sample";
            return false;
        }
    }

    return true;
}

bool FramingLearner::analyze_raw_sample(ByteSpan data, Analysis& analysis,
                                        std::string& error) {
    ByteSpan remaining = data;
    while (!remaining.empty()) {
        ParsedField field;
        std::size_t consumed = 0;
        bool complete = false;
        if (!parse_one_field(remaining, field, consumed, complete, parse_limits_, error)) {
            return false;
        }
        if (!complete) {
            analysis.score.partial = true;
            break;
        }
        if (consumed == 0) {
            error = "raw parser made no progress";
            return false;
        }

        ++analysis.score.frames;
        analysis.score.first_tag_valid = true;
        analysis.score.bytes += consumed;

        Aggregate& aggregate = analysis.fields[field.number];
        aggregate.wire_counts[field.wire_type]++;
        aggregate.occurrences++;

        remaining = remaining.subspan(consumed);
        if (analysis.score.frames > 1'000'000) {
            error = "too many fields in raw sample";
            return false;
        }
    }

    return true;
}

void FramingLearner::lock(const CandidateScore& best, double confidence, bool fallback) {
    if (locked_) {
        return;
    }
    locked_ = true;
    spec_ = best.spec;
    confidence_ = confidence;

    if (fallback) {
        log_warn("learn", "no valid framing candidate; falling back to RAW");
    } else {
        log_info("learn", "selected framing: ", best.label, " (confidence ",
                 format_score(confidence_), ")");
    }

    std::size_t shown = 0;
    for (const CandidateScore& candidate : candidates_) {
        if (shown >= 6) {
            break;
        }
        if (!candidate.valid) {
            continue;
        }
        std::ostringstream line;
        line << std::left << std::setw(22) << candidate.label << " score="
             << format_score(candidate.score) << " struct="
             << format_score(candidate.structural) << " frames=" << candidate.frames
             << " coverage=" << format_score(
                    sample_bytes_ > 0
                        ? static_cast<double>(candidate.bytes) /
                              static_cast<double>(sample_bytes_)
                        : 0.0);
        if (candidate.partial) {
            line << " partial";
        }
        if (candidate.bytes_needed > 0) {
            line << " need=" << candidate.bytes_needed;
        }
        log_info("learn", line.str());
        ++shown;
    }

    if (candidates_.size() >= 2 && candidates_[0].valid && candidates_[1].valid) {
        const double margin = candidates_[0].score - candidates_[1].score;
        if (margin < 0.05) {
            log_warn("learn", "framing is ambiguous (margin ", format_score(margin),
                     "); use --framing to override");
        }
    }

    samples_.clear();
}

bool FramingLearner::try_lock(bool force) {
    if (locked_) {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    const bool should_evaluate =
        force || !evaluated_once_ || sample_bytes_ >= learning_bytes_ ||
        (have_first_byte_ && now - last_eval_ >= std::chrono::milliseconds(50));
    if (should_evaluate) {
        evaluate();
    }

    if (candidates_.empty()) {
        if (force) {
            CandidateScore fallback;
            fallback.spec.kind = FramingKind::Raw;
            fallback.label = "raw";
            fallback.reason = "no data";
            lock(fallback, 0.05, true);
            return true;
        }
        return false;
    }

    const CandidateScore* best = nullptr;
    for (const CandidateScore& candidate : candidates_) {
        if (candidate.valid) {
            best = &candidate;
            break;
        }
    }

    const bool timed_out =
        have_first_byte_ && (now - first_byte_time_) >= learning_timeout_;
    const bool enough_bytes = sample_bytes_ >= learning_bytes_;
    const bool enough_frames =
        best != nullptr && best->frames >= learning_frames_;

    if (best == nullptr) {
        if (!force && !timed_out) {
            return false;
        }
        CandidateScore fallback;
        fallback.spec.kind = FramingKind::Raw;
        fallback.label = "raw";
        fallback.reason = "no candidate passed";
        lock(fallback, 0.05, true);
        return true;
    }

    const CandidateScore* runner = nullptr;
    for (const CandidateScore& candidate : candidates_) {
        if (&candidate != best && candidate.valid) {
            runner = &candidate;
            break;
        }
    }
    const double margin = runner != nullptr ? best->score - runner->score : best->score;
    const bool framed = best->spec.kind != FramingKind::Raw;
    const bool strong_complete =
        framed && best->header_complete && best->first_tag_valid &&
        best->frames >= 1 && best->structural >= 0.90 && margin >= 0.15;
    const bool strong_prefix =
        framed && best->header_complete && best->first_tag_valid &&
        best->structural >= 0.70 && margin >= 0.20;

    // With only a prefix and the first tag, payload-length and
    // header-inclusive framing are structurally indistinguishable. Use the
    // documented prior (payload length is the default) and keep confidence low.
    const bool includes_header_tie =
        strong_complete == false && framed && best->header_complete &&
        best->first_tag_valid && best->structural >= 0.70 &&
        best->spec.kind == FramingKind::Length && !best->spec.includes_header &&
        runner != nullptr && runner->valid && runner->spec.kind == FramingKind::Length &&
        runner->spec.width == best->spec.width &&
        runner->spec.big_endian == best->spec.big_endian &&
        runner->spec.includes_header != best->spec.includes_header;
    const bool early_decision =
        early_lock_ && (strong_complete || strong_prefix || includes_header_tie);

    if (!force && !early_decision && !timed_out &&
        !(enough_bytes || enough_frames)) {
        return false;
    }

    const std::size_t frame_target =
        std::max<std::size_t>(1, std::min<std::size_t>(learning_frames_, 4));
    double sample_factor = 0.35;
    if (best->frames > 0) {
        sample_factor =
            std::clamp(0.65 + 0.35 * std::min(1.0, static_cast<double>(best->frames) /
                                                       static_cast<double>(frame_target)),
                       0.65, 1.0);
    } else if (best->first_tag_valid) {
        sample_factor = 0.55;
    }

    const double margin_factor =
        0.65 + 0.35 * std::clamp(margin / 0.15, 0.0, 1.0);
    double confidence = std::clamp(best->score * sample_factor * margin_factor, 0.05, 0.99);
    if (best->spec.kind == FramingKind::Raw) {
        confidence = std::min(confidence, 0.80);
    } else if (best->frames == 0) {
        confidence = std::min(confidence, 0.70);
    } else if (best->frames == 1) {
        confidence = std::min(confidence, 0.85);
    }

    if (early_decision) {
        log_debug("learn", "early framing decision from ", best->frames, " complete frame(s), ",
                  sample_bytes_, " sampled byte(s)");
    }

    lock(*best, confidence, false);
    return true;
}

} // namespace pr
