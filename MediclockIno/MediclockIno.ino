/*
  Pastillero automatico
  --------------------------------------------------------------
  Refactor:
   - LeerRTC()   : lee el RTC una vez por segundo y cachea el valor.
   - CtrlAlarma(): unifica verificarAlarmas() + activarAlarma() + detenerAlarma()
                   en una sola maquina de estados, sin delay().
   - Botones por flanco con antirrebote por millis() (sin delay(200)).
   - Mensajes temporales del LCD sin delay().
   - Giro del motor no bloqueante (1 paso por vuelta de loop).
   - Correccion del dia de la semana (el RTC devuelve 0 = Domingo).
   - Compatible con AVR (Uno/Nano) y ESP32.
*/

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <EEPROM.h>
#include <Stepper.h>
#include <string.h>

// ----- Configuracion del motor -----
#define IN1 10
#define IN2 11
#define IN3 12
#define IN4 13
#define PASOS_POR_VUELTA 2048
#define PASOS_POR_DOSIS  273          // 1/7.5 de vuelta aprox.
Stepper motor(PASOS_POR_VUELTA, IN1, IN3, IN2, IN4);

// ----- Configuracion del LCD -----
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ----- RTC -----
ThreeWire myWire(7, 6, 8);            // DAT, CLK, RST
RtcDS1302<ThreeWire> Rtc(myWire);

// ----- Pines de los botones -----
#define PIN_BTN_BAJAR     2
#define PIN_BTN_CONFIRMAR 3
#define PIN_BTN_SUBIR     4

// ----- LED y buzzer -----
#define LED    9
#define BUZZER 5

// ----- EEPROM -----
#define EEPROM_SIZE   512
#define ALARMAS_POR_DIA 3
#define BYTES_POR_DIA  (ALARMAS_POR_DIA * 2)

// En ESP32 la EEPROM es emulada y necesita begin()/commit().
// En AVR esas funciones no existen, asi que se anulan.
#if defined(ESP32) || defined(ESP8266)
  #define EEPROM_INIT    EEPROM.begin(EEPROM_SIZE)
  #define EEPROM_COMMIT  EEPROM.commit()
#else
  #define EEPROM_INIT    do {} while (0)
  #define EEPROM_COMMIT  do {} while (0)
#endif

// ----- Configuracion de hardware -----
#define LCD_INIT          lcd.init()
#define LCD_BACKLIGHT     lcd.backlight()
#define INIT_SERIAL       Serial.begin(9600)
#define CFG_LED           pinMode(LED, OUTPUT)
#define CFG_BUZZER        pinMode(BUZZER, OUTPUT)
#define CFG_MOTOR         motor.setSpeed(10)
#define CFG_RTC           Rtc.Begin()
#define CLR_LCD           lcd.clear()
#define LCD_PRINT(X)      lcd.print(X)

// ----- Acciones -----
#define APAGAR_LED        digitalWrite(LED, LOW)
#define ENCENDER_LED      digitalWrite(LED, HIGH)

// =====================================================================
//  BOTONES (antirrebote + deteccion de flanco de bajada)
// =====================================================================
#define DEBOUNCE_MS 40

struct Boton {
  byte          pin;
  bool          lecturaPrev;
  bool          estable;
  unsigned long tCambio;
};

Boton btnBajar     = { PIN_BTN_BAJAR,     false, false, 0 };
Boton btnConfirmar = { PIN_BTN_CONFIRMAR, false, false, 0 };
Boton btnSubir     = { PIN_BTN_SUBIR,     false, false, 0 };

// Devuelve true SOLO en el ciclo en que el boton pasa de suelto a presionado.
bool flancoBoton(Boton &b) {
  bool lectura = (digitalRead(b.pin) == LOW);   // INPUT_PULLUP: LOW = presionado

  if (lectura != b.lecturaPrev) {               // la senal cambio: reinicia el conteo
    b.lecturaPrev = lectura;
    b.tCambio     = millis();
  }

  // La lectura se mantuvo estable el tiempo suficiente y difiere del estado guardado
  if ((millis() - b.tCambio) >= DEBOUNCE_MS && lectura != b.estable) {
    b.estable = lectura;
    return b.estable;                           // true solo al presionar
  }
  return false;
}

