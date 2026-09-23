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
#include <string.h>

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

//Configuración de Hardware
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

//Acciones y Lecturas
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

// ----- Cache del RTC, actualizado por LeerRTC() cada 1 segundo -----
RtcDateTime ahora(0);
unsigned long ultLecturaRTC = 0;

// ----- Funciones -----
void leerBotones() {
  bajar = LEER_BTN_BAJAR;
  confirmar = LEER_BTN_CONFIRMAR;
  subir = LEER_BTN_SUBIR;
}

// Lee el RTC una vez por segundo y guarda el resultado en "ahora".
// El resto del código usa "ahora" en vez de llamar a Rtc.GetDateTime() directamente.
void LeerRTC() {
  if (millis() - ultLecturaRTC >= 1000) {
    ultLecturaRTC = millis();
    ahora = Rtc.GetDateTime();
  }
}

void LCD_upd() {
  switch (estado) {
    //Dibuja reloj completo
    case 0: {
      RtcDateTime now = ahora; //usa la lectura cacheada por LeerRTC(), no llama al RTC directo
      const char* nombreDia = dias[now.DayOfWeek()]; //obtiene el nombre del día actual desde el arreglo
      lcd.setCursor(0, 0); //Primera fila
      LCD_PRINT(nombreDia);
      
      //Padding para que el espacio donde va el nombre del día siempre tenga un pequeño margen
      for (int i = strlen(nombreDia); i < 9; i++) LCD_PRINT(" ");
      LCD_PRINT(" ");
      
      //Impresión de hora, si la hora es menor a 10 se escribe un 0 y luego el valor de la hora (ej: "07:" en lugar de solo "7:")
      if (now.Hour() < 10) LCD_PRINT("0");
      LCD_PRINT(now.Hour());
      LCD_PRINT(":"); //dos puntos separadores entre hora y minutos.
      
      //Impresión de los minutos después del ":" de la hora, si el valor del minuto es menor a 10 se escribe un 0 en el primer dígito, mismo motivo que caso anterior
      if (now.Minute() < 10) LCD_PRINT("0");
      LCD_PRINT(now.Minute());
      lcd.setCursor(0, 1); //Segunda fila

      //Impresión del día, misma lógica que el caso anterior
      if (now.Day() < 10) LCD_PRINT("0"); 
      LCD_PRINT(now.Day());
      LCD_PRINT("/"); //Barra separadora de día y mes

      //Impresión del mes, misma lógica que el caso anterior
      if (now.Month() < 10) LCD_PRINT("0"); 
      LCD_PRINT(now.Month());
      LCD_PRINT("/"); //Barra separadora de mes y año
      
      //Impresión del año
      LCD_PRINT(now.Year());
      break;
    }

    //Sobreescritura del valor del día
    case 1: {
      const char* nombreDia = dias[diaSeleccionado];
      //Escribe después de "día: "
      lcd.setCursor(5, 0);
      LCD_PRINT(nombreDia);
      //Padding para que el espacio donde va el nombre del día siempre tenga un pequeño margen
      for (int i = strlen(nombreDia); i < 9; i++) LCD_PRINT(" ");
      break;
    }

    ////Sobreescritura del valor de la alarma
    case 2:
      //Escribe después de "alarma: "
      lcd.setCursor(8, 0);
      LCD_PRINT(alarmaSeleccionada + 1);
      break;
    
    //Sobreescritura del valor de la hora
    case 3:
      //Escribe después de "hora: "
      lcd.setCursor(6, 0);
      if (horaSeleccionada < 10) LCD_PRINT("0"); //Impresión de hora, si la hora es menor a 10 se escribe un 0 y luego el valor de la hora (ej: "07:" en lugar de solo "7:")
      LCD_PRINT(horaSeleccionada);
      break;

    //Sobreescritura del valor del minuto
    case 4:
      //Escribe después de "min: "
      lcd.setCursor(5, 0);
      if (minutoSeleccionado < 10) LCD_PRINT("0"); //Impresión de los minutos después del ":" de la hora, si el valor del minuto es menor a 10 se escribe un 0 en el primer dígito
      LCD_PRINT(minutoSeleccionado);
      break;
  }
}

//Define el marco inicial izquierdo que se muestra en el lcd y actualiza los valores a rellenar con LCD_UPD;
void LCD_full() {
  CLR_LCD; //Limpia pantalla
  switch (estado) {
    case 1: LCD_PRINT("Dia: ");    break;
    case 2: LCD_PRINT("Alarma: "); break;
    case 3: LCD_PRINT("Hora: ");   break;
    case 4: LCD_PRINT("Min: ");    break;
  }
  LCD_upd(); //Actualiza con los valores correspondientes
}

