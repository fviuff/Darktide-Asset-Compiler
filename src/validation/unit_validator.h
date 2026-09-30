#pragma once
#include <algorithm>
#include <filesystem>
#include <string>

namespace dtglb::validation {
bool validate_unit_v115(const std::filesystem::path& path, std::string& report,
                        const std::string& expected_state_machine_resource = {});
}
