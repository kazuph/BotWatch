/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 ayutaz
 *
 * Plan A-prime decoder in C99; see a2.h. The PyTorch reference is koeact.model.decoder_a2:
 * nn.TransformerDecoderLayer with norm_first, ReLU FFN, LayerNorm eps 1e-5, no final decoder
 * norm, and greedy decoding where an id is chosen among the grammar's allowed ids (ties go to
 * the lowest id) while min_prob and p_act use the unconstrained softmax. */
#include "a2.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "a2_grammar.h"

#if A2_MAX_TOKENS != A2_MAX_OUT
#error "a2_grammar.h and a2.h disagree on the token limit"
#endif

#define LN_EPS 1e-5f
#define HEAD_DIM (A2_D / A2_HEADS)

static size_t mat_bytes(size_t rows, size_t cols) { return 4 * rows + 2 * rows * cols; }

static size_t model_bytes(void) {
    const size_t d = A2_D;
    const size_t layer = 2 * (mat_bytes(3 * d, d) + 4 * 3 * d + mat_bytes(d, d) + 4 * d) +
                         mat_bytes(A2_FFN, d) + 4 * A2_FFN + mat_bytes(d, A2_FFN) + 4 * d +
                         4 * 6 * d;
    return 4 * (A2_LAYERS_IN + 2 * A2_DIN) + mat_bytes(d, A2_DIN) + 4 * d +
           4 * (A2_KERNEL * d + d) + mat_bytes(d, d) + 4 * d + 4 * A2_VOCAB * d +
           4 * (A2_MAX_TOKENS + 1) * d + A2_DEPTH * layer + 4 * 2 * d + mat_bytes(A2_VOCAB, d) +
           4 * A2_VOCAB;
}

int a2_load(a2_model *m, const void *blob, size_t bytes) {
    const uint8_t *p = (const uint8_t *)blob;
    uint32_t payload;
    if (bytes < 8 || memcmp(p, "A2W2", 4) != 0) return -1;
    memcpy(&payload, p + 4, 4);
    if (payload != model_bytes() || bytes != 8 + (size_t)payload) return -2;
    p += 8;
#define TAKE(field, n) (m->field = (const float *)p, p += 4 * (n))
#define TAKE_MAT(field, rows, cols)                                                     \
    (m->field.scale = (const float *)p, p += 4 * (rows), m->field.q = (const int16_t *)p, \
     p += 2 * (rows) * (cols))
    const size_t d = A2_D;
    TAKE(mix, A2_LAYERS_IN);
    TAKE(norm_w, A2_DIN);
    TAKE(norm_b, A2_DIN);
    TAKE_MAT(proj_w, d, A2_DIN);
    TAKE(proj_b, d);
    TAKE(conv_w, A2_KERNEL * d);
    TAKE(conv_b, d);
    TAKE_MAT(pw_w, d, d);
    TAKE(pw_b, d);
    TAKE(embed, A2_VOCAB * d);
    TAKE(pos, (A2_MAX_TOKENS + 1) * d);
    for (int k = 0; k < A2_DEPTH; k++) {
        TAKE_MAT(layers[k].self.in_w, 3 * d, d);
        TAKE(layers[k].self.in_b, 3 * d);
        TAKE_MAT(layers[k].self.out_w, d, d);
        TAKE(layers[k].self.out_b, d);
        TAKE_MAT(layers[k].cross.in_w, 3 * d, d);
        TAKE(layers[k].cross.in_b, 3 * d);
        TAKE_MAT(layers[k].cross.out_w, d, d);
        TAKE(layers[k].cross.out_b, d);
        TAKE_MAT(layers[k].l1_w, A2_FFN, d);
        TAKE(layers[k].l1_b, A2_FFN);
        TAKE_MAT(layers[k].l2_w, d, A2_FFN);
        TAKE(layers[k].l2_b, d);
        TAKE(layers[k].n1_w, d);
        TAKE(layers[k].n1_b, d);
        TAKE(layers[k].n2_w, d);
        TAKE(layers[k].n2_b, d);
        TAKE(layers[k].n3_w, d);
        TAKE(layers[k].n3_b, d);
    }
    TAKE(out_norm_w, d);
    TAKE(out_norm_b, d);
    TAKE_MAT(out_w, A2_VOCAB, d);
    TAKE(out_b, A2_VOCAB);
#undef TAKE
#undef TAKE_MAT
    return 0;
}

/* Arena layout (floats): h0 [capacity, D]; cross K and V per layer [capacity, D]; self K and V
 * per layer [MAX+1, D]; encode tiles; per-token scratch. */
