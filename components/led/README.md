# led

LED output primitives — `bb_led` (the portable LED primitive) plus
per-driver backends (GPIO, PWM, RGB PWM, APA102) and the `bb_led_anim`
animation helper, each registering with `bb_led` via the backend-dispatch
convention rather than being a standalone consumer-facing API.

<!-- BEGIN bbtool:group-index -->
| Component | Purpose |
|-----------|---------|
| [bb_led](./bb_led/) | — |
| [bb_led_anim](./bb_led_anim/) | — |
| [bb_led_apa102](./bb_led_apa102/) | — |
| [bb_led_gpio](./bb_led_gpio/) | — |
| [bb_led_pwm](./bb_led_pwm/) | — |
| [bb_led_rgb_pwm](./bb_led_rgb_pwm/) | — |
<!-- END bbtool:group-index -->
