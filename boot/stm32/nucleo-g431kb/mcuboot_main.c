#include "bootutil/bootutil.h"
#include "bootutil/image.h"
#include "mcuboot_config/mcuboot_logging.h"

void stm32_do_boot(struct boot_rsp* rsp)
{

}
void mcuboot_main(void)
{
    static struct boot_rsp rsp;

    int rv = boot_go(&rsp);

    if (rv == 0) 
    {
        stm32_do_boot(&rsp);
    }
}