
#pragma once
#include <string>

namespace mos::vis {

class Status {
 public:
  enum class Code {
    kOk = 0,
    kInvalidArgument,
    kNotFound,
    kInternal,
  };

  Status() = default;
  Status(Code code, std::string message);

  static Status Ok();
  static Status InvalidArgument(std::string message);
  static Status NotFound(std::string message);
  static Status Internal(std::string message);

  bool ok() const;
  Code code() const;
  const std::string& message() const;

 private:
  Code code_ = Code::kOk;
  std::string message_;
};

}  // namespace mos::vis
