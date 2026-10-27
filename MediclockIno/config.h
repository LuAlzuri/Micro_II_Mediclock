#ifndef MEDICLOCK_CONFIG_H
#define MEDICLOCK_CONFIG_H

// Configuración central de hardware y red del firmware MediClock para ESP32.
// Los pines se eligieron para no usar el bus de la memoria flash (GPIO6-GPIO11)
// ni los pines de arranque GPIO0 / GPIO2 / GPIO15. Ver tabla en README.md.

// ------------------------------------------------------------- Bus I2C
// Bus I2C por hardware del ESP32. Solo lo usa el LCD (el DS1302 usa 3 hilos).
#define PIN_I2C_SDA        21
#define PIN_I2C_SCL        22
#define LCD_I2C_ADDR       0x27
#define LCD_COLS           16
#define LCD_ROWS           2

// ------------------------------------------------ RTC DS1302 (3 hilos)
// Nota sobre GPIO5: es pin de arranque y debe leer HIGH al encender.
// La línea RST del DS1302 queda en LOW y solo sube a HIGH durante cada lectura,
// y el pull-up de la placa la mantiene en HIGH al arrancar, por eso es seguro.
// Nunca conectar GPIO5 a GND.
#define PIN_DS1302_DAT     19   // DAT (datos)
#define PIN_DS1302_CLK     18   // CLK (reloj)
#define PIN_DS1302_RST     5    // RST (habilitación)

// -------------------------------- Motor 28BYJ-48 + driver ULN2003
// Se alimenta con fuente externa de 5 V separada (ver README).
// Nota sobre GPIO12: es pin de arranque (MTDI) y debe leer LOW al encender
// para elegir bien el voltaje de la flash. La entrada del ULN2003 no lo
// fuerza a HIGH al arrancar, por eso IN2 en GPIO12 es seguro, pero NO
// agregar pull-up externo en esta línea.
#define PIN_STEPPER_IN1    13
#define PIN_STEPPER_IN2    12   // pin de arranque: sin pull-ups externos
#define PIN_STEPPER_IN3    14
#define PIN_STEPPER_IN4    27
#define STEPPER_STEPS_PER_REV 2048  // 28BYJ-48 en modo medio paso

// ---------------------------------------------- Reed (final de carrera)
// GPIO34 es solo entrada (sin salida y sin pull-up/pull-down interno).
// Lleva pull-up externo de 10 k a 3,3 V sí o sí; el contacto cierra a GND.
// Se pide INPUT_PULLUP por intención, pero en GPIO34-39 no tiene efecto.
#define PIN_REED_SWITCH    34

// ---------------------------------------- Botón de pánico (sin internet)
// Igual que el reed: GPIO35 es solo entrada sin pull-up interno,
// así que lleva pull-up externo de 10 k a 3,3 V sí o sí.
// Activo en LOW. Funciona sin WiFi, se atiende en T5.
#define PIN_PANIC_BUTTON   35

// ---------------------------------------------------------------- Salidas
#define PIN_LED            23   // LED de estado, activo en HIGH
#define PIN_BUZZER         26   // buzzer pasivo por PWM (LEDC)
#define BUZZER_LEDC_CHANNEL 0  // sin uso en núcleo v3.x (canal automático)
#define BUZZER_LEDC_FREQ_HZ 2000
#define BUZZER_LEDC_RES_BITS 8

// ------------------------------------------------------------------- Red
// SOLO MARCADORES — nunca guardar claves reales (ver política del repo).
#define WIFI_SSID          "TCL 30 SE"
#define WIFI_PASSWORD      "franbascal"
#define BACKEND_URL        "https://mediclockbackend.runasp.net/api/dispositivos/1"
#define BACKEND_ALARMS_PATH "/alarmas"
#define BACKEND_EVENT_PATH  "/eventos"
#define DISPOSITIVO_ID     1  // igual al id de la URL de arriba

// ------------------------------------------------------------------- NTP
// América/Argentina/Buenos_Aires (UTC-3, sin horario de verano).
#define NTP_SERVER         "pool.ntp.org"
#define NTP_GMT_OFFSET_SEC (-3 * 3600)
#define NTP_DAYLIGHT_OFFSET_SEC 0
#define NTP_SYNC_INTERVAL_MS (6UL * 3600UL * 1000UL)  // reintento cada 6 h

// --------------------------------------------------- Tiempos del sistema
#define RTC_CACHE_INTERVAL_MS   1000   // T2: releer el RTC una vez por segundo
#define LCD_REFRESH_INTERVAL_MS 1000   // T2: redibujar el reloj del LCD
#define LCD_SYNC_AVISO_MS       2000   // T2: mostrar "SYNC" tras sincronizar
#define NTP_REINTENTO_SIN_HORA_MS (30UL * 1000UL) // T2: reintento NTP sin hora válida
#define WIFI_RETRY_INTERVAL_MS  10000  // T4: ventana de reintento WiFi sin bloquear
#define ALARMS_FETCH_INTERVAL_MS (30UL * 60UL * 1000UL) // T4: GET /alarmas cada 30 min (fallback)
#define WS_HOST   "mediclockbackend.runasp.net"
#define WS_PORT   443
#define WS_PATH   "/ws/dispositivo/1"   // mantener sincronizado con DISPOSITIVO_ID
#define WS_RECONNECT_INTERVAL_MS 5000
#define LED_TEST 2
#define HTTP_TIMEOUT_MS         5000   // T4: tope de espera de GET/POST, sin bloquear

// --------------------------------------------------- T5: actuadores
// Pasos de dosis: giro del 28BYJ-48 tras llegar al origen.
// Valor inicial igual al firmware original (273 pasos).
// SE CALIBRA con las aspas: ajustar hasta que caiga una sola dosis.
#define DISPENSAR_PASOS         273
#define STEPS_PER_HENDIDURA     146   // 2048 pasos/vuelta / 14 hendiduras (truncado)
#define STEPPER_PASOS_POR_TURNO 8      // T5: pasos cortos por turno, sin bloquear
#define STEPPER_INTERVALO_MS    10     // T5: pausa entre turnos del motor
#define REVERSA_PAUSA_MS        3000    // T5: espera antes de retroceder (caida de la pastilla)
#define HOMING_MAX_PASOS        2048   // T5: tope de homing (una vuelta); evita giro infinito
#define BUZZER_PARPADEO_MS      500    // T5: patrón intermitente on/off del buzzer
#define BUZZER_DUTY             128    // T5: volumen del buzzer (0-255, 8 bits)
#define PANIC_DEBOUNCE_MS       50     // T5: antirebote del botón de pánico
#define DOSIS_AVISO_MS          1000   // T5: "Dosis entregada" en pantalla
#define PANIC_ESTADO_MS         2000   // T5: mensaje de estado del botón sin alarma

#endif // MEDICLOCK_CONFIG_H
