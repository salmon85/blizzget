#pragma once

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "file.h"

class HttpRequest {
public:
  enum RequestType { GET, POST };

  HttpRequest(std::string const& url, RequestType type = GET);

  void addHeader(std::string const& name, std::string const& value);
  void addHeader(std::string const& header);
  void addData(std::string const& key, std::string const& value);
  void setTimeout(long seconds) { timeout_ = seconds; }
  void setProgressCallback(std::function<void(uint64, uint64)> callback) {
    progress_ = std::move(callback);
  }

  bool send();
  bool download(std::string const& path, uint64_t resumeOffset = 0);
  uint32 status();
  std::string const& error() const { return error_; }
  std::map<std::string, std::string> headers();
  File response();

  static File get(std::string const& url);

private:
  bool perform(std::string* body, FILE* output);
  std::string url_;
  RequestType type_;
  std::vector<std::string> headers_;
  std::string post_;
  long status_ = 0;
  long timeout_ = 60;
  std::map<std::string, std::string> responseHeaders_;
  std::string body_;
  std::string error_;
  std::function<void(uint64, uint64)> progress_;
};
