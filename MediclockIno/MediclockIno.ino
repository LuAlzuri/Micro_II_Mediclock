/*
* LiquidCrystal\_I2C by Martin Kubovcik, Frank de Brabander
* Rtc by Makuna by Michael C. Miller

BASCAL
* Crear función LCD_upd() que se encargue de actualizar el contenido dinámico en el Display
* Crear función LCD_full() que se encargue de dibujar el LCD completo
* Crear función Menu() que contenga todo el manejo del menú

ALZURI
* Despejar el loop()
* Eliminar los delay

FUENTES
* Crear función LeerRTC() que lea el RTC cada 1 segundo
* Crear función CtrlAlarma que se encargue de la gestión de la alarma

*/
// TAREAS:
// ELIMINAR LOS DELAY DEL PROYECTO (10/9/26)
// Despejar el bucle principal

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <EEPROM.h>
#include <Stepper.h>

// ----- Configuración del motor -----
#define IN1 10
#define IN2 11
#define IN3 12
#define IN4 13
#define PASOS_POR_VUELTA 2048
Stepper motor(PASOS_POR_VUELTA, IN1, IN3, IN2, IN4);

// ----- Configuración del LCD -----
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ----- RTC -----
ThreeWire myWire(7, 6, 8); // DAT, CLK, RST
RtcDS1302<ThreeWire> Rtc(myWire);

// ----- Pines de los Botones (NÚMEROS DE PIN) -----
#define PIN_BTN_BAJAR     2
#define PIN_BTN_CONFIRMAR 3
#define PIN_BTN_SUBIR     4

// ----- LED y buzzer -----
#define LED 9
#define BUZZER 5

// ----- Tamaño de la EEPROM emulada (ESP32) -----
#define EEPROM_SIZE 512

// Configuración de Hardware
#define LCD_INIT          lcd.init()
#define LCD_BACKLIGHT     lcd.backlight()
#define INIT_SERIAL       Serial.begin(9600)
#define CFG_BTN_BAJAR     pinMode(PIN_BTN_BAJAR, INPUT_PULLUP)
#define CFG_BTN_CONFIRMAR pinMode(PIN_BTN_CONFIRMAR, INPUT_PULLUP)
#define CFG_BTN_SUBIR     pinMode(PIN_BTN_SUBIR, INPUT_PULLUP)
#define CFG_LED           pinMode(LED, OUTPUT)
#define CFG_BUZZER        pinMode(BUZZER, OUTPUT)
#define CFG_MOTOR         motor.setSpeed(10)
#define CFG_RTC           Rtc.Begin()
#define CLR_LCD           lcd.clear()
#define LCD_PRINT(X)      lcd.print(X)

// Acciones y Lecturas
#define APAGAR_LED        digitalWrite(LED, LOW)
#define ENCENDER_LED      digitalWrite(LED, HIGH)

#define LEER_BTN_BAJAR     (digitalRead(PIN_BTN_BAJAR) == LOW)
#define LEER_BTN_CONFIRMAR (digitalRead(PIN_BTN_CONFIRMAR) == LOW)
#define LEER_BTN_SUBIR     (digitalRead(PIN_BTN_SUBIR) == LOW)

// ----- Variables globales -----
const char* dias[] = {"Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado", "Domingo"};

int estado = 0;
int diaSeleccionado = 0;
int alarmaSeleccionada = 0;
int horaSeleccionada = 0;
int minutoSeleccionado = 0;

bool bajar = false, subir = false, confirmar = false;
bool alarmaActiva = false;

// ----- Funciones -----
void leerBotones() {
  bajar = LEER_BTN_BAJAR;
  confirmar = LEER_BTN_CONFIRMAR;
  subir = LEER_BTN_SUBIR;
}

void mostrarHora() {
  RtcDateTime now = Rtc.GetDateTime();
  lcd.setCursor(0, 0);
  lcd.print(dias[now.DayOfWeek()]);
  lcd.print(" ");
  if (now.Hour() < 10) lcd.print("0");
  lcd.print(now.Hour());
  lcd.print(":");
  if (now.Minute() < 10) lcd.print("0");
  lcd.print(now.Minute());

  lcd.setCursor(0, 1);
  if (now.Day() < 10) lcd.print("0");
  lcd.print(now.Day());
  lcd.print("/");
  if (now.Month() < 10) lcd.print("0");
  lcd.print(now.Month());
  lcd.print("/");
  lcd.print(now.Year());
}

void mostrarMenuDia() {
  lcd.clear();
  lcd.print("Dia: ");
  lcd.print(dias[diaSeleccionado]);
}

void mostrarMenuAlarma() {
  lcd.clear();
  lcd.print("Alarma: ");
  lcd.print(alarmaSeleccionada + 1);
}

void mostrarMenuHora() {
  lcd.clear();
  lcd.print("Hora: ");
  if (horaSeleccionada < 10) lcd.print("0");
  lcd.print(horaSeleccionada);
}

