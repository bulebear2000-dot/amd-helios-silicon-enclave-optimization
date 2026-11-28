// =============================================================================
// shuzi_gen.cpp  -  C++17 stream generator
//
// Byte-identical port of the --stdout mode of shuzi_fuzz_10b.py v8.5
// (same seed + same parameters  =>  exactly the same bytes as the Python tool).
//
// Build : g++ -O2 -std=c++17 -ffp-contract=off -o shuzi_gen shuzi_gen.cpp
// Check : ./shuzi_gen --selftest
// Run   : ./shuzi_gen --total 10000000000 --seed 20261115 --shard 0 | ./your_engine
//         ./shuzi_gen --total 10000000000 --must must.csv | ./your_engine     (all shards)
//
// Options (defaults equal the Python tool):
//   --total N          total records                       (10000000000)
//   --seed S           random seed                         (20261115)
//   --shard-lines N    records per shard                   (50000000)
//   --shard I          generate only shard I               (all shards, in order)
//   --garbage-ratio R  fraction of generation STEPS        (0.20)
//   --distress N --stealth N --softbrick N --satloss N --disaster N   events per shard
//                                                          (100 30 100 100 10)
//   --canary-every N   rescue canary every N records       (1000000, 0 disables)
//   --must FILE        write must-forward list (record,ts) for `--verify` of the Python tool
//   -v                 per-shard progress on stderr
//
// Not included (use the Python tool): truth/coverage files, harness, report, hashing.
// Output goes to stdout in binary; every record is terminated by "\n".
// =============================================================================
#include <algorithm>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <utility>
#include <vector>

using Bytes = std::string;
using Fields = std::vector<Bytes>;

static const char* kVersion = "shuzi_gen v8.5-EXTREME (byte-identical port of shuzi_fuzz_10b.py v8.5-EXTREME)";
static const int64_t BASE_TS = 1731500000000LL;

// ------------------------------------------------------------------ helpers
static Bytes unhex(const char* h) {
    auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; };
    Bytes o;
    for (size_t i = 0; h[i] && h[i + 1]; i += 2) o.push_back((char)(nib(h[i]) * 16 + nib(h[i + 1])));
    return o;
}
static std::vector<Bytes> table(std::initializer_list<const char*> l) {
    std::vector<Bytes> t;
    for (auto s : l) t.push_back(unhex(s));
    return t;
}
static inline Bytes D(int64_t v) { return std::to_string(v); }
static Bytes F1(double v) {  // Python  b"%.1f" % v
    char b[512];
    int n = snprintf(b, sizeof b, "%.1f", v);
    return Bytes(b, (size_t)n);
}
// multiply through a volatile so that no compiler can fuse a*b+c into an FMA
static inline double mulv(double a, double b) { volatile double r = a * b; return r; }

static Bytes join(const Fields& f, size_t from = 0, size_t to = (size_t)-1, const Bytes& sep = ",") {
    if (to > f.size()) to = f.size();
    Bytes o;
    for (size_t i = from; i < to; i++) {
        if (i > from) o += sep;
        o += f[i];
    }
    return o;
}
static Bytes repeat(const Bytes& s, int64_t n) {
    Bytes o;
    if (n > 0) { o.reserve(s.size() * (size_t)n); for (int64_t i = 0; i < n; i++) o += s; }
    return o;
}
static Bytes replace_first(Bytes s, char from, const Bytes& to) {
    size_t p = s.find(from);
    if (p != Bytes::npos) s.replace(p, 1, to);
    return s;
}
static Bytes utf16(const Bytes& ascii, bool le) {  // input is ASCII only
    Bytes o;
    for (unsigned char c : ascii) { if (le) { o.push_back((char)c); o.push_back(0); } else { o.push_back(0); o.push_back((char)c); } }
    return o;
}
static Bytes hexs(const Bytes& b, bool upper) {
    const char* d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    Bytes o;
    for (unsigned char c : b) { o.push_back(d[c >> 4]); o.push_back(d[c & 15]); }
    return o;
}
static Bytes b64(const Bytes& in, bool urlsafe, bool pad) {
    const char* a = urlsafe ? "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
                            : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Bytes o;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8) | (unsigned char)in[i + 2];
        o.push_back(a[v >> 18]); o.push_back(a[(v >> 12) & 63]); o.push_back(a[(v >> 6) & 63]); o.push_back(a[v & 63]);
    }
    if (i + 1 == in.size()) {
        uint32_t v = (unsigned char)in[i] << 16;
        o.push_back(a[v >> 18]); o.push_back(a[(v >> 12) & 63]); if (pad) o += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = ((unsigned char)in[i] << 16) | ((unsigned char)in[i + 1] << 8);
        o.push_back(a[v >> 18]); o.push_back(a[(v >> 12) & 63]); o.push_back(a[(v >> 6) & 63]); if (pad) o += "=";
    }
    return o;
}
// little-endian 32-bit words -> decimal text (Python: b"%d" % int)
static Bytes big_to_dec(std::vector<uint32_t> w) {
    while (!w.empty() && w.back() == 0) w.pop_back();
    if (w.empty()) return "0";
    Bytes digits;
    while (!w.empty()) {
        uint64_t rem = 0;
        for (size_t i = w.size(); i-- > 0;) {
            uint64_t cur = (rem << 32) | w[i];
            w[i] = (uint32_t)(cur / 1000000000ULL);
            rem = cur % 1000000000ULL;
        }
        while (!w.empty() && w.back() == 0) w.pop_back();
        for (int d = 0; d < 9; d++) { digits.push_back((char)('0' + rem % 10)); rem /= 10; }
    }
    while (digits.size() > 1 && digits.back() == '0') digits.pop_back();
    std::reverse(digits.begin(), digits.end());
    return digits;
}
// Python repr(float)
static Bytes py_repr(double v) {
    char buf[64];
    auto res = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::scientific);
    std::string s(buf, res.ptr);
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; }
    size_t epos = s.find('e');
    std::string mant = s.substr(i, epos - i);
    int e10 = atoi(s.c_str() + epos + 1);
    std::string digits;
    for (char c : mant) if (c != '.') digits.push_back(c);
    int decpt = e10 + 1;
    int nd = (int)digits.size();
    std::string out;
    if (decpt <= -4 || decpt > 16) {
        out = digits.substr(0, 1);
        if (nd > 1) { out += '.'; out += digits.substr(1); }
        int ee = decpt - 1;
        char eb[16];
        snprintf(eb, sizeof eb, "e%c%02d", ee < 0 ? '-' : '+', ee < 0 ? -ee : ee);
        out += eb;
    } else if (decpt <= 0) {
        out = "0." + std::string((size_t)-decpt, '0') + digits;
    } else if (decpt >= nd) {
        out = digits + std::string((size_t)(decpt - nd), '0') + ".0";
    } else {
        out = digits.substr(0, (size_t)decpt) + "." + digits.substr((size_t)decpt);
    }
    return neg ? "-" + out : out;
}

