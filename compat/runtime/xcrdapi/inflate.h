// Минимальный inflate (RFC 1950/1951), по схеме puff (D. Adler, public domain algorithm) — собственная реализация.
#pragma once
#include <cstdint>
#include <cstddef>
namespace infl {
struct St { const uint8_t* in; size_t inlen, incnt; uint32_t buf; int cnt; uint8_t* out; size_t outlen, outcnt; };
struct Huff { uint16_t count[16]; uint16_t symbol[288]; };
static inline int bits(St* s, int need)
{
    uint32_t v = s->buf;
    while (s->cnt < need) { if (s->incnt == s->inlen) return -1; v |= (uint32_t)s->in[s->incnt++] << s->cnt; s->cnt += 8; }
    s->buf = v >> need; s->cnt -= need; return (int)(v & ((1u << need) - 1));
}
static inline int construct(Huff* h, const uint16_t* length, int n)
{
    for (int l = 0; l < 16; l++) h->count[l] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; l++) { left <<= 1; left -= h->count[l]; if (left < 0) return left; }
    uint16_t offs[16]; offs[1] = 0;
    for (int l = 1; l < 15; l++) offs[l + 1] = offs[l] + h->count[l];
    for (int i = 0; i < n; i++) if (length[i]) h->symbol[offs[length[i]]++] = (uint16_t)i;
    return left;
}
static inline int decode(St* s, const Huff* h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        int b = bits(s, 1); if (b < 0) return -10;
        code |= b; int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -10;
}
static inline int codes(St* s, const Huff* lencode, const Huff* distcode)
{
    static const uint16_t lens[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const uint16_t lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const uint16_t dists[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const uint16_t dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    int sym;
    do {
        sym = decode(s, lencode); if (sym < 0) return sym;
        if (sym < 256) { if (s->outcnt == s->outlen) return 1; s->out[s->outcnt++] = (uint8_t)sym; }
        else if (sym > 256) {
            sym -= 257; if (sym >= 29) return -10;
            int e = bits(s, lext[sym]); if (e < 0) return -1; int len = lens[sym] + e;
            sym = decode(s, distcode); if (sym < 0) return sym; if (sym >= 30) return -10;
            e = bits(s, dext[sym]); if (e < 0) return -1; size_t dist = dists[sym] + e;
            if (dist > s->outcnt) return -11;
            if (s->outcnt + len > s->outlen) return 1;
            while (len--) { s->out[s->outcnt] = s->out[s->outcnt - dist]; s->outcnt++; }
        }
    } while (sym != 256);
    return 0;
}
static inline int fixed(St* s)
{
    static bool init = false; static Huff lc, dc;
    if (!init) { uint16_t l[288]; int i; for (i = 0; i < 144; i++) l[i] = 8; for (; i < 256; i++) l[i] = 9; for (; i < 280; i++) l[i] = 7; for (; i < 288; i++) l[i] = 8;
                 construct(&lc, l, 288); for (i = 0; i < 30; i++) l[i] = 5; construct(&dc, l, 30); init = true; }
    return codes(s, &lc, &dc);
}
static inline int dynamic(St* s)
{
    static const uint16_t order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint16_t lengths[320]; Huff lc, dc;
    int nlen = bits(s, 5), ndist = bits(s, 5), ncode = bits(s, 4); if (nlen < 0 || ndist < 0 || ncode < 0) return -1;
    nlen += 257; ndist += 1; ncode += 4; if (nlen > 286 || ndist > 30) return -3;
    int i; for (i = 0; i < ncode; i++) { int b = bits(s, 3); if (b < 0) return -1; lengths[order[i]] = (uint16_t)b; }
    for (; i < 19; i++) lengths[order[i]] = 0;
    if (construct(&lc, lengths, 19) != 0) return -4;
    int idx = 0;
    while (idx < nlen + ndist) {
        int sym = decode(s, &lc); if (sym < 0) return sym;
        if (sym < 16) lengths[idx++] = (uint16_t)sym;
        else {
            int len = 0, rep;
            if (sym == 16) { if (idx == 0) return -5; len = lengths[idx - 1]; int b = bits(s, 2); if (b < 0) return -1; rep = 3 + b; }
            else if (sym == 17) { int b = bits(s, 3); if (b < 0) return -1; rep = 3 + b; }
            else { int b = bits(s, 7); if (b < 0) return -1; rep = 11 + b; }
            if (idx + rep > nlen + ndist) return -6;
            while (rep--) lengths[idx++] = (uint16_t)len;
        }
    }
    if (lengths[256] == 0) return -9;
    int err = construct(&lc, lengths, nlen); if (err && (err < 0 || nlen - lc.count[0] != 1)) return -7;
    err = construct(&dc, lengths + nlen, ndist); if (err && (err < 0 || ndist - dc.count[0] != 1)) return -8;
    return codes(s, &lc, &dc);
}
// raw deflate -> 0 ok; *outlen = записано. 1 = переполнение выходного буфера.
static inline int inflate_raw(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen)
{
    St s = { in, inlen, 0, 0, 0, out, *outlen, 0 }; int last, err = 0;
    do {
        last = bits(&s, 1); int type = bits(&s, 2); if (last < 0 || type < 0) { err = -1; break; }
        if (type == 0) {
            s.buf = 0; s.cnt = 0; if (s.incnt + 4 > s.inlen) { err = -2; break; }
            unsigned len = in[s.incnt] | (in[s.incnt + 1] << 8); unsigned nlen = in[s.incnt + 2] | (in[s.incnt + 3] << 8); s.incnt += 4;
            if (len != (~nlen & 0xffff)) { err = -2; break; }
            if (s.incnt + len > s.inlen) { err = -2; break; }
            if (s.outcnt + len > s.outlen) { err = 1; break; }
            for (unsigned i = 0; i < len; i++) s.out[s.outcnt++] = in[s.incnt++];
        } else if (type == 1) err = fixed(&s); else if (type == 2) err = dynamic(&s); else err = -1;
        if (err) break;
    } while (!last);
    *outlen = s.outcnt; return err;
}
// zlib-обёртка (RFC 1950): заголовок 2 байта (CM=8), Adler-32 в конце не проверяется.
static inline int inflate_zlib(const uint8_t* in, size_t inlen, uint8_t* out, size_t* outlen)
{
    if (inlen < 2 || (in[0] & 0x0f) != 8 || ((in[0] << 8) | in[1]) % 31 != 0 || (in[1] & 0x20)) return -20;
    return inflate_raw(in + 2, inlen - 2, out, outlen);
}
}
