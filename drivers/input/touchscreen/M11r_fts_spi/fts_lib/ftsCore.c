// SPDX-License-Identifier: GPL-2.0-or-later
/*
 **************************************************************************
 **                        STMicroelectronics                            **
 **************************************************************************
 **                        marco.cali@st.com                             **
 **************************************************************************
 *                                                                        *
 *                           FTS Core functions                           *
 *                                                                        *
 **************************************************************************
 **************************************************************************
 */

/*!
 * \file ftsCore.c
 * \brief Contains the implementation of the Core functions
 */

#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>
#include "ftsCompensation.h"
#include "ftsCore.h"
#include "ftsError.h"
#include "ftsIO.h"
#include "ftsTest.h"
#include "ftsTime.h"
#include "ftsTool.h"

extern struct fts_ts_info *fts_info;
/** @addtogroup system_info
 * @{
 */
SysInfo systemInfo;							/*Global System Info variable, accessible in all the driver*/
/** @}*/

static int reset_gpio = GPIO_NOT_DEFINED;	/*gpio number of the rest pin, the value is  GPIO_NOT_DEFINED if the reset pin is not connected*/
static int system_reseted_up;				/*flag checked during resume to understand if there was a system reset and restore the proper state*/
static int system_reseted_down;				/*flag checked during suspend to understand if there was a system reset and restore the proper state*/
static int disable_irq_count;				/*count the number of call to disable_irq, start with 1 because at the boot IRQ are already disabled*/
spinlock_t fts_int;							/*spinlock to controll the access to the disable_irq_counter*/

/**
 * Initialize core variables of the library. Must be called during the probe before any other lib function
 * @param info pointer to fts_ts_info which contains info about the device and its hw setup
 * @return OK if success or an error code which specify the type of error encountered
 */
int initCore(struct fts_ts_info *info)
{
	int ret = OK;

	logError(0, "%s %s: Initialization of the Core...\n", tag, __func__);
	ret |= openChannel(info->client);
	ret |= resetErrorList();
	ret |= initTestToDo();
	setResetGpio(info->board->reset_gpio);
	if (ret < OK) {
		logError(0, "%s %s: Initialization Core ERROR %08X!\n", tag,
			 __func__, ret);
	} else {
		logError(0, "%s %s: Initialization Finished!\n", tag,
			 __func__);
	}
	return ret;
}

/**
 * Set the reset_gpio variable with the actual gpio number of the board link to the reset pin
 * @param gpio number link to the reset pin of the IC
 */
void setResetGpio(int gpio)
{
	reset_gpio = gpio;
	logError(0, "%s %s: reset_gpio = %d\n", tag, __func__, reset_gpio);
}

/**
 * Perform a system reset of the IC.
 * If the reset pin is associated to a gpio, the function execute an hw reset (toggling of reset pin) otherwise send an hw command to the IC
 * @return OK if success or an error code which specify the type of error encountered
 */
int fts_system_reset(void)
{
	u8 readData[FIFO_EVENT_SIZE];
	int event_to_search;
	int res = -1;
	int i;
	u8 data[1] = { SYSTEM_RESET_VALUE };

	event_to_search = (int)EVT_ID_CONTROLLER_READY;

	logError(1, "%s System resetting...\n", tag);
	if (fts_info) {
		reinit_completion(&fts_info->tp_reset_completion);
		atomic_set(&fts_info->system_is_resetting, 1);
	}
	for (i = 0; i < RETRY_SYSTEM_RESET && res < 0; i++) {
		resetErrorList();
		fts_disableInterruptNoSync();

		if (reset_gpio == GPIO_NOT_DEFINED) {
			res = fts_writeU8UX(FTS_CMD_HW_REG_W, ADDR_SIZE_HW_REG,
					    ADDR_SYSTEM_RESET, data,
					    ARRAY_SIZE(data));
		} else {
			gpio_set_value(reset_gpio, 0);
			mdelay(10);
			gpio_set_value(reset_gpio, 1);
			res = OK;
		}
		if (res < OK) {
			logError(1, "%s %s: ERROR %08X\n", tag, __func__,
				 ERROR_BUS_W);
		} else {
			res = pollForEvent(&event_to_search, 1, readData,
					   GENERAL_TIMEOUT);
			if (res < OK) {
				logError(1, "%s %s: ERROR %08X\n",
					 tag, __func__, res);
			}
		}
	}
	if (fts_info) {
		complete(&fts_info->tp_reset_completion);
		atomic_set(&fts_info->system_is_resetting, 0);
	}
	if (res < OK) {
		logError(1,
			 "%s %s...failed after 3 attempts: ERROR %08X\n",
			 tag, __func__, (res | ERROR_SYSTEM_RESET_FAIL));
		return (res | ERROR_SYSTEM_RESET_FAIL);
	}
	logError(1, "%s System reset DONE!\n", tag);
	system_reseted_down = 1;
	system_reseted_up = 1;
	return OK;
}

