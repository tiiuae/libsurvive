
static inline int update_feature_report(libusb_device_handle *dev, uint16_t interface, uint8_t *data, int datalen) {
	//	int xfer;
	//	int r = libusb_interrupt_transfer(dev, 0x01, data, datalen, &xfer, 1000);
	//	printf( "XFER: %d / R: %d\n", xfer, r );
	//	return xfer;
	return libusb_control_transfer(dev, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
								   0x09, 0x300 | data[0], interface, data, datalen, 1000);
}

void monitor_transfer(struct libusb_transfer *transfer) {
	assert(transfer->status == 0);
	libusb_free_transfer(transfer);
}

int libusb_control_transfer_async(libusb_device_handle *dev_handle, uint8_t bmRequestType, uint8_t bRequest,
								  uint16_t wValue, uint16_t wIndex, unsigned char *data, uint16_t wLength,
								  unsigned int timeout) {
	struct libusb_transfer *transfer = libusb_alloc_transfer(0);
	unsigned char *buffer;
	int r;
	if (!transfer)
		return LIBUSB_ERROR_NO_MEM;

	buffer = SV_MALLOC(LIBUSB_CONTROL_SETUP_SIZE + wLength);
	if (!buffer) {
		libusb_free_transfer(transfer);
		return LIBUSB_ERROR_NO_MEM;
	}
	libusb_fill_control_setup(buffer, bmRequestType, bRequest, wValue, wIndex, wLength);
	if ((bmRequestType & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_OUT)
		memcpy(buffer + LIBUSB_CONTROL_SETUP_SIZE, data, wLength);
	libusb_fill_control_transfer(transfer, dev_handle, buffer, monitor_transfer, dev_handle, timeout);
	transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
	r = libusb_submit_transfer(transfer);
	if (r < 0) {
		libusb_free_transfer(transfer);
		return r;
	}
	return wLength;
}

static inline int update_feature_report_async(libusb_device_handle *dev, uint16_t interface, uint8_t *data,
											  int datalen) {
	return libusb_control_transfer_async(dev,
										 LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT,
										 0x09, 0x300 | data[0], interface, data, datalen, 1000);
}

static inline int getupdate_feature_report(libusb_device_handle *dev, uint16_t interface, uint8_t *data, int datalen) {

	int ret = libusb_control_transfer(dev, LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN,
									  0x01, 0x300 | data[0], interface, data, datalen, 1000);
	if (ret == -9)
		return -9;
	if (ret < 0)
		return -1;
	return ret;
}

typedef libusb_device *survive_usb_device_t;
typedef libusb_device **survive_usb_devices_t;

static int survive_usb_subsystem_init(SurviveViveData *sv) {
	int rtn = libusb_init(&sv->usbctx);
#if LIBUSB_API_VERSION < 0x01000106
	libusb_set_debug(NULL, LIBUSB_LOG_LEVEL_WARNING);
#else
	libusb_set_option(NULL, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#endif
	return rtn;
}
static int survive_get_usb_devices(SurviveViveData *sv, survive_usb_devices_t *devs) {
	return libusb_get_device_list(sv->usbctx, devs);
}
static void survive_free_usb_devices(survive_usb_devices_t devs) { libusb_free_device_list(devs, 1); }

typedef int survive_usb_device_enumerator;
static survive_usb_device_t get_next_device(survive_usb_device_enumerator *iterator, survive_usb_devices_t list) {
	assert(iterator);
	return list[(*iterator)++];
}

int survive_vive_add_usb_device(SurviveViveData *sv, survive_usb_device_t d);
int libusb_hotplug(libusb_context *usbctx, libusb_device *device, libusb_hotplug_event event, void *user_data) {
	SurviveViveData *sv = user_data;
	SurviveContext *ctx = sv->ctx;

	if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
		survive_vive_add_usb_device(sv, device);
	}

	return 0;
}
static bool setup_hotplug(SurviveViveData *sv) {
	libusb_hotplug_register_callback(sv->usbctx, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT,
									 LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
									 LIBUSB_HOTPLUG_MATCH_ANY, libusb_hotplug, sv, 0);
	return 0;
}

static int survive_get_ids(survive_usb_device_t d, uint16_t *idVendor, uint16_t *idProduct, uint8_t *class_id) {
	struct libusb_device_descriptor desc;

	int ret = libusb_get_device_descriptor(d, &desc);
	*idVendor = 0;
	*idProduct = 0;
	if (class_id)
		*class_id = 0;
	if (ret)
		return ret;

	*idVendor = desc.idVendor;
	*idProduct = desc.idProduct;
	if (class_id)
		*class_id = desc.bDeviceClass;

	return ret;
}

static const char *survive_usb_error_name(int ret) { return libusb_error_name(ret); }

static int survive_open_usb_device(SurviveViveData *sv, survive_usb_device_t d, struct SurviveUSBInfo *usbInfo) {
	struct libusb_config_descriptor *conf;
	int ret = libusb_get_config_descriptor(d, 0, &conf);
	if (ret)
		goto cleanup_and_rtn;

	const struct DeviceInfo *info = usbInfo->device_info;
	ret = libusb_open(d, &usbInfo->handle);

	uint16_t idVendor;
	uint16_t idProduct;
	survive_get_ids(d, &idVendor, &idProduct, 0);

	SurviveContext *ctx = sv->ctx;
	if (!usbInfo->handle || ret) {
		SV_WARN("Error: cannot open device \"%s\" with vid/pid %04x:%04x error %d (%s)", survive_colorize(info->name),
				idVendor, idProduct, ret, libusb_error_name(ret));
		goto cleanup_and_rtn;
	}

	libusb_set_auto_detach_kernel_driver(usbInfo->handle, 1);
	for (int j = 0; j < conf->bNumInterfaces; j++) {
		ret = libusb_claim_interface(usbInfo->handle, j);
		if (ret != 0) {
			SV_WARN("Could not claim interface %d of %s: %d %s", j, survive_colorize(info->name), ret,
					libusb_error_name(ret));
			goto cleanup_and_rtn;
		}
	}

	SV_VERBOSE(40, "Successfully enumerated %s (%d) %04x:%04x", survive_colorize(info->name), conf->bNumInterfaces,
			   idVendor, idProduct);

	usleep(100000);

cleanup_and_rtn:
	libusb_free_config_descriptor(conf);
	return ret;
}

static void handle_transfer(struct libusb_transfer *transfer) {
	uint64_t time = OGGetAbsoluteTimeUS();

	SurviveUSBInterface *iface = transfer->user_data;
	if (iface->shutdown) {
		SurviveContext *ctx = iface->ctx;
		SV_VERBOSE(100, "Cleaning up transfer on %d %s", iface->which_interface_am_i, survive_colorize(iface->hname));
		iface->ctx = 0;
		return;
	}

	SurviveContext *ctx = iface->ctx;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		SV_ERROR(SURVIVE_ERROR_HARWARE_FAULT, "Transfer problem %s %d with %s", libusb_error_name(transfer->status),
				 transfer->status, iface->hname);
		return;
	}

	iface->actual_len = transfer->actual_length;

	iface->cb(time, iface);
	iface->packet_count++;

	if (libusb_submit_transfer(transfer)) {
		SV_ERROR(SURVIVE_ERROR_HARWARE_FAULT, "Error resubmitting transfer for %s", iface->hname);
	}
}

