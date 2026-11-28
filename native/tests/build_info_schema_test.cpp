#include "../downloader.cpp"

#include <iostream>
#include <string>

int main() {
  if (detectBuildInfoSchema("Branch Build Key CDN Servers Product") != 1) {
    std::cerr << "modern schema not detected\n";
    return 1;
  }
  if (detectBuildInfoSchema("Branch Install Key KeyService Product") != 0) {
    std::cerr << "legacy schema not detected\n";
    return 1;
  }
  if (detectBuildInfoSchema("Branch KeyService CDN Servers Product") != -1) {
    std::cerr << "ambiguous schema should not be detected\n";
    return 1;
  }
  if (detectBuildInfoSchema("unrelated executable strings") != -1) {
    std::cerr << "unrelated executable should not be detected\n";
    return 1;
  }
  if (!isClientExecutableName("x64/Diablo III64.exe") ||
      isClientExecutableName("InspectorReporter/BlizzardError.exe") ||
      isClientExecutableName("Diablo III Launcher.exe")) {
    std::cerr << "client executable filter failed\n";
    return 1;
  }
  return 0;
}
