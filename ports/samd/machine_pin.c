/*
 * This is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2016-2021 Damien P. George
 * Copyright (c) 2022 Robert Hammelrath (pin.irq)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Uses pins.h & pins.c to create board (MCU package) specific 'machine_pin_obj' array.
 */

#include "string.h"
#include "py/runtime.h"
#include "py/mphal.h"
#include "shared/runtime/mpirq.h"
#include "extmod/virtpin.h"
#include "modmachine.h"
#include "samd_soc.h"
#include "pins.h"
#include "pin_af.h"

#include "hal_gpio.h"

#define GPIO_MODE_IN (0)
#define GPIO_MODE_OUT (1)
#define GPIO_MODE_OPEN_DRAIN (2)

#define GPIO_STRENGTH_2MA (0)
#define GPIO_STRENGTH_8MA (1)
#define GPIO_IRQ_EDGE_RISE (1)
#define GPIO_IRQ_EDGE_FALL (2)

typedef struct _machine_pin_irq_obj_t {
    mp_irq_obj_t base;
    uint32_t flags;
    uint32_t trigger;
    uint8_t pin_id;
} machine_pin_irq_obj_t;

STATIC const mp_irq_methods_t machine_pin_irq_methods;

uint32_t machine_pin_open_drain_mask[4];

// Open drain behaviour is simulated.
#define GPIO_IS_OPEN_DRAIN(id) (machine_pin_open_drain_mask[id / 32] & (1 << (id % 32)))

STATIC void machine_pin_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    machine_pin_obj_t *self = self_in;
    char *mode_str;
    char *pull_str[] = {"PULL_OFF", "PULL_UP", "PULL_DOWN"};
    if (GPIO_IS_OPEN_DRAIN(self->id)) {
        mode_str = "OPEN_DRAIN";
    } else {
        mode_str = (mp_hal_get_pin_direction(self->id) == GPIO_DIRECTION_OUT) ? "OUT" : "IN";
    }

    mp_printf(print, "Pin(\"%s\", mode=%s, pull=%s, GPIO=P%c%02u)",
        self->name,
        mode_str,
        pull_str[mp_hal_get_pull_mode(self->id)],
        "ABCD"[self->id / 32], self->id % 32);
}

STATIC void pin_validate_drive(bool strength) {
    if (strength != GPIO_STRENGTH_2MA && strength != GPIO_STRENGTH_8MA) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid argument(s) value"));
    }
}

int pin_find(mp_obj_t pin, const machine_pin_obj_t machine_pin_obj[], int table_size) {
    int wanted_pin = -1;
    if (mp_obj_is_small_int(pin)) {
        // Pin defined by the index of pin table
        wanted_pin = mp_obj_get_int(pin);
    } else if (mp_obj_is_str(pin)) {
        // Search by name
        size_t slen;
        const char *s = mp_obj_str_get_data(pin, &slen);
        for (wanted_pin = 0; wanted_pin < table_size; wanted_pin++) {
            if (slen == strlen(machine_pin_obj[wanted_pin].name) &&
                strncmp(s, machine_pin_obj[wanted_pin].name, slen) == 0) {
                break;
            }
        }
    }
    return wanted_pin;
}

