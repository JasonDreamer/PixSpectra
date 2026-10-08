#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <cmath>

namespace pix {
constexpr uint32_t magic = 0x31535850; // PXS1, little endian, Windows x64 only
constexpr uint32_t version = 2, slots = 128, maxConfig = 16 * 1024 * 1024;
constexpr uint64_t maxPixels = 32 * 1024 * 1024;
#pragma pack(push, 1)
struct ConfigHeader {
    uint32_t tag = magic, ver = version, xmlBytes = 0;
    char plugin[128] = {};
    char labels[slots][32] = {};
    float values[slots] = {};
};
struct Request {
    uint32_t tag = magic, ver = version, width = 0, height = 0, configBytes = 0;
    float values[slots] = {};
    float inputDb = 0, sampleRate = 48000, bpm = 120;
};
struct Response {
    uint32_t tag = magic, ver = version, width = 0, height = 0, status = 0;
    char message[240] = {};
};
#pragma pack(pop)
inline bool validConfig(const void* data, size_t size) {
    if (!data || size < sizeof(ConfigHeader) || size > maxConfig) return false;
    ConfigHeader h; std::memcpy(&h, data, sizeof(h));
    if (h.tag != magic || h.ver != version || size != sizeof(h) + h.xmlBytes) return false;
    if (!std::memchr(h.plugin, 0, sizeof(h.plugin))) return false;
    for (unsigned i=0; i<slots; ++i)
        if (!std::isfinite(h.values[i]) || h.values[i]<0 || h.values[i]>1 ||
            !std::memchr(h.labels[i],0,32)) return false;
    return true;
}
// Version 1 stored 16 slots. Disk IDs 200..215 and their assignments stay intact.
inline std::vector<uint8_t> upgradeConfig(const void* data,size_t size) {
    if(validConfig(data,size)) return {static_cast<const uint8_t*>(data),static_cast<const uint8_t*>(data)+size};
    constexpr size_t oldSize=12+128+16*32+16*4;
    if(!data || size<oldSize || size>maxConfig) throw std::runtime_error("Invalid saved VST state");
    const auto* p=static_cast<const uint8_t*>(data); uint32_t fields[3];std::memcpy(fields,p,12);
    if(fields[0]!=magic || fields[1]!=1 || size!=oldSize+fields[2] || sizeof(ConfigHeader)+fields[2]>maxConfig)
        throw std::runtime_error("Invalid saved VST state");
    ConfigHeader h;h.xmlBytes=fields[2];std::memcpy(h.plugin,p+12,128);
    std::memcpy(h.labels,p+140,16*32);std::memcpy(h.values,p+140+16*32,16*4);
    std::vector<uint8_t> b(sizeof(h)+h.xmlBytes);std::memcpy(b.data(),&h,sizeof(h));
    std::memcpy(b.data()+sizeof(h),p+oldSize,h.xmlBytes);
    if(!validConfig(b.data(),b.size())) throw std::runtime_error("Invalid legacy VST state");
    return b;
}
inline std::vector<uint8_t> emptyConfig() {
    ConfigHeader h;
    std::vector<uint8_t> b(sizeof(h)); std::memcpy(b.data(), &h, sizeof(h)); return b;
}
inline void validate(const Request& r) {
    if (r.tag != magic || r.ver != version || !r.width || !r.height || r.height>8191 ||
        uint64_t(r.width)*r.height > maxPixels || r.configBytes > maxConfig ||
        r.sampleRate != 48000 || !std::isfinite(r.inputDb) || std::abs(r.inputDb)>60 ||
        !std::isfinite(r.bpm) || r.bpm<20 || r.bpm>400) throw std::runtime_error("Invalid render request");
    for (float v : r.values) if (!std::isfinite(v) || v<0 || v>1) throw std::runtime_error("Invalid automation value");
}
}
