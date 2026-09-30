#include "stingray/binary_writer.h"
#include <fstream>

namespace dtglb::stingray {
void BinaryWriter::bytes(const void* p, std::size_t n) {
    const auto* b = static_cast<const std::uint8_t*>(p);
    data_.insert(data_.end(), b, b + n);
}
void BinaryWriter::blob(const std::vector<std::uint8_t>& v) {
    u32(static_cast<std::uint32_t>(v.size()));
    if (!v.empty()) bytes(v.data(), v.size());
}
bool BinaryWriter::save(const std::filesystem::path& path, std::string& error) const {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot open output: " + path.string(); return false; }
    f.write(reinterpret_cast<const char*>(data_.data()), static_cast<std::streamsize>(data_.size()));
    if (!f) { error = "failed writing output: " + path.string(); return false; }
    return true;
}
}
