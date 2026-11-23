/*
 * The first ordinary program: dynamically linked against the real Darwin
 * libSystem, loaded by the real dyld out of the dyld shared cache.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>

static void
raise_wire_limit(void)
{
	uint64_t mem = 0, old_lim = 0, new_lim = 0;
	size_t len = sizeof(mem);

	if (sysctlbyname("hw.memsize", &mem, &len, NULL, 0) != 0 || mem <= (64ULL << 20)) {
		return;
	}
	len = sizeof(old_lim);
	(void)sysctlbyname("vm.global_user_wire_limit", &old_lim, &len, NULL, 0);
	new_lim = mem - (32ULL << 20);
	if (new_lim > old_lim) {
		if (sysctlbyname("vm.global_user_wire_limit", NULL, NULL, &new_lim, sizeof(new_lim)) == 0) {
			printf("hakios: vm.global_user_wire_limit %llu -> %llu MB (mem %llu MB, uid %u)\n",
			    (unsigned long long)(old_lim >> 20), (unsigned long long)(new_lim >> 20),
			    (unsigned long long)(mem >> 20), (unsigned)getuid());
		} else {
			printf("hakios: vm.global_user_wire_limit failed: %s (uid %u)\n", strerror(errno), (unsigned)getuid());
		}
	}
	uint32_t pc_thresh = 100000000u;
	(void)sysctlbyname("vm.phantom_cache_thrashing_threshold", NULL, NULL, &pc_thresh, sizeof(pc_thresh));
	(void)sysctlbyname("vm.phantom_cache_thrashing_threshold_ssd", NULL, NULL, &pc_thresh, sizeof(pc_thresh));
}

int
main(int argc, char **argv)
{
	struct utsname u;
	char *copy = strdup("malloc works");

	raise_wire_limit();

	printf("Hello from a dynamically linked program! pid %d, %d argument(s)\n", getpid(), argc);
	if (uname(&u) == 0) {
		printf("uname: %s %s %s\n", u.sysname, u.release, u.machine);
	}
	printf("heap: %s (%p), stack near %p, main at %p, printf at %p\n", copy, (void *)copy, (void *)&u, (void *)main, (void *)printf);
	for (int i = 0; i < argc; i++) {
		printf("argv[%d] = %s\n", i, argv[i]);
	}
	free(copy);
	return 7;
}