// Pin.init(mode, pull=None, *, value=None, drive=0). No 'alt' yet.
STATIC mp_obj_t machine_pin_obj_init_helper(const machine_pin_obj_t *self, size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_mode, ARG_pull, ARG_value, ARG_drive, ARG_alt };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_mode, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
        { MP_QSTR_pull, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
        { MP_QSTR_value, MP_ARG_KW_ONLY | MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE}},
        { MP_QSTR_drive, MP_ARG_KW_ONLY | MP_ARG_INT, {.u_int = GPIO_STRENGTH_2MA} },
    };

    // parse args
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args, pos_args, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // set initial value (do this before configuring mode/pull)
    if (args[ARG_value].u_obj != mp_const_none) {
        mp_hal_pin_write(self->id, mp_obj_is_true(args[ARG_value].u_obj));
    }

    // configure mode
    if (args[ARG_mode].u_obj != mp_const_none) {
        mp_int_t mode = mp_obj_get_int(args[ARG_mode].u_obj);
        if (mode == GPIO_MODE_IN) {
            mp_hal_pin_input(self->id);
        } else if (mode == GPIO_MODE_OUT) {
            mp_hal_pin_output(self->id);
        } else if (mode == GPIO_MODE_OPEN_DRAIN) {
            mp_hal_pin_open_drain(self->id);
        } else {
            mp_hal_pin_input(self->id); // If no args are given, the Pin is 'input'.
        }
    }
    // configure pull. Only to be used with IN mode. The function sets the pin to INPUT.
    uint32_t pull = 0;
    mp_int_t dir = mp_hal_get_pin_direction(self->id);
    if (dir == GPIO_DIRECTION_OUT && args[ARG_pull].u_obj != mp_const_none) {
        mp_raise_ValueError(MP_ERROR_TEXT("OUT incompatible with pull"));
    } else if (args[ARG_pull].u_obj != mp_const_none) {
        pull = mp_obj_get_int(args[ARG_pull].u_obj);
        gpio_set_pin_pull_mode(self->id, pull); // hal_gpio.h
    }

    // get the strength
    bool strength = args[3].u_int;
    pin_validate_drive(strength);

    return mp_const_none;
}

// constructor(id, ...)
mp_obj_t mp_pin_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, MP_OBJ_FUN_ARGS_MAX, true);

    // get the wanted pin object
    int wanted_pin = pin_find(args[0], machine_pin_obj, MP_ARRAY_SIZE(machine_pin_obj));

    const machine_pin_obj_t *self = NULL;
    if (0 <= wanted_pin && wanted_pin < MP_ARRAY_SIZE(machine_pin_obj)) {
        self = (machine_pin_obj_t *)&machine_pin_obj[wanted_pin];
    }

    if (self == NULL || self->base.type == NULL) {
        mp_raise_ValueError(MP_ERROR_TEXT("invalid pin"));
    }

    if (n_args > 1 || n_kw > 0) {
        // pin mode given, so configure this GPIO
        mp_map_t kw_args;
        mp_map_init_fixed_table(&kw_args, n_kw, args + n_args);
        machine_pin_obj_init_helper(self, n_args - 1, args + 1, &kw_args);
    }

    return MP_OBJ_FROM_PTR(self);
}

// fast method for getting/setting pin value
mp_obj_t machine_pin_call(mp_obj_t self_in, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 1, false);
    machine_pin_obj_t *self = self_in;
    if (n_args == 0) {
        // get pin
        return MP_OBJ_NEW_SMALL_INT(mp_hal_pin_read(self->id));
    } else {
        // set pin
        bool value = mp_obj_is_true(args[0]);
        if (GPIO_IS_OPEN_DRAIN(self->id)) {
            if (value == 0) {
                mp_hal_pin_od_low(self->id);
            } else {
                mp_hal_pin_od_high(self->id);
            }
        } else {
            mp_hal_pin_write(self->id, value);
        }
        return mp_const_none;
    }
}

// Pin.init(mode, pull)
STATIC mp_obj_t machine_pin_obj_init(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    return machine_pin_obj_init_helper(args[0], n_args - 1, args + 1, kw_args);
}
MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_init_obj, 1, machine_pin_obj_init);

// Pin.value([value])
mp_obj_t machine_pin_value(size_t n_args, const mp_obj_t *args) {
    return machine_pin_call(args[0], n_args - 1, 0, args + 1);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_value_obj, 1, 2, machine_pin_value);

// Pin.disable(pin)
STATIC mp_obj_t machine_pin_disable(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    gpio_set_pin_direction(self->id, GPIO_DIRECTION_OFF); // Disables the pin (low power state)
    return mp_const_none;
}
STATIC MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_disable_obj, machine_pin_disable);

