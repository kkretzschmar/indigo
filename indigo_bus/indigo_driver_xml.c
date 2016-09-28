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
//  THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS
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
//  0.0 PoC by Peter Polakovic <peter.polakovic@cloudmakers.eu>


#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>
#include <assert.h>

#include "indigo_xml.h"
#include "indigo_driver_xml.h"

static pthread_mutex_t xmutex = PTHREAD_MUTEX_INITIALIZER;

static char encoding_table[] =
{ 'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H',
  'I', 'J', 'K', 'L', 'M', 'N', 'O', 'P',
  'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X',
  'Y', 'Z', 'a', 'b', 'c', 'd', 'e', 'f',
  'g', 'h', 'i', 'j', 'k', 'l', 'm', 'n',
  'o', 'p', 'q', 'r', 's', 't', 'u', 'v',
  'w', 'x', 'y', 'z', '0', '1', '2', '3',
  '4', '5', '6', '7', '8', '9', '+', '/'
};

static int mod_table[] = {0, 2, 1};

static void xprintf(int handle, const char *format, ...) {
  char buffer[1024];
  va_list args;
  va_start(args, format);
  int length = vsnprintf(buffer, 1024, format, args);
  va_end(args);
  write(handle, buffer, length);
  INDIGO_DEBUG(indigo_debug("sent: %s", buffer));
}

static indigo_result xml_driver_adapter_define_property(indigo_client *client, struct indigo_driver *driver, indigo_property *property) {
  assert(driver != NULL);
  assert(client != NULL);
  assert(property != NULL);
  INDIGO_LOCK(&xmutex);
  indigo_xml_driver_adapter_context *client_context = (indigo_xml_driver_adapter_context *)client->client_context;
  assert(client_context != NULL);
  int handle = client_context->output;
  switch (property->type) {
    case INDIGO_TEXT_VECTOR:
      xprintf(handle, "<defTextVector device='%s' name='%s' group='%s' label='%s' perm='%s' state='%s'>\n", property->device, property->name, property->group, property->label, indigo_property_perm_text[property->perm], indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<defText name='%s' label='%s'>%s</defText>\n", item->name, item->label, item->text_value);
      }
      xprintf(handle, "</defTextVector>\n");
      break;
    case INDIGO_NUMBER_VECTOR:
      xprintf(handle, "<defNumberVector device='%s' name='%s' group='%s' label='%s' perm='%s' state='%s'>\n", property->device, property->name, property->group, property->label, indigo_property_perm_text[property->perm], indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<defNumber name='%s' label='%s' min='%g' max='%g' step='%g'>%g</defNumber>\n", item->name, item->label, item->number_min, item->number_max, item->number_step, item->number_value);
      }
      xprintf(handle, "</defNumberVector>\n");
      break;
    case INDIGO_SWITCH_VECTOR:
      xprintf(handle, "<defSwitchVector device='%s' name='%s' group='%s' label='%s' perm='%s' state='%s' rule='%s'>\n", property->device, property->name, property->group, property->label, indigo_property_perm_text[property->perm], indigo_property_state_text[property->state], indigo_switch_rule_text[property->rule]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<defSwitch name='%s' label='%s'>%s</defSwitch>\n", item->name, item->label, item->switch_value ? "On" : "Off");
      }
      xprintf(handle, "</defSwitchVector>\n");
      break;
    case INDIGO_LIGHT_VECTOR:
      xprintf(handle, "<defLightVector device='%s' name='%s' group='%s' label='%s' perm='%s' state='%s'>\n", property->device, property->name, property->group, property->label, indigo_property_perm_text[property->perm], indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, " <defLight name='%s' label='%s'>%s</defLight>\n", item->name, item->label, indigo_property_state_text[item->light_value]);
      }
      xprintf(handle, "</defLightVector>\n");
      break;
    case INDIGO_BLOB_VECTOR:
      xprintf(handle, "<defBLOBVector device='%s' name='%s' group='%s' label='%s' perm='%s' state='%s'>\n", property->device, property->name, property->group, property->label, indigo_property_perm_text[property->perm], indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<defBLOB name='%s' label='%s'/>\n", item->name, item->label);
      }
      xprintf(handle, "</defBLOBVector>\n");
      break;
  }
  INDIGO_UNLOCK(&xmutex);
  return INDIGO_OK;
}

