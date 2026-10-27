// Firmware MediClock para ESP32 — base (T1).
// Bucle principal no bloqueante: cada tarea se atiende por turnos según millis().
// Sin delay() en loop(). Sin menú por botones. Sin EEPROM (se usa NVS con Preferences).

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WebSocketsClient.h>
#include <Preferences.h>
#include <ThreeWire.h>
#include <RtcDS1302.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
#include <time.h>  // hora NTP del ESP32 (getLocalTime/configTime, sin librerías extra)

#include "config.h"

// ------------------------------------------------------ Objetos globales
ThreeWire ds1302Wire(PIN_DS1302_DAT, PIN_DS1302_CLK, PIN_DS1302_RST);
RtcDS1302<ThreeWire> rtc(ds1302Wire);
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, LCD_COLS, LCD_ROWS);
Stepper stepper(STEPPER_STEPS_PER_REV,
                PIN_STEPPER_IN1, PIN_STEPPER_IN3,
                PIN_STEPPER_IN2, PIN_STEPPER_IN4);
Preferences prefs;  // Espacio NVS donde se guardan las alarmas (T3)

// Hora en memoria: se lee del RTC como máximo una vez por segundo (T2).
RtcDateTime tiempoCacheado;
unsigned long ultimaLecturaRTCms = 0;
unsigned long ultimaSincNTPms = 0;    // última sincronización NTP exitosa
unsigned long ultimoIntentoNTPms = 0;  // último intento NTP (exitoso o no)
unsigned long ultimaConsultaMs = 0;
unsigned long ledTestHastaMs = 0;
bool ledTestEncendido = false;
unsigned long ultimoParpadeoLedMs = 0;
WebSocketsClient webSocket;
long pasosAvanceTotal = 0;   // pasos a avanzar (y retroceder) en la dosis actual
bool faseReversaDosis = false; // true = fase de retroceso
unsigned long pausaReversaHastaMs = 0; // espera no bloqueante antes de retroceder
bool wsConectado = false;
bool refrescarAlarmasPendiente = false;
bool dispensarManualPendiente = false;
unsigned long ultimoReintentoWiFims = 0;
unsigned long ultimaPantallaRelojms = 0;  // último dibujo del reloj en LCD
unsigned long avisoSyncHastaMs = 0;  // muestra "SYNC" hasta este momento
unsigned long mensajeTemporalHastaMs = 0;  // T5: mientras no vence, el LCD lo usa T5
                                           // ("Dosis entregada" o estado de pánico)
                                           // y mostrarReloj() no lo pisa.

// Estado de la hora (T2): rtcValido dice si el DS1302 responde bien;
// horaValida dice si tiempoCacheado se puede usar (RTC válido o NTP reciente).
bool rtcValido = false;
bool horaValida = false;
bool ntpConfigurado = false;  // configTime() se llama una sola vez en setup()

// ------------------------------------------- T3: estado de alarmas en NVS
// Aviso pendiente que T5 consume para mover el motor y sonar el buzzer.
// verificarAlarmas() lo deja activo; T5 lo atiende y lo pone en false.
struct AvisoAlarma {
  bool activa = false;
  bool esManual = false;
  uint8_t dia = 0;     // 0 = domingo, igual que DayOfWeek del RTC
  uint8_t slot = 0;    // 0..2
  uint8_t hora = 0;
  uint8_t minuto = 0;
};
AvisoAlarma alarmaPendiente;

// T4: evento listo para avisar al back. T5 lo llena al dispensar
// (con encolarEventoParaEnvio) y el POST de T4 lo vacía al lograr 200/201/202.
// Cola de 1: si llega otro antes de enviar, el nuevo reemplaza al anterior.
struct EventoPendiente {
  bool pendiente = false;
  bool esManual = false;
  uint8_t dia = 0;
  uint8_t slot = 0;
  uint8_t hora = 0;
  uint8_t minuto = 0;
  char fecha[11] = "";  // "DD/MM/AAAA" del RTC al momento del disparo
};
EventoPendiente eventoPendiente;

// Marca ya-disparada en memoria: cada celda dispara una sola vez por día.
// Se libera al cambiar de día (ver verificarAlarmas).
bool yaDisparo[7][2] = {{false}};
uint16_t diaMarcaAnio = 0;
uint8_t diaMarcaMes = 0;
uint8_t diaMarcaDia = 0;

// ---------------------------------------------------------- Utilidades
// Núcleo ESP32 v3.x: ledcAttach() asigna el canal solo; ledcWrite() va por pin.
void apagarBuzzer() {
  ledcWrite(PIN_BUZZER, 0);
}

// ------------------------------------------ T2: hora (RTC en memoria + NTP)
// Revisa que una fecha del RTC sea posible (pila agotada o primer
// arranque devuelven años absurdos). Solo valida el calendario.
bool esFechaPosible(const RtcDateTime& t) {
  if (t.Year() < 2024 || t.Year() > 2099) return false;
  if (t.Month() == 0 || t.Month() > 12) return false;
  if (t.Day() == 0 || t.Day() > 31) return false;
  return true;
}

// Lee el DS1302 una vez por segundo y lo guarda en tiempoCacheado.
// Así el resto del programa usa el dato en memoria sin frenar el bucle.
// Si el RTC falla (oscilador detenido o error de lectura) no se confía
// en sus registros: se marca horaValida = false para que NTP tome el relevo.
void leerTiempo() {
  unsigned long ahora = millis();
  if (ahora - ultimaLecturaRTCms < RTC_CACHE_INTERVAL_MS) {
    return;
  }
  ultimaLecturaRTCms = ahora;
  RtcDateTime lectura = rtc.GetDateTime();
  // El DS1302 (Makuna) no expone LastError(): se valida con
  // IsDateTimeValid() (oscilador) más rango de calendario.
  if (!rtc.IsDateTimeValid() || !esFechaPosible(lectura)) {
    rtcValido = false;
    horaValida = false;
    return;
  }
  rtcValido = true;
  horaValida = true;
  tiempoCacheado = lectura;
}

