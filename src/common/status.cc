
#include "mos/vis/common/status.h"

namespace mos::vis {

Status::Status(Code code, std::string message)
    : code_(code), message_(std::move(message)) {}

Status Status::Ok() { return Status(); }
Status Status::InvalidArgument(std::string message) {
  return Status(Code::kInvalidArgument, std::move(message));
}
Status Status::NotFound(std::string message) {
  return Status(Code::kNotFound, std::move(message));
}
Status Status::Internal(std::string message) {
  return Status(Code::kInternal, std::move(message));
}

bool Status::ok() const { return code_ == Code::kOk; }
Status::Code Status::code() const { return code_; }
const std::string& Status::message() const { return message_; }

}  // namespace mos::vis