struct survive_config_packet {
	SurviveContext* ctx;
	SurviveViveData *sv;
	struct SurviveUSBInfo *usbInfo;
	const struct Magic_t *current_magic;

	uint8_t buffer[256];

	cstring cfg;
	double start_time;

	enum {
		SURVIVE_CONFIG_STATE_CONFIG,
		SURVIVE_CONFIG_STATE_VERSION,
		SURVIVE_CONFIG_STATE_MAGICS,
		SURVIVE_CONFIG_STATE_DONE
	} state;
	uint16_t stall_counter;
	struct libusb_transfer *tx;
};

static int survive_config_submit(struct SurviveUSBInfo *usbInfo, int iface) {
	struct survive_config_packet *config_packet = usbInfo->cfg_user;
	SurviveContext *ctx = config_packet->sv->ctx;

	SV_VERBOSE(650, "Submitting config for %s %s at %f",
			   survive_colorize(usbInfo->so ? usbInfo->so->codename : usbInfo->device_info->codename),
			   survive_colorize(usbInfo->device_info->name), usbInfo->nextCfgSubmitTime);
	usbInfo->nextCfgSubmitTime = 0;
	assert(config_packet->tx);
	int rc = libusb_submit_transfer(config_packet->tx);
	if (rc) {
		return -6;
	}
	return 0;
}

static inline void survive_close_usb_device(struct SurviveUSBInfo *usbInfo) {
	for (size_t j = 0; j < usbInfo->interface_cnt; j++) {
		usbInfo->interfaces[j].shutdown = 1;
	}
	SurviveContext *ctx = usbInfo->viveData->ctx;
	if (usbInfo->nextCfgSubmitTime > 0) {
		survive_config_submit(usbInfo, 0);
	}

	struct survive_config_packet *cfg = usbInfo->cfg_user;
	if (cfg) {
		libusb_cancel_transfer(cfg->tx);
	}
	for (int j = 0; j < usbInfo->interface_cnt; j++) {
		SurviveUSBInterface *iface = &usbInfo->interfaces[j];
		SV_INFO("Cleaning up interface on %d %s", iface->which_interface_am_i, survive_colorize(iface->hname));
		libusb_cancel_transfer(usbInfo->interfaces[j].transfer);
		while (usbInfo->interfaces[j].ctx || usbInfo->cfg_user) {
			survive_release_ctx_lock(ctx);
			libusb_handle_events(usbInfo->viveData->usbctx);
			survive_get_ctx_lock(ctx);
		}
		libusb_release_interface(usbInfo->handle, usbInfo->interfaces[j].which_interface_am_i);
		libusb_free_transfer(usbInfo->interfaces[j].transfer);
	}

	libusb_close(usbInfo->handle);
}

