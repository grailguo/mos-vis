#include "mos/vis/control/control_engine.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include <boost/asio/connect.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/beast/core/buffers_to_string.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/system/error_code.hpp>
#include <nlohmann/json.hpp>

#include "mos/vis/common/logging.h"
#include "mos/vis/config/app_config.h"

namespace mos::vis {
namespace {
using Json = nlohmann::json;
namespace websocket = boost::beast::websocket;
using tcp = boost::asio::ip::tcp;

constexpr char kMethodAuthorization[] = "authorization";
constexpr char kMethodAuthorizationNotify[] = "authorization_notify";
constexpr char kMethodAuthorizationNotifyV2[] = "system/authorization_notify";
constexpr char kMethodStartCalibration[] = "start_calibration";
constexpr char kMethodStartCalibrationV2[] = "calib/start_calibration";
constexpr char kMethodStartAnalysis[] = "start_analysis";
constexpr char kMethodStartAnalysisV2[] = "analysis/start_analysis";
constexpr char kMethodStopAnalysis[] = "stop_analysis";
constexpr char kMethodStopAnalysisV2[] = "analysis/stop_analysis";
constexpr char kMethodAnalysisNotify[] = "analysis_result_notify";
constexpr char kMethodAnalysisNotifyV2[] = "analysis/analysis_result_notify";
constexpr char kMethodCalibrationNotify[] = "calibration_result_notify";
constexpr char kMethodCalibrationNotifyV2[] = "analysis/calibration_result_notify";

bool IsStartCalibrationMethod(const std::string& method) {
  return method == kMethodStartCalibration || method == kMethodStartCalibrationV2;
}

bool IsStartAnalysisMethod(const std::string& method) {
  return method == kMethodStartAnalysis || method == kMethodStartAnalysisV2;
}

bool IsStopAnalysisMethod(const std::string& method) {
  return method == kMethodStopAnalysis || method == kMethodStopAnalysisV2;
}

bool IsCalibrationNotifyMethod(const std::string& method) {
  return method == kMethodCalibrationNotify || method == kMethodCalibrationNotifyV2;
}

bool IsAnalysisNotifyMethod(const std::string& method) {
  return method == kMethodAnalysisNotify || method == kMethodAnalysisNotifyV2;
}

struct RequestContext {
  std::mutex mutex;
  std::condition_variable cv;
  bool done = false;
  bool success = false;
  bool canceled = false;
  int code = -1;
  std::string message;
  std::string method;
};

struct OutgoingMessage {
  std::string method;
  std::string payload;
  std::shared_ptr<RequestContext> context;
};

struct ActiveTask {
  std::string method;
  std::string uuid;
  std::chrono::steady_clock::time_point start_time;
  std::chrono::steady_clock::time_point eta_time;
};

int ExtractCode(const Json& j) {
  if (j.contains("result") && j.at("result").is_object()) {
    const auto& result = j.at("result");
    if (result.contains("code") && result.at("code").is_number_integer()) {
      return result.at("code").get<int>();
    }
  }
  if (j.contains("error") && j.at("error").is_object()) {
    const auto& error = j.at("error");
    if (error.contains("code") && error.at("code").is_number_integer()) {
      return error.at("code").get<int>();
    }
  }
  return -1;
}

std::string ExtractMessage(const Json& j) {
  if (j.contains("result") && j.at("result").is_object()) {
    const auto& result = j.at("result");
    if (result.contains("message") && result.at("message").is_string()) {
      return result.at("message").get<std::string>();
    }
    if (result.contains("details") && result.at("details").is_string()) {
      return result.at("details").get<std::string>();
    }
  }
  if (j.contains("error") && j.at("error").is_object()) {
    const auto& error = j.at("error");
    if (error.contains("message") && error.at("message").is_string()) {
      return error.at("message").get<std::string>();
    }
    if (error.contains("details") && error.at("details").is_string()) {
      return error.at("details").get<std::string>();
    }
  }
  return "";
}

Json DefaultCalibrationParameter() {
  Json parameter;
  parameter["cartridge_code"] = "CNC-0001";
  parameter["cartridge_info"] = {
      {"spray_voltage_neg", 2000.0},
      {"spray_voltage_pos", 2000.0},
  };
  parameter["scan_data_param"] = {{"save_report", 1}};
  parameter["scan_mode_list"] = Json::array({"normal"});
  parameter["scan_mode_params"] = Json::array(
      {{{"scan_mode", "normal"}, {"sub_params", Json::array({"50,1000"})}}});
  parameter["trap_loc_list"] = Json::array({"q2"});
  parameter["pressure_enabled"] = 0;
  parameter["gain_enabled"] = 1;
  parameter["mass_enabled"] = 1;
  return parameter;
}

Json DefaultAnalysisParameter() {
  return Json::object({{"save_report", 1}});
}

Json ParseParameterOrDefault(const std::string& raw, const Json& fallback) {
  if (raw.empty()) {
    return fallback;
  }
  try {
    Json parsed = Json::parse(raw);
    if (parsed.is_object()) {
      return parsed;
    }
  } catch (const std::exception&) {
  }
  return fallback;
}

std::string DurationHintText(const std::string& method, const ControlConfig& config) {
  if (IsStartCalibrationMethod(method)) {
    return "好的，已开始校准，预计耗时15分钟。";
  }
  if (IsStartAnalysisMethod(method)) {
    const int minutes = std::max(1, config.analysis_duration_sec / 60);
    return "好的，已开始分析，预计耗时" + std::to_string(minutes) + "分钟。";
  }
  if (IsStopAnalysisMethod(method)) {
    return "好的，已发送停止分析指令。";
  }
  return "";
}

std::string CompletionTextForNotify(const std::string& method, int code, const std::string& message) {
  if (IsCalibrationNotifyMethod(method)) {
    if (code == 0) {
      return "校准完成。";
    }
    return message.empty() ? "校准失败。" : ("校准失败：" + message);
  }
  if (IsAnalysisNotifyMethod(method)) {
    if (code == 0) {
      return "分析完成。";
    }
    return message.empty() ? "分析失败。" : ("分析失败：" + message);
  }
  return "";
}

}  // namespace

class SimpleControlEngine final : public ControlEngine {
 public:
  explicit SimpleControlEngine(ControlConfig config)
      : config_(std::move(config)),
        work_guard_(boost::asio::make_work_guard(io_)) {}

