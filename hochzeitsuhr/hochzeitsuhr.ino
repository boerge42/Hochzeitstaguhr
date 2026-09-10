/* ****************************************************************************************
 * 
 *   Hochzeitsuhr
 * ================
 * Uwe Berger; 2026
 * 
 * 
 * Hardware:
 * ---------
 *   --> https://www.waveshare.com/esp32-s3-lcd-1.28.htm
 *   --> https://www.waveshare.com/wiki/ESP32-S3-LCD-1.28
 * 
 * 
 * SW-Umgebung:
 * ------------
 * 
 * Arduino-IDE (für übersetzen/flashen):
 *  - Version 2.3.8
 * 
 * Board: 
 * -  Arduino‑ESP32‑Cores 3.3.10 (Espressif Systems)
 * 
 * Bibliotheken:
 * - Adafruit_GFX 1.12.6
 * - QMI8658 1.0.1
 * - WiFiMamager 2.0.17
 * - ...und deren entsprechendnen Voraussetzungen
 * 
 * 
 * Funktionen:
 * -----------
 * 
 * - Anmeldung an WiFi via WiFi-Manager 
 *   (siehe auch https://github.com/tzapu/WiFiManager)
 * 
 * - Zeit-Synchronisation via NTP-Server
 * 
 * - Anzeige von:
 *   - Uhrzeit (12h-System) mit aktuellem Datum
 *   - Anzahl Jahre/Tage seit Hochzeitstag (siehe entspr. Defines)
 *     - Tage --> als Kuchendiagramm mit unterschiedlichen Farben
 *       (grün/gelb/rot; entpr. Tage vor nächsten Hochzeitstag; siehe 
 *       Quelltext ;-)...)
 *   - wenn ein Hochzeitstag ist, dann dessen Name als blinkender Text
 * 
 * - Gimmick: da in dem Display-Modul ein QMI8658 verbaut ist, konnte 
 *            ich nicht widerstehen, die Ausgabenorientierung nach der 
 *            physischen Ausrichtung des Displays zu gestalten
 *
 *  
 * ToDo:
 * -----
 * 
 * - viele "magic numbers"...
 * 
 * - man koennte auch das Heiratsdatum als Parameter via WiFi-Manager
 *   eingebbar gestalten
 * 
 * - eigentlich müsste alles, konsequenterweise, mittels FreeRTOS-Tasks
 *   realisiert werden! 
 *   Grund: man braucht (leider) eine FreeRTOS-Task, um jederzeit, also 
 *          auch ausserhalb von loop(), auf den BOOT-Button reagieren
 *          UND in der Folge dann WiFiManager-Methoden (z.B. 
 *          wm.resetSettings();) aufrufen zu können..., ...die Methoden
 *          können nicht innerhalb einer ISR aufgerufen werden :-(...
 *
 * 
 * =========
 * Have fun! 
 * 
 * ****************************************************************************************
*/
#include <WiFiManager.h> // https://github.com/tzapu/WiFiManager
#include <Arduino_GFX_Library.h>
#include <QMI8658.h>
#include <time.h>
#include "esp_sntp.h"

#include "FreeSans18pt7b.h"
#include "FreeSansBold12pt7b.h"
#include "FreeSans9pt7b.h"

#include "hochzeitstage.h"

// **********************************
// **********************************
// Hochzeitsdatum festlegen
// **********************************
// ********************************** 07.09.2001
#define WEDDING_DAY     07
#define WEDDING_MON     9
#define WEDDING_YEAR    2001
// **********************************
// **********************************
// **********************************

// QMI8658
QMI8658 imu;

// display pins on esp
#define TFT_CS      9
#define TFT_DC      8
#define TFT_RST     12
#define TFT_SCK     10
#define TFT_MOSI    11
#define TFT_MISO    -1  // no data coming back
#define TFT_BL      40

#define BOOT_BTN    0   // GPIO Boot-Button --> wm.resetSettings(); ESP.restart();
volatile bool boot_btn_down = false;
volatile uint32_t boot_btn_start = 0;

#if defined(DISPLAY_DEV_KIT)

    Arduino_GFX *gfx = create_default_Arduino_GFX();