void survive_usb_close(SurviveViveData *sv) { libusb_exit(sv->usbctx); }

static inline void setup_config_req(struct survive_config_packet *config_packet) {
	config_packet->state = SURVIVE_CONFIG_STATE_CONFIG;
	config_packet->buffer[8] = VIVE_REPORT_CONFIG_READMODE;
	libusb_fill_control_setup(config_packet->buffer,
							  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN, 0x01,
							  0x300 | config_packet->buffer[8], 0, sizeof(config_packet->buffer));
}
static inline void setup_version(struct survive_config_packet *packet) {
	struct libusb_transfer *transfer = packet->tx;
	packet->state = SURVIVE_CONFIG_STATE_VERSION;
	transfer->buffer[8] = VIVE_REPORT_VERSION;
	libusb_fill_control_setup(transfer->buffer,
							  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN, 0x01,
							  0x300 | transfer->buffer[8], 0, 256);
}
static inline void setup_magic(struct survive_config_packet *packet) {
	SurviveContext *ctx = packet->ctx;
	SurviveObject *so = packet->usbInfo->so;

	packet->state = SURVIVE_CONFIG_STATE_MAGICS;

	SV_VERBOSE(100, "Submitting magic %s at %f sec for %s - %s", survive_colorize(packet->current_magic->name),
			   OGGetAbsoluteTime() - packet->start_time, survive_colorize(so ? so->codename : "BRD"),
			   survive_colorize(packet->usbInfo->device_info->name));
	memcpy(packet->buffer + 8, packet->current_magic->magic, packet->current_magic->length);
	libusb_fill_control_setup(packet->buffer,
							  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_OUT, 0x09,
							  0x300 | packet->buffer[8], 0, packet->current_magic->length + 8);
	do {
		packet->current_magic++;
	} while (packet->current_magic->magic && packet->current_magic->code == 0);
}
void handle_config_tx(struct libusb_transfer *transfer);
static inline void setup_packet_state(struct survive_config_packet *packet) {
	switch (packet->state) {
	case SURVIVE_CONFIG_STATE_MAGICS:
		setup_magic(packet);
		break;
	case SURVIVE_CONFIG_STATE_VERSION:
		setup_version(packet);
		break;
	case SURVIVE_CONFIG_STATE_CONFIG:
		setup_config_req(packet);
		break;
	case SURVIVE_CONFIG_STATE_DONE:
		return;
	}
	libusb_fill_control_transfer(packet->tx, packet->usbInfo->handle, packet->buffer, handle_config_tx, packet, 1000);
}
void handle_config_tx(struct libusb_transfer *transfer) {
	struct survive_config_packet *packet = transfer->user_data;
	SurviveContext *ctx = packet->ctx;

	SurviveObject *so = packet->usbInfo->so;
	uint8_t cmd = transfer->buffer[8];

	if (packet->usbInfo->interfaces[0].shutdown) {
		goto cleanup;
	}

	if (transfer->status == LIBUSB_TRANSFER_STALL) {
		SV_VERBOSE(650, "Waiting, Transfer status %d at %f sec for %s", transfer->status,
				   OGGetAbsoluteTime() - packet->start_time, survive_colorize(so ? so->codename : "unknown"));

		if (packet->usbInfo->device_info->codename[0] == 0) {
			goto cleanup;
		}

		packet->usbInfo->nextCfgSubmitTime = OGGetAbsoluteTime() + .02;
		return;
	}
	packet->stall_counter = 0;

	if (transfer->status != LIBUSB_TRANSFER_COMPLETED) {
		SV_WARN("Transfer status %d at %f sec for %s", transfer->status, OGGetAbsoluteTime() - packet->start_time,
				survive_colorize(so ? so->codename : "unknown"));
		goto cleanup;
	}

	switch (packet->state) {
	case SURVIVE_CONFIG_STATE_MAGICS:
		if (!packet->current_magic->magic) {
			SV_VERBOSE(100, "Magics done in %f sec for %s %s", OGGetAbsoluteTime() - packet->start_time,
					   survive_colorize(so->codename), survive_colorize(packet->usbInfo->device_info->name));
			packet->usbInfo->lightcapMode = LightcapMode_raw0;
			goto setup_next;
		}

		setup_magic(packet);
		goto resubmit;
	case SURVIVE_CONFIG_STATE_CONFIG:
		switch (cmd) {
		case VIVE_REPORT_CONFIG_READMODE:
			transfer->buffer[8] = VIVE_REPORT_CONFIG_READ;
			transfer->buffer[8 + 1] = 0xaa;
			libusb_fill_control_setup(transfer->buffer,
									  LIBUSB_REQUEST_TYPE_CLASS | LIBUSB_RECIPIENT_INTERFACE | LIBUSB_ENDPOINT_IN, 0x01,
									  0x300 | transfer->buffer[8], 0, 256);
			goto resubmit;
		case VIVE_REPORT_CONFIG_READ: {
			size_t size = transfer->buffer[1 + 8];
			str_append_n(&packet->cfg, (const char *)&transfer->buffer[2 + 8], size);
			if (size == 0) {
				SV_VERBOSE(100, "Config done in %f sec for %s", OGGetAbsoluteTime() - packet->start_time,
						   survive_colorize(so->codename));

				uint8_t uncompressed_data[65536];
				int uncompressed_data_len = survive_simple_inflate(ctx, (uint8_t *)packet->cfg.d, packet->cfg.length,
																   uncompressed_data, sizeof(uncompressed_data) - 1);
				if (uncompressed_data_len < 0) {
					SV_ERROR(SURVIVE_ERROR_INVALID_CONFIG, "Invalid config for %s", survive_colorize(so->codename));
					goto cleanup;
				}
				packet->usbInfo->so->conf = SV_CALLOC(1, uncompressed_data_len + 1);
				packet->usbInfo->so->conf_cnt = uncompressed_data_len;
				memcpy(packet->usbInfo->so->conf, uncompressed_data, uncompressed_data_len);
				goto setup_next;
			}
			goto resubmit;
		} break;
		}
	case SURVIVE_CONFIG_STATE_VERSION: {
		parse_tracker_version_info(packet->usbInfo->so, &transfer->buffer[1 + 8], transfer->actual_length);
		ctx->configproc(so, so->conf, so->conf_cnt);
		SV_VERBOSE(100, "Version done in %f sec for %s", OGGetAbsoluteTime() - packet->start_time,
				   survive_colorize(so->codename));

		goto setup_next;
	}
	default:
		SV_WARN("Config state matchine saw packet of type %d; not sure how to proceed.", cmd);
		goto cleanup;
	}
	return;

setup_next : {
	packet->state++;
	setup_packet_state(packet);
	if (packet->state == SURVIVE_CONFIG_STATE_DONE)
		goto cleanup;
	else
		goto resubmit;
};
	resubmit: {
		int submit_transfer_error = survive_config_submit(packet->usbInfo, 0);
		if (submit_transfer_error != 0) {
			SV_WARN("Config state machine could not submit transfer %d\n", submit_transfer_error);
			goto cleanup;
		}
	return;
}
	cleanup:
		SV_VERBOSE(100, "Cleanup config for %s at %f", survive_colorize(packet->usbInfo->so->codename),
				   packet->usbInfo->nextCfgSubmitTime);
		packet->usbInfo->nextCfgSubmitTime = 0;
		packet->usbInfo->cfg_user = 0;
		free(packet);
		libusb_free_transfer(transfer);
}

