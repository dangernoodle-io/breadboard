#pragma once

/*
 * BB_DISPLAY_AUTOREGISTER — link-time backend self-registration macro.
 *
 * Private to bb_display driver implementations; not part of the public
 * bb_display header surface.
 *
 * Usage:
 *   BB_DISPLAY_AUTOREGISTER(mydriver, CONFIG_BB_DISPLAY_MYDRIVER_AUTOREGISTER, &s_backend)
 *
 * Expands to a __attribute__((constructor)) function named
 * bb_display_register__<chip_name>.  The Kconfig symbol is evaluated at
 * compile time as a constant expression; the optimizer eliminates the dead
 * branch when autoregister is disabled, preserving the same behaviour as the
 * previous #if guard.  The CMakeLists -u flag keeps the symbol live under
 * PlatformIO's linker GC.
 */
#define BB_DISPLAY_AUTOREGISTER(chip_name, kconfig_sym, backend_ptr)   \
    void bb_display_register__##chip_name(void)                        \
        __attribute__((constructor));                                   \
    void bb_display_register__##chip_name(void)                        \
    {                                                                   \
        if (kconfig_sym) {                                              \
            bb_display_register_backend(backend_ptr);                  \
        }                                                               \
    }