/**
 * Return the value of system_resetted_down.
 * @return the flag value: 0 if not set, 1 if set
 */
int isSystemResettedDown(void)
{
	return system_reseted_down;
}

/**
 * Return the value of system_resetted_up.
 * @return the flag value: 0 if not set, 1 if set
 */
int isSystemResettedUp(void)
{
	return system_reseted_up;
}

/**
 * Set the value of system_reseted_down flag
 * @param val value to write in the flag
 */
void setSystemResetedDown(int val)
{
	system_reseted_down = val;
}

/**
 * Set the value of system_reseted_up flag
 * @param val value to write in the flag
 */
void setSystemResetedUp(int val)
{
	system_reseted_up = val;
}

/**
 * Poll the FIFO looking for a specified event within a timeout. Support a retry mechanism.
 * @param event_to_search pointer to an array of int where each element correspond to a byte of the event to find. If the element of the array has value -1, the byte of the event, in the same position of the element is ignored.
 * @param event_bytes size of event_to_search
 * @param readData pointer to an array of byte which will contain the event found
 * @param time_to_wait time to wait before going in timeout
 * @return OK if success or an error code which specify the type of error encountered
 */
int pollForEvent(int *event_to_search, int event_bytes, u8 *readData,
		 int time_to_wait)
{
	int i, find, retry, count_err;
	int time_to_count;
	int err_handling = OK;
	/*StopWatch clock*/

	u8 cmd[1] = { FIFO_CMD_READONE };
	char temp[128] = { 0 };

	find = 0;
	retry = 0;
	count_err = 0;
	time_to_count = time_to_wait / TIMEOUT_RESOLUTION;

	/*startStopWatch(&clock);*/
	while (find != 1 && retry < time_to_count
	       && fts_writeReadU8UX(cmd[0], 0, 0, readData, FIFO_EVENT_SIZE,
				    DUMMY_FIFO) >= OK) {

		if (readData[0] == EVT_ID_ERROR) {

			logError(1, "%s %s\n", tag,
				 printHex("ERROR EVENT = ", readData,
					  FIFO_EVENT_SIZE, temp));
			memset(temp, 0, 128);
			count_err++;
			err_handling = errorHandler(readData, FIFO_EVENT_SIZE);
			if ((err_handling & 0xF0FF0000) == ERROR_HANDLER_STOP_PROC) {
				logError(1,
					 "%s %s: forced to be stopped! ERROR %08X\n",
					 tag, __func__, err_handling);
				return err_handling;
			}
		} else {
			if (readData[0] != EVT_ID_NOEVENT) {
				logError(0, "%s %s\n", tag,
					 printHex("READ EVENT = ", readData,
						  FIFO_EVENT_SIZE, temp));
				memset(temp, 0, 128);

			}
			if (readData[0] == EVT_ID_CONTROLLER_READY
			    && event_to_search[0] != EVT_ID_CONTROLLER_READY) {
				logError(0,
					 "%s %s: Unmanned Controller Ready Event! Setting reset flags...\n", tag, __func__);
				setSystemResetedUp(1);
				setSystemResetedDown(1);
			}
		}

		find = 1;

		for (i = 0; i < event_bytes; i++) {
			if (event_to_search[i] != -1
			    && (int)readData[i] != event_to_search[i]) {
				find = 0;
				break;
			}
		}

		retry++;
		mdelay(TIMEOUT_RESOLUTION);
	}
	/*stopStopWatch(&clock);*/
	if ((retry >= time_to_count) && find != 1) {
		logError(1, "%s %s: ERROR %02X\n", tag, __func__,
			 ERROR_TIMEOUT);
		return ERROR_TIMEOUT;
	} else if (find == 1) {
		logError(0, "%s %s\n", tag,
			 printHex("FOUND EVENT = ", readData, FIFO_EVENT_SIZE, temp));
		memset(temp, 0, 128);
		logError(0,
			 "%s Event found in (%d iterations)! Number of errors found = %d\n",
			 tag, /*elapsedMillisecond(&clock),*/retry, count_err);
		return count_err;
	}
	logError(1, "%s %s: ERROR %08X\n", tag, __func__, ERROR_BUS_R);
	return ERROR_BUS_R;
}