// ------------------------------------------------------------------ Python-compatible random.Random
class PyRandom {
    uint32_t mt_[624];
    int mti_;
    void init_genrand(uint32_t s) {
        mt_[0] = s;
        for (mti_ = 1; mti_ < 624; mti_++)
            mt_[mti_] = 1812433253U * (mt_[mti_ - 1] ^ (mt_[mti_ - 1] >> 30)) + (uint32_t)mti_;
    }
    void init_by_array(const std::vector<uint32_t>& key) {
        init_genrand(19650218U);
        size_t i = 1, j = 0, klen = key.size();
        size_t k = 624 > klen ? 624 : klen;
        for (; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525U)) + key[j] + (uint32_t)j;
            i++; j++;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
            if (j >= klen) j = 0;
        }
        for (k = 623; k; k--) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941U)) - (uint32_t)i;
            i++;
            if (i >= 624) { mt_[0] = mt_[623]; i = 1; }
        }
        mt_[0] = 0x80000000U;
    }
public:
    explicit PyRandom(uint64_t seed) {
        std::vector<uint32_t> key;
        if (seed == 0) key.push_back(0);
        else { key.push_back((uint32_t)(seed & 0xFFFFFFFFULL)); if (seed >> 32) key.push_back((uint32_t)(seed >> 32)); }
        init_by_array(key);
    }
    uint32_t u32() {
        if (mti_ >= 624) {
            static const uint32_t mag01[2] = {0U, 0x9908b0dfU};
            int kk;
            uint32_t y;
            for (kk = 0; kk < 624 - 397; kk++) {
                y = (mt_[kk] & 0x80000000U) | (mt_[kk + 1] & 0x7fffffffU);
                mt_[kk] = mt_[kk + 397] ^ (y >> 1) ^ mag01[y & 1U];
            }
            for (; kk < 623; kk++) {
                y = (mt_[kk] & 0x80000000U) | (mt_[kk + 1] & 0x7fffffffU);
                mt_[kk] = mt_[kk + (397 - 624)] ^ (y >> 1) ^ mag01[y & 1U];
            }
            y = (mt_[623] & 0x80000000U) | (mt_[0] & 0x7fffffffU);
            mt_[623] = mt_[396] ^ (y >> 1) ^ mag01[y & 1U];
            mti_ = 0;
        }
        uint32_t y = mt_[mti_++];
        y ^= (y >> 11);
        y ^= (y << 7) & 0x9d2c5680U;
        y ^= (y << 15) & 0xefc60000U;
        y ^= (y >> 18);
        return y;
    }
    double random() {
        uint32_t a = u32() >> 5, b = u32() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }
    uint64_t getrandbits(int k) {  // 1 <= k <= 64
        if (k <= 32) return (uint64_t)(u32() >> (32 - k));
        uint64_t lo = u32();
        int rem = k - 32;
        uint64_t hi = u32();
        if (rem < 32) hi >>= (32 - rem);
        return lo | (hi << 32);
    }
    std::vector<uint32_t> getrandbits_words(int k) {  // arbitrary k >= 1, little-endian words
        size_t words = (size_t)((k - 1) / 32 + 1);
        std::vector<uint32_t> w(words);
        int krem = k;
        for (size_t i = 0; i < words; i++, krem -= 32) {
            uint32_t r = u32();
            if (krem < 32) r >>= (32 - krem);
            w[i] = r;
        }
        return w;
    }
    uint64_t randbelow(uint64_t n) {  // n >= 1
        int k = 64 - __builtin_clzll(n);
        uint64_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }
    int64_t randrange(int64_t n) { return (int64_t)randbelow((uint64_t)n); }
    int64_t randint(int64_t a, int64_t b) { return a + (int64_t)randbelow((uint64_t)(b - a + 1)); }
    double uniform(double a, double b) {
        double d = b - a;
        double m = mulv(d, random());
        return a + m;
    }
    Bytes randbytes(size_t n) {  // n >= 1
        size_t words = (n * 8 - 1) / 32 + 1;
        int krem = (int)(n * 8);
        Bytes o;
        o.reserve(words * 4);
        for (size_t i = 0; i < words; i++, krem -= 32) {
            uint32_t r = u32();
            if (krem < 32) r >>= (32 - krem);
            for (int b = 0; b < 4; b++) o.push_back((char)((r >> (8 * b)) & 0xFF));
        }
        o.resize(n);
        return o;
    }
    template <class T> void shuffle(std::vector<T>& x) {
        for (size_t i = x.size(); i-- > 1;) {
            size_t j = (size_t)randbelow(i + 1);
            std::swap(x[i], x[j]);
        }
    }
};

// ------------------------------------------------------------------ constant tables (generated from the Python tool)

// STANDBY, OK, PRANK, DISTRESS
static const std::vector<Bytes> STATES = table({
    "5354414e444259", "4f4b", "5052414e4b", "4449535452455353",
});

// numeric trap tokens (59)
static const std::vector<Bytes> BAD_NUM = table({
    "4e614e", "6e616e", "2d4e614e", "496e66696e697479", "2d496e66696e697479", "696e66",
    "2d696e66", "2b696e66", "3165393939", "2d3165393939", "31652d393939", "3165",
    "31652b", "35652d333234", "322e32323530373338353835303732303134652d333038", "312e3739373639333133343836323331353765333038", "312e3739373639333133343836323331353965333038", "39323233333732303336383534373735383037",
    "39323233333732303336383534373735383038", "2d39323233333732303336383534373735383038", "2d39323233333732303336383534373735383039", "3138343436373434303733373039353531363135", "3138343436373434303733373039353531363136", "34323934393637323935",
    "34323934393637323936", "32313437343833363438", "2d32313437343833363439", "2d31", "30", "2d30",
    "2d302e30", "2b312e30", "312e", "2e35", "312e322e33", "2d2d31",
    "2d", "30783146", "3062313031", "306f3137", "315f303030", "312c35",
    "3120303030", "20312e30", "312e3020", "09312e30", "3030372e35", "",
    "6e756c6c", "4e554c4c", "4e6f6e65", "74727565", "00", "31002e30",
    "383530002e30", "d9a12ed9a5", "efbc912eefbc95", "e2808b312e30", "313233343536373839303132333435363738393031323334353637383930",
});

// inert strings (14)
static const std::vector<Bytes> NAUGHTY = table({
    "257325732573256e", "2578257825782578", "7b307d7b317d7b327d", "247b6a6e64693a6c6461703a2f2f782f617d", "27204f52202731273d2731", "2e2e2f2e2e2f2e2e2f2e2e2f6574632f706173737764",
    "3c7363726970743e616c6572742831293c2f7363726970743e", "5c5c3f5c433a5c6e756c", "434f4e", "4e554c", "f09f92a9", "eda080",
    "c0af", "efbbbf",
});

// bad state tokens (15)
static const std::vector<Bytes> BAD_STATES = table({
    "", "6f6b", "4f6b", "7374616e646279", "4469737472657373", "444953545245535320",
    "204449535452455353", "444953545245535300", "554e4c4f434b5f425950415353", "41444d494e", "4e554c4c", "30",
    "2d31", "4431535452455353", "e38386e382b9e38388",
});

// bad flag tokens (12)
static const std::vector<Bytes> BAD_FLAGS = table({
    "", "32", "2d31", "39", "393939", "3031",
    "312e30", "74727565", "66616c7365", "4e614e", "39393939393939393939393939393939393939393939393939393939393939393939393939393939", "00",
});