#else /* !defined(DISPLAY_DEV_KIT) */

    Arduino_DataBus *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCK, TFT_MOSI, TFT_MISO);
    Arduino_GC9A01 *gfx = new Arduino_GC9A01(bus, TFT_RST, 0 /* rotation */, true /* IPS */);

#endif /* !defined(DISPLAY_DEV_KIT) */

Arduino_Canvas *canvas = new Arduino_Canvas(gfx->width(), gfx->height(), gfx);

#define BLACK       0x0000
#define WHITE       0xFFFF
#define MIDDLE_GREY 0x8410
#define RED         0xF800
#define YELLOW      0xFFE0
#define GREEN       0x07E0
#define DARKGREEN   0x03E0
#define BLUE        0x001F

// ...colors
#define BACKGROUND              BLACK
#define COLOR_TEXT              WHITE
#define COLOR_SECOND            BLUE
#define COLOR_MINUTE            WHITE
#define COLOR_HOUR              WHITE
#define COLOR_FRAME             WHITE
#define COLOR_FRAME_1           MIDDLE_GREY
#define COLOR_UNIT              MIDDLE_GREY
#define COLOR_TEXT_BLINK_1      WHITE
#define COLOR_TEXT_BLINK_2      RED
#define COLOR_DATE              MIDDLE_GREY

#define CLOCK_X     center
#define CLOCK_Y     center + 44
#define CLOCK_R     75

#define YEARS_X     78
#define YEARS_Y     53
#define YEARS_R     40

#define DAYS_X      162
#define DAYS_Y      53
#define DAYS_R      40

// miscellaneous
static int16_t w, h, center, radius;

// lokale Zeitzone
// https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv
#define MY_TZ "CET-1CEST,M3.5.0/02,M10.5.0/03" 

volatile bool time_is_sync = false;
volatile bool display_was_rotated = false;

struct tm start_tm = {
    .tm_sec  =  0,
    .tm_min  =  0,
    .tm_hour =  0,
    .tm_mday =  WEDDING_DAY,
    .tm_mon  =  WEDDING_MON - 1,        // Januar = 0
    .tm_year =  WEDDING_YEAR - 1900,    // Jahre seit 1900
    .tm_wday =  0,
    .tm_yday =  0,
    .tm_isdst = 0    
};
time_t wedding_date;

// WiFi-Manager
WiFiManager wm;


// *********************************************************************
void IRAM_ATTR boot_btn_isr()
{
    Serial.println("IRAM_ATTR boot_btn_isr()");
    if (digitalRead(BOOT_BTN) == LOW) {
        boot_btn_down = true;
        boot_btn_start = millis();
    } else {
        boot_btn_down = false;
        boot_btn_start = 0;
    }
}

// *********************************************************************
void task_boot_button(void *param)
{
    for (;;) {
        if (boot_btn_down) {
            if (millis() - boot_btn_start >= 3000) {
                // *** Aktion nach 3 Sekunden ***
                Serial.println("Boot-Button 3s gedrueckt (Task)!");
                Serial.println("WLAN-Konfig loeschen und ESP-Restart!");
                wm.resetSettings();
                delay(500);
                ESP.restart();
                delay(500);
            }
        }
        vTaskDelay(100 / portTICK_PERIOD_MS);   // 100 ms Task-Sleep
    }
}

// *********************************************************************
int get_display_rotation()
{
    QMI8658_Data s;
    static int rot = 0;
    
    // Sensordaten auslesen
    if (imu.readSensorData(s)) {
        // Richtung bestimmen
        if        (s.accelY < -800) {
            rot = 0;
        } else if (s.accelX > 800) {
            rot = 1;        
        } else if (s.accelY > 800) {
            rot = 2;        
        } else if (s.accelX < -800) {
            rot = 3;        
        }         
    }
    // Rückgabewert ist momentane Ausrichtung
    return rot;
}

// *********************************************************************
// Prüfung, ob ein Jahr Schaltjahr ist
bool is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

// *********************************************************************
// Anzahl Tage im Monat (wird nicht aufgerufen, aber doku...)
int days_in_month(int year, int month) {
    static const int days[12] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 1) return is_leap(year) ? 29 : 28;
    return days[month];
}

