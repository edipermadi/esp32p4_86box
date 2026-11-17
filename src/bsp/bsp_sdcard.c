/*
 * microSD (TF) slot: SDMMC slot 0, 4-bit, IO_MUX pins CLK43 CMD44 D0..D3 39..42.
 * The slot's IO rail comes from on-chip LDO VO4 at 3.3 V, as in Waveshare's BSP.
 *
 * Deliberately NOT using the Arduino SD_MMC library: its generic P4 variant
 * toggles GPIO45 as a card power switch, which this board does not have.
 * The card is initialised for raw sector access only (no FAT mount), so a USB
 * host can own the filesystem without the firmware writing to it at the same time.
 */
#include "bsp_board.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_ldo_regulator.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "diskio_impl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_sdcard";

static sdmmc_card_t s_card;
static bool s_ready = false;

/* sdmmc_read/write_sectors are multi-command sequences (a write polls the card
 * until it finishes programming) and are not safe to interleave. The USB drive
 * and FatFs (player + ID3 scanner) run in different tasks, so every sector
 * access goes through this lock. */
static SemaphoreHandle_t s_io_lock;
static SemaphoreHandle_t s_fs_lock;          /* recursive, see bsp_sdcard_fs_lock() */
static esp_ldo_channel_handle_t s_ldo = NULL;

esp_err_t bsp_sdcard_init(sdmmc_card_t **ret_card)
{
    if (s_ready) {
        if (ret_card) {
            *ret_card = &s_card;
        }
        return ESP_OK;
    }

    if (!s_io_lock) {
        s_io_lock = xSemaphoreCreateMutex();
        s_fs_lock = xSemaphoreCreateRecursiveMutex();
        ESP_RETURN_ON_FALSE(s_io_lock, ESP_ERR_NO_MEM, TAG, "lock alloc failed");
    }
    if (!s_ldo) {
        esp_ldo_channel_config_t ldo_cfg = {
            .chan_id = BSP_SD_LDO_CHAN,
            .voltage_mv = BSP_SD_LDO_MV,
        };
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_cfg, &s_ldo), TAG, "LDO VO4 acquire failed");
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;      /* 40 MHz */

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;                                /* slot 0 uses fixed IO_MUX pins */

    ESP_RETURN_ON_ERROR(sdmmc_host_init(), TAG, "SDMMC host init failed");
    esp_err_t err = sdmmc_host_init_slot(host.slot, &slot);
    if (err == ESP_OK) {
        err = sdmmc_card_init(&host, &s_card);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no usable card: %s", esp_err_to_name(err));
        /* slot only: the host is shared with ESP-Hosted (Wi-Fi via the C6 on slot 1) */
        sdmmc_host_deinit_slot(host.slot);
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "card %s, %llu MB, %u-byte sectors",
             s_card.cid.name,
             (unsigned long long)s_card.csd.capacity * s_card.csd.sector_size / (1024 * 1024),
             (unsigned)s_card.csd.sector_size);
    if (ret_card) {
        *ret_card = &s_card;
    }
    return ESP_OK;
}

esp_err_t bsp_sdcard_read(void *buf, uint32_t sector, uint32_t count)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    esp_err_t err = sdmmc_read_sectors(&s_card, buf, sector, count);
    xSemaphoreGive(s_io_lock);
    return err;
}

esp_err_t bsp_sdcard_write(const void *buf, uint32_t sector, uint32_t count)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_io_lock, portMAX_DELAY);
    esp_err_t err = sdmmc_write_sectors(&s_card, buf, sector, count);
    xSemaphoreGive(s_io_lock);
    return err;
}

/* -------------------------------------------- FatFs disk driver (locked) */
static DSTATUS dio_init(BYTE pdrv)   { return s_ready ? 0 : STA_NOINIT; }
static DSTATUS dio_status(BYTE pdrv) { return s_ready ? 0 : STA_NOINIT; }

static DRESULT dio_read(BYTE pdrv, BYTE *buf, DWORD sector, UINT count)
{
    esp_err_t err = bsp_sdcard_read(buf, sector, count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FatFs read %lu+%u failed: %s", (unsigned long)sector, count, esp_err_to_name(err));
        return RES_ERROR;
    }
    return RES_OK;
}

static DRESULT dio_write(BYTE pdrv, const BYTE *buf, DWORD sector, UINT count)
{
    return bsp_sdcard_write(buf, sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT dio_ioctl(BYTE pdrv, BYTE cmd, void *buf)
{
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buf = s_card.csd.capacity;
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buf = s_card.csd.sector_size;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buf = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t s_diskio = {
    .init = dio_init,
    .status = dio_status,
    .read = dio_read,
    .write = dio_write,
    .ioctl = dio_ioctl,
};

/* ------------------------------------------------------------ FAT mount */
/* The USB drive (usb_msc.cpp) may be writing the card at the same time, so the
 * firmware must never write through this mount. Reads are safe for the card;
 * at worst a file the PC is still copying reads as garbage until a remount. */
static FATFS *s_fs = NULL;
static BYTE s_pdrv = 0xFF;
static char s_drv[3];

void bsp_sdcard_fs_lock(void)
{
    if (s_fs_lock) {
        xSemaphoreTakeRecursive(s_fs_lock, portMAX_DELAY);
    }
}

void bsp_sdcard_fs_unlock(void)
{
    if (s_fs_lock) {
        xSemaphoreGiveRecursive(s_fs_lock);
    }
}

static esp_err_t mount_locked(void);

esp_err_t bsp_sdcard_mount(void)
{
    ESP_RETURN_ON_ERROR(bsp_sdcard_init(NULL), TAG, "no card");
    bsp_sdcard_fs_lock();
    esp_err_t err = mount_locked();
    bsp_sdcard_fs_unlock();
    return err;
}

static esp_err_t mount_locked(void)
{
    if (s_fs) {                                   /* remount: drop cached FAT/dir data */
        f_mount(NULL, s_drv, 0);
        FRESULT fr = f_mount(s_fs, s_drv, 1);
        ESP_RETURN_ON_FALSE(fr == FR_OK, ESP_FAIL, TAG, "remount failed (%d)", fr);
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(ff_diskio_get_drive(&s_pdrv), TAG, "no free FatFs drive");
    ff_diskio_register(s_pdrv, &s_diskio);
    s_drv[0] = (char)('0' + s_pdrv);
    s_drv[1] = ':';
    s_drv[2] = '\0';

    esp_vfs_fat_conf_t conf = {
        .base_path = BSP_SD_MOUNT_POINT,
        .fat_drive = s_drv,
        .max_files = 4,
    };
    esp_err_t err = esp_vfs_fat_register_cfg(&conf, &s_fs);
    if (err != ESP_OK) {
        ff_diskio_unregister(s_pdrv);
        ESP_LOGE(TAG, "VFS register failed: %s", esp_err_to_name(err));
        return err;
    }
    FRESULT fr = f_mount(s_fs, s_drv, 1);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed (%d) - card must be FAT32 (exFAT is not enabled in this core)", fr);
        esp_vfs_fat_unregister_path(BSP_SD_MOUNT_POINT);
        ff_diskio_unregister(s_pdrv);
        s_fs = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "FAT mounted at %s", BSP_SD_MOUNT_POINT);
    return ESP_OK;
}