// alternative delimiters (6)
static const std::vector<Bytes> ALT_DELIMS = table({
    "3b", "09", "7c", "20", "1f", "efbc8c",
});

// yo-yo state rows without timestamp (5)
static const std::vector<Bytes> YOYO = table({
    "312e302c3231302e302c2d36352e302c5354414e4442592c30", "302e352c3231302e302c2d38302e302c5052414e4b2c30", "372e392c3835302e302c2d32302e302c44495354524553532c31", "312e322c3231302e302c2d36352e302c4f4b2c30", "382e312c3835302e302c2d31352e302c44495354524553532c31",
});

// injection-looking inert strings (45)
static const std::vector<Bytes> INJECT = table({
    "3d312b31", "2b312b31", "2d312b31", "4053554d28312c3129", "093d312b31", "0d3d312b31",
    "3d48595045524c494e4b282268747470733a2f2f6578616d706c652e696e76616c69642f222c22782229", "0d0a5b494e464f5d206f70657261746f723d61646d696e20726573756c743d4f4b", "0a323032362d30312d30315430303a30303a30305a204552524f5220666f7267656420656e747279", "1b5b324a1b5b48", "253064253061582d466f726765643a2031", "257325732573256e",
    "2578257825782578", "25393939393939393964", "7b307d7b317d7b327d", "7b7b372a377d7d", "247b372a377d", "3c253d20372a3720253e",
    "237b372a377d", "247b6a6e64693a6c6461703a2f2f6578616d706c652e696e76616c69642f617d", "2428696429", "60696460", "3b206c73", "7c20636174202f6465762f6e756c6c",
    "2626206563686f2078", "27204f52202731273d2731", "273b202d2d", "313b2053454c4543542031", "22204f522022223d22", "2e2e2f2e2e2f2e2e2f2e2e2f6574632f706173737764",
    "2e2e5c2e2e5c77696e646f77735c77696e2e696e69", "66696c653a2f2f2f6574632f686f73746e616d65", "5c5c6578616d706c652e696e76616c69645c7368617265", "687474703a2f2f6578616d706c652e696e76616c69642f", "687474703a2f2f3136392e3235342e3136392e3235342f6c61746573742f6d6574612d646174612f", "676f706865723a2f2f6578616d706c652e696e76616c69642f",
    "646174613a746578742f68746d6c3b6261736536342c5047492b65447776596a343d", "3c3f786d6c2076657273696f6e3d22312e30223f3e3c21444f43545950452064205b3c21454e544954592065202278223e5d3e3c643e26653b3c2f643e", "3c215b43444154415b785d5d3e", "3c7363726970743e616c6572742831293c2f7363726970743e", "7b227473223a312c227374617465223a224449535452455353227d", "6e756c6c",
    "756e646566696e6564", "5b5d", "7b7d",
});

// unicode / encoding state variants (21)
static const std::vector<Bytes> UNI_STATES = table({
    "444953e280ae53455254e280ac53", "d09e4b", "4449535452d0955353", "444953e2808b5452455353", "444953e2808d5452455353", "efbca4efbca9efbcb3efbcb4efbcb2efbca5efbcb3efbcb3",
    "4449535452455353cc81", "44c4b0535452455353", "4fe284aa", "efac81", "444953c0af5452455353", "c1814b",
    "e080af4b", "eda080edb080", "efbfbe", "f4908080", "f09f9a91", "444953e280a85452455353",
    "444953e280a95452455353", "444953c2855452455353", "4449535452455353e2808f",
});

// spoof tokens appended by the replay variant (4)
static const std::vector<Bytes> TMP12 = table({
    "484153485f53504f4f465f4445414442454146", "544f4b454e5f5245504c41595f30783030", "", "30303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030303030",
});


static const double BND1[] = {0.5, 0.75, 1.0, 1.25, 4.0, 7.5, 7.9, 8.1};
static const double BND2[] = {210.0, 530.0, 850.0};
static const double BND3[] = {-80.0, -72.5, -65.0, -42.5, -20.0, -15.0};

// ------------------------------------------------------------------ scenario events
// gap kinds:  REL = ms after previous record (may be negative), ABS = literal timestamp text,
//             BACK = event_start_ts - gap, DIV = current_ts // gap, RAW = `rest` is the whole line
enum { K_REL = 0, K_ABS = 1, K_BACK = 2, K_DIV = 3, K_RAW = 4 };
struct EvLine { int kind; int64_t gap; Bytes txt; Bytes rest; bool must; };
using EvList = std::vector<EvLine>;
struct Ev { EvList lines; int64_t spacing; };

static Bytes ROW(double c1, double c2, double c3, const Bytes& st, int fl) {
    return F1(c1) + "," + F1(c2) + "," + F1(c3) + "," + st + "," + D(fl);
}
static EvLine rel(int64_t gap, const Bytes& row, bool must = false) { return EvLine{K_REL, gap, "", row, must}; }

static Ev ev_distress(PyRandom& r) {
    int64_t k = r.randrange(5);
    int64_t n = r.randint(3, 40);
    EvList L;
    static const int64_t g2[5] = {1, 1, 1, 5000, 30000};
    static const int fl4[4] = {0, 1, 1, 2};
    static const int64_t g4[4] = {0, 0, 1, 7};
    for (int64_t i = 0; i < n; i++) {
        Bytes row;
        int64_t gap;
        if (k == 0) {
            double a = r.uniform(7.5, 8.5);
            double b = r.uniform(800, 900);
            double c = r.uniform(-25, -10);
            row = ROW(a, b, c, "DISTRESS", 1);
            gap = (i % 4) ? 100 : 300;
        } else if (k == 1) {
            double f = (double)i / (double)std::max<int64_t>(1, n - 1);
            double c1 = 1.0 + mulv(7.5, f);
            double c2 = 210.0 + mulv(650.0, f);
            double c3 = -65.0 + mulv(50.0, f);
            row = ROW(c1, c2, c3, "DISTRESS", 1);
            gap = r.randint(5, 50);
        } else if (k == 2) {
            double a = r.uniform(7.5, 8.5);
            row = ROW(a, 850.0, -20.0, "DISTRESS", 1);
            gap = g2[r.randbelow(5)];
        } else if (k == 3) {
            double a = r.uniform(-1e3, 1e3);
            double b = r.uniform(-1e4, 1e4);
            double c = r.uniform(-200, 50);
            row = ROW(a, b, c, "DISTRESS", 1);
            gap = r.randint(1, 20);
        } else {
            int fl = fl4[r.randbelow(4)];
            row = ROW(8.1, 850.0, -15.0, "DISTRESS", fl);
            gap = g4[r.randbelow(4)];
        }
        L.push_back(rel(gap, row, k <= 2));
    }
    return Ev{L, 0};
}