void mostrarMenuMinuto() {
  lcd.clear();
  lcd.print("Min: ");
  if (minutoSeleccionado < 10) lcd.print("0");
  lcd.print(minutoSeleccionado);
}

void activarAlarma() {
  lcd.clear();
  lcd.print("!ALARMA!");
  ENCENDER_LED;
  tone(BUZZER, 1000);
  alarmaActiva = true;
}

void detenerAlarma() {
  noTone(BUZZER);
  APAGAR_LED;
  motor.step(273); // Gira al confirmar
  alarmaActiva = false;
  lcd.clear();
  lcd.print("Dosis entregada");
  delay(1000);
  lcd.clear();
}

// ----- EEPROM: en ESP32 no existe update(), y hay que hacer commit() -----
void guardarAlarma(int dia, int numAlarma, int hora, int minuto) {
  int addr = (dia * 6) + (numAlarma * 2);

  if (EEPROM.read(addr) != hora) {
    EEPROM.write(addr, hora);
  }
  if (EEPROM.read(addr + 1) != minuto) {
    EEPROM.write(addr + 1, minuto);
  }
  EEPROM.commit(); // imprescindible en ESP32, si no se pierde al reiniciar
}

void verificarAlarmas() {
  RtcDateTime now = Rtc.GetDateTime();
  int d = now.DayOfWeek();
  int h = now.Hour();
  int m = now.Minute();
  int s = now.Second();

  for (int i = 0; i < 3; i++) {
    int addr = (d * 6) + (i * 2);
    int horaGuardada = EEPROM.read(addr);
    int minutoGuardado = EEPROM.read(addr + 1);
    if (horaGuardada == h && minutoGuardado == m && s == 0) {
      activarAlarma();
    }
  }
}

// ----- Setup -----
void setup() {
  LCD_INIT;
  LCD_BACKLIGHT;
  INIT_SERIAL;

  CFG_BTN_BAJAR;
  CFG_BTN_CONFIRMAR;
  CFG_BTN_SUBIR;
  CFG_LED;
  CFG_BUZZER;

  CFG_MOTOR;
  CFG_RTC;

  EEPROM.begin(EEPROM_SIZE); // requerido en ESP32 antes de leer/escribir

  CLR_LCD;
  LCD_PRINT("Sistema iniciado");
  delay(1500);
  CLR_LCD;
}

// ----- Loop -----
void loop() {
  leerBotones();
  RtcDateTime now = Rtc.GetDateTime();

  if (alarmaActiva) {
    if (confirmar) {
      detenerAlarma();
      delay(300);   /// RESOLVER ESTE RETARDO SIN DELAY
    }
    return;
  }

  switch (estado) {
    case 0:
      mostrarHora();
      if (confirmar) {
        estado = 1;
        delay(200);/// RESOLVER ESTE RETARDO SIN DELAY
        mostrarMenuDia();
      }
      break;

    case 1:
      if (subir) { diaSeleccionado = (diaSeleccionado + 1) % 7; delay(200); mostrarMenuDia(); }
      if (bajar) { diaSeleccionado = (diaSeleccionado == 0 ? 6 : diaSeleccionado - 1); delay(200); mostrarMenuDia(); }
      if (confirmar) { estado = 2; delay(200); mostrarMenuAlarma(); }
      break;

    case 2:
      if (subir) { alarmaSeleccionada = (alarmaSeleccionada + 1) % 3; delay(200); mostrarMenuAlarma(); }
      if (bajar) { alarmaSeleccionada = (alarmaSeleccionada == 0 ? 2 : alarmaSeleccionada - 1); delay(200); mostrarMenuAlarma(); }
      if (confirmar) { estado = 3; delay(200); mostrarMenuHora(); }
      break;

    case 3:
      if (subir) { horaSeleccionada = (horaSeleccionada + 1) % 24; delay(200); mostrarMenuHora(); }
      if (bajar) { horaSeleccionada = (horaSeleccionada == 0 ? 23 : horaSeleccionada - 1); delay(200); mostrarMenuHora(); }
      if (confirmar) { estado = 4; delay(200); mostrarMenuMinuto(); }
      break;

    case 4:
      if (subir) { minutoSeleccionado = (minutoSeleccionado + 1) % 60; delay(200); mostrarMenuMinuto(); }
      if (bajar) { minutoSeleccionado = (minutoSeleccionado == 0 ? 59 : minutoSeleccionado - 1); delay(200); mostrarMenuMinuto(); }
      if (confirmar) {
        guardarAlarma(diaSeleccionado, alarmaSeleccionada, horaSeleccionada, minutoSeleccionado);
        estado = 0;
        lcd.clear();
        lcd.print("Alarma ");
        lcd.print(alarmaSeleccionada + 1);
        lcd.print(" guardada");
        delay(1000);
        lcd.clear();
      }
      break;
  }

  verificarAlarmas();
  delay(100);
}
