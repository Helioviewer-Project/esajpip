/*
 * The copyright in this software is being made available under the 2-clauses
 * BSD License, included below. This software may be subject to other third
 * party and contributor rights, including patent rights, and no such rights
 * are granted under this license.
 *
 * Copyright (c) 2002-2014, Universite catholique de Louvain (UCL), Belgium
 * Copyright (c) 2002-2014, Professor Benoit Macq
 * Copyright (c) 2001-2003, David Janssens
 * Copyright (c) 2002-2003, Yannick Verschueren
 * Copyright (c) 2003-2007, Francois-Olivier Devaux
 * Copyright (c) 2003-2014, Antonin Descampe
 * Copyright (c) 2005, Herve Drolon, FreeImage Team
 * Copyright (c) 2008, Jerome Fimes, Communications & Systemes <jerome.fimes@c-s.fr>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS `AS IS'
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef OPJ_MQC_INL_H
#define OPJ_MQC_INL_H

/* For internal use of opj_mqc_decode_macro() */
#define opj_mqc_mpsexchange_macro(d, curctx, a) \
{ \
    const OPJ_UINT32 w_ = *(curctx); \
    if (a < (w_ >> 16)) { \
        d = !(w_ & 1); \
        *curctx = opj_mqc_dstates_next[2 * (w_ & 0x7f) + 1]; \
    } else { \
        d = w_ & 1; \
        *curctx = opj_mqc_dstates_next[2 * (w_ & 0x7f)]; \
    } \
}

/* For internal use of opj_mqc_decode_macro() */
#define opj_mqc_lpsexchange_macro(d, curctx, a) \
{ \
    const OPJ_UINT32 w_ = *(curctx); \
    if (a < (w_ >> 16)) { \
        a = w_ >> 16; \
        d = w_ & 1; \
        *curctx = opj_mqc_dstates_next[2 * (w_ & 0x7f)]; \
    } else { \
        a = w_ >> 16; \
        d = !(w_ & 1); \
        *curctx = opj_mqc_dstates_next[2 * (w_ & 0x7f) + 1]; \
    } \
}


/**
Decode a symbol using raw-decoder. Cfr p.506 TAUBMAN
@param mqc MQC handle
@return Returns the decoded symbol (0 or 1)
*/
static INLINE OPJ_UINT32 opj_mqc_raw_decode(opj_mqc_t *mqc)
{
    OPJ_UINT32 d;
    if (mqc->ct == 0) {
        /* Given opj_mqc_raw_init_dec() we know that at some point we will */
        /* have a 0xFF 0xFF artificial marker */
        if (mqc->c == 0xff) {
            if (*mqc->bp  > 0x8f) {
                mqc->c = 0xff;
                mqc->ct = 8;
            } else {
                mqc->c = *mqc->bp;
                mqc->bp ++;
                mqc->ct = 7;
            }
        } else {
            mqc->c = *mqc->bp;
            mqc->bp ++;
            mqc->ct = 8;
        }
    }
    mqc->ct--;
    d = ((OPJ_UINT32)mqc->c >> mqc->ct) & 0x01U;

    return d;
}


/* The four bytes at p as a big-endian number. */
static INLINE OPJ_UINT32 opj_mqc_load_be32(const OPJ_BYTE *p)
{
    OPJ_UINT32 v;
    memcpy(&v, p, sizeof(v));
#if defined(OPJ_BIG_ENDIAN)
    return v;
#elif defined(__GNUC__)
    return __builtin_bswap32(v);
#else
    return ((v & 0xffU) << 24) | ((v & 0xff00U) << 8) |
           ((v >> 8) & 0xff00U) | (v >> 24);
#endif
}

/* Whether none of the four bytes at p is 0xff. */
static INLINE OPJ_BOOL opj_mqc_no_ff32(const OPJ_BYTE *p)
{
    OPJ_UINT32 v;
    memcpy(&v, p, sizeof(v));
    v = ~v;
    /* Nonzero exactly when a byte of v is zero. */
    return ((v - 0x01010101U) & ~v & 0x80808080U) == 0;
}

/* c holds the 16-bit code value in its top bits (48..63) and buffers up to 48
 * following bits below it. ct is the number of buffered bits, at most 16 when
 * a refill starts. A refill adds whole bytes, four at a time when no 0xff
 * byte stuffing is involved. Reading ahead only adds bits below the ones in
 * use, so decisions do not change. */
