#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <type_traits>
#include <vector>

namespace dtglb::stingray {
class BinaryWriter {
public:
    template<class T> void pod(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const std::uint8_t*>(&v);
        data_.insert(data_.end(), p, p + sizeof(T));
    }
    void u8(std::uint8_t v) { pod(v); }
    void u16(std::uint16_t v) { pod(v); }
    void u32(std::uint32_t v) { pod(v); }
    void u64(std::uint64_t v) { pod(v); }
    void f32(float v) { pod(v); }
    void bytes(const void* p, std::size_t n);
    void blob(const std::vector<std::uint8_t>& v);
    bool save(const std::filesystem::path& path, std::string& error) const;
    const std::vector<std::uint8_t>& data() const { return data_; }
private:
    std::vector<std::uint8_t> data_;
};
}
