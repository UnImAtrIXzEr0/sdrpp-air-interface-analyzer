#pragma once
// Uplink bursts of a TETRA mobile station for the modulation accuracy and
// carrier frequency measurement (ETSI TS 100 394-1 V4.1.1, 10.1.1, 10.1.3 and
// 10.2.1; burst structure ETSI TS 100 392-2 V3.10.1, 9.4.4.2):
//   - finds the training sequence in a burst located by the power detection:
//     normal uplink burst (NUB, 231 symbols, normal training sequence 1 or 2)
//     or control uplink burst (CB, 103 symbols, extended training sequence).
//     This gives the burst type, SNmax and the symbol timing.
//   - ideal symbols S(k): blocks with a CRC (SCH/F, SCH/HD or STCH, SCH/HU) are
//     decoded and coded again bit by bit, as the mobile station sent them.
//     Blocks without a complete check (speech, TCH) take the symbol decisions,
//     made coherently after removing frequency, phase and amplitude; with
//     neighbouring points 90 degrees apart, a decision is only wrong for a
//     vector error above about 0.7.
//   - fit of TS 100 392-2 6.6.1.2 (symbol time, Theta, C0, C1; amplitude
//     changes stay in the vector error): RMS and peak vector error, residual
//     carrier and the frequency error df of TS 100 394-1 10.2.1.
// pi/4-DQPSK only. The input is the TETRA-filtered signal at 8 samples per
// symbol (as in etsi_vem.h). No SDR++ dependencies: unit-testable
// (e2e/gen/test_ul_vem.cpp).

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>
#include "etsi_vem.h"

namespace etsi_ul_vem {
    using etsi_vem::PI_D;
    using etsi_vem::SPS;
    static constexpr int NUB_SYM = 231, NUB_BITS = 462;   // TS 100 392-2 table 9.5
    static constexpr int CB_SYM = 103, CB_BITS = 206;     // TS 100 392-2 table 9.3
    // first symbol of the training sequence (1-based symbol number, SN)
    static constexpr int NUB_TRAIN_SN = 111;   // bits 221..242
    static constexpr int CB_TRAIN_SN = 45;     // bits 89..118
    // extended training sequence (TS 100 392-2 9.4.4.3.3, equation 9.9)
    static const uint8_t X_BITS[30] = { 1,0, 0,1, 1,1, 0,1, 0,0, 0,0, 1,1, 1,0, 1,0, 0,1, 1,1, 0,1, 0,0, 0,0, 1,1 };
    // tail bits (9.4.4.3.5, equation 9.12)
    static const uint8_t T_BITS[4] = { 1, 1, 0, 0 };
    // accepted bit errors in the training sequence
    static constexpr int MAX_ERR_N = 2, MAX_ERR_X = 3, MAX_ERR_KNOWN = 3;

    enum UlType { UL_NONE = 0, UL_NUB1, UL_NUB2, UL_CB };
    // where the ideal symbols come from
    enum Source { SRC_NONE = 0, SRC_CRC, SRC_PARTIAL, SRC_DECISION };

    inline int symbols(UlType t) { return t == UL_CB ? CB_SYM : (t == UL_NONE ? 0 : NUB_SYM); }

    // ---------------- burst building (TS 100 392-2 9.4.4.2.1, 9.4.4.2.4) ----------------
    inline void buildNub(uint8_t* b, const uint8_t* bkn1, const uint8_t* bkn2, bool nts2) {
        memcpy(b, T_BITS, 4);
        memcpy(b + 4, bkn1, 216);
        memcpy(b + 220, nts2 ? etsi_vem::P_BITS : etsi_vem::N_BITS, 22);
        memcpy(b + 242, bkn2, 216);
        memcpy(b + 458, T_BITS, 4);
    }
    inline void buildCb(uint8_t* b, const uint8_t* cb) {
        memcpy(b, T_BITS, 4);
        memcpy(b + 4, cb, 84);
        memcpy(b + 88, X_BITS, 30);
        memcpy(b + 118, cb + 84, 84);
        memcpy(b + 202, T_BITS, 4);
    }

