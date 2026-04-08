#include "mos/vis/runtime/stages/asr_stage.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mos/vis/common/logging.h"
#include "mos/vis/runtime/subsm/hotspot_subsms.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "AsrStage";

LogContext MakeLogCtx(const SessionContext& context) {
  LogContext ctx;
  ctx.module = kLogTag;
  ctx.session = context.session_id;
  ctx.turn = context.turn_id;
  ctx.state = std::to_string(static_cast<int>(context.state));
  ctx.req = context.current_control_request_id;
  return ctx;
}

// Helper to trim ASCII whitespace from a string (copied from session_controller.cc)
std::string TrimAsciiWhitespace(const std::string& s) {
  std::size_t start = 0;
  while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start])) != 0) {
    ++start;
  }
  std::size_t end = s.size();
  while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
    --end;
  }
  return s.substr(start, end - start);
}

}  // namespace

AsrStage::AsrStage() = default;

AsrStage::~AsrStage() = default;

Status AsrStage::OnAttach(SessionContext& context) {
  // Initialize audio reader
  reader_ = std::make_unique<AudioReader>(context.ring_buffer.get(), "asr");

  // Cache configuration values for performance
  const AsrConfig& asr_config = context.config.asr;
  asr_chunk_samples_ = static_cast<std::size_t>(std::max(1, asr_config.chunk_samples));
  max_chunks_per_tick_ = 8;  // Fixed as in original

  // Initialize ASR state in context
  auto& asr_state = context.asr_state;
  asr_state.processed_samples = 0;
  asr_state.last_partial_text.clear();
  asr_state.last_text_update_time = std::chrono::steady_clock::now();
  asr_state.no_text_timeout_seconds = 15;
  asr_state.has_pending_final_result = false;
  context.subsm_state.asr = subsm::AsrState::kListening;

  // Initialize NLU control state
  auto& nlu_control = context.nlu_control_state;
  nlu_control.has_pending_asr_final_result = false;
  nlu_control.pending_asr_final_result.reset();
  nlu_control.has_pending_nlu_result = false;
  nlu_control.pending_nlu_result.reset();
  nlu_control.pending_control_text.clear();
  nlu_control.wake_pos_samples.reset();

  // Pre-allocate buffer
  asr_chunk_buffer_.resize(asr_chunk_samples_, 0.0F);

  LogInfo(logevent::kSystemBoot, MakeLogCtx(context),
          {Kv("detail", "asr_stage_init"),
           Kv("chunk_samples", asr_chunk_samples_),
           Kv("timeout_sec", asr_state.no_text_timeout_seconds)});

  return Status::Ok();
}

void AsrStage::OnDetach(SessionContext& context) {
  reader_.reset();
  asr_chunk_buffer_.clear();
}

bool AsrStage::CanProcess(SessionState state) const {
  // ASR stage only processes when we are listening or finalizing
  return state == SessionState::kListening || state == SessionState::kFinalizing;
}

Status AsrStage::Process(SessionContext& context) {
  if (!context.config.asr.enabled || context.asr == nullptr) {
    return Status::Ok();
  }

  // Align ASR reader to the speech-start position captured by Vad2Stage.
  // Without this, ASR may decode stale ring-buffer audio first and miss
  // the beginning of the first command after wake.
  if (context.nlu_control_state.wake_pos_samples.has_value()) {
    const std::uint64_t wake_pos = *context.nlu_control_state.wake_pos_samples;
    const std::uint64_t oldest = context.ring_buffer->OldestPos();
    const std::uint64_t clamped_pos = std::max(oldest, wake_pos);
    reader_->Seek(clamped_pos);
    context.nlu_control_state.wake_pos_samples.reset();
    LogDebug(logevent::kAsrPartial, MakeLogCtx(context),
             {Kv("detail", "asr_reader_seek"),
              Kv("seek_pos", clamped_pos),
              Kv("oldest", oldest)});
  }

  // Check for timeout
  HandleNoTextTimeout(context);
  if (context.state != SessionState::kListening &&
      context.state != SessionState::kFinalizing) {
    // Sub-state machine may have redirected to pre-listening/idle.
    return Status::Ok();
  }

  // Process audio chunks
  Status st = ProcessAudioChunks(context);
  if (!st.ok()) {
    return st;
  }

  // If we are finalizing, handle tail audio and finalize ASR
  if (context.state == SessionState::kFinalizing) {
    const std::size_t tail_samples = static_cast<std::size_t>(
        std::max(0, context.config.asr.tail_ms) *
        std::max(1, context.config.audio.sample_rate) / 1000);
    st = ProcessTailAudio(context, tail_samples);
    if (!st.ok()) {
      return st;
    }
    st = FinalizeAsr(context);
    if (!st.ok()) {
      return st;
    }
  }

  return Status::Ok();
}

