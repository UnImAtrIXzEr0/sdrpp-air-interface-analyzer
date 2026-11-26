// Unit test of the uplink burst handling for the modulation accuracy
// measurement (decoder_modules/ch_tetra_demodulator/src/etsi_ul_vem.h):
// synthetic normal uplink bursts (training sequence 1 and 2) and control
// uplink bursts, coded with the decoder's channel coding, pi/4-DQPSK, TETRA
// filter on both sides, frequency error, residual carrier, amplitude droop,
// noise, inverted spectrum. Checks burst type, timing and that the ideal
// bits are found exactly.
//
// Build and run (from the repository root):
//   D=decoder_modules/ch_tetra_demodulator/src
//   gcc -c -O2 -I$D/decoder/src $D/decoder/src/lower_mac/{tetra_scramb,tetra_interleave,tetra_conv_enc,crc_simple,tetra_rm3014,viterbi}.c $D/decoder/src/phy/tetra_burst.c
//   g++ -O2 -std=c++17 -I$D -I$D/decoder/src e2e/gen/test_ul_vem.cpp *.o -o test_ul_vem
//   ./test_ul_vem

#include <cstdio>
#include <cmath>
#include <cstring>
#include <random>
#include <vector>
#include "etsi_ul_vem.h"

using cf = std::complex<float>;
using namespace etsi_ul_vem;

static int failures = 0;
#define CHECK(cond, what)                                                   \
    do {                                                                    \
        if (!(cond)) { printf("FAIL: %s (line %d)\n", what, __LINE__); failures++; } \
        else { printf("ok:   %s\n", what); }                               \
    } while (0)

static const uint16_t MCC = 901, MNC = 9999;
static const uint8_t CC = 1;

// type-1 bits -> CRC, tail, RCPC 2/3, interleaver, scrambling (as tetra_synth.cpp)
static void codeBlock(const uint8_t* t1, int n1, int K, int a, uint32_t scr, uint8_t* out) {
    uint8_t t2[288 + 4] = {}, mother[(288 + 4) * 4], t3[432];
    memcpy(t2, t1, n1);
    uint16_t crc = (uint16_t)~crc16_ccitt_bits(t2, n1);
    for (int i = 0; i < 16; i++) { t2[n1 + i] = (crc >> (15 - i)) & 1; }
    int n2 = n1 + 16 + 4;
    struct conv_enc_state { uint8_t d[4]; } ces;
    conv_enc_init((struct ::conv_enc_state*)&ces);
    conv_enc_input((struct ::conv_enc_state*)&ces, t2, n2, mother);
    get_punctured_rate(etsi_vem::PUNCT_2_3, mother, K, t3);
    block_interleave(K, a, t3, out);
    tetra_scramb_bits(scr, out, K);
}

struct Case {
    const char* name;
    UlType type;
    bool speech;        // TCH: random bits without CRC
    double freqHz;      // frequency error of the transmitter
    double c0;          // residual carrier C0 (relative to |S|)
    double droopDb;     // amplitude change over the burst
    double snrDb;       // in the channel
    bool mirror;        // inverted spectrum
    double offset;      // fractional start (samples)
    double evm;         // vector error E(k) added at the symbols (RMS, relative)
};