// Pin.low() Totem-pole (push-pull)
STATIC mp_obj_t machine_pin_low(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (GPIO_IS_OPEN_DRAIN(self->id)) {
        mp_hal_pin_od_low(self->id);
    } else {
        mp_hal_pin_low(self->id);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_low_obj, machine_pin_low);

// Pin.high() Totem-pole (push-pull)
STATIC mp_obj_t machine_pin_high(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (GPIO_IS_OPEN_DRAIN(self->id)) {
        mp_hal_pin_od_high(self->id);
    } else {
        mp_hal_pin_high(self->id);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_high_obj, machine_pin_high);

// Pin.toggle(). Only TOGGLE pins set as OUTPUT.
STATIC mp_obj_t machine_pin_toggle(mp_obj_t self_in) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);

    // Determine DIRECTION of PIN.
    bool pin_dir;

    if (GPIO_IS_OPEN_DRAIN(self->id)) {
        pin_dir = mp_hal_get_pin_direction(self->id);
        if (pin_dir) {
            // Pin is output, thus low, switch to high
            mp_hal_pin_od_high(self->id);
        } else {
            mp_hal_pin_od_low(self->id);
        }
    } else {
        gpio_toggle_pin_level(self->id);
    }
    return mp_const_none;
}
MP_DEFINE_CONST_FUN_OBJ_1(machine_pin_toggle_obj, machine_pin_toggle);

// Pin.drive(). Normal (0) is 2mA, High (1) allows 8mA.
STATIC mp_obj_t machine_pin_drive(size_t n_args, const mp_obj_t *args) {
    machine_pin_obj_t *self = args[0]; // Pin
    if (n_args == 1) {
        return mp_const_none;
    } else {
        bool strength = mp_obj_get_int(args[1]); // 0 or 1
        pin_validate_drive(strength);
        // Set the DRVSTR bit (ASF hri/hri_port_dxx.h
        hri_port_write_PINCFG_DRVSTR_bit(PORT,
            (enum gpio_port)GPIO_PORT(self->id),
            GPIO_PIN(self->id),
            strength);
        return mp_const_none;
    }
}
STATIC MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(machine_pin_drive_obj, 1, 2, machine_pin_drive);

// pin.irq(handler=None, trigger=IRQ_FALLING|IRQ_RISING, hard=False)
STATIC mp_obj_t machine_pin_irq(size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_handler, ARG_trigger, ARG_hard };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_handler, MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_trigger, MP_ARG_INT, {.u_int = 3} },
        { MP_QSTR_hard, MP_ARG_BOOL, {.u_bool = false} },
    };
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args, MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    // Get the IRQ object.
    uint8_t eic_id = get_pin_af_info(self->id)->eic;
    machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_objects[eic_id]);
    if (irq != NULL && irq->pin_id != self->id) {
        mp_raise_ValueError(MP_ERROR_TEXT("IRQ already used"));
    }

    // Allocate the IRQ object if it doesn't already exist.
    if (irq == NULL) {
        irq = m_new_obj(machine_pin_irq_obj_t);
        irq->base.base.type = &mp_irq_type;
        irq->base.methods = (mp_irq_methods_t *)&machine_pin_irq_methods;
        irq->base.parent = MP_OBJ_FROM_PTR(self);
        irq->base.handler = mp_const_none;
        irq->base.ishard = false;
        irq->pin_id = 0xff;
        MP_STATE_PORT(machine_pin_irq_objects[eic_id]) = irq;
    }
    // (Re-)configure the irq.
    if (n_args > 1 || kw_args->used != 0) {

        // set the mux config of the pin.
        mp_hal_set_pin_mux(self->id, ALT_FCT_EIC);

        // Configure IRQ.
        #if defined(MCU_SAMD21)

        uint32_t irq_num = 4;
        // Disable all IRQs from the affected source while data is updated.
        NVIC_DisableIRQ(irq_num);
        // Disable EIC
        EIC->CTRL.bit.ENABLE = 0;
        while (EIC->STATUS.bit.SYNCBUSY != 0) {
        }
        EIC->INTENCLR.reg = (1 << eic_id);
        // Enable the clocks
        PM->APBAMASK.bit.EIC_ |= 1;
        GCLK->CLKCTRL.reg = GCLK_CLKCTRL_CLKEN | GCLK_CLKCTRL_GEN_GCLK2 | EIC_GCLK_ID;

        #elif defined(MCU_SAMD51)

        uint32_t irq_num = eic_id + 12;
        // Disable all IRQs from the affected source while data is updated.
        NVIC_DisableIRQ(irq_num);
        // Disable EIC
        EIC->CTRLA.bit.ENABLE = 0;
        while (EIC->SYNCBUSY.bit.ENABLE != 0) {
        }
        EIC->INTENCLR.reg = (1 << eic_id);
        // Enable the clocks
        MCLK->APBAMASK.bit.EIC_ |= 1;
        GCLK->PCHCTRL[EIC_GCLK_ID].reg = GCLK_PCHCTRL_CHEN | GCLK_PCHCTRL_GEN_GCLK2;

        #endif
        // Clear the pending interrupts flag
        EIC->INTENCLR.reg = (1 << eic_id);

        // Update IRQ data.
        irq->base.handler = args[ARG_handler].u_obj;
        irq->base.ishard = args[ARG_hard].u_bool;
        irq->flags = 0;
        irq->trigger = args[ARG_trigger].u_int;
        irq->pin_id = self->id;

        // Enable IRQ if a handler is given.
        if (args[ARG_handler].u_obj != mp_const_none) {
            // Set EIC channel mode
            EIC->CONFIG[eic_id / 8].reg |= irq->trigger << ((eic_id % 8) * 4);
            EIC->INTENSET.reg = (1 << eic_id);
            EIC->INTFLAG.reg |= (1 << eic_id);
        }

        // Enable EIC (again)
        #if defined(MCU_SAMD21)
        EIC->CTRL.bit.ENABLE = 1;
        while (EIC->STATUS.bit.SYNCBUSY != 0) {
        }
        #elif defined(MCU_SAMD51)
        EIC->CTRLA.bit.ENABLE = 1;
        while (EIC->SYNCBUSY.bit.ENABLE != 0) {
        }
        #endif
        // Enable interrupt again
        NVIC_EnableIRQ(irq_num);
    }
    return MP_OBJ_FROM_PTR(irq);
}
STATIC MP_DEFINE_CONST_FUN_OBJ_KW(machine_pin_irq_obj, 1, machine_pin_irq);

