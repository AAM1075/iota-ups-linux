#include <linux/power_supply.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/hid.h>

#define IOTA_UPS_VENDOR_ID  0x2341
#define IOTA_UPS_PRODUCT_ID 0x8036

#define REPORT_ID_CAPACITY 0x0C
#define REPORT_ID_STATUS   0x07

#define STATUS_PLUGGED_IN  BIT(0)
#define STATUS_DISCHARGING BIT(1)
#define STATUS_CHARGING    BIT(2)

MODULE_AUTHOR("Andrew Maney");
MODULE_DESCRIPTION("LattePanda IOTA UPS power supply driver");
MODULE_LICENSE("GPL");

struct iota_ups {
    struct hid_device        *hiddev;
    struct power_supply      *psu;
    struct power_supply_desc psu_desc;
    spinlock_t lock;

    // These values are updated via HID reports
    bool plugged_in;
    int psu_status;
    int capacity;
    int charge_limit;

    // Wait for both reports (status and capacity) before registering
    struct completion got_initial_data;
    bool data_ready;
    bool got_status;
    bool got_capacity;
};

static enum power_supply_property iota_ups_properties[] = {
    POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_SCOPE,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD,
	POWER_SUPPLY_PROP_CAPACITY_LEVEL,
};

static const struct hid_device_id iota_ups_devices[] = {
    { HID_USB_DEVICE(IOTA_UPS_VENDOR_ID, IOTA_UPS_PRODUCT_ID) },
	{ }
};
MODULE_DEVICE_TABLE(hid, iota_ups_devices);

/* === PSU callbacks === */
static int iota_ups_get_property(struct power_supply *supply, enum power_supply_property supply_property, union power_supply_propval* property_value)
{
    struct iota_ups* ups = power_supply_get_drvdata(supply);
    unsigned long flags;

    spin_lock_irqsave(&ups->lock, flags);

    // Update the requested property
    switch(supply_property) {
        case POWER_SUPPLY_PROP_STATUS: // Charging | Discharging | Plugged in
            property_value->intval = ups->psu_status;
            break;

        case POWER_SUPPLY_PROP_CAPACITY: 
            property_value->intval = ups->capacity;
            break;

        case POWER_SUPPLY_PROP_PRESENT:
            property_value->intval = 1;
            break;

        case POWER_SUPPLY_PROP_ONLINE: // Is the UPS charging?
            property_value->intval = ups->plugged_in ? 1 : 0;
            break;

        case POWER_SUPPLY_PROP_SCOPE:
            property_value->intval = POWER_SUPPLY_SCOPE_SYSTEM;
            break;

        case POWER_SUPPLY_PROP_TECHNOLOGY: // V1.0 of the UPS board only accepts 18650 Li-ion cells
            property_value->intval = POWER_SUPPLY_TECHNOLOGY_LION;
            break;

        case POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD: // At what percentage should the battery stop charging?
            property_value->intval = ups->charge_limit;
            break;

        case POWER_SUPPLY_PROP_CAPACITY_LEVEL: // V1.0 ofthe UPS board does not report its capacity via HID-UPS
            property_value->intval = POWER_SUPPLY_CAPACITY_LEVEL_NORMAL;
            break;

        default:
            spin_unlock_irqrestore(&ups->lock, flags);
            return -EINVAL;
    }

    spin_unlock_irqrestore(&ups->lock, flags);
    return 0;
}

static int iota_ups_set_property(struct power_supply *supply, enum power_supply_property supply_property, const union power_supply_propval* property_value)
{
    struct iota_ups* ups = power_supply_get_drvdata(supply);

    // We will only handle the maximum charge capacity here
    if (supply_property == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD) {
        int charge_limit = property_value->intval;

        // V1.0 of the UPS board only supports 80% and 100% charge limits; this is configured via a DIP switch on the physical board, not via software
        // This is purely for visual purposes at the moment
        if (charge_limit != 80 && charge_limit != 100) return -EINVAL;

        ups->charge_limit = charge_limit;
        return 0;
    }

    return -EINVAL;
}

static int iota_ups_property_is_writable (struct power_supply *supply, enum power_supply_property supply_property)
{
    // Only the charge limit is writable at the moment
    return supply_property == POWER_SUPPLY_PROP_CHARGE_CONTROL_END_THRESHOLD;
}

