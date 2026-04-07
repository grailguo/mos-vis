#include "mos/vis/runtime/stages/asr_stage.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "mos/vis/common/logging.h"

namespace mos::vis {

namespace {

constexpr const char* kLogTag = "AsrStage";

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

  GetLogger()->info("{}: Initialized: chunk_samples={} timeout={}s",
                    kLogTag, asr_chunk_samples_, asr_state.no_text_timeout_seconds);

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

  // Check for timeout
  HandleNoTextTimeout(context);
  if (context.state == SessionState::kIdle) {
    // Timeout forced idle; nothing more to do this tick
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
    GetLogger()->warn("{}: no new text for {}s, force session -> kIdle",
                      kLogTag, asr_state.no_text_timeout_seconds);
    if (context.asr != nullptr && context.config.asr.enabled) {
      Status st = context.asr->Reset();
      if (!st.ok()) {
        GetLogger()->warn("{}: ASR reset failed on timeout: {}", kLogTag, st.message());
      }
    }
    context.state = SessionState::kIdle;
    context.nlu_control_state.wake_pos_samples.reset();
    asr_state.processed_samples = 0;
    asr_state.last_partial_text.clear();
    asr_state.has_pending_final_result = false;
    context.nlu_control_state.has_pending_asr_final_result = false;
    context.nlu_control_state.pending_asr_final_result.reset();
    context.nlu_control_state.has_pending_nlu_result = false;
    context.nlu_control_state.pending_nlu_result.reset();
    context.nlu_control_state.pending_control_text.clear();
    GetLogger()->info("{}: wait for awake", kLogTag);
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
      GetLogger()->error("{}: ASR accept failed: {}", kLogTag, st.message());
      break;
    }
    st = context.asr->DecodeAvailable();
    if (!st.ok()) {
      GetLogger()->error("{}: ASR decode failed: {}", kLogTag, st.message());
      break;
    }

    AsrResult partial;
    st = context.asr->GetResult(&partial);
    if (st.ok() && !partial.text.empty() && partial.text != asr_state.last_partial_text) {
      asr_state.last_partial_text = partial.text;
      asr_state.last_text_update_time = std::chrono::steady_clock::now();
      GetLogger()->debug("{}: partial text: {}", kLogTag, partial.text);
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
      GetLogger()->error("{}: ASR accept failed while tailing: {}", kLogTag, st.message());
      break;
    }
    st = context.asr->DecodeAvailable();
    if (!st.ok()) {
      GetLogger()->error("{}: ASR decode failed while tailing: {}", kLogTag, st.message());
      break;
    }
  }
  return Status::Ok();
}

Status AsrStage::FinalizeAsr(SessionContext& context) {
  AsrResult final_result;
  Status st = context.asr->FinalizeAndFlush(&final_result);
  if (!st.ok()) {
    GetLogger()->error("{}: ASR finalize failed: {}", kLogTag, st.message());
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
    GetLogger()->warn("{}: final text is empty, skip recognizing and return to pre‑listening",
                      kLogTag);
    context.state = SessionState::kPreListening;
    GetLogger()->info("{}: wait for instruction", kLogTag);
    context.nlu_control_state.wake_pos_samples.reset();
    context.asr_state.processed_samples = 0;
    context.asr_state.last_partial_text.clear();
    context.asr_state.has_pending_final_result = false;
    return Status::Ok();
  }

  GetLogger()->info("{}: final text: {}", kLogTag, final_text);

  // Store final result in shared context
  context.nlu_control_state.pending_asr_final_result = std::move(final_result);
  context.nlu_control_state.has_pending_asr_final_result = true;
  context.state = SessionState::kRecognizing;
  context.nlu_control_state.wake_pos_samples.reset();
  context.asr_state.processed_samples = 0;
  context.asr_state.last_partial_text.clear();
  context.asr_state.has_pending_final_result = false;

  return Status::Ok();
}

}  // namespace mos::vis