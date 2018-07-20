#ifndef UTIL_IPSET_H
#define UTIL_IPSET_H



int appl_entry_format (struct appl_t *appl,
	const uint32_t __oryx_unused_param__*rule_id,
	const char *unused_var,
	const char __oryx_unused_param__*vlan,
	const char __oryx_unused_param__*sip,
	const char __oryx_unused_param__*dip,
	const char __oryx_unused_param__*sp,
	const char __oryx_unused_param__*dp,
	const char __oryx_unused_param__*proto);

int appl_entry_del (vlib_appl_main_t *am, struct appl_t *appl);

int appl_table_entry_deep_lookup(const char *argv, 

void appl_entry_new (struct appl_t **appl, 
			const char *alias, uint32_t __oryx_unused_param__ type);

