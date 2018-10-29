// Copyright (c) 2018 CloudMakers, s. r. o.
// All rights reserved.
//
// You can use this software under the terms of 'INDIGO Astronomy
// open-source license' (see LICENSE.md).
//
// THIS SOFTWARE IS PROVIDED BY THE AUTHORS 'AS IS' AND ANY EXPRESS
// OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
// WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
// DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
// GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

// version history
// 2.0 by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO AltairAstro CCD driver
 \file indigo_ccd_altair.c
 */

#define DRIVER_VERSION 0x0006
#define DRIVER_NAME "indigo_ccd_altair"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <assert.h>
#include <pthread.h>
#include <sys/time.h>

#include <altaircam.h>

#include "indigo_usb_utils.h"
#include "indigo_driver_xml.h"

#include "indigo_ccd_altair.h"

#define PRIVATE_DATA        ((altair_private_data *)device->private_data)

typedef struct {
	AltaircamInstV2 cam;
	HAltairCam handle;
	bool present;
	indigo_device *camera;
	indigo_device *guider;
	indigo_timer *exposure_timer, *temperature_timer, *guider_timer;
	unsigned char *buffer;
	bool pull_active;
	bool push_active;
	int bits;
	bool can_check_temperature;
	pthread_mutex_t mutex;
} altair_private_data;

// -------------------------------------------------------------------------------- INDIGO CCD device implementation

static void pull_callback(unsigned event, void* callbackCtx) {
	AltaircamFrameInfoV2 frameInfo;
	HRESULT result;
	indigo_device *device = (indigo_device *)callbackCtx;
	if (PRIVATE_DATA->pull_active) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "pull_callback #%d", event);
		switch (event) {
			case ALTAIRCAM_EVENT_IMAGE: {
				PRIVATE_DATA->pull_active = false;
				result = Altaircam_PullImageV2(PRIVATE_DATA->handle, PRIVATE_DATA->buffer + FITS_HEADER_SIZE, PRIVATE_DATA->bits, &frameInfo);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_PullImageV2(%d, ->[%d x %d, %x, %d]) -> %08x", PRIVATE_DATA->bits, frameInfo.width, frameInfo.height, frameInfo.flag, frameInfo.seq, result);
				result = Altaircam_Pause(PRIVATE_DATA->handle, 1);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_Pause(1) -> %08x", result);
				indigo_process_image(device, PRIVATE_DATA->buffer, frameInfo.width, frameInfo.height, PRIVATE_DATA->bits, true, NULL);
				CCD_EXPOSURE_ITEM->number.value = 0;
				CCD_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
				indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
				break;
			}
			case ALTAIRCAM_EVENT_TIMEOUT:
			case ALTAIRCAM_EVENT_DISCONNECTED:
			case ALTAIRCAM_EVENT_ERROR: {
				CCD_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_update_property(device, CCD_EXPOSURE_PROPERTY, NULL);
				break;
			}
		}
	}
}

static void push_callback(const void *data, const AltaircamFrameInfoV2* frameInfo, int snap, void* callbackCtx) {
	HRESULT result;
	indigo_device *device = (indigo_device *)callbackCtx;
	if (PRIVATE_DATA->push_active) {
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "push_callback %d x %d, %x, %d", frameInfo->width, frameInfo->height, frameInfo->flag, frameInfo->seq);
		int size = frameInfo->width * frameInfo->height * (PRIVATE_DATA->bits / 8);
		memcpy(PRIVATE_DATA->buffer + FITS_HEADER_SIZE,data, size);
		indigo_process_image(device, PRIVATE_DATA->buffer, frameInfo->width, frameInfo->height, PRIVATE_DATA->bits, true, NULL);
		if (CCD_STREAMING_COUNT_ITEM->number.value > 0)
			CCD_STREAMING_COUNT_ITEM->number.value -= 1;
		if (CCD_STREAMING_COUNT_ITEM->number.value == 0) {
			PRIVATE_DATA->push_active = false;
			result = Altaircam_Pause(PRIVATE_DATA->handle, 1);
			INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_Pause(1) -> %08x", result);
			CCD_STREAMING_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, CCD_STREAMING_PROPERTY, NULL);
		}
		indigo_update_property(device, CCD_STREAMING_PROPERTY, NULL);
	}
}

