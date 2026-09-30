#pragma once

#include "compiler/resource_key.h"
#include "compiler/target_profile.h"

#include <string>
#include <optional>

namespace dtglb::compiler {

class CompilationContext {
public:
    CompilationContext() = default;

    ResourceKey generated_key(std::string type, std::string local_name) const;

    const std::string& file_base() const noexcept { return file_base_; }
    const TargetProfile& target_profile() const noexcept { return *target_profile_; }

private:
    friend bool make_compilation_context(const std::string&, CompilationContext&, std::string&,
                                         const std::optional<std::string>&);
    explicit CompilationContext(std::string file_base)
        : file_base_(std::move(file_base)), target_profile_(&darktide_target_profile()) {}

    std::string file_base_;
    std::optional<std::string> resource_directory_;
    const TargetProfile* target_profile_ = &darktide_target_profile();
};

bool make_compilation_context(const std::string& input_stem,
                              CompilationContext& out,
                              std::string& error,
                              const std::optional<std::string>& asset_path = std::nullopt);

} // namespace dtglb::compiler
