// SPDX-License-Identifier: GPL-3.0-or-later
//
// SD-backed TinyStories port of slvDev/esp32-ai.
// Same llm.h as the flash-mapped sketch; int4 PLE table + token embedding stay
// on the card, core+tied-head staged to int8 in PSRAM.

#include "llm_port.h"

#include "meshcomod_sd_io.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <stdio.h>
#include <string.h>

// Match firmware/esp32_tinystories/esp32_tinystories.ino
#define LLM_INT8_ACT 1
#define LLM_PROFILE 1
#define LLM_PROFILE_NOW() esp_timer_get_time()

static void llm_port_deq_row(const void *tv, int r, float *out);
#define LLM_DEQ_ROW(t, r, out) llm_port_deq_row((const void *)(t), (r), (out))

#include "llm.h"
#include "bpe_tokenizer.h"
#include "vocab.h"
#include "tokenizer_encoder.h"

constexpr const char *kDir = "/esp32ai/tinystories";
constexpr const char *kModelPath = "/esp32ai/tinystories/model.bin";
constexpr uint64_t kModelBytes = 14912348ull;
constexpr uint64_t kTokBytes = 1788896ull;
constexpr int kMaxPromptIds = 128;
// Upstream tinystories sketch uses N_GENERATE = 200.
constexpr int kDefaultGenTokens = 200;

struct SdQtMeta
{
    size_t codes_off;
    size_t scales_off;
    int rows, cols, group, n_groups, row_bytes;
    bool valid;
};

static Model s_model;
static Scratch s_scratch;
static BpeTokenizer s_bpe;
static File s_file;
static SdQtMeta s_tok_meta{};
static SdQtMeta s_ple_meta{};
static bool s_loaded = false;
static size_t s_psram_used = 0;
static size_t s_sram_used = 0;

// Dual-core int8 matvec — same pattern as esp32_tinystories.ino.
static TaskHandle_t s_mv_worker = nullptr;
static TaskHandle_t s_mv_main = nullptr;
static StaticTask_t *s_mv_tcb = nullptr;
static StackType_t *s_mv_stack = nullptr;
static const QT *s_job_t = nullptr;
static const int8_t *s_job_xq = nullptr;
static float s_job_xs = 0.f;
static float *s_job_y = nullptr;
static int s_job_split = 0;

static void setErr(char *err, size_t cap, const char *msg)
{
    if (err && cap) snprintf(err, cap, "%s", msg ? msg : "");
}

static void *psAlloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) s_psram_used += n;
    return p;
}

static void *sramAlloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) s_sram_used += n;
    return p;
}

static void mvWorkerMain(void *)
{
    for (;;)
    {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        matvec_i8_range(s_job_t, s_job_xq, s_job_xs, s_job_y, 0, s_job_split);
        xTaskNotifyGive(s_mv_main);
    }
}