void AsrStage::HandleNoTextTimeout(SessionContext& context) {
  const auto now = std::chrono::steady_clock::now();
  auto& asr_state = context.asr_state;
  if ((now - asr_state.last_text_update_time) >=
      std::chrono::seconds(std::max(1, asr_state.no_text_timeout_seconds))) {
    LogWarn(logevent::kAsrTimeout, MakeLogCtx(context),
            {Kv("detail", "no_new_text_timeout"),
             Kv("timeout_sec", asr_state.no_text_timeout_seconds)});
    context.local_events.asr_events.push_back(subsm::AsrEvent::kAsrTimeout);
    ConsumeAsrEvents(context, "");
  }
}

Status AsrStage::ProcessAudioChunks(SessionContext& context) {
  auto& asr_state = context.asr_state;
  std::size_t consumed_chunks = 0;
  while (consumed_chunks < max_chunks_per_tick_ &&
         reader_->Has(asr_chunk_buffer_.size()) &&
         reader_->ReadAndAdvance(asr_chunk_buffer_.data(),
                                 asr_chunk_buffer_.size())) {
    asr_state.processed_samples += asr_chunk_buffer_.size();
    Status st = context.asr->AcceptAudio(asr_chunk_buffer_.data(),
                                         asr_chunk_buffer_.size());
    if (!st.ok()) {
      LogError(logevent::kAsrError, MakeLogCtx(context), {Kv("detail", "accept_failed"), Kv("err", st.message())});
      context.local_events.asr_events.push_back(subsm::AsrEvent::kAsrError);
      ConsumeAsrEvents(context, "");
      break;
    }
    st = context.asr->DecodeAvailable();
    if (!st.ok()) {
      LogError(logevent::kAsrError, MakeLogCtx(context), {Kv("detail", "decode_failed"), Kv("err", st.message())});
      context.local_events.asr_events.push_back(subsm::AsrEvent::kAsrError);
      ConsumeAsrEvents(context, "");
      break;
    }

    AsrResult partial;
    st = context.asr->GetResult(&partial);
    if (st.ok() && !partial.text.empty() && partial.text != asr_state.last_partial_text) {
      asr_state.last_partial_text = partial.text;
      asr_state.last_text_update_time = std::chrono::steady_clock::now();
      LogDebug(logevent::kAsrPartial, MakeLogCtx(context),
               {Kv("text", MaskSummary(partial.text, 16))});
    }
    ++consumed_chunks;
  }
  return Status::Ok();
}

Status AsrStage::ProcessTailAudio(SessionContext& context, std::size_t tail_samples) {
  auto& asr_state = context.asr_state;
  std::size_t extra_samples = 0;
  while (extra_samples < tail_samples &&
         reader_->Has(asr_chunk_buffer_.size()) &&
         reader_->ReadAndAdvance(asr_chunk_buffer_.data(),
                                 asr_chunk_buffer_.size())) {
    asr_state.processed_samples += asr_chunk_buffer_.size();
    extra_samples += asr_chunk_buffer_.size();
    Status st = context.asr->AcceptAudio(asr_chunk_buffer_.data(),
                                         asr_chunk_buffer_.size());
    if (!st.ok()) {
      LogError(logevent::kAsrError, MakeLogCtx(context), {Kv("detail", "accept_failed_tail"), Kv("err", st.message())});
      break;
    }
    st = context.asr->DecodeAvailable();
    if (!st.ok()) {
      LogError(logevent::kAsrError, MakeLogCtx(context), {Kv("detail", "decode_failed_tail"), Kv("err", st.message())});
      break;
    }
  }
  return Status::Ok();
}