static Ev ev_stealth(PyRandom& r) {
    int64_t k = r.randrange(4);
    int64_t n = r.randint(4, 12);
    EvList L;
    for (int64_t i = 0; i < n; i++) {
        Bytes row;
        if (k == 0) {
            double a = r.uniform(.8, 1.5);
            double c = r.uniform(-66, -64);
            Bytes st = (r.randbelow(2) == 0) ? "OK" : "STANDBY";
            row = ROW(a, 210.0, c, st, 1);
        } else if (k == 1) {
            double a = r.uniform(.3, 1.5);
            double b = r.uniform(205, 215);
            double c = r.uniform(-70, -60);
            row = ROW(a, b, c, "DISTRESS", 1);
        } else if (k == 2) {
            double a = r.uniform(.3, .8);
            double c = r.uniform(-85, -78);
            row = ROW(a, 210.0, c, "PRANK", 1);
        } else {
            double a = r.uniform(7.5, 8.5);
            row = ROW(a, 850.0, -20.0, "DISTRESS", 0);
        }
        int64_t g = r.randint(1, 10);
        L.push_back(rel(g, row));
    }
    int64_t spacing = r.randint(50, 5000);
    return Ev{L, spacing};
}

static Ev ev_softbrick(PyRandom& r) {
    EvList L;
    int64_t n1 = r.randint(5, 20);
    for (int64_t i = 0; i < n1; i++) {
        int64_t g = r.randint(1, 10);
        double a = r.uniform(.8, 1.5);
        double c = r.uniform(-66, -64);
        L.push_back(rel(g, ROW(a, 210.0, c, "OK", 0)));
    }
    double fa = r.uniform(.8, 1.5);
    Bytes frozen = ROW(fa, 210.0, -65.0, "OK", 0);
    int64_t n2 = r.randint(10, 60);
    for (int64_t i = 0; i < n2; i++) L.push_back(rel(100, frozen));
    int64_t n3 = r.randint(3, 12);
    for (int64_t i = 0; i < n3; i++) {
        int64_t t = r.randint(0, 100000);
        L.push_back(EvLine{K_ABS, 0, D(t), ROW(0.0, 0.0, 0.0, "BOOT", 0), false});
        L.push_back(rel(1, ROW(0.0, 0.0, 0.0, "STANDBY", 0)));
    }
    int64_t n4 = r.randint(3, 12);
    static const double V[4][3] = {{65535.0, -32768.0, -32768.0}, {0.0, 0.0, 0.0}, {-1.0, -1.0, -1.0}, {3.4e38, 3.4e38, -3.4e38}};
    for (int64_t i = 0; i < n4; i++) {
        int64_t vi = (int64_t)r.randbelow(4);
        int64_t g = r.randint(1, 5);
        L.push_back(rel(g, ROW(V[vi][0], V[vi][1], V[vi][2], "BRICKED", 0)));
    }
    int64_t n5 = r.randint(2, 10);
    for (int64_t i = 0; i < n5; i++) {
        int64_t len = r.randint(1, 120);
        L.push_back(EvLine{K_RAW, 0, "", r.randbytes((size_t)len), false});
    }
    int64_t zg = r.randint(60000, 900000);
    L.push_back(rel(zg, ROW(8.1, 850.0, -15.0, "DISTRESS", 1)));
    return Ev{L, 0};
}

static Ev ev_satloss(PyRandom& r) {
    EvList L;
    int64_t n = r.randint(10, 40);
    for (int64_t i = 0; i < n; i++) {
        double f = (double)i / (double)(n - 1);
        int64_t g = r.randint(5, 50);
        double c1 = mulv(1.2, 1.0 - f);
        double c3 = -65.0 - mulv(50.0, f);
        L.push_back(rel(g, ROW(c1, 210.0, c3, "OK", 0)));
    }
    int64_t out_ms = r.randint(30000, 900000);
    L.push_back(rel(out_ms, ROW(0.0, 0.0, -120.0, "LINK_SAT_LOSS", 1)));
    static const char* LINKS[3] = {"5G", "LTE", "MESH"};
    Bytes link = Bytes("FAILOVER_") + LINKS[r.randbelow(3)];
    int64_t nf = r.randint(4, 20);
    for (int64_t i = 0; i < nf; i++) {
        double c = r.uniform(-90, -60);
        L.push_back(rel(1, ROW(1.0, 210.0, c, (i % 2 == 0) ? link : Bytes("SAT_UP"), 1)));
    }
    EvList backlog;
    int64_t cnt = r.randint(20, 300);
    static const char* ST3[3] = {"OK", "STANDBY", "DISTRESS"};
    for (int64_t i = 0; i < cnt; i++) {
        int64_t back = r.randint(1, out_ms + 60000);
        double a = r.uniform(.8, 1.5);
        double c = r.uniform(-66, -64);
        Bytes st = ST3[r.randbelow(3)];
        backlog.push_back(EvLine{K_BACK, back, "", ROW(a, 210.0, c, st, 0), false});
    }
    int64_t ndup = cnt / 10;
    EvList dups;
    for (int64_t i = 0; i < ndup; i++) dups.push_back(backlog[r.randbelow((uint64_t)cnt)]);
    backlog.insert(backlog.end(), dups.begin(), dups.end());
    r.shuffle(backlog);
    L.insert(L.end(), backlog.begin(), backlog.end());
    if (r.random() < 0.5) {
        int64_t sgn = (r.randbelow(2) == 0) ? -1 : 1;
        int64_t mag = r.randint(1000, 3600000);
        L.push_back(rel(sgn * mag, ROW(1.0, 210.0, -65.0, "OK", 0)));
    }
    if (r.random() < 0.2) {
        int64_t nu = r.randint(3, 10);
        for (int64_t i = 0; i < nu; i++) L.push_back(EvLine{K_DIV, 1000, "", ROW(1.0, 210.0, -65.0, "OK", 0), false});
    }
    return Ev{L, 0};
}

static Ev ev_disaster(PyRandom& r) {
    EvList L;
    Ev a = ev_satloss(r);   L.insert(L.end(), a.lines.begin(), a.lines.end());
    Ev b = ev_distress(r);  L.insert(L.end(), b.lines.begin(), b.lines.end());
    Ev c = ev_softbrick(r); L.insert(L.end(), c.lines.begin(), c.lines.end());
    Ev d = ev_distress(r);  L.insert(L.end(), d.lines.begin(), d.lines.end());
    return Ev{L, 0};
}

enum { E_DISTRESS = 0, E_STEALTH, E_SOFTBRICK, E_SATLOSS, E_DISASTER };
using EvFn = Ev (*)(PyRandom&);
static const EvFn EV_FN[5] = {ev_distress, ev_stealth, ev_softbrick, ev_satloss, ev_disaster};
using Counts = std::vector<std::pair<int, int64_t>>;   // (event type, events per shard) in Python dict order

// ------------------------------------------------------------------ the record generator
class Fuzz {
public:
    PyRandom rng;
    int64_t ts;
    std::deque<Bytes> ring;
    double garbage_ratio;
    double cum[13];
    double tot;

    Fuzz(uint64_t seed, double ratio) : rng(seed), garbage_ratio(ratio) {
        ts = BASE_TS + rng.randint(0, 1000000);
        static const double W[13] = {28, 30, 18, 10, 12, 1.5, 0.5, 8, 5, 6, 6, 3, 0.3};
        double acc = 0.0;
        for (int i = 0; i < 13; i++) { acc += W[i]; cum[i] = acc; }
        tot = acc;
    }

    const Bytes& pick(const std::vector<Bytes>& t) { return t[rng.randbelow(t.size())]; }

