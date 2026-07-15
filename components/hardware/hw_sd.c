#include "hw_sd.h"

#include "hw_board.h"
#include "hw_display.h"

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include <string.h>

// static const char *TAG = "hw_sd";

static sdmmc_card_t *s_sd_card;

void hw_sd_try_mount(void)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (board->sd_mounted) {
        return;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = LCD_HOST;
    host.max_freq_khz = SD_SPI_MAX_FREQ_KHZ;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.host_id = LCD_HOST;
    slot_config.gpio_cs = PIN_NUM_SD_CS;
    slot_config.wait_for_miso = 20;

    esp_vfs_fat_mount_config_t mount_config = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount_config.format_if_mount_failed = false;
    mount_config.max_files = 3;

    board->last_sd_err = esp_vfs_fat_sdspi_mount("/sdcard",
                                                   &host,
                                                   &slot_config,
                                                   &mount_config,
                                                   &s_sd_card);
    if (board->last_sd_err == ESP_OK && s_sd_card) {
        board->sd_mounted = true;
        memset(board->sd_name, 0, sizeof(board->sd_name));
        memcpy(board->sd_name,
               s_sd_card->cid.name,
               sizeof(board->sd_name) - 1 < sizeof(s_sd_card->cid.name)
                   ? sizeof(board->sd_name) - 1
                   : sizeof(s_sd_card->cid.name));
        board->sd_mb = (uint32_t)(((uint64_t)s_sd_card->csd.capacity * s_sd_card->csd.sector_size) / (1024 * 1024));
    } else {
        board->sd_mounted = false;
        s_sd_card = NULL;
        snprintf(board->sd_name, sizeof(board->sd_name), "NO CARD");
        board->sd_mb = 0;
    }
}

esp_err_t hw_sd_unmount(void)
{
    hw_board_state_t *board = hw_board_state_mut();
    esp_err_t err = ESP_ERR_NOT_FOUND;

    if (board->sd_mounted && s_sd_card) {
        err = esp_vfs_fat_sdcard_unmount("/sdcard", s_sd_card);
        if (err != ESP_OK) {
            board->last_sd_err = err;
            return err;
        }
    } else {
        board->last_sd_err = err;
        return err;
    }

    board->sd_mounted = false;
    s_sd_card = NULL;
    snprintf(board->sd_name, sizeof(board->sd_name), "NO CARD");
    board->sd_mb = 0;
    board->last_sd_err = ESP_ERR_NOT_FOUND;
    return ESP_OK;
}

void hw_sd_info(char *name, size_t name_size, uint32_t *mb)
{
    hw_board_state_t *board = hw_board_state_mut();
    if (name && name_size > 0) {
        snprintf(name, name_size, "%s", board->sd_name);
    }
    if (mb) {
        *mb = board->sd_mb;
    }
}
