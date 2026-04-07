#include "mos/vis/nlu/nlu_engine.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "mos/vis/common/logging.h"

namespace mos::vis {
namespace {

using json = nlohmann::json;

struct Rule {
  std::string intent;
  std::vector<std::string> keywords;
  std::string reply_text;
  float confidence = 0.8F;
};

std::string LowerAscii(std::string s) {
  std::transform(s.begin(),
                 s.end(),
                 s.begin(),
                 [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return s;
}

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

std::string RemoveAsciiWhitespace(std::string s) {
  s.erase(std::remove_if(s.begin(),
                         s.end(),
                         [](unsigned char ch) { return std::isspace(ch) != 0; }),
          s.end());
  return s;
}

bool ContainsText(const std::string& haystack_lower, const std::string& needle) {
  if (needle.empty()) {
    return false;
  }
  const std::string needle_lower = LowerAscii(needle);
  return haystack_lower.find(needle_lower) != std::string::npos;
}

std::vector<Rule> BuiltinRules() {
  return {
      Rule{"device.control.start_analysis",
           {"开始分析", "开始检测", "开始测试", "启动分析", "启动检测"},
           "好的，开始分析。",
           0.90F},
      Rule{"device.control.stop_analysis",
           {"停止分析", "结束分析", "停止检测", "停止测试", "终止分析"},
           "好的，停止分析。",
           0.90F},
      Rule{"device.control.calibrate",
           {"开始校准", "开始教准", "开始叫准", "校准", "教准", "较准", "叫准"},
           "好的，开始校准。",
           0.92F}
  };
}

Status ParseRulesFromJson(const json& j, std::vector<Rule>* rules) {
  if (rules == nullptr) {
    return Status::InvalidArgument("rules output is null");
  }

  rules->clear();

  const json* items = nullptr;
  if (j.is_array()) {
    items = &j;
  } else if (j.is_object() && j.contains("rules") && j.at("rules").is_array()) {
    items = &j.at("rules");
  } else {
    return Status::InvalidArgument("NLU rules json must be array or object with 'rules' array");
  }

  for (const auto& item : *items) {
    if (!item.is_object()) {
      continue;
    }
    Rule rule;
    rule.intent = item.value("intent", "");
    rule.reply_text = item.value("reply_text", "");
    rule.confidence = item.value("confidence", 0.8F);
    rule.confidence = std::max(0.0F, std::min(1.0F, rule.confidence));
    if (item.contains("keywords") && item.at("keywords").is_array()) {
      for (const auto& kw : item.at("keywords")) {
        if (kw.is_string()) {
          rule.keywords.push_back(kw.get<std::string>());
        }
      }
    }

    if (!rule.intent.empty() && !rule.keywords.empty()) {
      rules->push_back(std::move(rule));
    }
  }

  if (rules->empty()) {
    return Status::InvalidArgument("NLU rules are empty");
  }
  return Status::Ok();
}

Status LoadRulesFromDir(const std::string& model_dir, std::vector<Rule>* rules, std::string* rule_path) {
  if (rules == nullptr) {
    return Status::InvalidArgument("rules output is null");
  }
  if (rule_path == nullptr) {
    return Status::InvalidArgument("rule_path output is null");
  }

  const std::filesystem::path dir(model_dir);
  if (!std::filesystem::exists(dir)) {
    return Status::NotFound("NLU model_dir not found: " + model_dir);
  }

  const std::vector<std::filesystem::path> candidates = {
      dir / "rules.json",
      dir / "nlu_rules.json",
      dir / "intent_rules.json",
  };
  for (const auto& p : candidates) {
    if (!std::filesystem::exists(p)) {
      continue;
    }

    std::ifstream ifs(p.string());
    if (!ifs) {
      continue;
    }
    json j;
    try {
      ifs >> j;
    } catch (const std::exception& e) {
      return Status::Internal("failed to parse NLU rules json: " + std::string(e.what()));
    }

    Status st = ParseRulesFromJson(j, rules);
    if (!st.ok()) {
      return st;
    }
    *rule_path = p.string();
    return Status::Ok();
  }

  return Status::NotFound("NLU rules file not found in model_dir: " + model_dir);
}

class SimpleNluEngine final : public NluEngine {
 public:
  Status Initialize(const NluConfig& config) override {
    config_ = config;
    rules_.clear();
    initialized_ = false;

    if (config_.enabled == false) {
      initialized_ = true;
      GetLogger()->info("NLU initialized in disabled mode");
      return Status::Ok();
    }

    if (!config_.model_dir.empty()) {
      std::string loaded_path;
      Status st = LoadRulesFromDir(config_.model_dir, &rules_, &loaded_path);
      if (!st.ok()) {
        return st;
      }
      GetLogger()->info(
          "NLU initialized with rules file: {} (provider={}, threads={})",
          loaded_path,
          config_.provider,
          config_.num_threads);
    } else {
      rules_ = BuiltinRules();
      GetLogger()->info(
          "NLU initialized with built-in rules (provider={}, threads={})",
          config_.provider,
          config_.num_threads);
    }

    initialized_ = true;
    return Status::Ok();
  }

  Status Reset() override { return Status::Ok(); }

  Status Infer(const std::string& text, NluResult* result) override {
    if (result == nullptr) {
      return Status::InvalidArgument("NLU result is null");
    }
    result->intent.clear();
    result->confidence = 0.0F;
    result->reply_text.clear();
    result->json.clear();

    if (!initialized_) {
      return Status::Internal("NLU not initialized");
    }
    if (!config_.enabled) {
      return Status::Ok();
    }

    const std::string normalized = TrimAsciiWhitespace(text);
    if (normalized.empty()) {
      return Status::InvalidArgument("NLU input text is empty");
    }
    if (rules_.empty()) {
      return Status::Internal("NLU rules are empty");
    }

    const std::string text_lower = LowerAscii(normalized);
    const std::string compact_text_lower = RemoveAsciiWhitespace(text_lower);
    int best_index = -1;
    std::size_t best_hits = 0;
    float best_conf = 0.0F;

    std::vector<std::string> best_keywords;
    for (std::size_t i = 0; i < rules_.size(); ++i) {
      const Rule& rule = rules_[i];
      std::vector<std::string> hits;
      for (const auto& kw : rule.keywords) {
        const std::string kw_lower = LowerAscii(kw);
        const std::string compact_kw_lower = RemoveAsciiWhitespace(kw_lower);
        if (ContainsText(text_lower, kw) ||
            (!compact_kw_lower.empty() && compact_text_lower.find(compact_kw_lower) != std::string::npos)) {
          hits.push_back(kw);
        }
      }
      if (hits.empty()) {
        continue;
      }
      const float hit_ratio =
          static_cast<float>(hits.size()) / static_cast<float>(std::max<std::size_t>(1U, rule.keywords.size()));
      const float conf = std::max(rule.confidence, hit_ratio);
      if (best_index < 0 || hits.size() > best_hits || (hits.size() == best_hits && conf > best_conf)) {
        best_index = static_cast<int>(i);
        best_hits = hits.size();
        best_conf = conf;
        best_keywords = std::move(hits);
      }
    }

    json out;
    out["text"] = normalized;
    out["matched_keywords"] = best_keywords;

    if (best_index < 0) {
      result->intent = "unknown";
      result->confidence = 0.0F;
      out["intent"] = result->intent;
      out["confidence"] = result->confidence;
      out["reply_text"] = "";
      result->json = out.dump();
      return Status::Ok();
    }

    const Rule& best = rules_[static_cast<std::size_t>(best_index)];
    result->intent = best.intent;
    result->confidence = std::max(0.0F, std::min(1.0F, best_conf));
    result->reply_text = best.reply_text;

    out["intent"] = result->intent;
    out["confidence"] = result->confidence;
    out["reply_text"] = result->reply_text;
    result->json = out.dump();
    return Status::Ok();
  }

 private:
  NluConfig config_;
  std::vector<Rule> rules_;
  bool initialized_ = false;
};

}  // namespace

std::unique_ptr<NluEngine> CreateNluEngine() {
  return std::make_unique<SimpleNluEngine>();
}

}  // namespace mos::vis
