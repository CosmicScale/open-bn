/*
 * Tests of the confirm button on the inputs: circle and cross of the pad
 * (myPad) and of the remote (IRrm) follow the setting, the DVD player keys of
 * the remote keep their meaning.
 */

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "unity.h"
#include "app/systemoptions.h"
#include "input/irrm.h"
#include "input/mypad.h"
#include "support/testutil.h"

/* The pressures of circle and cross in myPad::m_neutral (packet +13, +14). */
#define PRESS_CIRCLE 9
#define PRESS_CROSS 10

/* The two key sets of the remote (IRrm::m_deviceId). */
#define DVD_KEYS 2362
#define CONTROLLER_KEYS 7002

/* Packet modes: a DualShock 2 with pressures, and a digital pad (the pads
   of the PlayStation among them). */
#define MODE_DUALSHOCK2 0x79
#define MODE_DIGITAL 0x41

static int s_pipe[2];
static myPad *s_pad;
static IRrm *s_rm;

/* The constructors open the drivers bn has open: the objects are built
   zeroed, reading the pad packets from a pipe. */
void setUp(void)
{
	SysOption.setPadEnter(0);
	TEST_ASSERT_EQUAL_INT(0, pipe(s_pipe));
	s_pad = (myPad *)calloc(1, sizeof(myPad));
	s_pad->m_fd = s_pipe[0];
	s_pad->m_readMode = 1;
	s_pad->m_analog = 1;
	s_rm = (IRrm *)calloc(1, sizeof(IRrm));
}

void tearDown(void)
{
	close(s_pipe[0]);
	close(s_pipe[1]);
	free(s_pad);
	free(s_rm);
	SysOption.setPadEnter(0);
}

/*
 * Has the pad read a packet with `pressed` held, as the driver returns it
 * ((mode & 0x0f) * 2 + 2 bytes), and `force` from the remote; the pressure
 * of every held button is 0xff. Returns the buttons held for the pages.
 */
static unsigned short readPad(int mode, unsigned short pressed, unsigned short force)
{
	/* the order of the pressures in the packet */
	static const unsigned short order[12] = { PAD_RIGHT, PAD_LEFT, PAD_UP, PAD_DOWN, PAD_TRIANGLE, PAD_CIRCLE,
		PAD_CROSS, PAD_SQUARE, PAD_L1, PAD_R1, PAD_L2, PAD_R2 };
	unsigned char packet[32];
	unsigned short raw = ~pressed;
	int size = (mode & 0x0f) * 2 + 2;
	int i;

	memset(packet, 0, sizeof(packet));
	packet[1] = mode;
	packet[2] = raw & 0xff;
	packet[3] = raw >> 8;
	memset(packet + 4, 128, 4);
	for (i = 0; i < 12; i++)
		if (pressed & order[i])
			packet[8 + i] = 0xff;
	TEST_ASSERT_EQUAL_INT(size, write(s_pipe[1], packet, size));
	s_pad->pad_button_read(force);
	return s_pad->m_buttons[0].w;
}

/* The pad buttons of a key of the remote. */
static unsigned short remoteKey(int keys, int index)
{
	s_rm->m_deviceId = keys;
	s_rm->m_index = index;
	return s_rm->GetPadCode();
}

static void test_swap_moves_only_circle_and_cross(void)
{
	TEST_ASSERT_EQUAL_HEX16(PAD_CROSS, padSwapEnter(PAD_CIRCLE));
	TEST_ASSERT_EQUAL_HEX16(PAD_CIRCLE, padSwapEnter(PAD_CROSS));
	TEST_ASSERT_EQUAL_HEX16(PAD_CIRCLE | PAD_CROSS, padSwapEnter(PAD_CIRCLE | PAD_CROSS));
	TEST_ASSERT_EQUAL_HEX16(PAD_TRIANGLE | PAD_START, padSwapEnter(PAD_TRIANGLE | PAD_START));
	TEST_ASSERT_EQUAL_HEX16(0, padSwapEnter(0));
	/* active low, as the packets of the driver */
	TEST_ASSERT_EQUAL_HEX16((unsigned short)~PAD_CROSS, padSwapEnter((unsigned short)~PAD_CIRCLE));
}

static void test_pad_circle_confirms(void)
{
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, readPad(MODE_DUALSHOCK2, PAD_CIRCLE, 0));
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, readPad(MODE_DUALSHOCK2, PAD_CROSS, 0));
	TEST_ASSERT_EQUAL_HEX8(0xff, s_pad->m_neutral[PRESS_CROSS]);
	TEST_ASSERT_EQUAL_HEX8(0, s_pad->m_neutral[PRESS_CIRCLE]);
}