// one burst: returns the bits sent and the TETRA-filtered received signal;
// t1 = sample position of SN1
// eRms / ePeak: RMS and peak of the vector error E(k) actually added (SN1..SNn)
static void makeBurst(const Case& c, uint32_t scr, std::mt19937& rng, std::vector<uint8_t>& bits, std::vector<cf>& y, double& t1, double& eRms, double& ePeak) {
    std::uniform_int_distribution<int> bit(0, 1);
    const int n = symbols(c.type);
    bits.assign(2 * (size_t)n, 0);
    if (c.type == UL_CB) {
        uint8_t t1b[92], cb[168];
        for (auto& b : t1b) { b = (uint8_t)bit(rng); }
        codeBlock(t1b, 92, 168, 13, scr, cb);
        buildCb(bits.data(), cb);
    }
    else if (c.type == UL_NUB1) {
        uint8_t blk[432];
        if (c.speech) { for (auto& b : blk) { b = (uint8_t)bit(rng); } }
        else { uint8_t t1b[268]; for (auto& b : t1b) { b = (uint8_t)bit(rng); } codeBlock(t1b, 268, 432, 103, scr, blk); }
        buildNub(bits.data(), blk, blk + 216, false);
    }
    else {
        uint8_t b1[216], b2[216], t1b[124];
        for (auto& b : t1b) { b = (uint8_t)bit(rng); }
        codeBlock(t1b, 124, 216, 101, scr, b1);
        if (c.speech) { for (auto& b : b2) { b = (uint8_t)bit(rng); } }   // STCH + TCH
        else { for (auto& b : t1b) { b = (uint8_t)bit(rng); } codeBlock(t1b, 124, 216, 101, scr, b2); }
        buildNub(bits.data(), b1, b2, true);
    }
    // impulses at 144 kS/s: SN0 (reference) .. SNn, guard of 40 symbols each side.
    // Model of TS 100 392-2 6.6.1.2 at the symbol times: C0 + S(k) + E(k); the
    // transmit and receive filters together are a Nyquist filter.
    const int G = 40, SPS = etsi_vem::SPS;
    const int len = (n + 1 + 2 * G) * SPS + 32;
    std::vector<cf> imp((size_t)len, cf(0.0f, 0.0f));
    std::normal_distribution<double> nd(0.0, 1.0);
    const double es = c.evm / std::sqrt(2.0);
    double se = 0.0;
    ePeak = 0.0;
    int ph = 0;
    for (int k = 0; k <= n; k++) {
        if (k > 0) { ph = (ph + etsi_vem::dibitPhase(bits[2 * k - 2], bits[2 * k - 1]) + 8) % 8; }
        std::complex<double> e(es * nd(rng), es * nd(rng));
        if (k > 0) { se += std::norm(e); ePeak = (std::max)(ePeak, std::abs(e)); }
        std::complex<double> v = std::polar(1.0, ph * PI_D / 4.0) + e + c.c0;
        imp[(size_t)(G + k) * SPS] = cf((float)(SPS * v.real()), (float)(SPS * v.imag()));
    }
    eRms = std::sqrt(se / n);
    const std::vector<float> h = etsi_vem::tetraFilter(33, SPS);
    std::vector<cf> x = etsi_vem::filterCentred(imp, h);
    // transmitter: amplitude droop over the burst (part of the vector error), frequency error, phase
    const double fs = etsi_vem::FS;
    const double a0 = (double)G * SPS, a1 = (double)(G + n) * SPS;
    for (size_t i = 0; i < x.size(); i++) {
        double tt = std::clamp(((double)i - a0) / (a1 - a0), 0.0, 1.0);
        double g = std::pow(10.0, -c.droopDb * tt / 20.0);
        std::complex<double> v = std::complex<double>(x[i]) * g;
        v *= std::polar(1.0, 2 * PI_D * c.freqHz * (double)i / fs + 0.9);
        x[i] = cf((float)v.real(), (float)v.imag());
    }
    // noise: SNR in the 18 kHz symbol bandwidth (fs / 8)
    double p = 0.0;
    for (int i = (int)a0; i < (int)a1; i++) { p += std::norm(x[i]); }
    p /= (a1 - a0);
    const float sigma = (float)std::sqrt(p * std::pow(10.0, -c.snrDb / 10.0) * SPS / 2.0);
    if (c.snrDb < 200.0) { for (auto& v : x) { v += cf((float)(sigma * nd(rng)), (float)(sigma * nd(rng))); } }
    if (c.mirror) { for (auto& v : x) { v = std::conj(v); } }
    // fractional delay: interpolate the transmitted signal at i - offset
    std::vector<cf> xd(x.size(), cf(0.0f, 0.0f));
    for (size_t i = 16; i + 16 < x.size(); i++) { xd[i] = cf(etsi_vem::interp(x, (double)i - c.offset)); }
    y = etsi_vem::filterCentred(xd, h);
    t1 = (double)(G + 1) * SPS + c.offset;
}