void pin_irq_deinit_all(void) {

    EIC->INTENCLR.reg = 0xffff;  // Disable all interrupts from the EIC.
    for (int i = 0; i < 16; i++) { // Clear all irq object pointers
        MP_STATE_PORT(machine_pin_irq_objects[i]) = NULL;
    }
    // Disable all irq's at the NVIC controller
    #if defined(MCU_SAMD21)
    NVIC_DisableIRQ(4);
    #elif defined(MCU_SAMD51)
    for (int i = 12; i < 20; i++) {
        NVIC_DisableIRQ(i);
    }
    #endif
}

// Common EIC handler for all events.
void EIC_Handler() {
    uint32_t mask = 1;
    uint32_t isr = EIC->INTFLAG.reg;
    for (int eic_id = 0; eic_id < 16; eic_id++, mask <<= 1) {
        // Did the ISR fire?
        if (isr & mask) {
            EIC->INTFLAG.reg |= mask; // clear the ISR flag
            machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_objects[eic_id]);
            if (irq != NULL) {
                irq->flags = irq->trigger;
                mp_irq_handler(&irq->base);
                break;
            }
        }
    }
}

STATIC const mp_rom_map_elem_t machine_pin_locals_dict_table[] = {
    // instance methods
    { MP_ROM_QSTR(MP_QSTR_init), MP_ROM_PTR(&machine_pin_init_obj) },
    { MP_ROM_QSTR(MP_QSTR_value), MP_ROM_PTR(&machine_pin_value_obj) },
    { MP_ROM_QSTR(MP_QSTR_low), MP_ROM_PTR(&machine_pin_low_obj) },
    { MP_ROM_QSTR(MP_QSTR_high), MP_ROM_PTR(&machine_pin_high_obj) },
    { MP_ROM_QSTR(MP_QSTR_off), MP_ROM_PTR(&machine_pin_low_obj) },
    { MP_ROM_QSTR(MP_QSTR_on), MP_ROM_PTR(&machine_pin_high_obj) },
    { MP_ROM_QSTR(MP_QSTR_toggle), MP_ROM_PTR(&machine_pin_toggle_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable), MP_ROM_PTR(&machine_pin_disable_obj) },
    { MP_ROM_QSTR(MP_QSTR_drive), MP_ROM_PTR(&machine_pin_drive_obj) },
    { MP_ROM_QSTR(MP_QSTR_irq), MP_ROM_PTR(&machine_pin_irq_obj) },

    // class constants
    { MP_ROM_QSTR(MP_QSTR_IN), MP_ROM_INT(GPIO_MODE_IN) },
    { MP_ROM_QSTR(MP_QSTR_OUT), MP_ROM_INT(GPIO_MODE_OUT) },
    { MP_ROM_QSTR(MP_QSTR_OPEN_DRAIN), MP_ROM_INT(GPIO_MODE_OPEN_DRAIN) },
    { MP_ROM_QSTR(MP_QSTR_PULL_OFF), MP_ROM_INT(GPIO_PULL_OFF) },
    { MP_ROM_QSTR(MP_QSTR_PULL_UP), MP_ROM_INT(GPIO_PULL_UP) },
    { MP_ROM_QSTR(MP_QSTR_PULL_DOWN), MP_ROM_INT(GPIO_PULL_DOWN) },
    { MP_ROM_QSTR(MP_QSTR_LOW_POWER), MP_ROM_INT(GPIO_STRENGTH_2MA) },
    { MP_ROM_QSTR(MP_QSTR_HIGH_POWER), MP_ROM_INT(GPIO_STRENGTH_8MA) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_RISING), MP_ROM_INT(GPIO_IRQ_EDGE_RISE) },
    { MP_ROM_QSTR(MP_QSTR_IRQ_FALLING), MP_ROM_INT(GPIO_IRQ_EDGE_FALL) },
};
STATIC MP_DEFINE_CONST_DICT(machine_pin_locals_dict, machine_pin_locals_dict_table);

