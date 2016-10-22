//  Copyright (c) 2016 CloudMakers, s. r. o.
//  All rights reserved.
//
//  Redistribution and use in source and binary forms, with or without
//  modification, are permitted provided that the following conditions
//  are met:
//
//  1. Redistributions of source code must retain the above copyright
//  notice, this list of conditions and the following disclaimer.
//
//  2. Redistributions in binary form must reproduce the above
//  copyright notice, this list of conditions and the following
//  disclaimer in the documentation and/or other materials provided
//  with the distribution.
//
//  3. The name of the author may not be used to endorse or promote
//  products derived from this software without specific prior
//  written permission.
//
//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR 'AS IS' AND ANY EXPRESS
//  OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//  WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//  ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
//  DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
//  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
//  GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
//  WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
//  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

//  version history
//  2.0 Build 0 - PoC by Peter Polakovic <peter.polakovic@cloudmakers.eu>

/** INDIGO Filter wheel Driver base
 \file indigo_wheel_driver.h
 */

#ifndef indigo_wheel_device_h
#define indigo_wheel_device_h

#include "indigo_bus.h"
#include "indigo_driver.h"

/** Main wheel group name string.
 */
#define WHEEL_MAIN_GROUP                "Filter wheel main"

/** Device context pointer.
 */
#define WHEEL_DEVICE_CONTEXT                ((indigo_wheel_device_context *)device->device_context)

/** WHEEL_SLOT property pointer, property is mandatory, property change request should be fully handled by device driver.
 */
#define WHEEL_SLOT_PROPERTY									(WHEEL_DEVICE_CONTEXT->wheel_slot_property)

/** WHEEL_SLOT.SLOT property item pointer.
 */
#define WHEEL_SLOT_ITEM											(WHEEL_SLOT_PROPERTY->items+0)

/** WHEEL_SLOT_NAME property pointer, property is mandatory, property change request should be fully handled by indigo_wheel_device_change_property.
 */
#define WHEEL_SLOT_NAME_PROPERTY             (WHEEL_DEVICE_CONTEXT->wheel_slot_name_property)

/** WHEEL_SLOT_NAME.NAME_1 property item pointer.
 */
#define WHEEL_SLOT_NAME_1_ITEM               (WHEEL_SLOT_NAME_PROPERTY->items+0)

/** Wheel device context structure.
 */
typedef struct {
	indigo_device_context device_context;       ///< device context base
	indigo_property *wheel_slot_property;				///< WHEEL_SLOT property pointer
	indigo_property *wheel_slot_name_property;  ///< WHEEL_SLOT_NAME property pointer
} indigo_wheel_device_context;

/** Attach callback function.
 */
extern indigo_result indigo_wheel_device_attach(indigo_device *device, indigo_version version);
/** Enumerate properties callback function.
 */
extern indigo_result indigo_wheel_device_enumerate_properties(indigo_device *device, indigo_client *client, indigo_property *property);
/** Change property callback function.
 */
extern indigo_result indigo_wheel_device_change_property(indigo_device *device, indigo_client *client, indigo_property *property);
/** Detach callback function.
 */
extern indigo_result indigo_wheel_device_detach(indigo_device *device);

#endif /* indigo_wheel_device_h */

