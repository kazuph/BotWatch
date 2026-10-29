/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ayutaz
 *
 * Plan A-prime decoder in C99 (E05): Ralomi layer 4 and 8 frames -> Action JSON piece ids,
 * grammar-constrained greedy. Same function as koeact.model.decoder_a2.greedy (eval mode).
 * Weights: scripts/a2_export.py weights ("A2W2": matrices as int16 rows with a float scale per row,
 * the rest float32). Grammar: firmware/a2/a2_grammar.h (generated). Arithmetic is float32; every
 * buffer comes from the caller's arena (PSRAM on the CoreS3). */
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define A2_LAYERS_IN 2
#define A2_DIN 320
#define A2_D 128
#define A2_HEADS 4
#define A2_FFN 384
#define A2_DEPTH 2
#define A2_KERNEL 5
#define A2_MAX_OUT 24 /* = A2_MAX_TOKENS of a2_grammar.h */

#define A2_TILE 16 /* encode steps per pass over a weight matrix */

typedef struct {
    const float *scale; /* [rows] */
    const int16_t *q;   /* [rows][cols]: W[r][c] = scale[r] * q[r][c] */
} a2_mat;

typedef struct {
    a2_mat in_w, out_w; /* in_w [3D, D]: q, k, v rows */
    const float *in_b, *out_b;
} a2_attn;

typedef struct {
    a2_attn self, cross;
    a2_mat l1_w, l2_w;
    const float *l1_b, *l2_b;
    const float *n1_w, *n1_b, *n2_w, *n2_b, *n3_w, *n3_b;
} a2_layer;

typedef struct {
    const float *mix, *norm_w, *norm_b, *proj_b, *conv_w, *conv_b, *pw_b, *embed, *pos;
    a2_mat proj_w, pw_w;
    a2_layer layers[A2_DEPTH];
    const float *out_norm_w, *out_norm_b, *out_b;
    a2_mat out_w;
} a2_model;

typedef struct {
    int ids[A2_MAX_OUT]; /* generated ids, </s> included when it was reached */
    int n;
    float min_prob, p_act;
} a2_result;

/* Point the model at a weights blob ("A2W2", payload bytes, tensors); 0 on success. The blob
 * must stay alive and 4-byte aligned. */
int a2_load(a2_model *m, const void *blob, size_t bytes);

/* Arena size for up to ``capacity`` frames. */
size_t a2_arena_bytes(int capacity);

/* Encode frames [n][A2_LAYERS_IN][A2_DIN] as steps at..at+n-1 (the encoder and the
 * cross-attention keys and values are causal per step, so the board runs this on each Ralomi
 * chunk while the user speaks; any split gives the same result). Steps 0..at-1 must already be
 * encoded into the same arena. 0 on success. */
int a2_encode(const a2_model *m, const float *frames, int at, int n, int capacity, void *arena);

/* Greedy decoding over steps 0..steps-1 of the arena (after the end of the utterance). */
int a2_generate(const a2_model *m, int steps, int capacity, void *arena, a2_result *out);

/* frames: [steps][A2_LAYERS_IN][A2_DIN]. a2_encode of all steps then a2_generate. */
int a2_decode(const a2_model *m, const float *frames, int steps, void *arena, a2_result *out);

/* Device glue: one chunk of the Ralomi stream model's int16 outputs layer4 and layer8
 * ([steps][A2_DIN] each, value = q * 2^exponent) into frames [.][A2_LAYERS_IN][A2_DIN] from
 * step ``at`` on. */
void a2_put_layers(float *frames, int at, int steps, const int16_t *layer4, int exp4,
                   const int16_t *layer8, int exp8);

#ifdef __cplusplus
}
#endif