// *********************************************************************
// Tage zwischen zwei tm-Strukturen
int days_between(struct tm a, struct tm b) {
    time_t ta = mktime(&a);
    time_t tb = mktime(&b);
    return (tb - ta) / 86400;
}

// *********************************************************************
// Exakte Jahre + Tage + fehlende Tage (bis zum nächsten Jubiläum)
void diff_years_days(struct tm start, struct tm now, int &years, int &days, int &missingDays) {

    struct tm cursor = start;

    // volle Jahre vorwärts zählen
    years = 0;
    while (true) {
        struct tm next = cursor;
        next.tm_year += 1;

        if (mktime(&next) <= mktime(&now)) {
            cursor = next;
            years++;
        } else {
            break;
        }
    }
    // Resttage (letzter Hochzeitstag bis Heute)
    days = days_between(cursor, now);
    // Tage bis zum nächsten Hochzeitstag
    struct tm nextYear = cursor;
    nextYear.tm_year += 1;
    missingDays = days_between(now, nextYear);
}

// ********************************************************************************
// Callback bei erfolgreichem NTP-Sync
void time_sync_cb(struct timeval *tv)
{
    time_is_sync = true;
}

// *********************************************************************
void display_setup_message(String msg)
{
	static int y = 60;
	const int x  = 20;
	const int dy = 22;
    
    canvas->setFont(&FreeSans9pt7b);    
    canvas->setCursor(x, y);
    canvas->println(msg);
    canvas->flush();
    y = y + dy;		
}

// *********************************************************************
void draw_centered_text(const char *txt, const GFXfont *font, int cx, int cy, uint16_t color) 
{
    int16_t x, y;
    uint16_t w, h;
    
    canvas->setFont(font);
    canvas->setTextColor(color);
    canvas->getTextBounds(txt, 0, 0, &x, &y, &w, &h);
    int xt = cx - w / 2;
    int yt = cy + h / 2;
    canvas->setCursor(xt, yt);
    canvas->print(txt);
}

// *********************************************************************
void draw_centered_value_unit(const char *txt, const char *unit, int cx, int cy) {

    draw_centered_text(txt, &FreeSans18pt7b, cx, cy-5, COLOR_TEXT);     // die 5 kann man bestimmt auch berechnen...
    draw_centered_text(unit, &FreeSans9pt7b, cx, cy+19, MIDDLE_GREY);   // die 19 kann man bestimmt auch berechnen...
}

// *********************************************************************
void draw_pie(int cx, int cy, int radius, int value, int maxValue, uint16_t color) {

    float angle = (float)value * 360.0f / (float)maxValue;

    // Arc zeichnen: Startwinkel = 0°, Endwinkel = angle
    canvas->drawArc(cx, cy, radius, radius, 0, angle, color);
}

// *********************************************************************
// Trennt einen Text/Wort an EINEM Leer- oder Minuszeichen
void split_text(const char *txt, bool first_part, char *out)
{
    const char *p = strchr(txt, ' ');
    
    if (!p) p = strchr(txt, '-');
    if (!p) {
        strcpy(out, txt);
        return;
    }
    bool is_minus = (*p == '-');
    if (first_part) {
        int len = p - txt;
        if (is_minus) {
            // Minuszeichen mit in den ersten Teil übernehmen
            strncpy(out, txt, len + 1);
            out[len + 1] = '\0';
        } else {
            strncpy(out, txt, len);
            out[len] = '\0';
        }
    } else {
        // zweiter Teil beginnt immer nach dem Trenner
        strcpy(out, p + 1);
    }
}

// *********************************************************************
void display_clear_wedding_area()
{
    canvas->fillCircle(YEARS_X, DAYS_Y, DAYS_R, BACKGROUND);
    canvas->fillCircle(DAYS_X, DAYS_Y, DAYS_R, BACKGROUND);
    canvas->fillRect(0, 0, w, 88, BACKGROUND);

}