Status AsrStage::FinalizeAsr(SessionContext& context) {
  AsrResult final_result;
  Status st = context.asr->FinalizeAndFlush(&final_result);
  if (!st.ok()) {
    LogError(logevent::kAsrError, MakeLogCtx(context), {Kv("detail", "finalize_failed"), Kv("err", st.message())});
    return st;
  }

  std::string final_text = TrimAsciiWhitespace(final_result.text);
  if (final_text.empty()) {
    const std::string partial_text = TrimAsciiWhitespace(context.asr_state.last_partial_text);
    if (!partial_text.empty()) {
      final_result.text = partial_text;
      final_text = partial_text;
    }
  }

  if (final_text.empty()) {
    LogWarn(logevent::kAsrFinalEmpty, MakeLogCtx(context), {Kv("detail", "final_text_empty")});
    context.local_events.asr_events.push_back(subsm::AsrEvent::kAsrFinalEmpty);
    ConsumeAsrEvents(context, "");
    return Status::Ok();
  }

  LogInfo(logevent::kAsrFinal, MakeLogCtx(context),
          {Kv("text", MaskSummary(final_text, 16))});

  context.nlu_control_state.pending_asr_final_result = std::move(final_result);
  context.nlu_control_state.has_pending_asr_final_result = true;
  context.local_events.asr_events.push_back(subsm::AsrEvent::kAsrFinalNonEmpty);
  ConsumeAsrEvents(context, final_text);

  return Status::Ok();
}

void AsrStage::ConsumeAsrEvents(SessionContext& context, const std::string& final_text) {
  auto& events = context.local_events.asr_events;
  while (!events.empty()) {
    const subsm::AsrEvent event = events.front();
    events.pop_front();

    if (context.state == SessionState::kFinalizing) {
      context.subsm_state.asr = subsm::AsrState::kFinalizing;
    } else if (context.state == SessionState::kRecognizing) {
      context.subsm_state.asr = subsm::AsrState::kRecognizing;
    } else {
      context.subsm_state.asr = subsm::AsrState::kListening;
    }

    const subsm::AsrState prev = context.subsm_state.asr;
    const subsm::AsrDecision decision = subsm::StepAsr(prev, event);
    context.subsm_state.asr = decision.next_state;

    LogDebug(logevent::kSubsmTransition, MakeLogCtx(context),
             {Kv("subsm", "asr"),
              Kv("from", static_cast<int>(prev)),
              Kv("ev", static_cast<int>(event)),
              Kv("to", static_cast<int>(decision.next_state)),
              Kv("action", static_cast<int>(decision.action)),
              Kv("final_text_empty", final_text.empty() ? 1 : 0)});

    if (decision.action == subsm::AsrAction::kToRecognizing) {
      context.state = SessionState::kRecognizing;
      context.nlu_control_state.wake_pos_samples.reset();
      context.asr_state.processed_samples = 0;
      context.asr_state.last_partial_text.clear();
      context.asr_state.has_pending_final_result = false;
      continue;
    }

    if (decision.action == subsm::AsrAction::kPrepareRetryReply) {
      if (context.asr != nullptr && context.config.asr.enabled) {
        Status st = context.asr->Reset();
        if (!st.ok()) {
          GetLogger()->warn("{}: ASR reset failed in retry path: {}", kLogTag, st.message());
        }
      }
      context.nlu_control_state.wake_pos_samples.reset();
      context.asr_state.processed_samples = 0;
      context.asr_state.last_partial_text.clear();
      context.asr_state.has_pending_final_result = false;
      context.nlu_control_state.has_pending_asr_final_result = false;
      context.nlu_control_state.pending_asr_final_result.reset();
      context.state = SessionState::kPreListening;
      ScheduleRetryReply(context);
      continue;
    }
  }
}

void AsrStage::ScheduleRetryReply(SessionContext& context) {
  context.keep_session_open = true;
  context.reply_tts_started = false;
  ++context.reply_playback_token;
  context.tts_state.tasks.push_back(TtsTask{"", "没听清，请再说一次。"});
}

}  // namespace mos::vis