STATIC mp_uint_t pin_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)errcode;
    machine_pin_obj_t *self = self_in;

    switch (request) {
        case MP_PIN_READ: {
            return mp_hal_pin_read(self->id);
        }
        case MP_PIN_WRITE: {
            mp_hal_pin_write(self->id, arg);
            return 0;
        }
    }
    return -1;
}

STATIC const mp_pin_p_t pin_pin_p = {
    .ioctl = pin_ioctl,
};

MP_DEFINE_CONST_OBJ_TYPE(
    machine_pin_type,
    MP_QSTR_Pin,
    MP_TYPE_FLAG_NONE,
    make_new, mp_pin_make_new,
    print, machine_pin_print,
    call, machine_pin_call,
    protocol, &pin_pin_p,
    locals_dict, &machine_pin_locals_dict
    );

static uint8_t find_eic_id(int pin) {
    for (int eic_id = 0; eic_id < 16; eic_id++) {
        machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_objects[eic_id]);
        if (irq != NULL && irq->pin_id == pin) {
            return eic_id;
        }
    }
    return 0xff;
}

STATIC mp_uint_t machine_pin_irq_trigger(mp_obj_t self_in, mp_uint_t new_trigger) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t eic_id = find_eic_id(self->id);
    if (eic_id != 0xff) {
        machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_objects[eic_id]);
        EIC->INTENCLR.reg |= (1 << eic_id);
        irq->flags = 0;
        irq->trigger = new_trigger;
        EIC->INTENSET.reg |= (1 << eic_id);
    }
    return 0;
}

STATIC mp_uint_t machine_pin_irq_info(mp_obj_t self_in, mp_uint_t info_type) {
    machine_pin_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint8_t eic_id = find_eic_id(self->id);
    if (eic_id != 0xff) {
        machine_pin_irq_obj_t *irq = MP_STATE_PORT(machine_pin_irq_objects[eic_id]);
        if (info_type == MP_IRQ_INFO_FLAGS) {
            return irq->flags;
        } else if (info_type == MP_IRQ_INFO_TRIGGERS) {
            return irq->trigger;
        }
    }
    return 0;
}

STATIC const mp_irq_methods_t machine_pin_irq_methods = {
    .trigger = machine_pin_irq_trigger,
    .info = machine_pin_irq_info,
};

mp_hal_pin_obj_t mp_hal_get_pin_obj(mp_obj_t obj) {
    if (!mp_obj_is_type(obj, &machine_pin_type)) {
        mp_raise_ValueError(MP_ERROR_TEXT("expecting a Pin"));
    }
    machine_pin_obj_t *pin = MP_OBJ_TO_PTR(obj);
    return pin->id;
}

MP_REGISTER_ROOT_POINTER(void *machine_pin_irq_objects[16]);