/**
 * Check that the FW sent the echo even after a command was sent
 * @param cmd pointer to an array of byte which contain the command previously sent
 * @param size of cmd
 * @return OK if success or an error code which specify the type of error encountered
 */
int checkEcho(u8 *cmd, int size)
{
	int ret, i;
	int event_to_search[FIFO_EVENT_SIZE];
	u8 readData[FIFO_EVENT_SIZE];

	if (size < 1) {
		logError(1, "%s %s: Error Size = %d not valid!\n", tag, __func__,
			 size, ERROR_OP_NOT_ALLOW);
		return ERROR_OP_NOT_ALLOW;
	}
	if ((size + 3) > FIFO_EVENT_SIZE)
		size = FIFO_EVENT_SIZE - 3;

	event_to_search[0] = EVT_ID_STATUS_UPDATE;
	event_to_search[1] = EVT_TYPE_STATUS_ECHO;
	for (i = 2; i < size + 2; i++)
		event_to_search[i] = cmd[i - 2];
	ret = pollForEvent(event_to_search, size + 2, readData,
			   TIMEOUT_ECHO);
	if (ret < OK) {
		logError(1,
			 "%s %s: Echo Event not found! ERROR %08X\n",
			 tag, __func__, ret);
		return (ret | ERROR_CHECK_ECHO_FAIL);
	} else if (ret > OK) {
		logError(1,
			 "%s %s: Echo Event found but with some error events before! num_error = %d\n",
			 tag, __func__, ret);
		return ERROR_CHECK_ECHO_FAIL;
	}

	logError(0, "%s ECHO OK!\n", tag);
	return ret;
}

/**
 * Set a scan mode in the IC
 * @param mode scan mode to set; possible values @link scan_opt Scan Mode Option @endlink
 * @param settings option for the selected scan mode (for example @link active_bitmask Active Mode Bitmask @endlink)
 * @return OK if success or an error code which specify the type of error encountered
 */
int setScanMode(u8 mode, u8 settings)
{
	u8 cmd[3] = { FTS_CMD_SCAN_MODE, mode, settings };
	int ret, size = 3;

	logError(0, "%s %s: Setting scan mode: mode = %02X settings = %02X !\n",
		 tag, __func__, mode, settings);
	if (mode == SCAN_MODE_LOW_POWER)
		size = 2;
	ret = fts_write_dma_safe(cmd, size);
	if (ret < OK) {
		logError(1, "%s %s: write failed...ERROR %08X !\n", tag,
			 __func__, ret);
		return ret | ERROR_SET_SCAN_MODE_FAIL;
	}
	logError(0, "%s %s: Setting scan mode OK!\n", tag, __func__);
	return OK;
}

/**
 * Set a feature and its option in the IC
 * @param feat feature to set; possible values @link feat_opt Feature Selection Option @endlink
 * @param settings pointer to an array of byte which store the options for the selected feature (for example the gesture mask to activate @link gesture_opt Gesture IDs @endlink)
 * @param size in bytes of settings
 * @return OK if success or an error code which specify the type of error encountered
 */
int setFeatures(u8 feat, u8 *settings, int size)
{
	u8 *cmd;
	int i = 0;
	int ret;

	logError(0, "%s %s: Setting feature: feat = %02X !\n", tag, __func__, feat);

	cmd = kzalloc(2 + size, GFP_KERNEL);
	if (cmd == NULL) {
		logError(1, "%s %s no memory\n", tag, __func__);
		return -ENOMEM;
	}
	cmd[0] = FTS_CMD_FEATURE;
	cmd[1] = feat;
	logError(0, "%s %s: Settings = ", tag, __func__);
	for (i = 0; i < size; i++) {
		cmd[2 + i] = settings[i];
		logError(0, "%02X ", settings[i]);
	}
	logError(0, "\n");
	ret = fts_write_dma_safe(cmd, 2 + size);
	if (ret < OK) {
		logError(1, "%s %s: write failed...ERROR %08X !\n", tag,
			 __func__, ret);
		return ret | ERROR_SET_FEATURE_FAIL;
	}
	logError(0, "%s %s: Setting feature OK!\n", tag, __func__);
	kfree(cmd);
	cmd = NULL;
	return OK;
}