#define opj_mqc_bytein_macro(mqc, c, ct) \
{ \
        const OPJ_BYTE *bp_ = mqc->bp; \
        /* Given opj_mqc_init_dec() we know that at some point we will */ \
        /* have a 0xFF 0xFF artificial marker */ \
        /* With three bytes to end, bp_[4] is still in the marker. */ \
        if (mqc->end - bp_ >= 3 && opj_mqc_no_ff32(bp_)) { \
            c += (OPJ_UINT64)opj_mqc_load_be32(bp_ + 1) << (16 - ct); \
            mqc->bp += 4; \
            ct += 32; \
        } else { \
            do { \
                if (*mqc->bp == 0xff) { \
                    if (mqc->bp[1] > 0x8f) { \
                        c += (OPJ_UINT64)0xff << (40 - ct); \
                        ct += 8; \
                        mqc->end_of_byte_stream_counter ++; \
                    } else { \
                        mqc->bp++; \
                        c += (OPJ_UINT64)*mqc->bp << (41 - ct); \
                        ct += 7; \
                    } \
                } else { \
                    mqc->bp++; \
                    c += (OPJ_UINT64)*mqc->bp << (40 - ct); \
                    ct += 8; \
                } \
            } while (ct <= 40); \
        } \
}

/* For internal use of opj_mqc_decode_macro() */
#define opj_mqc_renormd_macro(mqc, a, c, ct) \
{ \
    do { \
        if (ct == 0) { \
            opj_mqc_bytein_macro(mqc, c, ct); \
        } \
        a <<= 1; \
        c <<= 1; \
        ct--; \
    } while (a < 0x8000); \
}

#define opj_mqc_decode_macro(d, mqc, curctx, a, c, ct) \
{ \
    /* Implements ISO 15444-1 C.3.2 Decoding a decision (DECODE) */ \
    /* Note: alternate "J.2 - Decoding an MPS or an LPS in the */ \
    /* software-conventions decoder" has been tried, but does not bring any */ \
    /* improvement. See https://github.com/uclouvain/openjpeg/issues/921 */ \
    a -= *(curctx) >> 16;  \
    if ((c >> 48) < (*(curctx) >> 16)) {  \
        opj_mqc_lpsexchange_macro(d, curctx, a);  \
        opj_mqc_renormd_macro(mqc, a, c, ct);  \
    } else {  \
        c -= (OPJ_UINT64)(*(curctx) >> 16) << 48;  \
        if ((a & 0x8000) == 0) { \
            opj_mqc_mpsexchange_macro(d, curctx, a); \
            opj_mqc_renormd_macro(mqc, a, c, ct); \
        } else { \
            d = *(curctx) & 1; \
        } \
    } \
}

/* The left shift that brings a (1 to 0xffff) to at least 0x8000. */
static INLINE OPJ_UINT32 opj_mqc_renorm_shift(OPJ_UINT32 a)
{
#if defined(__GNUC__)
    return (OPJ_UINT32)__builtin_clz(a) - 16U;
#elif defined(_MSC_VER)
    unsigned long msb;
    _BitScanReverse(&msb, a);
    return 15U - (OPJ_UINT32)msb;
#else
    OPJ_UINT32 n = 0;
    for (; a < 0x8000; a <<= 1) {
        ++n;
    }
    return n;
#endif
}

/* The same decision as opj_mqc_decode_macro(), for contexts whose symbols are
 * close to random: selects replace the LPS/MPS branches and one shift the
 * renormalization loop, so a mispredicted symbol costs no branch. Only a
 * renormalization that needs a new byte loops. */
#define opj_mqc_decode_branchless_macro(d, mqc, curctx, a, c, ct) \
{ \
    const OPJ_UINT32 state_ = *(curctx); \
    OPJ_UINT32 qeval_ = state_ >> 16, lps_, exchange_, shift_, next_; \
    a -= qeval_; \
    lps_ = (c >> 48) < qeval_; \
    exchange_ = lps_ ^ (a < qeval_); \
    d = (state_ & 1) ^ exchange_; \
    c -= lps_ ? 0 : (OPJ_UINT64)qeval_ << 48; \
    a = lps_ ? qeval_ : a; \
    shift_ = opj_mqc_renorm_shift(a); \
    /* The state changes only with a renormalization. */ \
    next_ = opj_mqc_dstates_next[2 * (state_ & 0x7f) + exchange_]; \
    *(curctx) = state_ ^ ((state_ ^ next_) & (0U - (OPJ_UINT32)(shift_ != 0))); \
    if (shift_ > ct) { \
        opj_mqc_bytein_macro(mqc, c, ct); \
    } \
    a <<= shift_; \
    c <<= shift_; \
    ct -= shift_; \
}

