/* orient_interface_for_device: only the two landscape sides map, and to the matching interface value. */
#include <stdio.h>
#include "platform/orient_map.h"

int main(void)
{
	/* device LandscapeLeft(3) -> interface LandscapeRight(3); device LandscapeRight(4) -> interface LandscapeLeft(4) */
	int want[] = {0, 0, 0, 3, 4, 0, 0, 0}; /* unknown, portrait, upside down, L, R, face up, face down, other */
	for (int d = 0; d < 8; d++)
		if (orient_interface_for_device(d) != want[d]) {
			printf("orient_map test: FAIL device %d\n", d);
			return 1;
		}
	printf("orient_map test: ok\n");
	return 0;
}