// *********************************************************************
void display_wedding_day(int years, int sec)
{
    char buf[50];
    uint16_t color;
    
    display_clear_wedding_area();
    // Blinken
    if (sec%2) {
        color = COLOR_TEXT_BLINK_1;
    } else {
        color = COLOR_TEXT_BLINK_2;
    }
    // years im Bereich von Array wedding_day_names?
    if (years <= 100) {
        // neg. years können nicht vorkommen... 
        // ...diff_years_days() --> beginnt bei years=0 ;-)
        // ...zwei Zeilen Text aus wedding_day_names
        split_text(wedding_day_name[years], true, buf);
        draw_centered_text(buf, &FreeSansBold12pt7b, center, 45, color);
        split_text(wedding_day_name[years], false, buf);
        draw_centered_text(buf, &FreeSansBold12pt7b, center, 70, color);
        canvas->flush();
    } else {
        // ...nur noch Xsten Hochzeitstag ausgeben... (...nie im Leben!)
        snprintf(buf, sizeof(buf), "%d.", years);
        draw_centered_text(buf, &FreeSansBold12pt7b, center, 45, color);
        draw_centered_text("Hochzeitstag", &FreeSansBold12pt7b, center, 70, color);
        canvas->flush();        
    }
}

// *********************************************************************
void display_wedding_diff(int y, int d, int md)
{
    char buf[5];
    int xm, ym, r; 
    uint16_t day_color;
    
    display_clear_wedding_area();
    // Jahre
    snprintf(buf, sizeof(buf), "%d", y);
    canvas->drawCircle(YEARS_X, DAYS_Y, DAYS_R, COLOR_FRAME);
    draw_centered_value_unit(buf, "Jahre", YEARS_X, YEARS_Y);
    
    // Tage
    snprintf(buf, sizeof(buf), "%d", d);
    // Farbe Pie-Kreis bestimmen
    if (md >= 30)                       // 30 Tage bis Hochzeitstag
        day_color = DARKGREEN;
    else if (md >= 7)                   // 7 Tage
        day_color = YELLOW;
    else 
        day_color = RED;
    // ...und zeichnen (Kreise etwas dicker...)
    canvas->drawCircle(DAYS_X, DAYS_Y, DAYS_R, COLOR_FRAME_1);
    canvas->drawCircle(DAYS_X, DAYS_Y, DAYS_R-1, COLOR_FRAME_1);    
    canvas->drawCircle(DAYS_X, DAYS_Y, DAYS_R-2, COLOR_FRAME_1);    
    draw_pie(DAYS_X, DAYS_Y, DAYS_R, d, d+md, day_color);
    draw_pie(DAYS_X, DAYS_Y, DAYS_R-1, d, d+md, day_color);
    draw_pie(DAYS_X, DAYS_Y, DAYS_R-2, d, d+md, day_color);
    draw_centered_value_unit(buf, "Tage", DAYS_X, DAYS_Y);
    
    canvas->flush();
}