// Trae la hora por NTP cuando hay WiFi y ya pasó el intervalo.
// Con hora válida sincroniza cada NTP_SYNC_INTERVAL_MS; sin hora válida
// reintenta cada NTP_REINTENTO_SIN_HORA_MS hasta lograr la primera.
// Al lograrla ajusta el RTC para seguir funcionando sin WiFi.
void sincronizarNTP() {
  if (!ntpConfigurado || WiFi.status() != WL_CONNECTED) {
    return;
  }
  unsigned long ahora = millis();
  bool toca = horaValida
      ? (ultimaSincNTPms == 0 || ahora - ultimaSincNTPms >= NTP_SYNC_INTERVAL_MS)
      : (ultimoIntentoNTPms == 0 || ahora - ultimoIntentoNTPms >= NTP_REINTENTO_SIN_HORA_MS);
  if (!toca) {
    return;
  }
  ultimoIntentoNTPms = ahora;
  struct tm partes;
  // Espera breve (máximo 1,5 s y solo en su turno): no frena el bucle.
  if (!getLocalTime(&partes, 1500)) {
    return;  // sin respuesta del servidor, se reintenta luego
  }
  RtcDateTime desdeNTP(partes.tm_year + 1900, partes.tm_mon + 1, partes.tm_mday,
                       partes.tm_hour, partes.tm_min, partes.tm_sec);
  rtc.SetDateTime(desdeNTP);  // el RTC guarda la hora como respaldo offline
  tiempoCacheado = desdeNTP;
  ultimaLecturaRTCms = ahora;
  ultimaSincNTPms = ahora;
  rtcValido = true;
  horaValida = true;
  avisoSyncHastaMs = ahora + LCD_SYNC_AVISO_MS;  // aviso breve en pantalla
}

// Nombres cortos de día en español (DayOfWeek: 0 = domingo).
const char* DIAS_ES[7] = {"Dom", "Lun", "Mar", "Mie", "Jue", "Vie", "Sab"};

// Dibuja el reloj en el LCD 16x2, refresco ~1 s sin bloquear.
// Línea 0: "Mie 07:30" · Línea 1: "01/10/2026" (+ " SYNC" al sincronizar).
// Sin hora válida muestra "SIN HORA" hasta que llegue NTP.
void mostrarReloj() {
  unsigned long ahora = millis();
  if (alarmaPendiente.activa) {
    return;  // el aviso "!ALARMA!" queda en pantalla hasta que T5 lo consuma
  }
  if (ahora < mensajeTemporalHastaMs) {
    return;  // T5 muestra "Dosis entregada" o estado de pánico (1-2 s)
  }
  if (ahora - ultimaPantallaRelojms < LCD_REFRESH_INTERVAL_MS) {
    return;
  }
  ultimaPantallaRelojms = ahora;
  char texto[17];
  char linea[17];
  if (!horaValida) {
    snprintf(linea, sizeof(linea), "%-16s", "SIN HORA");
    lcd.setCursor(0, 0);
    lcd.print(linea);
    snprintf(linea, sizeof(linea), "%-16s", "Esperando NTP");
    lcd.setCursor(0, 1);
    lcd.print(linea);
    return;
  }
  snprintf(texto, sizeof(texto), "%s %02d:%02d",
           DIAS_ES[tiempoCacheado.DayOfWeek() % 7],
           tiempoCacheado.Hour(), tiempoCacheado.Minute());
  snprintf(linea, sizeof(linea), "%-16s", texto);
  lcd.setCursor(0, 0);
  lcd.print(linea);
  snprintf(texto, sizeof(texto), "%02d/%02d/%04d",
           tiempoCacheado.Day(), tiempoCacheado.Month(), tiempoCacheado.Year());
  if (ahora < avisoSyncHastaMs) {
    char conAviso[17];
    snprintf(conAviso, sizeof(conAviso), "%s SYNC", texto);  // 15 letras, entra en 16
    snprintf(linea, sizeof(linea), "%-16s", conAviso);
  } else {
    snprintf(linea, sizeof(linea), "%-16s", texto);
  }
  lcd.setCursor(0, 1);
  lcd.print(linea);
}

// ------------------------------ T3: planificador de alarmas (por ventana)
// Espacio NVS "mediclock" (ver prefs.begin en setup). Formato de claves:
//   a{d}s{s}h : hora (0-23, UChar) · a{d}s{s}m : minuto (0-59, UChar)
//   a{d}s{s}e : habilitada (bool). d = día 0-6 (0 = domingo, igual que
//   DayOfWeek del RTC), s = slot 0-2. Ejemplo: "a3s1h" = hora del
//   miércoles, slot 1. Clave "alarm_init" (bool): marca de NVS ya iniciado.
// Sin EEPROM: solo Preferences (NVS).

// Arma la clave NVS de una celda ("a3s1h"). buf necesita al menos 7 letras.
void claveAlarma(uint8_t dia, uint8_t slot, char tipo, char* buf, size_t largo) {
  snprintf(buf, largo, "a%us%u%c", dia, slot, tipo);
}