/* === HID callbacks === */
static int iota_ups_raw_event(struct hid_device* hdev, struct hid_report* report, u8* data, int size)
{
    struct iota_ups* ups = hid_get_drvdata(hdev);
    unsigned long flags;
    bool data_changed = false;

    // All of the IOTA's UPS data reports are at least 2 bytes
    if (size < 2) return 0;

    spin_lock_irqsave(&ups->lock, flags);

    // The first byte of the data tells us whether the UPS is reporting a STATUS or CAPACITY event
    switch(data[0]) {
        case REPORT_ID_STATUS: {
            int new_status;
            u8 status = data[1];
            bool plugged_in = !!(status & STATUS_PLUGGED_IN);

            if (status & STATUS_CHARGING) {
                if (ups->capacity >= ups->charge_limit) new_status = POWER_SUPPLY_STATUS_FULL;
                else new_status = POWER_SUPPLY_STATUS_CHARGING;
            }
            else if (status & STATUS_DISCHARGING) {
                new_status = POWER_SUPPLY_STATUS_DISCHARGING;
            }
            else if (plugged_in) new_status = POWER_SUPPLY_STATUS_FULL;
            else new_status = POWER_SUPPLY_STATUS_UNKNOWN;

            if (new_status != ups->psu_status || plugged_in != ups->plugged_in) {
                ups->plugged_in = plugged_in;
                ups->psu_status = new_status;
                data_changed = true;
            }

            ups->got_status = true;
            break;
        }

        case REPORT_ID_CAPACITY: {
            int new_capacity = clamp((int)data[1], 0, 100);

            if (new_capacity != ups->capacity) {
                ups->capacity = new_capacity;
                data_changed = true;
            }

            ups->got_capacity = true;
            break;
        }
    }

    // We should only signal once we have have received status AND capacity reports, this ensures that we register with valid data
    if (!ups->data_ready && ups->got_status && ups->got_capacity) {
        ups->data_ready = true;
        complete(&ups->got_initial_data);
    }

    spin_unlock_irqrestore(&ups->lock, flags);

    // Notify the power_supply core outside of the spinlock, this triggers UPower's PropertiesChanged signal with the new data
    if (data_changed && ups->psu) power_supply_changed(ups->psu);
    return 0;
}

static int iota_ups_probe(struct hid_device* hdev, const struct hid_device_id* device_ID)
{
    struct iota_ups* ups;
    struct power_supply_config psu_config = {};
    int ret;

    ups = devm_kzalloc(&hdev->dev,  sizeof(*ups), GFP_KERNEL);
    if (!ups) return -ENOMEM;

    ups->hiddev       = hdev;
	ups->psu_status   = POWER_SUPPLY_STATUS_UNKNOWN;
	ups->capacity     = 50;
	ups->charge_limit = 100; // V1.0 of the UPS comes with 80% set as the default, but we will assume 100% since there is no way to know for sure at this point
	ups->data_ready   = false;
	ups->got_status   = false;
	ups->got_capacity = false;

    init_completion(&ups->got_initial_data);
    spin_lock_init(&ups->lock);
    hid_set_drvdata(hdev, ups);

    // Initialize the UPS as a HID device
    ret = hid_parse(hdev);

    if (ret) {
        hid_err(hdev, "HID parse failed with error %d\n", ret);
        return ret;
    }

    ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
    if (ret) {
        hid_err(hdev, "HID HW start failed with error %d\n", ret);
        return ret;
    }

    ret = hid_hw_open(hdev);
    if (ret) {
        hid_err(hdev, "HID HW open failed with error %d\n", ret);
        goto err_stop;
    }

    // We should wait until we have have received status AND capacity reports, this ensures that we register with valid data
    // The UPS sends reports every ~1 second, so 3 seconds is a safe value that covers both reports
    wait_for_completion_timeout(&ups->got_initial_data, msecs_to_jiffies(3000));

    // Now we need to set up the UPS's properties and register it
    ups->psu_desc.name                  = "lattepanda-iota-ups";
	ups->psu_desc.type                  = POWER_SUPPLY_TYPE_BATTERY;
	ups->psu_desc.properties            = iota_ups_properties;
	ups->psu_desc.num_properties        = ARRAY_SIZE(iota_ups_properties);
	ups->psu_desc.get_property          = iota_ups_get_property;
	ups->psu_desc.set_property          = iota_ups_set_property;
	ups->psu_desc.property_is_writeable = iota_ups_property_is_writable;
    psu_config.drv_data = ups;

    ups->psu = devm_power_supply_register(&hdev->dev, &ups->psu_desc, &psu_config);
    if (IS_ERR(ups->psu)) {
        ret = PTR_ERR(ups->psu);
        hid_err(hdev, "Failed to register the UPS as power_supply, got error %d\n", ret);
        goto err_close;
    }

    // We need to force an immediate notification so apps like UPower read the correct initial state right after we register, rather than having to wait until the next change report
    power_supply_changed(ups->psu);

    // Initialization is done
    hid_info(hdev, "LattePanda IOTA UPS is now registered as powersupply.\n");
    return 0;

    err_close:
        hid_hw_close(hdev);

    err_stop:
        hid_hw_stop(hdev);
        return ret;
}

static void iota_ups_remove(struct hid_device* hdev)
{
    hid_hw_close(hdev);
    hid_hw_stop(hdev);
}

static struct hid_driver iota_ups_driver = {
	.name      = "lattepanda-iota-ups",
	.id_table  = iota_ups_devices,
	.probe     = iota_ups_probe,
	.remove    = iota_ups_remove,
	.raw_event = iota_ups_raw_event,
};
module_hid_driver(iota_ups_driver);