    // ideal symbols S(1..n) of a burst from its bits, S(SN0) = 1 (phase reference);
    // mirror: other rotation sense (inverted spectrum)
    inline std::vector<std::complex<double>> idealSymbols(const uint8_t* bits, int n, bool mirror) {
        std::vector<std::complex<double>> s(n);
        int ph = 0;   // units of pi/4
        for (int k = 0; k < n; k++) {
            int d = etsi_vem::dibitPhase(bits[2 * k], bits[2 * k + 1]);
            ph = (ph + (mirror ? -d : d) + 8000) % 8;
            s[k] = std::polar(1.0, ph * PI_D / 4.0);
        }
        return s;
    }

    // phase changes into the symbols at t, t + sps, ... (n values; the first is
    // relative to the sample at t - sps), frequency offset removed
    inline void phaseSteps(const std::vector<std::complex<float>>& y, double t, int n, std::vector<double>& d, double sps = SPS) {
        d.assign(n, 0.0);
        std::complex<double> prev = etsi_vem::interp(y, t - sps);
        for (int k = 0; k < n; k++) {
            std::complex<double> z = etsi_vem::interp(y, t + k * sps);
            d[k] = std::arg(z * std::conj(prev));
            prev = z;
        }
        for (int pass = 0; pass < 2; pass++) {
            double s = 0.0;
            for (int k = 0; k < n; k++) {
                double q = std::round((d[k] / (PI_D / 4.0) - 1.0) / 2.0) * 2.0 + 1.0;
                s += d[k] - q * PI_D / 4.0;
            }
            s /= n;
            for (int k = 0; k < n; k++) {
                d[k] -= s;
                while (d[k] > PI_D) { d[k] -= 2 * PI_D; }
                while (d[k] < -PI_D) { d[k] += 2 * PI_D; }
            }
        }
    }
    // decision on a phase change: +1, +3, -3, -1 (units of pi/4)
    inline int decideStep(double v) {
        int q = (int)(std::round((v / (PI_D / 4.0) - 1.0) / 2.0) * 2.0 + 1.0);
        if (q > 3) { q = -3; }
        if (q < -3) { q = 3; }
        return q;
    }
    // sum of squared decision errors of the phase changes (timing criterion)
    inline double stepCost(const std::vector<std::complex<float>>& y, double t, int n) {
        std::vector<double> d;
        phaseSteps(y, t, n, d);
        double s = 0.0;
        for (int k = 0; k < n; k++) { double e = d[k] - decideStep(d[k]) * PI_D / 4.0; s += e * e; }
        return s;
    }