// Guarda una alarma con validación de rangos. Si algún dato está fuera
// de rango devuelve false y no escribe nada.
bool guardarAlarmaLocal(uint8_t dia, uint8_t slot, uint8_t hora, uint8_t minuto, bool habilitada) {
  if (dia > 6 || slot > 1 || hora > 23 || minuto > 59) {
    return false;
  }
  char clave[8];
  claveAlarma(dia, slot, 'h', clave, sizeof(clave));
  prefs.putUChar(clave, hora);
  claveAlarma(dia, slot, 'm', clave, sizeof(clave));
  prefs.putUChar(clave, minuto);
  claveAlarma(dia, slot, 'e', clave, sizeof(clave));
  prefs.putBool(clave, habilitada);
  return true;
}

// Lee una alarma. Devuelve false con día/slot fuera de rango.
// Si la celda nunca se escribió, entrega 0:00 deshabilitada.
bool leerAlarmaLocal(uint8_t dia, uint8_t slot, uint8_t &hora, uint8_t &minuto, bool &habilitada) {
  if (dia > 6 || slot > 1) {
    return false;
  }
  char clave[8];
  claveAlarma(dia, slot, 'h', clave, sizeof(clave));
  hora = prefs.getUChar(clave, 0);
  claveAlarma(dia, slot, 'm', clave, sizeof(clave));
  minuto = prefs.getUChar(clave, 0);
  claveAlarma(dia, slot, 'e', clave, sizeof(clave));
  habilitada = prefs.getBool(clave, false);
  return true;
}

// Primer arranque: si NVS no tiene la marca, crea las 14 celdas
// deshabilitadas (0:00, sin horarios inventados) y deja la marca.
void inicializarNVSsiVacio() {
  if (prefs.isKey("alarm_init")) {
    return;
  }
  for (uint8_t dia = 0; dia < 7; dia++) {
    for (uint8_t slot = 0; slot < 2; slot++) {
      guardarAlarmaLocal(dia, slot, 0, 0, false);
    }
  }
  prefs.putBool("alarm_init", true);
}

// Muestra el aviso como hacía el original ("!ALARMA!").
// No bloquea: solo escribe y vuelve; mostrarReloj() lo respeta
// mientras alarmaPendiente siga activa (la consume T5).
void mostrarAlarmaEnLCD(uint8_t dia, uint8_t slot, uint8_t hora, uint8_t minuto) {
  char linea[17];
  snprintf(linea, sizeof(linea), "%-16s", "!ALARMA!");
  lcd.setCursor(0, 0);
  lcd.print(linea);
  snprintf(linea, sizeof(linea), "D%u S%u %02u:%02u", dia, slot, hora, minuto);
  char linea2[17];
  snprintf(linea2, sizeof(linea2), "%-16s", linea);
  lcd.setCursor(0, 1);
  lcd.print(linea2);
}

// Revisa las 2 alarmas del día actual contra la ventana [hh:mm] del
// minuto en curso. Sin hora válida no dispara nada. Cada celda dispara
// una sola vez por día (marca yaDisparo, liberada al cambiar de día);
// dentro del minuto las pasadas repetidas del loop no re-disparan.
// Al disparar deja alarmaPendiente para T5 y avisa en el LCD.
void verificarAlarmas() {
  if (!horaValida) {
    return;  // RTC caído y sin NTP: no se confía en la hora
  }
  // Cambio de día: se liberan las marcas para disparar de nuevo.
  uint16_t anio = tiempoCacheado.Year();
  uint8_t mes = tiempoCacheado.Month();
  uint8_t hoy = tiempoCacheado.Day();
  if (anio != diaMarcaAnio || mes != diaMarcaMes || hoy != diaMarcaDia) {
    memset(yaDisparo, 0, sizeof(yaDisparo));
    diaMarcaAnio = anio;
    diaMarcaMes = mes;
    diaMarcaDia = hoy;
  }
  uint8_t dia = tiempoCacheado.DayOfWeek() % 7;
  uint8_t hh = tiempoCacheado.Hour();
  uint8_t mm = tiempoCacheado.Minute();
  for (uint8_t slot = 0; slot < 2; slot++) {
    if (yaDisparo[dia][slot]) {
      continue;  // esta celda ya disparó hoy
    }
    uint8_t ah = 0, am = 0;
    bool hab = false;
    if (!leerAlarmaLocal(dia, slot, ah, am, hab) || !hab) {
      continue;
    }
    // Ventana del minuto actual [hh:mm]: dispara en cualquier segundo
    // del minuto, nunca con s == 0 (ese era el error del original).
    if (ah == hh && am == mm) {
      yaDisparo[dia][slot] = true;
      if (!alarmaPendiente.activa) {
        alarmaPendiente.activa = true;
        alarmaPendiente.esManual = false;
        alarmaPendiente.dia = dia;
        alarmaPendiente.slot = slot;
        alarmaPendiente.hora = ah;
        alarmaPendiente.minuto = am;
      }
      mostrarAlarmaEnLCD(dia, slot, ah, am);
      Serial.printf("[mediclock] alarma d%u s%u %02u:%02u\n", dia, slot, ah, am);
    }
  }
}