// *********************************************************************
void display_clock(int hour, int min, int sec, int day, int mon, int year, int cx, int cy, int radius)
{
    static int old_xs = -1, old_ys = -1;
    static int old_xm = -1, old_ym = -1;
    static int old_xh = -1, old_yh = -1;
    static bool clockFaceDrawn = false;

    // Hilfsfunktion lokal definieren (optional)
    auto polarToXY = [&](float angle_deg, int r, int &x, int &y) {
        float a = (angle_deg - 90) * 0.0174532925;
        x = cx + r * cos(a);
        y = cy + r * sin(a);
    };

    // Zifferblatt nur einmal zeichnen oder Display-Rotation
    if (!clockFaceDrawn || display_was_rotated) {
        canvas->drawCircle(cx, cy, radius, COLOR_FRAME);
        for (int i = 0; i < 12; i++) {
            int x1, y1, x2, y2;
            polarToXY(i * 30, radius,  x1, y1);
            // ToDo: Länge in Abhängigkeit radius...? 
            polarToXY(i * 30, radius - 10, x2, y2);
            canvas->drawLine(x1, y1, x2, y2, COLOR_FRAME);
        }
        clockFaceDrawn = true;
    }

    // Winkel berechnen
    float angle_s = sec * 6;
    float angle_m = min * 6 + sec * 0.1;
    float angle_h = (hour % 12) * 30 + min * 0.5;

    // Neue Zeigerpositionen
    int xs, ys, xm, ym, xh, yh;

    // ToDo: Längen in Abhängigkeit radius...? 
    polarToXY(angle_s, radius - 10, xs, ys);
    polarToXY(angle_m, radius - 10, xm, ym);
    polarToXY(angle_h, radius - 30, xh, yh);

    // Alte Zeiger löschen
    if (old_xs != -1) {
        canvas->drawLine(cx, cy, old_xs, old_ys, BACKGROUND);
        canvas->drawLine(cx+1, cy+1, old_xs+1, old_ys+1, BACKGROUND);
        canvas->drawLine(cx, cy, old_xm, old_ym, BLACK);
        canvas->drawLine(cx+1, cy+1, old_xm+1, old_ym+1, BACKGROUND);
        canvas->drawLine(cx, cy, old_xh, old_yh, BLACK);
        canvas->drawLine(cx+1, cy+1, old_xh+1, old_yh+1, BACKGROUND);
        // Datum auch löschen... (geschätzte Werte für FreeSans9pt7b) 
        canvas->fillRect(cx-35, cy+40-7, 70, 14, BACKGROUND); 
    }
    
    // Datum
    char buf[15];
    snprintf(buf, sizeof(buf), "%02d.%02d.%02d", day, mon, year);
    draw_centered_text(buf, &FreeSans9pt7b, cx, cy+40, COLOR_DATE);
    
    // Sekundenzeiger
    canvas->drawLine(cx, cy, xs, ys, BLUE);
    canvas->drawLine(cx+1, cy+1, xs+1, ys+1, COLOR_SECOND);

    // Minutenzeiger (dicker)
    canvas->drawLine(cx, cy, xm, ym, WHITE);
    canvas->drawLine(cx+1, cy+1, xm+1, ym+1, COLOR_MINUTE);

    // Stundenzeiger (noch dicker + länger)
    canvas->drawLine(cx, cy, xh, yh, WHITE);
    canvas->drawLine(cx+1, cy+1, xh+1, yh+1, COLOR_HOUR);

    // Mittelpunkt
    canvas->fillCircle(cx, cy, 4, COLOR_FRAME);
    
    canvas->flush();

    // Neue Positionen speichern
    old_xs = xs; old_ys = ys;
    old_xm = xm; old_ym = ym;
    old_xh = xh; old_yh = yh;
}