size_t a2_arena_bytes(int capacity) {
    const size_t s = (size_t)capacity, d = A2_D, t = A2_MAX_TOKENS + 1;
    size_t att = s > t ? s : t;
    return sizeof(float) * (s * d + 2 * A2_DEPTH * s * d + 2 * A2_DEPTH * t * d +
                            A2_TILE * (A2_DIN + 2 * d) + 5 * d + A2_FFN + att + A2_VOCAB);
}

/* Rows r0.. of a matrix with ``cols`` columns. */
static a2_mat rows_from(a2_mat w, int r0, int cols) {
    a2_mat r = {w.scale + r0, w.q + (size_t)r0 * cols};
    return r;
}

static float dot(const int16_t *w, const float *x, int n) { /* n is a multiple of 4 */
    float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
    for (int i = 0; i < n; i += 4) {
        s0 += w[i] * x[i];
        s1 += w[i + 1] * x[i + 1];
        s2 += w[i + 2] * x[i + 2];
        s3 += w[i + 3] * x[i + 3];
    }
    return (s0 + s1) + (s2 + s3);
}

/* y[j][n_out] = W[n_out, n_in] x[j] + b for j < n (rows of x and y are contiguous). The loop over
 * j is inside, so each weight row is read from flash once for all n inputs. */
static void linear(a2_mat w, const float *b, const float *x, int n, int n_in, int n_out,
                   float *y) {
    for (int o = 0; o < n_out; o++) {
        const int16_t *row = w.q + (size_t)o * n_in;
        const float scale = w.scale[o], bias = b ? b[o] : 0.0f;
        for (int j = 0; j < n; j++)
            y[(size_t)j * n_out + o] = scale * dot(row, x + (size_t)j * n_in, n_in) + bias;
    }
}

static void layer_norm(const float *x, const float *w, const float *b, int n, float *y) {
    float mean = 0.0f, var = 0.0f;
    for (int i = 0; i < n; i++) mean += x[i];
    mean /= (float)n;
    for (int i = 0; i < n; i++) var += (x[i] - mean) * (x[i] - mean);
    float inv = 1.0f / sqrtf(var / (float)n + LN_EPS);
    for (int i = 0; i < n; i++) y[i] = (x[i] - mean) * inv * w[i] + b[i];
}

/* out[D] = attention of q[D] over rows 0..n-1 of K, V [n, D], per head */
static void attend(const float *q, const float *k, const float *v, int n, float *score,
                   float *out) {
    const float scale = 1.0f / sqrtf((float)HEAD_DIM);
    for (int h = 0; h < A2_HEADS; h++) {
        const int o = h * HEAD_DIM;
        float top = -INFINITY, sum = 0.0f;
        for (int j = 0; j < n; j++) {
            float s = 0.0f;
            for (int c = 0; c < HEAD_DIM; c++) s += q[o + c] * k[(size_t)j * A2_D + o + c];
            score[j] = s * scale;
            if (score[j] > top) top = score[j];
        }
        for (int j = 0; j < n; j++) sum += (score[j] = expf(score[j] - top));
        for (int c = 0; c < HEAD_DIM; c++) out[o + c] = 0.0f;
        for (int j = 0; j < n; j++) {
            const float p = score[j] / sum;
            for (int c = 0; c < HEAD_DIM; c++) out[o + c] += p * v[(size_t)j * A2_D + o + c];
        }
    }
}

typedef struct {
    float *h0, *ck[A2_DEPTH], *cv[A2_DEPTH], *sk[A2_DEPTH], *sv[A2_DEPTH];
    float *tin, *th, *tm; /* encode tiles: [A2_TILE, A2_DIN], [A2_TILE, D], [A2_TILE, D] */
    float *x, *xn, *q, *a, *o, *ffn, *score, *logits;
} buffers;

static void carve(void *arena, int capacity, buffers *b) {
    float *p = (float *)arena;
    const size_t s = (size_t)capacity, d = A2_D, t = A2_MAX_TOKENS + 1;
    b->h0 = p, p += s * d;
    for (int k = 0; k < A2_DEPTH; k++) {
        b->ck[k] = p, p += s * d;
        b->cv[k] = p, p += s * d;
        b->sk[k] = p, p += t * d;
        b->sv[k] = p, p += t * d;
    }
    b->tin = p, p += A2_TILE * A2_DIN;
    b->th = p, p += A2_TILE * d;
    b->tm = p, p += A2_TILE * d;
    b->x = p, p += d;
    b->xn = p, p += d;
    b->q = p, p += d;
    b->a = p, p += d;
    b->o = p, p += d;
    b->ffn = p, p += A2_FFN;
    b->score = p, p += s > t ? s : t;
    b->logits = p;
}

