#include <stdint.h>

#include "mcuboot_config/mcuboot_logging.h"
#include "flash_map_backend/flash_map_backend.h"
#include "sysflash/sysflash.h"
#include "bootutil/bootutil.h"

#include "stm32g4xx_hal.h"

/*
NUCLEO-G431KB: STM32G431KB 128kB, single bank, 2kB per sector
28kB: 0x08000000-0x08006FFF  bootloader
04kB: 0x08070000-0x08007FFF  scratch area
48kB: 0x08008000-0x08013FFF  first image
48kB: 0x08014000-0x0801FFFF  second image
*/

#define ARRAY_SIZE(arr) sizeof(arr)/sizeof(arr[0])

#define FLASH_DEVICE_INTERNAL_FLASH 0

#define BOOTLOADER_START_ADDRESS            0x00000000L
#define BOOTLOADER_SIZE                     (28*1024)

#define SCRATCH_OFFSET                      (BOOTLOADER_START_ADDRESS + BOOTLOADER_SIZE)
#define SCRATCH_SIZE                        (4*1024)

#define APPLICATION_PRIMARY_START_ADDRESS   (SCRATCH_OFFSET + SCRATCH_SIZE)
#define APPLICATION_SIZE                    (48*1024)

#define APPLICATION_SECONDARY_START_ADDRESS (APPLICATION_PRIMARY_START_ADDRESS + APPLICATION_SIZE)

static const struct flash_area bootloader = {
    .fa_id = FLASH_AREA_BOOTLOADER,
    .fa_device_id = FLASH_DEVICE_INTERNAL_FLASH,
    .fa_off = BOOTLOADER_START_ADDRESS,
    .fa_size = BOOTLOADER_SIZE,
};

static const struct flash_area scratch_img0 = {
    .fa_id = FLASH_AREA_IMAGE_SCRATCH,
    .fa_device_id = FLASH_DEVICE_INTERNAL_FLASH,
    .fa_off = SCRATCH_OFFSET,
    .fa_size = SCRATCH_SIZE,
};

static const struct flash_area primary_img0 = {
    .fa_id = FLASH_AREA_IMAGE_PRIMARY(0),
    .fa_device_id = FLASH_DEVICE_INTERNAL_FLASH,
    .fa_off = APPLICATION_PRIMARY_START_ADDRESS,
    .fa_size = APPLICATION_SIZE,
};

static const struct flash_area secondary_img0 = {
    .fa_id = FLASH_AREA_IMAGE_SECONDARY(0),
    .fa_device_id = FLASH_DEVICE_INTERNAL_FLASH,
    .fa_off = APPLICATION_SECONDARY_START_ADDRESS,
    .fa_size = APPLICATION_SIZE,
};

static const struct flash_area *s_flash_areas[] = {
    &bootloader,
    &scratch_img0,
    &primary_img0,
    &secondary_img0,
};

static const struct flash_area *prv_lookup_flash_area(uint8_t id) {
    for (size_t i = 0; i < ARRAY_SIZE(s_flash_areas); i++) {
        const struct flash_area *area = s_flash_areas[i];
        if (id == area->fa_id) {
            return area;
        }
    }
    return NULL;
}

int flash_area_open(uint8_t id, const struct flash_area **area_outp)
{
    MCUBOOT_LOG_DBG("%s: ID=%d", __func__, (int)id);
    const struct flash_area *area = prv_lookup_flash_area(id);
    *area_outp = area;
    return area != NULL ? 0 : -1;
}

void flash_area_close(const struct flash_area *area)
{

}

static void port_flash_unlock(void)
{
	if(READ_BIT(FLASH->CR, FLASH_CR_LOCK))
	{
		WRITE_REG(FLASH->KEYR, FLASH_KEY1);
		WRITE_REG(FLASH->KEYR, FLASH_KEY2);
	}
}

static void port_flash_lock(void)
{
	SET_BIT(FLASH->CR, FLASH_CR_LOCK);
}

static void port_flash_cache_disable(void)
{
	reactivate_icache = false;
	reactivate_dcache = false;

	if(READ_BIT(FLASH->ACR, FLASH_ACR_ICEN))
	{
		LL_FLASH_DisableInstCache();
		reactivate_icache = true;
	}

	if(READ_BIT(FLASH->ACR, FLASH_ACR_DCEN))
	{
		LL_FLASH_DisableDataCache();
		reactivate_dcache = true;
	}
}

static void port_flash_cache_restore(void)
{

	if(reactivate_icache)
	{
		LL_FLASH_EnableInstCacheReset();
		LL_FLASH_EnableInstCache();
	}

	if(reactivate_dcache)
	{
		LL_FLASH_EnableDataCacheReset();
		LL_FLASH_EnableDataCache();
	}
}

static void port_flash_operation_wait(void)
{
	while(READ_BIT(FLASH->SR, FLASH_SR_BSY))
	{}
}