#define DOWNLOAD_MQC_DEC_VARIABLES(mqc, curctx, a, c, ct) \
        register OPJ_UINT32 *curctx = mqc->dcurctx; \
        register OPJ_UINT64 c = mqc->c; \
        register OPJ_UINT32 a = mqc->a; \
        register OPJ_UINT32 ct = mqc->ct

#define UPLOAD_MQC_DEC_VARIABLES(mqc, curctx, a, c, ct) \
        mqc->dcurctx = curctx; \
        mqc->c = c; \
        mqc->a = a; \
        mqc->ct = ct;

#define DOWNLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        register const opj_mqc_state_t **curctx = mqc->curctx; \
        register OPJ_UINT64 c = mqc->c; \
        register OPJ_UINT32 a = mqc->a; \
        register OPJ_UINT32 ct = mqc->ct

#define UPLOAD_MQC_VARIABLES(mqc, curctx, a, c, ct) \
        mqc->curctx = curctx; \
        mqc->c = c; \
        mqc->a = a; \
        mqc->ct = ct;

/**
Input a byte
@param mqc MQC handle
*/
static INLINE void opj_mqc_bytein(opj_mqc_t *const mqc)
{
    opj_mqc_bytein_macro(mqc, mqc->c, mqc->ct);
}

/**
Renormalize mqc->a and mqc->c while decoding
@param mqc MQC handle
*/
#define opj_mqc_renormd(mqc) \
    opj_mqc_renormd_macro(mqc, mqc->a, mqc->c, mqc->ct)

/**
Decode a symbol
@param d OPJ_UINT32 value where to store the decoded symbol
@param mqc MQC handle
@return Returns the decoded symbol (0 or 1) in d
*/
#define opj_mqc_decode(d, mqc) \
    opj_mqc_decode_macro(d, mqc, mqc->dcurctx, mqc->a, mqc->c, mqc->ct)

/**
Output a byte, doing bit-stuffing if necessary.
After a 0xff byte, the next byte must be smaller than 0x90.
@param mqc MQC handle
*/
void opj_mqc_byteout(opj_mqc_t *mqc);

/**
Renormalize mqc->a and mqc->c while encoding, so that mqc->a stays between 0x8000 and 0x10000
@param mqc MQC handle
@param a_ value of mqc->a
@param c_ value of mqc->c_
@param ct_ value of mqc->ct_
*/
#define opj_mqc_renorme_macro(mqc, a_, c_, ct_) \
{ \
    do { \
        a_ <<= 1; \
        c_ <<= 1; \
        ct_--; \
        if (ct_ == 0) { \
            mqc->c = c_; \
            opj_mqc_byteout(mqc); \
            c_ = mqc->c; \
            ct_ = mqc->ct; \
        } \
    } while( (a_ & 0x8000) == 0); \
}

#define opj_mqc_codemps_macro(mqc, curctx, a, c, ct) \
{ \
    a -= (*curctx)->qeval; \
    if ((a & 0x8000) == 0) { \
        if (a < (*curctx)->qeval) { \
            a = (*curctx)->qeval; \
        } else { \
            c += (*curctx)->qeval; \
        } \
        *curctx = (*curctx)->nmps; \
        opj_mqc_renorme_macro(mqc, a, c, ct); \
    } else { \
        c += (*curctx)->qeval; \
    } \
}

#define opj_mqc_codelps_macro(mqc, curctx, a, c, ct) \
{ \
    a -= (*curctx)->qeval; \
    if (a < (*curctx)->qeval) { \
        c += (*curctx)->qeval; \
    } else { \
        a = (*curctx)->qeval; \
    } \
    *curctx = (*curctx)->nlps; \
    opj_mqc_renorme_macro(mqc, a, c, ct); \
}

#define opj_mqc_encode_macro(mqc, curctx, a, c, ct, d) \
{ \
    if ((*curctx)->mps == (d)) { \
        opj_mqc_codemps_macro(mqc, curctx, a, c, ct); \
    } else { \
        opj_mqc_codelps_macro(mqc, curctx, a, c, ct); \
    } \
}


#define opj_mqc_bypass_enc_macro(mqc, c, ct, d) \
{\
    if (ct == BYPASS_CT_INIT) {\
        ct = 8;\
    }\
    ct--;\
    c = c + ((d) << ct);\
    if (ct == 0) {\
        *mqc->bp = (OPJ_BYTE)c;\
        ct = 8;\
        /* If the previous byte was 0xff, make sure that the next msb is 0 */ \
        if (*mqc->bp == 0xff) {\
            ct = 7;\
        }\
        mqc->bp++;\
        c = 0;\
    }\
}

#endif /* OPJ_MQC_INL_H */