// ------------------------------------------------- T4: WiFi + servidor
// Contrato JSON esperado (el formato exacto del back vive en el front,
// FrontMediclock/Modelos/Alarma.cs: lista de {diaSemana 1-7, numeroAlarma 1-3, hora "HH:mm:ss"}).
//   GET BACKEND_URL + BACKEND_ALARMS_PATH -> 200 con lista JSON.
//     Formato real del back (.NET): [
//       {"alarmaId":2,"diaSemana":2,"numeroAlarma":1,"hora":"08:00:00"} ]
//       diaSemana 1-7 (1 = lunes, 7 = domingo), numeroAlarma 1-3.
//       Toda alarma listada se toma como habilitada (el back no trae flag).
//       Conversión: dia = diaSemana % 7 (7 -> domingo 0), slot = numero - 1.
//     Formato simple alternativo (mismo de la consigna T4): [
//       {"dia":3,"slot":1,"hora":7,"minuto":30,"habilitada":1} ]
//       dia 0-6 (0 = domingo, igual que DayOfWeek), slot 0-2,
//       hora 0-23, minuto 0-59, habilitada 0/1 (si falta, se asume 1).
//   POST BACKEND_URL + BACKEND_EVENT_PATH con el modelo Evento del back:
//     {"tipo":"DosisEntregada","descripcion":"Dosis entregada dia 3 turno 2",
//      "dispositivoId":1,"alarmaId":null}
//     tipo fijo "DosisEntregada", descripcion de 10-300 letras, dispositivoId
//     igual al de la URL. fechaHora la asigna el servidor; alarmaId nulo
//     hasta que el GET guarde el id real. Espera 200/201/202.
// Parseo manual mínimo (sin ArduinoJson): el núcleo ESP32 estándar no la
// incluye y así no se suma ninguna dependencia nueva.
// Regla offline: errores HTTP o JSON se informan por Serial y la NVS
// queda intacta; lo ausente en el GET no borra celdas locales.
unsigned long ultimaDescargaAlarmasms = 0;  // último GET (exitoso o no)

// Busca "clave" : número dentro de un objeto JSON plano.
// Acepta 123, "123" y true/false (valen 1/0). Devuelve false si no está.
bool extraerEnteroJson(const String& obj, const char* clave, int& valor) {
  String patron = String("\"") + clave + "\"";
  int i = obj.indexOf(patron);
  if (i < 0) return false;
  i = obj.indexOf(':', i + patron.length());
  if (i < 0) return false;
  i++;
  while (i < (int)obj.length() && (obj[i] == ' ' || obj[i] == '\t' || obj[i] == '"')) i++;
  if (obj.indexOf("true", i) == i) { valor = 1; return true; }
  if (obj.indexOf("false", i) == i) { valor = 0; return true; }
  int signo = 1;
  if (i < (int)obj.length() && obj[i] == '-') { signo = -1; i++; }
  if (i >= (int)obj.length() || !isDigit(obj[i])) return false;
  long v = 0;
  while (i < (int)obj.length() && isDigit(obj[i])) { v = v * 10 + (obj[i] - '0'); i++; }
  valor = (int)(signo * v);
  return true;
}

// Busca "clave" : "texto" dentro de un objeto JSON plano.
bool extraerTextoJson(const String& obj, const char* clave, char* salida, size_t largo) {
  String patron = String("\"") + clave + "\"";
  int i = obj.indexOf(patron);
  if (i < 0) return false;
  i = obj.indexOf(':', i + patron.length());
  if (i < 0) return false;
  i++;
  while (i < (int)obj.length() && (obj[i] == ' ' || obj[i] == '\t')) i++;
  if (i >= (int)obj.length() || obj[i] != '"') return false;
  i++;
  int fin = obj.indexOf('"', i);
  if (fin < 0) return false;
  obj.substring(i, fin).toCharArray(salida, largo);
  return true;
}

// Parte "HH:mm:ss" (o "HH:mm") en hora y minuto. Devuelve false si no encaja.
bool partirHoraTexto(const char* texto, int& hh, int& mm) {
  int h = -1, m = -1;
  if (sscanf(texto, "%d:%d", &h, &m) != 2) return false;
  hh = h;
  mm = m;
  return true;
}

// Guarda en NVS lo válido del cuerpo del GET. Cada objeto admite ambos
// formatos del contrato; lo inválido se cuenta e ignora sin tocar la NVS.
// Las celdas ausentes en la respuesta se dejan como están (no se borran).
void aplicarAlarmasDesdeJson(const String& cuerpo) {
  int pos = 0, validas = 0, ignoradas = 0;
  while (true) {
    int ini = cuerpo.indexOf('{', pos);
    if (ini < 0) break;
    int fin = cuerpo.indexOf('}', ini + 1);  // objetos planos, sin anidar
    if (fin < 0) {
      Serial.println(F("[mediclock] GET alarmas: JSON truncado, NVS intacta"));
      break;
    }
    String obj = cuerpo.substring(ini, fin + 1);
    pos = fin + 1;
    int dia = -1, slot = -1, hh = -1, mm = -1, v = 0;
    // Día: formato back (1-7) o simple (0-6).
    if (extraerEnteroJson(obj, "diaSemana", v)) {
      if (v < 1 || v > 7) { ignoradas++; continue; }
      dia = v % 7;  // 1-6 igual, 7 (domingo) -> 0
    } else if (extraerEnteroJson(obj, "dia", v)) {
      if (v < 0 || v > 6) { ignoradas++; continue; }
      dia = v;
    }
    // Slot: formato back (1-3) o simple (0-2).
    if (extraerEnteroJson(obj, "numeroAlarma", v)) slot = v - 1;
    else if (extraerEnteroJson(obj, "slot", v)) slot = v;
    // Hora: número directo o texto "HH:mm:ss" del back.
    int hTexto = -1, mTexto = -1;
    bool horaEsTexto = false;
    {
      char th[16];
      if (extraerTextoJson(obj, "hora", th, sizeof(th)) &&
          partirHoraTexto(th, hTexto, mTexto)) {
        horaEsTexto = true;
      } else if (extraerEnteroJson(obj, "hora", v)) {
        hh = v;
      }
    }
    if (extraerEnteroJson(obj, "minuto", v)) mm = v;
    else if (horaEsTexto) mm = mTexto;
    if (horaEsTexto && hh < 0) hh = hTexto;
    // Habilitada: si falta se asume 1 (el back lista solo las vigentes).
    int e = 1;
    if (extraerEnteroJson(obj, "habilitada", v)) e = v;
    else if (extraerEnteroJson(obj, "e", v)) e = v;
    if (dia < 0 || slot < 0 || slot > 2 || hh < 0 || hh > 23 || mm < 0 || mm > 59) {
      ignoradas++;
      continue;
    }
    if (guardarAlarmaLocal((uint8_t)dia, (uint8_t)slot, (uint8_t)hh, (uint8_t)mm, e != 0)) {
      validas++;
    } else {
      ignoradas++;
    }
  }
  Serial.printf("[mediclock] alarmas del back: %d guardadas, %d ignoradas\n", validas, ignoradas);
}

