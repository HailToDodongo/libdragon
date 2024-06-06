#include <libdragon.h>
#include <string.h>

DEFINE_RSP_UCODE(rsp_vec);
DEFINE_RSP_UCODE(rsp_vec_sibling);

int main()
{
    console_init();
    console_set_debug(true);
	debug_init_isviewer();
	debug_init_usblog();

    uint32_t* buff = malloc_uncached(128);

    rspq_init();
    rdpq_init();

    uint32_t ovlMainId = rspq_overlay_register(&rsp_vec);
    uint32_t ovlSiblingId = rspq_overlay_register_sibling(ovlMainId, &rsp_vec_sibling);
    
    printf("Overlay IDs: %ld %ld\n", ovlMainId>>28, ovlSiblingId>>28);
    printf("Value: %08lX %08lX\n", buff[0], buff[1]);

    printf("Run Commands\n");
    rspq_write(ovlMainId, 0, 0x1111);
    rspq_write(ovlMainId, 0, 0x2222);
    rspq_write(ovlMainId, 0, 0xBEEF);
    rspq_write(ovlSiblingId, 0, PhysicalAddr(buff));
    rspq_write(ovlMainId, 0, 0x1234);
    rspq_write(ovlSiblingId, 0, PhysicalAddr(buff));
    rspq_wait();

    printf("Value: %08lX %08lX\n", buff[0], buff[1]);

    return 0;
}
