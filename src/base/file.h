#pragma once

#include <cstdarg>
#include <cstdio>
#include "common.h"
#include "error.h"
#include "path.h"
#include <string>
#ifdef _WIN32
#include <intrin.h>
#endif

#ifdef _WIN32
#define BSWAP16 _byteswap_ushort
#define BSWAP32 _byteswap_ulong
#define BSWAP64 _byteswap_uint64
#else
#define BSWAP16 __builtin_bswap16
#define BSWAP32 __builtin_bswap32
#define BSWAP64 __builtin_bswap64
#endif

class FileBuffer : public RefCounted {
public:
  virtual ~FileBuffer() {}

  virtual int getc() {
    uint8 chr;
    if (read(&chr, 1) != 1) {
      return EOF;
    }
    return chr;
  }
  virtual void putc(int chr) {
    write(&chr, 1);
  }

  virtual uint64 tell() const = 0;
  virtual void seek(int64 pos, int mode) = 0;
  virtual uint64 size() {
    uint64 pos = tell();
    seek(0, SEEK_END);
    uint64 res = tell();
    seek(pos, SEEK_SET);
    return res;
  }

  virtual size_t read(void* ptr, size_t size) = 0;
  virtual size_t write(void const* ptr, size_t size) = 0;
  virtual bool flush() { return true; }
};

class File {
protected:
  FileBuffer* file_;
public:
  File()
    : file_(nullptr)
  {}
  File(FileBuffer* file)
    : file_(file)
  {}
  File(File const& file)
    : file_(file.file_)
  {
    if (file_) file_->addref();
  }
  File(File&& file)
    : file_(file.file_)
  {
    file.file_ = nullptr;
  }

  enum {
    READ = 0,
    REWRITE = 1,
    MODIFY = 2,
  };

  File(char const* name, uint32 mode = READ);
  File(std::string const& name, uint32 mode = READ)
    : File(name.c_str(), mode)
  {}
  ~File() {
    if (file_) file_->release();
  }
  void release() {
    if (file_) file_->release();
    file_ = nullptr;
  }

  File& operator=(File const& file) {
    if (file_ == file.file_) {
      return *this;
    }
    if (file_) file_->release();
    file_ = file.file_;
    if (file_) file_->addref();
    return *this;
  }
  File& operator=(File&& file) {
    if (file_ == file.file_) {
      return *this;
    }
    if (file_) file_->release();
    file_ = file.file_;
    file.file_ = nullptr;
    return *this;
  }

  operator bool() const {
    return file_ != nullptr;
  }

  int getc() {
    return file_ ? file_->getc() : EOF;
  }
  void putc(int chr) {
    write(&chr, 1);
  }

  void seek(int64 pos, int mode = SEEK_SET) {
    if (file_) file_->seek(pos, mode);
  }
  uint64 tell() const {
    return file_ ? file_->tell() : 0;
  }
  uint64 size() {
    return file_ ? file_->size() : 0;
  }

  size_t read(void* dst, size_t size) {
    return file_ ? file_->read(dst, size) : 0;
  }
  template<class T>
  T read() {
    T x{};
    read(&x, sizeof(T));
    return x;
  }
  uint8 read8() {
    return read<uint8>();
  }
  uint16 read16(bool big = false) {
    uint16 x = read<uint16>();
    if (big) x = BSWAP16(x);
    return x;
  }
  uint32 read32(bool big = false) {
    uint32 x = read<uint32>();
    if (big) x = BSWAP32(x);
    return x;
  }
  uint64 read64(bool big = false) {
    uint64 x = read<uint64>();
    if (big) x = BSWAP64(x);
    return x;
  }

  size_t write(void const* ptr, size_t size) {
    if (!file_) throw Exception("write to invalid file");
    size_t written = file_->write(ptr, size);
    if (written != size) throw Exception("short write");
    return written;
  }
  template<class T>
  bool write(T const& x) {
    return write(&x, sizeof(T)) == sizeof(T);
  }
  bool write8(uint8 x) {
    return write(x);
  }
  bool write16(uint16 x, bool big = false) {
    if (big) x = BSWAP16(x);
    return write(x);
  }
  bool write32(uint32 x, bool big = false) {
    if (big) x = BSWAP32(x);
    return write(x);
  }
  bool write64(uint64 x, bool big = false) {
    if (big) x = BSWAP64(x);
    return write(x);
  }

  void flush() {
    if (!file_ || !file_->flush()) throw Exception("failed to flush file");
  }

  void printf(char const* fmt, ...);

  bool getline(std::string& line);

  class LineIterator;
  LineIterator begin();
  LineIterator end();

  static File memfile(void const* ptr, size_t size, bool clone = false);
  File subfile(uint64 offset, uint64 size);

  void copy(File& src, uint64 size = max_uint64);
  void md5(void* digest);
  std::string md5();

  static bool exists(char const* path);
  static bool exists(std::string const& path) {
    return exists(path.c_str());
  }
};

class MemoryFile : public File {
public:
  MemoryFile(size_t initial = 16384, size_t grow = (1 << 20));
  uint8 const* data() const;
  size_t csize() const;
  uint8* reserve(uint32 size);
  void resize(uint32 size);
};

class File::LineIterator {
  friend class File;
  File file_;
  std::string line_;
  LineIterator(File& file) {
    if (file.getline(line_)) {
      file_ = file;
    }
  }
public:
  LineIterator() {}

  std::string const& operator*() {
    return line_;
  }
  std::string const* operator->() {
    return &line_;
  }

  bool operator!=(LineIterator const& it) const {
    return file_ != it.file_;
  }

  LineIterator& operator++() {
    if (!file_.getline(line_)) {
      file_.release();
    }
    return *this;
  }
};

class FileLoader {
  std::string root;
public:
  FileLoader(std::string const& root = path::root())
    : root(root)
  {
  }

  struct SearchResults {
    std::vector<std::string> files;
    std::vector<std::string> folders;
  };

  File load(char const* name) {
    return File(root / name);
  }
  SearchResults search(char const* mask);
};