/**
 * Write a system command to the IC
 * @param sys_cmd System Command to execute; possible values @link sys_opt System Command Option @endlink
 * @param sett settings option for the selected system command (@link sys_special_opt	 Special Command Option @endlink, @link ito_opt	ITO Test Option @endlink, @link load_opt Load Host Data Option @endlink)
 * @param size in bytes of settings
 * @return OK if success or an error code which specify the type of error encountered
 */
int writeSysCmd(u8 sys_cmd, u8 *sett, int size)
{
	u8 *cmd = NULL;
	int ret;

	cmd = kzalloc(sizeof(u8) * size + 2, GFP_KERNEL);
	if (!cmd) {
		ret = -ENOMEM;
		goto end;
	}

	cmd[0] = FTS_CMD_SYSTEM;
	cmd[1] = sys_cmd;

	logError(0, "%s %s: Command = %02X %02X ", tag, __func__, cmd[0], cmd[1]);
	for (ret = 0; ret < size; ret++) {
		cmd[2 + ret] = sett[ret];
		logError(0, "%02X ", cmd[2 + ret]);
	}
	logError(0, "\n%s %s: Writing Sys command...\n", tag, __func__);
	if (sys_cmd != SYS_CMD_LOAD_DATA)
		ret = fts_writeFwCmd(cmd, 2 + size);
	else {
		if (size >= 1)
			ret = requestSyncFrame(sett[0]);
		else {
			logError(1, "%s %s: No setting argument! ERROR %08X\n",
				 tag, __func__, ERROR_OP_NOT_ALLOW);
			kfree(cmd);
			return ERROR_OP_NOT_ALLOW;
		}
	}
	if (ret < OK)
		logError(1, "%s %s: ERROR %08X\n", tag, __func__, ret);
	else
		logError(0, "%s %s: FINISHED!\n", tag, __func__);

end:
	if (cmd)
		kfree(cmd);
	return ret;
}

/**
 * Initialize the System Info Struct with default values according to the error found during the reading
 * @param i2cError 1 if there was an I2C error while reading the System Info data from memory, other value if another error occurred
 * @return OK if success or an error code which specify the type of error encountered
 */
int defaultSysInfo(int i2cError)
{
	int i;

	logError(0, "%s Setting default System Info...\n", tag);

	if (i2cError == 1) {
		systemInfo.u16_fwVer = 0xFFFF;
		systemInfo.u16_cfgProgectId = 0xFFFF;
		for (i = 0; i < RELEASE_INFO_SIZE; i++)
			systemInfo.u8_releaseInfo[i] = 0xFF;
		systemInfo.u16_cxVer = 0xFFFF;
	} else {
		systemInfo.u16_fwVer = 0x0000;
		systemInfo.u16_cfgProgectId = 0x0000;
		for (i = 0; i < RELEASE_INFO_SIZE; i++)
			systemInfo.u8_releaseInfo[i] = 0x00;
		systemInfo.u16_cxVer = 0x0000;
	}

	systemInfo.u8_scrRxLen = 0;
	systemInfo.u8_scrTxLen = 0;

	logError(0, "%s default System Info DONE!\n", tag);
	return OK;

}

/**
 * Read the System Info data from memory. System Info is loaded automatically after every system reset.
 * @param request if 1, will be asked to the FW to reload the data, otherwise attempt to read it directly from memory
 * @return OK if success or an error code which specify the type of error encountered
 */
