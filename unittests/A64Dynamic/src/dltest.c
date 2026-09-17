/* SPDX-License-Identifier: MIT */
/* dlopen/dlsym/dlclose with a TLS variable in the plugin, and dlerror. */
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "libplugin.so";
	void *h = dlopen(path, RTLD_NOW);
	if (!h) {
		printf("dlopen failed: %s\n", dlerror());
		return 1;
	}
	int (*add)(int, int) = dlsym(h, "plugin_add");
	const char *(*name)(void) = dlsym(h, "plugin_name");
	int *tls = dlsym(h, "plugin_tls");
	printf("syms: add=%s name=%s tls=%s\n", add ? "yes" : "no", name ? "yes" : "no", tls ? "yes" : "no");
	printf("add=%d add=%d tls=%d\n", add(1, 2), add(10, 20), *tls);
	printf("name=%s\n", name());
	printf("missing: %s\n", dlsym(h, "no_such_symbol") ? "found" : "null");
	const char *err = dlerror();
	printf("dlerror-mentions-symbol=%s\n", err && strstr(err, "no_such_symbol") ? "yes" : "no");
	printf("dlclose=%d\n", dlclose(h));
	void *bad = dlopen("libdoesnotexist.so", RTLD_NOW);
	printf("dlopen-missing=%s\n", bad ? "found" : "null");
	return 0;
}