// *********************************************************************
// *********************************************************************
void setup() 
{

    Serial.begin(115200);
    
    Serial.println("**************");
    Serial.println("Setup beginnt!");
    
    // Initialisierung QMI8658
    Serial.println("QMI8658 initialisieren..."); 
    if (!imu.begin(6, 7)) {
        Serial.println("Fehler bei Initialisierung QMI8658...");
        delay(5000);
        ESP.restart();
    }
    Serial.println("QMI8658 erfolgreich initialisert!"); 
    Serial.println("Konfiguration QMI8658...");
    imu.setAccelRange(QMI8658_ACCEL_RANGE_8G);  // Set accelerometer range (±8g)
    imu.setAccelODR(QMI8658_ACCEL_ODR_1000HZ);  // Set accelerometer output data rate (1000Hz)
    imu.setAccelUnit_mg(true);                  // Use mg (like your screen: ACC_X = -965.82)
    Serial.println("QMI8658 (ACCEL) einschalten..."); 
    imu.enableSensors(QMI8658_ENABLE_ACCEL); 
    delay(200);   

    // Initialisierung Display
    gfx->begin();
    // ...tft-backlight
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    // ...init LCD constant
    w = gfx->width();
    h = gfx->height();
    if (w < h)
        center = w / 2;
    else
        center = h / 2;
    // Display-Ausgaben nach Physik ausrichten
    gfx->setRotation(get_display_rotation());
    canvas->begin();
    canvas->fillScreen(BLACK);
    
    // Initialisierung Boot-Button
    pinMode(BOOT_BTN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BOOT_BTN), boot_btn_isr, CHANGE); // HW-Interrupt-Routine
    // obwohl der Rest des Programmes nicht dieser Architektur entspricht:
    // -> eine entsprechende FreeRTOS-Task starten, damit dort 
    //    wm..resetSettings() etc. aufgerufen werden kann...
    xTaskCreatePinnedToCore(task_boot_button, "task_boot_button", 4096, NULL, 1, NULL, 1);    

    // WiFi via WiFiManager
    display_setup_message("Initialisierung...");
    WiFi.mode(WIFI_STA);
    
    bool res;
    display_setup_message("Wifi-Setup via Handy!");
    display_setup_message("-> WLAN : Hochzeitsuhr");
    display_setup_message("-> AP-IP: 192.168.4.1");
    res = wm.autoConnect("Hochzeitsuhr");           // anonymous ap
    if(!res) {
        Serial.println("Fehler WLAN :-(");
        display_setup_message("Fehler WLAN :-(");
        delay(3000);
        gfx->fillScreen(BLACK);
        ESP.restart();
    } else {
        // ...hier sind wir mit dem WLAN verbunden
        Serial.println("Verbunden :-)");
        display_setup_message("Verbunden :-)");
    }

    // NTP-Client initialisieren, Zeitzone, etc.
    // ...lokale Zeitzone
    setenv("TZ", MY_TZ, 1);
    tzset();
    // ...SNTP konfigurieren
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    // ...mehrere NTP-Server (als Fallback)
    sntp_setservername(0, "ptbtime1.ptb.de");
    sntp_setservername(1, "ptbtime2.ptb.de");
    sntp_setservername(2, "ptbtime3.ptb.de");
    sntp_setservername(3, "de.pool.ntp.org");
    // ...NTP-Sync-Intervall
    sntp_set_sync_interval(30 * 60 * 1000);             // 30min
    // ...Smooth Sync dauerhaft aktivieren
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    // ... ein Callback registrieren (--> erstmals aktuelle Zeit von NTP-Server)
    sntp_set_time_sync_notification_cb(time_sync_cb);
    // ...SNTP starten
    sntp_init(); 

    // auf Zeitsynchronisation warten
    Serial.println("Warte auf NTP-Sync.");
    display_setup_message("Warte auf NTP-Sync.");
    while (time_is_sync == false) {
       delay(10);
    }
    
    // Display löschen
    canvas->fillScreen(BLACK);
    canvas->flush();
    
    // Hochzeitsdatum setzen
    wedding_date = mktime(&start_tm);
    
    Serial.println("Setup ist durch!");
}

// *********************************************************************
// *********************************************************************
// *********************************************************************
void loop() 
{
    time_t now;
    tm tm;
    char buf[50];  
    int years, days, missing_days;
    int rotation;
    static int old_sec  = -1;
    static int old_days = -1;
    static int old_rotation = -1;
    
    // aktuelle Zeit ermitteln
    time(&now);
    localtime_r(&now, &tm);

    // aktuelle Differenz Hochzeit zu heute berechnen
    diff_years_days(start_tm, tm, years, days, missing_days);
    
    // Display-Ausrichtung geändert?
    rotation = get_display_rotation();
    if (old_rotation != rotation) {
        gfx->setRotation(rotation);
        canvas->fillScreen(BACKGROUND);
        old_rotation = rotation;
        display_was_rotated = true;
    }

    // eine Sekunde weiter oder Display-Rotation?
    if (old_sec != tm.tm_sec || display_was_rotated) { 
        old_sec = tm.tm_sec;
        display_clock(tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_mday, tm.tm_mon+1, tm.tm_year-100, CLOCK_X, CLOCK_Y, CLOCK_R);
        
        // Name Hochzeitstag, wenn er dann da ist...
        // ...da der Text blinken soll, muss der Aufruf hier rein!
        if (tm.tm_mday == WEDDING_DAY && tm.tm_mon == WEDDING_MON-1) {
            display_wedding_day(years, tm.tm_sec);
        }        
    }
    
    // ein Tag (Diff. zu Hochzeit) weiter oder Display-Rotation?
    if ((old_days != days || display_was_rotated) && !(tm.tm_mday == WEDDING_DAY && tm.tm_mon == WEDDING_MON-1)) {
        old_days  = days;
        display_wedding_diff(years, days, missing_days);
    }
    
    display_was_rotated = false;
}