static void test_pad_cross_confirms(void)
{
	SysOption.setPadEnter(1);
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, readPad(MODE_DUALSHOCK2, PAD_CROSS, 0));
	TEST_ASSERT_EQUAL_HEX8(0xff, s_pad->m_neutral[PRESS_CIRCLE]);
	TEST_ASSERT_EQUAL_HEX8(0, s_pad->m_neutral[PRESS_CROSS]);
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, readPad(MODE_DUALSHOCK2, PAD_CIRCLE, 0));
	TEST_ASSERT_EQUAL_HEX16(PAD_TRIANGLE | PAD_L1, readPad(MODE_DUALSHOCK2, PAD_TRIANGLE | PAD_L1, 0));
}

static void test_digital_pad_follows_the_setting(void)
{
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, readPad(MODE_DIGITAL, PAD_CROSS, 0));
	SysOption.setPadEnter(1);
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, readPad(MODE_DIGITAL, PAD_CROSS, 0));
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, readPad(MODE_DIGITAL, PAD_CIRCLE, 0));
}

/* The circle and cross keys of the remote (codes 85 and 86). */
static void test_remote_circle_and_cross_follow_the_setting(void)
{
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, remoteKey(CONTROLLER_KEYS, 0x5d));
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, remoteKey(CONTROLLER_KEYS, 0x5e));
	TEST_ASSERT_EQUAL_HEX16(PAD_TRIANGLE, remoteKey(CONTROLLER_KEYS, 0x5c));
	SysOption.setPadEnter(1);
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, remoteKey(CONTROLLER_KEYS, 0x5e));
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, remoteKey(CONTROLLER_KEYS, 0x5d));
	TEST_ASSERT_EQUAL_HEX16(PAD_TRIANGLE, remoteKey(CONTROLLER_KEYS, 0x5c));
}

/* The DVD player keys give the same buttons whatever the confirm button. */
static void test_dvd_keys(void)
{
	/* index: the key in the names of sceRm2CodeToStr */
	static const struct {
		int index;
		unsigned short buttons;
	} keys[] = {
		{ 0x0b, PAD_CONFIRM }, /* ENTER */
		{ 0x38, PAD_BACK },    /* STOP */
		{ 0x30, PAD_L1 },      /* PREVIOUS */
		{ 0x31, PAD_R1 },      /* NEXT */
		{ 0x33, PAD_L2 },      /* REV SCAN */
		{ 0x34, PAD_R2 },      /* FWD SCAN */
		{ 0x32, PAD_START },   /* PLAY */
		{ 0x39, PAD_START },   /* PAUSE */
		{ 0x54, PAD_SELECT },  /* DISPLAY ON/OFF */
		{ 0x00, 0 },           /* 1 */
		{ 0x1b, 0 },           /* DVDMENU */
	};
	int enter;
	unsigned i;

	for (enter = 0; enter < 2; enter++) {
		SysOption.setPadEnter(enter);
		for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
			TEST_ASSERT_EQUAL_HEX16(keys[i].buttons, remoteKey(DVD_KEYS, keys[i].index));
	}
}

static void test_no_key(void)
{
	TEST_ASSERT_EQUAL_HEX16(0, remoteKey(0, 0));
}

/* The buttons of the remote reach the pages through the pad, not swapped
   again. */
static void test_remote_through_the_pad(void)
{
	SysOption.setPadEnter(1);
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, readPad(MODE_DUALSHOCK2, 0, remoteKey(DVD_KEYS, 0x0b)));
	TEST_ASSERT_EQUAL_HEX16(PAD_BACK, readPad(MODE_DUALSHOCK2, 0, remoteKey(DVD_KEYS, 0x38)));
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM, readPad(MODE_DUALSHOCK2, 0, remoteKey(CONTROLLER_KEYS, 0x5e)));
	TEST_ASSERT_EQUAL_HEX16(PAD_CONFIRM | PAD_L1, readPad(MODE_DUALSHOCK2, PAD_CROSS, remoteKey(DVD_KEYS, 0x30)));
}

/* crt1 calls main, which is bn's: the link wraps it (ld --wrap main). */
extern "C" int __wrap_main(int, char **)
{
	startTests();
	UNITY_BEGIN();
	RUN_TEST(test_swap_moves_only_circle_and_cross);
	RUN_TEST(test_pad_circle_confirms);
	RUN_TEST(test_pad_cross_confirms);
	RUN_TEST(test_digital_pad_follows_the_setting);
	RUN_TEST(test_remote_circle_and_cross_follow_the_setting);
	RUN_TEST(test_dvd_keys);
	RUN_TEST(test_no_key);
	RUN_TEST(test_remote_through_the_pad);
	return UNITY_END();
}
