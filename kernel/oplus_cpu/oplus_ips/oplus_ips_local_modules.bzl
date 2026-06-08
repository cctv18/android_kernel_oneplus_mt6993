load("//build/kernel/kleaf:kernel.bzl", "ddk_headers")
load("//build/kernel/oplus:oplus_modules_define.bzl", "define_oplus_ddk_module", "oplus_ddk_get_target", "oplus_ddk_get_kernel_version")
load("//build/kernel/oplus:oplus_modules_dist.bzl", "ddk_copy_to_dist_dir")

def define_oplus_ips_local_modules():
    target = oplus_ddk_get_target()
    kernel_version = oplus_ddk_get_kernel_version()

    ko_deps = []
    copts = []
    kconfig = "oplus_ips/Kconfig"
    defconfig = "build/defconfig/{}/ips_configs".format(target)

    define_oplus_ddk_module(
        name = "oplus_ips",
        srcs = native.glob([
            "oplus_ips/oplus_pmu.h",
            "oplus_ips/trace_ips.h",
            "oplus_ips/ips_table_chip.h",
            "oplus_ips/ips_private.h",
            "oplus_ips/trace_ips.c",
            "oplus_ips/ips_stats.c",
            "oplus_ips/ips_governor.c",
            "oplus_ips/ips_freqqos.c",
            "oplus_ips/ips_core.c",
            "oplus_ips/ips_memlat.c",
        ]),
        includes = ["."],
        kconfig = kconfig,
        ko_deps = [
		":oplus_pmu",
		"//kernel_device_modules-{}/drivers/gpu/drm/mediatek/mediatek_v2:mediatek-drm".format(kernel_version),
		"//kernel_device_modules-{}/drivers/misc/mediatek/dvfsrc:mtk-dvfsrc-helper".format(kernel_version),
		],
        defconfig = defconfig,
	copts = [
	"-I$(DEVICE_MODULES_PATH)/drivers/gpu/drm/mediatek/mediatek_v2",
	"-I$(DEVICE_MODULES_PATH)/drivers/misc/mediatek/include/mt-plat",
	],
	local_defines = [
		"CONFIG_OPLUS_IPS_FOR_MTK",
		"CONFIG_OPLUS_IPS_FOR_MTK_DDR",
		# Uncomment to enable fault injection testing
		# "CONFIG_OPLUS_IPS_INJECT_TEST",
	]
    )

    define_oplus_ddk_module(
        name = "oplus_pmu",
        srcs = native.glob([
            "oplus_ips/oplus_pmu.h",
            "oplus_ips/oplus_pmu.c",
        ]),
        includes = ["."],
        kconfig = kconfig,
        defconfig = defconfig,
        ko_deps = ko_deps,
	    local_defines = [
		    # Uncomment to enable fault injection testing
		    # "CONFIG_OPLUS_IPS_INJECT_TEST",
	    ],
    )
