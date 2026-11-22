#include "kernel/Zip.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <zlib.h>

namespace silex::kernel {

namespace {

void put16(std::string& s, uint32_t v)
{
    s.push_back(char(v & 0xff));
    s.push_back(char((v >> 8) & 0xff));
}
void put32(std::string& s, uint32_t v)
{
    put16(s, v & 0xffff);
    put16(s, (v >> 16) & 0xffff);
}
uint32_t get16(const std::string& s, size_t at)
{
    return uint32_t(uint8_t(s[at])) | (uint32_t(uint8_t(s[at + 1])) << 8);
}
uint32_t get32(const std::string& s, size_t at) { return get16(s, at) | (get16(s, at + 2) << 16); }

constexpr uint32_t kDosTime = 0;              // 00:00:00
constexpr uint32_t kDosDate = (46u << 9) | (1u << 5) | 1u;  // 2026-01-01 (fixed: reproducible archives)

std::string deflateRaw(const std::string& in)
{
    z_stream zs{};
    if (deflateInit2(&zs, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw std::runtime_error("zlib: deflateInit2 failed");
    std::string out;
    out.resize(deflateBound(&zs, uLong(in.size())) + 16);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = uInt(in.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uInt(out.size());
    const int rc = deflate(&zs, Z_FINISH);
    deflateEnd(&zs);
    if (rc != Z_STREAM_END)
        throw std::runtime_error("zlib: deflate failed");
    out.resize(zs.total_out);
    return out;
}

}  // namespace

void ZipWriter::add(const std::string& name, const std::string& data)
{
    Entry e;
    e.name = name;
    e.size = data.size();
    e.crc = crc32(0L, reinterpret_cast<const Bytef*>(data.data()), uInt(data.size()));
    e.compressed = deflateRaw(data);
    e.offset = m_body.size();

    put32(m_body, 0x04034b50);
    put16(m_body, 20);  // version needed
    put16(m_body, 0);   // flags
    put16(m_body, 8);   // deflate
    put16(m_body, kDosTime);
    put16(m_body, kDosDate);
    put32(m_body, uint32_t(e.crc));
    put32(m_body, uint32_t(e.compressed.size()));
    put32(m_body, uint32_t(e.size));
    put16(m_body, uint32_t(name.size()));
    put16(m_body, 0);
    m_body += name;
    m_body += e.compressed;
    m_entries.push_back(std::move(e));
}

std::string ZipWriter::finish() const
{
    std::string out = m_body;
    const size_t cdStart = out.size();
    for (const Entry& e : m_entries) {
        put32(out, 0x02014b50);
        put16(out, 20);  // made by
        put16(out, 20);  // needed
        put16(out, 0);
        put16(out, 8);
        put16(out, kDosTime);
        put16(out, kDosDate);
        put32(out, uint32_t(e.crc));
        put32(out, uint32_t(e.compressed.size()));
        put32(out, uint32_t(e.size));
        put16(out, uint32_t(e.name.size()));
        put16(out, 0);  // extra
        put16(out, 0);  // comment
        put16(out, 0);  // disk
        put16(out, 0);  // internal attrs
        put32(out, 0);  // external attrs
        put32(out, uint32_t(e.offset));
        out += e.name;
    }
    const size_t cdSize = out.size() - cdStart;
    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, uint32_t(m_entries.size()));
    put16(out, uint32_t(m_entries.size()));
    put32(out, uint32_t(cdSize));
    put32(out, uint32_t(cdStart));
    put16(out, 0);
    return out;
}

bool zipRead(const std::string& a, const std::string& name, std::string* out)
{
    // locate the end of central directory record
    if (a.size() < 22)
        return false;
    size_t eocd = std::string::npos;
    for (size_t i = a.size() - 22 + 1; i-- > 0;)
        if (get32(a, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos)
        return false;
    const uint32_t count = get16(a, eocd + 10);
    size_t p = get32(a, eocd + 16);
    for (uint32_t k = 0; k < count && p + 46 <= a.size(); ++k) {
        if (get32(a, p) != 0x02014b50)
            return false;
        const uint32_t method = get16(a, p + 10);
        const uint32_t crc = get32(a, p + 16);
        const uint32_t csize = get32(a, p + 20), usize = get32(a, p + 24);
        const uint32_t nlen = get16(a, p + 28), xlen = get16(a, p + 30), clen = get16(a, p + 32);
        const uint32_t off = get32(a, p + 42);
        const std::string n = a.substr(p + 46, nlen);
        p += 46 + nlen + xlen + clen;
        if (n != name)
            continue;
        if (off + 30 > a.size() || get32(a, off) != 0x04034b50)
            return false;
        const size_t data = off + 30 + get16(a, off + 26) + get16(a, off + 28);
        if (data + csize > a.size())
            return false;
        std::string res;
        if (method == 0) {
            res = a.substr(data, csize);
        }
        else if (method == 8) {
            res.resize(usize);
            z_stream zs{};
            if (inflateInit2(&zs, -15) != Z_OK)
                return false;
            zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(a.data() + data));
            zs.avail_in = csize;
            zs.next_out = reinterpret_cast<Bytef*>(res.data());
            zs.avail_out = usize;
            const int rc = inflate(&zs, Z_FINISH);
            inflateEnd(&zs);
            if (rc != Z_STREAM_END)
                return false;
        }
        else {
            return false;
        }
        if (crc32(0L, reinterpret_cast<const Bytef*>(res.data()), uInt(res.size())) != crc)
            return false;
        *out = std::move(res);
        return true;
    }
    return false;
}

}  // namespace silex::kernel