bool bajar = false, subir = false, confirmar = false;

void leerBotones() {
  bajar     = flancoBoton(btnBajar);
  confirmar = flancoBoton(btnConfirmar);
  subir     = flancoBoton(btnSubir);
}

// =====================================================================
//  RTC CACHEADO
// =====================================================================
const char* dias[] = { "Lunes", "Martes", "Miercoles", "Jueves",
                       "Viernes", "Sabado", "Domingo" };

RtcDateTime   ahora         = RtcDateTime(0);  // ultima lectura valida
unsigned long ultLecturaRTC = 0;
bool          rtcNuevo      = false;           // true solo en el ciclo de lectura nueva

// El DS1302 devuelve 0 = Domingo, pero dias[] arranca en Lunes.
// Este ajuste alinea ambos: Lunes = 0 ... Domingo = 6.
int idxDia(const RtcDateTime &t) {
  return (t.DayOfWeek() + 6) % 7;
}

// Lee el RTC una vez por segundo y guarda el resultado en "ahora".
void LeerRTC() {
  rtcNuevo = false;
  if (millis() - ultLecturaRTC >= 1000) {
    ultLecturaRTC = millis();
    ahora    = Rtc.GetDateTime();
    rtcNuevo = true;
  }
}

// =====================================================================
//  ESTADO DEL MENU Y MENSAJES TEMPORALES
// =====================================================================
int estado             = 0;
int diaSeleccionado    = 0;
int alarmaSeleccionada = 0;
int horaSeleccionada   = 0;
int minutoSeleccionado = 0;

bool          forzarRedibujo  = true;    // pide repintar el reloj aunque no haya lectura nueva
bool          mostrandoMsj    = false;   // hay un mensaje temporal en pantalla
unsigned long tMensaje        = 0;
unsigned int  duracionMensaje = 0;

// Muestra un mensaje que se borra solo despues de "ms", sin bloquear.
void mostrarMensaje(const char* linea1, const char* linea2, unsigned int ms) {
  CLR_LCD;
  LCD_PRINT(linea1);
  if (linea2 != NULL) {
    lcd.setCursor(0, 1);
    LCD_PRINT(linea2);
  }
  mostrandoMsj    = true;
  tMensaje        = millis();
  duracionMensaje = ms;
}

// Devuelve true mientras el mensaje siga visible.
bool mensajeEnCurso() {
  if (!mostrandoMsj) return false;
  if (millis() - tMensaje >= duracionMensaje) {
    mostrandoMsj   = false;
    CLR_LCD;
    forzarRedibujo = true;
    return false;
  }
  return true;
}

// =====================================================================
//  LCD
// =====================================================================
void LCD_upd() {
  switch (estado) {

    // Reloj completo. Solo se repinta cuando hay lectura nueva del RTC.
    case 0: {
      if (!rtcNuevo && !forzarRedibujo) break;
      forzarRedibujo = false;

      const char* nombreDia = dias[idxDia(ahora)];
      lcd.setCursor(0, 0);
      LCD_PRINT(nombreDia);

      // Padding para dejar siempre el mismo margen tras el nombre del dia
      for (int i = strlen(nombreDia); i < 9; i++) LCD_PRINT(" ");
      LCD_PRINT(" ");

      if (ahora.Hour() < 10) LCD_PRINT("0");    // "07:" en lugar de "7:"
      LCD_PRINT(ahora.Hour());
      LCD_PRINT(":");
      if (ahora.Minute() < 10) LCD_PRINT("0");
      LCD_PRINT(ahora.Minute());

      lcd.setCursor(0, 1);
      if (ahora.Day() < 10) LCD_PRINT("0");
      LCD_PRINT(ahora.Day());
      LCD_PRINT("/");
      if (ahora.Month() < 10) LCD_PRINT("0");
      LCD_PRINT(ahora.Month());
      LCD_PRINT("/");
      LCD_PRINT(ahora.Year());
      break;
    }

    // Valor del dia (se escribe despues de "Dia: ")
    case 1: {
      const char* nombreDia = dias[diaSeleccionado];
      lcd.setCursor(5, 0);
      LCD_PRINT(nombreDia);
      for (int i = strlen(nombreDia); i < 9; i++) LCD_PRINT(" ");
      break;
    }

    // Numero de alarma (despues de "Alarma: ")
    case 2:
      lcd.setCursor(8, 0);
      LCD_PRINT(alarmaSeleccionada + 1);
      break;

    // Hora (despues de "Hora: ")
    case 3:
      lcd.setCursor(6, 0);
      if (horaSeleccionada < 10) LCD_PRINT("0");
      LCD_PRINT(horaSeleccionada);
      break;

    // Minuto (despues de "Min: ")
    case 4:
      lcd.setCursor(5, 0);
      if (minutoSeleccionado < 10) LCD_PRINT("0");
      LCD_PRINT(minutoSeleccionado);
      break;
  }
}

