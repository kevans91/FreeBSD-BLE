#include <sys/consio.h>
#include <sys/mouse.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/sysctl.h>
#include <sys/bitstring.h>
#include <sys/select.h>
#include <assert.h>
#include <err.h>
#include <errno.h>
#include <openssl/aes.h>
#include <netgraph/ng_message.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid.h>
#define L2CAP_SOCKET_CHECKED
#include <bluetooth.h>
#include "hccontrol.h"
#include "att.h"
#include "gatt.h"
#include <sqlite3.h>
#include <getopt.h>
#include "sql.h"
#include "service.h"
#include <usbhid.h>
#include <dev/usb/usbhid.h>
#include "hogp.h"
void hogp_init(struct service *service, int s);
void hogp_notify(void *sc, int charid, unsigned char *buf, int len);
#define HID_INFORMATION 0x2a4a 
#define HID_REPORT_MAP 0x2a4b
#define HID_CONTROL_POINT 0x2a4c
#define HID_REPORT 0x2a4d
#define PROTOCOL_MODE 0x2a4e
#define HID_BOOT_KEYBOARD 0x2a22
#define HID_BOOT_MOUSE 0x2432
#define REPORT_REFERENCE 0x2908
#define CLIENT_CONFIGURATION 0x2902
extern uuid_t uuid_base;
#define MAXRIDMAP 10
struct hogp_ridmap{
	int cid;
	int rid;
	int type;
};
struct hogp_service{
	unsigned char hidmap[512];
	report_desc_t desc;
	int nrmap;
	int cons;
	struct hogp_ridmap rmap[MAXRIDMAP];
};

static struct service_driver hogp_driver=
{

	.init = hogp_init,
	.notify = hogp_notify,
};


void hogp_register()
{
	hogp_driver.uuid = uuid_base;
	hogp_driver.uuid.time_low = 0x1812;
	register_driver(&hogp_driver);
}


static const char *
hid_collection_type(int32_t type)
{
	static char num[8];

	switch (type) {
	case 0: return ("Physical");
	case 1: return ("Application");
	case 2: return ("Logical");
	case 3: return ("Report");
	case 4: return ("Named_Array");
	case 5: return ("Usage_Switch");
	case 6: return ("Usage_Modifier");
	}
	snprintf(num, sizeof(num), "0x%02x", type);
	return (num);
}

static void
dumpitem(const char *label, struct hid_item *h)
{
	//if ((h->flags & HIO_CONST) && !verbose)
	//return;
	printf("%s rid=%d size=%d count=%d page=%s usage=%s%s%s", label,
	       h->report_ID, h->report_size, h->report_count,
	       hid_usage_page(HID_PAGE(h->usage)),
	       hid_usage_in_page(h->usage),
	       h->flags & HIO_CONST ? " Const" : "",
	       h->flags & HIO_VARIABLE ? "" : " Array");
	printf(", logical range %d..%d",
	       h->logical_minimum, h->logical_maximum);
	if (h->physical_minimum != h->physical_maximum)
		printf(", physical range %d..%d",
		       h->physical_minimum, h->physical_maximum);
	if (h->unit)
		printf(", unit=0x%02x exp=%d", h->unit, h->unit_exponent);
	printf("\n");
}

void hid_dump_item(report_desc_t rd)
{
	hid_data_t hd;
	hid_item_t it;
	int res;


	hd = hid_start_parse(rd, ~0, -1);
	for (hd = hid_start_parse(rd, ~0, -1); hid_get_item(hd, &it); ) {
		switch(it.kind){
		case hid_collection:
			printf("Collection type=%s page=%s usage=%s\n",
			       hid_collection_type(it.collection),
			       hid_usage_page(HID_PAGE(it.usage)),
			       hid_usage_in_page(it.usage));
			break;
		case hid_endcollection:
			printf("End collection\n");
			break;
		case hid_input:
			dumpitem("Input  ", &it);
			break;
		case hid_output:
			dumpitem("Output ", &it);
			break;
		case hid_feature:
			dumpitem("Feature", &it);
			break;
		}
	}
}