    Fields valid_fields() {
        ts += rng.randint(1, 10);
        const Bytes& st = STATES[rng.randrange(4)];
        double c1, c2, c3;
        int fl;
        if (st == "DISTRESS") {
            c1 = rng.uniform(7.5, 8.5); c2 = rng.uniform(800, 900); c3 = rng.uniform(-25, -10); fl = 1;
        } else if (st == "PRANK") {
            c1 = rng.uniform(0.3, 0.8); c2 = 210.0; c3 = rng.uniform(-85, -78); fl = 0;
        } else {
            c1 = rng.uniform(0.8, 1.5); c2 = 210.0; c3 = rng.uniform(-66, -64); fl = 0;
        }
        return Fields{D(ts), F1(c1), F1(c2), F1(c3), st, D(fl)};
    }
    std::vector<Bytes> valid() {
        Bytes line = join(valid_fields());
        ring.push_back(line);
        if (ring.size() > 256) ring.pop_front();
        return {line};
    }

    // ---- numeric traps
    std::vector<Bytes> g_numeric() {
        Fields f = valid_fields();
        int64_t idx = rng.randrange(4);
        double k = rng.random();
        if (k < 0.80) {
            f[idx] = pick(BAD_NUM);
        } else if (k < 0.90) {
            int64_t n = rng.randint(17, 400);
            f[idx] = "7." + Bytes((size_t)n, '9');
        } else if (k < 0.95) {
            double s = rng.random();
            Bytes sign = (s < .5) ? "-" : "";
            int64_t bits = rng.randint(60, 4000);
            Bytes num = big_to_dec(rng.getrandbits_words((int)bits));
            f[idx] = sign + num;
        } else {
            f[idx] = "1e" + D(rng.randint(300, 99999));
        }
        if (rng.random() < 0.15) {
            Bytes val = pick(BAD_NUM);
            int64_t j = rng.randrange(4);
            f[j] = val;
        }
        return {join(f)};
    }

    // ---- structural
    std::vector<Bytes> g_structural() {
        Fields f = valid_fields();
        Bytes v = join(f);
        int64_t k = rng.randrange(26);
        switch (k) {
        case 0: return {""};
        case 1: return {Bytes((size_t)rng.randint(1, 64), ' ')};
        case 2: return {Bytes((size_t)rng.randint(1, 40), ',')};
        case 3: { int64_t p = rng.randint(0, (int64_t)v.size()); return {v.substr(0, (size_t)p)}; }
        case 4: { int64_t m = rng.randint(1, 5); return {join(f, 0, (size_t)m)}; }
        case 5: {
            int64_t n = rng.randint(1, 20);
            Bytes s = v + ",";
            for (int64_t i = 0; i < n; i++) { if (i) s += ","; s += pick(BAD_NUM); }
            return {s};
        }
        case 6: { int64_t n = rng.randint(500, 4096); return {f[0] + Bytes((size_t)n, ',') + join(f, 2)}; }
        case 7: return {replace_first(v, ',', ",\"") + ",x"};
        case 8: return {f[0] + ",\"" + f[1] + "\n" + f[2] + "\"," + join(f, 3)};
        case 9: return {"\"" + f[0] + "\"\",\"\"" + f[1] + "\"," + join(f, 2)};
        case 10: return {unhex("efbbbf") + v};
        case 11: return {v + "\r"};
        case 12: return {replace_first(v, ',', "\r")};
        case 13: {
            int64_t p = rng.randint(0, (int64_t)v.size());
            int64_t n = rng.randint(1, 8);
            return {v.substr(0, (size_t)p) + Bytes((size_t)n, '\0') + v.substr((size_t)p)};
        }
        case 14: {
            int64_t n = rng.randint(1, 24);
            Bytes mid;
            for (int64_t i = 0; i < n; i++) mid.push_back((char)rng.randrange(32));
            return {f[0] + "," + mid + "," + join(f, 2)};
        }
        case 15: return {f[0] + unhex("2c1b5b33316d1b5d303b78072c") + join(f, 2)};
        case 16: return {v + unhex("2cfffec328a0a1eda080f888808080")};
        case 17: { const Bytes& d = pick(ALT_DELIMS); return {join(f, 0, (size_t)-1, d)}; }
        case 18: { int64_t n = rng.randint(1, 300); return {rng.randbytes((size_t)n)}; }
        case 19: {
            int64_t n = rng.randint(1, 300);
            Bytes s;
            for (int64_t i = 0; i < n; i++) s.push_back((char)(32 + rng.randbelow(95)));
            return {s};
        }
        case 20: return {Bytes((size_t)rng.randint(1, 64), '\0')};
        case 21: { int64_t n = rng.randint(1000, 8000); return {v + "," + Bytes((size_t)n, 'A')}; }
        case 22: return {"," + v};
        case 23: return {v + ",,,,"};
        case 24: return {Bytes((size_t)rng.randint(1, 50), '"')};
        default: break;
        }
        double h = rng.random();
        if (h < 0.01) {
            Bytes pad = rng.randbytes(32768);
            for (auto& c : pad) { if (c == 10) c = 'A'; else if (c == 13) c = 'B'; else if (c == 44) c = 'C'; }
            return {v + "," + pad};
        }
        if (h < 0.0102) {
            Bytes pad = rng.randbytes(1048576);
            for (auto& c : pad) { if (c == 10) c = 'A'; else if (c == 13) c = 'B'; else if (c == 44) c = 'C'; }
            return {v + "," + pad};
        }
        int64_t n = rng.randint(100, 2000);
        return {v + "," + Bytes((size_t)n, 'Z')};
    }

    // ---- temporal / replay
    std::vector<Bytes> g_temporal() {
        Fields f = valid_fields();
        int64_t k = rng.randrange(14);
        if (k == 0) f[0] = D(ts - 315360000000LL);
        else if (k == 1) f[0] = D(ts + 3153600000000LL);
        else if (k == 2) f[0] = "0";
        else if (k == 3) f[0] = "-" + D(ts);
        else if (k == 4) f[0] = D(ts / 1000);
        else if (k == 5) f[0] = D(ts * 1000);
        else if (k == 6) f[0] = D(ts * 1000000);
        else if (k == 7) { unsigned long long v = 10000000000000000000ULL + rng.getrandbits(40); f[0] = std::to_string(v); }
        else if (k == 8) f[0] = D(ts) + ".5";
        else if (k == 9) f[0] = "1.7315e12";
        else if (k == 10) { int64_t d = rng.randint(1, 1000000); f[0] = D(ts - d); }
        else if (k == 11 && !ring.empty()) return {ring[rng.randbelow(ring.size())]};
        else if (k == 12) {
            f.push_back(pick(TMP12));
        } else f[0] = D(ts - 1);
        return {join(f)};
    }