// Dibuja el marco fijo de la pantalla y rellena los valores.
void LCD_full() {
  CLR_LCD;
  switch (estado) {
    case 0: forzarRedibujo = true; break;
    case 1: LCD_PRINT("Dia: ");    break;
    case 2: LCD_PRINT("Alarma: "); break;
    case 3: LCD_PRINT("Hora: ");   break;
    case 4: LCD_PRINT("Min: ");    break;
  }
  LCD_upd();
}

// =====================================================================
//  EEPROM
// =====================================================================
// Mapa: cada dia ocupa 6 bytes (3 alarmas x 2 bytes: hora y minuto).
// dia 0 = Lunes ... dia 6 = Domingo.
void guardarAlarma(int dia, int numAlarma, int hora, int minuto) {
  int addr = (dia * BYTES_POR_DIA) + (numAlarma * 2);

  if (EEPROM.read(addr)     != hora)   EEPROM.write(addr,     hora);
  if (EEPROM.read(addr + 1) != minuto) EEPROM.write(addr + 1, minuto);
  EEPROM_COMMIT;
}

// =====================================================================
//  MENU
// =====================================================================
void Menu() {
  switch (estado) {

    // Pantalla principal: espera confirmar para entrar a configurar
    case 0:
      LCD_upd();
      if (confirmar) { estado = 1; LCD_full(); }
      break;

    // Seleccion de dia
    case 1:
      if (subir) { diaSeleccionado = (diaSeleccionado + 1) % 7; LCD_upd(); }
      if (bajar) { diaSeleccionado = (diaSeleccionado == 0) ? 6 : diaSeleccionado - 1; LCD_upd(); }
      if (confirmar) { estado = 2; LCD_full(); }
      break;

    // Seleccion de alarma
    case 2:
      if (subir) { alarmaSeleccionada = (alarmaSeleccionada + 1) % ALARMAS_POR_DIA; LCD_upd(); }
      if (bajar) { alarmaSeleccionada = (alarmaSeleccionada == 0) ? ALARMAS_POR_DIA - 1 : alarmaSeleccionada - 1; LCD_upd(); }
      if (confirmar) { estado = 3; LCD_full(); }
      break;

    // Seleccion de hora
    case 3:
      if (subir) { horaSeleccionada = (horaSeleccionada + 1) % 24; LCD_upd(); }
      if (bajar) { horaSeleccionada = (horaSeleccionada == 0) ? 23 : horaSeleccionada - 1; LCD_upd(); }
      if (confirmar) { estado = 4; LCD_full(); }
      break;

    // Seleccion de minuto y guardado
    case 4:
      if (subir) { minutoSeleccionado = (minutoSeleccionado + 1) % 60; LCD_upd(); }
      if (bajar) { minutoSeleccionado = (minutoSeleccionado == 0) ? 59 : minutoSeleccionado - 1; LCD_upd(); }
      if (confirmar) {
        guardarAlarma(diaSeleccionado, alarmaSeleccionada, horaSeleccionada, minutoSeleccionado);

        char msj[17];
        snprintf(msj, sizeof(msj), "Alarma %d", alarmaSeleccionada + 1);
        mostrarMensaje(msj, "guardada", 1200);

        estado = 0;
      }
      break;
  }
}