// Pide la lista al back cuando es su turno. Sin WiFi no hace nada:
// el equipo sigue con NVS + RTC. Timeout corto, sin delay().
void descargarAlarmasSiToca() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  unsigned long ahora = millis();
  if (ultimaDescargaAlarmasms != 0 && ahora - ultimaDescargaAlarmasms < ALARMS_FETCH_INTERVAL_MS) {
    return;
  }
  ultimaDescargaAlarmasms = ahora;  // se anota antes: un fallo también espera su turno
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = String(BACKEND_URL) + BACKEND_ALARMS_PATH;
  http.begin(url);
  int codigo = http.GET();
  if (codigo != HTTP_CODE_OK) {
    Serial.printf("[mediclock] GET alarmas fallo: %d, NVS intacta\n", codigo);
    http.end();
    return;
  }
  String cuerpo = http.getString();
  http.end();
  if (cuerpo.length() == 0) {
    Serial.println(F("[mediclock] GET alarmas: cuerpo vacio, NVS intacta"));
    return;
  }
  aplicarAlarmasDesdeJson(cuerpo);
}

// ¿Hay un evento sin avisar? (la usa T5 para saber si quedó pendiente)
bool hayEventoPendiente() {
  return eventoPendiente.pendiente;
}

// Marca el evento como ya avisado. Solo se llama tras POST 200/201/202.
void marcarEventoEnviado() {
  eventoPendiente.pendiente = false;
}

// T5 la llama al dispensar (motor/buzzer o botón de pánico).
// Guarda la fecha del RTC; sin hora válida anota "00/00/0000".
void encolarEventoParaEnvio(uint8_t dia, uint8_t slot, uint8_t hora, uint8_t minuto, bool esManual) {
  eventoPendiente.pendiente = true;
  eventoPendiente.esManual = esManual;
  eventoPendiente.dia = dia;
  eventoPendiente.slot = slot;
  eventoPendiente.hora = hora;
  eventoPendiente.minuto = minuto;
  if (horaValida) {
    snprintf(eventoPendiente.fecha, sizeof(eventoPendiente.fecha), "%02u/%02u/%04u",
             tiempoCacheado.Day(), tiempoCacheado.Month(), tiempoCacheado.Year());
  } else {
    snprintf(eventoPendiente.fecha, sizeof(eventoPendiente.fecha), "00/00/0000");
  }
}

// Envía el evento pendiente cuando hay WiFi. Si falla, queda pendiente
// y se reintenta en el próximo turno, sin bloquear el bucle.
void enviarEventoSiToca() {
  if (!eventoPendiente.pendiente) {
    return;  // nada que avisar
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;  // sin red se reintenta luego; el dato sigue en memoria
  }
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  String url = String(BACKEND_URL) + BACKEND_EVENT_PATH;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  // Contrato Evento del back: tipo + descripcion (10-300) + dispositivoId.
  // fechaHora la pone el servidor; alarmaId nulo por ahora.
  char descripcion[64];
  if (eventoPendiente.esManual) {
    snprintf(descripcion, sizeof(descripcion), "Dispensacion manual dia %u",
             eventoPendiente.dia);
  } else {
    snprintf(descripcion, sizeof(descripcion), "Dosis entregada dia %u turno %u",
             eventoPendiente.dia, eventoPendiente.slot + 1);
  }
  char cuerpo[192];
  snprintf(cuerpo, sizeof(cuerpo),
           "{\"tipo\":\"DosisEntregada\",\"descripcion\":\"%s\",\"dispositivoId\":%d,\"alarmaId\":null}",
           descripcion, DISPOSITIVO_ID);
  int codigo = http.POST(String(cuerpo));
  http.end();
  if (codigo == HTTP_CODE_OK || codigo == HTTP_CODE_CREATED || codigo == HTTP_CODE_ACCEPTED) {
    marcarEventoEnviado();
    Serial.println(F("[mediclock] evento avisado al back"));
  } else {
    Serial.printf("[mediclock] POST evento fallo: %d, reintenta luego\n", codigo);
  }
}

// Reconexión no bloqueante + turnos de GET/POST.
// Sin WiFi el equipo sigue con lo guardado en NVS (T3) y el RTC (T2).
void atenderWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    unsigned long ahora = millis();
    bool primerIntento = (ultimoReintentoWiFims == 0);
    if (!primerIntento && ahora - ultimoReintentoWiFims < WIFI_RETRY_INTERVAL_MS) {
      return;  // aún no es su turno; el resto sigue funcionando
    }
    ultimoReintentoWiFims = ahora;
    WiFi.disconnect();                      // sale del estado "conectando"
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);  // reintento limpio, no bloquea
    Serial.println(F("[mediclock] WiFi conectando..."));
    return;  // GET/POST recién cuando haya conexión, en próximos turnos
  }
  descargarAlarmasSiToca();  // cada ALARMS_FETCH_INTERVAL_MS
  enviarEventoSiToca();      // si hay evento pendiente
}

