/* JSON-lines harness that executes the actual firmware storage module. */
#include "hub_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* str(cJSON* q, const char* k)
{
    cJSON* v = cJSON_GetObjectItem(q, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
static size_t num(cJSON* q, const char* k)
{
    cJSON* v = cJSON_GetObjectItem(q, k);
    return cJSON_IsNumber(v) ? (size_t)v->valuedouble : 0;
}
int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    hub_store_t store;
    bool opened = hub_store_open(&store, argv[1]);
    char* line = NULL;
    size_t cap = 0;
    while (getline(&line, &cap, stdin) > 0)
    {
        cJSON *q = cJSON_Parse(line), *a = cJSON_CreateObject();
        const char *op = str(q, "op"), *err = NULL;
        uint32_t id = 0;
        if (!strcmp(op, "begin"))
            err = hub_store_begin(&store, cJSON_GetObjectItem(q, "metadata"), &id);
        else if (!strcmp(op, "chunk"))
        {
            const char* hex = str(q, "hex");
            size_t n = strlen(hex) / 2;
            uint8_t* b = malloc(n ? n : 1);
            for (size_t i = 0; i < n; i++)
            {
                unsigned x = 0;
                sscanf(hex + i * 2, "%2x", &x);
                b[i] = x;
            }
            err = hub_store_chunk(&store, str(q, "event_id"), !strcmp(str(q, "part"), "card"), num(q, "offset"), b, n);
            free(b);
        }
        else if (!strcmp(op, "commit"))
            err = hub_store_commit(&store, str(q, "event_id"), &id);
        else if (!strcmp(op, "abort"))
            hub_store_abort(&store);
        else if (!strcmp(op, "favorite"))
            err = hub_store_favorite(&store, num(q, "id"), cJSON_IsTrue(cJSON_GetObjectItem(q, "favorite")));
        else if (!strcmp(op, "delete"))
            err = hub_store_delete(&store, num(q, "id"));
        else if (!strcmp(op, "clear"))
            err = hub_store_clear(&store);
        else if (!strcmp(op, "pause"))
            err = hub_store_pause(&store, cJSON_IsTrue(cJSON_GetObjectItem(q, "paused")));
        else if (!strcmp(op, "reopen"))
        {
            hub_store_close(&store);
            opened = hub_store_open(&store, argv[1]);
        }
        else if (strcmp(op, "status"))
            err = "unknown";
        cJSON_AddBoolToObject(a, "ok", !err);
        cJSON_AddBoolToObject(a, "ready", opened && store.ready);
        if (err)
            cJSON_AddStringToObject(a, "error", err);
        cJSON_AddNumberToObject(a, "record_id", id);
        cJSON_AddNumberToObject(a, "used_bytes", hub_store_used(&store));
        if (store.index)
            cJSON_AddItemToObject(a, "records", cJSON_Duplicate(hub_store_records(&store), true));
        char* out = cJSON_PrintUnformatted(a);
        puts(out ? out : "{}");
        fflush(stdout);
        free(out);
        cJSON_Delete(a);
        cJSON_Delete(q);
    }
    free(line);
    hub_store_close(&store);
    return 0;
}
