/*
 * Copyright (c) 2023 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/sys/util_macro.h>
#include <string.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

const struct device *zmk_behavior_get_binding(const char *name) {
    return behavior_get_binding(name);
}

const struct device *z_impl_behavior_get_binding(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (z_device_is_ready(item->device) && item->device->name == name) {
            return item->device;
        }
    }

    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (z_device_is_ready(item->device) && strcmp(item->device->name, name) == 0) {
            return item->device;
        }
    }

    return NULL;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
static int validate_hid_usage(uint16_t usage_page, uint16_t usage_id) {
    LOG_DBG("Validate usage %d in page %d", usage_id, usage_page);
    switch (usage_page) {
    case HID_USAGE_KEY:
        if (usage_id == 0 || usage_id > ZMK_HID_KEYBOARD_NKRO_MAX_USAGE) {
            return -EINVAL;
        }
        break;
    case HID_USAGE_CONSUMER:
        if (usage_id >
            COND_CODE_1(IS_ENABLED(CONFIG_ZMK_HID_CONSUMER_REPORT_USAGES_BASIC), (0xFF), (0xFFF))) {
            return -EINVAL;
        }
        break;
    default:
        LOG_WRN("Unsupported HID usage page %d", usage_page);
        return -EINVAL;
    }

    return 0;
}

static int validate_standard_param(enum behavior_parameter_standard_domain standard_domain,
                                   uint32_t val) {
    switch (standard_domain) {
    case BEHAVIOR_PARAMETER_STANDARD_DOMAIN_NULL:
        if (val != 0) {
            return -EINVAL;
        }
        break;
    case BEHAVIOR_PARAMETER_STANDARD_DOMAIN_HID_USAGE:
        return validate_hid_usage(ZMK_HID_USAGE_PAGE(val), ZMK_HID_USAGE_ID(val));
    case BEHAVIOR_PARAMETER_STANDARD_DOMAIN_LAYER_INDEX:
        if (val >= ZMK_KEYMAP_LEN) {
            return -EINVAL;
        }
        break;
    case BEHAVIOR_PARAMETER_STANDARD_DOMAIN_HSV:
        // TODO: No real way to validate? Maybe max brightness?
        break;
    }

    return 0;
}

static int validate_custom_params(const struct behavior_parameter_metadata_custom *custom,
                                  uint32_t param1, uint32_t param2) {
    if (!custom) {
        return -ENODEV;
    }

    for (int s = 0; s < custom->sets_len; s++) {
        const struct behavior_parameter_metadata_custom_set *set = &custom->sets[s];

        bool had_param1_metadata = false, had_param2_metadata = false;
        bool param1_matched = false, param2_matched = false;

        for (int v = 0; v < set->values_len && (!param1_matched || !param2_matched); v++) {
            const struct behavior_parameter_value_metadata *value_meta = &set->values[v];
            uint32_t param = value_meta->position == 0 ? param1 : param2;
            bool *matched = value_meta->position == 0 ? &param1_matched : &param2_matched;

            *(value_meta->position == 0 ? &had_param1_metadata : &had_param2_metadata) = true;

            switch (value_meta->type) {
            case BEHAVIOR_PARAMETER_VALUE_METADATA_TYPE_STANDARD:
                if (validate_standard_param(value_meta->standard, param) == 0) {
                    *matched = true;
                }
                break;
            case BEHAVIOR_PARAMETER_VALUE_METADATA_TYPE_VALUE:
                if (param == value_meta->value) {
                    *matched = true;
                }
                break;
            case BEHAVIOR_PARAMETER_VALUE_METADATA_TYPE_RANGE:
                if (param >= value_meta->range.min && param <= value_meta->range.max) {
                    *matched = true;
                }
                break;
            }
        }

        if ((param1_matched || (!had_param1_metadata && param1 == 0)) &&
            (param2_matched || (!had_param2_metadata && param2 == 0))) {
            return 0;
        }
    }

    return -EINVAL;
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

int zmk_behavior_validate_binding(const struct zmk_behavior_binding *binding) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    const struct device *behavior = zmk_behavior_get_binding(binding->behavior_dev);

    if (!behavior) {
        return -ENODEV;
    }

    struct behavior_parameter_metadata metadata;
    int ret = behavior_get_parameter_domains(behavior, &metadata);

    if (ret < 0) {
        return ret;
    }

    switch (metadata.type) {
    case BEHAVIOR_PARAMETER_METADATA_STANDARD:
        int ret = validate_standard_param(metadata.standard.param1, binding->param1);

        if (ret < 0) {
            return ret;
        }

        return validate_standard_param(metadata.standard.param2, binding->param2);
    case BEHAVIOR_PARAMETER_METADATA_CUSTOM:
        return validate_custom_params(metadata.custom, binding->param1, binding->param2);
    default:
        return -ENOTSUP;
    }
#else
    return 0;
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
}

#if IS_ENABLED(CONFIG_LOG)
static int check_behavior_names(void) {
    // Behavior names must be unique, but we don't have a good way to enforce this
    // at compile time, so log an error at runtime if they aren't unique.
    ptrdiff_t count;
    STRUCT_SECTION_COUNT(zmk_behavior_ref, &count);

    for (ptrdiff_t i = 0; i < count; i++) {
        const struct zmk_behavior_ref *current;
        STRUCT_SECTION_GET(zmk_behavior_ref, i, &current);

        for (ptrdiff_t j = i + 1; j < count; j++) {
            const struct zmk_behavior_ref *other;
            STRUCT_SECTION_GET(zmk_behavior_ref, j, &other);

            if (strcmp(current->device->name, other->device->name) == 0) {
                LOG_ERR("Multiple behaviors have the same name '%s'", current->device->name);
            }
        }
    }

    return 0;
}

SYS_INIT(check_behavior_names, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
#endif // IS_ENABLED(CONFIG_LOG)