// --------------------------- T5: actuadores (motor, buzzer, botones)
// El dispensador avanza por pasos cortos sin bloquear: cada turno del loop
// mueve solo STEPPER_PASOS_POR_TURNO pasos y vuelve. Estados:
//   REPOSO -> HOMING (buscar origen con reed) -> DOSIS (giro de entrega)
//   -> AVISO ("Dosis entregada" 1 s) -> REPOSO.
// El buzzer suena intermitente (500 ms on/off) con el LED a la par mientras
// alarmaPendiente sigue activa. El botón de pánico confirma sin WiFi.
enum EstadoDispensador {
  DISP_EN_REPOSO = 0,  // sin trabajo pendiente
  DISP_HOMING = 1,     // girando hasta que el reed marque el origen
  DISP_DOSIS = 2,      // avanzando los pasos de la dosis
  DISP_AVISO = 3       // mostrando "Dosis entregada" antes de liberar
};
EstadoDispensador estadoDispensador = DISP_EN_REPOSO;
long pasosHomingDados = 0;       // pasos girados buscando el origen
long pasosDosisRestantes = 0;    // pasos que faltan de la dosis
unsigned long ultimoPasoMotorms = 0;     // último turno de pasos del motor
unsigned long ultimoCambioBuzzerms = 0;  // último cambio on/off del buzzer
bool buzzerEncendido = false;    // estado actual del patrón intermitente
int ultimaLecturaPanico = HIGH;  // última lectura cruda del botón
int estadoPanicoEstable = HIGH;  // lectura confirmada tras antirebote
unsigned long ultimoRebotePanicoms = 0;  // último cambio de la lectura cruda

// Enciende el buzzer pasivo por LEDC (el apagado usa apagarBuzzer()).
void encenderBuzzer() {
  ledcWrite(PIN_BUZZER, BUZZER_DUTY);
}

// Patrón intermitente no bloqueante: cada BUZZER_PARPADEO_MS cambia de
// estado y el LED acompaña (encendido = suena + LED). Solo suena mientras
// hay alarma pendiente; al terminar se apaga en finalizarDosis().
void actualizarBuzzerAlarma() {
  if (!alarmaPendiente.activa) {
    return;
  }
  unsigned long ahora = millis();
  if (ahora - ultimoCambioBuzzerms < BUZZER_PARPADEO_MS) {
    return;  // aún no es su turno
  }
  ultimoCambioBuzzerms = ahora;
  buzzerEncendido = !buzzerEncendido;
  if (buzzerEncendido) {
    encenderBuzzer();
    digitalWrite(PIN_LED, HIGH);
  } else {
    apagarBuzzer();
    digitalWrite(PIN_LED, LOW);
  }
}

// Empieza el ciclo de entrega: primero homing con reed, luego la dosis.
// Enciende el patrón de buzzer/LED de inmediato para avisar sin demora.
void iniciarCicloDosis(long pasosAvance) {
  pasosAvanceTotal = pasosAvance;
  faseReversaDosis = false;
  pausaReversaHastaMs = 0;
  ultimoPasoMotorms = millis();
  buzzerEncendido = true;
  ultimoCambioBuzzerms = millis();
  encenderBuzzer();
  digitalWrite(PIN_LED, HIGH);
  ledTestHastaMs = millis() + 5000;
  if (pasosAvanceTotal <= 0) {
    // Sin movimiento (ej. Lunes alarma 1): entrega directa en el origen.
    Serial.println(F("[mediclock] dispensa: sin movimiento, origen"));
    finalizarDosis();
    return;
  }
  estadoDispensador = DISP_DOSIS;
  pasosDosisRestantes = pasosAvanceTotal;
  Serial.printf("[mediclock] dispensa: avance %ld pasos\n", pasosAvanceTotal);
}

// Cierra la entrega: apaga buzzer/LED, muestra "Dosis entregada" 1 s,
// limpia alarmaPendiente y deja el evento listo para el POST de T4.
// Vale tanto para fin de giro normal como para confirmación con pánico.
void finalizarDosis() {
  if (!alarmaPendiente.activa) {
    return;  // sin alarma no hay nada que cerrar
  }
  uint8_t dia = alarmaPendiente.dia;
  uint8_t slot = alarmaPendiente.slot;
  uint8_t hora = alarmaPendiente.hora;
  uint8_t minuto = alarmaPendiente.minuto;
  apagarBuzzer();
  digitalWrite(PIN_LED, LOW);
  buzzerEncendido = false;
  char linea[17];
  snprintf(linea, sizeof(linea), "%-16s", "Dosis entregada");
  lcd.setCursor(0, 0);
  lcd.print(linea);
  snprintf(linea, sizeof(linea), "%-16s", "");
  lcd.setCursor(0, 1);
  lcd.print(linea);
  alarmaPendiente.activa = false;
  encolarEventoParaEnvio(dia, slot, hora, minuto, alarmaPendiente.esManual);  // T4 lo envía en su turno
  estadoDispensador = DISP_AVISO;
  mensajeTemporalHastaMs = millis() + DOSIS_AVISO_MS;
  ultimaPantallaRelojms = millis();  // el reloj retoma tras el aviso
  Serial.println(F("[mediclock] dosis entregada"));
}

