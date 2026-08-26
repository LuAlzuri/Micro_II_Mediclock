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

// ----- Botones -----
#define BTN_BAJAR 2
#define BTN_CONFIRMAR 3
#define BTN_SUBIR 4

// ----- LED y buzzer -----
#define LED 9
#define BUZZER 5

// ----- Variables globales -----
const char* dias[] = {"Lunes", "Martes", "Miercoles", "Jueves", "Viernes", "Sabado", "Domingo"};

int estado = 0; // 0: reloj, 1: seleccionar día, 2: seleccionar alarma, 3: hora, 4: minuto
int diaSeleccionado = 0;
int alarmaSeleccionada = 0;
int horaSeleccionada = 0;
int minutoSeleccionado = 0;

bool bajar = false, subir = false, confirmar = false;
bool alarmaActiva = false;

// ----- Funciones -----
void leerBotones() {
  bajar = digitalRead(BTN_BAJAR) == LOW;
  confirmar = digitalRead(BTN_CONFIRMAR) == LOW;
  subir = digitalRead(BTN_SUBIR) == LOW;
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
  digitalWrite(LED, HIGH);
  tone(BUZZER, 1000);
  alarmaActiva = true;
}

void detenerAlarma() {
  noTone(BUZZER);
  digitalWrite(LED, LOW);
  motor.step(273); // Gira al confirmar
  alarmaActiva = false;
  lcd.clear();
  lcd.print("Dosis entregada");
  delay(1000);
  lcd.clear();
}

void guardarAlarma(int dia, int numAlarma, int hora, int minuto) {
  int addr = (dia * 6) + (numAlarma * 2);
  EEPROM.update(addr, hora);
  EEPROM.update(addr + 1, minuto);
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
  lcd.init();
  lcd.backlight();
  Serial.begin(9600);

  pinMode(BTN_BAJAR, INPUT_PULLUP);
  pinMode(BTN_CONFIRMAR, INPUT_PULLUP);
  pinMode(BTN_SUBIR, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
  pinMode(BUZZER, OUTPUT);

  motor.setSpeed(10);
  Rtc.Begin();

  lcd.clear();
  lcd.print("Sistema Iniciado");
  delay(1500);
  lcd.clear();
}

// ----- Loop -----
void loop() {
  leerBotones();
  RtcDateTime now = Rtc.GetDateTime();

  if (alarmaActiva) {
    if (confirmar) {
      detenerAlarma();
      delay(300);
    }
    return;
  }

  switch (estado) {
    case 0:
      mostrarHora();
      if (confirmar) {
        estado = 1;
        delay(200);
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