    // ---------------- finding the burst ----------------
    struct Found {
        bool ok = false;
        UlType type = UL_NONE;
        double t1 = 0.0;      // sample position of the symbol time of SN1
        bool mirror = false;
        int trainErr = 99;    // bit errors in the training sequence
    };
    // errors in the fields the standard fixes (tail bits, training sequence)
    // of a burst of type t decided at t1
    inline int knownFieldErrors(const std::vector<std::complex<float>>& y, double t1, UlType t, bool mirror) {
        const int n = symbols(t);
        std::vector<double> d;
        phaseSteps(y, t1, n, d);
        std::vector<uint8_t> b(2 * (size_t)n);
        for (int k = 0; k < n; k++) {
            int q = decideStep(d[k]);
            etsi_vem::phaseDibit(mirror ? -q : q, &b[2 * (size_t)k]);
        }
        int e = etsi_vem::hamming(b.data(), T_BITS, 4) + etsi_vem::hamming(b.data() + 2 * n - 4, T_BITS, 4);
        if (t == UL_CB) { e += etsi_vem::hamming(b.data() + 88, X_BITS, 30); }
        else { e += etsi_vem::hamming(b.data() + 220, t == UL_NUB2 ? etsi_vem::P_BITS : etsi_vem::N_BITS, 22); }
        return e;
    }
    // y: TETRA-filtered samples; centre: middle of the burst from the power
    // detection (sample index). The training sequences of NUB and CB both sit
    // in the middle of the burst, so they are searched within +-span symbols.
    // Training sequences alone are ambiguous: the extended sequence contains
    // normal sequence 1, and sequence 2 is sequence 1 with the other rotation
    // sense. So every match is checked with the tail bits at both ends of the
    // burst it implies; 'length' (0 unknown, 1 half slot, 2 full slot, from the
    // power detection) leaves out the other burst type.
    inline Found findBurst(const std::vector<std::complex<float>>& y, double centre, int length = 0, int span = 8) {
        Found f;
        const int W = 2 * span + 16;        // symbols around the centre
        const double start = std::floor(centre - (double)(W / 2) * SPS);
        if (start - 2 * SPS < 16 || centre + (W / 2 + 2) * SPS + 16 >= (double)y.size()) { return f; }
        struct Cand { UlType t; double t1; bool mir; int trainErr; };
        std::vector<Cand> cands;
        std::vector<double> d;
        std::vector<uint8_t> bits(2 * (size_t)W);
        for (int ph = 0; ph < SPS; ph++) {
            phaseSteps(y, start + ph, W, d);
            for (int mir = 0; mir < 2; mir++) {
                for (int k = 0; k < W; k++) {
                    int q = decideStep(d[k]);
                    etsi_vem::phaseDibit(mir ? -q : q, &bits[2 * (size_t)k]);
                }
                // dibit k is the phase change into the symbol at start + ph + k * SPS
                for (int k = 0; k < W; k++) {
                    const uint8_t* b = &bits[2 * (size_t)k];
                    const double tk = start + ph + (double)k * SPS;
                    if (length != 1 && k + 11 <= W) {
                        int dn = etsi_vem::hamming(b, etsi_vem::N_BITS, 22), dp = etsi_vem::hamming(b, etsi_vem::P_BITS, 22);
                        if (dn <= MAX_ERR_N) { cands.push_back({ UL_NUB1, tk - (NUB_TRAIN_SN - 1) * SPS, mir == 1, dn }); }
                        if (dp <= MAX_ERR_N) { cands.push_back({ UL_NUB2, tk - (NUB_TRAIN_SN - 1) * SPS, mir == 1, dp }); }
                    }
                    if (length != 2 && k + 15 <= W) {
                        int e = etsi_vem::hamming(b, X_BITS, 30);
                        if (e <= MAX_ERR_X) { cands.push_back({ UL_CB, tk - (CB_TRAIN_SN - 1) * SPS, mir == 1, e }); }
                    }
                }
            }
        }
        // best candidate: fewest errors in tail bits and training sequence together
        int bestScore = 1 << 30;
        Cand best = { UL_NONE, 0.0, false, 99 };
        for (const Cand& c : cands) {
            const int n = symbols(c.t);
            if (c.t1 - 3 * SPS < 16 || c.t1 + (n + 2) * SPS + 16 >= (double)y.size()) { continue; }
            int sc = knownFieldErrors(y, c.t1, c.t, c.mir);
            if (sc < bestScore) { bestScore = sc; best = c; }
        }
        // a real burst decodes its tail bits too: at most MAX_ERR_KNOWN errors in
        // all fixed bits together (30 for NUB, 38 for CB)
        if (best.t == UL_NONE || bestScore > MAX_ERR_KNOWN) { return f; }
        const int n = symbols(best.t);
        double t1 = best.t1;
        // timing: whole samples within +-half a symbol (several sample phases can
        // decode the training sequence), then fractional; criterion: smallest
        // decision error of the phase changes over the burst
        {
            double bc = INFINITY, bt = t1;
            for (int j = -SPS / 2; j <= SPS / 2; j++) {
                double c = stepCost(y, t1 + j, n);
                if (c < bc) { bc = c; bt = t1 + j; }
            }
            t1 = bt;
        }
        t1 = etsi_vem::golden([&](double t) { return stepCost(y, t, n); }, t1 - 0.6, t1 + 0.6, 16);
        f.ok = true;
        f.type = best.t;
        f.t1 = t1;
        f.mirror = best.mir;
        f.trainErr = best.trainErr;
        return f;
    }