  ~SimpleControlEngine() override { Shutdown(); }

  Status Initialize() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) {
      return Status::Ok();
    }
    initialized_ = true;
    io_thread_ = std::thread([this]() { io_.run(); });
    if (config_.enabled && !connecting_ && !connected_) {
      connecting_ = true;
      boost::asio::post(io_, [this]() { DoConnect(); });
      GetLogger()->info("control pre-connect scheduled: ws://{}:{}{}",
                        config_.host,
                        config_.port,
                        config_.path);
    }
    return Status::Ok();
  }

  Status Reset() override { return Status::Ok(); }

  Status Execute(const ControlRequest& request, ControlResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("control result is null");
    }
    result->handled = false;
    result->action.clear();
    result->ws_payload_json.clear();
    result->reply_text.clear();

    if (!initialized_) {
      return Status::Internal("control engine is not initialized");
    }
    if (!config_.enabled) {
      return Status::Ok();
    }

    const std::optional<std::string> method = IntentToMethod(request.intent);
    if (!method.has_value()) {
      return Status::Ok();
    }

    Status st = EnsureConnectedAndAuthorized();
    if (!st.ok()) {
      return st;
    }

    std::string selected_method = method.value();
    Json payload;
    std::shared_ptr<RequestContext> context;
    for (;;) {
      payload = BuildPayload(selected_method);
      context = std::make_shared<RequestContext>();
      context->method = selected_method;

      {
        std::lock_guard<std::mutex> lock(mutex_);
        outgoing_queue_.push_back(OutgoingMessage{selected_method, payload.dump(), context});
      }
      PostWriteLoop();

      const auto timeout = std::chrono::seconds(15);
      if (!WaitRequest(context, timeout)) {
        return Status::Internal("request timeout for method: " + selected_method);
      }
      if (context->success) {
        break;
      }

      const bool unknown_method = (context->code == -32600) || context->message == "Unknown";
      const std::optional<std::string> fallback = AlternativeMethodOnUnknown(selected_method);
      if (unknown_method && fallback.has_value()) {
        GetLogger()->warn("WS method {} rejected (code={}, msg={}), retry with {}",
                          selected_method,
                          context->code,
                          context->message,
                          fallback.value());
        selected_method = fallback.value();
        continue;
      }

      const std::string msg = context->message.empty() ? "control request failed" : context->message;
      return Status::Internal(msg);
    }

    result->handled = true;
    result->action = selected_method;
    result->ws_payload_json = payload.dump();
    result->reply_text = DurationHintText(selected_method, config_);

    if (IsStartCalibrationMethod(selected_method) || IsStartAnalysisMethod(selected_method)) {
      const auto now = std::chrono::steady_clock::now();
      const int duration_sec =
          IsStartCalibrationMethod(selected_method) ? config_.calibration_duration_sec
                                                    : config_.analysis_duration_sec;
      std::string analysis_uuid;
      {
        std::lock_guard<std::mutex> lock(last_response_mutex_);
        if (last_response_.contains("result") && last_response_.at("result").is_object()) {
          const auto& r = last_response_.at("result");
          if (r.contains("analysis_uuid") && r.at("analysis_uuid").is_string()) {
            analysis_uuid = r.at("analysis_uuid").get<std::string>();
          }
        }
      }
      if (!analysis_uuid.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_tasks_[analysis_uuid] = ActiveTask{
            selected_method,
            analysis_uuid,
            now,
            now + std::chrono::seconds(std::max(1, duration_sec)),
        };
      }
    }

    return Status::Ok();
  }

  Status PollNotification(ControlResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("control notify result is null");
    }
    result->handled = false;
    result->action.clear();
    result->ws_payload_json.clear();
    result->reply_text.clear();

    std::lock_guard<std::mutex> lock(mutex_);
    if (notifications_.empty()) {
      return Status::Ok();
    }
    *result = notifications_.front();
    notifications_.pop_front();
    return Status::Ok();
  }

 private:
  std::optional<std::string> IntentToMethod(const std::string& intent) const {
    if (intent == "device.control.calibrate") {
      return kMethodStartCalibration;
    }
    if (intent == "device.control.start_analysis") {
      return kMethodStartAnalysis;
    }
    if (intent == "device.control.stop_analysis") {
      return kMethodStopAnalysis;
    }
    return std::nullopt;
  }

  std::optional<std::string> AlternativeMethodOnUnknown(const std::string& method) const {
    if (method == kMethodStartCalibration) {
      return kMethodStartCalibrationV2;
    }
    if (method == kMethodStartAnalysis) {
      return kMethodStartAnalysisV2;
    }
    if (method == kMethodStopAnalysis) {
      return kMethodStopAnalysisV2;
    }
    return std::nullopt;
  }

  Json BuildPayload(const std::string& method) const {
    Json payload;
    payload["method"] = method;
    if (IsStartCalibrationMethod(method)) {
      payload["parameter"] = ParseParameterOrDefault(
          config_.start_calibration_parameter_json,
          DefaultCalibrationParameter());
      payload["version"] = config_.version;
      return payload;
    }
    if (IsStartAnalysisMethod(method)) {
      payload["parameter"] = ParseParameterOrDefault(
          config_.start_analysis_parameter_json,
          DefaultAnalysisParameter());
      payload["version"] = config_.version;
      return payload;
    }
    if (IsStopAnalysisMethod(method)) {
      payload["version"] = config_.version;
      return payload;
    }
    if (method == kMethodAuthorization) {
      payload["parameter"] = {
          {"client_name", config_.client_name},
          {"client_version", config_.client_version},
          {"client_key", config_.client_key},
          {"compatibility_mode", config_.compatibility_mode},
          {"license_user_name", config_.license_user_name},
      };
      payload["version"] = config_.version;
      return payload;
    }
    return payload;
  }

  Status EnsureConnectedAndAuthorized() {
    std::shared_ptr<RequestContext> auth_context;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!connected_ && !connecting_) {
        connecting_ = true;
        boost::asio::post(io_, [this]() { DoConnect(); });
      }
      const bool connected = state_cv_.wait_for(
          lock,
          std::chrono::seconds(10),
          [this]() { return connected_ || (!connecting_ && !last_error_.empty()); });
      if (!connected_ || !connected) {
        const std::string msg = last_error_.empty() ? "websocket connect timeout" : last_error_;
        return Status::Internal(msg);
      }
      if (authorized_) {
        return Status::Ok();
      }
      if (auth_context_ == nullptr || auth_context_->done || auth_context_->canceled) {
        auth_context_ = std::make_shared<RequestContext>();
        auth_context_->method = kMethodAuthorization;
        outgoing_queue_.push_back(
            OutgoingMessage{kMethodAuthorization, BuildPayload(kMethodAuthorization).dump(), auth_context_});
      }
      auth_context = auth_context_;
    }
    PostWriteLoop();

    const auto timeout = std::chrono::seconds(std::max(1, config_.authorization_timeout_sec));
    if (!WaitRequest(auth_context, timeout)) {
      return Status::Internal("authorization timeout");
    }
    if (!auth_context->success) {
      const std::string msg = auth_context->message.empty() ? "authorization failed" : auth_context->message;
      return Status::Internal(msg);
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      authorized_ = true;
      auth_context_.reset();
    }
    return Status::Ok();
  }

  bool WaitRequest(const std::shared_ptr<RequestContext>& context,
                   std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(context->mutex);
    if (!context->cv.wait_for(lock, timeout, [&context]() { return context->done; })) {
      context->canceled = true;
      {
        std::lock_guard<std::mutex> state_lock(mutex_);
        if (pending_response_ == context) {
          pending_response_.reset();
        }
        auto it = outgoing_queue_.begin();
        while (it != outgoing_queue_.end()) {
          if (it->context == context) {
            it = outgoing_queue_.erase(it);
            continue;
          }
          ++it;
        }
      }
      PostWriteLoop();
      return false;
    }
    return true;
  }

  void CompleteRequest(const std::shared_ptr<RequestContext>& context,
                       bool success,
                       int code,
                       const std::string& message,
                       const Json* response) {
    if (response != nullptr) {
      std::lock_guard<std::mutex> lock(last_response_mutex_);
      last_response_ = *response;
    }

    std::lock_guard<std::mutex> lock(context->mutex);
    if (context->done || context->canceled) {
      return;
    }
    context->done = true;
    context->success = success;
    context->code = code;
    context->message = message;
    context->cv.notify_all();
  }

  void DoConnect() {
    std::string error;
    try {
      tcp::resolver resolver(io_);
      const auto endpoints = resolver.resolve(config_.host, std::to_string(config_.port));
      auto ws = std::make_unique<websocket::stream<tcp::socket>>(io_);
      boost::asio::connect(ws->next_layer(), endpoints.begin(), endpoints.end());
      ws->set_option(websocket::stream_base::timeout::suggested(boost::beast::role_type::client));
      ws->handshake(config_.host, config_.path);

      {
        std::lock_guard<std::mutex> lock(mutex_);
        ws_ = std::move(ws);
        connected_ = true;
        connecting_ = false;
        authorized_ = false;
        auth_context_.reset();
        last_error_.clear();
        auto auth_context = std::make_shared<RequestContext>();
        auth_context->method = kMethodAuthorization;
        auth_context_ = auth_context;
        outgoing_queue_.push_back(
            OutgoingMessage{kMethodAuthorization, BuildPayload(kMethodAuthorization).dump(), auth_context});
      }
      GetLogger()->info("control websocket connected: ws://{}:{}{}",
                        config_.host,
                        config_.port,
                        config_.path);
      state_cv_.notify_all();
      DoReadLoop();
      PostWriteLoop();
      return;
    } catch (const std::exception& e) {
      error = e.what();
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      connected_ = false;
      connecting_ = false;
      authorized_ = false;
      auth_context_.reset();
      last_error_ = "websocket connect failed: " + error;
    }
    GetLogger()->warn("control websocket connect failed: ws://{}:{}{} error={}",
                      config_.host,
                      config_.port,
                      config_.path,
                      error);
    state_cv_.notify_all();
  }

  void PostWriteLoop() {
    boost::asio::post(io_, [this]() { DoWrite(); });
  }

  void DoWrite() {
    OutgoingMessage message;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!connected_ || ws_ == nullptr || write_inflight_) {
        return;
      }
      if (pending_response_ != nullptr) {
        return;
      }
      if (outgoing_queue_.empty()) {
        return;
      }
      message = std::move(outgoing_queue_.front());
      outgoing_queue_.pop_front();
      write_inflight_ = true;
    }
    GetLogger()->info("WS send method={}", message.method);
    if (IsStartCalibrationMethod(message.method)) {
      GetLogger()->info("WS send payload={}", message.payload);
    }

    ws_->async_write(
        boost::asio::buffer(message.payload),
        [this, message](const boost::system::error_code& ec, std::size_t /*bytes*/) {
          if (ec) {
            HandleSocketError("async_write failed: " + ec.message(), message.context);
            return;
          }

          {
            std::lock_guard<std::mutex> lock(mutex_);
            write_inflight_ = false;
            pending_response_ = message.context;
          }
        });
  }

  void DoReadLoop() {
    if (!connected_ || ws_ == nullptr) {
      return;
    }
    ws_->async_read(
        read_buffer_,
        [this](const boost::system::error_code& ec, std::size_t /*bytes*/) {
          if (ec) {
            HandleSocketError("async_read failed: " + ec.message(), nullptr);
            return;
          }

          const std::string text = boost::beast::buffers_to_string(read_buffer_.data());
          read_buffer_.consume(read_buffer_.size());
          DispatchInboundMessage(text);
          DoReadLoop();
        });
  }

  void DispatchInboundMessage(const std::string& text) {
    Json j;
    try {
      j = Json::parse(text);
    } catch (const std::exception& e) {
      GetLogger()->warn("control received invalid json: {}", e.what());
      return;
    }

    const std::string method = j.value("method", "");
    const int code = ExtractCode(j);
    const std::string message = ExtractMessage(j);
    if (!method.empty()) {
      GetLogger()->info("WS recv method={} code={} message={}", method, code, message);
    }

    std::shared_ptr<RequestContext> pending;
    bool completed_pending = false;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (pending_response_ != nullptr) {
        if (!method.empty() && method == pending_response_->method) {
          pending = pending_response_;
          pending_response_.reset();
          completed_pending = true;
        } else if (method == "Unknown" || method.empty()) {
          // Some servers return generic error frames without echoing request method.
          // In single in-flight mode, bind it to the current pending request.
          pending = pending_response_;
          pending_response_.reset();
          completed_pending = true;
        } else if (pending_response_->method == kMethodAuthorization &&
                   (method == kMethodAuthorizationNotify ||
                    method == kMethodAuthorizationNotifyV2)) {
          pending = pending_response_;
          pending_response_.reset();
          completed_pending = true;
        }
      }
      if ((method == kMethodAuthorizationNotify ||
           method == kMethodAuthorizationNotifyV2) &&
          code == 0) {
        authorized_ = true;
        auth_context_.reset();
      }
    }

    if (completed_pending && pending != nullptr) {
      CompleteRequest(pending, code == 0, code, message, &j);
      if (pending->method == kMethodAuthorization && code == 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        authorized_ = true;
        auth_context_.reset();
      }
      PostWriteLoop();
    }

    HandleNotify(j, method, code, message);
  }

  void HandleNotify(const Json& j,
                    const std::string& method,
                    int code,
                    const std::string& message) {
    if (method == kMethodAuthorizationNotify || method == kMethodAuthorizationNotifyV2) {
      if (code != 0) {
        ControlResult result;
        result.handled = true;
        result.action = method;
        result.reply_text = message.empty() ? "控制授权已终止。" : ("控制授权异常：" + message);
        std::lock_guard<std::mutex> lock(mutex_);
        notifications_.push_back(std::move(result));
      }
      return;
    }

    if (!IsCalibrationNotifyMethod(method) && !IsAnalysisNotifyMethod(method)) {
      return;
    }

    std::string analysis_uuid;
    if (j.contains("result") && j.at("result").is_object()) {
      const auto& r = j.at("result");
      if (r.contains("analysis_uuid") && r.at("analysis_uuid").is_string()) {
        analysis_uuid = r.at("analysis_uuid").get<std::string>();
      }
      if (analysis_uuid.empty() && r.contains("data") && r.at("data").is_object()) {
        const auto& data = r.at("data");
        if (data.contains("analysis_uuid") && data.at("analysis_uuid").is_string()) {
          analysis_uuid = data.at("analysis_uuid").get<std::string>();
        }
      }
    }

    ControlResult result;
    result.handled = true;
    result.action = method;
    result.reply_text = CompletionTextForNotify(method, code, message);
    result.ws_payload_json = j.dump();

    std::lock_guard<std::mutex> lock(mutex_);
    if (!analysis_uuid.empty()) {
      active_tasks_.erase(analysis_uuid);
    }
    if (!result.reply_text.empty()) {
      notifications_.push_back(std::move(result));
    }
  }

  void HandleSocketError(const std::string& message,
                         const std::shared_ptr<RequestContext>& context) {
    std::shared_ptr<RequestContext> pending;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      write_inflight_ = false;
      connected_ = false;
      connecting_ = false;
      authorized_ = false;
      last_error_ = message;

      if (pending_response_ != nullptr) {
        pending = pending_response_;
        pending_response_.reset();
      }
    }
    state_cv_.notify_all();

    if (context != nullptr) {
      CompleteRequest(context, false, -1, message, nullptr);
    }
    if (pending != nullptr) {
      CompleteRequest(pending, false, -1, message, nullptr);
    }

    GetLogger()->warn("{}", message);
  }

  void Shutdown() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!initialized_) {
        return;
      }
      initialized_ = false;
    }

    boost::asio::post(io_, [this]() {
      if (ws_ != nullptr) {
        boost::system::error_code ec;
        ws_->close(websocket::close_code::normal, ec);
      }
      work_guard_.reset();
    });

    io_.stop();
    if (io_thread_.joinable()) {
      io_thread_.join();
    }
  }

  ControlConfig config_;
  boost::asio::io_context io_;
  boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_guard_;
  std::thread io_thread_;

  mutable std::mutex mutex_;
  std::condition_variable state_cv_;
  std::unique_ptr<websocket::stream<tcp::socket>> ws_;
  boost::beast::flat_buffer read_buffer_;

  bool initialized_ = false;
  bool connected_ = false;
  bool connecting_ = false;
  bool authorized_ = false;
  bool write_inflight_ = false;

  std::string last_error_;
  std::deque<OutgoingMessage> outgoing_queue_;
  std::shared_ptr<RequestContext> pending_response_;
  std::shared_ptr<RequestContext> auth_context_;
  std::unordered_map<std::string, ActiveTask> active_tasks_;
  std::deque<ControlResult> notifications_;

  std::mutex last_response_mutex_;
  Json last_response_;
};

std::unique_ptr<ControlEngine> CreateControlEngine(const ControlConfig& config) {
  return std::make_unique<SimpleControlEngine>(config);
}

}  // namespace mos::vis