int readSysInfo(int request)
{
	int ret, i, index = 0;
	u8 sett = LOAD_SYS_INFO;
	u8 data[SYS_INFO_SIZE] = { 0 };
	char temp[256] = { 0 };
	u16 temp16;

	if (request == 1) {
		logError(0, "%s %s: Requesting System Info...\n", tag,
			 __func__);

		ret = writeSysCmd(SYS_CMD_LOAD_DATA, &sett, 1);
		if (ret < OK) {
			logError(1,
				 "%s %s: error while writing the sys cmd ERROR %08X\n",
				 tag, __func__, ret);
			goto FAIL;
		}
	}

	logError(0, "%s %s: Reading System Info...\n", tag, __func__);
	ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R, BITS_16, ADDR_FRAMEBUFFER,
				data, SYS_INFO_SIZE, DUMMY_FRAMEBUFFER);
	if (ret < OK) {
		logError(1,
			 "%s %s: error while reading the system data ERROR %08X\n",
			 tag, __func__, ret);
		goto FAIL;
	}

	logError(0, "%s %s: Parsing System Info...\n", tag, __func__);

	if (data[0] != HEADER_SIGNATURE) {
		logError(1,
			 "%s %s: The Header Signature is wrong!  sign: %02X != %02X ERROR %08X\n",
			 tag, __func__, data[0], HEADER_SIGNATURE,
			 ERROR_WRONG_DATA_SIGN);
		ret = ERROR_WRONG_DATA_SIGN;
		goto FAIL;
	}

	if (data[1] != LOAD_SYS_INFO) {
		logError(1,
			 "%s %s: The Data ID is wrong!  ids: %02X != %02X ERROR %08X\n",
			 tag, __func__, data[3], LOAD_SYS_INFO,
			 ERROR_DIFF_DATA_TYPE);
		ret = ERROR_DIFF_DATA_TYPE;
		goto FAIL;
	}

	index += 4;
	u8ToU16(&data[index], &systemInfo.u16_apiVer_rev);
	index += 2;
	systemInfo.u8_apiVer_minor = data[index++];
	systemInfo.u8_apiVer_major = data[index++];
	u8ToU16(&data[index], &systemInfo.u16_chip0Ver);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip0Id);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip1Ver);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_chip1Id);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_fwVer);
	index += 2;
	logError(1, "%s FW VER = %04X\n", tag, systemInfo.u16_fwVer);

	u8ToU16(&data[index], &systemInfo.u16_svnRev);
	index += 2;
	logError(1, "%s SVN REV = %04X\n", tag, systemInfo.u16_svnRev);
	u8ToU16(&data[index], &systemInfo.u16_cfgVer);
	index += 2;
	logError(1, "%s CONFIG VER = %04X\n", tag, systemInfo.u16_cfgVer);
	u8ToU16(&data[index], &systemInfo.u16_cfgProgectId);
	index += 2;
	logError(1, "%s CONFIG PROJECT ID = %04X\n", tag,
		 systemInfo.u16_cfgProgectId);
	u8ToU16(&data[index], &systemInfo.u16_cxVer);
	index += 2;
	logError(1, "%s CX VER = %04X\n", tag, systemInfo.u16_cxVer);
	u8ToU16(&data[index], &systemInfo.u16_cxProjectId);
	index += 2;
	logError(1, "%s CX PROJECT ID = %04X\n", tag,
		 systemInfo.u16_cxProjectId);
	systemInfo.u8_cfgAfeVer = data[index++];
	systemInfo.u8_cxAfeVer = data[index++];
	systemInfo.u8_panelCfgAfeVer = data[index++];
	logError(1, "%s AFE VER: CFG = %02X - CX = %02X - PANEL = %02X\n", tag,
		 systemInfo.u8_cfgAfeVer, systemInfo.u8_cxAfeVer,
		 systemInfo.u8_panelCfgAfeVer);
	systemInfo.u8_protocol = data[index++];
	logError(1, "%s Protocol = %02X\n", tag, systemInfo.u8_protocol);

	for (i = 0; i < DIE_INFO_SIZE; i++)
		systemInfo.u8_dieInfo[i] = data[index++];

	logError(0, "%s %s\n", tag,
		 printHex("Die Info =  ", systemInfo.u8_dieInfo, DIE_INFO_SIZE,
			  temp));
	memset(temp, 0, 256);

	for (i = 0; i < RELEASE_INFO_SIZE; i++)
		systemInfo.u8_releaseInfo[i] = data[index++];

	logError(1, "%s %s\n", tag,
		 printHex("Release Info =  ", systemInfo.u8_releaseInfo,
			  RELEASE_INFO_SIZE, temp));
	memset(temp, 0, 256);

	u8ToU32(&data[index], &systemInfo.u32_fwCrc);
	index += 4;
	u8ToU32(&data[index], &systemInfo.u32_cfgCrc);

	index += 4;

	index += 8;

	u8ToU16(&data[index], &systemInfo.u16_scrResX);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_scrResY);
	index += 2;
	logError(1, "%s Screen Resolution = %d x %d\n", tag,
		 systemInfo.u16_scrResX, systemInfo.u16_scrResY);
	if (systemInfo.u16_scrResX > systemInfo.u16_scrResY) {
		temp16 = systemInfo.u16_scrResX;
		systemInfo.u16_scrResX = systemInfo.u16_scrResY;
		systemInfo.u16_scrResY = temp16;
	}
	if (systemInfo.u8_protocol == 6) {
		systemInfo.u16_scrResX = (systemInfo.u16_scrResX + 1) * 10 - 1;
		systemInfo.u16_scrResY = (systemInfo.u16_scrResY + 1) * 10 - 1;
	}
	logError(1, "%s Touch Resolution = %d x %d\n", tag,
		 systemInfo.u16_scrResX, systemInfo.u16_scrResY);
	systemInfo.u8_scrTxLen = data[index++];
	logError(0, "%s TX Len = %d\n", tag, systemInfo.u8_scrTxLen);
	systemInfo.u8_scrRxLen = data[index++];
	logError(0, "%s RX Len = %d\n", tag, systemInfo.u8_scrRxLen);
	systemInfo.u8_keyLen = data[index++];
	logError(0, "%s Key Len = %d\n", tag, systemInfo.u8_keyLen);
	systemInfo.u8_forceLen = data[index++];
	logError(0, "%s Force Len = %d\n", tag, systemInfo.u8_forceLen);

	index += 40;

	u8ToU16(&data[index], &systemInfo.u16_dbgInfoAddr);
	index += 2;

	index += 6;

	u8ToU16(&data[index], &systemInfo.u16_msTchRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_msTchBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssTchTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssTchRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssTchRxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_keyRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_keyBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_frcRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_frcBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssHvrRxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxTxBaselineAddr);
	index += 2;

	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxRawAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxFilterAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxStrenAddr);
	index += 2;
	u8ToU16(&data[index], &systemInfo.u16_ssPrxRxBaselineAddr);
	index += 2;

	logError(0, "%s Parsed %d bytes!\n", tag, index);

	if (index != SYS_INFO_SIZE) {
		logError(1, "%s %s: index = %d different from %d ERROR %08X\n",
			 tag, __func__, index, SYS_INFO_SIZE,
			 ERROR_OP_NOT_ALLOW);
		return ERROR_OP_NOT_ALLOW;
	}

	logError(0, "%s System Info Read DONE!\n", tag);
	return OK;