/* Steps t0..t0+n-1 (n <= A2_TILE); h0 of the 4 steps before t0 is already in the arena. */
static void encode_tile(const a2_model *m, const float *frames, int t0, int n, buffers *b) {
    const int d = A2_D;
    float w0 = m->mix[0], w1 = m->mix[1], top = w0 > w1 ? w0 : w1;
    w0 = expf(w0 - top), w1 = expf(w1 - top);
    const float s0 = w0 / (w0 + w1), s1 = w1 / (w0 + w1);
    for (int j = 0; j < n; j++) {
        const float *f = frames + (size_t)j * A2_LAYERS_IN * A2_DIN;
        float *row = b->tin + (size_t)j * A2_DIN;
        for (int c = 0; c < A2_DIN; c++) row[c] = f[c] * s0 + f[A2_DIN + c] * s1;
        layer_norm(row, m->norm_w, m->norm_b, A2_DIN, row);
    }
    float *h0 = b->h0 + (size_t)t0 * d;
    linear(m->proj_w, m->proj_b, b->tin, n, A2_DIN, d, h0);
    for (int j = 0; j < n; j++) { /* causal depthwise conv */
        for (int c = 0; c < d; c++) {
            float acc = m->conv_b[c];
            for (int k = 0; k < A2_KERNEL; k++) {
                const int src = t0 + j + k - (A2_KERNEL - 1);
                if (src >= 0) acc += m->conv_w[k * d + c] * b->h0[(size_t)src * d + c];
            }
            b->th[(size_t)j * d + c] = acc;
        }
    }
    linear(m->pw_w, m->pw_b, b->th, n, d, d, b->tm); /* pointwise, ReLU, residual */
    for (size_t i = 0; i < (size_t)n * d; i++) b->tm[i] = h0[i] + (b->tm[i] > 0 ? b->tm[i] : 0);
    for (int k = 0; k < A2_DEPTH; k++) {
        const a2_attn *at = &m->layers[k].cross;
        linear(rows_from(at->in_w, d, d), at->in_b + d, b->tm, n, d, d, b->ck[k] + (size_t)t0 * d);
        linear(rows_from(at->in_w, 2 * d, d), at->in_b + 2 * d, b->tm, n, d, d,
               b->cv[k] + (size_t)t0 * d);
    }
}

/* Logits for the token at position pos (the self-attention cache holds 0..pos-1). */
static void step(const a2_model *m, int token, int pos, int steps, buffers *b) {
    const int d = A2_D;
    for (int c = 0; c < d; c++)
        b->x[c] = m->embed[(size_t)token * d + c] + m->pos[(size_t)pos * d + c];
    for (int k = 0; k < A2_DEPTH; k++) {
        const a2_layer *L = &m->layers[k];
        layer_norm(b->x, L->n1_w, L->n1_b, d, b->xn);
        linear(L->self.in_w, L->self.in_b, b->xn, 1, d, d, b->q);
        linear(rows_from(L->self.in_w, d, d), L->self.in_b + d, b->xn, 1, d, d,
               b->sk[k] + (size_t)pos * d);
        linear(rows_from(L->self.in_w, 2 * d, d), L->self.in_b + 2 * d, b->xn, 1, d, d,
               b->sv[k] + (size_t)pos * d);
        attend(b->q, b->sk[k], b->sv[k], pos + 1, b->score, b->a);
        linear(L->self.out_w, L->self.out_b, b->a, 1, d, d, b->o);
        for (int c = 0; c < d; c++) b->x[c] += b->o[c];

        layer_norm(b->x, L->n2_w, L->n2_b, d, b->xn);
        linear(L->cross.in_w, L->cross.in_b, b->xn, 1, d, d, b->q);
        attend(b->q, b->ck[k], b->cv[k], steps, b->score, b->a);
        linear(L->cross.out_w, L->cross.out_b, b->a, 1, d, d, b->o);
        for (int c = 0; c < d; c++) b->x[c] += b->o[c];

        layer_norm(b->x, L->n3_w, L->n3_b, d, b->xn);
        linear(L->l1_w, L->l1_b, b->xn, 1, d, A2_FFN, b->ffn);
        for (int c = 0; c < A2_FFN; c++) b->ffn[c] = b->ffn[c] > 0 ? b->ffn[c] : 0;
        linear(L->l2_w, L->l2_b, b->ffn, 1, A2_FFN, d, b->o);
        for (int c = 0; c < d; c++) b->x[c] += b->o[c];
    }
    layer_norm(b->x, m->out_norm_w, m->out_norm_b, d, b->xn);
    linear(m->out_w, m->out_b, b->xn, 1, d, A2_VOCAB, b->logits);
}

/* Grammar state (koeact.model.pieces.ActionGrammar): [] </s> | [ CALL ] </s> |
 * [ CALL , CALL ] </s>, the second call different from the first. */
enum { G_START, G_CALL, G_AFTER, G_END };