static void port_flash_check_errors(void)
{
	if(READ_BIT(FLASH->SR, FLASH_SR_PROGERR))
				SET_BIT(FLASH->SR, FLASH_SR_PROGERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_WRPERR))
			SET_BIT(FLASH->SR, FLASH_SR_WRPERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_PGAERR))
			SET_BIT(FLASH->SR, FLASH_SR_PGAERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_SIZERR))
			SET_BIT(FLASH->SR, FLASH_SR_SIZERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_PGSERR))
			SET_BIT(FLASH->SR, FLASH_SR_PGSERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_MISERR))
			SET_BIT(FLASH->SR, FLASH_SR_MISERR);

	if(READ_BIT(FLASH->SR, FLASH_SR_FASTERR))
			SET_BIT(FLASH->SR, FLASH_SR_FASTERR);
}

static bool port_flash_sector_erase(ulora_fls_sector_t sec)
{
	uint32_t page_index = sector_page[sec];

	port_flash_unlock();
	port_flash_cache_disable();

	port_flash_operation_wait();
	port_flash_check_errors();

	MODIFY_REG(FLASH->CR, FLASH_CR_PNB, ((page_index & 0xFFU) << FLASH_CR_PNB_Pos));
	SET_BIT(FLASH->CR, FLASH_CR_PER);
	SET_BIT(FLASH->CR, FLASH_CR_STRT);

	port_flash_operation_wait();

	CLEAR_BIT(FLASH->CR, (FLASH_CR_PER | FLASH_CR_PNB));

	port_flash_cache_restore();
	port_flash_lock();

	return true;
}

static void port_flash_data_write(ulora_fls_sector_t sec, uint16_t index, uint64_t data)
{
	uint32_t addr = sector_address[sec] + index*sizeof(uint64_t);

	port_flash_unlock();
	port_flash_cache_disable();

	// Check that no Flash main memory operation is ongoing by checking the BSY bit in the Flash status register (FLASH_SR).
	port_flash_operation_wait();

	// Check and clear all error programming flags due to a previous programming. If not, PGSERR is set.
	port_flash_check_errors();

	//  Set the PG bit in the Flash control register (FLASH_CR)
	SET_BIT(FLASH->CR, FLASH_CR_PG);

	// Write a first word in an address aligned with double word
	// Write the second word

	*(__IO uint32_t*)addr = (uint32_t)data;
	/* Barrier to ensure programming is performed in 2 steps, in right order
	    (independently of compiler optimization behavior) */
	__ISB();

	/* Program second word */
	*(__IO uint32_t*)(addr+4U) = (uint32_t)(data >> 32);

	 // Wait until the BSY bit is cleared in the FLASH_SR register
	 port_flash_operation_wait();

	 // Check that EOP flag is set in the FLASH_SR register (meaning that the programming operation has succeed), and clear it by software.
	 // (somente se tem interrupcao)

	 // Clear the PG bit in the FLASH_CR register if there no more programming request anymore.
	 CLEAR_BIT(FLASH->CR, FLASH_CR_PG);

	 port_flash_cache_restore();
	 port_flash_lock();
}

static uint64_t port_flash_data_read(ulora_fls_sector_t sec, uint16_t index)
{
	uint32_t addr = sector_address[sec] + index*sizeof(uint64_t);

	return *((uint64_t *)addr);
}

#if 0
int flash_area_read(const struct flash_area *fa, uint32_t off, void *dst,
                    uint32_t len)
{
    uint64_t internal_data = 0, read_len = 0;
    void *read_ptr;
    if (fa->fa_device_id != FLASH_DEVICE_INTERNAL_FLASH) 
    {
        return -1;
    }

    const uint32_t end_offset = off + len;

    if (end_offset > fa->fa_size) 
    {
        MCUBOOT_LOG_ERR("%s: Out of Bounds (0x%x vs 0x%x)", __func__, end_offset, fa->fa_size);
        return -1;
    }

    read_len = (len < 8) ? sizeof(uint64_t) : len;
    read_ptr = (len < 8) ? (void *)&internal_data : dst;
    
    if (bootloader_flash_read(fa->fa_off + off, read_ptr, read_len, true) != ESP_OK) 
    {
        MCUBOOT_LOG_ERR("%s: Flash read failed", __func__);
        return -1;
    }
    if (len < 4) {
        memcpy(dst, read_ptr, len);
    }
    return 0;
}


int flash_area_read(const struct flash_area *fa, uint32_t off, void *dst,
                    uint32_t len)
{
    uint32_t internal_data = 0, read_len = 0;
    void *read_ptr;
    if (fa->fa_device_id != FLASH_DEVICE_INTERNAL_FLASH) {
        return -1;
    }

    const uint32_t end_offset = off + len;

    if (end_offset > fa->fa_size) {
        MCUBOOT_LOG_ERR("%s: Out of Bounds (0x%x vs 0x%x)", __func__, end_offset, fa->fa_size);
        return -1;
    }
    read_len = (len < 4) ? sizeof(uint32_t) : len;
    read_ptr = (len < 4) ? (void *)&internal_data : dst;
    if (bootloader_flash_read(fa->fa_off + off, read_ptr, read_len, true) != ESP_OK) {
        MCUBOOT_LOG_ERR("%s: Flash read failed", __func__);
        return -1;
    }
    if (len < 4) {
        memcpy(dst, read_ptr, len);
    }
    return 0;
}