// =====================================================================
//  CtrlAlarma: verificacion + activacion + entrega, todo en uno
// =====================================================================
#define AL_INACTIVA  0    // vigilando el reloj
#define AL_SONANDO   1    // buzzer y LED encendidos, espera confirmacion
#define AL_GIRANDO   2    // entregando la dosis paso a paso
#define AL_ENTREGADA 3    // mensaje final en pantalla

byte          estadoAlarma   = AL_INACTIVA;
bool          alarmaActiva   = false;   // true mientras la alarma bloquea el menu
int           pasosRestantes = 0;
unsigned long tAlarma        = 0;
int           ultMinDisparado = -1;     // evita repetir la alarma dentro del mismo minuto

// Corta la corriente de las bobinas para que el motor no se caliente en reposo.
void liberarMotor() {
  digitalWrite(IN1, LOW);
  digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW);
  digitalWrite(IN4, LOW);
}

void CtrlAlarma() {
  switch (estadoAlarma) {

    // ---- Busca coincidencia con las alarmas guardadas del dia ----
    case AL_INACTIVA: {
      if (!rtcNuevo) break;                 // evalua una vez por segundo

      int d = idxDia(ahora);
      int h = ahora.Hour();
      int m = ahora.Minute();

      if (m == ultMinDisparado) break;      // ya se disparo en este minuto

      for (int i = 0; i < ALARMAS_POR_DIA; i++) {
        int addr           = (d * BYTES_POR_DIA) + (i * 2);
        int horaGuardada   = EEPROM.read(addr);
        int minutoGuardado = EEPROM.read(addr + 1);

        // 255 = celda nunca escrita, se ignora
        if (horaGuardada > 23 || minutoGuardado > 59) continue;

        if (horaGuardada == h && minutoGuardado == m) {
          ENCENDER_LED;
          tone(BUZZER, 1000);
          CLR_LCD;
          LCD_PRINT("!ALARMA!");
          lcd.setCursor(0, 1);
          LCD_PRINT("OK = dosis");

          alarmaActiva    = true;
          mostrandoMsj    = false;          // descarta cualquier mensaje pendiente
          ultMinDisparado = m;
          estadoAlarma    = AL_SONANDO;
          break;
        }
      }
      break;
    }

    // ---- Suena hasta que el usuario confirma ----
    case AL_SONANDO:
      if (confirmar) {
        noTone(BUZZER);
        APAGAR_LED;
        CLR_LCD;
        LCD_PRINT("Entregando...");
        pasosRestantes = PASOS_POR_DOSIS;
        estadoAlarma   = AL_GIRANDO;
      }
      break;

    // ---- Gira el motor de a un paso por ciclo de loop ----
    case AL_GIRANDO:
      if (pasosRestantes > 0) {
        motor.step(1);                      // Stepper ya respeta la velocidad fijada
        pasosRestantes--;
      } else {
        liberarMotor();
        mostrarMensaje("Dosis entregada", NULL, 1200);
        tAlarma      = millis();
        estadoAlarma = AL_ENTREGADA;
      }
      break;

    // ---- Espera a que se apague el mensaje y devuelve el control al menu ----
    case AL_ENTREGADA:
      if (!mensajeEnCurso()) {
        alarmaActiva   = false;
        estado         = 0;
        forzarRedibujo = true;
        estadoAlarma   = AL_INACTIVA;
      }
      break;
  }
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  LCD_INIT;
  LCD_BACKLIGHT;
  INIT_SERIAL;

  pinMode(PIN_BTN_BAJAR,     INPUT_PULLUP);
  pinMode(PIN_BTN_CONFIRMAR, INPUT_PULLUP);
  pinMode(PIN_BTN_SUBIR,     INPUT_PULLUP);
  CFG_LED;
  CFG_BUZZER;

  CFG_MOTOR;
  CFG_RTC;
  EEPROM_INIT;

  ahora         = Rtc.GetDateTime();
  ultLecturaRTC = millis();

  mostrarMensaje("Sistema iniciado", NULL, 1500);
}

void loop() {
  leerBotones();
  LeerRTC();
  CtrlAlarma();

  if (alarmaActiva) return;      // la alarma tiene prioridad sobre el menu
  if (mensajeEnCurso()) return;  // no dibuja el menu encima de un mensaje

  Menu();
}
