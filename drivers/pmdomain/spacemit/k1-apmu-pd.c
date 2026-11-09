// SPDX-License-Identifier: GPL-2.0-only
/*
 * SpacemiT K1 APMU power domains (software controlled domains only).
 *
 * Each domain has a control register in the APMU with a power switch
 * pair (SLEEP1/SLEEP2) and an isolation release bit, plus a status bit
 * in the APMU power status register.
 */

#include <linux/auxiliary_bus.h>
#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/pm_domain.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <soc/spacemit/ccu.h>
#include <dt-bindings/power/spacemit,k1-power.h>

#define K1_APMU_PWR_STATUS	0xf0
#define K1_PD_ISO_BIT		BIT(1)
#define K1_PD_SLEEP1_BIT	BIT(2)
#define K1_PD_SLEEP2_BIT	BIT(3)
#define K1_PD_POLL_US		10000

struct k1_pd_desc {
	const char *name;
	u32 ctrl;
	u32 stat_bit;
};

static const struct k1_pd_desc k1_pd_descs[] = {
	[K1_PD_GPU] = { "gpu", 0xd0, 0 },
};

struct k1_pd {
	struct generic_pm_domain genpd;
	struct regmap *regmap;
	const struct k1_pd_desc *desc;
};

#define to_k1_pd(gpd) container_of(gpd, struct k1_pd, genpd)

static bool k1_pd_is_on(struct k1_pd *pd)
{
	u32 val = 0;

	regmap_read(pd->regmap, K1_APMU_PWR_STATUS, &val);
	return val & BIT(pd->desc->stat_bit);
}

static int k1_pd_wait(struct k1_pd *pd, bool on)
{
	for (int i = 0; i < K1_PD_POLL_US / 10; i++) {
		if (k1_pd_is_on(pd) == on)
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int k1_pd_power_on(struct generic_pm_domain *gpd)
{
	struct k1_pd *pd = to_k1_pd(gpd);
	u32 reg = pd->desc->ctrl;

	regmap_set_bits(pd->regmap, reg, K1_PD_SLEEP1_BIT);
	udelay(20);
	regmap_set_bits(pd->regmap, reg, K1_PD_SLEEP2_BIT);
	udelay(20);
	regmap_set_bits(pd->regmap, reg, K1_PD_ISO_BIT);
	udelay(10);

	return k1_pd_wait(pd, true);
}

static int k1_pd_power_off(struct generic_pm_domain *gpd)
{
	struct k1_pd *pd = to_k1_pd(gpd);
	u32 reg = pd->desc->ctrl;

	regmap_clear_bits(pd->regmap, reg, K1_PD_ISO_BIT);
	udelay(10);
	regmap_clear_bits(pd->regmap, reg, K1_PD_SLEEP2_BIT);
	udelay(10);
	regmap_clear_bits(pd->regmap, reg, K1_PD_SLEEP1_BIT);

	return k1_pd_wait(pd, false);
}

static int k1_apmu_pd_probe(struct auxiliary_device *adev,
			    const struct auxiliary_device_id *id)
{
	struct spacemit_ccu_adev *cadev = to_spacemit_ccu_adev(adev);
	struct device *dev = &adev->dev;
	struct genpd_onecell_data *data;
	struct generic_pm_domain **domains;
	u32 n = ARRAY_SIZE(k1_pd_descs);
	int ret;

	data = devm_kzalloc(dev, sizeof(*data), GFP_KERNEL);
	domains = devm_kcalloc(dev, n, sizeof(*domains), GFP_KERNEL);
	if (!data || !domains)
		return -ENOMEM;

	for (u32 i = 0; i < n; i++) {
		struct k1_pd *pd;

		if (!k1_pd_descs[i].name)
			continue;

		pd = devm_kzalloc(dev, sizeof(*pd), GFP_KERNEL);
		if (!pd)
			return -ENOMEM;

		pd->regmap = cadev->regmap;
		pd->desc = &k1_pd_descs[i];
		pd->genpd.name = pd->desc->name;
		pd->genpd.power_on = k1_pd_power_on;
		pd->genpd.power_off = k1_pd_power_off;

		ret = pm_genpd_init(&pd->genpd, NULL, !k1_pd_is_on(pd));
		if (ret)
			return ret;

		domains[i] = &pd->genpd;
	}

	data->domains = domains;
	data->num_domains = n;

	return of_genpd_add_provider_onecell(dev->parent->of_node, data);
}

static const struct auxiliary_device_id k1_apmu_pd_ids[] = {
	{ .name = "spacemit_ccu.k1-apmu-pd" },
	{ }
};
MODULE_DEVICE_TABLE(auxiliary, k1_apmu_pd_ids);

static struct auxiliary_driver k1_apmu_pd_driver = {
	.probe = k1_apmu_pd_probe,
	.id_table = k1_apmu_pd_ids,
};
module_auxiliary_driver(k1_apmu_pd_driver);

MODULE_DESCRIPTION("SpacemiT K1 APMU power domain driver");
MODULE_LICENSE("GPL");
