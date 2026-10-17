#include "hub_assets.h"
#include "protocol.h"
#include "ui.h"
#include <stddef.h>
#include <string.h>

extern const uint8_t assets_start[] asm("_binary_ui_assets_bin_start");
extern const uint8_t assets_end[] asm("_binary_ui_assets_bin_end");
static const uint8_t* s_assets[CLIP_ASSET_COUNT];

void hub_assets_init(void)
{
    const uint8_t* p = assets_start;
    while (p + 10 <= assets_end)
    {
        uint32_t n;
        memcpy(&n, p + 2, 4);
        if (n < 5 || n > (size_t)(assets_end - p - 10))
            break;
        const uint8_t* a = p + 6;
        uint16_t w, h;
        memcpy(&w, a + 1, 2);
        memcpy(&h, a + 3, 2);
        if (a[0] < CLIP_ASSET_COUNT && n == 5u + (uint32_t)w * h * 2)
        {
            s_assets[a[0]] = a;
            ui_set_asset(a[0], a + 5, w, h);
        }
        p += n + 10;
    }
}

void hub_notice(uint8_t asset)
{
    if (asset >= CLIP_ASSET_COUNT || !s_assets[asset])
        return;
    const uint8_t* a = s_assets[asset];
    uint16_t w, h;
    memcpy(&w, a + 1, 2);
    memcpy(&h, a + 3, 2);
    ui_toast(a + 5, w, h, 1800);
}
