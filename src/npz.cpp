#include "npz.h"

#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace tts {

static uint16_t rd16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

Tensor parse_npy(const uint8_t* buf, size_t len) {
    if (len < 10 || buf[0] != 0x93 || std::memcmp(buf + 1, "NUMPY", 5) != 0)
        throw std::runtime_error("npz: bad .npy magic");
    int major = buf[6];
    size_t off = 8;
    uint32_t hlen;
    if (major == 1) {
        hlen = rd16(buf + off);
        off += 2;
    } else {
        hlen = rd32(buf + off);
        off += 4;
    }
    if (off + hlen > len) throw std::runtime_error("npz: header overruns file");
    std::string header(reinterpret_cast<const char*>(buf + off), hlen);
    off += hlen;

    Tensor t;
    // descr like '<f4' / '|b1' / '<i8'
    size_t d = header.find("'descr'");
    if (d == std::string::npos) d = header.find("\"descr\"");
    size_t colon = header.find(':', d);
    size_t q1 = header.find_first_of("'\"", colon);
    size_t q2 = header.find_first_of("'\"", q1 + 1);
    std::string descr = header.substr(q1 + 1, q2 - q1 - 1);
    std::string dtype = descr.size() >= 2 ? descr.substr(descr.size() - 2) : descr;
    if (dtype == "f4") t.dtype = DType::F32;
    else if (dtype == "f8") t.dtype = DType::F64;
    else if (dtype == "i8") t.dtype = DType::I64;
    else if (dtype == "b1") t.dtype = DType::BOOL;
    else if (dtype == "f2") t.dtype = DType::F16;
    else throw std::runtime_error("npz: unsupported dtype " + descr);

    // fortran_order must be False for these archives; we still parse it.
    if (header.find("'fortran_order': True") != std::string::npos)
        throw std::runtime_error("npz: fortran_order not supported");
    if (header.find("False, 'shape'") == std::string::npos &&
        header.find("False, \"shape\"") == std::string::npos) {
        // tolerate different key orderings; shape parse below is authoritative
    }
    size_t s = header.find("'shape'");
    if (s == std::string::npos) s = header.find("\"shape\"");
    size_t lp = header.find('(', s);
    size_t rp = header.find(')', lp);
    if (lp == std::string::npos || rp == std::string::npos)
        throw std::runtime_error("npz: missing shape");
    std::string dims = header.substr(lp + 1, rp - lp - 1);
    std::stringstream ss(dims);
    std::string item;
    while (std::getline(ss, item, ',')) {
        // trim
        size_t a = item.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        size_t b = item.find_last_not_of(" \t");
        t.shape.push_back(std::stoll(item.substr(a, b - a + 1)));
    }
    size_t elem = (t.dtype == DType::F32 || t.dtype == DType::F16) ? (t.dtype == DType::F16 ? 2 : 4)
                  : (t.dtype == DType::F64 || t.dtype == DType::I64) ? 8
                  : 1;
    size_t need = (size_t)t.numel() * elem;
    if (off + need > len) throw std::runtime_error("npz: payload overruns file");
    t.data.assign(buf + off, buf + off + need);
    return t;
}

const Tensor& Npz::operator[](const std::string& name) const {
    for (size_t i = 0; i < names.size(); i++)
        if (names[i] == name) return tensors[i];
    throw std::runtime_error("npz: missing entry " + name);
}

Npz load_npz(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("npz: cannot open " + path);
    f.seekg(0, std::ios::end);
    size_t n = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> buf(n);
    f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)n);
    if (n < 22) throw std::runtime_error("npz: too small " + path);

    // End of central directory: scan backwards for 0x06054b50.
    size_t eocd = std::string::npos;
    for (size_t i = n - 22 + 1; i-- > 0;) {
        if (rd32(&buf[i]) == 0x06054b50) { eocd = i; break; }
        if (n - i > 66000) break;
    }
    if (eocd == std::string::npos) throw std::runtime_error("npz: no EOCD");
    uint16_t count = rd16(&buf[eocd + 10]);
    uint32_t cd_off = rd32(&buf[eocd + 16]);

    Npz out;
    size_t p = cd_off;
    for (uint16_t i = 0; i < count; i++) {
        if (p + 46 > n || rd32(&buf[p]) != 0x02014b50)
            throw std::runtime_error("npz: bad central directory");
        uint16_t method = rd16(&buf[p + 10]);
        uint32_t csize = rd32(&buf[p + 20]);
        uint16_t nlen = rd16(&buf[p + 28]);
        uint16_t elen = rd16(&buf[p + 30]);
        uint16_t clen = rd16(&buf[p + 32]);
        uint32_t lho = rd32(&buf[p + 42]);
        std::string name(reinterpret_cast<const char*>(&buf[p + 46]), nlen);
        p += 46 + nlen + elen + clen;
        if (name.size() && name.back() == '/') continue;  // directory
        if (method != 0) throw std::runtime_error("npz: entry " + name + " is deflated (unsupported)");
        if (lho + 30 > n || rd32(&buf[lho]) != 0x04034b50)
            throw std::runtime_error("npz: bad local header");
        uint16_t lnlen = rd16(&buf[lho + 26]);
        uint16_t lelen = rd16(&buf[lho + 28]);
        size_t dstart = lho + 30 + lnlen + lelen;
        if (dstart + csize > n) throw std::runtime_error("npz: entry overruns file");
        out.tensors.push_back(parse_npy(&buf[dstart], csize));
        std::string base = name;
        if (base.size() > 4 && base.substr(base.size() - 4) == ".npy")
            base = base.substr(0, base.size() - 4);
        out.names.push_back(base);
    }
    return out;
}

}  // namespace tts