    // ---- enum / semantic
    std::vector<Bytes> g_enum() {
        Fields f = valid_fields();
        int64_t k = rng.randrange(8);
        if (k == 0) f[4] = pick(BAD_STATES);
        else if (k == 1) f[5] = pick(BAD_FLAGS);
        else if (k == 2) { f[4] = "DISTRESS"; f[5] = "0"; }
        else if (k == 3) { static const char* S3[3] = {"OK", "STANDBY", "PRANK"}; f[4] = S3[rng.randbelow(3)]; f[5] = "1"; }
        else if (k == 4) { f[4] = "UNLOCK_BYPASS"; f[5] = "999"; }
        else if (k == 5) { Bytes val = pick(NAUGHTY); int64_t j = rng.randrange(6); f[j] = val; }
        else if (k == 6) { int64_t n = rng.randint(200, 4096); f[4] = Bytes((size_t)n, 'S'); }
        else { Bytes a = pick(STATES); Bytes b = pick(NAUGHTY); f[4] = a + b; }
        return {join(f)};
    }

    // ---- byte-level mutation
    std::vector<Bytes> g_mutate() {
        Fields f = valid_fields();
        Bytes v = join(f);
        int64_t reps = rng.randint(1, 4);
        for (int64_t it = 0; it < reps; it++) {
            int64_t k = rng.randrange(6);
            if (v.empty()) break;
            int64_t p = rng.randrange((int64_t)v.size());
            if (k == 0) { int64_t b = rng.randrange(8); v[(size_t)p] = (char)((unsigned char)v[(size_t)p] ^ (1 << b)); }
            else if (k == 1) { int64_t b = rng.randrange(256); v.insert(v.begin() + p, (char)b); }
            else if (k == 2) v.erase((size_t)p, 1);
            else if (k == 3) {
                int64_t s = rng.randrange((int64_t)v.size());
                int64_t m = rng.randint(1, 16);
                Bytes seg = v.substr((size_t)s);
                if ((int64_t)seg.size() > m) seg.resize((size_t)m);
                v.insert((size_t)p, seg);
            }
            else if (k == 4) v[(size_t)p] = ',';
            else {
                Fields parts;
                size_t st = 0;
                for (;;) {
                    size_t q = v.find(',', st);
                    if (q == Bytes::npos) { parts.push_back(v.substr(st)); break; }
                    parts.push_back(v.substr(st, q - st));
                    st = q + 1;
                }
                rng.shuffle(parts);
                v = join(parts);
            }
        }
        return {v};
    }

    // ---- yo-yo
    std::vector<Bytes> g_yoyo() {
        int64_t n = rng.randint(20, 200);
        int64_t t0 = ts + 1;
        int64_t off = rng.randrange((int64_t)YOYO.size());
        std::vector<Bytes> out;
        out.reserve((size_t)n);
        for (int64_t i = 0; i < n; i++) out.push_back(D(t0 + i) + "," + YOYO[(size_t)((i + off) % (int64_t)YOYO.size())]);
        ts = t0 + n;
        return out;
    }

    // ---- identical-line flood
    std::vector<Bytes> g_burst() {
        Bytes line;
        double x = rng.random();
        if (x < 0.7) line = valid()[0];
        else { std::vector<Bytes> s = g_structural(); line = s[rng.randbelow(s.size())]; }
        int64_t n = rng.randint(10, 500);
        return std::vector<Bytes>((size_t)n, line);
    }

    // ---- injection strings
    std::vector<Bytes> g_inject() {
        Fields f = valid_fields();
        int64_t k = rng.randrange((int64_t)INJECT.size());
        int64_t j = rng.randrange(6);
        f[(size_t)j] = INJECT[(size_t)k];
        if (rng.random() < 0.15) {
            Bytes val = pick(INJECT);
            int64_t j2 = rng.randrange(6);
            f[(size_t)j2] = val;
        }
        return {join(f)};
    }

    // ---- unicode / encoding
    std::vector<Bytes> g_unicode() {
        Fields f = valid_fields();
        int64_t n = (int64_t)UNI_STATES.size();
        int64_t k = rng.randrange(n + 4);
        if (k < n) { f[4] = UNI_STATES[(size_t)k]; return {join(f)}; }
        Bytes plain = join(f);
        if (k == n) return {unhex("fffe") + utf16(plain, true)};
        if (k == n + 1) return {unhex("feff") + utf16(plain, false)};
        if (k == n + 2) return {utf16(plain, true)};
        return {join(f, 0, 3) + "," + unhex("efbbbf") + join(f, 3)};
    }

    // ---- threshold boundaries
    std::vector<Bytes> g_boundary() {
        Fields f = valid_fields();
        int64_t idx = 1 + rng.randrange(3);
        double base;
        if (idx == 1) base = BND1[rng.randbelow(8)];
        else if (idx == 2) base = BND2[rng.randbelow(3)];
        else base = BND3[rng.randbelow(6)];
        int64_t mode = rng.randrange(8);
        double v;
        if (mode == 0) v = base;
        else if (mode == 1) v = std::nextafter(base, INFINITY);
        else if (mode == 2) v = std::nextafter(base, -INFINITY);
        else if (mode == 3) v = base + 1e-9;
        else if (mode == 4) v = base - 1e-9;
        else if (mode == 5) v = base + 0.05;
        else if (mode == 6) v = base - 0.05;
        else {
            int64_t ri = rng.randint(-4, 4);
            double t = mulv(std::ldexp(1.0, -52), (double)ri);
            double u = 1.0 + t;
            v = mulv(base, u);
        }
        f[(size_t)idx] = py_repr(v);
        if (rng.random() < 0.3) f[4] = pick(STATES);
        return {join(f)};
    }

    // ---- parser / dialect differentials
    std::vector<Bytes> g_parser() {
        Fields f = valid_fields();
        Bytes v = join(f);
        Bytes v2 = join(valid_fields());
        int64_t k = rng.randrange(21);
        switch (k) {
        case 0: return {v + "\r" + v2};
        case 1: return {v + "\n\r" + v2};
        case 2: return {v + "\x1a" + v2};
        case 3: return {v + "\x04" + v2};
        case 4: { Bytes s = replace_first(v, ',', "\x0c"); s = replace_first(s, ',', "\x0b"); return {s}; }
        case 5: return {f[0] + ",\"" + f[1] + "\\\"," + join(f, 2)};
        case 6: return {f[0] + ",a\"b," + join(f, 2)};
        case 7: return {f[0] + ",x\"," + join(f, 2)};
        case 8: { static const char* P[4] = {"#", "//", ";", "--"}; Bytes p = P[rng.randbelow(4)]; return {p + v}; }
        case 9: return {"sep=;"};
        case 10: return {f[0] + ";" + join(f, 1)};
        case 11: return {unhex("efbbbf")};
        case 12: { int64_t n = rng.randint(10, 2000); return {repeat("\",\"", n)}; }
        case 13: { Bytes s = v; for (auto& c : s) if (c == ',') c = '\0'; return {s}; }
        case 14: return {v + "\\"};
        case 15: return {"\"" + v + "\""};
        case 16: return {"ts,c1,c2,c3,state,flag"};
        case 17: return {"ts,c1,c2,c3,state,flag\n" + v};
        case 18: return {utf16(v, true)};
        case 19: { int64_t n = rng.randint(2, 6); return {v + Bytes((size_t)n, '\n')}; }
        default: return {unhex("efbbbf") + v + Bytes(1, '\0')};
        }
    }