static void ccd_temperature_callback(indigo_device *device) {
	if (!CONNECTION_CONNECTED_ITEM->sw.value)
		return;
	if (PRIVATE_DATA->can_check_temperature) {
		short temperature;
		if (Altaircam_get_Temperature(PRIVATE_DATA->handle, &temperature) >= 0) {
			CCD_TEMPERATURE_ITEM->number.value = temperature / 1.0;
			if (CCD_TEMPERATURE_PROPERTY->perm == INDIGO_RW_PERM && fabs(CCD_TEMPERATURE_ITEM->number.value - CCD_TEMPERATURE_ITEM->number.target) > 1.0) {
				if (!CCD_COOLER_PROPERTY->hidden && CCD_COOLER_OFF_ITEM->sw.value)
					CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
				else
					CCD_TEMPERATURE_PROPERTY->state = INDIGO_BUSY_STATE;
			} else {
				CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			}
			indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
		}
	}
	indigo_reschedule_timer(device, 5, &PRIVATE_DATA->temperature_timer);
}

static void setup_exposure(indigo_device *device) {
	HRESULT result = Altaircam_Stop(PRIVATE_DATA->handle);
	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_Stop() -> %08x", result);
	PRIVATE_DATA->bits = 0;
	unsigned resolutionIndex = 0;
	for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
		indigo_item *item = CCD_MODE_PROPERTY->items + i;
		if (item->sw.value) {
			if (strncmp(item->name, "RAW8", 4) == 0) {
				result = Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_RAW, 1);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Option(ALTAIRCAM_OPTION_RAW, 1) -> %08x", result);
				result = Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_BITDEPTH, 0);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Option(ALTAIRCAM_OPTION_BITDEPTH, 0) -> %08x", result);
				resolutionIndex = atoi(item->name + 5);
				result = Altaircam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_eSize(%d) -> %08x", resolutionIndex, result);
				PRIVATE_DATA->bits = 8;
			} else if (strncmp(item->name, "RAW16", 4) == 0) {
				result = Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_RAW, 1);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Option(ALTAIRCAM_OPTION_RAW, 1) -> %08x", result);
				result = Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_BITDEPTH, 1);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Option(ALTAIRCAM_OPTION_BITDEPTH, 1) -> %08x", result);
				resolutionIndex = atoi(item->name + 6);
				result = Altaircam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_eSize(%d) -> %08x", resolutionIndex, result);
				PRIVATE_DATA->bits = 16;
			} else if (strncmp(item->name, "RGB", 3) == 0) {
				result = Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_RAW, 0);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Option(ALTAIRCAM_OPTION_RAW, 0) -> %08x", result);
				resolutionIndex = atoi(item->name + 4);
				result = Altaircam_put_eSize(PRIVATE_DATA->handle, resolutionIndex);
				INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_eSize(%d) -> %08x", resolutionIndex, result);
				PRIVATE_DATA->bits = 24;
			}
		}
	}
	if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_ROI_HARDWARE) {
		unsigned left = 2 * ((unsigned)CCD_FRAME_LEFT_ITEM->number.value / (unsigned)CCD_BIN_HORIZONTAL_ITEM->number.value / 2);
		unsigned top = 2 * ((unsigned)CCD_FRAME_TOP_ITEM->number.value / (unsigned)CCD_BIN_VERTICAL_ITEM->number.value / 2);
		unsigned width = 2 * ((unsigned)CCD_FRAME_WIDTH_ITEM->number.value / (unsigned)CCD_BIN_HORIZONTAL_ITEM->number.value / 2);
		if (width < 16)
			width = 16;
		unsigned height = 2 * ((unsigned)CCD_FRAME_HEIGHT_ITEM->number.value / (unsigned)CCD_BIN_VERTICAL_ITEM->number.value / 2);
		if (height < 16)
			height = 16;
		result = Altaircam_put_Roi(PRIVATE_DATA->handle, left, top, width, height);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_Roi(%d, %d, %d, %d) -> %08x", left, top, width, height, result);
	}
	result = Altaircam_Flush(PRIVATE_DATA->handle);
	INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_Flush() -> %08x", result);
}

