#include "downloader.h"

#include <iostream>
#include <string>

int main(int argc, char** argv) {
  DownloadOptions options;
  options.product = "d3";
  options.region = "eu";
  options.platform = "Windows";
  options.arch = "x86_64";
  options.locale = "enUS";
  options.build_config = "5194054cfa6480884c9a451b6f1107a3";
  options.cdn_config = "6547fc64ca796c7c1b25864145814ace";
  options.cdn_path = "diablo3";
  options.output_dir = argc > 1 ? argv[1] : "/media/salmon/Games/Games/D3_2.7.4";
  options.cache_dir = argc > 2 ? argv[2] : "/media/salmon/Storage/cascette";
  options.global_connections = 48;
  options.per_host_connections = 12;
  options.skip_optional = false;
  options.entire_cdn = false;

  std::string last;
  Downloader downloader;
  downloader.setCallback([&](ProgressSnapshot const& p) {
    if (!p.log_line.empty() && p.log_line != last) {
      last = p.log_line;
      std::cout << p.log_line << std::endl;
    }
    if (p.finished || p.failed) {
      std::cout << "RESULT finished=" << p.finished << " failed=" << p.failed
                << " files=" << p.files_done << "/" << p.files_total << std::endl;
    }
  });
  try {
    downloader.run(options);
  } catch (std::exception const& e) {
    std::cerr << "EXCEPTION: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