    // ---- token / signature column abuse
    std::vector<Bytes> g_auth() {
        Bytes v = join(valid_fields());
        int64_t k = rng.randrange(10);
        Bytes h = hexs(rng.randbytes(32), false);
        Bytes tok;
        if (k == 0) tok = h;
        else if (k == 1) tok = h.substr(0, h.size() - 1);
        else if (k == 2) tok = h + "0";
        else if (k == 3) tok = hexs(unhex(h.c_str()), true);
        else if (k == 4) tok = "sha256=" + h;
        else if (k == 5) tok = b64(rng.randbytes(32), false, true);
        else if (k == 6) {
            for (int i = 0; i < 3; i++) {
                int64_t len = rng.randint(8, 40);
                Bytes seg = b64(rng.randbytes((size_t)len), true, false);
                if (i) tok += ".";
                tok += seg;
            }
        }
        else if (k == 7) tok = Bytes(64, '0');
        else if (k == 8) tok = "Bearer " + h;
        else tok = unhex("d0b0d0b1d181");
        if (rng.random() < 0.3 && !ring.empty()) v = ring[rng.randbelow(ring.size())];
        return {v + "," + tok};
    }

    // ---- bounded exhaustion
    std::vector<Bytes> g_exhaust() {
        Fields f = valid_fields();
        int64_t k = rng.randrange(8);
        int64_t n = rng.randint(2000, 20000);
        if (k == 0) f[4] = Bytes((size_t)n, 'a') + "!";
        else if (k == 1) f[4] = Bytes((size_t)n, '(');
        else if (k == 2) f[1] = Bytes((size_t)n, '9');
        else if (k == 3) return {repeat("1,", n)};
        else if (k == 4) return {Bytes((size_t)n, '"')};
        else if (k == 5) f[4] = Bytes((size_t)(n / 2), 'A') + Bytes(1, '\0') + Bytes((size_t)(n / 2), 'B');
        else if (k == 6) f[4] = repeat("(a+)+", n / 5) + "$";
        else f[4] = Bytes((size_t)n, '\\');
        return {join(f)};
    }

    // ---- scenario helpers
    std::pair<std::vector<Bytes>, std::vector<std::pair<size_t, Bytes>>> materialize(const EvList& lines, int64_t start_ts) {
        std::vector<Bytes> out;
        std::vector<std::pair<size_t, Bytes>> musts;
        for (const EvLine& it : lines) {
            if (it.kind == K_RAW) { out.push_back(it.rest); continue; }
            Bytes t;
            if (it.kind == K_REL) { ts += it.gap; t = D(ts); }
            else if (it.kind == K_ABS) t = it.txt;
            else if (it.kind == K_BACK) t = D(start_ts - it.gap);
            else t = D(ts / it.gap);
            if (it.must) musts.push_back({out.size(), t});
            out.push_back(t + "," + it.rest);
        }
        return {out, musts};
    }

    Bytes corrupt(const Bytes& line) {
        Bytes v = line;
        int64_t reps = rng.randint(1, 3);
        for (int64_t i = 0; i < reps; i++) {
            if (v.empty()) break;
            int64_t k = rng.randrange(4);
            int64_t p = rng.randrange((int64_t)v.size());
            if (k == 0) { int64_t b = rng.randrange(8); v[(size_t)p] = (char)((unsigned char)v[(size_t)p] ^ (1 << b)); }
            else if (k == 1) v.erase((size_t)p, 1);
            else if (k == 2) { int64_t b = rng.randrange(256); v.insert(v.begin() + p, (char)b); }
            else v.resize((size_t)p);
        }
        return v;
    }

    std::vector<Bytes> next_records() {
        if (rng.random() >= garbage_ratio) return valid();
        double x = rng.random() * tot;
        int i = (int)(std::upper_bound(cum, cum + 13, x) - cum);   // bisect_right
        switch (i) {
        case 0: return g_numeric();
        case 1: return g_structural();
        case 2: return g_temporal();
        case 3: return g_enum();
        case 4: return g_mutate();
        case 5: return g_yoyo();
        case 6: return g_burst();
        case 7: return g_inject();
        case 8: return g_unicode();
        case 9: return g_boundary();
        case 10: return g_parser();
        case 11: return g_auth();
        default: return g_exhaust();
        }
    }
};

// ------------------------------------------------------------------ shard driver
struct Item { int64_t pos; int eid; int et; EvList lines; };
struct MustRow { int64_t rec; Bytes ts; };

static uint64_t shard_seed(uint64_t seed, int64_t idx) {
    return seed * 1000003ULL + (uint64_t)idx * 7919ULL + 0x5EEDULL;   // wraps mod 2^64 like the Python mask
}

static std::vector<Item> build_items(uint64_t seed, int64_t idx, int64_t n, const Counts& counts) {
    PyRandom rr(shard_seed(seed, idx) ^ 0xE7E47ULL);
    std::vector<Item> items;
    int eid = 0;
    for (const auto& pc : counts) {
        for (int64_t c = 0; c < pc.second; c++) {
            int64_t pos = rr.randrange(n);
            Ev ev = EV_FN[pc.first](rr);
            if (ev.spacing == 0) {
                items.push_back(Item{pos, eid, pc.first, std::move(ev.lines)});
            } else {
                for (size_t j = 0; j < ev.lines.size(); j++)
                    items.push_back(Item{pos + (int64_t)j * ev.spacing, eid, pc.first, EvList{ev.lines[j]}});
            }
            eid++;
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        return a.pos < b.pos || (a.pos == b.pos && a.eid < b.eid);
    });
    return items;
}

struct Out {
    FILE* fp = stdout;
    Bytes buf;
    bool broken = false;
    void flush() {
        if (!buf.empty() && !broken && fwrite(buf.data(), 1, buf.size(), fp) != buf.size()) broken = true;
        buf.clear();
    }
    void put(const Bytes& r) {
        buf += r;
        buf.push_back('\n');
        if (buf.size() >= (1u << 22)) flush();
    }
};

template <class Sink>
static void gen_shard(int64_t idx, int64_t n, uint64_t seed, double ratio, const Counts& counts,
                      int64_t canary_every, Sink&& emit, std::vector<MustRow>* must) {
    Fuzz fz(shard_seed(seed, idx), ratio);
    std::vector<Item> items;
    if (!counts.empty()) items = build_items(seed, idx, n, counts);
    size_t ip = 0;
    bool has_canary = canary_every > 0;
    int64_t next_canary = canary_every;
    int64_t produced = 0;
    while (produced < n) {
        while (has_canary && produced >= next_canary && produced < n) {
            fz.ts += 1;                                         // well-formed rescue canary
            Bytes t = D(fz.ts);
            emit(t + ",8.1,850.0,-15.0,DISTRESS,1");
            if (must) must->push_back(MustRow{produced, t});
            produced += 1;
            next_canary += canary_every;
        }
        while (ip < items.size() && items[ip].pos <= produced && produced < n) {
            const Item& it = items[ip];
            ip++;
            auto mz = fz.materialize(it.lines, fz.ts);
            std::vector<Bytes>& recs = mz.first;
            double p = (it.et == E_DISASTER) ? 0.10 : 0.03;
            std::vector<char> ismust(recs.size(), 0);
            for (auto& m : mz.second) ismust[m.first] = 1;
            for (size_t i = 0; i < recs.size(); i++) {          // must-forward lines are never corrupted
                if (ismust[i]) continue;
                if (!(fz.rng.random() >= p)) recs[i] = fz.corrupt(recs[i]);
            }
            size_t lim = (size_t)(n - produced);
            if (recs.size() > lim) recs.resize(lim);
            if (recs.empty()) break;
            if (must)
                for (auto& m : mz.second)
                    if (m.first < recs.size()) must->push_back(MustRow{produced + (int64_t)m.first, m.second});
            for (auto& r : recs) emit(r);
            produced += (int64_t)recs.size();
        }
        if (produced < n) {
            std::vector<Bytes> recs = fz.next_records();
            size_t lim = (size_t)(n - produced);
            if (recs.size() > lim) recs.resize(lim);
            for (auto& r : recs) emit(r);
            produced += (int64_t)recs.size();
        }
    }
}

