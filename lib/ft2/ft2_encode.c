#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdbool.h>

#include "ft8_lib/ft8/message.h"
#include "ft8_lib/ft8/encode.h"
#include "ft8_lib/ft8/crc.h"
#include "ft8_lib/ft8/constants.h"
#include "ft2_constants.h"

#define GFSK_CONST_K 5.336446f  /* pi * sqrt(2 / ln(2)) */

static void gfsk_pulse(int n_spsym, float symbol_bt, float* pulse)
{
    for (int i = 0; i < 3 * n_spsym; ++i)
    {
        float t = i / (float)n_spsym - 1.5f;
        float arg1 = GFSK_CONST_K * symbol_bt * (t + 0.5f);
        float arg2 = GFSK_CONST_K * symbol_bt * (t - 0.5f);
        pulse[i] = (erff(arg1) - erff(arg2)) / 2;
    }
}

static void synth_gfsk(const uint8_t* symbols, int n_sym, float f0,
                        float symbol_bt, float symbol_period,
                        int signal_rate, float* signal)
{
    int n_spsym = (int)(0.5f + signal_rate * symbol_period);
    int n_wave = n_sym * n_spsym;
    float hmod = 1.0f;

    float dphi_peak = 2 * M_PI * hmod / n_spsym;
    float dphi[n_wave + 2 * n_spsym];

    for (int i = 0; i < n_wave + 2 * n_spsym; ++i)
        dphi[i] = 2 * M_PI * f0 / signal_rate;

    float pulse[3 * n_spsym];
    gfsk_pulse(n_spsym, symbol_bt, pulse);

    for (int i = 0; i < n_sym; ++i)
    {
        int ib = i * n_spsym;
        for (int j = 0; j < 3 * n_spsym; ++j)
            dphi[j + ib] += dphi_peak * symbols[i] * pulse[j];
    }

    /* Dummy symbols at edges */
    for (int j = 0; j < 2 * n_spsym; ++j)
    {
        dphi[j] += dphi_peak * pulse[j + n_spsym] * symbols[0];
        dphi[j + n_sym * n_spsym] += dphi_peak * pulse[j] * symbols[n_sym - 1];
    }

    float phi = 0;
    for (int k = 0; k < n_wave; ++k)
    {
        signal[k] = sinf(phi);
        phi = fmodf(phi + dphi[k + n_spsym], 2 * M_PI);
    }

    /* Envelope shaping on first and last symbols */
    int n_ramp = n_spsym / 8;
    for (int i = 0; i < n_ramp; ++i)
    {
        float env = (1 - cosf(2 * M_PI * i / (2 * n_ramp))) / 2;
        signal[i] *= env;
        signal[n_wave - 1 - i] *= env;
    }
}

/*
 * Encode an FT2 message to audio samples.
 * Returns 0 on success, -2 on message parse error.
 * Output signal must have space for FT2_TX_SAMPLES (30240) floats.
 */
int ft2_exec_encode(char* message, float frequency, float* signal)
{
    /* 1. Pack text message to 77-bit payload */
    ftx_message_t msg;
    ftx_message_rc_t rc = ftx_message_encode(&msg, NULL, message);
    if (rc != FTX_MESSAGE_RC_OK)
        return -2;

    /* 2. XOR with FT2 randomization vector */
    uint8_t payload_xor[10];
    for (int i = 0; i < 10; ++i)
        payload_xor[i] = msg.payload[i] ^ kFT2_XOR_sequence[i];

    /* 3. Add CRC14 and LDPC encode */
    uint8_t a91[FTX_LDPC_K_BYTES];
    ftx_add_crc(payload_xor, a91);

    uint8_t codeword[FTX_LDPC_N_BYTES];
    /* Use the same encode174 as ft8_lib — via ft4_encode's approach */
    /* We replicate the LDPC encoding + bit-pair extraction inline */

    /* --- LDPC encode (174,91) --- */
    /* Copy a91 into codeword and compute parity bits */
    for (int j = 0; j < FTX_LDPC_N_BYTES; ++j)
        codeword[j] = (j < FTX_LDPC_K_BYTES) ? a91[j] : 0;

    uint8_t col_mask = (0x80u >> (FTX_LDPC_K % 8u));
    uint8_t col_idx = FTX_LDPC_K_BYTES - 1;
    for (int i = 0; i < FTX_LDPC_M; ++i)
    {
        uint8_t nsum = 0;
        for (int j = 0; j < FTX_LDPC_K_BYTES; ++j)
        {
            uint8_t bits = a91[j] & kFTX_LDPC_generator[i][j];
            bits ^= bits >> 4;
            bits ^= bits >> 2;
            bits ^= bits >> 1;
            nsum ^= (bits & 1);
        }
        if (nsum)
            codeword[col_idx] |= col_mask;
        col_mask >>= 1;
        if (col_mask == 0) { col_mask = 0x80u; ++col_idx; }
    }

    /* 4. Extract bit-pairs and Gray-map to 4 tones -> 87 data tones */
    uint8_t data_tones[FT2_ND]; /* 87 */
    uint8_t mask = 0x80u;
    int i_byte = 0;
    for (int i = 0; i < FT2_ND; ++i)
    {
        uint8_t bits2 = 0;
        if (codeword[i_byte] & mask) bits2 |= 2;
        if (0 == (mask >>= 1)) { mask = 0x80u; i_byte++; }
        if (codeword[i_byte] & mask) bits2 |= 1;
        if (0 == (mask >>= 1)) { mask = 0x80u; i_byte++; }
        data_tones[i] = kFT2_Gray_map[bits2];
    }

    /* 5. Assemble 105-symbol frame */
    uint8_t tones[FT2_NN]; /* 105 */
    int di = 0; /* data tone index */

    for (int i = 0; i < FT2_NN; ++i)
    {
        if (i == 0 || i == 104)
        {
            tones[i] = 0; /* ramp */
        }
        else if (i >= FT2_SYNC_POS_A && i < FT2_SYNC_POS_A + FT2_LENGTH_SYNC)
        {
            tones[i] = kFT2_Costas[0][i - FT2_SYNC_POS_A];
        }
        else if (i >= FT2_SYNC_POS_B && i < FT2_SYNC_POS_B + FT2_LENGTH_SYNC)
        {
            tones[i] = kFT2_Costas[1][i - FT2_SYNC_POS_B];
        }
        else if (i >= FT2_SYNC_POS_C && i < FT2_SYNC_POS_C + FT2_LENGTH_SYNC)
        {
            tones[i] = kFT2_Costas[2][i - FT2_SYNC_POS_C];
        }
        else if (i >= FT2_SYNC_POS_D && i < FT2_SYNC_POS_D + FT2_LENGTH_SYNC)
        {
            tones[i] = kFT2_Costas[3][i - FT2_SYNC_POS_D];
        }
        else
        {
            tones[i] = data_tones[di++];
        }
    }

    /* 6. Synthesize GFSK waveform */
    memset(signal, 0, FT2_TX_SAMPLES * sizeof(float));
    synth_gfsk(tones, FT2_NN, frequency, FT2_SYMBOL_BT,
               FT2_SYMBOL_PERIOD, FT2_SAMPLE_RATE, signal);

    return 0;
}