void hogp_init(struct service *service, int s)
{
	unsigned char buf[40];
	uuid_t uuid;
	int cid;
	int len;
	hid_init(NULL);
	int error;
	static sqlite3_stmt *stmt;
	struct hogp_service *serv;
	service->sc = serv = malloc(sizeof(*serv));
	
	printf("HOGP:%d\n", service->service_id);
	uuid = uuid_base;
	if(stmt == NULL)
		stmt = get_stmt("SELECT chara_id from ble_chara where service_id = $1 and uuid = $2;");

	uuid.time_low = HID_INFORMATION;
	sqlite3_bind_int(stmt, 1, service->service_id);
	sqlite3_bind_blob(stmt, 2, &uuid, sizeof(uuid), SQLITE_TRANSIENT);
	
	if((error = sqlite3_step(stmt)) != SQLITE_ROW){
		fprintf(stderr, "HID Information not found %d\n", error);
		return ;
	}
	cid = sqlite3_column_int(stmt, 0);
	sqlite3_reset(stmt);
	len = le_char_read(s, cid, buf, sizeof(buf), 0);
	if(len < 0){
		fprintf(stderr, "Cannot read HID Info %d\n", len);
	}
	printf("HID Version:%x Country Code %d FLAG:%x\n", buf[0]|(buf[1]<<8),
	       buf[2], buf[3]);
	serv->cons = open("/dev/consolectl", O_RDWR);
	printf("%d\n", serv->cons);
	uuid.time_low = HID_REPORT_MAP;
	sqlite3_bind_int(stmt, 1, service->service_id);
	sqlite3_bind_blob(stmt, 2, &uuid, sizeof(uuid), SQLITE_TRANSIENT);
	
	if((error = sqlite3_step(stmt)) != SQLITE_ROW){
		fprintf(stderr, "HID REPORT MAP not found %d\n", error);
		return ;
	}
	cid = sqlite3_column_int(stmt, 0);
	sqlite3_reset(stmt);
	len = le_char_read(s, cid, serv->hidmap, sizeof(serv->hidmap), 0);
	if(len < 0){
		fprintf(stderr, "Cannot read REPORT MAP %d \n", len);
		return;
	}
	serv->desc = hid_use_report_desc(serv->hidmap, len);	
	hid_dump_item(serv->desc);
	uuid.time_low = HID_REPORT;
	sqlite3_bind_int(stmt, 1, service->service_id);
	sqlite3_bind_blob(stmt, 2, &uuid, sizeof(uuid), SQLITE_TRANSIENT);
	serv->nrmap = 0;
	while((error = sqlite3_step(stmt)) == SQLITE_ROW){
		int report_type;
		cid = sqlite3_column_int(stmt, 0);
		uuid.time_low = REPORT_REFERENCE;
		le_char_desc_read(s, cid, &uuid, buf, sizeof(buf), 0);
		serv->rmap[serv->nrmap].cid = cid;
		serv->rmap[serv->nrmap].rid = buf[0];
		report_type = serv->rmap[serv->nrmap].type = buf[1];		
		printf("CharID: %x ReportID:%d ReportType%d\n", cid,
		       buf[0], buf[1]);
		serv->nrmap++;
		if(report_type == 1){
			buf[0] = 1;
			buf[1] = 0;
			uuid.time_low = CLIENT_CONFIGURATION;
			le_char_desc_write(s, cid, &uuid, buf, 2, 0);
		}
	}
	sqlite3_reset(stmt);
	printf("%d\n", serv->nrmap);
	return;
}
void hogp_process_report(struct hogp_service *serv, unsigned char *buf)
{
	int rid = buf[0];
	int32_t	usage, page, val,
		mouse_x, mouse_y, mouse_z, mouse_butt,
		mevents, kevents, i;
	hid_data_t d;
	hid_item_t h;
	mouse_x = mouse_y = mouse_z = mouse_butt = mevents = kevents = 0;	
  	for (d = hid_start_parse(serv->desc, 1 << hid_input, -1);
	     hid_get_item(d, &h) > 0; ) {
		if ((h.flags & HIO_CONST) || (h.report_ID != rid) ||
		    (h.kind != hid_input))
			continue;
		page = HID_PAGE(h.usage);		
		val = hid_get_data(buf, &h);
		
		/*
		 * When the input field is an array and the usage is specified
		 * with a range instead of an ID, we have to derive the actual
		 * usage by using the item value as an index in the usage range
		 * list.
		 */
		if ((h.flags & HIO_VARIABLE)) {
			usage = HID_USAGE(h.usage);
		} else {
			const uint32_t usage_offset = val - h.logical_minimum;
			usage = HID_USAGE(h.usage_minimum + usage_offset);
		}
		
		switch (page) {
		case HUP_GENERIC_DESKTOP:
			switch (usage) {
			case HUG_X:
				mouse_x = val;
				mevents ++;
				break;

			case HUG_Y:
				mouse_y = val;
				mevents ++;
				break;

			case HUG_WHEEL:
				mouse_z = -val;
				mevents ++;
				break;

			case HUG_SYSTEM_SLEEP:
				if (val)
					printf("SLEEP BUTTON PRESSED\n");
				break;
			}
			break;

		case HUP_KEYBOARD:
			kevents ++;
			printf("Key value %d\n", val);
#if 0
			if (h.flags & HIO_VARIABLE) {
				if (val && usage < kbd_maxkey())
					bit_set(s->keys1, usage);
			} else {
				if (val && val < kbd_maxkey())
					bit_set(s->keys1, val);

				for (i = 1; i < h.report_count; i++) {
					h.pos += h.report_size;
					val = hid_get_data(data, &h);
					if (val && val < kbd_maxkey())
						bit_set(s->keys1, val);
				}
			}
#endif			
			break;

		case HUP_BUTTON:
			if (usage != 0) {
				if (usage == 2)
					usage = 3;
				else if (usage == 3)
					usage = 2;
				
				mouse_butt |= (val << (usage - 1));
				mevents ++;
			}
			break;

		case HUP_CONSUMER:
			if (!val)
				break;

			switch (usage) {
			case HUC_AC_PAN:
				/* Horizontal scroll */
				if (val < 0)
					mouse_butt |= (1 << 5);
				else
					mouse_butt |= (1 << 6);

				mevents ++;
				val = 0;
				break;

			case 0xb5: /* Scan Next Track */
				val = 0x19;
				break;
				
			case 0xb6: /* Scan Previous Track */
				val = 0x10;
				break;
				
			case 0xb7: /* Stop */
				val = 0x24;
				break;
				
			case 0xcd: /* Play/Pause */
				val = 0x22;
				break;
				
			case 0xe2: /* Mute */
				val = 0x20;
				break;

			case 0xe9: /* Volume Up */
				val = 0x30;
				break;
				
			case 0xea: /* Volume Down */
				val = 0x2E;
				break;
				
			case 0x183: /* Media Select */
				val = 0x6D;
				break;
				
			case 0x018a: /* Mail */
				val = 0x6C;
				break;
				
			case 0x192: /* Calculator */
				val = 0x21;
				break;
				
			case 0x194: /* My Computer */
				val = 0x6B;
				break;
				
			case 0x221: /* WWW Search */
				val = 0x65;
				break;
				
			case 0x223: /* WWW Home */
				val = 0x32;
				break;

			case 0x224: /* WWW Back */
				val = 0x6A;
				break;

			case 0x225: /* WWW Forward */
				val = 0x69;
				break;

			case 0x226: /* WWW Stop */
				val = 0x68;
				break;

			case 0x227: /* WWW Refresh */
				val = 0x67;
				break;

			case 0x22a: /* WWW Favorites */
				val = 0x66;
				break;
				
			default:
				val = 0;
				break;
			}
			
			/* XXX FIXME - UGLY HACK */
			if (val != 0) {
				printf("Consumer Page:%d\n", val);
#if 0				
				if (hid_device->keyboard) {
					int32_t	buf[4] = { 0xe0, val,
							   0xe0, val|0x80 };
					
					assert(s->vkbd != -1);
					write(s->vkbd, buf, sizeof(buf));
				} else
					syslog(LOG_ERR, "Keyboard events " \
					       "received from non-keyboard " \
					       "device %s. Please report",
					       bt_ntoa(&s->bdaddr, NULL));
#endif							
			}

			break;
			
		case HUP_MICROSOFT:
			switch (usage) {
			case 0xfe01:
#if 0				
				if (!hid_device->battery_power)
					break;
#endif				
				printf("Battery value:%d\n", val);
			}
			break;
		}
	}
	hid_end_parse(d);
	if (mevents > 0) {
		struct mouse_info	mi;

		mi.operation = MOUSE_ACTION;
		mi.u.data.x = mouse_x;
		mi.u.data.y = mouse_y;
		mi.u.data.z = mouse_z;
		mi.u.data.buttons = mouse_butt;

		if (ioctl(serv->cons, CONS_MOUSECTL, &mi) < 0)
			fprintf(stderr, "%s %d\n",
				strerror(errno), errno);
	}

	
}
void hogp_notify(void *sc, int charid, unsigned char *buf, int len)
{

	int i;
	struct hogp_service *serv = sc;
	int rid;
	int page;
	rid = -1;
	for(i=0; i< serv->nrmap; i++){
		if(charid == serv->rmap[i].cid){
			rid = serv->rmap[i].rid;
			break;
		}
	}
	buf[1] = rid;
	hogp_process_report(serv, buf+1);
}
