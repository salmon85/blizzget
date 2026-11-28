#include "http.h"
#include "common.h"

#include <curl/curl.h>
#include <algorithm>
#include <cctype>

namespace {
size_t writeBody(char* ptr, size_t size, size_t count, void* userdata) {
  auto* body = static_cast<std::string*>(userdata);
  body->append(ptr, size * count);
  return size * count;
}

size_t writeHeaders(char* ptr, size_t size, size_t count, void* userdata) {
  auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
  std::string line(ptr, size * count);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  size_t colon = line.find(':');
  if (colon != std::string::npos) {
    (*headers)[trim(line.substr(0, colon))] = trim(line.substr(colon + 1));
  }
  return size * count;
}

int transferProgress(void* userdata, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
  auto* callback = static_cast<std::function<void(uint64, uint64)>*>(userdata);
  if (callback && *callback) (*callback)(static_cast<uint64>(now), static_cast<uint64>(total));
  return 0;
}

std::string urlencode(std::string const& str) {
  std::string dst;
  char hex[4];
  for (unsigned char c : str) {
    if (std::isalnum(c)) {
      dst.push_back(c);
    } else if (c == ' ') {
      dst.push_back('+');
    } else {
      snprintf(hex, sizeof hex, "%%%02X", c);
      dst.append(hex);
    }
  }
  return dst;
}
}

HttpRequest::HttpRequest(std::string const& url, RequestType type)
  : url_(url), type_(type) {}

void HttpRequest::addHeader(std::string const& name, std::string const& value) {
  headers_.push_back(name + ": " + value);
}

void HttpRequest::addHeader(std::string const& header) {
  headers_.push_back(header);
}

void HttpRequest::addData(std::string const& key, std::string const& value) {
  if (!post_.empty()) post_.push_back('&');
  post_.append(urlencode(key));
  post_.push_back('=');
  post_.append(urlencode(value));
}

namespace {
size_t writeFile(char* ptr, size_t size, size_t count, void* userdata) {
  auto* output = static_cast<FILE*>(userdata);
  size_t bytes = size * count;
  return fwrite(ptr, 1, bytes, output) == bytes ? bytes : 0;
}

struct ThreadCurl {
  CURL* handle = nullptr;
  ~ThreadCurl() {
    if (handle) curl_easy_cleanup(handle);
  }
};

ThreadCurl& threadCurl() {
  static thread_local ThreadCurl curl;
  return curl;
}
}

bool HttpRequest::perform(std::string* body, FILE* output) {
  CURL* curl = threadCurl().handle;
  if (!curl) {
    curl = curl_easy_init();
    if (!curl) return false;
    threadCurl().handle = curl;
  } else {
    curl_easy_reset(curl);
  }

  if (body) body->clear();
  responseHeaders_.clear();
  status_ = 0;
  curl_easy_setopt(curl, CURLOPT_URL, url_.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "BlizzGet-Native/1.2");
  curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 8L);
  if (output) {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, output);
  } else {
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
  }
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, writeHeaders);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &responseHeaders_);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

  curl_slist* list = nullptr;
  for (auto const& header : headers_) list = curl_slist_append(list, header.c_str());
  if (list) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
  if (type_ == POST) {
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_.data());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_.size()));
  }
  if (progress_) {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transferProgress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_);
  } else {
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
  }

  CURLcode result = curl_easy_perform(curl);
  error_ = curl_easy_strerror(result);
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status_);
  if (list) curl_slist_free_all(list);
  return result == CURLE_OK;
}

bool HttpRequest::send() {
  return perform(&body_, nullptr);
}

bool HttpRequest::download(std::string const& path, uint64_t resumeOffset) {
  FILE* output = fopen(path.c_str(), resumeOffset ? "ab" : "wb");
  if (!output) return false;
  if (resumeOffset) addHeader("Range", "bytes=" + std::to_string(resumeOffset) + "-");
  bool ok = perform(nullptr, output);
  ok = fclose(output) == 0 && ok;
  return ok;
}

uint32 HttpRequest::status() {
  return static_cast<uint32>(status_);
}

std::map<std::string, std::string> HttpRequest::headers() {
  return responseHeaders_;
}

File HttpRequest::response() {
  return File::memfile(body_.data(), body_.size(), true);
}

File HttpRequest::get(std::string const& url) {
  HttpRequest request(url);
  if (!request.send() || request.status() != 200) return File();
  return request.response();
}