// Avanza el motor sin bloquear: homing hasta el reed, luego la dosis.
// stepper.step() se llama solo con pasos cortos (STEPPER_PASOS_POR_TURNO),
// así cada turno dura pocos milisegundos y el resto sigue funcionando.
void avanzarMotorSiToca() {
  if (estadoDispensador != DISP_DOSIS) {
    return;
  }
  unsigned long ahora = millis();
  if (ahora - ultimoPasoMotorms < STEPPER_INTERVALO_MS) {
    return;  // aún no es el turno del motor
  }
  ultimoPasoMotorms = ahora;
  // Fase forward: avanza la cantidad de pasos necesaria.
  if (!faseReversaDosis) {
    long turno = pasosDosisRestantes < STEPPER_PASOS_POR_TURNO
        ? pasosDosisRestantes
        : STEPPER_PASOS_POR_TURNO;
    stepper.step((int)turno);
    pasosDosisRestantes -= turno;
    if (pasosDosisRestantes <= 0) {
      // Pasa a la fase de retroceso: espera REVERSA_PAUSA_MS y luego retrocede.
      faseReversaDosis = true;
      pausaReversaHastaMs = millis() + REVERSA_PAUSA_MS;
      pasosDosisRestantes = pasosAvanceTotal;
      if (pasosDosisRestantes <= 0) {
        finalizarDosis();
      }
    }
    return;
  }
  // Fase reverse: espera la pausa y luego retrocede la misma cantidad de pasos.
  if (pausaReversaHastaMs != 0 && millis() < pausaReversaHastaMs) {
    return;  // aún en pausa, no mover
  }
  pausaReversaHastaMs = 0;
  long turnoR = pasosDosisRestantes < STEPPER_PASOS_POR_TURNO
      ? pasosDosisRestantes
      : STEPPER_PASOS_POR_TURNO;
  stepper.step(-(int)turnoR);
  pasosDosisRestantes -= turnoR;
  if (pasosDosisRestantes <= 0) {
    finalizarDosis();
  }
}

// Un toque sin alarma muestra el estado sin mover nada peligroso:
// línea 0 fija + hora actual (o "SIN HORA") durante PANIC_ESTADO_MS.
void mostrarEstadoSinAlarma() {
  char linea[17];
  snprintf(linea, sizeof(linea), "%-16s", "MediClock listo");
  lcd.setCursor(0, 0);
  lcd.print(linea);
  if (horaValida) {
    char texto[17];
    snprintf(texto, sizeof(texto), "%s %02u:%02u",
             DIAS_ES[tiempoCacheado.DayOfWeek() % 7],
             tiempoCacheado.Hour(), tiempoCacheado.Minute());
    snprintf(linea, sizeof(linea), "%-16s", texto);
  } else {
    snprintf(linea, sizeof(linea), "%-16s", "SIN HORA");
  }
  lcd.setCursor(0, 1);
  lcd.print(linea);
  mensajeTemporalHastaMs = millis() + PANIC_ESTADO_MS;
  ultimaPantallaRelojms = millis();  // el reloj retoma tras el mensaje
  Serial.println(F("[mediclock] panico: sin alarma, solo estado"));
}

// Lee el botón de pánico con flanco + antirebote por millis (activo en LOW,
// pull-up externo como el reed). Sin librerías: si la lectura cambia se
// espera PANIC_DEBOUNCE_MS estable antes de aceptar el toque.
// Con alarma: confirma la entrega (funciona sin WiFi). Sin alarma: solo
// muestra el estado, sin mover el motor ni sonar.
void atenderBotonPanico() {
  int lectura = digitalRead(PIN_PANIC_BUTTON);
  unsigned long ahora = millis();
  if (lectura != ultimaLecturaPanico) {
    ultimoRebotePanicoms = ahora;  // hubo ruido o un toque real: se espera
    ultimaLecturaPanico = lectura;
  }
  if (ahora - ultimoRebotePanicoms < PANIC_DEBOUNCE_MS) {
    return;  // lectura aún inestable
  }
  if (estadoPanicoEstable == lectura) {
    return;  // sin cambio confirmado
  }
  estadoPanicoEstable = lectura;
  if (lectura != LOW) {
    return;  // solo importa el flanco de presión (HIGH -> LOW)
  }
  if (alarmaPendiente.activa) {
    Serial.println(F("[mediclock] panico: confirma alarma"));
    finalizarDosis();  // silencia y dispensa igual que confirmar, sin WiFi
  } else {
    mostrarEstadoSinAlarma();
  }
}

// Índice de hendidura (0..13) para un día/slot de alarma.
// Día firmware: 0=domingo..6=sábado. Slot 0/1 (máx. 2 alarmas).
// Lunes alarma 1 = 0 (sin movimiento), Lunes alarma 2 = 1, ..., Domingo alarma 2 = 13.
int IndiceHendidura(uint8_t dia, uint8_t slot) {
  int diaLunes0 = (dia + 6) % 7;  // convierte a 0=lunes
  int s = (slot > 1) ? 1 : slot;
  return diaLunes0 * 2 + s;
}