FAIL:
	defaultSysInfo(isI2cError(ret));
	return ret;

}


/**
 * Read data from the Config Memory
 * @param offset Starting address in the Config Memory of data to read
 * @param outBuf pointer of a byte array which contain the bytes to read
 * @param len number of bytes to read
 * @return OK if success or an error code which specify the type of error encountered
 */
int readConfig(u16 offset, u8 *outBuf, int len)
{
	int ret;
	u64 final_address = offset + ADDR_CONFIG_OFFSET;

	logError(0, "%s %s: Starting to read config memory at %08X ...", tag,
		 __func__, final_address);
	ret = fts_writeReadU8UX(FTS_CMD_CONFIG_R, BITS_16, final_address, outBuf,
				len, DUMMY_CONFIG);
	if (ret < OK) {
		logError(1,
			 "%s %s: Impossible to read Config Memory... ERROR %08X!",
			 tag, __func__, ret);
		return ret;
	}

	logError(0, "%s %s: Read config memory FINISHED!", tag, __func__);
	return OK;
}

/**
 * Disable the interrupt so the ISR of the driver can not be called
 * @return OK if success or an error code which specify the type of error encountered
 */
int fts_disableInterrupt(void)
{
	if (getClient() != NULL) {
		logError(0, "%s Number of disable = %d\n", tag,
			 disable_irq_count);
		if (disable_irq_count == 0) {
			logError(0, "%s Excecuting Disable...\n", tag);
			disable_irq(getClient()->irq);
			disable_irq_count++;
			logError(1, "%s Interrupt Disabled!\n", tag);
		}
		return OK;
	}
	logError(1, "%s %s: Impossible get client irq... ERROR %08X\n",
		 tag, __func__, ERROR_OP_NOT_ALLOW);
	return ERROR_OP_NOT_ALLOW;
}

