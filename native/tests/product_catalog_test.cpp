#include "downloader.h"
#include "../downloader.cpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <curl/curl.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

int main() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  fs::path cache = fs::temp_directory_path() / "blizzget-product-catalog-test";
  std::error_code ec;
  fs::remove_all(cache, ec);
  fs::create_directories(cache / "catalog");
  std::ofstream(cache / "catalog" / "summary.json") << R"json({
    "success": true,
    "result": {
      "results": [
        {
          "results": [
            {"product":"d3","name":"Diablo III","flags":"versions","encrypted":false},
            {"product":"d3","name":"Duplicate","flags":"versions","encrypted":false},
            {"product":"anbs","name":"Diablo Immortal","flags":"versions","encrypted":false},
            {"product":"scor","name":"Sea of Thieves","flags":"versions","encrypted":false},
            {"product":"d3","name":"CDN only","flags":"cdns","encrypted":false}
          ]
        }
      ]
    }
  })json";

  auto records = Downloader::fetchProductCatalog(cache.string(), [](std::string const&) {});
  if (records.size() != 3) {
    std::cerr << "expected 3 unique version products, got " << records.size() << "\n";
    return 1;
  }
  if (records[0].code != "d3" || records[0].name != "Diablo III" || records[0].encrypted) {
    std::cerr << "catalog sorting or parsing failed\n";
    return 1;
  }
  if (records[1].code != "anbs" || records[2].code != "scor") {
    std::cerr << "catalog order failed\n";
    return 1;
  }

  fs::create_directories(cache / "cdn-paths" / "d3t");
  std::ofstream(cache / "cdn-paths" / "d3t" / "us.json") << R"json({
    "success": true,
    "result": {
      "data": [
        {"region":"us","path":"tpr/diablo3"},
        {"region":"eu","path":"tpr/other"}
      ]
    }
  })json";
  if (Downloader::fetchCdnPath("d3t", "us", cache.string()) != "diablo3") {
    std::cerr << "CDN path resolution failed\n";
    return 1;
  }

  std::string currentVersions = R"json({
    "success": true,
    "result": {
      "data": [
        {"region":"us","cdn_config":"6547fc64ca796c7c1b25864145814ace"},
        {"region":"eu","cdn_config":"66a853b5852c8be22c0e6b8cfc8a6587"}
      ]
    }
  })json";
  if (parseCurrentCdnConfig(currentVersions, "us") != "6547fc64ca796c7c1b25864145814ace") return 1;
  if (parseCurrentCdnConfig(currentVersions, "eu") != "66a853b5852c8be22c0e6b8cfc8a6587") return 1;
  if (!parseCurrentCdnConfig(currentVersions, "kr").empty()) return 1;

  Tag windows;
  windows.name = "Windows";
  windows.type = 1;
  windows.mask = {0x80};
  Tag osx;
  osx.name = "OSX";
  osx.type = 1;
  osx.mask = {0x40};
  Tag enGB;
  enGB.name = "enGB";
  enGB.type = 2;
  enGB.mask = {0xC0};
  auto platformMask = selectionMask({windows, osx, enGB}, 2, {"Windows", "enGB"});
  if (!selected(platformMask, 0) || selected(platformMask, 1)) {
    std::cerr << "platform tag selection leaked non-Windows entries\n";
    return 1;
  }
  if (installRelativePath("x64\\Scene\\vcruntime140.dll") != fs::path("x64/Scene/vcruntime140.dll")) {
    std::cerr << "Windows install path was not normalized\n";
    return 1;
  }

  DownloadEntry required;
  required.priority = 0;
  DownloadEntry requiredSecondary;
  requiredSecondary.priority = 1;
  DownloadEntry optional;
  optional.priority = 2;
  if (!downloadEntryNeeded(required, true) || !downloadEntryNeeded(requiredSecondary, true) ||
      downloadEntryNeeded(optional, true) || !downloadEntryNeeded(optional, false)) {
    std::cerr << "optional download priority filter failed\n";
    return 1;
  }
  if (!includeDownloadEntry(optional, false, true, true) ||
      includeDownloadEntry(optional, false, true, false) ||
      !includeDownloadEntry(optional, true, false, false)) {
    std::cerr << "entire-CDN selection override failed\n";
    return 1;
  }

  std::string downloadData;
  auto put16 = [&](uint16_t v) { downloadData.push_back(char(v >> 8)); downloadData.push_back(char(v)); };
  auto put32 = [&](uint32_t v) { for (int i = 3; i >= 0; --i) downloadData.push_back(char(v >> (i * 8))); };
  put16(0x444C);
  downloadData.push_back(char(3));
  downloadData.push_back(char(16));
  downloadData.push_back(char(0));
  put32(1);
  put16(0);
  downloadData.push_back(char(1));
  downloadData.append(4, '\0');
  for (int i = 0; i < 16; ++i) downloadData.push_back(char(i));
  for (int i = 4; i >= 0; --i) downloadData.push_back(char((uint64_t(0x0102030405) >> (i * 8)) & 0xFF));
  downloadData.push_back(char(2));
  downloadData.push_back(char(0));
  File downloadFile = File::memfile(downloadData.data(), downloadData.size(), true);
  Manifest parsedDownload = parseDownloadManifest(downloadFile);
  if (parsedDownload.downloads.size() != 1 || parsedDownload.downloads[0].size != 0x0102030405ULL ||
      parsedDownload.downloads[0].priority != 2 || parsedDownload.downloads[0].flags.size() != 1) {
    std::cerr << "download manifest priority parsing failed\n";
    return 1;
  }

  Manifest manifest;
  for (int i = 0; i < 3; ++i) {
    InstallEntry entry;
    entry.name = "file" + std::to_string(i);
    entry.hash[0] = static_cast<uint8_t>(i + 1);
    manifest.installs.push_back(entry);
  }
  std::vector<uint8_t> installMask{0xFF};
  if (selectedEntryHashes(manifest.installs, installMask, 0).size() != 3) return 1;
  if (selectedEntryHashes(manifest.installs, installMask, 1).size() != 1) return 1;
  if (versionFromBuildName("84161_Win32_2_7_4_RC") != "2.7.4.84161") return 1;

  ProgressSnapshot fileProgress;
  fileProgress.files_total = 10;
  fileProgress.files_done = 4;
  if (progressFraction(fileProgress) != 0.4) return 1;
  fileProgress.bytes_total = 100;
  fileProgress.bytes_done = 25;
  if (progressFraction(fileProgress) != 0.25) return 1;
  fileProgress.casc_files_total = 4;
  fileProgress.casc_files_done = 1;
  fileProgress.bytes_done = 50;
  if (progressFraction(fileProgress) != 0.375) return 1;
  fileProgress.bytes_total = 0;
  fileProgress.files_total = 0;
  if (progressFraction(fileProgress) != 0.25) return 1;
  if (etaFromProgress(0, 4, 2.0) != UINT64_MAX) return 1;
  if (etaFromProgress(1, 4, 2.0) != 6) return 1;
  if (etaFromProgress(4, 4, 2.0) != UINT64_MAX) return 1;
  if (etaFromRate(1, 4, 0.5) != 6) return 1;
  if (etaFromRate(4, 4, 0.5) != UINT64_MAX) return 1;
  if (bytesPerSecond(1000, 2.0) != 500) return 1;
  if (bytesPerSecond(1000, 0.0) != 0) return 1;
  if (cascWorkerCount(48, 662512) != 8) return 1;
  if (cascWorkerCount(2, 100) != 2) return 1;
  fs::path retryPath = "/home/salmon/.hermes/cache/scratch/blizzget-loose-retry-test.bin";
  fs::remove(retryPath);
  std::atomic<int> fetchAttempts{0};
  auto flakyFetch = [&](std::string const& destination, uint64_t offset) {
    if (fetchAttempts.fetch_add(1) == 0) {
      writeBytes(destination, "a");
      throw Exception("temporary transfer failure");
    }
    if (offset != 1) throw Exception("resume offset was not preserved");
    std::ofstream output(destination, std::ios::binary | std::ios::app);
    output << "bc";
  };
  File retried = fetchLooseWithRetry(retryPath.string(), flakyFetch);
  if (fetchAttempts != 2 || readAll(retried) != "abc") return 1;
  fs::remove(retryPath);
  std::atomic<int> failedAttempts{0};
  bool sawFailure = false;
  try {
    fetchLooseWithRetry(retryPath.string(), [&](std::string const&, uint64_t) {
      ++failedAttempts;
      throw Exception("persistent transfer failure");
    });
  } catch (Exception const&) {
    sawFailure = true;
  }
  if (!sawFailure || failedAttempts != LooseFetchAttempts) return 1;
  int listenFd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd < 0) return 1;
  int reuse = 1;
  setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(listenFd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) return 1;
  if (listen(listenFd, 1) != 0) return 1;
  socklen_t addressSize = sizeof address;
  if (getsockname(listenFd, reinterpret_cast<sockaddr*>(&address), &addressSize) != 0) return 1;
  std::thread server([&] {
    int client = accept(listenFd, nullptr, nullptr);
    if (client < 0) return;
    std::string response = "HTTP/1.1 200 OK\r\nContent-Length: 16\r\nConnection: keep-alive\r\n\r\nconnection reuse";
    for (int i = 0; i < 2; ++i) {
      char buffer[1024];
      std::string request;
      while (request.find("\r\n\r\n") == std::string::npos) {
        ssize_t count = read(client, buffer, sizeof buffer);
        if (count <= 0) break;
        request.append(buffer, static_cast<size_t>(count));
      }
      if (request.empty()) break;
      if (send(client, response.data(), response.size(), MSG_NOSIGNAL) != static_cast<ssize_t>(response.size())) break;
    }
    close(client);
  });
  std::string localUrl = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port)) + "/";
  fs::path downloadDest = "/home/salmon/.hermes/cache/scratch/blizzget-http-download-dest.bin";
  for (int i = 0; i < 2; ++i) {
    HttpRequest request(localUrl);
    if (!request.download(downloadDest.string())) { std::cerr << "http download failed: " << request.error() << "\n"; return 1; }
    File downloaded(downloadDest.string());
    if (readAll(downloaded) != "connection reuse") { std::cerr << "http download content mismatch\n"; return 1; }
  }
  server.join();
  close(listenFd);
  fs::remove(downloadDest);

  Location location;
  location.offset = BlockSize - 10;
  location.size = 20;
  std::vector<uint8_t> archiveMask{0x01, 0x00};
  if (archiveRangeReady(location, archiveMask)) { std::cerr << "partial archive range marked ready\n"; return 1; }
  archiveMask[0] |= 0x02;
  if (!archiveRangeReady(location, archiveMask)) { std::cerr << "complete archive range marked pending\n"; return 1; }

  DownloadOptions looseOptions;
  looseOptions.cache_dir = (cache / "loose").string();
  looseOptions.cdn_path = "diablo3";
  std::string looseKey = "9c2b2a86a2fdb8fdc87cc315f1b5c1cb";
  MemoryFile remote;
  remote.write("loose-blte", 10);
  remote.seek(0);
  File loose = loadCachedBlte(looseKey, looseOptions, {}, {}, [&](std::string const& key, std::string const& destination, uint64_t) {
    if (key == looseKey) {
      remote.seek(0);
      writeBytes(destination, readAll(remote));
    }
  });
  if (!loose || readAll(loose) != "loose-blte") return 1;
  if (!fs::exists(cachePath(looseOptions, looseKey))) return 1;

  fs::path resumeFile = cache / "resume.bin";
  writeBytes(resumeFile.string(), "abc");
  Hash resumeHash{};
  NGDP::from_string(resumeHash, "900150983cd24fb0d6963f7d28e17f72");
  if (!extractedFileMatches(resumeFile, resumeHash, 3)) { std::cerr << "valid extracted file did not match\n"; return 1; }
  if (extractedFileMatches(resumeFile, resumeHash, 4)) { std::cerr << "wrong-size extracted file matched\n"; return 1; }
  writeBytes(resumeFile.string(), "abd");
  if (extractedFileMatches(resumeFile, resumeHash, 3)) { std::cerr << "changed extracted file matched\n"; return 1; }
  return 0;
}