int main() {
    tetra_rm3014_init();
    const uint32_t scr = tetra_scramb_get_init(MCC, MNC, CC);
    std::mt19937 rng(20261126);
    const Case cases[] = {
        { "NUB, training sequence 1, SCH/F", UL_NUB1, false, 0.0, 0.0, 0.0, 40.0, false, 0.0, 0.0 },
        { "NUB, training sequence 1, SCH/F, +60 Hz, C0 3 %, droop 1 dB", UL_NUB1, false, 60.0, 0.03, 1.0, 30.0, false, 0.37, 0.0 },
        { "NUB, training sequence 1, speech (decisions)", UL_NUB1, true, -45.0, 0.02, 0.5, 30.0, false, 0.81, 0.0 },
        { "NUB, training sequence 2, SCH/HD + SCH/HD", UL_NUB2, false, 25.0, 0.01, 0.0, 30.0, false, 0.5, 0.0 },
        { "NUB, training sequence 2, STCH + speech", UL_NUB2, true, -80.0, 0.0, 0.0, 30.0, true, 0.2, 0.0 },
        { "CB, SCH/HU", UL_CB, false, 90.0, 0.04, 0.0, 30.0, false, 0.66, 0.0 },
        { "CB, SCH/HU, inverted spectrum, 22 dB", UL_CB, false, -30.0, 0.0, 0.5, 22.0, true, 0.1, 0.0 },
        { "CB, SCH/HU, inverted spectrum, SDR off by +400 Hz", UL_CB, false, 400.0, 0.02, 0.0, 30.0, true, 0.45, 0.0 },
        { "NUB, training sequence 1, speech, 15 dB, SDR off by -400 Hz", UL_NUB1, true, -400.0, 0.0, 0.0, 15.0, false, 0.3, 0.0 },
    };
    for (const Case& c : cases) {
        printf("-- %s\n", c.name);
        for (int rep = 0; rep < 20; rep++) {
            std::vector<uint8_t> sent;
            std::vector<cf> y;
            double t1;
            double eRms, ePeak;
            makeBurst(c, scr, rng, sent, y, t1, eRms, ePeak);
            const int n = symbols(c.type);
            // centre from the power detection: middle of SN1..SNn, off by up to +-3 symbols
            double centre = t1 + (n - 1) * SPS / 2.0 + (double)((rep % 7) - 3) * SPS;
            // length from the power detection known for half of the bursts
            int length = (rep & 1) ? (c.type == UL_CB ? 1 : 2) : 0;
            Found f = findBurst(y, centre, length);
            // coarse timing (the vector error fit refines it): within 1/20 symbol
            if (!f.ok || f.type != c.type || f.mirror != c.mirror || std::fabs(f.t1 - t1) > 0.4) {
                printf("FAIL: rep %d found %d type %d mirror %d t1 %.2f (sent %.2f)\n", rep, f.ok, f.type, f.mirror, f.t1, t1);
                failures++;
                continue;
            }
            std::vector<uint8_t> rx(2 * (size_t)n);
            decideBits(y, f.t1, n, f.mirror, rx.data());
            Ideal id = idealBits(f.type, rx.data(), scr);
            int errIdeal = etsi_vem::hamming(id.bits, sent.data(), 2 * n);
            Source want = c.speech ? (c.type == UL_NUB2 ? SRC_PARTIAL : SRC_DECISION) : SRC_CRC;
            if (!id.ok || id.src != want || errIdeal != 0) {
                printf("FAIL: rep %d src %d (want %d), %d wrong ideal bits, %d decision errors\n", rep, id.src, want, errIdeal, id.bitErrors);
                failures++;
            }
        }
        printf("ok:   done\n");
    }
    // vector error, residual carrier and frequency without noise (TS 100 392-2
    // 6.6.1.2, TS 100 394-1 10.1.3 / 10.2.1): the measurement has to return what
    // was put in. Limits: residual error of the test system (TS 100 394-1
    // B.1.2.4: RMS < 0.01, peak < 0.03) and a fraction of the uncertainties of
    // clause 11.3 (RMS +-1 %, peak +-3 %, C0 +-1 %).
    {
        struct MCase { const char* name; Case c; };
        const MCase mc[] = {
            { "ideal NUB: residual error of the measurement", { "", UL_NUB1, false, 0.0, 0.0, 0.0, 999.0, false, 0.0, 0.0 } },
            { "ideal CB, inverted spectrum, off by 0.4 samples", { "", UL_CB, false, 0.0, 0.0, 0.0, 999.0, true, 0.4, 0.0 } },
            { "NUB, E 5 %, C0 3 %, +60 Hz", { "", UL_NUB1, false, 60.0, 0.03, 0.0, 999.0, false, 0.37, 0.05 } },
            { "NUB speech, E 8 %, C0 1 %, -95 Hz, inverted", { "", UL_NUB1, true, -95.0, 0.01, 0.0, 999.0, true, 0.81, 0.08 } },
            { "NUB TS2, E 2 %, C0 4.5 %, +350 Hz (SDR offset)", { "", UL_NUB2, false, 350.0, 0.045, 0.0, 999.0, false, 0.5, 0.02 } },
            { "CB, E 10 %, C0 2 %, -40 Hz", { "", UL_CB, false, -40.0, 0.02, 0.0, 999.0, false, 0.66, 0.10 } },
        };
        for (const MCase& m : mc) {
            double worstRms = 0.0, worstPk = 0.0, worstC0 = 0.0, worstF = 0.0, maxRmsIdeal = 0.0, maxPkIdeal = 0.0;
            bool allOk = true;
            for (int rep = 0; rep < 20; rep++) {
                std::vector<uint8_t> sent;
                std::vector<cf> y;
                double t1, eRms, ePeak;
                makeBurst(m.c, scr, rng, sent, y, t1, eRms, ePeak);
                const int n = symbols(m.c.type);
                Meas r = measureBurst(y, t1 + (n - 1) * SPS / 2.0, 0, scr);
                if (!r.ok || r.type != m.c.type) { allOk = false; continue; }
                worstRms = (std::max)(worstRms, std::fabs(r.fit.rmsVe - eRms));
                worstPk = (std::max)(worstPk, std::fabs(r.fit.peakVe - ePeak));
                worstC0 = (std::max)(worstC0, std::fabs(r.fit.residualCarrier - m.c.c0));
                worstF = (std::max)(worstF, std::fabs(r.freqHz - m.c.freqHz));
                maxRmsIdeal = (std::max)(maxRmsIdeal, r.fit.rmsVe);
                maxPkIdeal = (std::max)(maxPkIdeal, r.fit.peakVe);
            }
            printf("-- %s: |dRMS| %.4f, |dPeak| %.4f, |dC0| %.4f, |df| %.3f Hz (RMS max %.4f, peak max %.4f)\n", m.name, worstRms, worstPk, worstC0, worstF, maxRmsIdeal, maxPkIdeal);
            CHECK(allOk, "every burst measured");
            if (m.c.evm == 0.0) {
                CHECK(maxRmsIdeal < 0.002 && maxPkIdeal < 0.006, "residual error well below B.1.2.4 (RMS < 0.01, peak < 0.03)");
            }
            // The estimates of C0, C1 and Theta take part of the random E(k) with
            // them: per burst they scatter by about E / sqrt(SNmax) (frequency:
            // the slope of a phase noise of E / sqrt(2)); the limits allow 4 sigma.
            const int n = symbols(m.c.type);
            const double sC0 = m.c.evm / std::sqrt((double)n);
            const double sF = m.c.evm / std::sqrt(2.0) * std::sqrt(12.0 / (n * ((double)n * n - 1.0))) * etsi_vem::SYMRATE / (2.0 * PI_D);
            // RMS: the fit removes the part of E(k) that looks like C0, C1, Theta
            // or a timing shift; over 1000 bursts this stays below 6 % of E
            // (clause 11.3 allows +-1 % absolute)
            CHECK(worstRms < 0.002 + 0.06 * m.c.evm, "RMS vector error as put in");
            CHECK(worstPk < 0.006 + 4.0 * sC0, "peak vector error as put in");
            CHECK(worstC0 < 0.002 + 4.0 * sC0, "residual carrier as put in");
            CHECK(worstF < 0.5 + 4.0 * sF, "frequency error as put in");
        }
    }
    // amplitude droop is part of the vector error (not compensated): 1 dB over
    // the burst gives E(k) = g(k) / mean(g) - 1
    {
        Case c = { "", UL_NUB1, false, 0.0, 0.0, 1.0, 999.0, false, 0.0, 0.0 };
        const int n = NUB_SYM;
        double mg = 0.0, se = 0.0;
        std::vector<double> g(n);
        for (int k = 1; k <= n; k++) { g[k - 1] = std::pow(10.0, -1.0 * k / n / 20.0); mg += g[k - 1]; }
        mg /= n;
        for (double v : g) { se += (v / mg - 1.0) * (v / mg - 1.0); }
        const double want = std::sqrt(se / n);
        std::vector<uint8_t> sent;
        std::vector<cf> y;
        double t1, eRms, ePeak;
        makeBurst(c, scr, rng, sent, y, t1, eRms, ePeak);
        Meas r = measureBurst(y, t1 + (n - 1) * SPS / 2.0, 2, scr);
        printf("-- droop 1 dB: RMS %.4f, expected %.4f\n", r.fit.rmsVe, want);
        CHECK(r.ok && std::fabs(r.fit.rmsVe - want) < 0.002, "droop of 1 dB shows up in the RMS vector error");
    }
    // nothing in the middle: no burst must be found in pure noise
    {
        std::normal_distribution<float> nd(0.0f, 1.0f);
        std::vector<cf> y(4000);
        for (auto& v : y) { v = cf(nd(rng), nd(rng)); }
        y = etsi_vem::filterCentred(y, etsi_vem::tetraFilter(33, etsi_vem::SPS));
        int hits = 0;
        for (int i = 0; i < 20; i++) { hits += findBurst(y, 1500.0 + i * 50).ok; }
        CHECK(hits == 0, "no burst found in noise");
    }
    printf(failures ? "%d FAILURES\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
