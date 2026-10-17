#ifndef AURORA_APP_HPP
#define AURORA_APP_HPP

#include "aurora_app.h"

namespace aurora {

class HttpResponse {
 public:
  explicit HttpResponse(::HttpResponse *response = nullptr)
      : response_(response) {}
  ~HttpResponse() { reset(); }
  HttpResponse(const HttpResponse &) = delete;
  HttpResponse &operator=(const HttpResponse &) = delete;
  HttpResponse(HttpResponse &&other) noexcept
      : response_(other.response_) {
    other.response_ = nullptr;
  }
  HttpResponse &operator=(HttpResponse &&other) noexcept {
    if (this != &other) {
      reset();
      response_ = other.response_;
      other.response_ = nullptr;
    }
    return *this;
  }
  explicit operator bool() const { return response_ != nullptr; }
  const ::HttpResponse *get() const { return response_; }
  ::HttpResponse *get() { return response_; }
  void reset(::HttpResponse *response = nullptr) {
    if (response_)
      http_free(response_);
    response_ = response;
  }

 private:
  ::HttpResponse *response_;
};

inline HttpResponse get(const char *url) { return HttpResponse(http_get(url)); }
inline HttpResponse post(const char *url, const char *type, const void *data,
                         size_t size) {
  return HttpResponse(http_post(url, type, data, size));
}

class File {
 public:
  File() : file_{-1} {}
  ~File() { close(); }
  File(const File &) = delete;
  File &operator=(const File &) = delete;
  bool open(const char *path, const char *mode) {
    close();
    return fs_open(&file_, path, mode);
  }
  int read(void *data, size_t size) { return fs_read(&file_, data, size); }
  int write(const void *data, size_t size) {
    return fs_write(&file_, data, size);
  }
  long seek(long offset, int whence) {
    return fs_seek(&file_, offset, whence);
  }
  void close() {
    if (file_.fd >= 0)
      fs_close(&file_);
  }

 private:
  FsFile file_;
};

}  // namespace aurora

#endif