    // ---------------- symbol decisions ----------------
    // Coherent decisions on SN0..SNn, every symbol on its own (a wrong decision
    // does not carry over as with differential decisions):
    //   1. frequency: mean deviation of the phase changes from their decisions
    //   2. after removing it and the pi/4 rotation of every second symbol, all
    //      symbols lie on one QPSK set; its phase from the 4th power
    //   3. nearest point; then once more with the residual carrier C0 and the
    //      complex amplitude C1 by least squares on these decisions
    // The phase ambiguity of pi/2 drops out of the phase changes, which carry
    // the bits. Returns the 2n bits.
    inline void decideBits(const std::vector<std::complex<float>>& y, double t1, int n, bool mirror, uint8_t* bits) {
        std::vector<std::complex<double>> Z(n + 1);
        for (int k = 0; k <= n; k++) {
            Z[k] = etsi_vem::interp(y, t1 + (k - 1) * (double)SPS);
            if (mirror) { Z[k] = std::conj(Z[k]); }
        }
        // 1. frequency (radians per symbol)
        double w = 0.0;
        for (int pass = 0; pass < 2; pass++) {
            double sum = 0.0;
            for (int k = 1; k <= n; k++) {
                double v = std::arg(Z[k] * std::conj(Z[k - 1])) - w;
                while (v > PI_D) { v -= 2 * PI_D; }
                while (v < -PI_D) { v += 2 * PI_D; }
                sum += v - decideStep(v) * PI_D / 4.0;
            }
            w += sum / n;
        }
        // 2. onto one QPSK set; there the 4th power removes the modulation.
        // Every wrong decision of a phase change in 1 shifts the mean by pi/2 / n,
        // so the frequency is refined on the 4th power (largest coherent sum).
        std::vector<std::complex<double>> U(n + 1), U4(n + 1);
        for (int k = 0; k <= n; k++) {
            U[k] = Z[k] * std::polar(1.0, -w * k - ((k & 1) ? PI_D / 4.0 : 0.0));
            std::complex<double> u2 = U[k] * U[k];
            U4[k] = u2 * u2;
        }
        auto power4 = [&](double dw) {
            std::complex<double> a(0.0, 0.0), r(1.0, 0.0), st = std::polar(1.0, -4.0 * dw);
            for (int k = 0; k <= n; k++) { a += U4[k] * r; r *= st; }
            return std::abs(a);
        };
        {
            // grid over +-3 wrong decisions (step well below the main lobe width 2 pi / (4 n)), then golden section
            const double lim = 3.0 * (PI_D / 2.0) / n + 1e-3, stp = PI_D / (8.0 * n);
            double bd = 0.0, bp = -1.0;
            for (double dw = -lim; dw <= lim; dw += stp) {
                double v = power4(dw);
                if (v > bp) { bp = v; bd = dw; }
            }
            bd = etsi_vem::golden([&](double dw) { return -power4(dw); }, bd - stp, bd + stp, 16);
            w += bd;
        }
        std::complex<double> acc(0.0, 0.0);
        for (int k = 0; k <= n; k++) {
            U[k] = Z[k] * std::polar(1.0, -w * k - ((k & 1) ? PI_D / 4.0 : 0.0));
            std::complex<double> u2 = U[k] * U[k];
            acc += u2 * u2;
        }
        const double phi = std::arg(acc) / 4.0;   // QPSK at 0, 90, 180, 270 degrees (+ phi): 4th power at 4 phi
        // 3. decisions: phase index p (units of pi/4) of every symbol
        std::vector<int> p(n + 1);
        auto decide = [&](const std::vector<std::complex<double>>& V) {
            for (int k = 0; k <= n; k++) {
                int q = (int)std::lround(std::arg(V[k]) / (PI_D / 2.0));
                p[k] = ((2 * q + (k & 1)) % 8 + 8) % 8;
            }
        };
        {
            std::vector<std::complex<double>> V(n + 1);
            for (int k = 0; k <= n; k++) { V[k] = U[k] * std::polar(1.0, -phi); }
            decide(V);
        }
        // refinement: Z e^-jwk = C0 + C1 S by least squares, then decide (Z e^-jwk - C0) / C1
        {
            std::complex<double> sS(0, 0), sY(0, 0), sYS(0, 0);
            double sSS = 0.0;
            std::vector<std::complex<double>> Y(n + 1);
            for (int k = 0; k <= n; k++) {
                Y[k] = Z[k] * std::polar(1.0, -w * k);
                std::complex<double> S = std::polar(1.0, p[k] * PI_D / 4.0);
                sS += S; sY += Y[k]; sYS += Y[k] * std::conj(S); sSS += 1.0;
            }
            const double N = n + 1;
            std::complex<double> det = N * sSS - std::norm(sS);
            if (std::abs(det) > 1e-12) {
                std::complex<double> C1 = (N * sYS - sY * std::conj(sS)) / det;
                std::complex<double> C0 = (sY - C1 * sS) / N;
                if (std::abs(C1) > 1e-12) {
                    std::vector<std::complex<double>> V(n + 1);
                    for (int k = 0; k <= n; k++) { V[k] = (Y[k] - C0) / C1 * std::polar(1.0, (k & 1) ? -PI_D / 4.0 : 0.0); }
                    decide(V);
                }
            }
        }
        for (int k = 1; k <= n; k++) {
            int step = ((p[k] - p[k - 1]) % 8 + 8) % 8;   // 1, 3, 5 or 7
            int qs = step == 1 ? 1 : step == 3 ? 3 : step == 5 ? -3 : -1;
            etsi_vem::phaseDibit(qs, bits + 2 * (k - 1));   // Z is already unmirrored
        }
    }