static indigo_result ccd_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_ccd_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		// --------------------------------------------------------------------------------
		unsigned long long flags = PRIVATE_DATA->cam.model->flag;
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "flags = %0LX", flags);
		char name[128], label[128];
		CCD_INFO_PIXEL_WIDTH_ITEM->number.value = PRIVATE_DATA->cam.model->xpixsz;
		CCD_INFO_PIXEL_HEIGHT_ITEM->number.value = PRIVATE_DATA->cam.model->ypixsz;
		CCD_INFO_PIXEL_SIZE_ITEM->number.value = (CCD_INFO_PIXEL_WIDTH_ITEM->number.value + CCD_INFO_PIXEL_HEIGHT_ITEM->number.value) / 2.0;
		CCD_MODE_PROPERTY->perm = INDIGO_RW_PERM;
		CCD_MODE_PROPERTY->count = 0;
		CCD_INFO_WIDTH_ITEM->number.value = 0;
		CCD_INFO_HEIGHT_ITEM->number.value = 0;
		for (int i = 0; i < PRIVATE_DATA->cam.model->preview; i++) {
			int frame_width = PRIVATE_DATA->cam.model->res[i].width;
			int frame_height = PRIVATE_DATA->cam.model->res[i].height;
			if (frame_width > CCD_INFO_WIDTH_ITEM->number.value)
				CCD_INFO_WIDTH_ITEM->number.value = frame_width;
			if (frame_height > CCD_INFO_HEIGHT_ITEM->number.value)
				CCD_INFO_HEIGHT_ITEM->number.value = frame_height;
			if (flags & ALTAIRCAM_FLAG_RAW8) {
				snprintf(name, sizeof(name), "RAW8_%d", i);
				snprintf(label, sizeof(label), "RAW %d x %dx8", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
			if (flags & ALTAIRCAM_FLAG_RAW16 || flags & ALTAIRCAM_FLAG_RAW14 || flags & ALTAIRCAM_FLAG_RAW12 || flags & ALTAIRCAM_FLAG_RAW10) {
				snprintf(name, sizeof(name), "RAW16_%d", i);
				snprintf(label, sizeof(label), "RAW %d x %dx16", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
			if ((flags & ALTAIRCAM_FLAG_MONO) == 0) {
				snprintf(name, sizeof(name), "RGB_%d", i);
				snprintf(label, sizeof(label), "RGB %d x %d", frame_width, frame_height);
				indigo_init_switch_item(CCD_MODE_ITEM + CCD_MODE_PROPERTY->count, name, label, false);
				CCD_MODE_PROPERTY->count++;
			}
		}
		CCD_MODE_ITEM->sw.value = true;
		CCD_FRAME_WIDTH_ITEM->number.value = CCD_FRAME_WIDTH_ITEM->number.max = CCD_FRAME_LEFT_ITEM->number.max = CCD_INFO_WIDTH_ITEM->number.value;
		CCD_FRAME_HEIGHT_ITEM->number.value = CCD_FRAME_HEIGHT_ITEM->number.max = CCD_FRAME_TOP_ITEM->number.max = CCD_INFO_HEIGHT_ITEM->number.value;
		CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 0;
		if (flags & ALTAIRCAM_FLAG_RAW8) {
			CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 8;
		}
		if (flags & ALTAIRCAM_FLAG_RAW16 || flags & ALTAIRCAM_FLAG_RAW14 || flags & ALTAIRCAM_FLAG_RAW12 || flags & ALTAIRCAM_FLAG_RAW10) {
			if (CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min == 0)
				CCD_FRAME_BITS_PER_PIXEL_ITEM->number.min = 16;
			CCD_INFO_BITS_PER_PIXEL_ITEM->number.value = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.max = CCD_FRAME_BITS_PER_PIXEL_ITEM->number.value = 16;
		}
		if ((flags & ALTAIRCAM_FLAG_ROI_HARDWARE) == 0) {
			CCD_FRAME_PROPERTY->perm = INDIGO_RO_PERM;
		}
		if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_GETTEMPERATURE) {
			CCD_TEMPERATURE_PROPERTY->hidden = false;
			if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_PUTTEMPERATURE) {
				CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RW_PERM;
				if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_TEC_ONOFF) {
					CCD_COOLER_PROPERTY->hidden = false;
					indigo_set_switch(CCD_COOLER_PROPERTY, CCD_COOLER_OFF_ITEM, true);
				}
			} else {
				CCD_TEMPERATURE_PROPERTY->perm = INDIGO_RO_PERM;
			}
		}
		CCD_BIN_PROPERTY->perm = INDIGO_RO_PERM;
		CCD_STREAMING_PROPERTY->hidden = false;
		PRIVATE_DATA->buffer = (unsigned char *)indigo_alloc_blob_buffer(3 * CCD_INFO_WIDTH_ITEM->number.value * CCD_INFO_HEIGHT_ITEM->number.value + FITS_HEADER_SIZE);
		pthread_mutex_init(&PRIVATE_DATA->mutex, NULL);
		// --------------------------------------------------------------------------------
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_ccd_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result ccd_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	HRESULT result;
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION -> CCD_INFO, CCD_COOLER, CCD_TEMPERATURE
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (PRIVATE_DATA->handle == NULL) {
				if (indigo_try_global_lock(device) != INDIGO_OK) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "indigo_try_global_lock(): failed to get lock.");
				} else {
					PRIVATE_DATA->handle = Altaircam_Open(PRIVATE_DATA->cam.id);
				}
			}
			device->gp_bits = 1;
			if (PRIVATE_DATA->handle) {
				if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_GETTEMPERATURE)
					PRIVATE_DATA->temperature_timer = indigo_set_timer(device, 5.0, ccd_temperature_callback);
				else
					PRIVATE_DATA->temperature_timer = NULL;
				int rawMode;
				int bitDepth;
				unsigned resolutionIndex;
				char name[16];
				if (PRIVATE_DATA->cam.model->flag & ALTAIRCAM_FLAG_MONO) {
					rawMode = 1;
				} else {
					result = Altaircam_get_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_RAW, &rawMode);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_get_Option(ALTAIRCAM_OPTION_RAW, ->%d) -> %08x", rawMode, result);
				}
				if (rawMode) {
					result = Altaircam_get_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_BITDEPTH, &bitDepth);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_get_Option(ALTAIRCAM_OPTION_BITDEPTH, ->%d) -> %08x", bitDepth, result);
					result = Altaircam_get_eSize(PRIVATE_DATA->handle, &resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_get_eSize(->%d) -> %08x", resolutionIndex, result);
					sprintf(name, "RAW%d_%d", bitDepth ? 16 : 8, resolutionIndex);
				} else {
					result = Altaircam_get_eSize(PRIVATE_DATA->handle, &resolutionIndex);
					INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_get_eSize(->%d) -> %08x", resolutionIndex, result);
					sprintf(name, "RGB_%d", resolutionIndex);
				}
				for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
					if (strcmp(name, CCD_MODE_PROPERTY->items[i].name) == 0) {
						indigo_set_switch(CCD_MODE_PROPERTY, CCD_MODE_PROPERTY->items + i, true);
					}
				}
				CCD_BIN_HORIZONTAL_ITEM->number.value = (int)(CCD_INFO_WIDTH_ITEM->number.value / PRIVATE_DATA->cam.model->res[resolutionIndex].width);
				CCD_BIN_VERTICAL_ITEM->number.value = (int)(CCD_INFO_HEIGHT_ITEM->number.value / PRIVATE_DATA->cam.model->res[resolutionIndex].height);
			} else {
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
				device->gp_bits = 0;
			}
		} else {
			indigo_cancel_timer(device, &PRIVATE_DATA->temperature_timer);
			if (PRIVATE_DATA->buffer != NULL) {
				free(PRIVATE_DATA->buffer);
				PRIVATE_DATA->buffer = NULL;
			}
			if (PRIVATE_DATA->guider && PRIVATE_DATA->guider->gp_bits == 0) {
				if (((altair_private_data *)PRIVATE_DATA->guider->private_data)->handle == NULL) {
					pthread_mutex_lock(&PRIVATE_DATA->mutex);
					Altaircam_Close(PRIVATE_DATA->handle);
					pthread_mutex_unlock(&PRIVATE_DATA->mutex);
				}
				PRIVATE_DATA->handle = NULL;
				indigo_global_unlock(device);
			}
			device->gp_bits = 0;
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(CCD_MODE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_MODE
		indigo_property_copy_values(CCD_MODE_PROPERTY, property, false);
		for (int i = 0; i < CCD_MODE_PROPERTY->count; i++) {
			indigo_item *item = &CCD_MODE_PROPERTY->items[i];
			if (item->sw.value) {
				char *underscore = strchr(item->name, '_');
				unsigned resolutionIndex = atoi(underscore + 1);
				CCD_BIN_HORIZONTAL_ITEM->number.value = (int)(CCD_INFO_WIDTH_ITEM->number.value / PRIVATE_DATA->cam.model->res[resolutionIndex].width);
				CCD_BIN_VERTICAL_ITEM->number.value = (int)(CCD_INFO_HEIGHT_ITEM->number.value / PRIVATE_DATA->cam.model->res[resolutionIndex].height);
				break;
			}
		}
		if (IS_CONNECTED) {
			CCD_BIN_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, CCD_BIN_PROPERTY, NULL);
			CCD_MODE_PROPERTY->state = INDIGO_OK_STATE;
			indigo_update_property(device, CCD_MODE_PROPERTY, NULL);
		}
		return INDIGO_OK;
	} else if (indigo_property_match(CCD_EXPOSURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_EXPOSURE
		if (CCD_EXPOSURE_PROPERTY->state == INDIGO_BUSY_STATE)
			return INDIGO_OK;
		indigo_property_copy_values(CCD_EXPOSURE_PROPERTY, property, false);
		pthread_mutex_lock(&PRIVATE_DATA->mutex);
		setup_exposure(device);
		result = Altaircam_put_ExpoTime(PRIVATE_DATA->handle, (unsigned)(CCD_EXPOSURE_ITEM->number.target * 1000000));
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_ExpoTime(%u) -> %08x", (unsigned)(CCD_EXPOSURE_ITEM->number.target * 1000000), result);
		PRIVATE_DATA->pull_active = true;
		HRESULT result = Altaircam_StartPullModeWithCallback(PRIVATE_DATA->handle, pull_callback, device);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_StartPullModeWithCallback() -> %08x", result);
		pthread_mutex_unlock(&PRIVATE_DATA->mutex);
		CCD_EXPOSURE_PROPERTY->state = INDIGO_BUSY_STATE;
	} else if (indigo_property_match(CCD_STREAMING_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_STREAMING
		if (CCD_STREAMING_PROPERTY->state == INDIGO_BUSY_STATE)
			return INDIGO_OK;
		indigo_property_copy_values(CCD_STREAMING_PROPERTY, property, false);
		pthread_mutex_lock(&PRIVATE_DATA->mutex);
		setup_exposure(device);
		result = Altaircam_put_ExpoTime(PRIVATE_DATA->handle, (unsigned)(CCD_STREAMING_EXPOSURE_ITEM->number.target * 1000000));
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_put_ExpoTime(%u) -> %08x", (unsigned)(CCD_STREAMING_EXPOSURE_ITEM->number.target * 1000000), result);
		PRIVATE_DATA->push_active = true;
		HRESULT result = Altaircam_StartPushModeV2(PRIVATE_DATA->handle, push_callback, device);
		INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_StartPushModeV2() -> %08x", result);
		pthread_mutex_unlock(&PRIVATE_DATA->mutex);
		CCD_STREAMING_PROPERTY->state = INDIGO_BUSY_STATE;
	} else if (indigo_property_match(CCD_ABORT_EXPOSURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_ABORT_EXPOSURE
		indigo_property_copy_values(CCD_ABORT_EXPOSURE_PROPERTY, property, false);
		if (CCD_ABORT_EXPOSURE_ITEM->sw.value) {
			PRIVATE_DATA->pull_active = false;
			PRIVATE_DATA->push_active = false;
			CCD_ABORT_EXPOSURE_ITEM->sw.value = false;
			pthread_mutex_lock(&PRIVATE_DATA->mutex);
			result = Altaircam_Stop(PRIVATE_DATA->handle);
			INDIGO_DRIVER_DEBUG(DRIVER_NAME, "Altaircam_Stop() -> %08x", result);
			pthread_mutex_unlock(&PRIVATE_DATA->mutex);
			if (result >=0)
				CCD_ABORT_EXPOSURE_PROPERTY->state = INDIGO_OK_STATE;
			else
				CCD_ABORT_EXPOSURE_PROPERTY->state = INDIGO_ALERT_STATE;
		}
	} else if (indigo_property_match(CCD_COOLER_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_COOLER
		indigo_property_copy_values(CCD_COOLER_PROPERTY, property, false);
		if (Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_TEC, CCD_COOLER_ON_ITEM->sw.value ? 1 : 0) >= 0)
			CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
		else
			CCD_COOLER_PROPERTY->state = INDIGO_ALERT_STATE;
		indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(CCD_TEMPERATURE_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CCD_TEMPERATURE
		indigo_property_copy_values(CCD_TEMPERATURE_PROPERTY, property, false);
		if (Altaircam_put_Temperature(PRIVATE_DATA->handle, (short)(CCD_TEMPERATURE_ITEM->number.target * 10))) {
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_OK_STATE;
			if (!CCD_COOLER_PROPERTY->hidden && CCD_COOLER_OFF_ITEM->sw.value) {
				if (Altaircam_put_Option(PRIVATE_DATA->handle, ALTAIRCAM_OPTION_TEC, 1) >= 0) {
					indigo_set_switch(CCD_COOLER_PROPERTY, CCD_COOLER_ON_ITEM, true);
					CCD_COOLER_PROPERTY->state = INDIGO_OK_STATE;
				} else {
					CCD_COOLER_PROPERTY->state = INDIGO_ALERT_STATE;
				}
				indigo_update_property(device, CCD_COOLER_PROPERTY, NULL);
			}
		} else {
			CCD_TEMPERATURE_PROPERTY->state = INDIGO_ALERT_STATE;
		}
		indigo_update_property(device, CCD_TEMPERATURE_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_ccd_change_property(device, client, property);
}

static indigo_result ccd_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	if (device == device->master_device)
		indigo_global_unlock(device);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_ccd_detach(device);
}

// -------------------------------------------------------------------------------- INDIGO guider device implementation


static indigo_result guider_attach(indigo_device *device) {
	assert(device != NULL);
	assert(PRIVATE_DATA != NULL);
	if (indigo_guider_attach(device, DRIVER_VERSION) == INDIGO_OK) {
		INDIGO_DEVICE_ATTACH_LOG(DRIVER_NAME, device->name);
		return indigo_guider_enumerate_properties(device, NULL, NULL);
	}
	return INDIGO_FAILED;
}

static indigo_result guider_change_property(indigo_device *device, indigo_client *client, indigo_property *property) {
	assert(device != NULL);
	assert(DEVICE_CONTEXT != NULL);
	assert(property != NULL);
	if (indigo_property_match(CONNECTION_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- CONNECTION
		indigo_property_copy_values(CONNECTION_PROPERTY, property, false);
		if (CONNECTION_CONNECTED_ITEM->sw.value) {
			if (PRIVATE_DATA->handle == NULL) {
				if (indigo_try_global_lock(device) != INDIGO_OK) {
					INDIGO_DRIVER_ERROR(DRIVER_NAME, "indigo_try_global_lock(): failed to get lock.");
				} else {
					PRIVATE_DATA->handle = Altaircam_Open(PRIVATE_DATA->cam.id);
				}
			}
			device->gp_bits = 1;
			if (PRIVATE_DATA->handle) {
			} else {
				CONNECTION_PROPERTY->state = INDIGO_ALERT_STATE;
				indigo_set_switch(CONNECTION_PROPERTY, CONNECTION_DISCONNECTED_ITEM, true);
				device->gp_bits = 0;
			}
		} else {
			if (PRIVATE_DATA->buffer != NULL) {
				free(PRIVATE_DATA->buffer);
				PRIVATE_DATA->buffer = NULL;
			}
			if (PRIVATE_DATA->camera && PRIVATE_DATA->camera->gp_bits == 0) {
				if (((altair_private_data *)PRIVATE_DATA->camera->private_data)->handle == NULL) {
					pthread_mutex_lock(&PRIVATE_DATA->mutex);
					Altaircam_Close(PRIVATE_DATA->handle);
					pthread_mutex_unlock(&PRIVATE_DATA->mutex);
					indigo_global_unlock(device);
				}
				PRIVATE_DATA->handle = NULL;
			}
			device->gp_bits = 0;
			CONNECTION_PROPERTY->state = INDIGO_OK_STATE;
		}
	} else if (indigo_property_match(GUIDER_GUIDE_DEC_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_DEC
		HRESULT result = 0;
		indigo_property_copy_values(GUIDER_GUIDE_DEC_PROPERTY, property, false);
		if (GUIDER_GUIDE_NORTH_ITEM->number.value > 0)
			result = Altaircam_ST4PlusGuide(PRIVATE_DATA->handle, 0, GUIDER_GUIDE_NORTH_ITEM->number.value);
		else if (GUIDER_GUIDE_SOUTH_ITEM->number.value > 0)
			result = Altaircam_ST4PlusGuide(PRIVATE_DATA->handle, 1, GUIDER_GUIDE_SOUTH_ITEM->number.value);
		GUIDER_GUIDE_DEC_PROPERTY->state = SUCCEEDED(result) ? INDIGO_OK_STATE : INDIGO_ALERT_STATE;
		indigo_update_property(device, GUIDER_GUIDE_DEC_PROPERTY, NULL);
		return INDIGO_OK;
	} else if (indigo_property_match(GUIDER_GUIDE_RA_PROPERTY, property)) {
		// -------------------------------------------------------------------------------- GUIDER_GUIDE_RA
		HRESULT result = 0;
		indigo_property_copy_values(GUIDER_GUIDE_RA_PROPERTY, property, false);
		if (GUIDER_GUIDE_EAST_ITEM->number.value > 0)
			result = Altaircam_ST4PlusGuide(PRIVATE_DATA->handle, 2, GUIDER_GUIDE_EAST_ITEM->number.value);
		else if (GUIDER_GUIDE_WEST_ITEM->number.value > 0)
			result = Altaircam_ST4PlusGuide(PRIVATE_DATA->handle, 3, GUIDER_GUIDE_WEST_ITEM->number.value);
		GUIDER_GUIDE_RA_PROPERTY->state = SUCCEEDED(result) ? INDIGO_OK_STATE : INDIGO_ALERT_STATE;
		indigo_update_property(device, GUIDER_GUIDE_RA_PROPERTY, NULL);
		return INDIGO_OK;
		// --------------------------------------------------------------------------------
	}
	return indigo_guider_change_property(device, client, property);
}

static indigo_result guider_detach(indigo_device *device) {
	assert(device != NULL);
	if (CONNECTION_CONNECTED_ITEM->sw.value)
		indigo_device_disconnect(NULL, device->name);
	if (device == device->master_device)
		indigo_global_unlock(device);
	INDIGO_DEVICE_DETACH_LOG(DRIVER_NAME, device->name);
	return indigo_guider_detach(device);
}

// -------------------------------------------------------------------------------- hot-plug support

static bool hotplug_callback_initialized = false;
static indigo_device *devices[ALTAIRCAM_MAX];

static void hotplug_callback(void* pCallbackCtx) {
	for (int i = 0; i < ALTAIRCAM_MAX; i++) {
		indigo_device *device = devices[i];
		if (device)
			PRIVATE_DATA->present = false;
	}
	AltaircamInstV2 cams[ALTAIRCAM_MAX];
	int cnt = Altaircam_EnumV2(cams);
	for (int j = 0; j < cnt; j++) {
		AltaircamInstV2 cam = cams[j];
		bool found = false;
		for (int i = 0; i < ALTAIRCAM_MAX; i++) {
			indigo_device *device = devices[i];
			if (device && memcmp(PRIVATE_DATA->cam.id, cam.id, sizeof(64)) == 0) {
				found = true;
				PRIVATE_DATA->present = true;
				break;
			}
		}
		if (!found) {
			static indigo_device ccd_template = INDIGO_DEVICE_INITIALIZER(
				"",
				ccd_attach,
				indigo_ccd_enumerate_properties,
				ccd_change_property,
				NULL,
				ccd_detach
				);
			altair_private_data *private_data = malloc(sizeof(altair_private_data));
			assert(private_data != NULL);
			memset(private_data, 0, sizeof(altair_private_data));
			private_data->cam = cam;
			private_data->present = true;
			indigo_device *camera = malloc(sizeof(indigo_device));
			assert(camera != NULL);
			memcpy(camera, &ccd_template, sizeof(indigo_device));
			snprintf(camera->name, INDIGO_NAME_SIZE, "AltairAstro %s #%s", cam.displayname, cam.id);
			camera->private_data = private_data;
			private_data->camera = camera;
			for (int i = 0; i < ALTAIRCAM_MAX; i++) {
				if (devices[j] == NULL) {
					indigo_async((void *)(void *)indigo_attach_device, devices[j] = camera);
					break;
				}
			}
			if (cam.model->flag & ALTAIRCAM_FLAG_ST4) {
				static indigo_device guider_template = INDIGO_DEVICE_INITIALIZER(
					"",
					guider_attach,
					indigo_guider_enumerate_properties,
					guider_change_property,
					NULL,
					guider_detach
					);
				indigo_device *guider = malloc(sizeof(indigo_device));
				assert(guider != NULL);
				memcpy(guider, &guider_template, sizeof(indigo_device));
				snprintf(guider->name, INDIGO_NAME_SIZE, "AltairAstro %s (guider) #%s", cam.displayname, cam.id);
				guider->private_data = private_data;
				private_data->guider = guider;
				indigo_async((void *)(void *)indigo_attach_device, guider);
			}
		}
	}
	for (int i = 0; i < ALTAIRCAM_MAX; i++) {
		indigo_device *device = devices[i];
		if (device && !PRIVATE_DATA->present) {
			indigo_device *guider = PRIVATE_DATA->guider;
			if (guider) {
				indigo_detach_device(guider);
				free(guider);
			}
			indigo_detach_device(device);
			if (device->private_data)
				free(device->private_data);
			free(device);
			devices[i] = NULL;
		}
	}
}


indigo_result indigo_ccd_altair(indigo_driver_action action, indigo_driver_info *info) {
	static indigo_driver_action last_action = INDIGO_DRIVER_SHUTDOWN;

	SET_DRIVER_INFO(info, "AltairAstro Camera", __FUNCTION__, DRIVER_VERSION, true, last_action);

	if (action == last_action)
		return INDIGO_OK;

	switch(action) {
		case INDIGO_DRIVER_INIT: {
			last_action = action;
			for (int i = 0; i < ALTAIRCAM_MAX; i++)
				devices[i] = NULL;
			if (!hotplug_callback_initialized) {
				Altaircam_HotPlug(hotplug_callback, NULL);
				hotplug_callback_initialized = true;
			}
			INDIGO_DRIVER_LOG(DRIVER_NAME, "AltairAstro SDK version %s", Altaircam_Version());
			hotplug_callback(NULL);
			break;
		}
		case INDIGO_DRIVER_SHUTDOWN:
			break;

		case INDIGO_DRIVER_INFO:
			break;
	}

	return INDIGO_OK;
}