/**
 * Disable the interrupt async so the ISR of the driver can not be called
 * @return OK if success or an error code which specify the type of error encountered
 */
int fts_disableInterruptNoSync(void)
{
	if (getClient() != NULL) {
		spin_lock_irq(&fts_int);
		logError(0, "%s Number of disable = %d\n", tag,
			 disable_irq_count);
		if (disable_irq_count == 0) {
			logError(0, "%s Executing Disable...\n", tag);
			disable_irq_nosync(getClient()->irq);
			disable_irq_count++;
		}

		spin_unlock_irq(&fts_int);
		logError(0, "%s Interrupt No Sync Disabled!\n", tag);
		return OK;
	}
	logError(1, "%s %s: Impossible get client irq... ERROR %08X\n",
		 tag, __func__, ERROR_OP_NOT_ALLOW);
	return ERROR_OP_NOT_ALLOW;
}

/**
 * Reset the disable_irq count
 * @return OK
 */
int fts_resetDisableIrqCount(void)
{
	disable_irq_count = 0;
	return OK;
}

/**
 * Enable the interrupt so the ISR of the driver can be called
 * @return OK if success or an error code which specify the type of error encountered
 */
int fts_enableInterrupt(void)
{
	if (getClient() != NULL) {

		logError(0, "%s Number of re-enable = %d\n", tag,
			 disable_irq_count);
		while (disable_irq_count > 0) {
			logError(0, "%s Excecuting Enable...\n", tag);
			enable_irq(getClient()->irq);
			disable_irq_count--;
			logError(1, "%s Interrupt Enabled!\n", tag);
		}
		return OK;
	}
	logError(1, "%s %s: Impossible get client irq... ERROR %08X\n",
		 tag, __func__, ERROR_OP_NOT_ALLOW);
	return ERROR_OP_NOT_ALLOW;
}

/**
 *	Check if there is a crc error in the IC which prevent the fw to run.
 *	@return  OK if no CRC error, or a number >OK according the CRC error found
 */
int fts_crc_check(void)
{
	u8 val;
	u8 crc_status;
	int res;
	u8 error_to_search[6] = { EVT_TYPE_ERROR_CRC_CFG_HEAD, EVT_TYPE_ERROR_CRC_CFG,
				  EVT_TYPE_ERROR_CRC_CX, EVT_TYPE_ERROR_CRC_CX_HEAD,
				  EVT_TYPE_ERROR_CRC_CX_SUB,
				  EVT_TYPE_ERROR_CRC_CX_SUB_HEAD
				};

	res = fts_writeReadU8UX(FTS_CMD_HW_REG_R, ADDR_SIZE_HW_REG, ADDR_CRC,
				&val, 1, DUMMY_HW_REG);
	if (res < OK) {
		logError(1, "%s %s Cannot read crc status ERROR %08X\n", tag,
			 __func__, res);
		return res;
	}

	crc_status = val & CRC_MASK;
	if (crc_status != OK) {
		logError(1, "%s %s CRC ERROR = %02X\n", tag, __func__,
			 crc_status);
		return CRC_CODE;
	}

	logError(1, "%s %s: Verifying if Config CRC Error...\n", tag, __func__);
	res = fts_system_reset();
	if (res >= OK) {
		res = pollForErrorType(error_to_search, 2);
		if (res < OK) {
			logError(1, "%s %s: No Config CRC Error Found!\n", tag,
				 __func__);
			logError(1, "%s %s: Verifying if Cx CRC Error...\n",
				 tag, __func__);
			res = pollForErrorType(&error_to_search[2], 4);
			if (res < OK) {
				logError(1, "%s %s: No Cx CRC Error Found!\n",
					 tag, __func__);
				return OK;
			}
			logError(1,
				 "%s %s: Cx CRC Error found! CRC ERROR = %02X\n",
				 tag, __func__, res);
			return CRC_CX;
		}

		logError(1,
			 "%s %s: Config CRC Error found! CRC ERROR = %02X\n",
			 tag, __func__, res);
		return CRC_CONFIG;
	}
	logError(1,
		 "%s %s: Error while executing system reset! ERROR %08X\n",
		 tag, __func__, res);
	return res;
}