    // ---------------- ideal symbols of a burst ----------------
    struct Ideal {
        bool ok = false;
        UlType type = UL_NONE;
        Source src = SRC_NONE;
        int nsym = 0;
        uint8_t bits[NUB_BITS] = {};   // the burst as sent (as far as known)
        int bitErrors = 0;             // decided bits vs. rebuilt bits (blocks with CRC)
        const char* why = "";
    };
    // rx: decided bits of the burst (2 * symbols(type)); scr: scrambling code of
    // the cell (extended colour code, TS 100 392-2 8.2.5.2; 0 = cell not known:
    // decisions only)
    inline Ideal idealBits(UlType type, const uint8_t* rx, uint32_t scr) {
        Ideal r;
        r.type = type;
        r.nsym = symbols(type);
        if (type == UL_NONE) { r.why = "no burst"; return r; }
        const int nb = 2 * r.nsym;
        memcpy(r.bits, rx, nb);
        // known fields as the standard defines them
        memcpy(r.bits, T_BITS, 4);
        memcpy(r.bits + nb - 4, T_BITS, 4);
        int okBlocks = 0, blocks = 0;
        if (type == UL_CB) {
            memcpy(r.bits + 88, X_BITS, 30);
            blocks = 1;
            if (scr) {
                uint8_t cb[168], out[168];
                memcpy(cb, rx + 4, 84);
                memcpy(cb + 84, rx + 118, 84);
                // SCH/HU: 92 type-1 bits, (168, 13) interleaver (TS 100 392-2 8.3.1.4.3)
                if (etsi_vem::recodeBlock(cb, 168, 13, 112, 92, scr, out)) { buildCb(r.bits, out); okBlocks = 1; }
            }
        }
        else {
            const bool nts2 = type == UL_NUB2;
            memcpy(r.bits + 220, nts2 ? etsi_vem::P_BITS : etsi_vem::N_BITS, 22);
            if (!nts2) {
                // training sequence 1: TCH or SCH/F (268 type-1 bits, (432, 103))
                blocks = 1;
                if (scr) {
                    uint8_t blk[432], out[432];
                    memcpy(blk, rx + 4, 216);
                    memcpy(blk + 216, rx + 242, 216);
                    if (etsi_vem::recodeBlock(blk, 432, 103, 288, 268, scr, out)) { buildNub(r.bits, out, out + 216, false); okBlocks = 1; }
                }
            }
            else {
                // training sequence 2: two half blocks, STCH or SCH/HD (124 type-1 bits, (216, 101)), or STCH + TCH
                blocks = 2;
                if (scr) {
                    uint8_t out[216];
                    if (etsi_vem::recodeBlock(rx + 4, 216, 101, 144, 124, scr, out)) { memcpy(r.bits + 4, out, 216); okBlocks++; }
                    if (etsi_vem::recodeBlock(rx + 242, 216, 101, 144, 124, scr, out)) { memcpy(r.bits + 242, out, 216); okBlocks++; }
                }
            }
        }
        r.src = okBlocks == blocks ? SRC_CRC : (okBlocks > 0 ? SRC_PARTIAL : SRC_DECISION);
        r.bitErrors = etsi_vem::hamming(rx, r.bits, nb);
        r.ok = true;
        return r;
    }

