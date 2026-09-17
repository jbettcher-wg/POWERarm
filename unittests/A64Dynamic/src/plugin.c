/* SPDX-License-Identifier: MIT */
#include <stdio.h>

__thread int plugin_tls = 41;
static int counter;

int plugin_add(int a, int b)
{
	counter++;
	return a + b + plugin_tls++;
}

const char *plugin_name(void)
{
	static char buf[64];
	snprintf(buf, sizeof(buf), "plugin(calls=%d,tls=%d)", counter, plugin_tls);
	return buf;
}
