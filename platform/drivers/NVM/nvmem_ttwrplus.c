/*
 * SPDX-FileCopyrightText: Copyright 2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/fs/nvs.h>
#include <zephyr/sys/printk.h>

#include <crc.h>
#include <interfaces/nvmem.h>

#define SETTINGS_ID 1
#define VFO_ID      2
#define RECORD_MAGIC 0x4f525458u
#define RECORD_VERSION 1u

struct record_header
{
    uint32_t magic;
    uint16_t version;
    uint16_t size;
    uint16_t crc;
} __attribute__((packed));

struct settings_record
{
    struct record_header header;
    settings_t data;
} __attribute__((packed));

struct vfo_record
{
    struct record_header header;
    channel_t data;
} __attribute__((packed));

static struct nvs_fs storage;
static bool storage_ready;

static int read_record(uint16_t id, void *data, size_t size)
{
    union
    {
        struct settings_record settings;
        struct vfo_record vfo;
    } record;

    if(!storage_ready)
        return -1;

    const size_t expected = sizeof(struct record_header) + size;
    int ret = nvs_read(&storage, id, &record, sizeof(record));
    if(ret != expected || record.settings.header.magic != RECORD_MAGIC ||
       record.settings.header.version != RECORD_VERSION ||
       record.settings.header.size != size ||
       record.settings.header.crc !=
           crc_ccitt((uint8_t *) &record + sizeof(struct record_header), size))
        return -1;

    memcpy(data, (uint8_t *) &record + sizeof(struct record_header), size);
    return 0;
}

static int write_record(uint16_t id, const void *data, size_t size)
{
    union
    {
        struct settings_record settings;
        struct vfo_record vfo;
    } record;

    if(!storage_ready)
        return -1;

    if(read_record(id, &record, size) == 0 &&
       memcmp(&record, data, size) == 0)
        return 0;

    struct record_header header = {
        .magic = RECORD_MAGIC,
        .version = RECORD_VERSION,
        .size = size,
        .crc = crc_ccitt(data, size),
    };
    memcpy(&record, &header, sizeof(header));
    memcpy((uint8_t *) &record + sizeof(header), data, size);

    const size_t total = sizeof(header) + size;
    return nvs_write(&storage, id, &record, total) == total ? 0 : -1;
}

void nvm_init()
{
    const struct device *flash = FIXED_PARTITION_DEVICE(storage_partition);
    struct flash_pages_info page;

    storage_ready = false;
    if(!device_is_ready(flash))
    {
        printk("NVM: flash device is not ready\n");
        return;
    }

    if(flash_get_page_info_by_offs(flash,
                                   FIXED_PARTITION_OFFSET(storage_partition),
                                   &page) < 0)
    {
        printk("NVM: unable to read flash page layout\n");
        return;
    }

    storage.flash_device = flash;
    storage.offset = FIXED_PARTITION_OFFSET(storage_partition);
    storage.sector_size = page.size;
    storage.sector_count = FIXED_PARTITION_SIZE(storage_partition) / page.size;

    if(storage.offset % page.size != 0 ||
       FIXED_PARTITION_SIZE(storage_partition) % page.size != 0 ||
       storage.sector_count < 2)
    {
        printk("NVM: invalid storage partition geometry\n");
        return;
    }

    storage_ready = nvs_mount(&storage) == 0;
    if(!storage_ready)
        printk("NVM: unable to mount storage partition\n");
}

void nvm_terminate()
{
}

void nvm_readCalibData(void *buf)
{
    (void) buf;
}

void nvm_readHwInfo(hwInfo_t *info)
{
    (void) info;
}

int nvm_readVfoChannelData(channel_t *channel)
{
    return read_record(VFO_ID, channel, sizeof(*channel));
}

int nvm_readSettings(settings_t *settings)
{
    return read_record(SETTINGS_ID, settings, sizeof(*settings));
}

int nvm_writeSettings(const settings_t *settings)
{
    return write_record(SETTINGS_ID, settings, sizeof(*settings));
}

int nvm_writeSettingsAndVfo(const settings_t *settings,
                            const channel_t *vfo)
{
    int settings_ret = nvm_writeSettings(settings);
    int vfo_ret = write_record(VFO_ID, vfo, sizeof(*vfo));
    return settings_ret < 0 || vfo_ret < 0 ? -1 : 0;
}
