#pragma once
// Minimal ZIP writer (deflate via zlib) for OPC packages such as 3MF.

#include <string>
#include <vector>

namespace silex::kernel {

class ZipWriter
{
public:
    void add(const std::string& name, const std::string& data);
    /// Serialized archive bytes.
    std::string finish() const;

private:
    struct Entry
    {
        std::string name;
        std::string compressed;
        unsigned long crc = 0;
        size_t size = 0;
        size_t offset = 0;
    };
    std::vector<Entry> m_entries;
    std::string m_body;
};

/// Reads one entry back (stored or deflated). Returns false if missing / corrupt. Used by tests.
bool zipRead(const std::string& archive, const std::string& name, std::string* out);

}  // namespace silex::kernel