int flash_area_write(const struct flash_area *fa, uint32_t off, const void *src,
                     uint32_t len)
{
    uint32_t write_len = len, write_data = 0;
    void *write_ptr = (void *)src;
    if (fa->fa_device_id != FLASH_DEVICE_INTERNAL_FLASH) {
        return -1;
    }

    const uint32_t end_offset = off + len;

    if (end_offset > fa->fa_size) {
        MCUBOOT_LOG_ERR("%s: Out of Bounds (0x%x vs 0x%x)", __func__, end_offset, fa->fa_size);
        return -1;
    }

    const uint32_t start_addr = fa->fa_off + off;
    MCUBOOT_LOG_DBG("%s: Addr: 0x%08x Length: %d", __func__, (int)start_addr, (int)len);
    if (len < 4) {
        flash_area_read(fa, start_addr, &write_data, sizeof(uint32_t));
        memcpy(&write_data, src, len);
        write_ptr = (void *)&write_data;
        write_len = sizeof(uint32_t);
    }

    if (bootloader_flash_write(start_addr, write_ptr, write_len, false) != ESP_OK) {
        MCUBOOT_LOG_ERR("%s: Flash write failed", __func__);
        return -1;
    }
#if VALIDATE_PROGRAM_OP
    if (memcmp((void *)addr, src, len) != 0) {
        MCUBOOT_LOG_ERR("%s: Program Failed", __func__);
        assert(0);
    }
#endif

    return 0;
}

int flash_area_erase(const struct flash_area *fa, uint32_t off, uint32_t len)
{
    if (fa->fa_device_id != FLASH_DEVICE_INTERNAL_FLASH) {
        return -1;
    }

    if ((len % FLASH_SECTOR_SIZE) != 0 || (off % FLASH_SECTOR_SIZE) != 0) {
        MCUBOOT_LOG_ERR("%s: Not aligned on sector Offset: 0x%x Length: 0x%x", __func__,
                    (int)off, (int)len);
        return -1;
    }

    const uint32_t start_addr = fa->fa_off + off;
    MCUBOOT_LOG_DBG("%s: Addr: 0x%08x Length: %d", __func__, (int)start_addr, (int)len);

    if (bootloader_flash_erase_range(start_addr, len) != ESP_OK) {
        MCUBOOT_LOG_ERR("%s: Flash erase failed", __func__);
        return -1;
    }
#if VALIDATE_PROGRAM_OP
    for (size_t i = 0; i < len; i++) {
        uint8_t *val = (void *)(start_addr + i);
        if (*val != 0xff) {
            MCUBOOT_LOG_ERR("%s: Erase at 0x%x Failed", __func__, (int)val);
            assert(0);
        }
    }
#endif

    return 0;
}

size_t flash_area_align(const struct flash_area *area)
{
    return 4;
}

uint8_t flash_area_erased_val(const struct flash_area *area)
{
    return 0xff;
}

int flash_area_get_sectors(int fa_id, uint32_t *count,
                           struct flash_sector *sectors)
{
    const struct flash_area *fa = prv_lookup_flash_area(fa_id);
    if (fa->fa_device_id != FLASH_DEVICE_INTERNAL_FLASH) {
        return -1;
    }

    const size_t sector_size = FLASH_SECTOR_SIZE;
    uint32_t total_count = 0;
    for (size_t off = 0; off < fa->fa_size; off += sector_size) {
        // Note: Offset here is relative to flash area, not device
        sectors[total_count].fs_off = off;
        sectors[total_count].fs_size = sector_size;
        total_count++;
    }

    *count = total_count;
    return 0;
}

int flash_area_id_from_multi_image_slot(int image_index, int slot)
{
    MCUBOOT_LOG_DBG("%s", __func__);
    switch (slot) {
      case 0:
        return FLASH_AREA_IMAGE_PRIMARY(image_index);
      case 1:
        return FLASH_AREA_IMAGE_SECONDARY(image_index);
    }

    MCUBOOT_LOG_ERR("Unexpected Request: image_index=%d, slot=%d", image_index, slot);
    return -1; /* flash_area_open will fail on that */
}

int flash_area_id_from_image_slot(int slot)
{
  return flash_area_id_from_multi_image_slot(0, slot);
}

int flash_area_to_sectors(int idx, int *cnt, struct flash_area *fa)
{
	return -1;
}

void mcuboot_assert_handler(const char *file, int line, const char *func)
{
    ets_printf("assertion failed: file \"%s\", line %d, func: %s\n", file, line, func);
    abort();
}

#endif