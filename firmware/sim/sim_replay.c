/* Replay host frames (as written by host tooling) into ui.c. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "ui.h"

int sim_replay(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); fread(b, 1, n, f); fclose(f);
    long i = 0; int frames = 0;
    while (i + 10 <= n) {
        uint8_t type = b[i + 1]; uint32_t len; memcpy(&len, b + i + 2, 4);
        uint8_t *p = b + i + 6;
        if (type == CLIP_CARD_PUT) {
            uint32_t id; uint16_t w, h; memcpy(&id, p, 4); memcpy(&w, p + 5, 2); memcpy(&h, p + 7, 2);
            uint8_t rc = p[9]; uint32_t dropped;
            if (!ui_put_card(id, p + 10 + rc * 10, w, h, p + 10, rc, &dropped)) printf("put %u failed\n", id);
        } else if (type == CLIP_LIST_ORDER) {
            uint16_t c; memcpy(&c, p + 1, 2); uint32_t ids[80];
            for (int k = 0; k < c; k++) memcpy(&ids[k], p + 3 + k * 4, 4);
            ui_set_order(p[0], ids, c);
        } else if (type == CLIP_ASSET_PUT) {
            uint16_t w, h; memcpy(&w, p + 1, 2); memcpy(&h, p + 3, 2);
            ui_set_asset(p[0], p + 5, w, h);
        } else if (type == CLIP_TOAST) {
            uint16_t d, w, h; memcpy(&d, p, 2); memcpy(&w, p + 2, 2); memcpy(&h, p + 4, 2);
            ui_toast(p + 6, w, h, d);
        }
        i += 10 + len; frames++;
    }
    free(b);
    return frames;
}
