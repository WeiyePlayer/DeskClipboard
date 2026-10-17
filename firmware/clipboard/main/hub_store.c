#include "hub_store.h"
#include <dirent.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef ESP_PLATFORM
#include "mbedtls/sha256.h"
typedef mbedtls_sha256_context sha_ctx;
static void sha_init(sha_ctx* c)
{
    mbedtls_sha256_init(c);
    mbedtls_sha256_starts(c, 0);
}
static void sha_add(sha_ctx* c, const void* p, size_t n) { mbedtls_sha256_update(c, p, n); }
static void sha_end(sha_ctx* c, uint8_t* p)
{
    mbedtls_sha256_finish(c, p);
    mbedtls_sha256_free(c);
}
#else
#include <CommonCrypto/CommonDigest.h>
typedef CC_SHA256_CTX sha_ctx;
static void sha_init(sha_ctx* c) { CC_SHA256_Init(c); }
static void sha_add(sha_ctx* c, const void* p, size_t n) { CC_SHA256_Update(c, p, (CC_LONG)n); }
static void sha_end(sha_ctx* c, uint8_t* p) { CC_SHA256_Final(p, c); }
#endif

#define INDEX_LIMIT (512 * 1024)
#define INDEX_RESERVE (1024 * 1024)
static const char* str(const cJSON* o, const char* key)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static size_t num(const cJSON* o, const char* key)
{
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) && isfinite(v->valuedouble) && v->valuedouble >= 0 && v->valuedouble <= UINT32_MAX && floor(v->valuedouble) == v->valuedouble ? (size_t)v->valuedouble : 0;
}
static void set_num(cJSON* o, const char* k, double n)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddNumberToObject(o, k, n);
}
static void set_bool(cJSON* o, const char* k, bool b)
{
    cJSON_DeleteItemFromObjectCaseSensitive(o, k);
    cJSON_AddBoolToObject(o, k, b);
}
static void path(hub_store_t* s, const char* name, char out[192]) { snprintf(out, 192, "%s/%s", s->root, name); }
static void hex_digest(const uint8_t in[32], char out[65])
{
    for (int i = 0; i < 32; i++)
        sprintf(out + i * 2, "%02x", in[i]);
    out[64] = 0;
}
void hub_sha256(const uint8_t* data, size_t size, char out[65])
{
    sha_ctx c;
    uint8_t d[32];
    sha_init(&c);
    sha_add(&c, data, size);
    sha_end(&c, d);
    hex_digest(d, out);
}
static bool file_sha(const char* p, char out[65])
{
    FILE* f = fopen(p, "rb");
    if (!f)
        return false;
    sha_ctx c;
    sha_init(&c);
    uint8_t buf[1024], digest[32];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)))
        sha_add(&c, buf, n);
    bool ok = !ferror(f);
    fclose(f);
    sha_end(&c, digest);
    hex_digest(digest, out);
    return ok;
}
static uint32_t crc32(const uint8_t* p, size_t n)
{
    uint32_t c = ~0u;
    while (n--)
    {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = (c >> 1) ^ ((0u - (c & 1)) & 0xedb88320u);
    }
    return ~c;
}
bool hub_valid_id(const char* p)
{
    if (!p || strlen(p) != 36)
        return false;
    for (int i = 0; i < 36; i++)
    {
        if (i == 8 || i == 13 || i == 18 || i == 23)
        {
            if (p[i] != '-')
                return false;
        }
        else if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return false;
    }
    return true;
}
static bool valid_hash(const char* p)
{
    if (strlen(p) != 64)
        return false;
    for (int i = 0; i < 64; i++)
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'a' && p[i] <= 'f')))
            return false;
    return true;
}
static bool valid_utf8(const uint8_t* p, size_t n)
{
    for (size_t i = 0; i < n;)
    {
        uint32_t v = p[i++];
        unsigned more = 0;
        uint32_t min = 0;
        if (v < 0x80)
            continue;
        if (v >= 0xc2 && v <= 0xdf)
        {
            more = 1;
            min = 0x80;
            v &= 31;
        }
        else if (v >= 0xe0 && v <= 0xef)
        {
            more = 2;
            min = 0x800;
            v &= 15;
        }
        else if (v >= 0xf0 && v <= 0xf4)
        {
            more = 3;
            min = 0x10000;
            v &= 7;
        }
        else
            return false;
        if (i + more > n)
            return false;
        while (more--)
        {
            if ((p[i] & 0xc0) != 0x80)
                return false;
            v = (v << 6) | (p[i++] & 63);
        }
        if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
            return false;
    }
    return true;
}
static bool flush_close(FILE* f)
{
    bool ok = fflush(f) == 0;
    if (ok)
        ok = fsync(fileno(f)) == 0;
    if (fclose(f) != 0)
        ok = false;
    return ok;
}
static cJSON* empty_index(void)
{
    cJSON* o = cJSON_CreateObject();
    if (!o)
        return NULL;
    cJSON_AddNumberToObject(o, "version", 4);
    cJSON_AddNumberToObject(o, "next_id", 1);
    cJSON_AddNumberToObject(o, "clock", 0);
    cJSON_AddBoolToObject(o, "paused", false);
    cJSON_AddArrayToObject(o, "records");
    cJSON_AddArrayToObject(o, "receipts");
    return o;
}
static cJSON* read_index(hub_store_t* s, int slot, uint32_t* generation)
{
    char name[20], p[192];
    snprintf(name, sizeof(name), "INDEX%d.BIN", slot);
    path(s, name, p);
    FILE* f = fopen(p, "rb");
    if (!f)
        return NULL;
    uint32_t h[4];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || h[0] != 0x34425548 || h[2] > INDEX_LIMIT || h[2] == 0)
    {
        fclose(f);
        return NULL;
    }
    char* b = calloc(h[2] + 1, 1);
    if (!b)
    {
        fclose(f);
        return NULL;
    }
    bool ok = fread(b, 1, h[2], f) == h[2] && fgetc(f) == EOF && crc32((uint8_t*)b, h[2]) == h[3];
    fclose(f);
    cJSON* o = ok ? cJSON_ParseWithLength(b, h[2]) : NULL;
    free(b);
    if (!o || num(o, "version") != 4 || !cJSON_IsArray(cJSON_GetObjectItem(o, "records")) || !cJSON_IsArray(cJSON_GetObjectItem(o, "receipts")) || cJSON_GetArraySize(cJSON_GetObjectItem(o, "records")) > HUB_MAX_RECORDS)
    {
        cJSON_Delete(o);
        return NULL;
    }
    *generation = h[1];
    return o;
}
static bool save(hub_store_t* s)
{
    char* b = cJSON_PrintUnformatted(s->index);
    if (!b)
        return false;
    size_t n = strlen(b);
    if (n > INDEX_LIMIT)
    {
        free(b);
        return false;
    }
    uint32_t g = s->generation + 1, h[4] = { 0x34425548, g, n, crc32((uint8_t*)b, n) };
    char name[20], p[192];
    snprintf(name, sizeof(name), "INDEX%u.BIN", (unsigned)(g & 1));
    path(s, name, p);
    FILE* f = fopen(p, "wb");
    if (!f)
    {
        free(b);
        return false;
    }
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) && fwrite(b, 1, n, f) == n;
    if (!flush_close(f))
        ok = false;
    free(b);
    if (ok)
        s->generation = g;
    return ok;
}
static bool reload(hub_store_t* s)
{
    uint32_t a = 0, b = 0;
    cJSON *x = read_index(s, 0, &a), *y = read_index(s, 1, &b);
    if (!x && !y)
        return false;
    cJSON_Delete(s->index);
    if (x && (!y || a >= b))
    {
        s->index = x;
        s->generation = a;
        cJSON_Delete(y);
    }
    else
    {
        s->index = y;
        s->generation = b;
        cJSON_Delete(x);
    }
    return true;
}
cJSON* hub_store_records(hub_store_t* s) { return cJSON_GetObjectItemCaseSensitive(s->index, "records"); }
cJSON* hub_store_find(hub_store_t* s, uint32_t id)
{
    cJSON* v;
    cJSON_ArrayForEach(v, hub_store_records(s)) if (num(v, "id") == id) return v;
    return NULL;
}
bool hub_store_path(hub_store_t* s, uint32_t id, bool card, char* out, size_t size)
{
    if (!id || !s->ready)
        return false;
    int n = snprintf(out, size, "%s/%08lX.%s", s->root, (unsigned long)id, card ? "RGB" : "DAT");
    return n > 0 && (size_t)n < size;
}
size_t hub_store_used(hub_store_t* s)
{
    size_t n = 0;
    DIR* d = opendir(s->root);
    if (!d)
        return HUB_MAX_BYTES;
    struct dirent* e;
    char p[192];
    struct stat st;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        path(s, e->d_name, p);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            n += st.st_size;
    }
    closedir(d);
    return n;
}
static void cleanup(hub_store_t* s)
{
    DIR* d = opendir(s->root);
    if (!d)
        return;
    struct dirent* e;
    char p[192];
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "INDEX0.BIN") || !strcmp(e->d_name, "INDEX1.BIN"))
            continue;
        bool keep = s->upload && (!strcmp(e->d_name, "UPLOAD.DAT") || !strcmp(e->d_name, "UPLOAD.RGB"));
        cJSON* r;
        cJSON_ArrayForEach(r, hub_store_records(s))
        {
            char q[192];
            hub_store_path(s, num(r, "id"), false, q, sizeof(q));
            const char* base = strrchr(q, '/');
            if (base && !strcmp(base + 1, e->d_name))
                keep = true;
            hub_store_path(s, num(r, "id"), true, q, sizeof(q));
            base = strrchr(q, '/');
            if (base && !strcmp(base + 1, e->d_name))
                keep = true;
        }
        if (!keep)
        {
            path(s, e->d_name, p);
            unlink(p);
        }
    }
    closedir(d);
}
bool hub_store_open(hub_store_t* s, const char* root)
{
    memset(s, 0, sizeof(*s));
    if (strlen(root) >= sizeof(s->root))
        return false;
    strcpy(s->root, root);
    DIR* d = opendir(root);
    if (!d)
        return false;
    closedir(d);
    if (!reload(s))
    {
        char p[192];
        struct stat st;
        path(s, "INDEX0.BIN", p);
        bool had = stat(p, &st) == 0;
        path(s, "INDEX1.BIN", p);
        had |= stat(p, &st) == 0;
        if (had)
            return false; /* Corruption must not become a silently empty history. */
        s->index = empty_index();
        if (!s->index || !save(s))
        {
            hub_store_close(s);
            return false;
        }
    }
    s->ready = true;
    bool changed = false;
    cJSON* rows = hub_store_records(s);
    for (int i = cJSON_GetArraySize(rows) - 1; i >= 0; i--)
    {
        cJSON* r = cJSON_GetArrayItem(rows, i);
        char p[192], digest[65];
        struct stat st;
        bool ok = num(r, "id") > 0 && !strcmp(str(r, "kind"), "text");
        for (int part = 0; part < 2 && ok; part++)
        {
            hub_store_path(s, num(r, "id"), part, p, sizeof(p));
            ok = stat(p, &st) == 0 && (size_t)st.st_size == num(r, part ? "card_bytes" : "bytes") && file_sha(p, digest) && !strcmp(digest, str(r, part ? "card_sha256" : "sha256"));
        }
        if (!ok)
        {
            cJSON_DeleteItemFromArray(rows, i);
            changed = true;
        }
    }
    if (changed && !save(s))
    {
        hub_store_close(s);
        return false;
    }
    cleanup(s);
    return true;
}
void hub_store_abort(hub_store_t* s)
{
    char p[192];
    path(s, "UPLOAD.DAT", p);
    unlink(p);
    path(s, "UPLOAD.RGB", p);
    unlink(p);
    cJSON_Delete(s->upload);
    s->upload = NULL;
    s->content_offset = s->card_offset = 0;
}
void hub_store_close(hub_store_t* s)
{
    cJSON_Delete(s->index);
    cJSON_Delete(s->upload);
    s->index = s->upload = NULL;
    s->ready = false;
}
static const char* persist(hub_store_t* s)
{
    if (save(s))
        return NULL;
    if (!reload(s))
        s->ready = false;
    return "storage_error";
}
const char* hub_store_delete(hub_store_t* s, uint32_t id)
{
    if (!s->ready)
        return "storage_error";
    cJSON* rows = hub_store_records(s);
    for (int i = 0; i < cJSON_GetArraySize(rows); i++)
        if (num(cJSON_GetArrayItem(rows, i), "id") == id)
        {
            cJSON_DeleteItemFromArray(rows, i);
            const char* err = persist(s);
            if (!err)
                cleanup(s);
            return err;
        }
    return "not_found";
}
const char* hub_store_clear(hub_store_t* s)
{
    if (!s->ready)
        return "storage_error";
    cJSON* rows = hub_store_records(s);
    for (int i = cJSON_GetArraySize(rows) - 1; i >= 0; i--)
        if (!cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(rows, i), "favorite")))
            cJSON_DeleteItemFromArray(rows, i);
    const char* e = persist(s);
    if (!e)
        cleanup(s);
    return e;
}
const char* hub_store_favorite(hub_store_t* s, uint32_t id, bool favorite)
{
    cJSON* r = hub_store_find(s, id);
    if (!r)
        return "not_found";
    set_bool(r, "favorite", favorite);
    return persist(s);
}
const char* hub_store_pause(hub_store_t* s, bool paused)
{
    if (!s->ready)
        return "storage_error";
    set_bool(s->index, "paused", paused);
    return persist(s);
}
static uint32_t oldest(hub_store_t* s)
{
    uint32_t id = 0;
    size_t order = SIZE_MAX;
    cJSON* r;
    cJSON_ArrayForEach(r, hub_store_records(s)) if (!cJSON_IsTrue(cJSON_GetObjectItem(r, "favorite")) && num(r, "order") < order)
    {
        id = num(r, "id");
        order = num(r, "order");
    }
    return id;
}
static void receipt(hub_store_t* s, const char* event, uint32_t id)
{
    cJSON *a = cJSON_GetObjectItem(s->index, "receipts"), *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event_id", event);
    cJSON_AddNumberToObject(o, "id", id);
    cJSON_AddItemToArray(a, o);
    while (cJSON_GetArraySize(a) > 128)
        cJSON_DeleteItemFromArray(a, 0);
}
const char* hub_store_begin(hub_store_t* s, const cJSON* meta, uint32_t* existing)
{
    *existing = 0;
    if (!s->ready)
        return "storage_error";
    if (cJSON_IsTrue(cJSON_GetObjectItem(s->index, "paused")))
        return "paused";
    const char *event = str(meta, "event_id"), *kind = str(meta, "kind");
    size_t bytes = num(meta, "bytes"), card = num(meta, "card_bytes");
    if (!hub_valid_id(event) || !hub_valid_id(str(meta, "source_device")) || strlen(str(meta, "source_name")) > 64 || !valid_hash(str(meta, "sha256")) || !valid_hash(str(meta, "card_sha256")))
        return "invalid_metadata";
    if (strcmp(kind, "text"))
        return "invalid_kind";
    if (!bytes || bytes > HUB_MAX_TEXT)
        return "too_large";
    if (card < 12 || card > HUB_MAX_CARD || num(meta, "width") > 65535 || num(meta, "height") > 65535)
        return "invalid_card";
    cJSON* chips = cJSON_GetObjectItem(meta, "chips");
    if (!cJSON_IsArray(chips) || cJSON_GetArraySize(chips) > 3)
        return "invalid_chips";
    cJSON* v;
    cJSON_ArrayForEach(v, chips) if (!cJSON_IsString(v) || strlen(v->valuestring) > 2048) return "invalid_chips";
    cJSON_ArrayForEach(v, cJSON_GetObjectItem(s->index, "receipts")) if (!strcmp(event, str(v, "event_id")))
    {
        *existing = num(v, "id");
        return hub_store_find(s, *existing) ? NULL : "already_evicted";
    }
    cJSON_ArrayForEach(v, hub_store_records(s)) if (!strcmp(kind, str(v, "kind")) && !strcmp(str(meta, "sha256"), str(v, "sha256")))
    {
        *existing = num(v, "id");
        double order = num(s->index, "clock") + 1;
        set_num(s->index, "clock", order);
        set_num(v, "order", order);
        receipt(s, event, *existing);
        return persist(s);
    }
    if (s->upload)
        return !strcmp(event, str(s->upload, "event_id")) && !strcmp(str(meta, "sha256"), str(s->upload, "sha256")) && !strcmp(str(meta, "source_device"), str(s->upload, "source_device")) ? NULL : "busy";
    while (cJSON_GetArraySize(hub_store_records(s)) >= HUB_MAX_RECORDS || hub_store_used(s) + bytes + card + INDEX_RESERVE > HUB_MAX_BYTES)
    {
        uint32_t id = oldest(s);
        if (!id)
            return "storage_full";
        const char* e = hub_store_delete(s, id);
        if (e)
            return e;
    }
    s->upload = cJSON_CreateObject();
    if (!s->upload)
        return "out_of_memory";
    const char* keys[] = { "event_id", "source_device", "source_name", "kind", "bytes", "sha256", "card_bytes", "card_sha256", "width", "height", "chips", "favorite" };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
    {
        const cJSON* value = cJSON_GetObjectItemCaseSensitive(meta, keys[i]);
        if (value)
            cJSON_AddItemToObject(s->upload, keys[i], cJSON_Duplicate(value, true));
    }
    char p[192];
    for (int i = 0; i < 2; i++)
    {
        path(s, i ? "UPLOAD.RGB" : "UPLOAD.DAT", p);
        FILE* f = fopen(p, "wb");
        if (!f || !flush_close(f))
        {
            hub_store_abort(s);
            return "storage_error";
        }
    }
    s->content_offset = s->card_offset = 0;
    return NULL;
}
const char* hub_store_chunk(hub_store_t* s, const char* event, bool card, size_t offset, const uint8_t* data, size_t length)
{
    if (!s->upload || strcmp(event, str(s->upload, "event_id")))
        return "no_upload";
    size_t* current = card ? &s->card_offset : &s->content_offset;
    if (length > 32768 || offset != *current || length > num(s->upload, card ? "card_bytes" : "bytes") - *current)
        return "invalid_offset";
    char p[192];
    path(s, card ? "UPLOAD.RGB" : "UPLOAD.DAT", p);
    FILE* f = fopen(p, "ab");
    if (!f)
        return "storage_error";
    bool ok = fwrite(data, 1, length, f) == length;
    if (!flush_close(f))
        ok = false;
    if (!ok)
    {
        hub_store_abort(s);
        return "storage_error";
    }
    *current += length;
    return NULL;
}
static bool valid_card(hub_store_t* s)
{
    char p[192];
    path(s, "UPLOAD.RGB", p);
    FILE* f = fopen(p, "rb");
    if (!f)
        return false;
    uint8_t h[10] = { 0 };
    bool ok = fread(h, 1, 10, f) == 10;
    unsigned w = h[5] | h[6] << 8, height = h[7] | h[8] << 8, count = h[9];
    ok = ok && w > 0 && w <= 224 && height > 0 && height <= 256 && count >= 2 && count <= 8 && 10 + count * 10 + w * height * 2 == s->card_offset;
    for (unsigned i = 0; i < count && ok; i++)
    {
        uint8_t r[10] = { 0 };
        ok = fread(r, 1, 10, f) == 10;
        unsigned x = r[2] | r[3] << 8, y = r[4] | r[5] << 8, rw = r[6] | r[7] << 8, rh = r[8] | r[9] << 8;
        ok = ok && r[0] <= 2 && rw > 0 && rh > 0 && x + rw <= w && y + rh <= height && (r[0] != 1 || r[1] < cJSON_GetArraySize(cJSON_GetObjectItem(s->upload, "chips")));
    }
    fclose(f);
    return ok;
}
const char* hub_store_commit(hub_store_t* s, const char* event, uint32_t* id)
{
    *id = 0;
    if (!s->upload || strcmp(event, str(s->upload, "event_id")))
        return "no_upload";
    if (s->content_offset != num(s->upload, "bytes") || s->card_offset != num(s->upload, "card_bytes"))
        return "incomplete";
    char p[192], digest[65];
    for (int i = 0; i < 2; i++)
    {
        path(s, i ? "UPLOAD.RGB" : "UPLOAD.DAT", p);
        if (!file_sha(p, digest) || strcmp(digest, str(s->upload, i ? "card_sha256" : "sha256")))
        {
            hub_store_abort(s);
            return "checksum_mismatch";
        }
    }
    if (!valid_card(s))
    {
        hub_store_abort(s);
        return "invalid_card";
    }
    path(s, "UPLOAD.DAT", p);
    FILE* f = fopen(p, "rb");
    if (!f)
        return "storage_error";
    uint8_t* b = malloc(s->content_offset);
    if (!b)
    {
        fclose(f);
        return "out_of_memory";
    }
    bool valid = fread(b, 1, s->content_offset, f) == s->content_offset && valid_utf8(b, s->content_offset);
    free(b);
    fclose(f);
    if (!valid)
    {
        hub_store_abort(s);
        return "invalid_content";
    }
    uint32_t next = num(s->index, "next_id");
    if (!next || next == UINT32_MAX)
        return "id_exhausted";
    for (int i = 0; i < 2; i++)
    {
        char dst[192];
        path(s, i ? "UPLOAD.RGB" : "UPLOAD.DAT", p);
        hub_store_path(s, next, i, dst, sizeof(dst));
        if (rename(p, dst) != 0)
        {
            hub_store_abort(s);
            cleanup(s);
            return "storage_error";
        }
    }
    cJSON* r = s->upload;
    s->upload = NULL;
    set_num(r, "id", next);
    double order = num(s->index, "clock") + 1;
    set_num(r, "order", order);
    set_num(s->index, "clock", order);
    set_num(s->index, "next_id", next + 1);
    cJSON_AddItemToArray(hub_store_records(s), r);
    receipt(s, event, next);
    const char* err = persist(s);
    if (!err)
        *id = next;
    else
        cleanup(s);
    return err;
}
