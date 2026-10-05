#pragma once
// Python's random.Random, for what the benchmarks used of it: the same numbers
// from the same seed, so a library made here is the one the Python benchmarks'
// generator made (file for file), and the files given use counts are the ones
// the Python benchmark picked.
//
// Mersenne Twister seeded as CPython seeds it from an integer (init_by_array
// with the integer's 32-bit words), and Python 3's random.py on top of it:
// random(), _randbelow() by rejection with getrandbits(), choice(), randint(),
// sample() (both of its ways of picking) and choices() with weights.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <vector>

namespace sub::bench {

class PyRandom {
public:
    explicit PyRandom(uint32_t seed) { seedByArray(seed); }

    // random.random(): 53 random bits, in [0, 1).
    double random() {
        const uint32_t a = next() >> 5, b = next() >> 6;
        return (a * 67108864.0 + b) * (1.0 / 9007199254740992.0);
    }

    // random.getrandbits(k) for 1 <= k <= 32.
    uint32_t getrandbits(int k) { return next() >> (32 - k); }

    // random._randbelow(n): uniform in [0, n), for n > 0.
    size_t randbelow(size_t n) {
        int k = 0;  // n.bit_length()
        for (size_t v = n; v; v >>= 1) ++k;
        if (k > 32) throw std::invalid_argument("PyRandom: n too large");
        size_t r = getrandbits(k);
        while (r >= n) r = getrandbits(k);
        return r;
    }

    // random.randint(a, b): a <= N <= b.
    int randint(int a, int b) { return a + static_cast<int>(randbelow(static_cast<size_t>(b - a + 1))); }

    // random.choice(seq).
    template <typename T>
    const T& choice(const std::vector<T>& seq) {
        return seq[randbelow(seq.size())];
    }

    // random.sample(population, k): k distinct items, in the order picked. Like
    // Python, from a shrinking pool when the population is small next to k,
    // otherwise by drawing indexes until they are new.
    template <typename T>
    std::vector<T> sample(const std::vector<T>& population, size_t k) {
        const size_t n = population.size();
        if (k > n) throw std::invalid_argument("Sample larger than population");
        std::vector<T> result;
        result.reserve(k);
        size_t setsize = 21;  // the size of a small set minus that of an empty list
        if (k > 5) {  // the table size of big sets: 4 ** ceil(log(k * 3, 4))
            const auto exponent = static_cast<int>(std::ceil(std::log(static_cast<double>(k) * 3.0) / std::log(4.0)));
            setsize += size_t{1} << (2 * exponent);
        }
        if (n <= setsize) {
            std::vector<T> pool = population;
            for (size_t i = 0; i < k; ++i) {
                const size_t j = randbelow(n - i);
                result.push_back(pool[j]);
                pool[j] = pool[n - i - 1];  // the one not picked moves into the gap
            }
        } else {
            std::set<size_t> selected;
            for (size_t i = 0; i < k; ++i) {
                size_t j = randbelow(n);
                while (selected.count(j)) j = randbelow(n);
                selected.insert(j);
                result.push_back(population[j]);
            }
        }
        return result;
    }

    // random.choices(population, weights)[0]: one item, with these (whole) weights.
    template <typename T>
    const T& weightedChoice(const std::vector<T>& population, const std::vector<int>& weights) {
        std::vector<double> cumulative;
        double total = 0.0;
        for (const int w : weights) cumulative.push_back(total += w);
        const double x = random() * total;
        // bisect_right(cumulative, x, 0, n - 1)
        const auto at = std::upper_bound(cumulative.begin(), cumulative.end() - 1, x);
        return population[static_cast<size_t>(at - cumulative.begin())];
    }

private:
    static constexpr int kN = 624;
    static constexpr int kM = 397;

    void initGenrand(uint32_t s) {
        mt_[0] = s;
        for (int i = 1; i < kN; ++i) mt_[i] = 1812433253u * (mt_[i - 1] ^ (mt_[i - 1] >> 30)) + static_cast<uint32_t>(i);
        index_ = kN;
    }

    // init_by_array with the seed's one 32-bit word (CPython's random_seed for an int below 2**32).
    void seedByArray(uint32_t seed) {
        initGenrand(19650218u);
        const uint32_t key[1] = {seed};
        const int keyLength = 1;
        int i = 1, j = 0;
        for (int k = std::max(kN, keyLength); k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1664525u)) + key[j] + static_cast<uint32_t>(j);
            ++i;
            ++j;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
            if (j >= keyLength) j = 0;
        }
        for (int k = kN - 1; k; --k) {
            mt_[i] = (mt_[i] ^ ((mt_[i - 1] ^ (mt_[i - 1] >> 30)) * 1566083941u)) - static_cast<uint32_t>(i);
            ++i;
            if (i >= kN) {
                mt_[0] = mt_[kN - 1];
                i = 1;
            }
        }
        mt_[0] = 0x80000000u;  // non-zero
        index_ = kN;
    }

    // genrand_uint32
    uint32_t next() {
        if (index_ >= kN) {
            for (int k = 0; k < kN; ++k) {
                const uint32_t y = (mt_[k] & 0x80000000u) | (mt_[(k + 1) % kN] & 0x7fffffffu);
                mt_[k] = mt_[(k + kM) % kN] ^ (y >> 1) ^ ((y & 1u) ? 0x9908b0dfu : 0u);
            }
            index_ = 0;
        }
        uint32_t y = mt_[index_++];
        y ^= y >> 11;
        y ^= (y << 7) & 0x9d2c5680u;
        y ^= (y << 15) & 0xefc60000u;
        y ^= y >> 18;
        return y;
    }

    std::array<uint32_t, kN> mt_{};
    int index_ = kN;
};

}  // namespace sub::bench