/**
 * Request a host data and use the sync method to understand when the FW load it
 * @param type the type ID of host data to load (@link load_opt	 Load Host Data Option  @endlink)
 * @return OK if success or an error code which specify the type of error encountered
 */
int requestSyncFrame(u8 type)
{
	u8 request[3] = { FTS_CMD_SYSTEM, SYS_CMD_LOAD_DATA, type };
	u8 readData[DATA_HEADER] = { 0 };
	int ret, retry = 0, retry2 = 0, time_to_count;
	int count, new_count;

	logError(0, "%s %s: Starting to get a sync frame...\n", tag, __func__);

	while (retry2 < RETRY_MAX_REQU_DATA) {
		logError(0, "%s %s: Reading count...\n", tag, __func__);

		ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R, BITS_16,
					ADDR_FRAMEBUFFER, readData, DATA_HEADER,
					DUMMY_FRAMEBUFFER);
		if (ret < OK) {
			logError(0,
				 "%s %s: Error while reading count! ERROR %08X\n",
				 tag, __func__, ret | ERROR_REQU_DATA);
			ret |= ERROR_REQU_DATA;
			retry2++;
			continue;
		}

		if (readData[0] != HEADER_SIGNATURE) {
			logError(1,
				 "%s %s: Invalid Signature while reading count! ERROR %08X\n",
				 tag, __func__, ret | ERROR_REQU_DATA);
			ret |= ERROR_REQU_DATA;
			retry2++;
			continue;
		}

		count = (readData[3] << 8) | readData[2];
		new_count = count;
		logError(0, "%s %s: Base count = %d\n", tag, __func__, count);

		logError(0, "%s %s: Requesting frame %02X  attempt = %d\n",
			 tag, __func__, type, retry2 + 1);
		ret = fts_write_dma_safe(request, ARRAY_SIZE(request));
		if (ret >= OK) {

			logError(0, "%s %s: Polling for new count...\n", tag,
				 __func__);
			time_to_count = TIMEOUT_REQU_DATA / TIMEOUT_RESOLUTION;
			while (count == new_count && retry < time_to_count) {
				ret = fts_writeReadU8UX(FTS_CMD_FRAMEBUFFER_R,
							BITS_16, ADDR_FRAMEBUFFER,
							readData, DATA_HEADER,
							DUMMY_FRAMEBUFFER);
				if (ret >= OK
				    && readData[0] == HEADER_SIGNATURE)
					new_count = ((readData[3] << 8) | readData[2]);
				else {
					logError(0,
						 "%s %s: invalid Signature or can not read count... ERROR %08X\n",
						 tag, __func__, ret);
				}
				retry++;
				mdelay(TIMEOUT_RESOLUTION);
			}

			if (count == new_count) {
				logError(1,
					 "%s %s: New count not received! ERROR %08X\n",
					 tag, __func__,
					 ERROR_TIMEOUT | ERROR_REQU_DATA);
				ret = ERROR_TIMEOUT | ERROR_REQU_DATA;
			} else {
				logError(0,
					 "%s %s: New count found! count = %d! Frame ready!\n",
					 tag, __func__, new_count);
				return OK;
			}
		}
		retry2++;
	}
	logError(1, "%s %s: Request Data failed! ERROR %08X\n", tag, __func__,
		 ret);
	return ret;
}

int calculateCRC8(u8 *u8_srcBuff, int size, u8 *crc)
{
	u8 u8_remainder;
	u8 bit;
	int i = 0;

	u8_remainder = 0x00;

	logError(0, "%s %s: Start CRC computing...\n", tag, __func__);
	if (size != 0 && u8_srcBuff != NULL) {
		for (i = 0; i < size; i++) {
			u8_remainder ^= u8_srcBuff[i];
			for (bit = 8; bit > 0; --bit) {
				if (u8_remainder & (0x1 << 7))
					u8_remainder = (u8_remainder << 1) ^ 0x9B;
				else
					u8_remainder = (u8_remainder << 1);
			}
		}
		*crc = u8_remainder;
		logError(0, "%s %s: CRC value = %02X\n", tag, __func__, *crc);
		return OK;
	}
	logError(1,
		 "%s %s: Arguments passed not valid! Data pointer = NULL or size = 0 (%d) ERROR %08X\n",
		 tag, __func__, size, ERROR_OP_NOT_ALLOW);
	return ERROR_OP_NOT_ALLOW;

}

