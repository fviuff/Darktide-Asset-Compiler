#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>

namespace dtglb::stingray::package {
struct PackageEntry { std::string engine_type; std::string name; };
std::vector<std::uint8_t> build_package_body(std::vector<PackageEntry> entries);
std::vector<std::uint8_t> build_package_blob(const std::string& package_name,std::vector<PackageEntry> entries);
bool validate_package_blob(const std::vector<std::uint8_t>& blob,std::string& error);
bool write_package_blob(const std::string& package_name,std::vector<PackageEntry> entries,const std::filesystem::path&path,std::string&error);
}