    // ---------------- vector error fit ----------------
    // Model of TS 100 392-2 6.6.1.2 (equations 6.6 and 6.7): the symbol time,
    // the frequency (Theta, radians per symbol), the residual carrier C0 and the
    // complex amplitude C1 are estimated; amplitude changes over the burst are
    // not compensated, they are part of the vector error. The estimates are the
    // ones with the smallest sum square vector error (the standard does not fix
    // the criterion). sps: samples per symbol of the SDR (sample clock vs.
    // symbol rate), e.g. from the downlink measurement; a burst is too short to
    // estimate it on its own. S: ideal symbols SN1..SNmax.
    inline etsi_vem::Fit fitBurst(const std::vector<std::complex<float>>& y, double t1, const std::vector<std::complex<double>>& S, double sps = SPS) {
        double w = etsi_vem::slope(y, t1, S, sps);   // initial frequency
        auto cost = [&](double t, double ww) { return etsi_vem::evalModel(y, t, S, ww, false, nullptr, sps).cost; };
        double span = 2.0 * PI_D * 20.0 / etsi_vem::SYMRATE;   // +-20 Hz
        double tspan = 0.3;
        for (int it = 0; it < 3; it++) {
            w = etsi_vem::golden([&](double ww) { return cost(t1, ww); }, w - span, w + span, 20);
            t1 = etsi_vem::golden([&](double t) { return cost(t, w); }, t1 - tspan, t1 + tspan, 14);
            span /= 4.0;
            tspan /= 3.0;
        }
        return etsi_vem::evalModel(y, t1, S, w, false, nullptr, sps);
    }

    // ---------------- one burst ----------------
    struct Meas {
        bool ok = false;
        UlType type = UL_NONE;
        Source src = SRC_NONE;
        int nsym = 0;              // SNmax
        bool mirror = false;
        int bitErrors = 0;         // decisions vs. CRC-checked bits
        etsi_vem::Fit fit;         // rmsVe, peakVe (peakSym: 0 = SN1), residualCarrier
        double freqHz = NAN;       // df (TS 100 394-1 10.2.1) vs. the tap, in the tap's spectrum like the downlink error
        const char* why = "";
    };
    // centre / length: from the power detection (see findBurst); scr: scrambling
    // code of the cell (0: not known, ideal symbols from the decisions only)
    inline Meas measureBurst(const std::vector<std::complex<float>>& y, double centre, int length, uint32_t scr, double sps = SPS) {
        Meas m;
        Found f = findBurst(y, centre, length);
        if (!f.ok) { m.why = "no training sequence"; return m; }
        const int n = symbols(f.type);
        uint8_t rx[NUB_BITS];
        decideBits(y, f.t1, n, f.mirror, rx);
        Ideal id = idealBits(f.type, rx, scr);
        if (!id.ok) { m.why = id.why; return m; }
        std::vector<std::complex<double>> S = idealSymbols(id.bits, n, f.mirror);
        m.fit = fitBurst(y, f.t1, S, sps);
        if (!m.fit.ok) { m.why = "fit"; return m; }
        m.type = f.type;
        m.src = id.src;
        m.nsym = n;
        m.mirror = f.mirror;
        m.bitErrors = id.bitErrors;
        // S(k) carries the rotation sense, so the fit gives the frequency as seen
        // in the tap whatever the sense: no change of sign
        m.freqHz = m.fit.freqHz;
        m.ok = true;
        return m;
    }
}