static indigo_result xml_driver_adapter_update_property(indigo_client *client, indigo_driver *driver, indigo_property *property) {
  assert(driver != NULL);
  assert(client != NULL);
  assert(property != NULL);
  INDIGO_LOCK(&xmutex);
  indigo_xml_driver_adapter_context *client_context = (indigo_xml_driver_adapter_context *)client->client_context;
  assert(client_context != NULL);
  int handle = client_context->output;
  switch (property->type) {
    case INDIGO_TEXT_VECTOR:
      xprintf(handle, "<setTextVector device='%s' name='%s' state='%s'>\n", property->device, property->name, indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<oneText name='%s'>%s</oneText>\n", item->name, item->text_value);
      }
      xprintf(handle, "</setTextVector>\n");
      break;
    case INDIGO_NUMBER_VECTOR:
      xprintf(handle, "<setNumberVector device='%s' name='%s' state='%s'>\n", property->device, property->name, indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<oneNumber name='%s'>%g</oneNumber>\n", item->name, item->number_value);
      }
      xprintf(handle, "</setNumberVector>\n");
      break;
    case INDIGO_SWITCH_VECTOR:
      xprintf(handle, "<setSwitchVector device='%s' name='%s' state='%s'>\n", property->device, property->name, indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<oneSwitch name='%s'>%s</oneSwitch>\n", item->name, item->switch_value ? "On" : "Off");
      }
      xprintf(handle, "</setSwitchVector>\n");
      break;
    case INDIGO_LIGHT_VECTOR:
      xprintf(handle, "<setLightVector device='%s' name='%s' state='%s'>\n", property->device, property->name, indigo_property_state_text[property->state]);
      for (int i = 0; i < property->count; i++) {
        indigo_item *item = &property->items[i];
        xprintf(handle, "<oneLight name='%s'>%s</oneLight>\n", item->name, indigo_property_state_text[item->light_value]);
      }
      xprintf(handle, "</setLightVector>\n");
      break;
    case INDIGO_BLOB_VECTOR:
      xprintf(handle, "<setBLOBVector device='%s' name='%s' state='%s'>\n", property->device, property->name, indigo_property_state_text[property->state]);
      if (property->state == INDIGO_OK_STATE) {
        for (int i = 0; i < property->count; i++) {
          indigo_item *item = &property->items[i];
          long input_length = item->blob_size;
          unsigned char *data = item->blob_value;
          char encoded_data[74];
          xprintf(handle, "<oneBLOB name='%s' format='%s' size='%ld'>\n", item->name, item->blob_format, item->blob_size);
          int j = 0;
          int i = 0;
          while (i < input_length) {
            unsigned int octet_a = i < input_length ? (unsigned char)data[i++] : 0;
            unsigned int octet_b = i < input_length ? (unsigned char)data[i++] : 0;
            unsigned int octet_c = i < input_length ? (unsigned char)data[i++] : 0;
            unsigned int triple = (octet_a << 0x10) + (octet_b << 0x08) + octet_c;
            encoded_data[j++] = encoding_table[(triple >> 3 * 6) & 0x3F];
            encoded_data[j++] = encoding_table[(triple >> 2 * 6) & 0x3F];
            encoded_data[j++] = encoding_table[(triple >> 1 * 6) & 0x3F];
            encoded_data[j++] = encoding_table[(triple >> 0 * 6) & 0x3F];
            if (j == 72) {
              encoded_data[j++] = '\n';
              encoded_data[j++] = 0;
              xprintf(handle, encoded_data);
              j = 0;
            }
          }
          for (int i = 0; i < mod_table[input_length % 3]; i++) {
            encoded_data[j++] = '=';
          }
          encoded_data[j++] = '\n';
          encoded_data[j++] = 0;
          xprintf(handle, encoded_data);
          xprintf(handle, "</oneBLOB>\n");
        }
      }
      xprintf(handle, "</setBLOBVector>\n");
      break;
  }
  INDIGO_UNLOCK(&xmutex);
  return INDIGO_OK;
}

static indigo_result xml_driver_adapter_delete_property(indigo_client *client, indigo_driver *driver, indigo_property *property) {
  assert(driver != NULL);
  assert(client != NULL);
  assert(property != NULL);
  INDIGO_LOCK(&xmutex);
  indigo_xml_driver_adapter_context *client_context = (indigo_xml_driver_adapter_context *)client->client_context;
  assert(client_context != NULL);
  int handle = client_context->output;
  xprintf(handle, "<delProperty device='%s' name='%s'>\n", property->device, property->name);
  INDIGO_UNLOCK(&xmutex);
  return INDIGO_OK;
}

indigo_client *xml_driver_adapter(int input, int ouput) {
  static indigo_client client_template = {
    NULL, INDIGO_OK, INDIGO_VERSION_CURRENT,
    NULL,
    xml_driver_adapter_define_property,
    xml_driver_adapter_update_property,
    xml_driver_adapter_delete_property,
    NULL
  };
  indigo_client *client = malloc(sizeof(indigo_client));
  if (client != NULL) {
    memcpy(client, &client_template, sizeof(indigo_client));
    indigo_xml_driver_adapter_context *client_context = malloc(sizeof(indigo_xml_driver_adapter_context));
    client_context->input = input;
    client_context->output = ouput;
    client->client_context = client_context;
  }
  return client;
}