// Tarea T5 del loop: buzzer + motor + pánico, todo sin delay().
void actualizarActuadores() {
  atenderBotonPanico();  // primero: el pánico puede cerrar una alarma
  if (alarmaPendiente.activa && estadoDispensador == DISP_EN_REPOSO) {
    long pasos = (long)IndiceHendidura(alarmaPendiente.dia, alarmaPendiente.slot) * STEPS_PER_HENDIDURA;
    iniciarCicloDosis(pasos);
  }
  actualizarBuzzerAlarma();  // patrón 500 ms on/off + LED a la par
  avanzarMotorSiToca();      // homing con reed + dosis por pasos cortos
  if (estadoDispensador == DISP_AVISO && millis() >= mensajeTemporalHastaMs) {
    estadoDispensador = DISP_EN_REPOSO;  // aviso cumplido, el reloj retoma
  }
  if (!alarmaPendiente.activa && estadoDispensador == DISP_EN_REPOSO && buzzerEncendido) {
    apagarBuzzer();  // seguridad: sin alarma nunca queda sonando
    digitalWrite(PIN_LED, LOW);
    buzzerEncendido = false;
  }
}

// ------------------------------------------------------- WebSocket (push del back)
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      wsConectado = true;
      Serial.println(F("[ws] conectado al back"));
      break;
    case WStype_DISCONNECTED:
      wsConectado = false;
      Serial.println(F("[ws] desconectado del back"));
      break;
    case WStype_TEXT: {
      char buf[128];
      size_t n = length < sizeof(buf) - 1 ? length : sizeof(buf) - 1;
      memcpy(buf, payload, n);
      buf[n] = '\0';
      char cmd[32];
      if (extraerTextoJson(String(buf), "cmd", cmd, sizeof(cmd))) {
        if (strcmp(cmd, "REFRESH_CONFIG") == 0) {
          refrescarAlarmasPendiente = true;
        } else if (strcmp(cmd, "DISPENSAR") == 0) {
          dispensarManualPendiente = true;
        }
      }
      break;
    }
    default:
      break;
  }
}

// --------------------------------------------------------------- Arranque
void setup() {
  Serial.begin(115200);

  // Modo estación desde el arranque (no bloquea, no conecta solo).
  // atenderWiFi() llama a WiFi.begin() en su turno con reintentos cada
  // WIFI_RETRY_INTERVAL_MS; sin red el equipo sigue con NVS + RTC.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);

  pinMode(LED_TEST, OUTPUT);
  digitalWrite(LED_TEST, LOW);

  // Inicialización directa SSL mediante WebSocketsClient
  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(WS_RECONNECT_INTERVAL_MS);
  //webSocket.enableHeartbeat(15000, 3000, 2);

  // GPIO34/35 son solo entrada y no tienen pull-up interno:
  // llevan pull-up externo de 10 k a 3,3 V y activan en LOW (a GND).
  pinMode(PIN_REED_SWITCH, INPUT_PULLUP);
  pinMode(PIN_PANIC_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);

  // Buzzer por LEDC (núcleo v3.x: canal automático por pin).
  ledcAttach(PIN_BUZZER, BUZZER_LEDC_FREQ_HZ, BUZZER_LEDC_RES_BITS);
  apagarBuzzer();

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("MediClock boot");

  rtc.Begin();

  // Fuente principal de hora: se configura una sola vez al arrancar.
  // sincronizarNTP() solo lee cuando toca, sin bloquear el bucle.
  configTime(NTP_GMT_OFFSET_SEC, NTP_DAYLIGHT_OFFSET_SEC, NTP_SERVER);
  ntpConfigurado = true;

  // Primera lectura con la misma validación que leerTiempo():
  // si el DS1302 perdió la hora no se confía en sus registros
  // y sincronizarNTP() la traerá cuando haya WiFi.
  ultimaLecturaRTCms = millis();
  tiempoCacheado = rtc.GetDateTime();
  if (!rtc.IsDateTimeValid() || !esFechaPosible(tiempoCacheado)) {
    rtcValido = false;
    horaValida = false;
  } else {
    rtcValido = true;
    horaValida = true;
  }

  stepper.setSpeed(12);  // rpm; el giro real sigue por pasos no bloqueantes en T5
  prefs.begin("mediclock", false);
  inicializarNVSsiVacio();  // T3: primer arranque deja las 21 celdas deshabilitadas

  Serial.println(F("[mediclock] setup done"));
}

// -------------------------------------------------------- Bucle principal
void loop() {
  // Cada función decide sola si ya es su turno; ninguna detiene a las demás.
  webSocket.loop();
  leerTiempo();
  sincronizarNTP();
  mostrarReloj();
  verificarAlarmas();
  atenderWiFi();
  if (refrescarAlarmasPendiente) {
    refrescarAlarmasPendiente = false;
    ultimaDescargaAlarmasms = 0;
    descargarAlarmasSiToca();
  }
  if (dispensarManualPendiente && !alarmaPendiente.activa && estadoDispensador == DISP_EN_REPOSO) {
    dispensarManualPendiente = false;
    alarmaPendiente.activa = true;
    alarmaPendiente.esManual = true;
    alarmaPendiente.dia = horaValida ? (tiempoCacheado.DayOfWeek() % 7) : 0;
    alarmaPendiente.slot = 0;
    alarmaPendiente.hora = horaValida ? tiempoCacheado.Hour() : 0;
    alarmaPendiente.minuto = horaValida ? tiempoCacheado.Minute() : 0;
    iniciarCicloDosis(STEPS_PER_HENDIDURA);
  }
  if (millis() < ledTestHastaMs) {
    if (millis() - ultimoParpadeoLedMs >= 250) {
      ultimoParpadeoLedMs = millis();
      ledTestEncendido = !ledTestEncendido;
      digitalWrite(LED_TEST, ledTestEncendido ? HIGH : LOW);
    }
  } else if (ledTestHastaMs != 0) {
    digitalWrite(LED_TEST, LOW);
    ledTestHastaMs = 0;
  }
  actualizarActuadores();
}