static int survive_start_get_config(SurviveViveData *sv, struct SurviveUSBInfo *usbInfo, int iface) {
	struct libusb_transfer *tx = libusb_alloc_transfer(0);
	if (!tx) {
		return -4;
	}

	SurviveContext *ctx = sv->ctx;

	struct survive_config_packet *config_packet = SV_CALLOC(1, sizeof(struct survive_config_packet));
	usbInfo->cfg_user = config_packet;
	config_packet->tx = tx;

	config_packet->ctx = ctx;
	config_packet->sv = sv;
	config_packet->usbInfo = usbInfo;
	config_packet->current_magic = config_packet->usbInfo->device_info->magics;
	config_packet->state = SURVIVE_CONFIG_STATE_CONFIG;

	USBHANDLE dev = usbInfo->handle;
	config_packet->start_time = OGGetAbsoluteTime();

	if (config_packet->usbInfo->device_info->codename[0] == 0) {
		config_packet->state = SURVIVE_CONFIG_STATE_MAGICS;
	}
	setup_packet_state(config_packet);

	SV_VERBOSE(100, "Requesting config for %s %p %d",
			   survive_colorize(usbInfo->so ? usbInfo->so->codename : usbInfo->device_info->name), (void *)tx,
			   config_packet->state);

	usbInfo->nextCfgSubmitTime = OGGetAbsoluteTime();

	return 0;
}