// ------------------------------------------------------------------ self test
static uint64_t fnv1a(const Bytes& s, uint64_t h) {
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
static std::string rng_vector() {
    std::string o;
    char b[160];
    PyRandom r(20261115ULL);
    for (int i = 0; i < 3; i++) { snprintf(b, sizeof b, "%.17g;", r.random()); o += b; }
    int64_t a1 = r.randint(1, 10);
    int64_t a2 = r.randrange(50000000);
    uint64_t a3 = r.getrandbits(40);
    o += std::to_string(a1) + ";" + std::to_string(a2) + ";" + std::to_string(a3) + ";";
    o += hexs(r.randbytes(7), false) + ";";
    snprintf(b, sizeof b, "%.17g;", r.uniform(-85, -78)); o += b;
    std::vector<int> a(10);
    for (int i = 0; i < 10; i++) a[(size_t)i] = i;
    r.shuffle(a);
    for (int x : a) o += std::to_string(x);
    o += ";";
    o += big_to_dec(r.getrandbits_words(100)) + ";";
    PyRandom q(18446744073709551615ULL);
    snprintf(b, sizeof b, "%.17g;", q.random()); o += b;
    o += py_repr(8.1 + 1e-9) + ";" + py_repr(std::nextafter(850.0, INFINITY)) + ";" + py_repr(-0.45) + ";" +
         py_repr(0.0001) + ";" + py_repr(1e16) + ";" + py_repr(5e-324) + ";";
    return o;
}
static int selftest() {
    int bad = 0;
    std::string got = rng_vector();
    const std::string want = "0.30083737541245636;0.47569455729687127;0.76625418453036953;10;10836296;194783716987;4d7d8ab28089b3;-79.544114540527048;8265490137;197668942469496600061887663828;0.021825695401270107;8.100000001;850.0000000000001;-0.45;0.0001;1e+16;5e-324;";
    bool ok1 = (got == want);
    printf("[selftest] python-compatible RNG / number formatting : %s\n", ok1 ? "OK" : "MISMATCH");
    if (!ok1) { printf("  got : %s\n  want: %s\n", got.c_str(), want.c_str()); bad++; }
    Counts counts = {{E_DISTRESS, 100}, {E_STEALTH, 30}, {E_SOFTBRICK, 100}, {E_SATLOSS, 100}, {E_DISASTER, 10}};
    uint64_t h = 1469598103934665603ULL;
    uint64_t len = 0;
    gen_shard(0, 20000, 20261115ULL, 0.20, counts, 5000, [&](const Bytes& r) {
        h = fnv1a(r, h); h = fnv1a("\n", h); len += r.size() + 1;
    }, nullptr);
    bool ok2 = (h == 12946870207749043303ULL && len == 771276ULL);
    printf("[selftest] 20,000-record stream regression (fnv1a64) : %s\n", ok2 ? "OK" : "MISMATCH");
    if (!ok2) { printf("  got  fnv=%llu len=%llu\n", (unsigned long long)h, (unsigned long long)len); bad++; }
    printf("SELFTEST: %s\n", bad ? "FAIL" : "PASS");
    return bad ? 1 : 0;
}

// ------------------------------------------------------------------ main
int main(int argc, char** argv) {
    signal(SIGPIPE, SIG_IGN);
    int64_t total = 10000000000LL, shard_lines = 50000000LL, only = -1, canary = 1000000;
    uint64_t seed = 20261115ULL;
    double ratio = 0.20;
    int64_t cnt[5] = {100, 30, 100, 100, 10};
    const char* must_path = nullptr;
    bool verbose = false;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { fprintf(stderr, "missing value for %s\n", a.c_str()); exit(2); }
            return argv[++i];
        };
        if (a == "--selftest") return selftest();
        else if (a == "--version") { puts(kVersion); return 0; }
        else if (a == "--total") total = strtoll(val(), nullptr, 10);
        else if (a == "--seed") seed = strtoull(val(), nullptr, 10);
        else if (a == "--shard-lines") shard_lines = strtoll(val(), nullptr, 10);
        else if (a == "--shard") only = strtoll(val(), nullptr, 10);
        else if (a == "--garbage-ratio") ratio = strtod(val(), nullptr);
        else if (a == "--distress") cnt[0] = strtoll(val(), nullptr, 10);
        else if (a == "--stealth") cnt[1] = strtoll(val(), nullptr, 10);
        else if (a == "--softbrick") cnt[2] = strtoll(val(), nullptr, 10);
        else if (a == "--satloss") cnt[3] = strtoll(val(), nullptr, 10);
        else if (a == "--disaster") cnt[4] = strtoll(val(), nullptr, 10);
        else if (a == "--canary-every") canary = strtoll(val(), nullptr, 10);
        else if (a == "--must") must_path = val();
        else if (a == "-v") verbose = true;
        else { fprintf(stderr, "unknown option: %s  (see header of shuzi_gen.cpp)\n", a.c_str()); return 2; }
    }
    if (total < 1 || shard_lines < 1) { fprintf(stderr, "--total and --shard-lines must be >= 1\n"); return 2; }
    Counts counts;
    for (int e = 0; e < 5; e++) if (cnt[e] > 0) counts.push_back({e, cnt[e]});

    int64_t n_shards = (total + shard_lines - 1) / shard_lines;
    Out out;
    bool first = true;
    for (int64_t i = 0; i < n_shards; i++) {
        if (only >= 0 && i != only) continue;
        int64_t n = std::min<int64_t>(shard_lines, total - i * shard_lines);
        std::vector<MustRow> must;
        gen_shard(i, n, seed, ratio, counts, canary, [&](const Bytes& r) { out.put(r); }, must_path ? &must : nullptr);
        out.flush();
        if (out.broken) return 0;                       // consumer closed the pipe: stop quietly, like the Python tool
        if (must_path) {
            FILE* f = fopen(must_path, first ? "w" : "a");
            if (!f) { fprintf(stderr, "cannot open %s\n", must_path); return 2; }
            if (first) fputs("record,ts\n", f);
            for (auto& m : must) fprintf(f, "%lld,%s\n", (long long)(i * shard_lines + m.rec), m.ts.c_str());
            fclose(f);
        }
        first = false;
        if (verbose) fprintf(stderr, "[shard %06lld] done (%lld records)\n", (long long)i, (long long)n);
    }
    return 0;
}