static void matvecPar(const QT *t, const float *x, float *y)
{
    static int8_t xq[LLM_Q8_MAX_INPUT];
    float xs;
    if (t->w8 == nullptr || t->rows < 128)
    {
        MATVEC(t, x, y);
        return;
    }
    quantize_act(x, t->cols, xq, &xs);
    s_job_t = t;
    s_job_xq = xq;
    s_job_xs = xs;
    s_job_y = y;
    s_job_split = t->rows / 2;
    xTaskNotifyGive(s_mv_worker);
    matvec_i8_range(t, xq, xs, y, s_job_split, t->rows);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

static void stopMatvecWorker(void)
{
    if (s_mv_worker)
    {
        vTaskDelete(s_mv_worker);
        s_mv_worker = nullptr;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_mv_stack)
    {
        heap_caps_free(s_mv_stack);
        s_mv_stack = nullptr;
    }
    if (s_mv_tcb)
    {
        heap_caps_free(s_mv_tcb);
        s_mv_tcb = nullptr;
    }
    s_mv_main = nullptr;
    s_model.layer_matvec = nullptr;
    s_model.head_matvec = nullptr;
}

static bool startMatvecWorker(void)
{
    if (s_mv_worker) return true;
    s_mv_tcb = (StaticTask_t *)heap_caps_malloc(sizeof(StaticTask_t),
                                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_mv_stack = (StackType_t *)heap_caps_malloc(4096, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!s_mv_tcb || !s_mv_stack)
    {
        stopMatvecWorker();
        return false;
    }
    // Upstream pins the worker to core 0; our generate task is also on core 0,
    // so put the helper on core 1 so matvec_par actually splits across cores.
    s_mv_worker = xTaskCreateStaticPinnedToCore(mvWorkerMain, "esp32ai-mv", 4096 / sizeof(StackType_t),
                                                nullptr, 2, s_mv_stack, s_mv_tcb, 1);
    if (!s_mv_worker)
    {
        stopMatvecWorker();
        return false;
    }
    s_model.layer_matvec = matvecPar;
    s_model.head_matvec = matvecPar;
    return true;
}

static bool readExact(File &f, void *dst, size_t n)
{
    uint8_t *p = (uint8_t *)dst;
    size_t got = 0;
    while (got < n)
    {
        const int r = f.read(p + got, n - got);
        if (r <= 0) return false;
        got += (size_t)r;
    }
    return true;
}

static bool skipBytes(File &f, size_t n)
{
    // Prefer seek; fall back to draining if needed.
    const size_t cur = f.position();
    if (f.seek(cur + n)) return true;
    uint8_t buf[256];
    while (n)
    {
        const size_t chunk = n > sizeof buf ? sizeof buf : n;
        if (!readExact(f, buf, chunk)) return false;
        n -= chunk;
    }
    return true;
}

static bool fileSizeOk(const char *path, uint64_t expect)
{
    File f = SD.open(path, FILE_READ);
    if (!f || f.isDirectory())
    {
        if (f) f.close();
        return false;
    }
    const uint64_t n = (uint64_t)f.size();
    f.close();
    return n == expect;
}

// Bind one QT from the current file position; optionally stage to int8 and/or
// record SD offsets for later deq_row. When stage=true, int4 is read into a
// temporary PSRAM buffer, staged, then freed.
static bool bindQt(File &f, QT *t, int rows, int cols, bool stage, SdQtMeta *meta)
{
    int32_t group = 0;
    if (!readExact(f, &group, 4)) return false;
    t->rows = rows;
    t->cols = cols;
    t->group = group;
    t->n_groups = (cols + group - 1) / group;
    t->row_bytes = (cols + 1) / 2;
    t->w8 = nullptr;
    t->scale8 = nullptr;

    const size_t codes_bytes = (size_t)rows * (size_t)t->row_bytes;
    const size_t scales_bytes = (size_t)rows * (size_t)t->n_groups * 2;
    const size_t codes_off = f.position();

    if (meta)
    {
        meta->codes_off = codes_off;
        meta->scales_off = codes_off + codes_bytes;
        meta->rows = rows;
        meta->cols = cols;
        meta->group = group;
        meta->n_groups = t->n_groups;
        meta->row_bytes = t->row_bytes;
        meta->valid = true;
    }

    if (!stage)
    {
        t->codes = nullptr;
        t->scales = nullptr;
        return skipBytes(f, codes_bytes + scales_bytes);
    }

    const size_t total = codes_bytes + scales_bytes;
    uint8_t *buf = (uint8_t *)psAlloc(total);
    if (!buf) return false;
    if (!readExact(f, buf, total))
    {
        heap_caps_free(buf);
        return false;
    }
    t->codes = buf;
    t->scales = (const uint16_t *)(buf + codes_bytes);

    void *staged = psAlloc(llm_stage_int8_bytes(t));
    if (!staged)
    {
        heap_caps_free(buf);
        t->codes = nullptr;
        t->scales = nullptr;
        return false;
    }
    llm_stage_int8(t, staged);
    heap_caps_free(buf);
    // After staging, int4 pointers are unused; clear so we never touch freed mem.
    t->codes = nullptr;
    t->scales = nullptr;
    return true;
}

static bool bindF(File &f, const float **out, int n, bool to_sram)
{
    const size_t bytes = (size_t)n * sizeof(float);
    void *dst = to_sram ? sramAlloc(bytes) : psAlloc(bytes);
    if (!dst) return false;
    if (!readExact(f, dst, bytes))
    {
        heap_caps_free(dst);
        return false;
    }
    *out = (const float *)dst;
    return true;
}

static bool allocScratch(char *err, size_t errCap)
{
    Cfg *c = &s_model.c;
    const int D = c->dim, L = c->n_layers, P = c->ple_dim, F = c->ffn, S = c->seq_len;
    auto fail = [&](const char *what) -> bool {
        setErr(err, errCap, what);
        return false;
    };
    if (!(s_scratch.x = (float *)sramAlloc(D * 4))) return fail("SRAM x");
    if (!(s_scratch.h = (float *)sramAlloc((F > D ? F : D) * 4))) return fail("SRAM h");
    if (!(s_scratch.qkv = (float *)sramAlloc(3 * D * 4))) return fail("SRAM qkv");
    if (!(s_scratch.att = (float *)sramAlloc(D * 4))) return fail("SRAM att");
    if (!(s_scratch.g1 = (float *)sramAlloc(F * 4))) return fail("SRAM g1");
    if (!(s_scratch.g2 = (float *)sramAlloc((P > F ? P : F) * 4))) return fail("SRAM g2");
    if (!(s_scratch.ple = (float *)sramAlloc(L * P * 4))) return fail("SRAM ple");
    if (!(s_scratch.tmpP = (float *)sramAlloc(L * P * 4))) return fail("SRAM tmpP");
    if (!(s_scratch.trow = (float *)sramAlloc(L * P * 4))) return fail("SRAM trow");
    if (!(s_scratch.scores = (float *)sramAlloc(S * 4))) return fail("SRAM scores");
    if (!(s_scratch.logits = (float *)psAlloc((size_t)s_model.out_vocab * 4)))
        return fail("PSRAM logits");
    if (!(s_scratch.kcache = (float *)psAlloc((size_t)L * S * D * 4))) return fail("PSRAM kcache");
    if (!(s_scratch.vcache = (float *)psAlloc((size_t)L * S * D * 4))) return fail("PSRAM vcache");
    return true;
}

static void freeScratch()
{
    auto freep = [](void *p) {
        if (p) heap_caps_free(p);
    };
    freep(s_scratch.x);
    freep(s_scratch.h);
    freep(s_scratch.qkv);
    freep(s_scratch.att);
    freep(s_scratch.g1);
    freep(s_scratch.g2);
    freep(s_scratch.ple);
    freep(s_scratch.tmpP);
    freep(s_scratch.trow);
    freep(s_scratch.scores);
    freep(s_scratch.logits);
    freep(s_scratch.kcache);
    freep(s_scratch.vcache);
    memset(&s_scratch, 0, sizeof s_scratch);
}

static void freeModelWeights()
{
    // Staged int8 buffers and SRAM norms — every non-null pointer we allocated.
    auto freep = [](const void *p) {
        if (p) heap_caps_free((void *)p);
    };
    freep(s_model.ple_model_proj.w8);
    freep(s_model.out_head.w8);
    freep(s_model.ple_proj_norm);
    freep(s_model.out_norm);
    for (int l = 0; l < s_model.c.n_layers; l++)
    {
        freep(s_model.attn_norm[l]);
        freep(s_model.ffn_norm[l]);
        freep(s_model.ple_norm[l]);
        freep(s_model.qkv[l].w8);
        freep(s_model.attn_proj[l].w8);
        freep(s_model.gate[l].w8);
        freep(s_model.up[l].w8);
        freep(s_model.down[l].w8);
        freep(s_model.ple_gate[l].w8);
        freep(s_model.ple_proj[l].w8);
    }
    memset(&s_model, 0, sizeof s_model);
    s_tok_meta = {};
    s_ple_meta = {};
}

static bool loadModel(char *err, size_t errCap)
{
    s_file = SD.open(kModelPath, FILE_READ);
    if (!s_file)
    {
        setErr(err, errCap, "cannot open model.bin");
        return false;
    }
    if ((uint64_t)s_file.size() != kModelBytes)
    {
        setErr(err, errCap, "model.bin size mismatch");
        s_file.close();
        return false;
    }

    uint32_t h[4];
    if (!readExact(s_file, h, 16))
    {
        setErr(err, errCap, "short model header");
        s_file.close();
        return false;
    }
    if (h[0] != LLM_MAGIC)
    {
        setErr(err, errCap, "bad model magic");
        s_file.close();
        return false;
    }
    if (h[1] == 0 || h[1] > LLM_FORMAT_VERSION || h[2] < LLM_HEADER_BYTES ||
        h[2] > LLM_HEADER_MAX || (h[3] & ~(uint32_t)LLM_FLAGS_KNOWN))
    {
        setErr(err, errCap, "unsupported model header");
        s_file.close();
        return false;
    }
    const uint32_t flags = h[3];
    uint32_t vio[2];
    if (!readExact(s_file, vio, 8))
    {
        setErr(err, errCap, "short model header");
        s_file.close();
        return false;
    }
    s_model.c.vocab = (int)vio[0];
    s_model.out_vocab = (int)vio[1];
    int32_t hv[7];
    if (!readExact(s_file, hv, 28) || !readExact(s_file, &s_model.c.rope_theta, 4))
    {
        setErr(err, errCap, "short model header");
        s_file.close();
        return false;
    }
    s_model.c.dim = hv[0];
    s_model.c.n_layers = hv[1];
    s_model.c.n_heads = hv[2];
    s_model.c.ffn = hv[3];
    s_model.c.ple_dim = hv[4];
    s_model.c.seq_len = hv[5];
    s_model.c.group = hv[6];
    if (!s_file.seek(h[2]))
    {
        setErr(err, errCap, "cannot seek past header");
        s_file.close();
        return false;
    }

    if (s_model.c.vocab <= 0 || s_model.out_vocab <= 0 || s_model.c.dim <= 0 ||
        s_model.c.ffn <= 0 || s_model.c.ple_dim <= 0 || s_model.c.seq_len <= 0 ||
        s_model.c.group <= 0 || s_model.c.n_layers <= 0 ||
        s_model.c.n_layers > LLM_MAX_LAYERS || s_model.c.n_heads <= 0 ||
        s_model.c.dim % s_model.c.n_heads != 0 ||
        (s_model.c.dim / s_model.c.n_heads) % 2 != 0 ||
        ((flags & LLM_FLAG_TIED_HEAD) && s_model.out_vocab > s_model.c.vocab) ||
        s_model.c.dim > LLM_Q8_MAX_INPUT || s_model.c.ffn > LLM_Q8_MAX_INPUT ||
        s_model.c.ple_dim > LLM_Q8_MAX_INPUT)
    {
        setErr(err, errCap, "model dims rejected");
        s_file.close();
        return false;
    }

    if (VOCAB_N != s_model.out_vocab)
    {
        setErr(err, errCap, "vocab.h / model mismatch");
        s_file.close();
        return false;
    }

    s_model.head_matvec = nullptr;
    s_model.layer_matvec = nullptr;
    const int D = s_model.c.dim, L = s_model.c.n_layers, P = s_model.c.ple_dim;
    const int F = s_model.c.ffn, V = s_model.c.vocab;

    // tok_emb: keep on SD for deq_row; stage tied head from a one-shot read.
    {
        int32_t group = 0;
        if (!readExact(s_file, &group, 4))
        {
            setErr(err, errCap, "tok_emb group");
            s_file.close();
            return false;
        }
        s_model.tok_emb.rows = V;
        s_model.tok_emb.cols = D;
        s_model.tok_emb.group = group;
        s_model.tok_emb.n_groups = (D + group - 1) / group;
        s_model.tok_emb.row_bytes = (D + 1) / 2;
        s_model.tok_emb.w8 = nullptr;
        s_model.tok_emb.scale8 = nullptr;
        s_model.tok_emb.codes = nullptr;
        s_model.tok_emb.scales = nullptr;

        const size_t codes_bytes = (size_t)V * s_model.tok_emb.row_bytes;
        const size_t scales_bytes = (size_t)V * s_model.tok_emb.n_groups * 2;
        s_tok_meta.codes_off = s_file.position();
        s_tok_meta.scales_off = s_tok_meta.codes_off + codes_bytes;
        s_tok_meta.rows = V;
        s_tok_meta.cols = D;
        s_tok_meta.group = group;
        s_tok_meta.n_groups = s_model.tok_emb.n_groups;
        s_tok_meta.row_bytes = s_model.tok_emb.row_bytes;
        s_tok_meta.valid = true;

        // Stage tied head: first out_vocab rows of the embedding.
        if (flags & LLM_FLAG_TIED_HEAD)
        {
            const size_t head_codes =
                (size_t)s_model.out_vocab * s_model.tok_emb.row_bytes;
            const size_t head_scales =
                (size_t)s_model.out_vocab * s_model.tok_emb.n_groups * 2;
            const size_t head_total = head_codes + head_scales;
            uint8_t *buf = (uint8_t *)psAlloc(head_total);
            if (!buf)
            {
                setErr(err, errCap, "PSRAM tok_emb head");
                s_file.close();
                return false;
            }
            if (!readExact(s_file, buf, head_codes))
            {
                heap_caps_free(buf);
                setErr(err, errCap, "read tok_emb codes");
                s_file.close();
                return false;
            }
            // Skip remaining embedding code rows, then read head scales (first
            // out_vocab scale rows), then skip the rest of the scales.
            const size_t rest_codes = codes_bytes - head_codes;
            if (!skipBytes(s_file, rest_codes))
            {
                heap_caps_free(buf);
                setErr(err, errCap, "skip tok_emb codes");
                s_file.close();
                return false;
            }
            if (!readExact(s_file, buf + head_codes, head_scales))
            {
                heap_caps_free(buf);
                setErr(err, errCap, "read tok_emb scales");
                s_file.close();
                return false;
            }
            const size_t rest_scales = scales_bytes - head_scales;
            if (!skipBytes(s_file, rest_scales))
            {
                heap_caps_free(buf);
                setErr(err, errCap, "skip tok_emb scales");
                s_file.close();
                return false;
            }

            QT head = s_model.tok_emb;
            head.rows = s_model.out_vocab;
            head.codes = buf;
            head.scales = (const uint16_t *)(buf + head_codes);
            head.w8 = nullptr;
            head.scale8 = nullptr;
            void *staged = psAlloc(llm_stage_int8_bytes(&head));
            if (!staged)
            {
                heap_caps_free(buf);
                setErr(err, errCap, "PSRAM staged head");
                s_file.close();
                return false;
            }
            llm_stage_int8(&head, staged);
            heap_caps_free(buf);
            s_model.out_head = head;
            s_model.out_head.codes = nullptr;
            s_model.out_head.scales = nullptr;
        }
        else
        {
            if (!skipBytes(s_file, codes_bytes + scales_bytes))
            {
                setErr(err, errCap, "skip tok_emb");
                s_file.close();
                return false;
            }
        }
    }

    if (!bindQt(s_file, &s_model.ple_model_proj, L * P, D, true, nullptr))
    {
        setErr(err, errCap, "ple_model_proj");
        s_file.close();
        return false;
    }
    if (!bindF(s_file, &s_model.ple_proj_norm, P, true))
    {
        setErr(err, errCap, "ple_proj_norm");
        s_file.close();
        return false;
    }
    if (!bindQt(s_file, &s_model.ple_table, V, L * P, false, &s_ple_meta))
    {
        setErr(err, errCap, "ple_table");
        s_file.close();
        return false;
    }

    for (int i = 0; i < L; i++)
    {
        if (!bindF(s_file, &s_model.attn_norm[i], D, true) ||
            !bindQt(s_file, &s_model.qkv[i], 3 * D, D, true, nullptr) ||
            !bindQt(s_file, &s_model.attn_proj[i], D, D, true, nullptr) ||
            !bindF(s_file, &s_model.ffn_norm[i], D, true) ||
            !bindQt(s_file, &s_model.gate[i], F, D, true, nullptr) ||
            !bindQt(s_file, &s_model.up[i], F, D, true, nullptr) ||
            !bindQt(s_file, &s_model.down[i], D, F, true, nullptr) ||
            !bindQt(s_file, &s_model.ple_gate[i], P, D, true, nullptr) ||
            !bindQt(s_file, &s_model.ple_proj[i], D, P, true, nullptr) ||
            !bindF(s_file, &s_model.ple_norm[i], D, true))
        {
            setErr(err, errCap, "layer tensor");
            s_file.close();
            return false;
        }
    }
    if (!bindF(s_file, &s_model.out_norm, D, true))
    {
        setErr(err, errCap, "out_norm");
        s_file.close();
        return false;
    }

    if (!(flags & LLM_FLAG_TIED_HEAD))
    {
        if (!bindQt(s_file, &s_model.out_head, s_model.out_vocab, D, true, nullptr))
        {
            setErr(err, errCap, "out_head");
            s_file.close();
            return false;
        }
    }

    s_model.image_bytes = (size_t)s_file.position();
    // Keep the file open for deq_row seeks.
    return true;
}


static void llm_port_deq_row(const void *tv, int r, float *out)
{
    const QT *t = (const QT *)tv;
    const SdQtMeta *meta = nullptr;
    if (t == &s_model.tok_emb) meta = &s_tok_meta;
    else if (t == &s_model.ple_table) meta = &s_ple_meta;

    if (!meta || !meta->valid || !s_file)
    {
        if (t->codes) deq_row(t, r, out);
        else memset(out, 0, (size_t)t->cols * sizeof(float));
        return;
    }
    if (r < 0 || r >= meta->rows)
    {
        memset(out, 0, (size_t)meta->cols * sizeof(float));
        return;
    }

    uint8_t codes[512];
    uint16_t scales[16];
    if ((size_t)meta->row_bytes > sizeof codes ||
        (size_t)meta->n_groups > sizeof scales / sizeof scales[0])
    {
        memset(out, 0, (size_t)meta->cols * sizeof(float));
        return;
    }
    meshcomodMarkSdIo();
    if (!s_file.seek(meta->codes_off + (size_t)r * meta->row_bytes) ||
        !readExact(s_file, codes, (size_t)meta->row_bytes) ||
        !s_file.seek(meta->scales_off + (size_t)r * meta->n_groups * 2) ||
        !readExact(s_file, scales, (size_t)meta->n_groups * 2))
    {
        memset(out, 0, (size_t)meta->cols * sizeof(float));
        return;
    }

    QT tmp = *t;
    tmp.rows = 1;
    tmp.cols = meta->cols;
    tmp.group = meta->group;
    tmp.n_groups = meta->n_groups;
    tmp.row_bytes = meta->row_bytes;
    tmp.codes = codes;
    tmp.scales = scales;
    tmp.w8 = nullptr;
    tmp.scale8 = nullptr;
    deq_row(&tmp, 0, out);
}

static void emitTok(int tok, char *out, size_t outCap, size_t *used, Esp32AiTokenFn onToken,
             void *user)
{
    if (tok < 0 || tok >= VOCAB_N) return;
    const unsigned char *bytes = VOCAB_BLOB + VOCAB_OFF[tok];
    const int len = VOCAB_OFF[tok + 1] - VOCAB_OFF[tok];
    if (len <= 0) return;
    if (onToken) onToken((const char *)bytes, len, user);
    if (out && outCap && *used + 1 < outCap)
    {
        const size_t copy = (size_t)len < outCap - 1 - *used ? (size_t)len : outCap - 1 - *used;
        memcpy(out + *used, bytes, copy);
        *used += copy;
        out[*used] = 0;
    }
}
bool esp32aiModelReady(void)
{
    char model[96], tok[96];
    snprintf(model, sizeof model, "%s/model.bin", kDir);
    snprintf(tok, sizeof tok, "%s/tokenizer.json", kDir);
    return fileSizeOk(model, kModelBytes) && fileSizeOk(tok, kTokBytes);
}

bool esp32aiIsLoaded(void) { return s_loaded; }

void esp32aiUnload(void)
{
    stopMatvecWorker();
    if (s_file) s_file.close();
    freeScratch();
    freeModelWeights();
    s_psram_used = 0;
    s_sram_used = 0;
    s_loaded = false;
}

bool esp32aiEnsureLoaded(char *err, size_t errCap)
{
    if (s_loaded) return true;
    if (!esp32aiModelReady())
    {
        setErr(err, errCap, "model files missing or incomplete");
        return false;
    }
    memset(&s_model, 0, sizeof s_model);
    memset(&s_scratch, 0, sizeof s_scratch);
    s_psram_used = 0;
    s_sram_used = 0;
    s_tok_meta = {};
    s_ple_meta = {};

    if (bpe_tokenizer_load(TOKENIZER_ENCODER_ASSET, TOKENIZER_ENCODER_ASSET_SIZE, &s_bpe))
    {
        setErr(err, errCap, "tokenizer encoder load failed");
        return false;
    }

    if (!loadModel(err, errCap))
    {
        esp32aiUnload();
        return false;
    }
    if (!allocScratch(err, errCap))
    {
        esp32aiUnload();
        return false;
    }

    // Same hooks as the tinystories sketch (optional; single-core if spawn fails).
    if (startMatvecWorker())
        Serial.println("[esp32ai] dual-core matvec ready");
    else
        Serial.println("[esp32ai] dual-core worker failed; single-core matvec");

    Serial.printf("[esp32ai] loaded Vin=%d Vout=%d D=%d L=%d H=%d F=%d P=%d  "
                  "sram=%uB psram=%.2fMB\n",
                  s_model.c.vocab, s_model.out_vocab, s_model.c.dim, s_model.c.n_layers,
                  s_model.c.n_heads, s_model.c.ffn, s_model.c.ple_dim,
                  (unsigned)s_sram_used, s_psram_used / 1048576.0);
    s_loaded = true;
    return true;
}

int esp32aiGenerate(const char *prompt, char *out, size_t outCap, int maxTokens,
                    Esp32AiTokenFn onToken, void *user, char *err, size_t errCap)
{
    if (!prompt)
    {
        setErr(err, errCap, "empty prompt");
        return -1;
    }
    if (!esp32aiEnsureLoaded(err, errCap)) return -1;

    uint16_t ids[kMaxPromptIds];
    const int n = bpe_encode_ascii(&s_bpe, prompt, ids, kMaxPromptIds);
    if (n < 0)
    {
        setErr(err, errCap,
               n == BTK_ERR_NOT_ASCII ? "prompt must be ASCII"
               : n == BTK_ERR_TOO_LONG ? "prompt too long"
                                      : "encode failed");
        return -1;
    }
    if (n == 0)
    {
        setErr(err, errCap, "prompt produced no tokens");
        return -1;
    }

    if (maxTokens <= 0) maxTokens = kDefaultGenTokens;
    if (maxTokens > s_model.c.seq_len - n) maxTokens = s_model.c.seq_len - n;
    if (maxTokens < 0) maxTokens = 0;

    size_t used = 0;
    if (out && outCap) out[0] = 0;

    // Upstream tinystories loop: prime prompt (forward only — chat UI already
    // shows the user turn), then argmax → emit → forward, delay(0) every 8.
    s_mv_main = xTaskGetCurrentTaskHandle();
    int pos = 0;
    int tok = 0;
    for (int i = 0; i < n; i++)
    {
        tok = (int)ids[i];
        llm_forward(&s_model, tok, pos++, &s_scratch);
    }

    llm_profile_reset(&s_scratch);
    int decoded = 0;
    for (int step = 0; step < maxTokens && pos < s_model.c.seq_len; step++)
    {
        int best = 0;
        float bv = -1e30f;
        for (int v = 0; v < s_model.out_vocab; v++)
        {
            if (s_scratch.logits[v] > bv)
            {
                bv = s_scratch.logits[v];
                best = v;
            }
        }
        tok = best;
        emitTok(tok, out, outCap, &used, onToken, user);
        decoded++;
        llm_forward(&s_model, tok, pos++, &s_scratch);
        if ((step & 7) == 0) delay(0); // feed the task WDT ~every 8 tokens
    }

    if (s_scratch.profile.calls)
    {
        float calls = (float)s_scratch.profile.calls * 1000.f;
        Serial.printf("[esp32ai] %d tok | profile ms/tok: in %.1f attn %.1f ffn %.1f ple %.1f head %.1f\n",
                      decoded, s_scratch.profile.input_us / calls, s_scratch.profile.attn_us / calls,
                      s_scratch.profile.ffn_us / calls, s_scratch.profile.ple_us / calls,
                      s_scratch.profile.head_us / calls);
    }
    return (int)used;
}
