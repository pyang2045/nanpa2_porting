/* Device orientation -> interface orientation for the two landscape sides, on UIKit's raw values:
 * UIDeviceOrientationLandscapeLeft = 3, LandscapeRight = 4; UIInterfaceOrientationLandscapeRight = 3,
 * LandscapeLeft = 4. The interface side that matches a device side has the same raw value (the names are
 * swapped: the device's "left" is the interface's "right"). 0 = not a landscape side: portrait, face
 * up/down and unknown never change the interface. Pure, tested by tests/orient_map_test.c. */
#ifndef ORIENT_MAP_H
#define ORIENT_MAP_H

static inline int orient_interface_for_device(int device_orientation)
{
	return (device_orientation == 3 || device_orientation == 4) ? device_orientation : 0;
}

#endif