void Menu() {
  switch (estado) {
    //Esperando que se presione el botón de confirmar para proceder
    case 0:
      LCD_upd();
      if (confirmar) {
        estado = 1;
        delay(200);
        LCD_full();
      }
      break;

    //Selección de Día
    case 1:
      if (subir) { diaSeleccionado = (diaSeleccionado + 1) % 7; delay(200); LCD_upd(); }
      if (bajar) { diaSeleccionado = (diaSeleccionado == 0) ? 6 : diaSeleccionado - 1; delay(200); LCD_upd(); }
      if (confirmar) { estado = 2; delay(200); LCD_full(); }
      break;

    //Selección de Alarma
    case 2:
      if (subir) { alarmaSeleccionada = (alarmaSeleccionada + 1) % 3; delay(200); LCD_upd(); }
      if (bajar) { alarmaSeleccionada = (alarmaSeleccionada == 0) ? 2 : alarmaSeleccionada - 1; delay(200); LCD_upd(); }
      if (confirmar) { estado = 3; delay(200); LCD_full(); }
      break;

    //Selección de Hora
    case 3:
      if (subir) { horaSeleccionada = (horaSeleccionada + 1) % 24; delay(200); LCD_upd(); }
      if (bajar) { horaSeleccionada = (horaSeleccionada == 0) ? 23 : horaSeleccionada - 1; delay(200); LCD_upd(); }
      if (confirmar) { estado = 4; delay(200); LCD_full(); }
      break;

    //Selección de Minuto y Guardado de Alarma al ser la última operación
    case 4:
      if (subir) { minutoSeleccionado = (minutoSeleccionado + 1) % 60; delay(200); LCD_upd(); }
      if (bajar) { minutoSeleccionado = (minutoSeleccionado == 0) ? 59 : minutoSeleccionado - 1; delay(200); LCD_upd(); }
      if (confirmar) {
        guardarAlarma(diaSeleccionado, alarmaSeleccionada, horaSeleccionada, minutoSeleccionado);
        estado = 0; //Devuelve a la pantalla principal donde espera la presión del botón de confirmar para comenzar
        CLR_LCD;
        LCD_PRINT("Alarma ");
        LCD_PRINT(alarmaSeleccionada + 1);
        LCD_PRINT(" guardada");
        delay(1000);
        CLR_LCD;
      }
      break;
  }
}

void guardarAlarma(int dia, int numAlarma, int hora, int minuto) {
  int addr = (dia * 6) + (numAlarma * 2);

  if (EEPROM.read(addr) != hora) {
    EEPROM.write(addr, hora);
  }
  if (EEPROM.read(addr + 1) != minuto) {
    EEPROM.write(addr + 1, minuto);
  }
  EEPROM.commit(); //Esto guarda el estado en la memoria persistente
}

// Unifica lo que antes eran activarAlarma(), detenerAlarma() y verificarAlarmas()
// en una sola función que se encarga de toda la gestión de la alarma.
// Misma lógica y mismos delay() que el código original, solo reorganizado.
void CtrlAlarma() {
  // --- Caso: la alarma ya está sonando, esperando confirmación ---
  if (alarmaActiva) {
    if (confirmar) {
      // Antes: detenerAlarma()
      noTone(BUZZER);
      APAGAR_LED;
      motor.step(273); // Gira al confirmar
      alarmaActiva = false;
      lcd.clear();
      lcd.print("Dosis entregada");
      delay(1000);
      lcd.clear();
    }
    return;
  }

  // --- Caso: no está sonando, se verifica si corresponde activarla ---
  // Antes: verificarAlarmas(), usando ahora la lectura cacheada por LeerRTC()
  int d = ahora.DayOfWeek();
  int h = ahora.Hour();
  int m = ahora.Minute();
  int s = ahora.Second();

  for (int i = 0; i < 3; i++) {
    int addr = (d * 6) + (i * 2);
    int horaGuardada = EEPROM.read(addr);
    int minutoGuardado = EEPROM.read(addr + 1);
    if (horaGuardada == h && minutoGuardado == m && s == 0) {
      // Antes: activarAlarma()
      lcd.clear();
      lcd.print("!ALARMA!");
      ENCENDER_LED;
      tone(BUZZER, 1000);
      alarmaActiva = true;
    }
  }
}

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

  ahora = Rtc.GetDateTime();   // primera lectura, antes de que arranque el loop
  ultLecturaRTC = millis();

  CLR_LCD;
  LCD_PRINT("Sistema iniciado");
  delay(1500);
  CLR_LCD;
}

void loop() {
  leerBotones();
  LeerRTC();

  if (alarmaActiva) {
    if (confirmar) {
      CtrlAlarma();
      delay(300);   /// RESOLVER ESTE RETARDO SIN DELAY
    }
    return;
  }

  Menu();
  CtrlAlarma();
  delay(100);
}