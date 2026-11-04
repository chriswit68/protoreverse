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

} // namespace

FramingLearner::FramingLearner(FramingSpec requested, std::size_t learning_bytes,
                               std::size_t learning_frames,
                               std::chrono::milliseconds learning_timeout,
                               std::size_t max_frame, std::size_t max_memory,
                               ParseLimits parse_limits)
    : requested_(requested),
      learning_bytes_(learning_bytes),
      learning_frames_(learning_frames),
      learning_timeout_(learning_timeout),
      max_frame_(max_frame),
      max_memory_(max_memory),
      parse_limits_(parse_limits) {
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

    if (analysis.score.frames == 0) {
        analysis.score.valid = false;
        analysis.score.reason = "no complete frames";
        return analysis;
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
    const double frame_score =
        std::min(1.0, static_cast<double>(analysis.score.frames) /
                          static_cast<double>(std::max<std::size_t>(1, learning_frames_)));

    double plausibility = 1.0;
    double zero_penalty = 1.0;
    if (analysis.score.frames < 2) {
        plausibility = 0.6;
    }
    if (analysis.score.frames > 0 && analysis.zero_frames > 0) {
        const double zero_ratio =
            static_cast<double>(analysis.zero_frames) /
            static_cast<double>(analysis.score.frames);
        zero_penalty = std::clamp(1.0 - 4.0 * zero_ratio, 0.02, 1.0);
    }

    const double framing_bonus = spec.kind == FramingKind::Raw ? 0.0 : 0.15;
    double score = 0.30 * coverage + 0.20 * frame_score + 0.30 * consistency +
                   0.05 * plausibility + framing_bonus;
    score *= zero_penalty;
    if (analysis.score.frames < 2) {
        score *= 0.7;
    }

    analysis.score.score = std::clamp(score, 0.0, 1.0);
    analysis.score.valid = true;
    return analysis;
}

bool FramingLearner::analyze_framed_sample(const FramingSpec& spec, ByteSpan data,
                                           Analysis& analysis, std::string& error) {
    std::unique_ptr<Framer> framer = make_framer(spec, max_frame_);
    if (!framer) {
        error = "framer is not available";
        return false;
    }

    ByteSpan remaining = data;
    while (!remaining.empty()) {
        std::size_t consumed = 0;
        ByteSpan frame;
        std::string frame_error;
        const FrameStatus status = framer->next(remaining, consumed, frame, frame_error);
        if (status == FrameStatus::NeedMore) {
            break;
        }
        if (status == FrameStatus::Error) {
            error = frame_error;
            return false;
        }
        if (consumed == 0) {
            error = "framer made no progress";
            return false;
        }

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
        analysis.score.bytes += consumed;

        for (const ParsedField& field : message.fields) {
            Aggregate& aggregate = analysis.fields[field.number];
            aggregate.wire_counts[field.wire_type]++;
            aggregate.occurrences++;
        }

        remaining = remaining.subspan(consumed);
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
            break;
        }
        if (consumed == 0) {
            error = "raw parser made no progress";
            return false;
        }

        ++analysis.score.frames;
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
             << format_score(candidate.score) << " frames=" << candidate.frames
             << " coverage=" << format_score(
                    sample_bytes_ > 0
                        ? static_cast<double>(candidate.bytes) /
                              static_cast<double>(sample_bytes_)
                        : 0.0);
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

    if (!force && !timed_out && !(best != nullptr && (enough_bytes || enough_frames))) {
        return false;
    }

    if (best == nullptr) {
        CandidateScore fallback;
        fallback.spec.kind = FramingKind::Raw;
        fallback.label = "raw";
        fallback.reason = "no candidate passed";
        lock(fallback, 0.05, true);
        return true;
    }

    double margin = best->score;
    if (candidates_.size() >= 2) {
        for (const CandidateScore& candidate : candidates_) {
            if (&candidate == best) {
                continue;
            }
            if (candidate.valid) {
                margin = best->score - candidate.score;
                break;
            }
        }
    }

    const double sample_factor =
        std::clamp(0.35 + 0.65 * std::min(1.0, static_cast<double>(best->frames) /
                                                   static_cast<double>(std::max<std::size_t>(
                                                       1, learning_frames_))),
                   0.35, 1.0);
    const double margin_factor =
        0.65 + 0.35 * std::clamp(margin / 0.15, 0.0, 1.0);
    double confidence = std::clamp(best->score * sample_factor * margin_factor, 0.05, 0.99);
    if (best->spec.kind == FramingKind::Raw) {
        confidence = std::min(confidence, 0.80);
    }

    lock(*best, confidence, false);
    return true;
}

} // namespace pr
