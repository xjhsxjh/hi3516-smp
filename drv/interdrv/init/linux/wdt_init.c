#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/version.h>
#include <linux/of_platform.h>

#define OSDRV_MODULE_VERSION_STRING "HISI_wtdg @HiMPP"

extern int default_margin;
module_param(default_margin, int, 0);
MODULE_PARM_DESC(default_margin, "Watchdog default_margin in seconds. (0<default_margin<80, default=" __MODULE_STRING(HIDOG_TIMER_MARGIN) ")");

//extern int nowayout;
//module_param(nowayout, int, 0);
//MODULE_PARM_DESC(nowayout, "Watchdog cannot be stopped once started (default=CONFIG_WATCHDOG_NOWAYOUT)");

extern int nodeamon;
module_param(nodeamon, int, 0);
MODULE_PARM_DESC(nodeamon, "By default, a kernel deamon feed watchdog when idle, set 'nodeamon=1' to disable this. (default=0)");

extern volatile void *gpWtdgAllReg;

extern int watchdog_init(void);
extern void  watchdog_exit(void);

static int hi_wdg_probe(struct platform_device *pdev)
{
    struct resource *mem;

    mem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
    gpWtdgAllReg = devm_ioremap_resource(&pdev->dev, mem);
    if (IS_ERR((void*)gpWtdgAllReg))
            return PTR_ERR((void*)gpWtdgAllReg);

    return watchdog_init();
}

static int hi_wdg_remove(struct platform_device *pdev)
{
    watchdog_exit();
    return 0;
}

static const struct of_device_id hi_wdg_match[] = {
    { .compatible = "hisilicon,hi_wdg" },
    {},
};

static struct platform_driver hi_wdg_driver = {
    .probe  = hi_wdg_probe,
    .remove = hi_wdg_remove,
    .driver =  { .name = "hi_wdg",
                .of_match_table = hi_wdg_match,
               },
};

module_platform_driver(hi_wdg_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Digital Media Team ,Hisilicon crop ");
MODULE_DESCRIPTION("Mipi Driver");
MODULE_VERSION("HI_VERSION=" OSDRV_MODULE_VERSION_STRING);