typedef struct {
    int state, node, depth, calls;
    uint16_t first[8]; /* trie nodes along the first call (calls have at most 7 pieces) */
} grammar;

static int allowed(const grammar *g, int *ids) {
    int n = 0;
    if (g->state == G_START) {
        ids[n++] = A2_EMPTY;
        ids[n++] = A2_OPEN;
    } else if (g->state == G_CALL) {
        const int lo = a2_node_first[g->node], kids = a2_node_kids[g->node];
        const int same = g->calls > 0 && g->first[g->depth] == g->node;
        for (int e = lo; e < lo + kids; e++) {
            int below = a2_node_calls[a2_edge_child[e]];
            if (same && a2_edge_child[e] == g->first[g->depth + 1]) below--;
            if (below > 0) ids[n++] = a2_edge_piece[e];
        }
    } else if (g->state == G_AFTER) {
        if (g->calls == 1) ids[n++] = A2_COMMA;
        ids[n++] = A2_CLOSE;
    } else {
        ids[n++] = A2_EOS;
    }
    return n; /* ascending: edges are sorted by piece, and the fixed ids are in order */
}

static void advance(grammar *g, int id) {
    if (g->state == G_START) {
        g->state = id == A2_EMPTY ? G_END : G_CALL;
        g->node = g->depth = 0;
    } else if (g->state == G_CALL) {
        const int lo = a2_node_first[g->node], kids = a2_node_kids[g->node];
        for (int e = lo; e < lo + kids; e++)
            if (a2_edge_piece[e] == id) {
                g->node = a2_edge_child[e];
                break;
            }
        g->depth++;
        if (g->calls == 0) g->first[g->depth] = (uint16_t)g->node;
        if (a2_node_kids[g->node] == 0) { /* a leaf is a complete call */
            g->calls++;
            g->state = G_AFTER;
        }
    } else if (g->state == G_AFTER) {
        if (id == A2_COMMA) {
            g->state = G_CALL;
            g->node = g->depth = 0;
        } else {
            g->state = G_END;
        }
    }
}

int a2_encode(const a2_model *m, const float *frames, int at, int n, int capacity, void *arena) {
    if (at < 0 || n <= 0 || at + n > capacity) return -1;
    buffers b;
    carve(arena, capacity, &b);
    for (int j = 0; j < n; j += A2_TILE) {
        const int count = n - j < A2_TILE ? n - j : A2_TILE;
        encode_tile(m, frames + (size_t)j * A2_LAYERS_IN * A2_DIN, at + j, count, &b);
    }
    return 0;
}

int a2_decode(const a2_model *m, const float *frames, int steps, void *arena, a2_result *out) {
    const int rc = a2_encode(m, frames, 0, steps, steps, arena);
    return rc != 0 ? rc : a2_generate(m, steps, steps, arena, out);
}

int a2_generate(const a2_model *m, int steps, int capacity, void *arena, a2_result *out) {
    if (steps <= 0 || steps > capacity) return -1;
    buffers b;
    carve(arena, capacity, &b);
    grammar g = {G_START, 0, 0, 0, {0}};
    int ids[A2_VOCAB], token = A2_BOS;
    out->n = 0;
    out->min_prob = 1.0f;
    for (int pos = 0; pos < A2_MAX_TOKENS; pos++) {
        step(m, token, pos, steps, &b);
        float top = -INFINITY, sum = 0.0f;
        for (int v = 0; v < A2_VOCAB; v++) top = b.logits[v] > top ? b.logits[v] : top;
        for (int v = 0; v < A2_VOCAB; v++) sum += (b.logits[v] = expf(b.logits[v] - top));
        for (int v = 0; v < A2_VOCAB; v++) b.logits[v] /= sum;
        if (pos == 0) out->p_act = 1.0f - b.logits[A2_EMPTY];
        const int n = allowed(&g, ids);
        int best = ids[0];
        for (int i = 1; i < n; i++)
            if (b.logits[ids[i]] > b.logits[best]) best = ids[i];
        if (b.logits[best] < out->min_prob) out->min_prob = b.logits[best];
        out->ids[out->n++] = best;
        if (best == A2_EOS) break;
        advance(&g, best);
        token = best;
    }
    return 0;
}

void a2_put_layers(float *frames, int at, int steps, const int16_t *layer4, int exp4,
                   const int16_t *layer8, int exp8) {
    const float s4 = ldexpf(1.0f, exp4), s8 = ldexpf(1.0f, exp8);
    for (int t = 0; t < steps; t++) {
        float *row = frames + (size_t)(at + t) * A2_LAYERS_IN * A2_DIN;
        for (int c = 0; c < A2_DIN; c++) {
            row[c] = layer4[t * A2_DIN + c] * s4;
            row[A2_DIN + c] = layer8[t * A2_DIN + c] * s8;
        }
    }
}
